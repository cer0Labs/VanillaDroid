package dev.cer.vanilladroid;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.NumberFormat;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Phase 22 (phonebake, ROADMAP 22.7, ADR-0005 §8): bakes the game's packs on the phone from the WoW copied to it.
 *
 * A foreground service in a process of its own (android:process=":baker": no game code, no Vulkan; the game keeps
 * running, or not, in its own) with a notification (progress, Stop) and a partial wake lock while it works. It runs
 * the plan the game wrote (files/bake/plan.json, engine/src/app/PhoneBakePlan.h) step by step with the baker shipped
 * in the APK (lib/arm64-v8a/libvdbaker.so: an executable in the native library folder, the one place an app may run
 * a program of its own from), reading its output as the progress, and writes files/bake/status.json, which the game
 * reads. Rule by rule as engine/src/app/PhoneBakeRunner.cpp (the desktop's runner and the tests' subject): keep them
 * alike. A step that ended well is never repeated: after Android killed this process (START_STICKY brings it back
 * when allowed; otherwise the game offers to go on), a Stop or a failure, it goes on from the next step.
 */
public class BakeService extends Service {
    static final String ACTION_START = "dev.cer.vanilladroid.action.BAKE";
    static final String ACTION_CANCEL = "dev.cer.vanilladroid.action.BAKE_CANCEL";

    private static final String TAG = "VanillaDroid";
    private static final String CHANNEL = "bake";
    private static final int NOTIFICATION = 22;
    private static final int NOTIFICATION_END = 23;
    private static final long HEARTBEAT_MS = 2000;
    private static final long THERMAL_MS = 15000;
    private static final long COOLING_MS = 30000;
    private static final double RESTART_SECONDS = 180.0; // kPhoneBakeRestartSeconds
    private static final int LINE_CHARS = 240;
    private static final long WAKE_LOCK_MS = 6L * 3600 * 1000; // never held for good, whatever happens

    private final Object lock = new Object();
    private Thread worker;
    private Plan current; // the plan the worker runs
    private volatile boolean cancelled;
    private volatile boolean timedOut;
    private PowerManager power;
    private PowerManager.WakeLock wakeLock;
    private NotificationManager notifications;
    private float lastHeadroom = Float.NaN; // getThermalHeadroom is NaN when asked more than once a second
    private long lastHeadroomMs;
    private long lastNotifyMs;

    /** <getExternalFilesDir(null)>/bake: the native externalDataPath's folder (engine/src/app/PhoneBake.cpp). */
    static File bakeDir(Context context) {
        return new File(context.getExternalFilesDir(null), "bake");
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent != null ? intent.getAction() : null; // null: started again by Android (sticky)
        if (ACTION_CANCEL.equals(action)) {
            cancelled = true; // the worker stops the baker, writes "cancelled" and ends the service
            if (worker == null || !worker.isAlive()) {
                stopSelf();
            }
            return START_NOT_STICKY;
        }
        if (worker != null && worker.isAlive()) {
            // Already on it; every startForegroundService wants its startForeground.
            try {
                startForeground(NOTIFICATION, progressNotification(current, null),
                                ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
            } catch (RuntimeException e) {
                Log.w(TAG, "Phone bake: " + e);
            }
            return START_STICKY;
        }
        notifications = getSystemService(NotificationManager.class);
        power = getSystemService(PowerManager.class);
        File dir = bakeDir(this);
        Plan plan = Plan.read(new File(dir, "plan.json"));
        createChannel(plan != null ? plan.texts.optString("channel", getString(R.string.app_name))
                                   : getString(R.string.app_name));
        try {
            // At once: the game asked with startForegroundService.
            startForeground(NOTIFICATION, progressNotification(plan, null), ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        } catch (RuntimeException e) {
            // Android 12+ does not let a service Android started again come to the foreground from the background:
            // the game shows it as stopped, and it goes on when the player says so.
            Log.w(TAG, "Phone bake: cannot run in the foreground now", e);
            if (plan != null) {
                markInterrupted(plan, "background");
            }
            stopSelf();
            return START_NOT_STICKY;
        }
        if (plan == null) {
            Log.w(TAG, "Phone bake: no plan in " + dir);
            stopForeground(STOP_FOREGROUND_REMOVE);
            stopSelf();
            return START_NOT_STICKY;
        }
        current = plan;
        cancelled = false;
        timedOut = false;
        wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "VanillaDroid:bake");
        wakeLock.setReferenceCounted(false);
        wakeLock.acquire(WAKE_LOCK_MS);
        worker = new Thread(() -> finish(plan, run(plan)), "vd-bake");
        worker.start();
        return START_STICKY;
    }

    // Android 15+: a dataSync service may run 6 hours a day; past them it must stop at once.
    @Override
    public void onTimeout(int startId, int foregroundServiceType) {
        Log.w(TAG, "Phone bake: Android's time limit");
        timedOut = true;
        cancelled = true;
    }

    @Override
    public void onDestroy() {
        cancelled = true; // the worker stops the baker
        if (wakeLock != null && wakeLock.isHeld()) {
            wakeLock.release();
        }
        super.onDestroy();
    }

    // --- The plan, step by step (engine/src/app/PhoneBakeRunner.cpp, runPhoneBakePlan) -----------------------

    private String run(Plan plan) {
        Run run = Run.read(plan.statusFile);
        boolean fresh = run == null || !plan.id.equals(run.plan);
        if (fresh) {
            run = new Run();
            run.plan = plan.id;
            run.planStartedMs = System.currentTimeMillis();
        } else if ("done".equals(run.state)) {
            return "done";
        }
        while (run.seconds.size() > run.finished) {
            run.seconds.remove(run.seconds.size() - 1);
        }
        File baker = new File(getApplicationInfo().nativeLibraryDir, "libvdbaker.so");
        try (Writer log = new FileWriter(plan.logFile, !fresh)) {
            for (int i = run.finished; i < plan.steps.size(); i++) {
                Step step = plan.steps.get(i);
                run.step = i;
                run.fraction = -1;
                run.line = "";
                run.error = "";
                run.exit = 0;
                run.stepStartedMs = System.currentTimeMillis();
                int jobs = coolDown(plan, run, readJobs(plan, run), log);
                if (jobs < 0) {
                    return stop(plan, run, log);
                }
                emptyStaging(plan.staging);
                run.state = "running";
                run.jobs = jobs;
                write(plan, run);
                notifyProgress(plan, run, true);
                int code;
                for (;;) { // the step; started again with fewer threads when the phone heats (textures)
                    mark(log, "=== " + step.name + " (" + (i + 1) + " of " + plan.steps.size() + "), " + jobs + " threads");
                    List<String> args = new ArrayList<>(step.bake);
                    args.add("--jobs");
                    args.add(Integer.toString(jobs));
                    Outcome outcome = runBaker(baker, args, plan, step, run, log, jobs, true);
                    code = outcome.code;
                    if (cancelled) {
                        return stop(plan, run, log);
                    }
                    if (!outcome.restart) {
                        break;
                    }
                    mark(log, "=== " + step.name + ": the phone heats, again with " + outcome.wanted + " threads");
                    jobs = coolDown(plan, run, outcome.wanted, log);
                    if (jobs < 0) {
                        return stop(plan, run, log);
                    }
                    run.state = "running";
                    run.jobs = jobs;
                    write(plan, run);
                }
                if (code != 0) {
                    return fail(plan, run, log, code);
                }
                // Its packs into packs/ (never stopped half way: it only renames).
                code = runBaker(baker, step.install, plan, step, run, log, jobs, false).code;
                if (code != 0) {
                    return fail(plan, run, log, code);
                }
                run.finished = i + 1;
                run.seconds.add((System.currentTimeMillis() - run.stepStartedMs) / 1000.0);
                run.fraction = 1;
                write(plan, run);
            }
            run.step = plan.steps.size();
            run.state = "done";
            write(plan, run);
            mark(log, "=== done");
            return "done";
        } catch (IOException e) {
            Log.e(TAG, "Phone bake: " + e);
            run.error = String.valueOf(e.getMessage());
            run.state = "failed";
            write(plan, run);
            return "failed";
        }
    }

    private String stop(Plan plan, Run run, Writer log) {
        run.state = timedOut ? "interrupted" : "cancelled";
        if (timedOut) {
            run.error = "timeout";
        }
        write(plan, run);
        mark(log, "=== " + run.state);
        return run.state;
    }

    private String fail(Plan plan, Run run, Writer log, int code) {
        synchronized (lock) {
            run.exit = code;
            if (run.error.isEmpty()) {
                run.error = run.line;
            }
            run.state = "failed";
        }
        write(plan, run);
        mark(log, "=== failed");
        return "failed";
    }

    /** Waits while the phone is too hot to bake (0 threads); -1 when cancelled meanwhile. */
    private int coolDown(Plan plan, Run run, int jobs, Writer log) {
        while (jobs == 0) {
            run.state = "cooling";
            write(plan, run);
            notifyProgress(plan, run, true);
            mark(log, "=== too hot (headroom " + run.headroom + ", status " + run.thermalStatus + "): waiting");
            for (long waited = 0; waited < COOLING_MS; waited += 1000) {
                if (cancelled) {
                    return -1;
                }
                if (waited % HEARTBEAT_MS == 0) {
                    write(plan, run);
                }
                try {
                    Thread.sleep(1000);
                } catch (InterruptedException e) {
                    return -1;
                }
            }
            jobs = readJobs(plan, run);
        }
        return cancelled ? -1 : jobs;
    }

    private static final class Outcome {
        int code = -1;
        boolean restart;
        int wanted;
    }

    /** vd_baker with these arguments to its end: its output (both streams) line by line as the progress. */
    private Outcome runBaker(File baker, List<String> args, Plan plan, Step step, Run run, Writer log, int jobs,
                             boolean stoppable) {
        Outcome outcome = new Outcome();
        List<String> command = new ArrayList<>();
        command.add(baker.getPath());
        command.addAll(args);
        Process process;
        try {
            process = new ProcessBuilder(command).redirectErrorStream(true).start();
        } catch (IOException e) {
            Log.e(TAG, "Phone bake: cannot run " + baker + ": " + e);
            synchronized (lock) {
                run.line = clip("cannot run " + baker.getName() + ": " + e.getMessage());
            }
            return outcome;
        }
        Thread reader = new Thread(() -> {
            try (BufferedReader in = new BufferedReader(
                         new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while ((line = in.readLine()) != null) {
                    onLine(line, step, run, log);
                }
            } catch (IOException ignored) {
                // the process ended or was stopped
            }
        }, "vd-bake-out");
        reader.start();
        long started = System.currentTimeMillis();
        long lastBeat = started;
        long lastThermal = started;
        boolean stopped = false;
        try {
            while (!process.waitFor(1, TimeUnit.SECONDS)) {
                if (!stoppable) {
                    continue;
                }
                if (cancelled) {
                    process.destroyForcibly();
                    stopped = true;
                    break;
                }
                long now = System.currentTimeMillis();
                if (now - lastBeat >= HEARTBEAT_MS) {
                    lastBeat = now;
                    write(plan, run);
                    notifyProgress(plan, run, false);
                }
                if (now - lastThermal >= THERMAL_MS) {
                    lastThermal = now;
                    int wanted = readJobs(plan, run);
                    if (step.restartable && restart(jobs, wanted, (now - started) / 1000.0)) {
                        outcome.restart = true;
                        outcome.wanted = wanted;
                        process.destroyForcibly();
                        stopped = true;
                        break;
                    }
                }
            }
            process.waitFor();
            reader.join();
        } catch (InterruptedException e) {
            process.destroyForcibly();
            stopped = true;
        }
        outcome.code = stopped ? -1 : process.exitValue();
        return outcome;
    }

    private void onLine(String line, Step step, Run run, Writer log) {
        synchronized (lock) {
            try {
                log.write(line);
                log.write('\n');
            } catch (IOException ignored) {
                // the log is a help, never a reason to stop
            }
            double fraction = lineFraction(line, step.name);
            if (fraction >= 0) {
                run.fraction = fraction;
            }
            String error = errorText(line);
            if (!error.isEmpty()) {
                run.error = clip(error);
            }
            run.line = clip(line);
        }
    }

    private void mark(Writer log, String line) {
        synchronized (lock) {
            try {
                log.write(line);
                log.write('\n');
                log.flush();
            } catch (IOException ignored) {
                // the log is a help
            }
        }
    }

    /** The staging folder's files: what a step cut short left (its .cache folder stays: textures' resume). */
    private static void emptyStaging(File staging) {
        staging.mkdirs();
        File[] files = staging.listFiles();
        if (files == null) {
            return;
        }
        for (File file : files) {
            if (file.isFile() && !file.delete()) {
                Log.w(TAG, "Phone bake: cannot delete " + file);
            }
        }
    }

    // phoneBakeLineFraction: "[INFO] textures: 1200 / 58502 (40 s)" for the step "textures" -> 0.0205; else -1.
    static double lineFraction(String line, String step) {
        Matcher m = Pattern.compile("^(?:\\[INFO\\] )?" + Pattern.quote(step) + ": (\\d+) / (\\d+)").matcher(line);
        if (!m.find()) {
            return -1;
        }
        try {
            double done = Double.parseDouble(m.group(1));
            double total = Double.parseDouble(m.group(2));
            return total > 0 ? Math.max(0, Math.min(1, done / total)) : -1;
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    // phoneBakeErrorText.
    static String errorText(String line) {
        return line.startsWith("[ERROR] ") ? line.substring(8) : "";
    }

    private static String clip(String line) {
        return line.length() > LINE_CHARS ? line.substring(0, LINE_CHARS) : line;
    }

    // --- Heat (phoneBakeJobs, phoneBakeRestart) -----------------------------------------------------------

    private int readJobs(Plan plan, Run run) {
        float headroom = Float.NaN;
        int status = -1;
        if (power != null) {
            try {
                headroom = power.getThermalHeadroom(10); // the forecast 10 s ahead (Android 11+)
                status = power.getCurrentThermalStatus();
            } catch (RuntimeException e) {
                Log.w(TAG, "Phone bake: no thermal reading: " + e);
            }
        }
        long now = System.currentTimeMillis();
        if (!Float.isNaN(headroom)) {
            lastHeadroom = headroom;
            lastHeadroomMs = now;
        } else if (now - lastHeadroomMs < 60000) {
            headroom = lastHeadroom; // asked too soon after the last time
        }
        synchronized (lock) {
            run.headroom = Float.isNaN(headroom) ? -1 : headroom;
            run.thermalStatus = status;
        }
        return plan.jobsFor(headroom, status);
    }

    static boolean restart(int runningJobs, int wantedJobs, double secondsSinceStart) {
        if (wantedJobs <= 0) {
            return true; // too hot to go on: wait
        }
        return wantedJobs * 2 <= runningJobs && secondsSinceStart >= RESTART_SECONDS;
    }

    // --- status.json and the notification --------------------------------------------------------------------

    private void write(Plan plan, Run run) {
        String text;
        synchronized (lock) {
            run.updatedMs = System.currentTimeMillis();
            text = run.toJson();
        }
        // Through a temporary name: the game never reads half a status.
        File temp = new File(plan.statusFile.getPath() + ".tmp");
        try (OutputStream out = new FileOutputStream(temp)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            Log.w(TAG, "Phone bake: cannot write " + temp + ": " + e);
            return;
        }
        if (!temp.renameTo(plan.statusFile)) {
            Log.w(TAG, "Phone bake: cannot replace " + plan.statusFile);
        }
    }

    private void markInterrupted(Plan plan, String why) {
        Run run = Run.read(plan.statusFile);
        if (run == null || !plan.id.equals(run.plan)) {
            run = new Run();
            run.plan = plan.id;
            run.planStartedMs = System.currentTimeMillis();
        }
        if ("done".equals(run.state)) {
            return;
        }
        run.state = "interrupted";
        run.error = why;
        write(plan, run);
    }

    private void finish(Plan plan, String state) {
        Log.i(TAG, "Phone bake: " + state);
        stopForeground(STOP_FOREGROUND_REMOVE);
        String text = "done".equals(state) ? plan.texts.optString("done")
                      : "failed".equals(state) ? plan.texts.optString("failed")
                      : "interrupted".equals(state) ? plan.texts.optString("stopped") : null;
        if (text != null && notifications != null) {
            Notification.Builder end = new Notification.Builder(this, CHANNEL)
                    .setSmallIcon(android.R.drawable.stat_notify_sync_noanim)
                    .setContentTitle(plan.texts.optString("title"))
                    .setContentText(text)
                    .setContentIntent(openGame())
                    .setAutoCancel(true);
            notifications.notify(NOTIFICATION_END, end.build());
        }
        if (wakeLock != null && wakeLock.isHeld()) {
            wakeLock.release();
        }
        stopSelf();
    }

    private void createChannel(String name) {
        NotificationChannel channel = new NotificationChannel(CHANNEL, name, NotificationManager.IMPORTANCE_LOW);
        channel.setShowBadge(false);
        notifications.createNotificationChannel(channel);
    }

    private PendingIntent openGame() {
        Intent open = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (open == null) {
            open = new Intent(this, MainActivity.class);
        }
        return PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    private Notification progressNotification(Plan plan, Run run) {
        Notification.Builder builder = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_notify_sync)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setCategory(Notification.CATEGORY_PROGRESS)
                .setContentIntent(openGame());
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            builder.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE); // not 10 s late
        }
        if (plan == null) {
            return builder.setContentTitle(getString(R.string.app_name)).build();
        }
        builder.setContentTitle(plan.texts.optString("title"));
        PendingIntent cancel = PendingIntent.getService(this, 1,
                new Intent(this, BakeService.class).setAction(ACTION_CANCEL),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        builder.addAction(new Notification.Action.Builder(null, plan.texts.optString("cancel"), cancel).build());
        if (run == null) {
            return builder.setProgress(0, 0, true).build();
        }
        // The whole plan's share done, by the steps' measured costs (the game adds this phone's own pace).
        double total = 0;
        double done = 0;
        for (int i = 0; i < plan.steps.size(); i++) {
            total += plan.steps.get(i).seconds;
            if (i < run.finished) {
                done += plan.steps.get(i).seconds;
            }
        }
        double stepFraction = 0;
        if (run.step < plan.steps.size()) {
            Step step = plan.steps.get(run.step);
            double elapsed = (System.currentTimeMillis() - run.stepStartedMs) / 1000.0;
            stepFraction = run.fraction >= 0 ? run.fraction : Math.min(elapsed / Math.max(step.seconds, 1), 0.95);
            done += step.seconds * stepFraction;
            builder.setContentText("cooling".equals(run.state) ? plan.texts.optString("cooling") : step.label);
        }
        double fraction = total > 0 ? Math.min(done / total, 1) : 0;
        builder.setProgress(1000, (int) (fraction * 1000), false);
        builder.setSubText(NumberFormat.getPercentInstance().format(fraction));
        return builder.build();
    }

    private void notifyProgress(Plan plan, Run run, boolean now) {
        long t = System.currentTimeMillis();
        if (notifications == null || (!now && t - lastNotifyMs < HEARTBEAT_MS)) {
            return;
        }
        lastNotifyMs = t;
        Notification notification;
        synchronized (lock) {
            notification = progressNotification(plan, run);
        }
        notifications.notify(NOTIFICATION, notification);
    }

    // --- plan.json and status.json (engine/src/app/PhoneBakePlan.h) -----------------------------------------

    static final class Step {
        String name;
        String label;
        double seconds;
        boolean restartable;
        List<String> bake;
        List<String> install;
    }

    static final class Plan {
        String id;
        File statusFile;
        File logFile;
        File staging;
        JSONObject texts;
        int maxJobs;
        double[][] headroomRows;
        int[][] statusRows;
        final List<Step> steps = new ArrayList<>();

        static Plan read(File file) {
            try {
                JSONObject root = new JSONObject(new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8));
                if (root.getInt("format") != 1) {
                    return null;
                }
                Plan plan = new Plan();
                plan.id = root.getString("id");
                plan.statusFile = new File(root.getString("status"));
                plan.logFile = new File(root.getString("log"));
                plan.staging = new File(root.getString("staging"));
                plan.texts = root.optJSONObject("texts");
                if (plan.texts == null) {
                    plan.texts = new JSONObject();
                }
                JSONObject thermal = root.getJSONObject("thermal");
                plan.maxJobs = Math.max(1, thermal.getInt("maxJobs"));
                JSONArray headroom = thermal.getJSONArray("headroom");
                plan.headroomRows = new double[headroom.length()][];
                for (int i = 0; i < headroom.length(); i++) {
                    JSONArray row = headroom.getJSONArray(i);
                    plan.headroomRows[i] = new double[] {row.getDouble(0), row.getDouble(1)};
                }
                JSONArray status = thermal.getJSONArray("status");
                plan.statusRows = new int[status.length()][];
                for (int i = 0; i < status.length(); i++) {
                    JSONArray row = status.getJSONArray(i);
                    plan.statusRows[i] = new int[] {row.getInt(0), row.getInt(1)};
                }
                JSONArray steps = root.getJSONArray("steps");
                for (int i = 0; i < steps.length(); i++) {
                    JSONObject value = steps.getJSONObject(i);
                    Step step = new Step();
                    step.name = value.getString("name");
                    step.label = value.optString("label", step.name);
                    step.seconds = value.optDouble("seconds", 10);
                    step.restartable = value.optBoolean("restartable", false);
                    step.bake = strings(value.getJSONArray("bake"));
                    step.install = strings(value.getJSONArray("install"));
                    plan.steps.add(step);
                }
                return plan.steps.isEmpty() ? null : plan;
            } catch (IOException | JSONException e) {
                Log.w(TAG, "Phone bake: " + file + ": " + e);
                return null;
            }
        }

        // phoneBakeJobs: at most so many threads from each headroom and each thermal status up; 0: wait.
        int jobsFor(float headroom, int status) {
            int jobs = maxJobs;
            if (!Float.isNaN(headroom) && headroom >= 0) {
                for (double[] row : headroomRows) {
                    if (headroom >= row[0]) {
                        jobs = Math.min(maxJobs, (int) row[1]);
                    }
                }
            }
            if (status >= 0) {
                for (int[] row : statusRows) {
                    if (status >= row[0]) {
                        jobs = Math.min(jobs, row[1]);
                    }
                }
            }
            return Math.max(0, jobs);
        }

        private static List<String> strings(JSONArray array) throws JSONException {
            List<String> items = new ArrayList<>();
            for (int i = 0; i < array.length(); i++) {
                items.add(array.getString(i));
            }
            return items;
        }
    }

    static final class Run {
        String plan = "";
        String state = "none";
        int finished;
        int step;
        long planStartedMs;
        long stepStartedMs;
        long updatedMs;
        double fraction = -1;
        String line = "";
        String error = "";
        int exit;
        int jobs;
        double headroom = -1;
        int thermalStatus = -1;
        final List<Double> seconds = new ArrayList<>();

        static Run read(File file) {
            if (!file.isFile()) {
                return null;
            }
            try {
                JSONObject root = new JSONObject(new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8));
                if (root.optInt("format") != 1) {
                    return null;
                }
                Run run = new Run();
                run.plan = root.optString("plan");
                run.state = root.optString("state", "none");
                run.finished = Math.max(0, root.optInt("finished"));
                run.step = Math.max(0, root.optInt("step"));
                run.planStartedMs = root.optLong("planStartedMs");
                run.stepStartedMs = root.optLong("stepStartedMs");
                run.updatedMs = root.optLong("updatedMs");
                run.fraction = root.optDouble("fraction", -1);
                run.line = root.optString("line");
                run.error = root.optString("error");
                run.exit = root.optInt("exit");
                run.jobs = root.optInt("jobs");
                run.headroom = root.optDouble("headroom", -1);
                run.thermalStatus = root.optInt("thermalStatus", -1);
                JSONArray seconds = root.optJSONArray("seconds");
                if (seconds != null) {
                    for (int i = 0; i < seconds.length(); i++) {
                        run.seconds.add(seconds.optDouble(i, 0));
                    }
                }
                return run;
            } catch (IOException | JSONException e) {
                Log.w(TAG, "Phone bake: " + file + ": " + e);
                return null;
            }
        }

        String toJson() {
            try {
                JSONObject root = new JSONObject();
                root.put("format", 1);
                root.put("plan", plan);
                root.put("state", state);
                root.put("finished", finished);
                root.put("step", step);
                root.put("planStartedMs", planStartedMs);
                root.put("stepStartedMs", stepStartedMs);
                root.put("updatedMs", updatedMs);
                root.put("fraction", Math.round(fraction * 1e4) / 1e4);
                root.put("line", line);
                root.put("error", error);
                root.put("exit", exit);
                root.put("jobs", jobs);
                root.put("headroom", Math.round(headroom * 1e3) / 1e3);
                root.put("thermalStatus", thermalStatus);
                JSONArray list = new JSONArray();
                for (double s : seconds) {
                    list.put(Math.round(s * 10) / 10.0);
                }
                root.put("seconds", list);
                return root.toString();
            } catch (JSONException e) {
                return "{}";
            }
        }
    }
}

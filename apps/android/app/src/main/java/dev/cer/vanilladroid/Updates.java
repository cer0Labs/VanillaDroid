package dev.cer.vanilladroid;

import android.app.Activity;
import android.app.DownloadManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;
import android.util.Log;

import androidx.core.content.ContextCompat;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;

/**
 * Phase 22 (updates, ADR-0005 §7): what the app's updates need from Android and C++ cannot do: HTTPS with the system's
 * TLS (the release's manifest and its signature), DownloadManager (the APK, with Android's notification), the
 * PackageInstaller session that installs it (Android asks the player to confirm) and the browser. Static methods called
 * from the native updates thread (engine/src/platform/UpdatePlatform.cpp, "vd-updates"): they block that thread, never
 * the game's. The native side verifies the manifest's signature and the file's SHA-256; install() checks the bytes it
 * writes into the session again, so what Android installs is exactly what was verified.
 */
public final class Updates {
    private static final String TAG = "VanillaDroid";
    private static final String ACTION_INSTALL_STATUS = "dev.cer.vanilladroid.UPDATE_INSTALL_STATUS";
    private static final String UPDATES_FOLDER = "updates";

    // install() results (UpdatePlatform::InstallStart).
    private static final int INSTALL_STARTED = 0;
    private static final int INSTALL_MISMATCH = 1;
    private static final int INSTALL_NOT_THIS_APP = 2;
    private static final int INSTALL_ERROR = 3;
    // installStatus() while nothing came back yet; otherwise PackageInstaller.STATUS_*.
    private static final int INSTALL_WAITING = -100;

    private static volatile int installStatus = INSTALL_WAITING;
    private static volatile String installMessage = "";
    private static BroadcastReceiver receiver;

    private Updates() {}

    /** A network that reaches the internet now. */
    public static boolean online(Activity activity) {
        ConnectivityManager manager = activity.getSystemService(ConnectivityManager.class);
        if (manager == null) {
            return false;
        }
        Network network = manager.getActiveNetwork();
        NetworkCapabilities capabilities = network != null ? manager.getNetworkCapabilities(network) : null;
        return capabilities != null && capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET);
    }

    /** A small file (at most maxBytes) over HTTP(S), redirects followed; null on any failure. */
    public static byte[] fetch(String url, int maxBytes, int timeoutMs) {
        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection) new URL(url).openConnection();
            connection.setConnectTimeout(timeoutMs);
            connection.setReadTimeout(timeoutMs);
            connection.setInstanceFollowRedirects(true);
            connection.setUseCaches(false);
            int code = connection.getResponseCode();
            if (code != HttpURLConnection.HTTP_OK) {
                Log.w(TAG, "Updates: HTTP " + code + " for " + url);
                return null;
            }
            try (InputStream in = connection.getInputStream()) {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] block = new byte[16384];
                int read;
                while ((read = in.read(block)) > 0) {
                    if (out.size() + read > maxBytes) {
                        Log.w(TAG, "Updates: " + url + " is bigger than " + maxBytes + " bytes");
                        return null;
                    }
                    out.write(block, 0, read);
                }
                return out.toByteArray();
            }
        } catch (Exception e) {
            Log.w(TAG, "Updates: cannot fetch " + url + ": " + e);
            return null;
        } finally {
            if (connection != null) {
                connection.disconnect();
            }
        }
    }

    /** Where download() puts fileName: the app's external files, updates/ (no permission needed). */
    public static String downloadPath(Activity activity, String fileName) {
        File folder = activity.getExternalFilesDir(UPDATES_FOLDER);
        return folder != null ? new File(folder, fileName).getAbsolutePath() : "";
    }

    /** Starts a DownloadManager download with its notification; its id, or -1. */
    public static long download(Activity activity, String url, String fileName, String title) {
        try {
            File file = new File(downloadPath(activity, fileName));
            if (file.exists() && !file.delete()) {
                Log.w(TAG, "Updates: cannot delete " + file);
            }
            DownloadManager manager = activity.getSystemService(DownloadManager.class);
            DownloadManager.Request request = new DownloadManager.Request(Uri.parse(url))
                    .setTitle(title)
                    .setMimeType("application/vnd.android.package-archive")
                    .setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE)
                    .setDestinationInExternalFilesDir(activity, UPDATES_FOLDER, fileName);
            return manager.enqueue(request);
        } catch (Exception e) {
            Log.w(TAG, "Updates: cannot download " + url + ": " + e);
            return -1;
        }
    }

    /** {DownloadManager status (-1 when it is gone), bytes so far, total bytes (-1 unknown)}. */
    public static long[] downloadState(Activity activity, long id) {
        long[] state = {-1, 0, -1};
        DownloadManager manager = activity.getSystemService(DownloadManager.class);
        try (Cursor cursor = manager.query(new DownloadManager.Query().setFilterById(id))) {
            if (cursor != null && cursor.moveToFirst()) {
                state[0] = cursor.getInt(cursor.getColumnIndexOrThrow(DownloadManager.COLUMN_STATUS));
                state[1] = cursor.getLong(cursor.getColumnIndexOrThrow(DownloadManager.COLUMN_BYTES_DOWNLOADED_SO_FAR));
                state[2] = cursor.getLong(cursor.getColumnIndexOrThrow(DownloadManager.COLUMN_TOTAL_SIZE_BYTES));
            }
        } catch (Exception e) {
            Log.w(TAG, "Updates: download state: " + e);
        }
        return state;
    }

    public static void cancelDownload(Activity activity, long id) {
        activity.getSystemService(DownloadManager.class).remove(id);
    }

    /** Whether the player let this app install others (Settings > Install unknown apps). */
    public static boolean canInstall(Activity activity) {
        return activity.getPackageManager().canRequestPackageInstalls();
    }

    /** That settings screen, for this app. */
    public static void openInstallSettings(Activity activity) {
        try {
            activity.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                    Uri.parse("package:" + activity.getPackageName())));
        } catch (Exception e) {
            Log.w(TAG, "Updates: cannot open the install settings: " + e);
        }
    }

    /**
     * Installs the APK at path through a PackageInstaller session: it must be this app (its package) at versionCode,
     * and the bytes written into the session must be size long with this SHA-256 (else the session is abandoned).
     * Android then asks the player (installStatus() follows).
     */
    public static int install(Activity activity, String path, long size, String sha256, long versionCode) {
        Context app = activity.getApplicationContext();
        PackageManager packages = activity.getPackageManager();
        PackageInfo archive = packages.getPackageArchiveInfo(path, 0);
        if (archive == null || !activity.getPackageName().equals(archive.packageName)
                || archive.getLongVersionCode() != versionCode) {
            Log.w(TAG, "Updates: " + path + " is not " + activity.getPackageName() + " " + versionCode);
            return INSTALL_NOT_THIS_APP;
        }
        registerReceiver(app);
        installStatus = INSTALL_WAITING;
        installMessage = "";
        PackageInstaller installer = packages.getPackageInstaller();
        PackageInstaller.SessionParams params =
                new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(activity.getPackageName());
        params.setSize(size);
        int sessionId = -1;
        PackageInstaller.Session session = null;
        try {
            sessionId = installer.createSession(params);
            session = installer.openSession(sessionId);
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            long written = 0;
            try (InputStream in = new FileInputStream(path);
                 OutputStream out = session.openWrite("base.apk", 0, size)) {
                byte[] block = new byte[1 << 16];
                int read;
                while ((read = in.read(block)) > 0) {
                    digest.update(block, 0, read);
                    out.write(block, 0, read);
                    written += read;
                }
                session.fsync(out);
            }
            if (written != size || !hex(digest.digest()).equals(sha256)) {
                Log.w(TAG, "Updates: " + path + " changed after it was checked");
                session.abandon();
                return INSTALL_MISMATCH;
            }
            // Explicit (this package): a mutable PendingIntent must not be implicit (Android 14).
            Intent intent = new Intent(ACTION_INSTALL_STATUS).setPackage(activity.getPackageName());
            PendingIntent pending = PendingIntent.getBroadcast(app, sessionId, intent,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
            session.commit(pending.getIntentSender());
            session.close();
            return INSTALL_STARTED;
        } catch (Exception e) {
            Log.w(TAG, "Updates: cannot install " + path + ": " + e);
            if (session != null) {
                session.abandon();
            }
            return INSTALL_ERROR;
        }
    }

    /** PackageInstaller.STATUS_* of the last session, or -100 while Android has not answered. */
    public static int installStatus() {
        return installStatus;
    }

    public static String installMessage() {
        return installMessage;
    }

    /** The browser on url. */
    public static void openUrl(Activity activity, String url) {
        try {
            activity.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (Exception e) {
            Log.w(TAG, "Updates: cannot open " + url + ": " + e);
        }
    }

    private static synchronized void registerReceiver(Context app) {
        if (receiver != null) {
            return;
        }
        receiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
                if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                    // Android's confirmation: shown over the game.
                    Intent confirm = confirmation(intent);
                    if (confirm != null) {
                        confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                        try {
                            context.startActivity(confirm);
                        } catch (Exception e) {
                            Log.w(TAG, "Updates: cannot ask for the installation: " + e);
                            installMessage = e.toString();
                            installStatus = PackageInstaller.STATUS_FAILURE;
                            return;
                        }
                    }
                    installStatus = status;
                    return;
                }
                String message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
                installMessage = message != null ? message : "";
                installStatus = status;
                Log.i(TAG, "Updates: installation status " + status + " " + installMessage);
            }
        };
        ContextCompat.registerReceiver(app, receiver, new IntentFilter(ACTION_INSTALL_STATUS),
                ContextCompat.RECEIVER_NOT_EXPORTED);
    }

    @SuppressWarnings("deprecation") // the typed getParcelableExtra is API 33; minSdk is 30
    private static Intent confirmation(Intent intent) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            return intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent.class);
        }
        Object extra = intent.getParcelableExtra(Intent.EXTRA_INTENT);
        return extra instanceof Intent ? (Intent) extra : null;
    }

    private static String hex(byte[] bytes) {
        StringBuilder text = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) {
            text.append(Character.forDigit((b >> 4) & 15, 16)).append(Character.forDigit(b & 15, 16));
        }
        return text.toString();
    }
}

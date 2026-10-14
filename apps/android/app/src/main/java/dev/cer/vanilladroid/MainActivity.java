package dev.cer.vanilladroid;

import android.Manifest;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;

import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;

import com.google.androidgamesdk.GameActivity;

/**
 * Thin Java shell. GameActivity loads libvanilladroid.so (android.app.lib_name) and runs
 * android_main() on its own thread; everything else lives in native code.
 */
public class MainActivity extends GameActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        hideSystemBars();
        if (savedInstanceState == null) {
            deliverLink(getIntent()); // phase 22 (pairing): opened by the PC page's link (not again when recreated)
        }
    }

    // Phase 22 (pairing): the PC page's "Link" button (vanilladroid://pair?h=...&k=..., AndroidManifest.xml) while the
    // app runs: the activity is singleTask, so it comes here; the game thread pairs with that PC (main_android.cpp).
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        deliverLink(intent);
    }

    private static void deliverLink(Intent intent) {
        if (intent != null && Intent.ACTION_VIEW.equals(intent.getAction()) && intent.getData() != null) {
            nativeOnLink(intent.getDataString());
        }
    }

    private static native void nativeOnLink(String uri);

    // Phase 22 (phonebake): baking the packs on the phone (engine/src/platform/BakeLauncher.cpp calls these from the
    // game's vd-bake thread): BakeService runs files/bake/plan.json in the process :baker. False when Android refused.
    public boolean startBake() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            // The notification is how the player follows it with the app closed: asked once, the bake runs anyway.
            runOnUiThread(() -> requestPermissions(new String[] {Manifest.permission.POST_NOTIFICATIONS}, 22));
        }
        try {
            startForegroundService(new Intent(this, BakeService.class).setAction(BakeService.ACTION_START));
            return true;
        } catch (RuntimeException e) {
            Log.w("VanillaDroid", "Phone bake: the service cannot start", e);
            return false;
        }
    }

    public boolean cancelBake() {
        try {
            startService(new Intent(this, BakeService.class).setAction(BakeService.ACTION_CANCEL));
            return true;
        } catch (RuntimeException e) {
            Log.w("VanillaDroid", "Phone bake: the service cannot be stopped", e);
            return false;
        }
    }

    // Audio focus (phase 9): asked for while the game is in front; the engine mutes or ducks its mix when another
    // app takes it (a call, navigation, a video), through nativeOnAudioFocusChange (main_android.cpp).
    private AudioManager audioManager;
    private AudioFocusRequest audioFocusRequest;

    @Override
    protected void onResume() {
        super.onResume();
        if (audioManager == null) {
            audioManager = (AudioManager) getSystemService(AUDIO_SERVICE);
            AudioAttributes attributes = new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_GAME)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build();
            audioFocusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                    .setAudioAttributes(attributes)
                    .setOnAudioFocusChangeListener(MainActivity::nativeOnAudioFocusChange)
                    .build();
        }
        if (audioManager != null) {
            int result = audioManager.requestAudioFocus(audioFocusRequest);
            nativeOnAudioFocusChange(result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
                    ? AudioManager.AUDIOFOCUS_GAIN : AudioManager.AUDIOFOCUS_LOSS_TRANSIENT);
        }
    }

    @Override
    protected void onPause() {
        if (audioManager != null) {
            audioManager.abandonAudioFocusRequest(audioFocusRequest);
        }
        super.onPause();
    }

    private static native void nativeOnAudioFocusChange(int focusChange);

    // Phase 10: the level of each memory warning, which GameActivity's native command leaves out (the engine lets
    // go of more of the streamed world the further the app is from the foreground).
    @Override
    public void onTrimMemory(int level) {
        nativeOnTrimMemory(level);
        super.onTrimMemory(level);
    }

    private static native void nativeOnTrimMemory(int level);

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemBars();
        }
    }

    private void hideSystemBars() {
        WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
        WindowInsetsControllerCompat controller =
                WindowCompat.getInsetsController(getWindow(), getWindow().getDecorView());
        controller.hide(WindowInsetsCompat.Type.systemBars());
        controller.setSystemBarsBehavior(WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }
}

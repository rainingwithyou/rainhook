package com.rain.mod;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.drawable.Icon;
import android.os.Build;

/**
 * Notification bridge for the Rain overlay.
 *
 * Lives in a tiny dex embedded in librain.so and loaded at runtime through an
 * InMemoryDexClassLoader — nothing is written to disk, and the class never
 * appears in the game's APK. It is instantiated from native code (JNI) and
 * handed the game's own Context, so every call below runs with MLBB's
 * identity: the notification is posted by the game, and the receiver is
 * registered on the game's process.
 *
 * Two actions:
 *   TOGGLE_UI  — flip the overlay's drawing on/off (the native side polls
 *                isUiVisible() each frame; the button is the user's switch).
 *   SHOW       — make the overlay visible again.
 *
 * Both are broadcast back to this same receiver as internal explicit
 * broadcasts, so no other app can drive the overlay and nothing is exported.
 */
public class RainNotify extends BroadcastReceiver {

    public static final String ACTION_TOGGLE = "com.rain.mod.TOGGLE_UI";
    public static final String ACTION_SHOW   = "com.rain.mod.SHOW_UI";

    private static final String CHANNEL_ID = "rain_overlay";
    private static final int NOTIFY_ID = 0x7A11;

    // The overlay's visibility as the native renderer sees it. Volatile: the
    // receiver runs on the main thread, the renderer polls from its own.
    private static volatile boolean sUiVisible = true;
    private static volatile boolean sRegistered = false;

    private Context mContext;

    /** Called from native once, with the game's Context. */
    public RainNotify(Context ctx) {
        mContext = ctx.getApplicationContext();
    }

    /** Native polls this each frame to decide whether to draw. */
    public static boolean isUiVisible() {
        return sUiVisible;
    }

    /** Native sets the initial state (so the notification text matches). */
    public static void setUiVisible(boolean v) {
        sUiVisible = v;
    }

    /**
     * Register the receiver and post the notification. Called from native on
     * the boot thread, once the game's Context is available.
     */
    public void install() {
        if (sRegistered) {
            return;
        }
        IntentFilter f = new IntentFilter();
        f.addAction(ACTION_TOGGLE);
        f.addAction(ACTION_SHOW);
        if (Build.VERSION.SDK_INT >= 33) {
            // API 33+ requires the flag; the app's own context means it is not
            // exported to anyone else either way.
            mContext.registerReceiver(this, f, Context.RECEIVER_NOT_EXPORTED);
        } else {
            mContext.registerReceiver(this, f);
        }
        sRegistered = true;
        post();
    }

    @Override
    public void onReceive(Context context, Intent intent) {
        String action = intent.getAction();
        if (ACTION_TOGGLE.equals(action)) {
            sUiVisible = !sUiVisible;
        } else if (ACTION_SHOW.equals(action)) {
            sUiVisible = true;
        } else {
            return;
        }
        post();
    }

    /**
     * Called from native on a slow timer. The game cancels its own
     * notifications on some scene transitions; if ours disappeared, re-post
     * it so the toggle stays reachable.
     */
    public void ensurePosted() {
        NotificationManager nm =
            (NotificationManager) mContext.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm == null) return;
        // notify() is idempotent for a fixed id — re-posting refreshes the
        // text and keeps the action alive.
        post();
    }

    /** Build and post (or update) the notification. */
    private void post() {
        NotificationManager nm =
            (NotificationManager) mContext.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm == null) {
            return;
        }
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationChannel ch = new NotificationChannel(
                CHANNEL_ID, "Rain", NotificationManager.IMPORTANCE_LOW);
            ch.setShowBadge(false);
            nm.createNotificationChannel(ch);
        }

        // The action must reach the dynamically-registered receiver. An
        // explicit Intent(context, Class) would target a manifest component
        // that does not exist, so target the package instead: same-app
        // delivery to this receiver's filter.
        Intent toggle = new Intent(ACTION_TOGGLE);
        toggle.setPackage(mContext.getPackageName());
        PendingIntent piToggle = PendingIntent.getBroadcast(
            mContext, 1, toggle,
            PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        Notification.Builder b = (Build.VERSION.SDK_INT >= 26)
            ? new Notification.Builder(mContext, CHANNEL_ID)
            : new Notification.Builder(mContext);
        b.setSmallIcon(android.R.drawable.ic_menu_view)
         .setContentTitle("Rain")
         .setContentText(sUiVisible ? "Overlay: visible" : "Overlay: hidden")
         .setOngoing(true)
         .setOnlyAlertOnce(true)
         .addAction(new Notification.Action.Builder(
             Icon.createWithResource(mContext, android.R.drawable.ic_menu_view),
             sUiVisible ? "Hide UI" : "Show UI",
             piToggle).build());
        nm.notify(NOTIFY_ID, b.build());
    }
}

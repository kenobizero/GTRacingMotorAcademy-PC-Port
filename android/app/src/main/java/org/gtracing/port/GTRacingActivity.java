package org.gtracing.port;

import android.Manifest;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.view.View;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

/**
 * GT Racing: Motor Academy HD on Android: SDL's activity running the host (libmain.so, host/core/main.c).
 *
 * The game's files go in GTRacing/ on the shared storage (GTRacing/GTRacingFreemiumHD.app/...), where any file
 * manager can copy them.  Writing there needs "All files access" on Android 11 and later: the first start asks
 * for it.  Without it the host uses the app's own folder (Android/data/org.gtracing.port/files) instead.
 */
public class GTRacingActivity extends SDLActivity {
    private static final int REQ_STORAGE = 1;

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        askForStorage();
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus)
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    /* the shared storage: All files access (Android 11+), else the storage permissions (Android 6-10) */
    private void askForStorage() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (!Environment.isExternalStorageManager()) {
                try {
                    startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                             Uri.parse("package:" + getPackageName())));
                } catch (Exception e) {
                    try {
                        startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                    } catch (Exception ignored) {
                    }
                }
            }
        } else if (checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                   != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE,
                                             Manifest.permission.WRITE_EXTERNAL_STORAGE}, REQ_STORAGE);
        }
    }

    /**
     * One HTTP(S) request for the host (host/core/android_jni.c, the Red Bull leaderboard: board.c).
     * The answer: the status as 4 big-endian bytes, then the body; null when there was no answer.
     * Called on the host's own threads, never the UI thread.
     */
    public static byte[] gtHttp(String method, String url, String body, int timeoutMs) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setRequestMethod(method);
            c.setConnectTimeout(timeoutMs);
            c.setReadTimeout(timeoutMs);
            c.setInstanceFollowRedirects(false);      // the body goes to that server only (as WinHTTP's)
            c.setUseCaches(false);
            c.setRequestProperty("User-Agent", "GTRacingPC-board/1");
            if (body != null) {
                byte[] b = body.getBytes(StandardCharsets.UTF_8);
                c.setDoOutput(true);
                c.setRequestProperty("Content-Type", "application/json");
                c.setFixedLengthStreamingMode(b.length);
                try (OutputStream o = c.getOutputStream()) {
                    o.write(b);
                }
            }
            int status = c.getResponseCode();
            InputStream in = status >= 400 ? c.getErrorStream() : c.getInputStream();
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            out.write(status >>> 24);
            out.write(status >>> 16);
            out.write(status >>> 8);
            out.write(status);
            if (in != null) {
                byte[] buf = new byte[8192];
                int n, total = 0;
                while ((n = in.read(buf)) > 0 && total < (1 << 20)) {
                    out.write(buf, 0, n);
                    total += n;
                }
                in.close();
            }
            return out.toByteArray();
        } catch (Exception e) {
            return null;
        } finally {
            if (c != null)
                c.disconnect();
        }
    }
}

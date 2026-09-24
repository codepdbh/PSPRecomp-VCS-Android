package com.psprecomp.vcs;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.PointF;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.SystemClock;
import android.provider.Settings;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.io.File;
import java.util.HashMap;
import java.util.Map;

public final class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static final int PSP_SELECT = 0x001, PSP_START = 0x008;
    private static final int PSP_UP = 0x010, PSP_RIGHT = 0x020, PSP_DOWN = 0x040, PSP_LEFT = 0x080;
    private static final int PSP_L = 0x100, PSP_R = 0x200;
    private static final int PSP_TRIANGLE = 0x1000, PSP_CIRCLE = 0x2000;
    private static final int PSP_CROSS = 0x4000, PSP_SQUARE = 0x8000;

    static { System.loadLibrary("vcs_android"); }
    private static native String nativeStatus();
    private static native void nativeSetSurface(Surface surface);
    private static native void nativeSetInput(int buttons, int analogX, int analogY,
                                              boolean accelerate, boolean brake);
    private static native void nativeStartGame(String gameRoot, String appDataDirectory);
    private static native void nativeStopGame();

    private SurfaceView surfaceView;
    private TextView statusView;
    private boolean surfaceReady, startRequested, permissionPrompted;
    private long gameStartAt;
    private final Handler handler = new Handler();
    private final Runnable statusPoll = new Runnable() {
        @Override public void run() {
            if (statusView != null) {
                String status = nativeStatus();
                statusView.setText(status);
                boolean running = status.startsWith("Juego en marcha.");
                statusView.setVisibility(running && SystemClock.uptimeMillis() - gameStartAt > 6000
                    ? View.GONE : View.VISIBLE);
            }
            handler.postDelayed(this, 1000);
        }
    };

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getDecorView().setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        surfaceView = new GameSurface();
        surfaceView.getHolder().setFormat(PixelFormat.RGBA_8888);
        surfaceView.getHolder().addCallback(this);
        root.addView(surfaceView, new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT, Gravity.CENTER));

        TouchOverlay controls = new TouchOverlay();
        root.addView(controls, new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT, Gravity.CENTER));

        statusView = new TextView(this);
        statusView.setTextColor(Color.WHITE);
        statusView.setTextSize(13);
        statusView.setPadding(18, 10, 18, 10);
        statusView.setBackgroundColor(0x99000000);
        statusView.setMaxLines(2);
        root.addView(statusView, new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.TOP | Gravity.CENTER_HORIZONTAL));
        setContentView(root);
        handler.post(statusPoll);
    }

    @Override protected void onResume() {
        super.onResume();
        maybeStartGame();
    }

    private void maybeStartGame() {
        if (startRequested || !surfaceReady) return;
        if (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
            if (!permissionPrompted) {
                permissionPrompted = true;
                statusView.setText("Permite acceso a archivos para leer el juego en Memoria interna/VCS.");
                try {
                    startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName())));
                } catch (Exception ignored) {
                    startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                }
            }
            return;
        }
        File gameRoot = new File(Environment.getExternalStorageDirectory(), "VCS");
        File elf = new File(gameRoot, "PSP_GAME/SYSDIR/EBOOT_DECRYPTED.ELF");
        if (!elf.isFile()) {
            statusView.setText("No encuentro el juego en Memoria interna/VCS/PSP_GAME.");
            return;
        }
        startRequested = true;
        gameStartAt = SystemClock.uptimeMillis();
        nativeStartGame(gameRoot.getAbsolutePath(), getFilesDir().getAbsolutePath());
    }

    @Override public void surfaceCreated(SurfaceHolder holder) {
        nativeSetSurface(holder.getSurface());
        surfaceReady = true;
        maybeStartGame();
    }
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        nativeSetSurface(holder.getSurface());
    }
    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        nativeSetSurface(null);
    }
    @Override protected void onDestroy() {
        handler.removeCallbacks(statusPoll);
        nativeStopGame();
        super.onDestroy();
    }

    private final class TouchOverlay extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Map<Integer, PointF> pointers = new HashMap<>();
        private int joystickPointerId = -1;
        private float analogX = 128, analogY = 128;
        TouchOverlay() {
            super(MainActivity.this);
            setLayerType(View.LAYER_TYPE_SOFTWARE, null);
            setOnTouchListener((view, event) -> handleTouch(event));
        }

        @Override protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
            int maxWidth = MeasureSpec.getSize(widthMeasureSpec);
            int maxHeight = MeasureSpec.getSize(heightMeasureSpec);
            int width = Math.min(maxWidth, maxHeight * 16 / 9);
            int height = width * 9 / 16;
            if (height > maxHeight) { height = maxHeight; width = height * 16 / 9; }
            setMeasuredDimension(width, height);
        }

        private boolean handleTouch(MotionEvent event) {
            int action = event.getActionMasked(), index = event.getActionIndex();
            if ((action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) &&
                joystickPointerId == -1 && event.getX(index) < getWidth() * .38f &&
                event.getY(index) > getHeight() * .36f)
                joystickPointerId = event.getPointerId(index);
            if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN ||
                action == MotionEvent.ACTION_MOVE) {
                for (int i = 0; i < event.getPointerCount(); ++i)
                    pointers.put(event.getPointerId(i), new PointF(event.getX(i), event.getY(i)));
            } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP ||
                       action == MotionEvent.ACTION_CANCEL) {
                if (action == MotionEvent.ACTION_CANCEL) {
                    pointers.clear();
                    joystickPointerId = -1;
                } else {
                    int releasedId = event.getPointerId(index);
                    pointers.remove(releasedId);
                    if (joystickPointerId == releasedId) joystickPointerId = -1;
                }
            }

            float w = Math.max(1, getWidth()), h = Math.max(1, getHeight());
            int buttons = 0;
            analogX = analogY = 128;
            boolean accelerate = false, brake = false;
            for (Map.Entry<Integer, PointF> entry : pointers.entrySet()) {
                PointF point = entry.getValue();
                float nx = point.x / w, ny = point.y / h;
                if (entry.getKey() == joystickPointerId) {
                    float dx = Math.max(-1, Math.min(1, (nx - .18f) / .20f));
                    float dy = Math.max(-1, Math.min(1, (ny - .70f) / .26f));
                    analogX = 128 + dx * 127;
                    analogY = 128 + dy * 127;
                } else if (nx >= .38f && nx <= .52f && ny >= .60f && ny <= .90f) {
                    // The PSP D-pad has different actions from its analog stick:
                    // D-pad right/left changes radio stations while driving.
                    float dx = (nx - .45f) / .055f;
                    float dy = (ny - .75f) / .10f;
                    if (dx < -.45f) buttons |= PSP_LEFT;
                    if (dx > .45f) buttons |= PSP_RIGHT;
                    if (dy < -.45f) buttons |= PSP_UP;
                    if (dy > .45f) buttons |= PSP_DOWN;
                } else if (ny < .20f && nx < .22f) buttons |= PSP_L;
                else if (ny < .20f && nx > .78f) buttons |= PSP_R;
                else if (ny < .18f) buttons |= nx < .46f ? PSP_SELECT : PSP_START;
                else if (nx > .60f && ny > .38f) {
                    float dx = (nx - .82f) / .16f, dy = (ny - .70f) / .22f;
                    if (Math.abs(dx) > Math.abs(dy)) buttons |= dx < 0 ? PSP_SQUARE : PSP_CIRCLE;
                    else buttons |= dy < 0 ? PSP_TRIANGLE : PSP_CROSS;
                    accelerate |= (buttons & PSP_CROSS) != 0;
                    brake |= (buttons & PSP_SQUARE) != 0;
                }
            }
            nativeSetInput(buttons, Math.round(analogX), Math.round(analogY), accelerate, brake);
            invalidate();
            return true;
        }

        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            float w = getWidth(), h = getHeight(), r = Math.min(w, h) * .105f;
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(0x55202020);
            canvas.drawCircle(w * .18f, h * .70f, r * 1.8f, paint);
            paint.setColor(0x8855CCFF);
            canvas.drawCircle(w * (.18f + (analogX - 128) / 127f * .12f),
                h * (.70f + (analogY - 128) / 127f * .12f), r * .72f, paint);
            drawButton(canvas, w * .45f, h * .65f, r * .32f, "\u2191", 0x665588AA);
            drawButton(canvas, w * .45f, h * .85f, r * .32f, "\u2193", 0x665588AA);
            drawButton(canvas, w * .395f, h * .75f, r * .32f, "\u2190", 0x665588AA);
            drawButton(canvas, w * .505f, h * .75f, r * .32f, "\u2192", 0x665588AA);
            drawButton(canvas, w * .82f, h * .49f, r * .58f, "△", 0x8877DD88);
            drawButton(canvas, w * .92f, h * .70f, r * .58f, "○", 0x88EE6677);
            drawButton(canvas, w * .82f, h * .91f, r * .58f, "×", 0x8877AAFF);
            drawButton(canvas, w * .72f, h * .70f, r * .58f, "□", 0x88DD77CC);
            drawButton(canvas, w * .12f, h * .16f, r * .38f, "L", 0x66777777);
            drawButton(canvas, w * .88f, h * .16f, r * .38f, "R", 0x66777777);
            paint.setColor(0xAAFFFFFF);
            paint.setTextSize(Math.max(14, r * .35f));
            canvas.drawText("SELECT", w * .39f, h * .13f, paint);
            canvas.drawText("START", w * .53f, h * .13f, paint);
        }

        private void drawButton(Canvas canvas, float x, float y, float radius,
                                String label, int color) {
            paint.setColor(color);
            canvas.drawCircle(x, y, radius, paint);
            paint.setColor(Color.WHITE);
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setTextSize(radius * 1.15f);
            canvas.drawText(label, x, y + radius * .38f, paint);
            paint.setTextAlign(Paint.Align.LEFT);
        }
    }

    private final class GameSurface extends SurfaceView {
        GameSurface() { super(MainActivity.this); }
        @Override protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
            int maxWidth = MeasureSpec.getSize(widthMeasureSpec);
            int maxHeight = MeasureSpec.getSize(heightMeasureSpec);
            int width = Math.min(maxWidth, maxHeight * 16 / 9);
            int height = width * 9 / 16;
            if (height > maxHeight) { height = maxHeight; width = height * 16 / 9; }
            setMeasuredDimension(width, height);
        }
    }
}

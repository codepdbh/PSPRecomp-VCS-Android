package com.psprecomp.vcs;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.SystemClock;
import android.provider.Settings;
import android.util.DisplayMetrics;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public final class MainActivity extends Activity implements SurfaceHolder.Callback {
    static { System.loadLibrary("vcs_android"); }
    private static native String nativeStatus();
    private static native void nativeSetSurface(Surface surface);
    private static native void nativeSetDisplaySize(int width, int height);
    private static native void nativeSetInput(int buttons, int analogX, int analogY,
                                              boolean accelerate, boolean brake);
    private static native void nativeStartGame(String gameRoot, String appDataDirectory);
    private static native void nativeStopGame();

    private static final String CONFIG_FILE = "VCSNative.ini";

    private SurfaceView surfaceView;
    private TouchControlsView controls;
    private TextView statusView;
    private boolean surfaceReady, startRequested, permissionPrompted;
    private long gameStartAt;
    private int panelWidth, panelHeight;

    // Touch and physical-controller state, merged in pushInput(): either can
    // drive the game, and a pad does not have to fight the touch overlay.
    private int touchButtons, touchAnalogX = 128, touchAnalogY = 128;
    private int padButtons, padHatButtons;
    private float padStickX, padStickY;
    private static final float PAD_DEAD_ZONE = 0.18f;
    // Hardware keyboard and mouse, bound like the desktop host (display_window.cpp).
    private int keyboardButtons, mouseButtons;
    private boolean keyW, keyA, keyS, keyD, keyWalk;
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
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            // Draw under the camera cutout too; the controls steer clear of it themselves.
            getWindow().getAttributes().layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        hideSystemBars();
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);

        // The panel's real size, landscape, for InternalResolutionMode=Desktop ("native").
        DisplayMetrics metrics = new DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(metrics);
        panelWidth = Math.max(metrics.widthPixels, metrics.heightPixels);
        panelHeight = Math.min(metrics.widthPixels, metrics.heightPixels);
        nativeSetDisplaySize(panelWidth, panelHeight);

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        surfaceView = new GameSurface();
        surfaceView.getHolder().setFormat(PixelFormat.RGBA_8888);
        surfaceView.getHolder().addCallback(this);
        root.addView(surfaceView, new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT, Gravity.CENTER));

        controls = new TouchControlsView(this, new TouchControlsView.Listener() {
            @Override public void onInput(int buttons, int analogX, int analogY,
                                          boolean accelerate, boolean brake) {
                touchButtons = buttons;
                touchAnalogX = analogX;
                touchAnalogY = analogY;
                pushInput();
            }
            @Override public void onSettingsRequested() {
                showSettingsDialog();
            }
            @Override public void onEditModeChanged(boolean editing) {
                if (editing) statusView.setVisibility(View.GONE);
            }
        });
        root.addView(controls, new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        statusView = new TextView(this);
        statusView.setTextColor(Color.WHITE);
        statusView.setTextSize(13);
        statusView.setPadding(18, 10, 18, 10);
        statusView.setBackgroundColor(0x99000000);
        statusView.setMaxLines(2);
        FrameLayout.LayoutParams statusParams = new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        root.addView(statusView, statusParams);
        setContentView(root);
        handler.post(statusPoll);
    }

    private void hideSystemBars() {
        getWindow().getDecorView().setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }

    @Override public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    @Override protected void onResume() {
        super.onResume();
        maybeStartGame();
    }

    @Override public void onBackPressed() {
        if (controls != null && controls.isEditing()) {
            controls.setEditMode(false);
            return;
        }
        super.onBackPressed();
    }

    @Override protected void onPause() {
        // Nothing may stay held while the app is in the background.
        padButtons = padHatButtons = keyboardButtons = mouseButtons = 0;
        padStickX = padStickY = 0f;
        keyW = keyA = keyS = keyD = keyWalk = false;
        if (controls != null) controls.releaseAll();
        super.onPause();
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

    // --- Physical controllers --------------------------------------------------------------

    private void pushInput() {
        int buttons = touchButtons | padButtons | padHatButtons | keyboardButtons | mouseButtons;
        int ax = touchAnalogX, ay = touchAnalogY;
        float magnitude = (float) Math.sqrt(padStickX * padStickX + padStickY * padStickY);
        if (keyW || keyA || keyS || keyD) {
            // WASD drives the analog stick, as on the desktop; Alt walks.
            float kx = (keyD ? 1f : 0f) - (keyA ? 1f : 0f);
            float ky = (keyS ? 1f : 0f) - (keyW ? 1f : 0f);
            float reach = keyWalk ? 60f : 127f;
            float length = (float) Math.sqrt(kx * kx + ky * ky);
            if (length > 0f) {
                ax = Math.round(128f + kx / length * reach);
                ay = Math.round(128f + ky / length * reach);
            }
        } else if (magnitude > PAD_DEAD_ZONE) {
            // Rescale past the dead zone so the edge of it reads as zero, not a jump.
            float scaled = Math.min(1f, (magnitude - PAD_DEAD_ZONE) / (1f - PAD_DEAD_ZONE));
            ax = Math.round(128f + padStickX / magnitude * scaled * 127f);
            ay = Math.round(128f + padStickY / magnitude * scaled * 127f);
        }
        ax = Math.max(0, Math.min(255, ax));
        ay = Math.max(0, Math.min(255, ay));
        nativeSetInput(buttons, ax, ay,
            (buttons & TouchControlsView.PSP_CROSS) != 0, (buttons & TouchControlsView.PSP_SQUARE) != 0);
    }

    private static boolean isController(InputDevice device, int source) {
        int sources = device != null ? device.getSources() : source;
        return (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
               (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    /** Standard Android gamepad keys, mapped by position to the PSP pad. */
    private static int padMask(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A: return TouchControlsView.PSP_CROSS;
            case KeyEvent.KEYCODE_BUTTON_B: return TouchControlsView.PSP_CIRCLE;
            case KeyEvent.KEYCODE_BUTTON_X: return TouchControlsView.PSP_SQUARE;
            case KeyEvent.KEYCODE_BUTTON_Y: return TouchControlsView.PSP_TRIANGLE;
            case KeyEvent.KEYCODE_BUTTON_L1:
            case KeyEvent.KEYCODE_BUTTON_L2: return TouchControlsView.PSP_L;
            case KeyEvent.KEYCODE_BUTTON_R1:
            case KeyEvent.KEYCODE_BUTTON_R2: return TouchControlsView.PSP_R;
            case KeyEvent.KEYCODE_BUTTON_START: return TouchControlsView.PSP_START;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return TouchControlsView.PSP_SELECT;
            case KeyEvent.KEYCODE_DPAD_UP: return TouchControlsView.PSP_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return TouchControlsView.PSP_DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT: return TouchControlsView.PSP_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return TouchControlsView.PSP_RIGHT;
            default: return 0;
        }
    }

    /** Keyboard bindings from the desktop host (kKeyBindings in display_window.cpp). */
    private static int keyboardMask(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_SPACE: return TouchControlsView.PSP_CROSS;       // sprint / handbrake
            case KeyEvent.KEYCODE_SHIFT_LEFT:
            case KeyEvent.KEYCODE_SHIFT_RIGHT: return TouchControlsView.PSP_SQUARE; // jump / brake
            case KeyEvent.KEYCODE_F:
            case KeyEvent.KEYCODE_ENTER: return TouchControlsView.PSP_TRIANGLE;    // enter / exit vehicle
            case KeyEvent.KEYCODE_Q: return TouchControlsView.PSP_LEFT;            // previous weapon / station
            case KeyEvent.KEYCODE_E: return TouchControlsView.PSP_RIGHT;           // next weapon / station
            case KeyEvent.KEYCODE_H: return TouchControlsView.PSP_L;               // horn
            case KeyEvent.KEYCODE_DPAD_UP: return TouchControlsView.PSP_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return TouchControlsView.PSP_DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT: return TouchControlsView.PSP_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return TouchControlsView.PSP_RIGHT;
            case KeyEvent.KEYCODE_ESCAPE: return TouchControlsView.PSP_START;      // pause
            case KeyEvent.KEYCODE_TAB: return TouchControlsView.PSP_SELECT;
            default: return 0;
        }
    }

    private boolean handleKeyboard(KeyEvent event) {
        boolean down = event.getAction() == KeyEvent.ACTION_DOWN;
        if (event.getAction() != KeyEvent.ACTION_DOWN && event.getAction() != KeyEvent.ACTION_UP) return false;
        switch (event.getKeyCode()) {
            case KeyEvent.KEYCODE_W: keyW = down; break;
            case KeyEvent.KEYCODE_A: keyA = down; break;
            case KeyEvent.KEYCODE_S: keyS = down; break;
            case KeyEvent.KEYCODE_D: keyD = down; break;
            case KeyEvent.KEYCODE_ALT_LEFT:
            case KeyEvent.KEYCODE_ALT_RIGHT: keyWalk = down; break;
            default: {
                int mask = keyboardMask(event.getKeyCode());
                if (mask == 0) return false;
                if (down) keyboardButtons |= mask; else keyboardButtons &= ~mask;
            }
        }
        if (down) controls.hideForController();
        pushInput();
        return true;
    }

    private static boolean isHardwareKeyboard(KeyEvent event) {
        InputDevice device = event.getDevice();
        return device != null && !device.isVirtual() &&
               (event.getSource() & InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD &&
               device.getKeyboardType() == InputDevice.KEYBOARD_TYPE_ALPHABETIC;
    }

    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        if (controls.isEditing()) return super.dispatchKeyEvent(event);
        if (!isController(event.getDevice(), event.getSource()) && isHardwareKeyboard(event) &&
            handleKeyboard(event)) return true;
        int mask = padMask(event.getKeyCode());
        if (mask != 0 && isController(event.getDevice(), event.getSource()) && !controls.isEditing()) {
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                padButtons |= mask;
                controls.hideForController();
            } else if (event.getAction() == KeyEvent.ACTION_UP) {
                padButtons &= ~mask;
            }
            pushInput();
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    /** Mouse buttons follow the desktop host: fire left, aim right, look behind middle. */
    private boolean handleMouse(MotionEvent event) {
        int state = event.getButtonState();
        int buttons = 0;
        if ((state & MotionEvent.BUTTON_PRIMARY) != 0) buttons |= TouchControlsView.PSP_CIRCLE;
        if ((state & MotionEvent.BUTTON_SECONDARY) != 0) buttons |= TouchControlsView.PSP_R;
        if ((state & MotionEvent.BUTTON_TERTIARY) != 0) buttons |= TouchControlsView.PSP_L;
        if (buttons != 0) controls.hideForController();
        if (buttons != mouseButtons) {
            mouseButtons = buttons;
            pushInput();
        }
        return true;
    }

    @Override public boolean dispatchTouchEvent(MotionEvent event) {
        // A mouse click arrives as a touch; keep it away from the on-screen pad.
        if (event.getPointerCount() > 0 && event.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE &&
            !controls.isEditing())
            return handleMouse(event);
        return super.dispatchTouchEvent(event);
    }

    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if ((event.getSource() & InputDevice.SOURCE_MOUSE) == InputDevice.SOURCE_MOUSE &&
            !controls.isEditing()) {
            int action = event.getActionMasked();
            if (action == MotionEvent.ACTION_BUTTON_PRESS || action == MotionEvent.ACTION_BUTTON_RELEASE)
                return handleMouse(event);
        }
        if (isController(event.getDevice(), event.getSource()) &&
            event.getActionMasked() == MotionEvent.ACTION_MOVE && !controls.isEditing()) {
            padStickX = event.getAxisValue(MotionEvent.AXIS_X);
            padStickY = event.getAxisValue(MotionEvent.AXIS_Y);
            // Many pads report the D-pad as a hat axis rather than as keys.
            float hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X);
            float hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
            int hat = 0;
            if (hatX < -0.5f) hat |= TouchControlsView.PSP_LEFT;
            if (hatX > 0.5f) hat |= TouchControlsView.PSP_RIGHT;
            if (hatY < -0.5f) hat |= TouchControlsView.PSP_UP;
            if (hatY > 0.5f) hat |= TouchControlsView.PSP_DOWN;
            // Analog triggers on pads that send no L2/R2 key events.
            if (event.getAxisValue(MotionEvent.AXIS_LTRIGGER) > 0.5f ||
                event.getAxisValue(MotionEvent.AXIS_BRAKE) > 0.5f) hat |= TouchControlsView.PSP_L;
            if (event.getAxisValue(MotionEvent.AXIS_RTRIGGER) > 0.5f ||
                event.getAxisValue(MotionEvent.AXIS_GAS) > 0.5f) hat |= TouchControlsView.PSP_R;
            padHatButtons = hat;
            if (hat != 0 || Math.abs(padStickX) > PAD_DEAD_ZONE || Math.abs(padStickY) > PAD_DEAD_ZONE)
                controls.hideForController();
            pushInput();
            return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }

    // --- Resolution ------------------------------------------------------------------------

    /** One choice in the resolution dialog: its label and the [Rendering] keys it writes. */
    private static final class ResolutionChoice {
        final String label, mode;
        final int scale;
        ResolutionChoice(String label, String mode, int scale) {
            this.label = label; this.mode = mode; this.scale = scale;
        }
    }

    /** Width the renderer actually uses at this height: widescreen fills the panel's shape. */
    private int widescreenWidth(int height) {
        return panelHeight > 0 ? Math.round(height * (float) panelWidth / panelHeight) : height * 480 / 272;
    }

    private List<ResolutionChoice> resolutionChoices() {
        List<ResolutionChoice> choices = new ArrayList<>();
        // Below HD: twice the PSP's own height, for the smoothest frame rate.
        choices.add(new ResolutionChoice(String.format(Locale.ROOT,
            "Rendimiento — %d×544 (más fluido)", widescreenWidth(544)), "Scale", 2));
        choices.add(new ResolutionChoice(String.format(Locale.ROOT, "HD — %d×816", widescreenWidth(816)),
            "Scale", 3));
        choices.add(new ResolutionChoice(String.format(Locale.ROOT, "Full HD — %d×1088", widescreenWidth(1088)),
            "Scale", 4));
        choices.add(new ResolutionChoice(String.format(Locale.ROOT,
            "Nativa de la pantalla — %d×%d", panelWidth, panelHeight), "Desktop", 0));
        return choices;
    }

    private void showSettingsDialog() {
        controls.releaseAll();
        String[] items = {"Resolución interna…", "Editar posición de controles", "Restablecer controles"};
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle("Ajustes")
            .setItems(items, (dialog, which) -> {
                if (which == 0) showResolutionDialog();
                else if (which == 1) controls.setEditMode(true);
                else controls.resetLayout();
            })
            .setNegativeButton("Cerrar", null)
            .setOnDismissListener(d -> hideSystemBars())
            .show();
    }

    private void showResolutionDialog() {
        controls.releaseAll();
        List<ResolutionChoice> choices = resolutionChoices();
        String[] labels = new String[choices.size()];
        for (int i = 0; i < labels.length; ++i) labels[i] = choices.get(i).label;
        int current = currentResolutionIndex(choices);
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle("Resolución interna")
            .setSingleChoiceItems(labels, current, (dialog, which) -> {
                dialog.dismiss();
                if (which == current) return;
                if (!writeResolution(choices.get(which))) {
                    statusView.setVisibility(View.VISIBLE);
                    statusView.setText("No se pudo guardar la resolución.");
                    return;
                }
                confirmRestart();
            })
            .setNegativeButton("Cancelar", null)
            .setOnDismissListener(d -> hideSystemBars())
            .show();
    }

    private void confirmRestart() {
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle("Reiniciar para aplicar")
            .setMessage("La nueva resolución se aplica al reiniciar el juego. "
                + "Se pierde lo que no hayas guardado.")
            .setPositiveButton("Reiniciar ahora", (d, w) -> restartApp())
            .setNegativeButton("Más tarde", null)
            .setOnDismissListener(d -> hideSystemBars())
            .show();
    }

    private void restartApp() {
        Intent launch = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (launch == null) return;
        Intent restart = Intent.makeRestartActivityTask(launch.getComponent());
        startActivity(restart);
        // The renderer is configured once per process; a fresh one picks up the new size.
        Runtime.getRuntime().exit(0);
    }

    private File configFile() {
        return new File(getFilesDir(), CONFIG_FILE);
    }

    private List<String> readConfig() {
        try {
            File file = configFile();
            if (file.isFile()) return new ArrayList<>(Files.readAllLines(file.toPath(), StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
        return new ArrayList<>();
    }

    private static String keyOf(String line) {
        int eq = line.indexOf('=');
        return eq < 0 ? "" : line.substring(0, eq).trim().toLowerCase(Locale.ROOT);
    }

    private static String valueOf(String line) {
        int eq = line.indexOf('=');
        return eq < 0 ? "" : line.substring(eq + 1).trim();
    }

    private int currentResolutionIndex(List<ResolutionChoice> choices) {
        String mode = "scale";
        int scale = 3; // the Android default, see RenderingConfiguration
        boolean inRendering = false;
        for (String raw : readConfig()) {
            String line = raw.trim();
            if (line.startsWith("[")) {
                inRendering = line.equalsIgnoreCase("[Rendering]");
                continue;
            }
            if (!inRendering) continue;
            String key = keyOf(line);
            if (key.equals("internalresolutionmode") || key.equals("internalmode"))
                mode = valueOf(line).toLowerCase(Locale.ROOT);
            else if (key.equals("internalscale") || key.equals("scale")) {
                try { scale = Integer.parseInt(valueOf(line)); } catch (NumberFormatException ignored) { }
            }
        }
        for (int i = 0; i < choices.size(); ++i) {
            ResolutionChoice c = choices.get(i);
            if (c.mode.equalsIgnoreCase("Desktop") &&
                (mode.equals("desktop") || mode.equals("native") || mode.equals("monitor"))) return i;
            if (c.mode.equalsIgnoreCase("Scale") && mode.startsWith("scale") && c.scale == scale) return i;
        }
        return -1;
    }

    /** Rewrites only the resolution keys of [Rendering], keeping everything else in the file. */
    private boolean writeResolution(ResolutionChoice choice) {
        List<String> lines = readConfig();
        List<String> out = new ArrayList<>();
        boolean inRendering = false, sawRendering = false;
        List<String> ours = new ArrayList<>();
        ours.add("InternalResolutionMode=" + choice.mode);
        if (choice.scale > 0) ours.add("InternalScale=" + choice.scale);
        for (String raw : lines) {
            String line = raw.trim();
            if (line.startsWith("[")) {
                inRendering = line.equalsIgnoreCase("[Rendering]");
                out.add(raw);
                if (inRendering) {
                    sawRendering = true;
                    out.addAll(ours);
                }
                continue;
            }
            if (inRendering) {
                String key = keyOf(line);
                if (key.equals("internalresolutionmode") || key.equals("internalmode") ||
                    key.equals("internalscale") || key.equals("scale")) continue;
            }
            out.add(raw);
        }
        if (!sawRendering) {
            out.add("[Rendering]");
            out.add("Backend=Vulkan");
            out.addAll(ours);
        }
        try {
            Files.write(configFile().toPath(), out, StandardCharsets.UTF_8);
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    // --- Surface ---------------------------------------------------------------------------

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

    /**
     * The game picture, edge to edge. The game renders widescreen (Hor+) at the
     * panel's own aspect - Widescreen is on by default on Android - so filling
     * the screen shows more of the world rather than stretching it.
     */
    private final class GameSurface extends SurfaceView {
        GameSurface() { super(MainActivity.this); }
    }
}

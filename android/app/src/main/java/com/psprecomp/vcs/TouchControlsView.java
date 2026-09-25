package com.psprecomp.vcs;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.DashPathEffect;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PointF;
import android.graphics.RectF;
import android.os.Build;
import android.view.DisplayCutout;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;

/**
 * On-screen PSP pad for Vice City Stories.
 *
 * Built on the same principles as reVC's TouchControlsView, which is where
 * this layout, look and layout editor come from:
 *
 *  - Every control is hit-tested against its OWN drawn shape (plus a small
 *    margin). The previous overlay tested broad screen strips that did not
 *    line up with what it drew, so some buttons fired from empty space and
 *    others missed when pressed dead centre.
 *  - The analog stick is circular and floating: the base appears wherever the
 *    thumb lands inside a generous zone, deflection is measured radially from
 *    there with a dead zone, and the knob is drawn exactly where it is read.
 *  - A finger that slides across the face buttons presses whichever one it is
 *    over, like rolling a thumb between buttons on a real pad.
 *  - Everything is laid out on the full screen, clear of the camera cutout,
 *    and every group can be dragged and resized in edit mode; the result is
 *    saved normalised to the screen, so it survives restarts.
 */
final class TouchControlsView extends View {
    // PSP pad bits, as sceCtrl reports them.
    static final int PSP_SELECT = 0x001, PSP_START = 0x008;
    static final int PSP_UP = 0x010, PSP_RIGHT = 0x020, PSP_DOWN = 0x040, PSP_LEFT = 0x080;
    static final int PSP_L = 0x100, PSP_R = 0x200;
    static final int PSP_TRIANGLE = 0x1000, PSP_CIRCLE = 0x2000;
    static final int PSP_CROSS = 0x4000, PSP_SQUARE = 0x8000;

    interface Listener {
        void onInput(int buttons, int analogX, int analogY, boolean accelerate, boolean brake);
        void onSettingsRequested();
        void onSaveStateRequested();
        /** A finger dragged over free screen: turn the camera by this many pixels. */
        void onCameraMotion(float dx, float dy);
        /** The on-screen right stick: -1..1 each, up positive, 0 at rest. */
        void onCameraStick(float x, float y);
        void onEditModeChanged(boolean editing);
    }

    private static final int SHAPE_CIRCLE = 0, SHAPE_PILL = 1, SHAPE_SHOULDER = 2;
    private static final int GLYPH_TEXT = 0, GLYPH_CROSS = 1, GLYPH_CIRCLE = 2,
            GLYPH_SQUARE = 3, GLYPH_TRIANGLE = 4, GLYPH_ARROW = 5, GLYPH_GEAR = 6,
            GLYPH_DISK = 7;

    // Groups move and scale as one unit in the editor.
    private static final int G_STICK = 0, G_DPAD = 1, G_FACE = 2, G_L = 3, G_R = 4,
            G_SELECT = 5, G_START = 6, G_CAMERA = 7, G_COUNT = 8;
    private static final String[] GROUP_KEYS = {"stick", "dpad", "face", "l", "r", "select", "start", "camera"};
    private static final float MIN_SCALE = 0.6f, MAX_SCALE = 1.8f, SCALE_STEP = 0.1f;

    private static final class Control {
        final int mask;          // PSP bit, or 0 for the settings button
        final int shape;
        final int glyph;
        final String label;
        final int accent;
        final boolean slidable;  // face/D-pad buttons: a finger can roll between them
        final int group;         // -1: not editable (the settings gear)
        int arrowDirection;      // GLYPH_ARROW: 0 up, 1 right, 2 down, 3 left
        final RectF bounds = new RectF();
        float hitMargin;
        int pointerId = -1;
        boolean pressed;

        Control(int mask, int shape, int glyph, String label, int accent, boolean slidable, int group) {
            this.mask = mask;
            this.shape = shape;
            this.glyph = glyph;
            this.label = label;
            this.accent = accent;
            this.slidable = slidable;
            this.group = group;
        }

        boolean hit(float x, float y) {
            if (shape == SHAPE_CIRCLE) {
                float r = bounds.width() / 2f + hitMargin;
                float dx = x - bounds.centerX(), dy = y - bounds.centerY();
                return dx * dx + dy * dy <= r * r;
            }
            return x >= bounds.left - hitMargin && x <= bounds.right + hitMargin &&
                   y >= bounds.top - hitMargin && y <= bounds.bottom + hitMargin;
        }

        /** Distance from the control's centre, normalised by its size: ties go to the closest. */
        float closeness(float x, float y) {
            float dx = (x - bounds.centerX()) / Math.max(1f, bounds.width());
            float dy = (y - bounds.centerY()) / Math.max(1f, bounds.height());
            return dx * dx + dy * dy;
        }
    }

    // PlayStation accent colours.
    private static final int COLOR_TRIANGLE = 0xFF3FD6A0;
    private static final int COLOR_CIRCLE = 0xFFFF5C6C;
    private static final int COLOR_CROSS = 0xFF6FA8FF;
    private static final int COLOR_SQUARE = 0xFFE58CE0;
    private static final int COLOR_NEUTRAL = 0xFFECECEC;
    private static final int COLOR_EDIT = 0xFFFFC94D;

    private final Control triangle = new Control(PSP_TRIANGLE, SHAPE_CIRCLE, GLYPH_TRIANGLE, null, COLOR_TRIANGLE, true, G_FACE);
    private final Control circle = new Control(PSP_CIRCLE, SHAPE_CIRCLE, GLYPH_CIRCLE, null, COLOR_CIRCLE, true, G_FACE);
    private final Control cross = new Control(PSP_CROSS, SHAPE_CIRCLE, GLYPH_CROSS, null, COLOR_CROSS, true, G_FACE);
    private final Control square = new Control(PSP_SQUARE, SHAPE_CIRCLE, GLYPH_SQUARE, null, COLOR_SQUARE, true, G_FACE);
    private final Control shoulderL = new Control(PSP_L, SHAPE_SHOULDER, GLYPH_TEXT, "L", COLOR_NEUTRAL, false, G_L);
    private final Control shoulderR = new Control(PSP_R, SHAPE_SHOULDER, GLYPH_TEXT, "R", COLOR_NEUTRAL, false, G_R);
    private final Control select = new Control(PSP_SELECT, SHAPE_PILL, GLYPH_TEXT, "SELECT", COLOR_NEUTRAL, false, G_SELECT);
    private final Control start = new Control(PSP_START, SHAPE_PILL, GLYPH_TEXT, "START", COLOR_NEUTRAL, false, G_START);
    private final Control dpadUp = new Control(PSP_UP, SHAPE_CIRCLE, GLYPH_ARROW, null, COLOR_NEUTRAL, true, G_DPAD);
    private final Control dpadRight = new Control(PSP_RIGHT, SHAPE_CIRCLE, GLYPH_ARROW, null, COLOR_NEUTRAL, true, G_DPAD);
    private final Control dpadDown = new Control(PSP_DOWN, SHAPE_CIRCLE, GLYPH_ARROW, null, COLOR_NEUTRAL, true, G_DPAD);
    private final Control dpadLeft = new Control(PSP_LEFT, SHAPE_CIRCLE, GLYPH_ARROW, null, COLOR_NEUTRAL, true, G_DPAD);
    private final Control settings = new Control(0, SHAPE_CIRCLE, GLYPH_GEAR, null, COLOR_NEUTRAL, false, -1);
    private final Control saveState = new Control(0, SHAPE_CIRCLE, GLYPH_DISK, null, COLOR_NEUTRAL, false, -1);
    {
        dpadUp.arrowDirection = 0;
        dpadRight.arrowDirection = 1;
        dpadDown.arrowDirection = 2;
        dpadLeft.arrowDirection = 3;
    }
    private final Control[] controls = {
        triangle, circle, cross, square, shoulderL, shoulderR, select, start,
        dpadUp, dpadRight, dpadDown, dpadLeft, settings, saveState,
    };

    // Analog stick.
    private final RectF stickZone = new RectF();   // where a thumb may land to grab it
    private final PointF stickHome = new PointF(); // resting spot, drawn when idle
    private final PointF stickBase = new PointF(); // where this drag's base sits
    private final PointF stickKnob = new PointF();
    private float stickRadius, knobRadius;
    private int stickPointer = -1;
    private float stickX, stickY; // -1..1 after dead zone
    private static final float DEAD_ZONE = 0.12f;

    // Camera: any finger that lands on free screen, away from the buttons and
    // the stick's zone, looks around while it moves, like a mouse.
    private int cameraPointer = -1;
    private float cameraLastX, cameraLastY;

    // Right stick: the camera as a joystick. Floating like the left one, in a
    // zone left of the face buttons; its deflection is a turn rate.
    private final RectF camZone = new RectF();
    private final PointF camHome = new PointF(), camBase = new PointF(), camKnob = new PointF();
    private float camRadius, camKnobRadius;
    private int camPointer = -1;
    private float camX, camY;

    // Layout customisation: offsets are fractions of the screen, scale is relative.
    private final float[] groupDx = new float[G_COUNT];
    private final float[] groupDy = new float[G_COUNT];
    private final float[] groupScale = new float[G_COUNT];
    private final RectF[] groupBounds = new RectF[G_COUNT];
    private final SharedPreferences prefs;

    // Hidden while a physical controller is in use; the next touch brings the
    // controls back (and is swallowed, so it does not also press something).
    private boolean hidden;

    // Editor state.
    private boolean editMode;
    private int selectedGroup = G_FACE;
    private int dragPointer = -1;
    private float dragLastX, dragLastY;
    private final RectF editShrink = new RectF(), editGrow = new RectF(),
            editReset = new RectF(), editDone = new RectF();

    private final Listener listener;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private int safeLeft, safeTop, safeRight, safeBottom;
    private int lastButtons = -1, lastAnalogX = -1, lastAnalogY = -1;

    TouchControlsView(Context context, Listener listener) {
        super(context);
        this.listener = listener;
        prefs = context.getSharedPreferences("touch_layout", Context.MODE_PRIVATE);
        for (int g = 0; g < G_COUNT; ++g) {
            groupBounds[g] = new RectF();
            groupDx[g] = prefs.getFloat(GROUP_KEYS[g] + "_dx", 0f);
            groupDy[g] = prefs.getFloat(GROUP_KEYS[g] + "_dy", 0f);
            groupScale[g] = clampScale(prefs.getFloat(GROUP_KEYS[g] + "_scale", 1f));
        }
        stroke.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        setOnApplyWindowInsetsListener((view, insets) -> {
            applyCutout(insets);
            return insets;
        });
    }

    private static float clampScale(float scale) {
        return Math.max(MIN_SCALE, Math.min(MAX_SCALE, scale));
    }

    private void applyCutout(WindowInsets insets) {
        int l = 0, t = 0, r = 0, b = 0;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            DisplayCutout cutout = insets.getDisplayCutout();
            if (cutout != null) {
                l = cutout.getSafeInsetLeft();
                t = cutout.getSafeInsetTop();
                r = cutout.getSafeInsetRight();
                b = cutout.getSafeInsetBottom();
            }
        }
        if (l != safeLeft || t != safeTop || r != safeRight || b != safeBottom) {
            safeLeft = l; safeTop = t; safeRight = r; safeBottom = b;
            layoutControls(getWidth(), getHeight());
            invalidate();
        }
    }

    @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        layoutControls(w, h);
    }

    private static void placeCircle(Control c, float cx, float cy, float radius) {
        c.bounds.set(cx - radius, cy - radius, cx + radius, cy + radius);
    }

    /** Moves and scales one group of controls about its default anchor. */
    private void transformGroup(int group, float anchorX, float anchorY, int w, int h) {
        float scale = groupScale[group];
        float nx = anchorX + groupDx[group] * w, ny = anchorY + groupDy[group] * h;
        RectF gb = groupBounds[group];
        gb.setEmpty();
        for (Control c : controls) {
            if (c.group != group) continue;
            float cx = nx + (c.bounds.centerX() - anchorX) * scale;
            float cy = ny + (c.bounds.centerY() - anchorY) * scale;
            float hw = c.bounds.width() / 2f * scale, hh = c.bounds.height() / 2f * scale;
            c.bounds.set(cx - hw, cy - hh, cx + hw, cy + hh);
            c.hitMargin *= scale;
            if (gb.isEmpty()) gb.set(c.bounds); else gb.union(c.bounds);
        }
    }

    private void layoutControls(int w, int h) {
        if (w <= 0 || h <= 0) return;
        float left = safeLeft, top = safeTop, right = w - safeRight, bottom = h - safeBottom;
        float unit = Math.min(right - left, bottom - top); // the short side, i.e. screen height in landscape
        float margin = unit * 0.045f;

        // Analog stick: bottom-left, big enough to steer precisely.
        float defaultStickRadius = unit * 0.155f;
        float homeX = left + margin + defaultStickRadius * 1.25f;
        float homeY = bottom - margin - defaultStickRadius * 1.15f;

        // Face buttons: diamond, bottom-right.
        float faceR = unit * 0.078f;
        float spread = faceR * 1.55f;
        float faceCx = right - margin - spread - faceR;
        float faceCy = bottom - margin - spread - faceR;
        placeCircle(triangle, faceCx, faceCy - spread, faceR);
        placeCircle(cross, faceCx, faceCy + spread, faceR);
        placeCircle(square, faceCx - spread, faceCy, faceR);
        placeCircle(circle, faceCx + spread, faceCy, faceR);
        for (Control c : new Control[]{triangle, cross, square, circle})
            c.hitMargin = faceR * 0.28f; // ~ fills the gaps of the diamond without overlapping

        // D-pad: cross just right of the stick, low, clear of both hands' main controls.
        float padR = unit * 0.050f;
        float padSpread = padR * 1.35f;
        float padCx = homeX + defaultStickRadius + margin * 1.4f + padSpread + padR;
        float padCy = bottom - margin - padSpread - padR;
        placeCircle(dpadUp, padCx, padCy - padSpread, padR);
        placeCircle(dpadDown, padCx, padCy + padSpread, padR);
        placeCircle(dpadLeft, padCx - padSpread, padCy, padR);
        placeCircle(dpadRight, padCx + padSpread, padCy, padR);
        for (Control c : new Control[]{dpadUp, dpadDown, dpadLeft, dpadRight})
            c.hitMargin = padR * 0.18f;

        // Shoulders: wide tabs in the top corners, where index fingers rest.
        float shoulderW = unit * 0.30f, shoulderH = unit * 0.13f;
        shoulderL.bounds.set(left + margin, top + margin, left + margin + shoulderW, top + margin + shoulderH);
        shoulderR.bounds.set(right - margin - shoulderW, top + margin, right - margin, top + margin + shoulderH);
        shoulderL.hitMargin = shoulderR.hitMargin = margin * 0.6f;

        // Select / Start: pills, top centre, with the settings gear between them.
        float pillW = unit * 0.20f, pillH = unit * 0.085f;
        float cx = (left + right) / 2f;
        float pillY = top + margin;
        float gearR = pillH * 0.62f;
        float gap = gearR + margin * 0.8f;
        select.bounds.set(cx - gap - pillW, pillY, cx - gap, pillY + pillH);
        start.bounds.set(cx + gap, pillY, cx + gap + pillW, pillY + pillH);
        select.hitMargin = start.hitMargin = margin * 0.4f;
        placeCircle(settings, cx, pillY + pillH / 2f, gearR);
        settings.hitMargin = margin * 0.3f;
        // Save states: their own button, just right of START.
        placeCircle(saveState, cx + gap + pillW + margin * 0.8f + gearR, pillY + pillH / 2f, gearR);
        saveState.hitMargin = margin * 0.3f;

        // The player's customisation, on top of the defaults.
        transformGroup(G_FACE, faceCx, faceCy, w, h);
        transformGroup(G_DPAD, padCx, padCy, w, h);
        transformGroup(G_L, shoulderL.bounds.centerX(), shoulderL.bounds.centerY(), w, h);
        transformGroup(G_R, shoulderR.bounds.centerX(), shoulderR.bounds.centerY(), w, h);
        transformGroup(G_SELECT, select.bounds.centerX(), select.bounds.centerY(), w, h);
        transformGroup(G_START, start.bounds.centerX(), start.bounds.centerY(), w, h);

        stickRadius = defaultStickRadius * groupScale[G_STICK];
        knobRadius = stickRadius * 0.46f;
        stickHome.set(homeX + groupDx[G_STICK] * w, homeY + groupDy[G_STICK] * h);
        groupBounds[G_STICK].set(stickHome.x - stickRadius, stickHome.y - stickRadius,
                                 stickHome.x + stickRadius, stickHome.y + stickRadius);
        // The grab zone is generous around wherever the stick now rests, but
        // never swallows the D-pad or reaches past the middle of the screen.
        stickZone.set(stickHome.x - stickRadius * 2.2f, stickHome.y - stickRadius * 2.4f,
                      stickHome.x + stickRadius * 2.2f, stickHome.y + stickRadius * 2.4f);
        stickZone.intersect(left, top + (bottom - top) * 0.22f, (left + right) / 2f, bottom);
        RectF pad = groupBounds[G_DPAD];
        if (RectF.intersects(stickZone, pad) && pad.left > stickHome.x)
            stickZone.right = Math.max(stickHome.x + stickRadius, pad.left - padR * 0.3f);
        if (stickPointer == -1) {
            stickBase.set(stickHome);
            stickKnob.set(stickHome);
        }

        // Right (camera) stick: left of the face buttons, as low as the left stick.
        float defaultCamRadius = unit * 0.13f;
        RectF face = groupBounds[G_FACE];
        float camHomeX = faceCx - spread - faceR - margin * 1.2f - defaultCamRadius;
        float camHomeY = bottom - margin - defaultCamRadius * 1.1f;
        camRadius = defaultCamRadius * groupScale[G_CAMERA];
        camKnobRadius = camRadius * 0.46f;
        camHome.set(camHomeX + groupDx[G_CAMERA] * w, camHomeY + groupDy[G_CAMERA] * h);
        groupBounds[G_CAMERA].set(camHome.x - camRadius, camHome.y - camRadius,
                                  camHome.x + camRadius, camHome.y + camRadius);
        camZone.set(camHome.x - camRadius * 1.8f, camHome.y - camRadius * 2.0f,
                    camHome.x + camRadius * 1.8f, camHome.y + camRadius * 2.0f);
        camZone.intersect((left + right) / 2f, top + (bottom - top) * 0.22f, right, bottom);
        if (RectF.intersects(camZone, face) && face.left > camHome.x)
            camZone.right = Math.max(camHome.x + camRadius, face.left - faceR * 0.2f);
        if (camPointer == -1) {
            camBase.set(camHome);
            camKnob.set(camHome);
        }

        // Editor toolbar, centre of the screen.
        float bw = unit * 0.20f, bh = unit * 0.11f, bgap = unit * 0.03f;
        float total = bw * 4f + bgap * 3f;
        float x0 = cx - total / 2f, y0 = (top + bottom) / 2f - bh / 2f;
        editShrink.set(x0, y0, x0 + bw, y0 + bh);
        editGrow.set(x0 + (bw + bgap), y0, x0 + (bw + bgap) + bw, y0 + bh);
        editReset.set(x0 + 2f * (bw + bgap), y0, x0 + 2f * (bw + bgap) + bw, y0 + bh);
        editDone.set(x0 + 3f * (bw + bgap), y0, x0 + 3f * (bw + bgap) + bw, y0 + bh);
    }

    // --- Editor ----------------------------------------------------------------------------

    boolean isEditing() {
        return editMode;
    }

    /** Called when a gamepad is used: get the touch controls out of the way. */
    void hideForController() {
        if (hidden || editMode) return;
        releaseAll();
        hidden = true;
        invalidate();
    }

    void setEditMode(boolean editing) {
        if (editMode == editing) return;
        releaseAll();
        editMode = editing;
        dragPointer = -1;
        listener.onEditModeChanged(editing);
        invalidate();
    }

    void resetLayout() {
        SharedPreferences.Editor editor = prefs.edit();
        for (int g = 0; g < G_COUNT; ++g) {
            groupDx[g] = groupDy[g] = 0f;
            groupScale[g] = 1f;
            editor.remove(GROUP_KEYS[g] + "_dx").remove(GROUP_KEYS[g] + "_dy")
                  .remove(GROUP_KEYS[g] + "_scale");
        }
        editor.apply();
        layoutControls(getWidth(), getHeight());
        invalidate();
    }

    private void saveGroup(int g) {
        prefs.edit()
            .putFloat(GROUP_KEYS[g] + "_dx", groupDx[g])
            .putFloat(GROUP_KEYS[g] + "_dy", groupDy[g])
            .putFloat(GROUP_KEYS[g] + "_scale", groupScale[g])
            .apply();
    }

    private int groupAt(float x, float y) {
        int best = -1;
        float bestArea = Float.MAX_VALUE;
        for (int g = 0; g < G_COUNT; ++g) {
            RectF r = new RectF(groupBounds[g]);
            r.inset(-getHeight() * 0.02f, -getHeight() * 0.02f);
            if (!r.contains(x, y)) continue;
            float area = r.width() * r.height(); // smallest wins: L over a big stick, etc.
            if (area < bestArea) { best = g; bestArea = area; }
        }
        return best;
    }

    private void editTouch(MotionEvent event) {
        int action = event.getActionMasked();
        int index = event.getActionIndex();
        float x = event.getX(index), y = event.getY(index);
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                if (dragPointer != -1) break;
                if (editShrink.contains(x, y) || editGrow.contains(x, y)) {
                    float delta = editGrow.contains(x, y) ? SCALE_STEP : -SCALE_STEP;
                    groupScale[selectedGroup] = clampScale(groupScale[selectedGroup] + delta);
                    saveGroup(selectedGroup);
                    layoutControls(getWidth(), getHeight());
                    break;
                }
                if (editReset.contains(x, y)) { resetLayout(); break; }
                if (editDone.contains(x, y)) { setEditMode(false); break; }
                int g = groupAt(x, y);
                if (g >= 0) {
                    selectedGroup = g;
                    dragPointer = event.getPointerId(index);
                    dragLastX = x;
                    dragLastY = y;
                }
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < event.getPointerCount(); ++i) {
                    if (event.getPointerId(i) != dragPointer) continue;
                    float mx = event.getX(i), my = event.getY(i);
                    groupDx[selectedGroup] += (mx - dragLastX) / Math.max(1, getWidth());
                    groupDy[selectedGroup] += (my - dragLastY) / Math.max(1, getHeight());
                    dragLastX = mx;
                    dragLastY = my;
                    layoutControls(getWidth(), getHeight());
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL:
                if (event.getPointerId(index) == dragPointer || action == MotionEvent.ACTION_CANCEL) {
                    if (dragPointer != -1) saveGroup(selectedGroup);
                    dragPointer = -1;
                }
                break;
        }
    }

    // --- Touch -----------------------------------------------------------------------------

    @Override public boolean onTouchEvent(MotionEvent event) {
        if (hidden) {
            if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
                hidden = false;
                invalidate();
            }
            return true;
        }
        if (editMode) {
            editTouch(event);
            invalidate();
            return true;
        }
        int action = event.getActionMasked();
        int index = event.getActionIndex();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                down(event.getPointerId(index), event.getX(index), event.getY(index));
                break;
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < event.getPointerCount(); ++i)
                    move(event.getPointerId(i), event.getX(i), event.getY(i));
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                up(event.getPointerId(index));
                break;
            case MotionEvent.ACTION_CANCEL:
                releaseAll();
                break;
        }
        publish();
        invalidate();
        return true;
    }

    private Control controlAt(float x, float y) {
        Control best = null;
        float bestCloseness = Float.MAX_VALUE;
        for (Control c : controls) {
            if (!c.hit(x, y)) continue;
            float closeness = c.closeness(x, y);
            if (closeness < bestCloseness) {
                best = c;
                bestCloseness = closeness;
            }
        }
        return best;
    }

    private void down(int id, float x, float y) {
        Control c = controlAt(x, y);
        if (c != null) {
            if (c == settings) {
                listener.onSettingsRequested();
                return;
            }
            if (c == saveState) {
                listener.onSaveStateRequested();
                return;
            }
            c.pointerId = id;
            c.pressed = true;
            return;
        }
        if (stickPointer == -1 && stickZone.contains(x, y)) {
            stickPointer = id;
            // Floating base: centred under the thumb, but kept on screen.
            float bx = Math.max(safeLeft + stickRadius, Math.min(x, getWidth() - safeRight - stickRadius));
            float by = Math.max(safeTop + stickRadius, Math.min(y, getHeight() - safeBottom - stickRadius));
            stickBase.set(bx, by);
            updateStick(x, y);
            return;
        }
        if (camPointer == -1 && camZone.contains(x, y)) {
            camPointer = id;
            float bx = Math.max(safeLeft + camRadius, Math.min(x, getWidth() - safeRight - camRadius));
            float by = Math.max(safeTop + camRadius, Math.min(y, getHeight() - safeBottom - camRadius));
            camBase.set(bx, by);
            updateCamStick(x, y);
            return;
        }
        if (cameraPointer == -1) {
            cameraPointer = id;
            cameraLastX = x;
            cameraLastY = y;
        }
    }

    private void move(int id, float x, float y) {
        if (id == stickPointer) {
            updateStick(x, y);
            return;
        }
        if (id == camPointer) {
            updateCamStick(x, y);
            return;
        }
        if (id == cameraPointer) {
            float dx = x - cameraLastX, dy = y - cameraLastY;
            cameraLastX = x;
            cameraLastY = y;
            if (dx != 0f || dy != 0f) listener.onCameraMotion(dx, dy);
            return;
        }
        for (Control c : controls) {
            if (c.pointerId != id) continue;
            if (!c.slidable || c.hit(x, y)) return;
            // Rolled off a face/D-pad button: hand the finger to whichever one it is over now.
            Control next = controlAt(x, y);
            if (next != null && next.slidable && next.pointerId == -1) {
                c.pointerId = -1;
                c.pressed = false;
                next.pointerId = id;
                next.pressed = true;
            }
            return;
        }
    }

    private void up(int id) {
        if (id == cameraPointer) cameraPointer = -1;
        if (id == camPointer) releaseCamStick();
        if (id == stickPointer) {
            stickPointer = -1;
            stickX = stickY = 0f;
            stickBase.set(stickHome);
            stickKnob.set(stickHome);
        }
        for (Control c : controls) {
            if (c.pointerId == id) {
                c.pointerId = -1;
                c.pressed = false;
            }
        }
    }

    void releaseAll() {
        cameraPointer = -1;
        releaseCamStick();
        stickPointer = -1;
        stickX = stickY = 0f;
        stickBase.set(stickHome);
        stickKnob.set(stickHome);
        for (Control c : controls) {
            c.pointerId = -1;
            c.pressed = false;
        }
        publish();
        invalidate();
    }

    private void updateStick(float x, float y) {
        float dx = x - stickBase.x, dy = y - stickBase.y;
        float distance = (float) Math.sqrt(dx * dx + dy * dy);
        if (distance > stickRadius) {
            dx = dx / distance * stickRadius;
            dy = dy / distance * stickRadius;
            distance = stickRadius;
        }
        stickKnob.set(stickBase.x + dx, stickBase.y + dy);
        // Radial dead zone, then rescale so the edge of it reads as zero rather than jumping.
        float magnitude = distance / stickRadius;
        if (magnitude < DEAD_ZONE || distance <= 0f) {
            stickX = stickY = 0f;
        } else {
            float scaled = (magnitude - DEAD_ZONE) / (1f - DEAD_ZONE);
            stickX = dx / distance * scaled;
            stickY = dy / distance * scaled;
        }
    }

    private void releaseCamStick() {
        boolean wasActive = camPointer != -1 || camX != 0f || camY != 0f;
        camPointer = -1;
        camX = camY = 0f;
        camBase.set(camHome);
        camKnob.set(camHome);
        if (wasActive) listener.onCameraStick(0f, 0f);
    }

    private void updateCamStick(float x, float y) {
        float dx = x - camBase.x, dy = y - camBase.y;
        float distance = (float) Math.sqrt(dx * dx + dy * dy);
        if (distance > camRadius) {
            dx = dx / distance * camRadius;
            dy = dy / distance * camRadius;
            distance = camRadius;
        }
        camKnob.set(camBase.x + dx, camBase.y + dy);
        float magnitude = distance / camRadius;
        if (magnitude < DEAD_ZONE || distance <= 0f) {
            camX = camY = 0f;
        } else {
            float scaled = (magnitude - DEAD_ZONE) / (1f - DEAD_ZONE);
            camX = dx / distance * scaled;
            camY = dy / distance * scaled;
        }
        listener.onCameraStick(camX, -camY);
    }

    private void publish() {
        int buttons = 0;
        for (Control c : controls)
            if (c.pressed) buttons |= c.mask;
        int ax = Math.round(128f + stickX * 127f);
        int ay = Math.round(128f + stickY * 127f);
        ax = Math.max(0, Math.min(255, ax));
        ay = Math.max(0, Math.min(255, ay));
        if (buttons == lastButtons && ax == lastAnalogX && ay == lastAnalogY) return;
        lastButtons = buttons; lastAnalogX = ax; lastAnalogY = ay;
        listener.onInput(buttons, ax, ay, (buttons & PSP_CROSS) != 0, (buttons & PSP_SQUARE) != 0);
    }

    // --- Drawing ---------------------------------------------------------------------------

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (hidden) return;
        if (editMode) {
            fill.setStyle(Paint.Style.FILL);
            fill.setColor(0x66000000);
            canvas.drawRect(0, 0, getWidth(), getHeight(), fill);
        }
        drawStick(canvas);
        for (Control c : controls) {
            if (editMode && (c == settings || c == saveState)) continue;
            drawControl(canvas, c);
        }
        if (editMode) drawEditor(canvas);
    }

    private void drawEditor(Canvas canvas) {
        // Every group gets a dashed frame; the selected one is highlighted.
        stroke.setStrokeWidth(Math.max(3f, getHeight() * 0.004f));
        stroke.setPathEffect(new DashPathEffect(new float[]{18f, 12f}, 0f));
        for (int g = 0; g < G_COUNT; ++g) {
            RectF r = new RectF(groupBounds[g]);
            r.inset(-getHeight() * 0.015f, -getHeight() * 0.015f);
            stroke.setColor(g == selectedGroup ? COLOR_EDIT : 0x99FFFFFF);
            canvas.drawRoundRect(r, 24f, 24f, stroke);
        }
        stroke.setPathEffect(null);

        drawToolbarButton(canvas, editShrink, "−", false);
        drawToolbarButton(canvas, editGrow, "+", false);
        drawToolbarButton(canvas, editReset, "Restablecer", false);
        drawToolbarButton(canvas, editDone, "Listo", true);

        text.setColor(0xFFFFFFFF);
        text.setTextSize(getHeight() * 0.035f);
        canvas.drawText("Arrastra un control para moverlo. − / + cambian el tamaño del seleccionado.",
            (editShrink.left + editDone.right) / 2f, editShrink.top - getHeight() * 0.04f, text);
    }

    private void drawToolbarButton(Canvas canvas, RectF r, String label, boolean primary) {
        float rad = r.height() / 2f;
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(primary ? COLOR_EDIT : 0xE61C1C22);
        canvas.drawRoundRect(r, rad, rad, fill);
        stroke.setColor(primary ? COLOR_EDIT : 0xCCFFFFFF);
        stroke.setStrokeWidth(Math.max(3f, r.height() * 0.05f));
        canvas.drawRoundRect(r, rad, rad, stroke);
        text.setColor(primary ? 0xFF15151A : 0xFFFFFFFF);
        text.setTextSize(label.length() > 2 ? r.height() * 0.34f : r.height() * 0.6f);
        canvas.drawText(label, r.centerX(), r.centerY() - (text.descent() + text.ascent()) / 2f, text);
    }

    private void drawStick(Canvas canvas) {
        drawStick(canvas, stickBase, stickKnob, stickRadius, knobRadius, stickPointer != -1);
        drawStick(canvas, camBase, camKnob, camRadius, camKnobRadius, camPointer != -1);
        // Eye mark on the right stick's knob, so the two sticks read apart.
        stroke.setColor(0x99000000);
        stroke.setStrokeWidth(Math.max(2f, camKnobRadius * 0.10f));
        float r = camKnobRadius * 0.42f;
        canvas.drawOval(camKnob.x - r, camKnob.y - r * 0.62f, camKnob.x + r, camKnob.y + r * 0.62f, stroke);
        fill.setColor(0x99000000);
        canvas.drawCircle(camKnob.x, camKnob.y, r * 0.30f, fill);
    }

    private void drawStick(Canvas canvas, PointF stickBase, PointF stickKnob, float stickRadius,
                           float knobRadius, boolean active) {
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(0x401C1C22);
        canvas.drawCircle(stickBase.x, stickBase.y, stickRadius, fill);
        stroke.setColor(active ? 0xCCFFFFFF : 0x77FFFFFF);
        stroke.setStrokeWidth(Math.max(3f, stickRadius * 0.035f));
        canvas.drawCircle(stickBase.x, stickBase.y, stickRadius, stroke);
        // Dead-zone ring, so the neutral spot is visible.
        stroke.setColor(0x33FFFFFF);
        stroke.setStrokeWidth(Math.max(2f, stickRadius * 0.02f));
        canvas.drawCircle(stickBase.x, stickBase.y, stickRadius * DEAD_ZONE + knobRadius * 0.2f, stroke);
        fill.setColor(active ? 0xE6F2F2F7 : 0xB3D8D8E0);
        canvas.drawCircle(stickKnob.x, stickKnob.y, knobRadius, fill);
        stroke.setColor(0x55000000);
        stroke.setStrokeWidth(Math.max(2f, knobRadius * 0.06f));
        canvas.drawCircle(stickKnob.x, stickKnob.y, knobRadius, stroke);
    }

    private void drawControl(Canvas canvas, Control c) {
        RectF r = c.bounds;
        if (r.isEmpty()) return;
        float ring = Math.max(3f, Math.min(r.width(), r.height()) * 0.065f);
        boolean face = c.glyph >= GLYPH_CROSS && c.glyph <= GLYPH_TRIANGLE;

        // Body: dark translucent, or filled with the accent while held.
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(c.pressed ? (face ? c.accent : 0xFFF2F2F7) : 0xFF1C1C22);
        fill.setAlpha(c.pressed ? 225 : 150);
        stroke.setStrokeWidth(ring);
        stroke.setColor(face ? c.accent : 0xFFFFFFFF);
        stroke.setAlpha(face ? 235 : 150);
        RectF body = new RectF(r);
        body.inset(ring / 2f, ring / 2f);
        switch (c.shape) {
            case SHAPE_CIRCLE:
                canvas.drawCircle(body.centerX(), body.centerY(), body.width() / 2f, fill);
                canvas.drawCircle(body.centerX(), body.centerY(), body.width() / 2f, stroke);
                break;
            case SHAPE_PILL: {
                float rad = body.height() / 2f;
                canvas.drawRoundRect(body, rad, rad, fill);
                canvas.drawRoundRect(body, rad, rad, stroke);
                break;
            }
            case SHAPE_SHOULDER: {
                float rad = body.height() * 0.45f;
                canvas.drawRoundRect(body, rad, rad, fill);
                canvas.drawRoundRect(body, rad, rad, stroke);
                break;
            }
        }

        int glyphColor = c.pressed ? 0xFF15151A : (face ? c.accent : 0xF0FFFFFF);
        float cx = r.centerX(), cy = r.centerY();
        float size = Math.min(r.width(), r.height());
        switch (c.glyph) {
            case GLYPH_CROSS:
            case GLYPH_CIRCLE:
            case GLYPH_SQUARE:
            case GLYPH_TRIANGLE:
                drawPsSymbol(canvas, c.glyph, cx, cy, size * 0.40f, glyphColor, ring);
                break;
            case GLYPH_ARROW:
                drawArrow(canvas, c.arrowDirection, cx, cy, size * 0.36f, glyphColor);
                break;
            case GLYPH_GEAR:
                drawGear(canvas, cx, cy, size * 0.30f, glyphColor, ring);
                break;
            case GLYPH_DISK:
                drawDisk(canvas, cx, cy, size * 0.25f, glyphColor, ring);
                break;
            default:
                text.setColor(glyphColor);
                text.setTextSize(c.shape == SHAPE_SHOULDER ? r.height() * 0.52f : r.height() * 0.42f);
                canvas.drawText(c.label, cx, cy - (text.descent() + text.ascent()) / 2f, text);
                break;
        }
    }

    /** The PlayStation symbols, drawn as shapes so no font has to have them. */
    private void drawPsSymbol(Canvas canvas, int glyph, float cx, float cy, float size,
                              int color, float ring) {
        stroke.setColor(color);
        stroke.setAlpha(255);
        stroke.setStrokeWidth(Math.max(4f, ring * 1.15f));
        stroke.setStrokeCap(Paint.Cap.ROUND);
        stroke.setStrokeJoin(Paint.Join.ROUND);
        float h = size / 2f;
        switch (glyph) {
            case GLYPH_CROSS:
                canvas.drawLine(cx - h, cy - h, cx + h, cy + h, stroke);
                canvas.drawLine(cx + h, cy - h, cx - h, cy + h, stroke);
                break;
            case GLYPH_CIRCLE:
                canvas.drawCircle(cx, cy, h * 1.05f, stroke);
                break;
            case GLYPH_SQUARE:
                canvas.drawRect(cx - h * 0.95f, cy - h * 0.95f, cx + h * 0.95f, cy + h * 0.95f, stroke);
                break;
            case GLYPH_TRIANGLE:
                path.reset();
                path.moveTo(cx, cy - h * 1.10f);
                path.lineTo(cx + h * 1.10f, cy + h * 0.80f);
                path.lineTo(cx - h * 1.10f, cy + h * 0.80f);
                path.close();
                canvas.drawPath(path, stroke);
                break;
        }
        stroke.setStrokeCap(Paint.Cap.BUTT);
        stroke.setStrokeJoin(Paint.Join.MITER);
    }

    private void drawArrow(Canvas canvas, int direction, float cx, float cy, float size, int color) {
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(color);
        float h = size / 2f;
        path.reset();
        path.moveTo(0f, -h);
        path.lineTo(h, h * 0.6f);
        path.lineTo(-h, h * 0.6f);
        path.close();
        canvas.save();
        canvas.translate(cx, cy);
        canvas.rotate(direction * 90f);
        canvas.drawPath(path, fill);
        canvas.restore();
    }

    /** A floppy disk: the universal "save" sign. */
    private void drawDisk(Canvas canvas, float cx, float cy, float h, int color, float ring) {
        stroke.setColor(color);
        stroke.setStrokeWidth(Math.max(3f, ring));
        stroke.setStrokeJoin(Paint.Join.ROUND);
        path.reset();
        path.moveTo(cx - h, cy - h);
        path.lineTo(cx + h * 0.6f, cy - h);
        path.lineTo(cx + h, cy - h * 0.6f);
        path.lineTo(cx + h, cy + h);
        path.lineTo(cx - h, cy + h);
        path.close();
        canvas.drawPath(path, stroke);
        // Shutter on top, label below.
        canvas.drawRect(cx - h * 0.45f, cy - h, cx + h * 0.35f, cy - h * 0.45f, stroke);
        canvas.drawRect(cx - h * 0.6f, cy + h * 0.15f, cx + h * 0.6f, cy + h, stroke);
        stroke.setStrokeJoin(Paint.Join.MITER);
    }

    private void drawGear(Canvas canvas, float cx, float cy, float radius, int color, float ring) {
        stroke.setColor(color);
        stroke.setStrokeWidth(Math.max(3f, ring));
        canvas.drawCircle(cx, cy, radius * 0.55f, stroke);
        stroke.setStrokeCap(Paint.Cap.ROUND);
        for (int i = 0; i < 8; ++i) {
            double a = Math.PI * i / 4.0;
            float c = (float) Math.cos(a), s = (float) Math.sin(a);
            canvas.drawLine(cx + c * radius * 0.78f, cy + s * radius * 0.78f,
                            cx + c * radius * 1.12f, cy + s * radius * 1.12f, stroke);
        }
        stroke.setStrokeCap(Paint.Cap.BUTT);
    }
}

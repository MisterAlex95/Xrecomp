package fr.adserver.xrecomp;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;

/** The second-screen panels' icons, drawn in code (the launcher's own set, Widgets.drawIcon, for the shared ones). */
final class PanelIcons {
    private PanelIcons() {}

    enum Icon { HOME, POWER, CAMERA, BARS, SHARE, PLAY, SETTINGS, DISC, STOP, MENU, SCREEN }

    /** Draws icon in a box of side s centred on (cx, cy) with the stroke paint p (its colour and width kept). */
    static void draw(Canvas c, Icon icon, float cx, float cy, float s, Paint p) {
        float h = s / 2;
        Paint f = new Paint(p);
        f.setStyle(Paint.Style.FILL);
        Path path = new Path();
        switch (icon) {
        case PLAY:
            Widgets.drawIcon(c, Widgets.Icon.PLAY, cx, cy, s, p);
            break;
        case SETTINGS:
            Widgets.drawIcon(c, Widgets.Icon.SLIDERS, cx, cy, s, p);
            break;
        case DISC:
            Widgets.drawIcon(c, Widgets.Icon.DISC, cx, cy, s, p);
            break;
        case HOME:   // four tiles: the launcher
            for (int i = 0; i < 4; i++) {
                float x = cx + (i % 2 == 0 ? -h * 0.78f : h * 0.12f), y = cy + (i < 2 ? -h * 0.78f : h * 0.12f);
                c.drawRoundRect(x, y, x + h * 0.66f, y + h * 0.66f, h * 0.14f, h * 0.14f, p);
            }
            break;
        case POWER:
            c.drawArc(cx - h * 0.72f, cy - h * 0.62f, cx + h * 0.72f, cy + h * 0.82f, -55, 290, false, p);
            c.drawLine(cx, cy - h * 0.9f, cx, cy - h * 0.05f, p);
            break;
        case CAMERA:
            c.drawRoundRect(cx - h * 0.85f, cy - h * 0.45f, cx + h * 0.85f, cy + h * 0.7f, h * 0.16f, h * 0.16f, p);
            path.moveTo(cx - h * 0.35f, cy - h * 0.45f);
            path.lineTo(cx - h * 0.22f, cy - h * 0.72f);
            path.lineTo(cx + h * 0.22f, cy - h * 0.72f);
            path.lineTo(cx + h * 0.35f, cy - h * 0.45f);
            c.drawPath(path, p);
            c.drawCircle(cx, cy + h * 0.12f, h * 0.3f, p);
            break;
        case BARS:
            c.drawLine(cx - h * 0.6f, cy + h * 0.75f, cx - h * 0.6f, cy + h * 0.1f, p);
            c.drawLine(cx, cy + h * 0.75f, cx, cy - h * 0.7f, p);
            c.drawLine(cx + h * 0.6f, cy + h * 0.75f, cx + h * 0.6f, cy - h * 0.25f, p);
            break;
        case SHARE:
            c.drawCircle(cx + h * 0.55f, cy - h * 0.6f, h * 0.22f, f);
            c.drawCircle(cx - h * 0.55f, cy, h * 0.22f, f);
            c.drawCircle(cx + h * 0.55f, cy + h * 0.6f, h * 0.22f, f);
            c.drawLine(cx - h * 0.55f, cy, cx + h * 0.55f, cy - h * 0.6f, p);
            c.drawLine(cx - h * 0.55f, cy, cx + h * 0.55f, cy + h * 0.6f, p);
            break;
        case MENU:   // three lines: the in-game menu
            for (int i = -1; i <= 1; i++)
                c.drawLine(cx - h * 0.7f, cy + i * h * 0.5f, cx + h * 0.7f, cy + i * h * 0.5f, p);
            break;
        case SCREEN:   // two stacked screens: the bottom screen
            c.drawRoundRect(cx - h * 0.8f, cy - h * 0.85f, cx + h * 0.8f, cy - h * 0.1f, h * 0.12f, h * 0.12f, p);
            c.drawRoundRect(cx - h * 0.55f, cy + h * 0.15f, cx + h * 0.55f, cy + h * 0.85f, h * 0.12f, h * 0.12f, p);
            break;
        case STOP:
            c.drawCircle(cx, cy, h * 0.82f, p);
            c.drawLine(cx - h * 0.32f, cy - h * 0.32f, cx + h * 0.32f, cy + h * 0.32f, p);
            c.drawLine(cx + h * 0.32f, cy - h * 0.32f, cx - h * 0.32f, cy + h * 0.32f, p);
            break;
        }
    }
}

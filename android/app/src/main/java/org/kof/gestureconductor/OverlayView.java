package org.kof.gestureconductor;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.AttributeSet;
import android.view.View;

import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;

public class OverlayView extends View {

    private final Paint gridPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint leftPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint rightPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint strikePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint tuttiPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private float leftX = 0.35f, leftY = 0.5f, leftEnergy = 0f, leftSpan = 0.2f;
    private boolean leftDetected = false;

    private float rightX = 0.65f, rightY = 0.5f, rightEnergy = 0f, rightSpan = 0.2f;
    private boolean rightDetected = false;

    private float fps = 0f;
    private float tuttiFlash = 0f;

    private static class Ripple {
        float x, y, radius, alpha, intensity;
    }
    private final List<Ripple> ripples = new ArrayList<>();

    public OverlayView(Context context, AttributeSet attrs) {
        super(context, attrs);

        gridPaint.setColor(Color.argb(40, 51, 255, 102));
        gridPaint.setStrokeWidth(1.5f);
        gridPaint.setStyle(Paint.Style.STROKE);

        leftPaint.setColor(Color.argb(220, 51, 204, 255)); // Cyan for Left Wash
        leftPaint.setStrokeWidth(3f);
        leftPaint.setStyle(Paint.Style.STROKE);

        rightPaint.setColor(Color.argb(220, 51, 255, 102)); // Phosphor green for Right Lead
        rightPaint.setStrokeWidth(3.5f);
        rightPaint.setStyle(Paint.Style.STROKE);

        textPaint.setColor(Color.argb(240, 51, 255, 102));
        textPaint.setTextSize(26f);
        textPaint.setFakeBoldText(true);

        strikePaint.setColor(Color.argb(255, 255, 230, 80)); // Amber flash
        strikePaint.setStyle(Paint.Style.STROKE);
        strikePaint.setStrokeWidth(4f);

        tuttiPaint.setColor(Color.argb(80, 255, 50, 50)); // Red burst
        tuttiPaint.setStyle(Paint.Style.FILL);
    }

    public void updateTelemetry(float[] telem) {
        if (telem == null || telem.length < 18) return;

        leftX = telem[0];
        leftY = telem[1];
        leftEnergy = telem[2];
        leftSpan = telem[3];
        leftDetected = (telem[4] > 0.5f);

        rightX = telem[5];
        rightY = telem[6];
        rightEnergy = telem[7];
        rightSpan = telem[8];
        rightDetected = (telem[9] > 0.5f);

        if (telem[10] > 0.5f) { // Ictus strike triggered
            Ripple r = new Ripple();
            r.x = telem[12];
            r.y = telem[13];
            r.radius = 15f;
            r.alpha = 255f;
            r.intensity = telem[11];
            synchronized (ripples) {
                ripples.add(r);
            }
        }

        if (telem[14] > 0.5f) { // Tutti strike triggered
            tuttiFlash = 1.0f;
        }

        fps = telem[17];
        postInvalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        int w = getWidth();
        int h = getHeight();
        if (w <= 0 || h <= 0) return;

        // 1. Tutti strike fullscreen flash
        if (tuttiFlash > 0.01f) {
            tuttiPaint.setAlpha((int)(tuttiFlash * 120));
            canvas.drawRect(0, 0, w, h, tuttiPaint);
            tuttiFlash *= 0.88f;
        }

        // 2. Phosphor CRT Grid
        for (int x = 0; x < w; x += 80) {
            canvas.drawLine(x, 0, x, h, gridPaint);
        }
        for (int y = 0; y < h; y += 80) {
            canvas.drawLine(0, y, w, y, gridPaint);
        }

        // Center line
        canvas.drawLine(w * 0.5f, 0, w * 0.5f, h, gridPaint);

        // 3. Left Hand Reticle (Cyan)
        if (leftDetected) {
            float lx = leftX * w;
            float ly = leftY * h;
            float radius = 30f + leftEnergy * 60f;
            canvas.drawCircle(lx, ly, radius, leftPaint);
            canvas.drawLine(lx - radius - 15f, ly, lx + radius + 15f, ly, leftPaint);
            canvas.drawLine(lx, ly - radius - 15f, lx, ly + radius + 15f, leftPaint);
            canvas.drawText(String.format("WASH [L]  elev: %.2f  nrg: %.2f", 1f - leftY, leftEnergy),
                    lx - 120f, ly - radius - 20f, textPaint);
        }

        // 4. Right Hand Reticle (Baton / Lead - Phosphor Green)
        if (rightDetected) {
            float rx = rightX * w;
            float ry = rightY * h;
            float radius = 35f + rightEnergy * 70f;
            canvas.drawCircle(rx, ry, radius, rightPaint);
            canvas.drawRect(rx - radius, ry - radius, rx + radius, ry + radius, rightPaint);
            canvas.drawLine(rx - radius - 20f, ry, rx + radius + 20f, ry, rightPaint);
            canvas.drawLine(rx, ry - radius - 20f, rx, ry + radius + 20f, rightPaint);
            canvas.drawText(String.format("LEAD [R]  elev: %.2f  pan: %.2f", 1f - rightY, rightX * 2f - 1f),
                    rx - 120f, ry - radius - 20f, textPaint);
        }

        // 5. Downbeat (Ictus) Ripples
        synchronized (ripples) {
            Iterator<Ripple> it = ripples.iterator();
            while (it.hasNext()) {
                Ripple r = it.next();
                strikePaint.setAlpha((int)r.alpha);
                canvas.drawCircle(r.x * w, r.y * h, r.radius, strikePaint);
                canvas.drawCircle(r.x * w, r.y * h, r.radius * 0.6f, strikePaint);
                r.radius += 12f;
                r.alpha *= 0.88f;
                if (r.alpha < 5f) {
                    it.remove();
                }
            }
        }

        // 6. HUD Telemetry Footer
        float textY = h - 40f;
        canvas.drawText(String.format("PIXELSENSE :: OPTICAL CONDUCER  |  %d FPS", (int)fps), 24f, textY, textPaint);
        canvas.drawText(String.format("LEAD: %.0f Hz  |  WASH: %.2f  |  SPAN: %.2f",
                55f * Math.pow(2.0, (1f - rightY) * 3.5), 1f - leftY, Math.abs(rightX - leftX)), 24f, textY - 35f, textPaint);
    }
}

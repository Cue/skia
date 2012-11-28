/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "gm.h"
#include "SkCanvas.h"
#include "SkPaint.h"
#include "SkDashPathEffect.h"

static void drawline(SkCanvas* canvas, int on, int off, const SkPaint& paint,
                     SkScalar finalX = SkIntToScalar(600)) {
    SkPaint p(paint);

    const SkScalar intervals[] = {
        SkIntToScalar(on),
        SkIntToScalar(off),
    };

    p.setPathEffect(new SkDashPathEffect(intervals, 2, 0))->unref();
    canvas->drawLine(0, 0, finalX, 0, p);
}

// earlier bug stopped us from drawing very long single-segment dashes, because
// SkPathMeasure was skipping very small delta-T values (nearlyzero). This is
// now fixes, so this giant dash should appear.
static void show_giant_dash(SkCanvas* canvas) {
    SkPaint paint;

    drawline(canvas, 1, 1, paint, SkIntToScalar(20 * 1000));
}

class DashingGM : public skiagm::GM {
public:
    DashingGM() {}

protected:
    SkString onShortName() {
        return SkString("dashing");
    }

    SkISize onISize() { return skiagm::make_isize(640, 300); }

    virtual void onDraw(SkCanvas* canvas) {
        static const struct {
            int fOnInterval;
            int fOffInterval;
        } gData[] = {
            { 1, 1 },
            { 4, 1 },
        };

        SkPaint paint;
        paint.setStyle(SkPaint::kStroke_Style);

        canvas->translate(SkIntToScalar(20), SkIntToScalar(20));
        canvas->translate(0, SK_ScalarHalf);

        for (int width = 0; width <= 2; ++width) {
            for (size_t data = 0; data < SK_ARRAY_COUNT(gData); ++data) {
                for (int aa = 0; aa <= 1; ++aa) {
                    int w = width * width * width;
                    paint.setAntiAlias(SkToBool(aa));
                    paint.setStrokeWidth(SkIntToScalar(w));

                    int scale = w ? w : 1;

                    drawline(canvas, gData[data].fOnInterval * scale,
                             gData[data].fOffInterval * scale,
                             paint);
                    canvas->translate(0, SkIntToScalar(20));
                }
            }
        }

        show_giant_dash(canvas);
    }
};

///////////////////////////////////////////////////////////////////////////////

static void make_unit_star(SkPath* path, int n) {
    SkScalar rad = -SK_ScalarPI / 2;
    const SkScalar drad = (n >> 1) * SK_ScalarPI * 2 / n;

    path->moveTo(0, -SK_Scalar1);
    for (int i = 1; i < n; i++) {
        rad += drad;
        SkScalar cosV, sinV = SkScalarSinCos(rad, &cosV);
        path->lineTo(cosV, sinV);
    }
    path->close();
}

static void make_path_line(SkPath* path, const SkRect& bounds) {
    path->moveTo(bounds.left(), bounds.top());
    path->lineTo(bounds.right(), bounds.bottom());
}

static void make_path_rect(SkPath* path, const SkRect& bounds) {
    path->addRect(bounds);
}

static void make_path_oval(SkPath* path, const SkRect& bounds) {
    path->addOval(bounds);
}

static void make_path_star(SkPath* path, const SkRect& bounds) {
    make_unit_star(path, 5);
    SkMatrix matrix;
    matrix.setRectToRect(path->getBounds(), bounds, SkMatrix::kCenter_ScaleToFit);
    path->transform(matrix);
}

class Dashing2GM : public skiagm::GM {
public:
    Dashing2GM() {}

protected:
    SkString onShortName() {
        return SkString("dashing2");
    }

    SkISize onISize() { return skiagm::make_isize(640, 480); }

    virtual void onDraw(SkCanvas* canvas) {
        static const int gIntervals[] = {
            3,  // 3 dashes: each count [0] followed by intervals [1..count]
            2,  10, 10,
            4,  20, 5, 5, 5,
            2,  2, 2
        };

        void (*gProc[])(SkPath*, const SkRect&) = {
            make_path_line, make_path_rect, make_path_oval, make_path_star,
        };

        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(SkIntToScalar(6));

        SkRect bounds = SkRect::MakeWH(SkIntToScalar(120), SkIntToScalar(120));
        bounds.offset(SkIntToScalar(20), SkIntToScalar(20));
        SkScalar dx = bounds.width() * 4 / 3;
        SkScalar dy = bounds.height() * 4 / 3;

        const int* intervals = &gIntervals[1];
        for (int y = 0; y < gIntervals[0]; ++y) {
            SkScalar vals[SK_ARRAY_COUNT(gIntervals)];  // more than enough
            int count = *intervals++;
            for (int i = 0; i < count; ++i) {
                vals[i] = SkIntToScalar(*intervals++);
            }
            SkScalar phase = vals[0] / 2;
            paint.setPathEffect(new SkDashPathEffect(vals, count, phase))->unref();

            for (size_t x = 0; x < SK_ARRAY_COUNT(gProc); ++x) {
                SkPath path;
                SkRect r = bounds;
                r.offset(x * dx, y * dy);
                gProc[x](&path, r);

                canvas->drawPath(path, paint);
            }
        }
    }
};

//////////////////////////////////////////////////////////////////////////////

// Test out the on/off line dashing Chrome if fond of
class Dashing3GM : public skiagm::GM {
public:
    Dashing3GM() {}

protected:
    SkString onShortName() {
        return SkString("dashing3");
    }

    SkISize onISize() { return skiagm::make_isize(640, 480); }

    // Draw a 100x100 block of dashed lines. The horizontal ones are BW
    // while the vertical ones are AA.
    void drawDashedLines(SkCanvas* canvas, SkScalar length, SkScalar phase) {
        SkPaint p;
        p.setColor(SK_ColorBLACK);
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(SK_Scalar1);

        SkScalar intervals[2] = { SK_Scalar1, SK_Scalar1 };

        p.setPathEffect(new SkDashPathEffect(intervals, 2, phase, false));

        SkPoint pts[2];

        for (int y = 0; y < 100; y += 5) {
            pts[0].set(0, SkIntToScalar(y));
            pts[1].set(length, SkIntToScalar(y));

            canvas->drawPoints(SkCanvas::kLines_PointMode, 2, pts, p);
        }

        p.setAntiAlias(true);

        for (int x = 0; x < 100; x += 7) {
            pts[0].set(SkIntToScalar(x), 0);
            pts[1].set(SkIntToScalar(x), length);

            canvas->drawPoints(SkCanvas::kLines_PointMode, 2, pts, p);
        }
    }

    virtual void onDraw(SkCanvas* canvas) {
        // fast path should work on this run
        canvas->save();
            this->drawDashedLines(canvas, 100, SK_Scalar1);
        canvas->restore();

        // non-1 phase should break the fast path
        canvas->save();
            canvas->translate(110, 0);
            this->drawDashedLines(canvas, 100, SK_ScalarHalf);
        canvas->restore();

        // non-integer length should break the fast path
        canvas->save();
            canvas->translate(220, 0);
            this->drawDashedLines(canvas, 99.5, SK_ScalarHalf);
        canvas->restore();

        // rotation should break the fast path
        canvas->save();
            canvas->translate(110+SK_ScalarRoot2Over2*100, 110+SK_ScalarRoot2Over2*100);
            canvas->rotate(45);
            canvas->translate(-50, -50);

            this->drawDashedLines(canvas, 100, SK_Scalar1);
        canvas->restore();

    }

};

//////////////////////////////////////////////////////////////////////////////

static skiagm::GM* F0(void*) { return new DashingGM; }
static skiagm::GM* F1(void*) { return new Dashing2GM; }
static skiagm::GM* F2(void*) { return new Dashing3GM; }

static skiagm::GMRegistry gR0(F0);
static skiagm::GMRegistry gR1(F1);
static skiagm::GMRegistry gR2(F2);


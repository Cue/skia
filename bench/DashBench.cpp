
/*
 * Copyright 2011 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "SkBenchmark.h"
#include "SkBitmap.h"
#include "SkCanvas.h"
#include "SkDashPathEffect.h"
#include "SkPaint.h"
#include "SkPath.h"
#include "SkRandom.h"
#include "SkString.h"
#include "SkTDArray.h"


/*
 *  Cases to consider:
 *
 *  1. antialiasing on/off (esp. width <= 1)
 *  2. strokewidth == 0, 1, 2
 *  3. hline, vline, diagonal, rect, oval
 *  4. dots [1,1] ([N,N] where N=strokeWidth?) or arbitrary (e.g. [2,1] or [1,2,3,2])
 */
static void path_hline(SkPath* path) {
    path->moveTo(SkIntToScalar(10), SkIntToScalar(10));
    path->lineTo(SkIntToScalar(600), SkIntToScalar(10));
}

class DashBench : public SkBenchmark {
protected:
    SkString            fName;
    SkTDArray<SkScalar> fIntervals;
    int                 fWidth;
    SkPoint             fPts[2];
    bool                fDoClip;

    enum {
        N = SkBENCHLOOP(100)
    };
public:
    DashBench(void* param, const SkScalar intervals[], int count, int width,
              bool doClip = false) : INHERITED(param) {
        fIntervals.append(count, intervals);
        for (int i = 0; i < count; ++i) {
            fIntervals[i] *= width;
        }
        fWidth = width;
        fName.printf("dash_%d_%s", width, doClip ? "clipped" : "noclip");
        fDoClip = doClip;

        fPts[0].set(SkIntToScalar(10), SkIntToScalar(10));
        fPts[1].set(SkIntToScalar(600), SkIntToScalar(10));
    }

    virtual void makePath(SkPath* path) {
        path_hline(path);
    }

protected:
    virtual const char* onGetName() SK_OVERRIDE {
        return fName.c_str();
    }

    virtual void onDraw(SkCanvas* canvas) SK_OVERRIDE {
        SkPaint paint;
        this->setupPaint(&paint);
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(SkIntToScalar(fWidth));
        paint.setAntiAlias(false);

        SkPath path;
        this->makePath(&path);

        paint.setPathEffect(new SkDashPathEffect(fIntervals.begin(),
                                                 fIntervals.count(), 0))->unref();

        if (fDoClip) {
            SkRect r = path.getBounds();
            r.inset(-SkIntToScalar(20), -SkIntToScalar(20));
            // now move it so we don't intersect
            r.offset(0, r.height() * 3 / 2);
            canvas->clipRect(r);
        }

        this->handlePath(canvas, path, paint, N);
    }

    virtual void handlePath(SkCanvas* canvas, const SkPath& path,
                            const SkPaint& paint, int N) {
        for (int i = 0; i < N; ++i) {
//            canvas->drawPoints(SkCanvas::kLines_PointMode, 2, fPts, paint);
            canvas->drawPath(path, paint);
        }
    }

private:
    typedef SkBenchmark INHERITED;
};

class RectDashBench : public DashBench {
public:
    RectDashBench(void* param, const SkScalar intervals[], int count, int width, bool doClip = false)
    : INHERITED(param, intervals, count, width) {
        fName.append("_rect");
    }

protected:
    virtual void handlePath(SkCanvas* canvas, const SkPath& path,
                            const SkPaint& paint, int N) SK_OVERRIDE {
        SkPoint pts[2];
        if (!path.isLine(pts) || pts[0].fY != pts[1].fY) {
            this->INHERITED::handlePath(canvas, path, paint, N);
        } else {
            SkRect rect;
            rect.fLeft = pts[0].fX;
            rect.fTop = pts[0].fY - paint.getStrokeWidth() / 2;
            rect.fRight = rect.fLeft + SkIntToScalar(fWidth);
            rect.fBottom = rect.fTop + paint.getStrokeWidth();

            SkPaint p(paint);
            p.setStyle(SkPaint::kFill_Style);
            p.setPathEffect(NULL);

            int count = SkScalarRoundToInt((pts[1].fX - pts[0].fX) / (2*fWidth));
            SkScalar dx = SkIntToScalar(2 * fWidth);

            for (int i = 0; i < N*10; ++i) {
                SkRect r = rect;
                for (int j = 0; j < count; ++j) {
                    canvas->drawRect(r, p);
                    r.offset(dx, 0);
                }
            }
        }
    }

private:
    typedef DashBench INHERITED;
};

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

static void make_poly(SkPath* path) {
    make_unit_star(path, 9);
    SkMatrix matrix;
    matrix.setScale(SkIntToScalar(100), SkIntToScalar(100));
    path->transform(matrix);
}

static void make_quad(SkPath* path) {
    SkScalar x0 = SkIntToScalar(10);
    SkScalar y0 = SkIntToScalar(10);
    path->moveTo(x0, y0);
    path->quadTo(x0,                    y0 + 400 * SK_Scalar1,
                 x0 + 600 * SK_Scalar1, y0 + 400 * SK_Scalar1);
}

static void make_cubic(SkPath* path) {
    SkScalar x0 = SkIntToScalar(10);
    SkScalar y0 = SkIntToScalar(10);
    path->moveTo(x0, y0);
    path->cubicTo(x0,                    y0 + 400 * SK_Scalar1,
                  x0 + 600 * SK_Scalar1, y0 + 400 * SK_Scalar1,
                  x0 + 600 * SK_Scalar1, y0);
}

class MakeDashBench : public SkBenchmark {
    SkString fName;
    SkPath   fPath;
    SkAutoTUnref<SkPathEffect> fPE;

    enum {
        N = SkBENCHLOOP(400)
    };

public:
    MakeDashBench(void* param, void (*proc)(SkPath*), const char name[]) : INHERITED(param) {
        fName.printf("makedash_%s", name);
        proc(&fPath);

        SkScalar vals[] = { SkIntToScalar(4), SkIntToScalar(4) };
        fPE.reset(new SkDashPathEffect(vals, 2, 0));
    }

protected:
    virtual const char* onGetName() SK_OVERRIDE {
        return fName.c_str();
    }

    virtual void onDraw(SkCanvas* canvas) SK_OVERRIDE {
        SkPath dst;
        for (int i = 0; i < N; ++i) {
            SkStrokeRec rec(SkStrokeRec::kHairline_InitStyle);

            fPE->filterPath(&dst, fPath, &rec);
            dst.rewind();
        }
    }

private:
    typedef SkBenchmark INHERITED;
};

/*
 *  We try to special case square dashes (intervals are equal to strokewidth).
 */
class DashLineBench : public SkBenchmark {
    SkString fName;
    SkScalar fStrokeWidth;
    bool     fIsRound;
    SkAutoTUnref<SkPathEffect> fPE;

    enum {
        N = SkBENCHLOOP(200)
    };

public:
    DashLineBench(void* param, SkScalar width, bool isRound) : INHERITED(param) {
        fName.printf("dashline_%g_%s", SkScalarToFloat(width), isRound ? "circle" : "square");
        fStrokeWidth = width;
        fIsRound = isRound;

        SkScalar vals[] = { SK_Scalar1, SK_Scalar1 };
        fPE.reset(new SkDashPathEffect(vals, 2, 0));
    }

protected:
    virtual const char* onGetName() SK_OVERRIDE {
        return fName.c_str();
    }

    virtual void onDraw(SkCanvas* canvas) SK_OVERRIDE {
        SkPaint paint;
        this->setupPaint(&paint);
        paint.setStrokeWidth(fStrokeWidth);
        paint.setStrokeCap(fIsRound ? SkPaint::kRound_Cap : SkPaint::kSquare_Cap);
        paint.setPathEffect(fPE);
        for (int i = 0; i < N; ++i) {
            canvas->drawLine(10 * SK_Scalar1, 10 * SK_Scalar1,
                             640 * SK_Scalar1, 10 * SK_Scalar1, paint);
        }
    }

private:
    typedef SkBenchmark INHERITED;
};

class DrawPointsDashingBench : public SkBenchmark {
    SkString fName;
    int      fStrokeWidth;
    bool     fdoAA;

    SkAutoTUnref<SkPathEffect> fPathEffect;

    enum {
        N = SkBENCHLOOP(480)
    };

public:
    DrawPointsDashingBench(void* param, int dashLength, int strokeWidth, bool doAA)
        : INHERITED(param) {
        fName.printf("drawpointsdash_%d_%d%s", dashLength, strokeWidth, doAA ? "_aa" : "_bw");
        fStrokeWidth = strokeWidth;
        fdoAA = doAA;

        SkScalar vals[] = { SkIntToScalar(dashLength), SkIntToScalar(dashLength) };
        fPathEffect.reset(new SkDashPathEffect(vals, 2, SK_Scalar1, false));
    }

protected:
    virtual const char* onGetName() SK_OVERRIDE {
        return fName.c_str();
    }

    virtual void onDraw(SkCanvas* canvas) SK_OVERRIDE {

        SkPaint p;
        this->setupPaint(&p);
        p.setColor(SK_ColorBLACK);
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(SkIntToScalar(fStrokeWidth));
        p.setPathEffect(fPathEffect);
        p.setAntiAlias(fdoAA);

        SkPoint pts[2] = {
            { SkIntToScalar(10), 0 },
            { SkIntToScalar(640), 0 }
        };

        for (int i = 0; i < N; ++i) {

            pts[0].fY = pts[1].fY = SkIntToScalar(i % 480);
            canvas->drawPoints(SkCanvas::kLines_PointMode, 2, pts, p);
        }
    }

private:
    typedef SkBenchmark INHERITED;
};
///////////////////////////////////////////////////////////////////////////////

static const SkScalar gDots[] = { SK_Scalar1, SK_Scalar1 };

#define PARAM(array)    array, SK_ARRAY_COUNT(array)

static SkBenchmark* gF0(void* p) { return new DashBench(p, PARAM(gDots), 0); }
static SkBenchmark* gF1(void* p) { return new DashBench(p, PARAM(gDots), 1); }
static SkBenchmark* gF2(void* p) { return new DashBench(p, PARAM(gDots), 1, true); }
static SkBenchmark* gF3(void* p) { return new DashBench(p, PARAM(gDots), 4); }
static SkBenchmark* gF4(void* p) { return new MakeDashBench(p, make_poly, "poly"); }
static SkBenchmark* gF5(void* p) { return new MakeDashBench(p, make_quad, "quad"); }
static SkBenchmark* gF6(void* p) { return new MakeDashBench(p, make_cubic, "cubic"); }
static SkBenchmark* gF700(void* p) { return new DashLineBench(p, 0, false); }
static SkBenchmark* gF710(void* p) { return new DashLineBench(p, SK_Scalar1, false); }
static SkBenchmark* gF720(void* p) { return new DashLineBench(p, 2 * SK_Scalar1, false); }
static SkBenchmark* gF701(void* p) { return new DashLineBench(p, 0, true); }
static SkBenchmark* gF711(void* p) { return new DashLineBench(p, SK_Scalar1, true); }
static SkBenchmark* gF721(void* p) { return new DashLineBench(p, 2 * SK_Scalar1, true); }

static SkBenchmark* gF8(void* p) { return new DrawPointsDashingBench(p, 1, 1, false); }
static SkBenchmark* gF9(void* p) { return new DrawPointsDashingBench(p, 1, 1, true); }
static SkBenchmark* gF10(void* p) { return new DrawPointsDashingBench(p, 3, 1, false); }
static SkBenchmark* gF11(void* p) { return new DrawPointsDashingBench(p, 3, 1, true); }
static SkBenchmark* gF12(void* p) { return new DrawPointsDashingBench(p, 5, 5, false); }
static SkBenchmark* gF13(void* p) { return new DrawPointsDashingBench(p, 5, 5, true); }

static BenchRegistry gR0(gF0);
static BenchRegistry gR1(gF1);
static BenchRegistry gR2(gF2);
static BenchRegistry gR3(gF3);
static BenchRegistry gR4(gF4);
static BenchRegistry gR5(gF5);
static BenchRegistry gR6(gF6);
static BenchRegistry gR700(gF700);
static BenchRegistry gR710(gF710);
static BenchRegistry gR720(gF720);
static BenchRegistry gR701(gF701);
static BenchRegistry gR711(gF711);
static BenchRegistry gR721(gF721);

static BenchRegistry gR8(gF8);
static BenchRegistry gR9(gF9);
static BenchRegistry gR10(gF10);
static BenchRegistry gR11(gF11);
static BenchRegistry gR12(gF12);
static BenchRegistry gR13(gF13);

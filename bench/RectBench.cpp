
/*
 * Copyright 2011 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "SkBenchmark.h"
#include "SkCanvas.h"
#include "SkPaint.h"
#include "SkRandom.h"
#include "SkString.h"
#include "SkShader.h"

class RectBench : public SkBenchmark {
public:
    int fShift, fStroke;
    enum {
        W = 640,
        H = 480,
        N = SkBENCHLOOP(300)
    };
    SkRect  fRects[N];
    SkColor fColors[N];

    RectBench(void* param, int shift, int stroke = 0) : INHERITED(param), fShift(shift), fStroke(stroke) {
        SkRandom rand;
        const SkScalar offset = SK_Scalar1/3;
        for (int i = 0; i < N; i++) {
            int x = rand.nextU() % W;
            int y = rand.nextU() % H;
            int w = rand.nextU() % W;
            int h = rand.nextU() % H;
            w >>= shift;
            h >>= shift;
            x -= w/2;
            y -= h/2;
            fRects[i].set(SkIntToScalar(x), SkIntToScalar(y),
                          SkIntToScalar(x+w), SkIntToScalar(y+h));
            fRects[i].offset(offset, offset);
            fColors[i] = rand.nextU() | 0xFF808080;
        }
    }

    SkString fName;
    const char* computeName(const char root[]) {
        fName.printf("%s_%d", root, fShift);
        if (fStroke > 0) {
            fName.appendf("_stroke_%d", fStroke);
        }
        return fName.c_str();
    }

protected:
    virtual void drawThisRect(SkCanvas* c, const SkRect& r, const SkPaint& p) {
        c->drawRect(r, p);
    }

    virtual const char* onGetName() { return computeName("rects"); }
    virtual void onDraw(SkCanvas* canvas) {
        SkPaint paint;
        if (fStroke > 0) {
            paint.setStyle(SkPaint::kStroke_Style);
            paint.setStrokeWidth(SkIntToScalar(fStroke));
        }
        for (int i = 0; i < N; i++) {
            paint.setColor(fColors[i]);
            this->setupPaint(&paint);
            this->drawThisRect(canvas, fRects[i], paint);
        }
    }
private:
    typedef SkBenchmark INHERITED;
};

class SrcModeRectBench : public RectBench {
public:
    SrcModeRectBench(void* param) : INHERITED(param, 1, 0) {
        fMode = SkXfermode::Create(SkXfermode::kSrc_Mode);
    }
    
    virtual ~SrcModeRectBench() {
        SkSafeUnref(fMode);
    }

protected:
    virtual void setupPaint(SkPaint* paint) SK_OVERRIDE {
        this->INHERITED::setupPaint(paint);
        // srcmode is most interesting when we're not opaque
        paint->setAlpha(0x80);
        paint->setXfermode(fMode);
    }

    virtual const char* onGetName() SK_OVERRIDE {
        fName.set(this->INHERITED::onGetName());
        fName.prepend("srcmode_");
        return fName.c_str();
    }

private:
    SkString fName;
    SkXfermode* fMode;

    typedef RectBench INHERITED;
};

class OvalBench : public RectBench {
public:
    OvalBench(void* param, int shift) : RectBench(param, shift) {}
protected:
    virtual void drawThisRect(SkCanvas* c, const SkRect& r, const SkPaint& p) {
        c->drawOval(r, p);
    }
    virtual const char* onGetName() { return computeName("ovals"); }
};

class RRectBench : public RectBench {
public:
    RRectBench(void* param, int shift) : RectBench(param, shift) {}
protected:
    virtual void drawThisRect(SkCanvas* c, const SkRect& r, const SkPaint& p) {
        c->drawRoundRect(r, r.width() / 4, r.height() / 4, p);
    }
    virtual const char* onGetName() { return computeName("rrects"); }
};

class PointsBench : public RectBench {
public:
    SkCanvas::PointMode fMode;
    const char* fName;

    PointsBench(void* param, SkCanvas::PointMode mode, const char* name) :
        RectBench(param, 2), fMode(mode) {
        fName = name;
    }

protected:
    virtual void onDraw(SkCanvas* canvas) {
        SkScalar gSizes[] = {
            SkIntToScalar(7), 0
        };
        size_t sizes = SK_ARRAY_COUNT(gSizes);

        if (this->hasStrokeWidth()) {
            gSizes[0] = this->getStrokeWidth();
            sizes = 1;
        }

        SkPaint paint;
        paint.setStrokeCap(SkPaint::kRound_Cap);

        for (size_t i = 0; i < sizes; i++) {
            paint.setStrokeWidth(gSizes[i]);
            this->setupPaint(&paint);
            canvas->drawPoints(fMode, N * 2, SkTCast<SkPoint*>(fRects), paint);
            paint.setColor(fColors[i]);
        }
    }
    virtual const char* onGetName() { return fName; }
};

/*******************************************************************************
 * to bench BlitMask [Opaque, Black, color, shader]
 *******************************************************************************/

class BlitMaskBench : public RectBench {
public:
    enum kMaskType {
        kMaskOpaque = 0,
        kMaskBlack,
        kMaskColor,
        KMaskShader
    };
    SkCanvas::PointMode fMode;
    const char* fName;

    BlitMaskBench(void* param, SkCanvas::PointMode mode,
                  BlitMaskBench::kMaskType type, const char* name) :
                  RectBench(param, 2), fMode(mode), _type(type) {
        fName = name;
    }

protected:
    virtual void onDraw(SkCanvas* canvas) {
        SkScalar gSizes[] = {
            SkIntToScalar(13), SkIntToScalar(24)
        };
        size_t sizes = SK_ARRAY_COUNT(gSizes);

        if (this->hasStrokeWidth()) {
            gSizes[0] = this->getStrokeWidth();
            sizes = 1;
        }
        SkRandom rand;
        SkColor color = 0xFF000000;
        U8CPU alpha = 0xFF;
        SkPaint paint;
        paint.setStrokeCap(SkPaint::kRound_Cap);
        if (_type == KMaskShader) {
            SkBitmap srcBM;
            srcBM.setConfig(SkBitmap::kARGB_8888_Config, 10, 1);
            srcBM.allocPixels();
            srcBM.eraseColor(0xFF00FF00);

            SkShader* s;
            s  = SkShader::CreateBitmapShader(srcBM, SkShader::kClamp_TileMode,
                                              SkShader::kClamp_TileMode);
            paint.setShader(s)->unref();
        }
        for (size_t i = 0; i < sizes; i++) {
            switch (_type) {
                case kMaskOpaque:
                    color = fColors[i];
                    alpha = 0xFF;
                    break;
                case kMaskBlack:
                    alpha = 0xFF;
                    color = 0xFF000000;
                    break;
                case kMaskColor:
                    color = fColors[i];
                    alpha = rand.nextU() & 255;
                    break;
                case KMaskShader:
                    break;
            }
            paint.setStrokeWidth(gSizes[i]);
            this->setupPaint(&paint);
            paint.setColor(color);
            paint.setAlpha(alpha);
            canvas->drawPoints(fMode, N * 2, SkTCast<SkPoint*>(fRects), paint);
       }
    }
    virtual const char* onGetName() { return fName; }
private:
    typedef RectBench INHERITED;
    kMaskType _type;
};


DEF_BENCH( return SkNEW_ARGS(RectBench, (p, 1)); )
DEF_BENCH( return SkNEW_ARGS(RectBench, (p, 1, 4)); )
DEF_BENCH( return SkNEW_ARGS(RectBench, (p, 3)); )
DEF_BENCH( return SkNEW_ARGS(RectBench, (p, 3, 4)); )
DEF_BENCH( return SkNEW_ARGS(OvalBench, (p, 1)); )
DEF_BENCH( return SkNEW_ARGS(OvalBench, (p, 3)); )
DEF_BENCH( return SkNEW_ARGS(RRectBench, (p, 1)); )
DEF_BENCH( return SkNEW_ARGS(RRectBench, (p, 3)); )
DEF_BENCH( return SkNEW_ARGS(PointsBench, (p, SkCanvas::kPoints_PointMode, "points")); )
DEF_BENCH( return SkNEW_ARGS(PointsBench, (p, SkCanvas::kLines_PointMode, "lines")); )
DEF_BENCH( return SkNEW_ARGS(PointsBench, (p, SkCanvas::kPolygon_PointMode, "polygon")); )

DEF_BENCH( return SkNEW_ARGS(SrcModeRectBench, (p)); )

/* init the blitmask bench
 */
DEF_BENCH( return SkNEW_ARGS(BlitMaskBench,
                      (p, SkCanvas::kPoints_PointMode,
                      BlitMaskBench::kMaskOpaque, "maskopaque")
                      ); )
DEF_BENCH( return SkNEW_ARGS(BlitMaskBench,
                      (p, SkCanvas::kPoints_PointMode,
                      BlitMaskBench::kMaskBlack, "maskblack")
                      ); )
DEF_BENCH( return SkNEW_ARGS(BlitMaskBench,
                      (p, SkCanvas::kPoints_PointMode,
                      BlitMaskBench::kMaskColor, "maskcolor")
                      ); )
DEF_BENCH( return SkNEW_ARGS(BlitMaskBench,
                     (p, SkCanvas::kPoints_PointMode,
                     BlitMaskBench::KMaskShader, "maskshader")
                     ); )


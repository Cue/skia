/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "gm.h"
#include "SkCanvas.h"
#include "SkGradientShader.h"

#define W   SkIntToScalar(80)
#define H   SkIntToScalar(60)

typedef void (*PaintProc)(SkPaint*);

static void identity_paintproc(SkPaint* paint) {}
static void gradient_paintproc(SkPaint* paint) {
    const SkColor colors[] = { SK_ColorGREEN, SK_ColorBLUE };
    const SkPoint pts[] = { { 0, 0 }, { W, H } };
    SkShader* s = SkGradientShader::CreateLinear(pts, colors, NULL,
                                                 SK_ARRAY_COUNT(colors),
                                                 SkShader::kClamp_TileMode);
    paint->setShader(s)->unref();
}

typedef void (*Proc)(SkCanvas*, const SkPaint&);

static void draw_hair(SkCanvas* canvas, const SkPaint& paint) {
    SkPaint p(paint);
    p.setStrokeWidth(0);
    canvas->drawLine(0, 0, W, H, p);
}

static void draw_thick(SkCanvas* canvas, const SkPaint& paint) {
    SkPaint p(paint);
    p.setStrokeWidth(H/5);
    canvas->drawLine(0, 0, W, H, p);
}

static void draw_rect(SkCanvas* canvas, const SkPaint& paint) {
    canvas->drawRect(SkRect::MakeWH(W, H), paint);
}

static void draw_oval(SkCanvas* canvas, const SkPaint& paint) {
    canvas->drawOval(SkRect::MakeWH(W, H), paint);
}

static void draw_text(SkCanvas* canvas, const SkPaint& paint) {
    SkPaint p(paint);
    p.setTextSize(H/4);
    canvas->drawText("Hamburge", 8, 0, H*2/3, p);
}

class SrcModeGM : public skiagm::GM {
    SkPath fPath;
public:
    SrcModeGM() {
        this->setBGColor(0xFFDDDDDD);
    }

protected:
    virtual SkString onShortName() {
        return SkString("srcmode");
    }

    virtual SkISize onISize() {
        return SkISize::Make(640, 760);
    }

    virtual void onDraw(SkCanvas* canvas) {
        canvas->translate(SkIntToScalar(20), SkIntToScalar(20));

        SkPaint paint;
        paint.setColor(0x80FF0000);

        const Proc procs[] = {
            draw_hair, draw_thick, draw_rect, draw_oval, draw_text
        };

        const SkXfermode::Mode modes[] = {
            SkXfermode::kSrcOver_Mode, SkXfermode::kSrc_Mode, SkXfermode::kClear_Mode
        };

        const PaintProc paintProcs[] = {
            identity_paintproc, gradient_paintproc
        };

        for (int aa = 0; aa <= 1; ++aa) {
            paint.setAntiAlias(SkToBool(aa));
            canvas->save();
            for (size_t i = 0; i < SK_ARRAY_COUNT(paintProcs); ++i) {
                paintProcs[i](&paint);
                for (size_t x = 0; x < SK_ARRAY_COUNT(modes); ++x) {
                    paint.setXfermodeMode(modes[x]);
                    canvas->save();
                    for (size_t y = 0; y < SK_ARRAY_COUNT(procs); ++y) {
                        procs[y](canvas, paint);
                        canvas->translate(0, H * 5 / 4);
                    }
                    canvas->restore();
                    canvas->translate(W * 5 / 4, 0);
                }
            }
            canvas->restore();
            canvas->translate(0, (H * 5 / 4) * SK_ARRAY_COUNT(procs));
        }
    }

private:
    typedef skiagm::GM INHERITED;
};

///////////////////////////////////////////////////////////////////////////////

DEF_GM(return new SrcModeGM;)


/*
 * Copyright 2011 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "gm.h"
#include "SkSurface.h"
#include "SkCanvas.h"
#include "SkStream.h"
#include "SkData.h"

#if SK_SUPPORT_GPU
#include "GrContext.h"

namespace skiagm {
extern GrContext* GetGr();
};
#endif

static SkData* fileToData(const char path[]) {
    SkFILEStream stream(path);
    if (!stream.isValid()) {
        return SkData::NewEmpty();
    }
    size_t size = stream.getLength();
    void* mem = sk_malloc_throw(size);
    stream.read(mem, size);
    return SkData::NewFromMalloc(mem, size);
}

static void drawJpeg(SkCanvas* canvas, const SkISize& size) {
    SkAutoDataUnref data(fileToData("/Users/mike/Downloads/skia.google.jpeg"));
    SkImage* image = SkImage::NewEncodedData(data);
    if (image) {
        SkAutoCanvasRestore acr(canvas, true);
        canvas->scale(size.width() * 1.0f / image->width(),
                      size.height() * 1.0f / image->height());
        image->draw(canvas,0, 0, NULL);
        image->unref();
    }
}

static void drawContents(SkSurface* surface, SkColor fillC) {
    SkSize size = SkSize::Make(SkIntToScalar(surface->width()),
                               SkIntToScalar(surface->height()));
    SkCanvas* canvas = surface->getCanvas();

    SkScalar stroke = size.fWidth / 10;
    SkScalar radius = (size.fWidth - stroke) / 2;

    SkPaint paint;

    paint.setAntiAlias(true);
    paint.setColor(fillC);
    canvas->drawCircle(size.fWidth/2, size.fHeight/2, radius, paint);

    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(stroke);
    paint.setColor(SK_ColorBLACK);
    canvas->drawCircle(size.fWidth/2, size.fHeight/2, radius, paint);
}

static void test_surface(SkCanvas* canvas, SkSurface* surf) {
    drawContents(surf, SK_ColorRED);
    SkImage* imgR = surf->newImageShapshot();

    if (true) {
        SkImage* imgR2 = surf->newImageShapshot();
        SkASSERT(imgR == imgR2);
        imgR2->unref();
    }

    drawContents(surf, SK_ColorGREEN);
    SkImage* imgG = surf->newImageShapshot();

    // since we've drawn after we snapped imgR, imgG will be a different obj
    SkASSERT(imgR != imgG);

    drawContents(surf, SK_ColorBLUE);

    SkPaint paint;
//    paint.setFilterBitmap(true);
//    paint.setAlpha(0x80);

    imgR->draw(canvas, 0, 0, &paint);
    imgG->draw(canvas, 0, 80, &paint);
    surf->draw(canvas, 0, 160, &paint);

    imgG->unref();
    imgR->unref();
}

class ImageGM : public skiagm::GM {
    void*   fBuffer;
    size_t  fBufferSize;
    SkSize  fSize;
    enum {
        W = 64,
        H = 64,
        RB = W * 4 + 8,
    };
public:
    ImageGM() {
        fBufferSize = RB * H;
        fBuffer = sk_malloc_throw(fBufferSize);
        fSize.set(SkIntToScalar(W), SkIntToScalar(H));
    }

    virtual ~ImageGM() {
        sk_free(fBuffer);
    }


protected:
    virtual SkString onShortName() {
        return SkString("image-surface");
    }

    virtual SkISize onISize() {
        return SkISize::Make(800, 500);
    }

    virtual void onDraw(SkCanvas* canvas) {
        drawJpeg(canvas, this->getISize());

        canvas->scale(2, 2);

        static const char* kLabel1 = "Original Img";
        static const char* kLabel2 = "Modified Img";
        static const char* kLabel3 = "Cur Surface";

        static const char* kLabel4 = "Pre-Alloc Img";
        static const char* kLabel5 = "New Alloc Img";
        static const char* kLabel6 = "SkPicture";
        static const char* kLabel7 = "GPU";

        SkPaint textPaint;

        canvas->drawText(kLabel1, strlen(kLabel1), 10,  60, textPaint);
        canvas->drawText(kLabel2, strlen(kLabel2), 10, 140, textPaint);
        canvas->drawText(kLabel3, strlen(kLabel3), 10, 220, textPaint);

        canvas->drawText(kLabel4, strlen(kLabel4),  80, 10, textPaint);
        canvas->drawText(kLabel5, strlen(kLabel5), 160, 10, textPaint);
        canvas->drawText(kLabel6, strlen(kLabel6), 250, 10, textPaint);
        canvas->drawText(kLabel7, strlen(kLabel7), 340, 10, textPaint);

        canvas->translate(80, 20);

        // since we draw into this directly, we need to start fresh
        sk_bzero(fBuffer, fBufferSize);

        SkImage::Info info;

        info.fWidth = W;
        info.fHeight = H;
        info.fColorType = SkImage::kPMColor_ColorType;
        info.fAlphaType = SkImage::kPremul_AlphaType;
        SkAutoTUnref<SkSurface> surf0(SkSurface::NewRasterDirect(info, fBuffer, RB));
        SkAutoTUnref<SkSurface> surf1(SkSurface::NewRaster(info));
        SkAutoTUnref<SkSurface> surf2(SkSurface::NewPicture(info.fWidth, info.fHeight));
#if SK_SUPPORT_GPU
        GrContext* ctx = skiagm::GetGr();

        SkAutoTUnref<SkSurface> surf3(SkSurface::NewRenderTarget(ctx, info, 0));
#endif

        test_surface(canvas, surf0);
        canvas->translate(80, 0);
        test_surface(canvas, surf1);
        canvas->translate(80, 0);
        test_surface(canvas, surf2);
#if SK_SUPPORT_GPU
        if (NULL != ctx) {
            canvas->translate(80, 0);
            test_surface(canvas, surf3);
        }
#endif
    }

    virtual uint32_t onGetFlags() const SK_OVERRIDE {
        return GM::kSkipPicture_Flag | GM::kSkipPipe_Flag;
    }

private:
    typedef skiagm::GM INHERITED;
};

//////////////////////////////////////////////////////////////////////////////

static skiagm::GM* MyFactory(void*) { return new ImageGM; }
static skiagm::GMRegistry reg(MyFactory);


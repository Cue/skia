/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "PictureRenderer.h"
#include "picture_utils.h"
#include "SamplePipeControllers.h"
#include "SkCanvas.h"
#include "SkDevice.h"
#include "SkGPipe.h"
#if SK_SUPPORT_GPU
#include "SkGpuDevice.h"
#endif
#include "SkGraphics.h"
#include "SkImageEncoder.h"
#include "SkMaskFilter.h"
#include "SkMatrix.h"
#include "SkPicture.h"
#include "SkRTree.h"
#include "SkScalar.h"
#include "SkStream.h"
#include "SkString.h"
#include "SkTemplates.h"
#include "SkTileGrid.h"
#include "SkTDArray.h"
#include "SkThreadUtils.h"
#include "SkTypes.h"
#include "SkData.h"
#include "SkPictureUtils.h"

namespace sk_tools {

enum {
    kDefaultTileWidth = 256,
    kDefaultTileHeight = 256
};

void PictureRenderer::init(SkPicture* pict) {
    SkASSERT(NULL == fPicture);
    SkASSERT(NULL == fCanvas.get());
    if (fPicture != NULL || NULL != fCanvas.get()) {
        return;
    }

    SkASSERT(pict != NULL);
    if (NULL == pict) {
        return;
    }

    fPicture = pict;
    fPicture->ref();
    fCanvas.reset(this->setupCanvas());
}

class FlagsDrawFilter : public SkDrawFilter {
public:
    FlagsDrawFilter(PictureRenderer::DrawFilterFlags* flags) :
        fFlags(flags) {}

    virtual bool filter(SkPaint* paint, Type t) {
        paint->setFlags(paint->getFlags() & ~fFlags[t] & SkPaint::kAllFlags);
        if ((PictureRenderer::kBlur_DrawFilterFlag | PictureRenderer::kLowBlur_DrawFilterFlag)
                & fFlags[t]) {
            SkMaskFilter* maskFilter = paint->getMaskFilter();
            SkMaskFilter::BlurInfo blurInfo;
            if (maskFilter && maskFilter->asABlur(&blurInfo)) {
                if (PictureRenderer::kBlur_DrawFilterFlag & fFlags[t]) {
                    paint->setMaskFilter(NULL);
                } else {
                    blurInfo.fHighQuality = false;
                    maskFilter->setAsABlur(blurInfo);
                }
            }
        }
        if (PictureRenderer::kHinting_DrawFilterFlag & fFlags[t]) {
            paint->setHinting(SkPaint::kNo_Hinting);
        } else if (PictureRenderer::kSlightHinting_DrawFilterFlag & fFlags[t]) {
            paint->setHinting(SkPaint::kSlight_Hinting);
        }
        return true;
    }

private:
    PictureRenderer::DrawFilterFlags* fFlags;
};

static SkCanvas* setUpFilter(SkCanvas* canvas, PictureRenderer::DrawFilterFlags* drawFilters) {
    if (drawFilters && !canvas->getDrawFilter()) {
        canvas->setDrawFilter(SkNEW_ARGS(FlagsDrawFilter, (drawFilters)))->unref();
        if (drawFilters[0] & PictureRenderer::kAAClip_DrawFilterFlag) {
            canvas->setAllowSoftClip(false);
        }
    }
    return canvas;
}

SkCanvas* PictureRenderer::setupCanvas() {
    return this->setupCanvas(fPicture->width(), fPicture->height());
}

SkCanvas* PictureRenderer::setupCanvas(int width, int height) {
    SkCanvas* canvas;
    switch(fDeviceType) {
        case kBitmap_DeviceType: {
            SkBitmap bitmap;
            sk_tools::setup_bitmap(&bitmap, width, height);
            canvas = SkNEW_ARGS(SkCanvas, (bitmap));
            return setUpFilter(canvas, fDrawFilters);
        }
#if SK_SUPPORT_GPU
        case kGPU_DeviceType: {
            SkAutoTUnref<SkGpuDevice> device(SkNEW_ARGS(SkGpuDevice,
                                                    (fGrContext, SkBitmap::kARGB_8888_Config,
                                                    width, height)));
            canvas = SkNEW_ARGS(SkCanvas, (device.get()));
            return setUpFilter(canvas, fDrawFilters);
        }
#endif
        default:
            SkASSERT(0);
    }

    return NULL;
}

void PictureRenderer::end() {
    this->resetState();
    SkSafeUnref(fPicture);
    fPicture = NULL;
    fCanvas.reset(NULL);
}

/** Converts fPicture to a picture that uses a BBoxHierarchy.
 *  PictureRenderer subclasses that are used to test picture playback
 *  should call this method during init.
 */
void PictureRenderer::buildBBoxHierarchy() {
    SkASSERT(NULL != fPicture);
    if (kNone_BBoxHierarchyType != fBBoxHierarchyType && NULL != fPicture) {
        SkPicture* newPicture = this->createPicture();
        SkCanvas* recorder = newPicture->beginRecording(fPicture->width(), fPicture->height(),
                                                        this->recordFlags());
        fPicture->draw(recorder);
        newPicture->endRecording();
        fPicture->unref();
        fPicture = newPicture;
    }
}

void PictureRenderer::resetState() {
#if SK_SUPPORT_GPU
    if (this->isUsingGpuDevice()) {
        SkGLContext* glContext = fGrContextFactory.getGLContext(
            GrContextFactory::kNative_GLContextType);

        SkASSERT(glContext != NULL);
        if (NULL == glContext) {
            return;
        }

        fGrContext->flush();
        SK_GL(*glContext, Finish());
    }
#endif
}

uint32_t PictureRenderer::recordFlags() {
    return kNone_BBoxHierarchyType == fBBoxHierarchyType ? 0 :
        SkPicture::kOptimizeForClippedPlayback_RecordingFlag;
}

/**
 * Write the canvas to the specified path.
 * @param canvas Must be non-null. Canvas to be written to a file.
 * @param path Path for the file to be written. Should have no extension; write() will append
 *             an appropriate one. Passed in by value so it can be modified.
 * @return bool True if the Canvas is written to a file.
 */
static bool write(SkCanvas* canvas, SkString path) {
    SkASSERT(canvas != NULL);
    if (NULL == canvas) {
        return false;
    }

    SkBitmap bitmap;
    SkISize size = canvas->getDeviceSize();
    sk_tools::setup_bitmap(&bitmap, size.width(), size.height());

    canvas->readPixels(&bitmap, 0, 0);
    sk_tools::force_all_opaque(bitmap);

    // Since path is passed in by value, it is okay to modify it.
    path.append(".png");
    return SkImageEncoder::EncodeFile(path.c_str(), bitmap, SkImageEncoder::kPNG_Type, 100);
}

/**
 * If path is non NULL, append number to it, and call write(SkCanvas*, SkString) to write the
 * provided canvas to a file. Returns true if path is NULL or if write() succeeds.
 */
static bool writeAppendNumber(SkCanvas* canvas, const SkString* path, int number) {
    if (NULL == path) {
        return true;
    }
    SkString pathWithNumber(*path);
    pathWithNumber.appendf("%i", number);
    return write(canvas, pathWithNumber);
}

///////////////////////////////////////////////////////////////////////////////////////////////

SkCanvas* RecordPictureRenderer::setupCanvas(int width, int height) {
    // defer the canvas setup until the render step
    return NULL;
}

static bool PNGEncodeBitmapToStream(SkWStream* wStream, const SkBitmap& bm) {
    return SkImageEncoder::EncodeStream(wStream, bm, SkImageEncoder::kPNG_Type, 100);
}

bool RecordPictureRenderer::render(const SkString* path) {
    SkAutoTUnref<SkPicture> replayer(this->createPicture());
    SkCanvas* recorder = replayer->beginRecording(fPicture->width(), fPicture->height(),
                                                  this->recordFlags());
    fPicture->draw(recorder);
    replayer->endRecording();
    if (path != NULL) {
        // Record the new picture as a new SKP with PNG encoded bitmaps.
        SkString skpPath(*path);
        // ".skp" was removed from 'path' before being passed in here.
        skpPath.append(".skp");
        SkFILEWStream stream(skpPath.c_str());
        replayer->serialize(&stream, &PNGEncodeBitmapToStream);
        return true;
    }
    return false;
}

SkString RecordPictureRenderer::getConfigNameInternal() {
    return SkString("record");
}

///////////////////////////////////////////////////////////////////////////////////////////////

bool PipePictureRenderer::render(const SkString* path) {
    SkASSERT(fCanvas.get() != NULL);
    SkASSERT(fPicture != NULL);
    if (NULL == fCanvas.get() || NULL == fPicture) {
        return false;
    }

    PipeController pipeController(fCanvas.get());
    SkGPipeWriter writer;
    SkCanvas* pipeCanvas = writer.startRecording(&pipeController);
    pipeCanvas->drawPicture(*fPicture);
    writer.endRecording();
    fCanvas->flush();
    if (NULL != path) {
        return write(fCanvas, *path);
    }
    return true;
}

SkString PipePictureRenderer::getConfigNameInternal() {
    return SkString("pipe");
}

///////////////////////////////////////////////////////////////////////////////////////////////

void SimplePictureRenderer::init(SkPicture* picture) {
    INHERITED::init(picture);
    this->buildBBoxHierarchy();
}

bool SimplePictureRenderer::render(const SkString* path) {
    SkASSERT(fCanvas.get() != NULL);
    SkASSERT(fPicture != NULL);
    if (NULL == fCanvas.get() || NULL == fPicture) {
        return false;
    }

    fCanvas->drawPicture(*fPicture);
    fCanvas->flush();
    if (NULL != path) {
        return write(fCanvas, *path);
    }
    return true;
}

SkString SimplePictureRenderer::getConfigNameInternal() {
    return SkString("simple");
}

///////////////////////////////////////////////////////////////////////////////////////////////

TiledPictureRenderer::TiledPictureRenderer()
    : fTileWidth(kDefaultTileWidth)
    , fTileHeight(kDefaultTileHeight)
    , fTileWidthPercentage(0.0)
    , fTileHeightPercentage(0.0)
    , fTileMinPowerOf2Width(0) { }

void TiledPictureRenderer::init(SkPicture* pict) {
    SkASSERT(pict != NULL);
    SkASSERT(0 == fTileRects.count());
    if (NULL == pict || fTileRects.count() != 0) {
        return;
    }

    // Do not call INHERITED::init(), which would create a (potentially large) canvas which is not
    // used by bench_pictures.
    fPicture = pict;
    fPicture->ref();
    this->buildBBoxHierarchy();

    if (fTileWidthPercentage > 0) {
        fTileWidth = sk_float_ceil2int(float(fTileWidthPercentage * fPicture->width() / 100));
    }
    if (fTileHeightPercentage > 0) {
        fTileHeight = sk_float_ceil2int(float(fTileHeightPercentage * fPicture->height() / 100));
    }

    if (fTileMinPowerOf2Width > 0) {
        this->setupPowerOf2Tiles();
    } else {
        this->setupTiles();
    }
}

void TiledPictureRenderer::end() {
    fTileRects.reset();
    this->INHERITED::end();
}

void TiledPictureRenderer::setupTiles() {
    for (int tile_y_start = 0; tile_y_start < fPicture->height(); tile_y_start += fTileHeight) {
        for (int tile_x_start = 0; tile_x_start < fPicture->width(); tile_x_start += fTileWidth) {
            *fTileRects.append() = SkRect::MakeXYWH(SkIntToScalar(tile_x_start),
                                                    SkIntToScalar(tile_y_start),
                                                    SkIntToScalar(fTileWidth),
                                                    SkIntToScalar(fTileHeight));
        }
    }
}

// The goal of the powers of two tiles is to minimize the amount of wasted tile
// space in the width-wise direction and then minimize the number of tiles. The
// constraints are that every tile must have a pixel width that is a power of
// two and also be of some minimal width (that is also a power of two).
//
// This is solved by first taking our picture size and rounding it up to the
// multiple of the minimal width. The binary representation of this rounded
// value gives us the tiles we need: a bit of value one means we need a tile of
// that size.
void TiledPictureRenderer::setupPowerOf2Tiles() {
    int rounded_value = fPicture->width();
    if (fPicture->width() % fTileMinPowerOf2Width != 0) {
        rounded_value = fPicture->width() - (fPicture->width() % fTileMinPowerOf2Width)
            + fTileMinPowerOf2Width;
    }

    int num_bits = SkScalarCeilToInt(SkScalarLog2(SkIntToScalar(fPicture->width())));
    int largest_possible_tile_size = 1 << num_bits;

    // The tile height is constant for a particular picture.
    for (int tile_y_start = 0; tile_y_start < fPicture->height(); tile_y_start += fTileHeight) {
        int tile_x_start = 0;
        int current_width = largest_possible_tile_size;
        // Set fTileWidth to be the width of the widest tile, so that each canvas is large enough
        // to draw each tile.
        fTileWidth = current_width;

        while (current_width >= fTileMinPowerOf2Width) {
            // It is very important this is a bitwise AND.
            if (current_width & rounded_value) {
                *fTileRects.append() = SkRect::MakeXYWH(SkIntToScalar(tile_x_start),
                                                        SkIntToScalar(tile_y_start),
                                                        SkIntToScalar(current_width),
                                                        SkIntToScalar(fTileHeight));
                tile_x_start += current_width;
            }

            current_width >>= 1;
        }
    }
}

/**
 * Draw the specified playback to the canvas translated to rectangle provided, so that this mini
 * canvas represents the rectangle's portion of the overall picture.
 * Saves and restores so that the initial clip and matrix return to their state before this function
 * is called.
 */
template<class T>
static void DrawTileToCanvas(SkCanvas* canvas, const SkRect& tileRect, T* playback) {
    int saveCount = canvas->save();
    // Translate so that we draw the correct portion of the picture
    canvas->translate(-tileRect.fLeft, -tileRect.fTop);
    playback->draw(canvas);
    canvas->restoreToCount(saveCount);
    canvas->flush();
}

///////////////////////////////////////////////////////////////////////////////////////////////

bool TiledPictureRenderer::render(const SkString* path) {
    SkASSERT(fPicture != NULL);
    if (NULL == fPicture) {
        return false;
    }

    // Reuse one canvas for all tiles.
    SkCanvas* canvas = this->setupCanvas(fTileWidth, fTileHeight);
    SkAutoUnref aur(canvas);

    bool success = true;
    for (int i = 0; i < fTileRects.count(); ++i) {
        DrawTileToCanvas(canvas, fTileRects[i], fPicture);
        if (NULL != path) {
            success &= writeAppendNumber(canvas, path, i);
        }
    }
    return success;
}

SkCanvas* TiledPictureRenderer::setupCanvas(int width, int height) {
    SkCanvas* canvas = this->INHERITED::setupCanvas(width, height);
    SkASSERT(fPicture != NULL);
    // Clip the tile to an area that is completely in what the SkPicture says is the
    // drawn-to area. This is mostly important for tiles on the right and bottom edges
    // as they may go over this area and the picture may have some commands that
    // draw outside of this area and so should not actually be written.
    SkRect clip = SkRect::MakeWH(SkIntToScalar(fPicture->width()),
                                 SkIntToScalar(fPicture->height()));
    canvas->clipRect(clip);
    return canvas;
}

SkString TiledPictureRenderer::getConfigNameInternal() {
    SkString name;
    if (fTileMinPowerOf2Width > 0) {
        name.append("pow2tile_");
        name.appendf("%i", fTileMinPowerOf2Width);
    } else {
        name.append("tile_");
        if (fTileWidthPercentage > 0) {
            name.appendf("%.f%%", fTileWidthPercentage);
        } else {
            name.appendf("%i", fTileWidth);
        }
    }
    name.append("x");
    if (fTileHeightPercentage > 0) {
        name.appendf("%.f%%", fTileHeightPercentage);
    } else {
        name.appendf("%i", fTileHeight);
    }
    return name;
}

///////////////////////////////////////////////////////////////////////////////////////////////

// Holds all of the information needed to draw a set of tiles.
class CloneData : public SkRunnable {

public:
    CloneData(SkPicture* clone, SkCanvas* canvas, SkTDArray<SkRect>& rects, int start, int end,
              SkRunnable* done)
        : fClone(clone)
        , fCanvas(canvas)
        , fPath(NULL)
        , fRects(rects)
        , fStart(start)
        , fEnd(end)
        , fSuccess(NULL)
        , fDone(done) {
        SkASSERT(fDone != NULL);
    }

    virtual void run() SK_OVERRIDE {
        SkGraphics::SetTLSFontCacheLimit(1024 * 1024);
        for (int i = fStart; i < fEnd; i++) {
            DrawTileToCanvas(fCanvas, fRects[i], fClone);
            if (fPath != NULL && !writeAppendNumber(fCanvas, fPath, i)
                && fSuccess != NULL) {
                *fSuccess = false;
                // If one tile fails to write to a file, do not continue drawing the rest.
                break;
            }
        }
        fDone->run();
    }

    void setPathAndSuccess(const SkString* path, bool* success) {
        fPath = path;
        fSuccess = success;
    }

private:
    // All pointers unowned.
    SkPicture*         fClone;      // Picture to draw from. Each CloneData has a unique one which
                                    // is threadsafe.
    SkCanvas*          fCanvas;     // Canvas to draw to. Reused for each tile.
    const SkString*    fPath;       // If non-null, path to write the result to as a PNG.
    SkTDArray<SkRect>& fRects;      // All tiles of the picture.
    const int          fStart;      // Range of tiles drawn by this thread.
    const int          fEnd;
    bool*              fSuccess;    // Only meaningful if path is non-null. Shared by all threads,
                                    // and only set to false upon failure to write to a PNG.
    SkRunnable*        fDone;
};

MultiCorePictureRenderer::MultiCorePictureRenderer(int threadCount)
: fNumThreads(threadCount)
, fThreadPool(threadCount)
, fCountdown(threadCount) {
    // Only need to create fNumThreads - 1 clones, since one thread will use the base
    // picture.
    fPictureClones = SkNEW_ARRAY(SkPicture, fNumThreads - 1);
    fCloneData = SkNEW_ARRAY(CloneData*, fNumThreads);
}

void MultiCorePictureRenderer::init(SkPicture *pict) {
    // Set fPicture and the tiles.
    this->INHERITED::init(pict);
    for (int i = 0; i < fNumThreads; ++i) {
        *fCanvasPool.append() = this->setupCanvas(this->getTileWidth(), this->getTileHeight());
    }
    // Only need to create fNumThreads - 1 clones, since one thread will use the base picture.
    fPicture->clone(fPictureClones, fNumThreads - 1);
    // Populate each thread with the appropriate data.
    // Group the tiles into nearly equal size chunks, rounding up so we're sure to cover them all.
    const int chunkSize = (fTileRects.count() + fNumThreads - 1) / fNumThreads;

    for (int i = 0; i < fNumThreads; i++) {
        SkPicture* pic;
        if (i == fNumThreads-1) {
            // The last set will use the original SkPicture.
            pic = fPicture;
        } else {
            pic = &fPictureClones[i];
        }
        const int start = i * chunkSize;
        const int end = SkMin32(start + chunkSize, fTileRects.count());
        fCloneData[i] = SkNEW_ARGS(CloneData,
                                   (pic, fCanvasPool[i], fTileRects, start, end, &fCountdown));
    }
}

bool MultiCorePictureRenderer::render(const SkString *path) {
    bool success = true;
    if (path != NULL) {
        for (int i = 0; i < fNumThreads-1; i++) {
            fCloneData[i]->setPathAndSuccess(path, &success);
        }
    }

    fCountdown.reset(fNumThreads);
    for (int i = 0; i < fNumThreads; i++) {
        fThreadPool.add(fCloneData[i]);
    }
    fCountdown.wait();

    return success;
}

void MultiCorePictureRenderer::end() {
    for (int i = 0; i < fNumThreads - 1; i++) {
        SkDELETE(fCloneData[i]);
        fCloneData[i] = NULL;
    }

    fCanvasPool.unrefAll();

    this->INHERITED::end();
}

MultiCorePictureRenderer::~MultiCorePictureRenderer() {
    // Each individual CloneData was deleted in end.
    SkDELETE_ARRAY(fCloneData);
    SkDELETE_ARRAY(fPictureClones);
}

SkString MultiCorePictureRenderer::getConfigNameInternal() {
    SkString name = this->INHERITED::getConfigNameInternal();
    name.appendf("_multi_%i_threads", fNumThreads);
    return name;
}

///////////////////////////////////////////////////////////////////////////////////////////////

void PlaybackCreationRenderer::setup() {
    fReplayer.reset(this->createPicture());
    SkCanvas* recorder = fReplayer->beginRecording(fPicture->width(), fPicture->height(),
                                                   this->recordFlags());
    fPicture->draw(recorder);
}

bool PlaybackCreationRenderer::render(const SkString*) {
    fReplayer->endRecording();
    // Since this class does not actually render, return false.
    return false;
}

SkString PlaybackCreationRenderer::getConfigNameInternal() {
    return SkString("playback_creation");
}

///////////////////////////////////////////////////////////////////////////////////////////////
// SkPicture variants for each BBoxHierarchy type

class RTreePicture : public SkPicture {
public:
    virtual SkBBoxHierarchy* createBBoxHierarchy() const SK_OVERRIDE{
        static const int kRTreeMinChildren = 6;
        static const int kRTreeMaxChildren = 11;
        SkScalar aspectRatio = SkScalarDiv(SkIntToScalar(fWidth),
                                           SkIntToScalar(fHeight));
        return SkRTree::Create(kRTreeMinChildren, kRTreeMaxChildren,
                               aspectRatio);
    }
};

class TileGridPicture : public SkPicture {
public:
    TileGridPicture(int tileWidth, int tileHeight, int xTileCount, int yTileCount) {
        fTileWidth = tileWidth;
        fTileHeight = tileHeight;
        fXTileCount = xTileCount;
        fYTileCount = yTileCount;
    }

    virtual SkBBoxHierarchy* createBBoxHierarchy() const SK_OVERRIDE{
        return SkNEW_ARGS(SkTileGrid, (fTileWidth, fTileHeight, fXTileCount, fYTileCount));
    }
private:
    int fTileWidth, fTileHeight, fXTileCount, fYTileCount;
};

SkPicture* PictureRenderer::createPicture() {
    switch (fBBoxHierarchyType) {
        case kNone_BBoxHierarchyType:
            return SkNEW(SkPicture);
        case kRTree_BBoxHierarchyType:
            return SkNEW(RTreePicture);
        case kTileGrid_BBoxHierarchyType:
            {
                int xTileCount = fPicture->width() / fGridWidth +
                    ((fPicture->width() % fGridWidth) ? 1 : 0);
                int yTileCount = fPicture->height() / fGridHeight +
                    ((fPicture->height() % fGridHeight) ? 1 : 0);
                return SkNEW_ARGS(TileGridPicture, (fGridWidth, fGridHeight, xTileCount,
                                                    yTileCount));
            }
    }
    SkASSERT(0); // invalid bbhType
    return NULL;
}

///////////////////////////////////////////////////////////////////////////////

class GatherRenderer : public PictureRenderer {
public:
    virtual bool render(const SkString* path) SK_OVERRIDE {
        SkRect bounds = SkRect::MakeWH(SkIntToScalar(fPicture->width()),
                                       SkIntToScalar(fPicture->height()));
        SkData* data = SkPictureUtils::GatherPixelRefs(fPicture, bounds);
        SkSafeUnref(data);

        return NULL == path;    // we don't have anything to write
    }

private:
    virtual SkString getConfigNameInternal() SK_OVERRIDE {
        return SkString("gather_pixelrefs");
    }
};

PictureRenderer* CreateGatherPixelRefsRenderer() {
    return SkNEW(GatherRenderer);
}

} // namespace sk_tools

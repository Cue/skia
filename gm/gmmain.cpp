/*
 * Copyright 2011 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * Code for the "gm" (Golden Master) rendering comparison tool.
 *
 * If you make changes to this, re-run the self-tests at gm/tests/run.sh
 * to make sure they still pass... you may need to change the expected
 * results of the self-test.
 */

#include "gm.h"
#include "system_preferences.h"
#include "SkColorPriv.h"
#include "SkData.h"
#include "SkDeferredCanvas.h"
#include "SkDevice.h"
#include "SkDrawFilter.h"
#include "SkGPipe.h"
#include "SkGraphics.h"
#include "SkImageDecoder.h"
#include "SkImageEncoder.h"
#include "SkOSFile.h"
#include "SkPicture.h"
#include "SkRefCnt.h"
#include "SkStream.h"
#include "SkTArray.h"
#include "SamplePipeControllers.h"

#if SK_SUPPORT_GPU
#include "GrContextFactory.h"
#include "GrRenderTarget.h"
#include "SkGpuDevice.h"
typedef GrContextFactory::GLContextType GLContextType;
#else
class GrContext;
class GrRenderTarget;
typedef int GLContextType;
#endif

static bool gForceBWtext;

extern bool gSkSuppressFontCachePurgeSpew;

#ifdef SK_SUPPORT_PDF
    #include "SkPDFDevice.h"
    #include "SkPDFDocument.h"
#endif

// Until we resolve http://code.google.com/p/skia/issues/detail?id=455 ,
// stop writing out XPS-format image baselines in gm.
#undef SK_SUPPORT_XPS
#ifdef SK_SUPPORT_XPS
    #include "SkXPSDevice.h"
#endif

#ifdef SK_BUILD_FOR_MAC
    #include "SkCGUtils.h"
    #define CAN_IMAGE_PDF   1
#else
    #define CAN_IMAGE_PDF   0
#endif

typedef int ErrorBitfield;
const static ErrorBitfield ERROR_NONE                    = 0x00;
const static ErrorBitfield ERROR_NO_GPU_CONTEXT          = 0x01;
const static ErrorBitfield ERROR_PIXEL_MISMATCH          = 0x02;
const static ErrorBitfield ERROR_DIMENSION_MISMATCH      = 0x04;
const static ErrorBitfield ERROR_READING_REFERENCE_IMAGE = 0x08;
const static ErrorBitfield ERROR_WRITING_REFERENCE_IMAGE = 0x10;

using namespace skiagm;

/*
 *  Return the max of the difference (in absolute value) for any component.
 *  Returns 0 if they are equal.
 */
static int compute_PMColor_maxDiff(SkPMColor c0, SkPMColor c1) {
    int da = SkAbs32(SkGetPackedA32(c0) - SkGetPackedA32(c1));
    int dr = SkAbs32(SkGetPackedR32(c0) - SkGetPackedR32(c1));
    int dg = SkAbs32(SkGetPackedG32(c0) - SkGetPackedG32(c1));
    int db = SkAbs32(SkGetPackedB32(c0) - SkGetPackedB32(c1));
    return SkMax32(da, SkMax32(dr, SkMax32(dg, db)));
}

struct FailRec {
    SkString    fName;
    int         fMaxPixelError;

    FailRec() : fMaxPixelError(0) {}
    FailRec(const SkString& name) : fName(name), fMaxPixelError(0) {}
};

class Iter {
public:
    Iter() {
        this->reset();
    }

    void reset() {
        fReg = GMRegistry::Head();
    }

    GM* next() {
        if (fReg) {
            GMRegistry::Factory fact = fReg->factory();
            fReg = fReg->next();
            return fact(0);
        }
        return NULL;
    }

    static int Count() {
        const GMRegistry* reg = GMRegistry::Head();
        int count = 0;
        while (reg) {
            count += 1;
            reg = reg->next();
        }
        return count;
    }

private:
    const GMRegistry* fReg;
};

enum Backend {
  kRaster_Backend,
  kGPU_Backend,
  kPDF_Backend,
  kXPS_Backend,
};

enum ConfigFlags {
    kNone_ConfigFlag  = 0x0,
    /* Write GM images if a write path is provided. */
    kWrite_ConfigFlag = 0x1,
    /* Read reference GM images if a read path is provided. */
    kRead_ConfigFlag  = 0x2,
    kRW_ConfigFlag    = (kWrite_ConfigFlag | kRead_ConfigFlag),
};

struct ConfigData {
    SkBitmap::Config                fConfig;
    Backend                         fBackend;
    GLContextType                   fGLContextType; // GPU backend only
    int                             fSampleCnt;     // GPU backend only
    ConfigFlags                     fFlags;
    const char*                     fName;
};

class BWTextDrawFilter : public SkDrawFilter {
public:
    virtual void filter(SkPaint*, Type) SK_OVERRIDE;
};
void BWTextDrawFilter::filter(SkPaint* p, Type t) {
    if (kText_Type == t) {
        p->setAntiAlias(false);
    }
}

struct PipeFlagComboData {
    const char* name;
    uint32_t flags;
};

static PipeFlagComboData gPipeWritingFlagCombos[] = {
    { "", 0 },
    { " cross-process", SkGPipeWriter::kCrossProcess_Flag },
    { " cross-process, shared address", SkGPipeWriter::kCrossProcess_Flag
        | SkGPipeWriter::kSharedAddressSpace_Flag }
};


class GMMain {
public:
    GMMain() {
        // Set default values of member variables, which tool_main()
        // may override.
        fNotifyMissingReadReference = true;
        fUseFileHierarchy = false;
    }

    SkString make_name(const char shortName[], const char configName[]) {
        SkString name;
        if (0 == strlen(configName)) {
            name.append(shortName);
        } else if (fUseFileHierarchy) {
            name.appendf("%s%c%s", configName, SkPATH_SEPARATOR, shortName);
        } else {
            name.appendf("%s_%s", shortName, configName);
        }
        return name;
    }

    static SkString make_filename(const char path[],
                                  const char pathSuffix[],
                                  const SkString& name,
                                  const char suffix[]) {
        SkString filename(path);
        if (filename.endsWith(SkPATH_SEPARATOR)) {
            filename.remove(filename.size() - 1, 1);
        }
        filename.appendf("%s%c%s.%s", pathSuffix, SkPATH_SEPARATOR,
                         name.c_str(), suffix);
        return filename;
    }

    /* since PNG insists on unpremultiplying our alpha, we take no
       precision chances and force all pixels to be 100% opaque,
       otherwise on compare we may not get a perfect match.
    */
    static void force_all_opaque(const SkBitmap& bitmap) {
        SkAutoLockPixels lock(bitmap);
        for (int y = 0; y < bitmap.height(); y++) {
            for (int x = 0; x < bitmap.width(); x++) {
                *bitmap.getAddr32(x, y) |= (SK_A32_MASK << SK_A32_SHIFT);
            }
        }
    }

    static bool write_bitmap(const SkString& path, const SkBitmap& bitmap) {
        SkBitmap copy;
        bitmap.copyTo(&copy, SkBitmap::kARGB_8888_Config);
        force_all_opaque(copy);
        return SkImageEncoder::EncodeFile(path.c_str(), copy,
                                          SkImageEncoder::kPNG_Type, 100);
    }

    static inline SkPMColor compute_diff_pmcolor(SkPMColor c0, SkPMColor c1) {
        int dr = SkGetPackedR32(c0) - SkGetPackedR32(c1);
        int dg = SkGetPackedG32(c0) - SkGetPackedG32(c1);
        int db = SkGetPackedB32(c0) - SkGetPackedB32(c1);
        return SkPackARGB32(0xFF, SkAbs32(dr), SkAbs32(dg), SkAbs32(db));
    }

    static void compute_diff(const SkBitmap& target, const SkBitmap& base,
                             SkBitmap* diff) {
        SkAutoLockPixels alp(*diff);

        const int w = target.width();
        const int h = target.height();
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                SkPMColor c0 = *base.getAddr32(x, y);
                SkPMColor c1 = *target.getAddr32(x, y);
                SkPMColor d = 0;
                if (c0 != c1) {
                    d = compute_diff_pmcolor(c0, c1);
                }
                *diff->getAddr32(x, y) = d;
            }
        }
    }

    // Records an error in fFailedTests, if we want to record errors
    // of this type.
    void RecordError(ErrorBitfield errorType, const SkString& name,
                     const char renderModeDescriptor [], int maxPixelError=0) {
        switch (errorType) {
        case ERROR_NONE:
            break;
        case ERROR_READING_REFERENCE_IMAGE:
            break;
        default:
            FailRec& rec = fFailedTests.push_back(make_name(
                name.c_str(), renderModeDescriptor));
            rec.fMaxPixelError = maxPixelError;
            break;
        }
    }

    // List contents of fFailedTests via SkDebug.
    void ListErrors() {
        for (int i = 0; i < fFailedTests.count(); ++i) {
            int pixErr = fFailedTests[i].fMaxPixelError;
            SkString pixStr;
            if (pixErr > 0) {
                pixStr.printf(" pixel_error %d", pixErr);
            }
            SkDebugf("\t\t%s%s\n", fFailedTests[i].fName.c_str(),
                     pixStr.c_str());
        }
    }

    // Compares "target" and "base" bitmaps, returning the result
    // (ERROR_NONE if the two bitmaps are identical).
    //
    // If a "diff" bitmap is passed in, pixel diffs (if any) will be written
    // into it.
    ErrorBitfield compare(const SkBitmap& target, const SkBitmap& base,
                          const SkString& name,
                          const char* renderModeDescriptor,
                          SkBitmap* diff) {
        SkBitmap copy;
        const SkBitmap* bm = &target;
        if (target.config() != SkBitmap::kARGB_8888_Config) {
            target.copyTo(&copy, SkBitmap::kARGB_8888_Config);
            bm = &copy;
        }
        SkBitmap baseCopy;
        const SkBitmap* bp = &base;
        if (base.config() != SkBitmap::kARGB_8888_Config) {
            base.copyTo(&baseCopy, SkBitmap::kARGB_8888_Config);
            bp = &baseCopy;
        }

        force_all_opaque(*bm);
        force_all_opaque(*bp);

        const int w = bm->width();
        const int h = bm->height();
        if (w != bp->width() || h != bp->height()) {
            SkDebugf(
                     "---- %s dimensions mismatch for %s base [%d %d] current [%d %d]\n",
                     renderModeDescriptor, name.c_str(),
                     bp->width(), bp->height(), w, h);
            RecordError(ERROR_DIMENSION_MISMATCH, name, renderModeDescriptor);
            return ERROR_DIMENSION_MISMATCH;
        }

        SkAutoLockPixels bmLock(*bm);
        SkAutoLockPixels baseLock(*bp);

        int maxErr = 0;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                SkPMColor c0 = *bp->getAddr32(x, y);
                SkPMColor c1 = *bm->getAddr32(x, y);
                if (c0 != c1) {
                    maxErr = SkMax32(maxErr, compute_PMColor_maxDiff(c0, c1));
                }
            }
        }

        if (maxErr > 0) {
            SkDebugf(
                     "----- %s max pixel mismatch for %s is %d\n",
                     renderModeDescriptor, name.c_str(), maxErr);
            if (diff) {
                diff->setConfig(SkBitmap::kARGB_8888_Config, w, h);
                diff->allocPixels();
                compute_diff(*bm, *bp, diff);
            }
            RecordError(ERROR_PIXEL_MISMATCH, name, renderModeDescriptor,
                        maxErr);
            return ERROR_PIXEL_MISMATCH;
        }
        return ERROR_NONE;
    }

    static bool write_document(const SkString& path,
                               const SkDynamicMemoryWStream& document) {
        SkFILEWStream stream(path.c_str());
        SkAutoDataUnref data(document.copyToData());
        return stream.writeData(data.get());
    }

    /// Returns true if processing should continue, false to skip the
    /// remainder of this config for this GM.
    //@todo thudson 22 April 2011 - could refactor this to take in
    // a factory to generate the context, always call readPixels()
    // (logically a noop for rasters, if wasted time), and thus collapse the
    // GPU special case and also let this be used for SkPicture testing.
    static void setup_bitmap(const ConfigData& gRec, SkISize& size,
                             SkBitmap* bitmap) {
        bitmap->setConfig(gRec.fConfig, size.width(), size.height());
        bitmap->allocPixels();
        bitmap->eraseColor(0);
    }

    static void installFilter(SkCanvas* canvas) {
        if (gForceBWtext) {
            canvas->setDrawFilter(new BWTextDrawFilter)->unref();
        }
    }

    static void invokeGM(GM* gm, SkCanvas* canvas, bool isPDF, bool isDeferred) {
        SkAutoCanvasRestore acr(canvas, true);

        if (!isPDF) {
            canvas->concat(gm->getInitialTransform());
        }
        installFilter(canvas);
        gm->setCanvasIsDeferred(isDeferred);
        gm->draw(canvas);
        canvas->setDrawFilter(NULL);
    }

    static ErrorBitfield generate_image(GM* gm, const ConfigData& gRec,
                                        GrContext* context,
                                        GrRenderTarget* rt,
                                        SkBitmap* bitmap,
                                        bool deferred) {
        SkISize size (gm->getISize());
        setup_bitmap(gRec, size, bitmap);

        SkAutoTUnref<SkCanvas> canvas;

        if (gRec.fBackend == kRaster_Backend) {
            SkAutoTUnref<SkDevice> device(new SkDevice(*bitmap));
            if (deferred) {
                canvas.reset(new SkDeferredCanvas(device));
            } else {
                canvas.reset(new SkCanvas(device));
            }
            invokeGM(gm, canvas, false, deferred);
            canvas->flush();
        }
#if SK_SUPPORT_GPU
        else {  // GPU
            if (NULL == context) {
                return ERROR_NO_GPU_CONTEXT;
            }
            SkAutoTUnref<SkDevice> device(new SkGpuDevice(context, rt));
            if (deferred) {
                canvas.reset(new SkDeferredCanvas(device));
            } else {
                canvas.reset(new SkCanvas(device));
            }
            invokeGM(gm, canvas, false, deferred);
            // the device is as large as the current rendertarget, so
            // we explicitly only readback the amount we expect (in
            // size) overwrite our previous allocation
            bitmap->setConfig(SkBitmap::kARGB_8888_Config, size.fWidth,
                              size.fHeight);
            canvas->readPixels(bitmap, 0, 0);
        }
#endif
        return ERROR_NONE;
    }

    static void generate_image_from_picture(GM* gm, const ConfigData& gRec,
                                            SkPicture* pict, SkBitmap* bitmap) {
        SkISize size = gm->getISize();
        setup_bitmap(gRec, size, bitmap);
        SkCanvas canvas(*bitmap);
        installFilter(&canvas);
        canvas.drawPicture(*pict);
    }

    static void generate_pdf(GM* gm, SkDynamicMemoryWStream& pdf) {
#ifdef SK_SUPPORT_PDF
        SkMatrix initialTransform = gm->getInitialTransform();
        SkISize pageSize = gm->getISize();
        SkPDFDevice* dev = NULL;
        if (initialTransform.isIdentity()) {
            dev = new SkPDFDevice(pageSize, pageSize, initialTransform);
        } else {
            SkRect content = SkRect::MakeWH(SkIntToScalar(pageSize.width()),
                                            SkIntToScalar(pageSize.height()));
            initialTransform.mapRect(&content);
            content.intersect(0, 0, SkIntToScalar(pageSize.width()),
                              SkIntToScalar(pageSize.height()));
            SkISize contentSize =
                SkISize::Make(SkScalarRoundToInt(content.width()),
                              SkScalarRoundToInt(content.height()));
            dev = new SkPDFDevice(pageSize, contentSize, initialTransform);
        }
        SkAutoUnref aur(dev);

        SkCanvas c(dev);
        invokeGM(gm, &c, true, false);

        SkPDFDocument doc;
        doc.appendPage(dev);
        doc.emitPDF(&pdf);
#endif
    }

    static void generate_xps(GM* gm, SkDynamicMemoryWStream& xps) {
#ifdef SK_SUPPORT_XPS
        SkISize size = gm->getISize();

        SkSize trimSize = SkSize::Make(SkIntToScalar(size.width()),
                                       SkIntToScalar(size.height()));
        static const SkScalar inchesPerMeter = SkScalarDiv(10000, 254);
        static const SkScalar upm = 72 * inchesPerMeter;
        SkVector unitsPerMeter = SkPoint::Make(upm, upm);
        static const SkScalar ppm = 200 * inchesPerMeter;
        SkVector pixelsPerMeter = SkPoint::Make(ppm, ppm);

        SkXPSDevice* dev = new SkXPSDevice();
        SkAutoUnref aur(dev);

        SkCanvas c(dev);
        dev->beginPortfolio(&xps);
        dev->beginSheet(unitsPerMeter, pixelsPerMeter, trimSize);
        invokeGM(gm, &c, false, false);
        dev->endSheet();
        dev->endPortfolio();

#endif
    }

    ErrorBitfield write_reference_image(
      const ConfigData& gRec, const char writePath [],
      const char renderModeDescriptor [], const SkString& name,
        SkBitmap& bitmap, SkDynamicMemoryWStream* document) {
        SkString path;
        bool success = false;
        if (gRec.fBackend == kRaster_Backend ||
            gRec.fBackend == kGPU_Backend ||
            (gRec.fBackend == kPDF_Backend && CAN_IMAGE_PDF)) {

            path = make_filename(writePath, renderModeDescriptor, name, "png");
            success = write_bitmap(path, bitmap);
        }
        if (kPDF_Backend == gRec.fBackend) {
            path = make_filename(writePath, renderModeDescriptor, name, "pdf");
            success = write_document(path, *document);
        }
        if (kXPS_Backend == gRec.fBackend) {
            path = make_filename(writePath, renderModeDescriptor, name, "xps");
            success = write_document(path, *document);
        }
        if (success) {
            return ERROR_NONE;
        } else {
            fprintf(stderr, "FAILED to write %s\n", path.c_str());
            RecordError(ERROR_WRITING_REFERENCE_IMAGE, name,
                        renderModeDescriptor);
            return ERROR_WRITING_REFERENCE_IMAGE;
        }
    }

    // Compares bitmap "bitmap" to "referenceBitmap"; if they are
    // different, writes out "bitmap" (in PNG format) within the
    // diffPath subdir.
    //
    // Returns the ErrorBitfield from compare(), describing any differences
    // between "bitmap" and "referenceBitmap" (or ERROR_NONE if there are none).
    ErrorBitfield compare_to_reference_image_in_memory(
      const SkString& name, SkBitmap &bitmap, const SkBitmap& referenceBitmap,
      const char diffPath [], const char renderModeDescriptor []) {
        ErrorBitfield errors;
        SkBitmap diffBitmap;
        errors = compare(bitmap, referenceBitmap, name, renderModeDescriptor,
                         diffPath ? &diffBitmap : NULL);
        if ((ERROR_NONE != errors) && diffPath) {
            // write out the generated image
            SkString genName = make_filename(diffPath, "", name, "png");
            if (!write_bitmap(genName, bitmap)) {
                RecordError(ERROR_WRITING_REFERENCE_IMAGE, name,
                            renderModeDescriptor);
                errors |= ERROR_WRITING_REFERENCE_IMAGE;
            }
        }
        return errors;
    }

    // Compares bitmap "bitmap" to a reference bitmap read from disk;
    // if they are different, writes out "bitmap" (in PNG format)
    // within the diffPath subdir.
    //
    // Returns a description of the difference between "bitmap" and
    // the reference bitmap, or ERROR_READING_REFERENCE_IMAGE if
    // unable to read the reference bitmap from disk.
    ErrorBitfield compare_to_reference_image_on_disk(
      const char readPath [], const SkString& name, SkBitmap &bitmap,
      const char diffPath [], const char renderModeDescriptor []) {
        SkString path = make_filename(readPath, "", name, "png");
        SkBitmap referenceBitmap;
        if (SkImageDecoder::DecodeFile(path.c_str(), &referenceBitmap,
                                       SkBitmap::kARGB_8888_Config,
                                       SkImageDecoder::kDecodePixels_Mode,
                                       NULL)) {
            return compare_to_reference_image_in_memory(name, bitmap,
                                                        referenceBitmap,
                                                        diffPath,
                                                        renderModeDescriptor);
        } else {
            if (fNotifyMissingReadReference) {
                fprintf(stderr, "FAILED to read %s\n", path.c_str());
            }
            RecordError(ERROR_READING_REFERENCE_IMAGE, name,
                        renderModeDescriptor);
            return ERROR_READING_REFERENCE_IMAGE;
        }
    }

    // NOTE: As far as I can tell, this function is NEVER called with a
    // non-blank renderModeDescriptor, EXCEPT when readPath and writePath are
    // both NULL (and thus no images are read from or written to disk).
    // So I don't trust that the renderModeDescriptor is being used for
    // anything other than debug output these days.
    ErrorBitfield handle_test_results(GM* gm,
                                      const ConfigData& gRec,
                                      const char writePath [],
                                      const char readPath [],
                                      const char diffPath [],
                                      const char renderModeDescriptor [],
                                      SkBitmap& bitmap,
                                      SkDynamicMemoryWStream* pdf,
                                      const SkBitmap* referenceBitmap) {
        SkString name = make_name(gm->shortName(), gRec.fName);
        ErrorBitfield retval = ERROR_NONE;

        if (readPath && (gRec.fFlags & kRead_ConfigFlag)) {
            retval |= compare_to_reference_image_on_disk(readPath, name, bitmap,
                                                         diffPath,
                                                         renderModeDescriptor);
        }
        if (writePath && (gRec.fFlags & kWrite_ConfigFlag)) {
            retval |= write_reference_image(gRec, writePath,
                                            renderModeDescriptor,
                                            name, bitmap, pdf);
        }
        if (referenceBitmap) {
            retval |= compare_to_reference_image_in_memory(
              name, bitmap, *referenceBitmap, diffPath, renderModeDescriptor);
        }
        return retval;
    }

    static SkPicture* generate_new_picture(GM* gm) {
        // Pictures are refcounted so must be on heap
        SkPicture* pict = new SkPicture;
        SkISize size = gm->getISize();
        SkCanvas* cv = pict->beginRecording(size.width(), size.height());
        invokeGM(gm, cv, false, false);
        pict->endRecording();

        return pict;
    }

    static SkPicture* stream_to_new_picture(const SkPicture& src) {

        // To do in-memory commiunications with a stream, we need to:
        // * create a dynamic memory stream
        // * copy it into a buffer
        // * create a read stream from it
        // ?!?!

        SkDynamicMemoryWStream storage;
        src.serialize(&storage);

        int streamSize = storage.getOffset();
        SkAutoMalloc dstStorage(streamSize);
        void* dst = dstStorage.get();
        //char* dst = new char [streamSize];
        //@todo thudson 22 April 2011 when can we safely delete [] dst?
        storage.copyTo(dst);
        SkMemoryStream pictReadback(dst, streamSize);
        SkPicture* retval = new SkPicture (&pictReadback);
        return retval;
    }

    // Test: draw into a bitmap or pdf.
    // Depending on flags, possibly compare to an expected image
    // and possibly output a diff image if it fails to match.
    ErrorBitfield test_drawing(GM* gm,
                               const ConfigData& gRec,
                               const char writePath [],
                               const char readPath [],
                               const char diffPath [],
                               GrContext* context,
                               GrRenderTarget* rt,
                               SkBitmap* bitmap) {
        SkDynamicMemoryWStream document;

        if (gRec.fBackend == kRaster_Backend ||
            gRec.fBackend == kGPU_Backend) {
            // Early exit if we can't generate the image.
            ErrorBitfield errors = generate_image(gm, gRec, context, rt, bitmap,
                                                  false);
            if (ERROR_NONE != errors) {
                return errors;
            }
        } else if (gRec.fBackend == kPDF_Backend) {
            generate_pdf(gm, document);
#if CAN_IMAGE_PDF
            SkAutoDataUnref data(document.copyToData());
            SkMemoryStream stream(data->data(), data->size());
            SkPDFDocumentToBitmap(&stream, bitmap);
#endif
        } else if (gRec.fBackend == kXPS_Backend) {
            generate_xps(gm, document);
        }
        return handle_test_results(gm, gRec, writePath, readPath, diffPath,
                                   "", *bitmap, &document, NULL);
    }

    ErrorBitfield test_deferred_drawing(GM* gm,
                                        const ConfigData& gRec,
                                        const SkBitmap& referenceBitmap,
                                        const char diffPath [],
                                        GrContext* context,
                                        GrRenderTarget* rt) {
        SkDynamicMemoryWStream document;

        if (gRec.fBackend == kRaster_Backend ||
            gRec.fBackend == kGPU_Backend) {
            SkBitmap bitmap;
            // Early exit if we can't generate the image, but this is
            // expected in some cases, so don't report a test failure.
            if (!generate_image(gm, gRec, context, rt, &bitmap, true)) {
                return ERROR_NONE;
            }
            return handle_test_results(gm, gRec, NULL, NULL, diffPath,
                                       "-deferred", bitmap, NULL,
                                       &referenceBitmap);
        }
        return ERROR_NONE;
    }

    ErrorBitfield test_pipe_playback(GM* gm,
                                     const ConfigData& gRec,
                                     const SkBitmap& referenceBitmap,
                                     const char readPath [],
                                     const char diffPath []) {
        ErrorBitfield errors = ERROR_NONE;
        for (size_t i = 0; i < SK_ARRAY_COUNT(gPipeWritingFlagCombos); ++i) {
            SkBitmap bitmap;
            SkISize size = gm->getISize();
            setup_bitmap(gRec, size, &bitmap);
            SkCanvas canvas(bitmap);
            PipeController pipeController(&canvas);
            SkGPipeWriter writer;
            SkCanvas* pipeCanvas = writer.startRecording(
              &pipeController, gPipeWritingFlagCombos[i].flags);
            invokeGM(gm, pipeCanvas, false, false);
            writer.endRecording();
            SkString string("-pipe");
            string.append(gPipeWritingFlagCombos[i].name);
            errors |= handle_test_results(gm, gRec, NULL, NULL, diffPath,
                                          string.c_str(), bitmap, NULL,
                                          &referenceBitmap);
            if (errors != ERROR_NONE) {
                break;
            }
        }
        return errors;
    }

    ErrorBitfield test_tiled_pipe_playback(
      GM* gm, const ConfigData& gRec, const SkBitmap& referenceBitmap,
      const char readPath [], const char diffPath []) {
        ErrorBitfield errors = ERROR_NONE;
        for (size_t i = 0; i < SK_ARRAY_COUNT(gPipeWritingFlagCombos); ++i) {
            SkBitmap bitmap;
            SkISize size = gm->getISize();
            setup_bitmap(gRec, size, &bitmap);
            SkCanvas canvas(bitmap);
            TiledPipeController pipeController(bitmap);
            SkGPipeWriter writer;
            SkCanvas* pipeCanvas = writer.startRecording(
              &pipeController, gPipeWritingFlagCombos[i].flags);
            invokeGM(gm, pipeCanvas, false, false);
            writer.endRecording();
            SkString string("-tiled pipe");
            string.append(gPipeWritingFlagCombos[i].name);
            errors |= handle_test_results(gm, gRec, NULL, NULL, diffPath,
                                          string.c_str(), bitmap, NULL,
                                          &referenceBitmap);
            if (errors != ERROR_NONE) {
                break;
            }
        }
        return errors;
    }

    //
    // member variables.
    // They are public for now, to allow easier setting by tool_main().
    //

    // if true, emit a message when we can't find a reference image to compare
    bool fNotifyMissingReadReference;

    bool fUseFileHierarchy;

    // information about all failed tests we have encountered so far
    SkTArray<FailRec> fFailedTests;

}; // end of GMMain class definition

#if SK_SUPPORT_GPU
static const GLContextType kDontCare_GLContextType = GrContextFactory::kNative_GLContextType;
#else
static const GLContextType kDontCare_GLContextType = 0;
#endif

// If the platform does not support writing PNGs of PDFs then there will be no
// reference images to read. However, we can always write the .pdf files
static const ConfigFlags kPDFConfigFlags = CAN_IMAGE_PDF ? kRW_ConfigFlag :
                                                           kWrite_ConfigFlag;

static const ConfigData gRec[] = {
    { SkBitmap::kARGB_8888_Config, kRaster_Backend, kDontCare_GLContextType,                  0, kRW_ConfigFlag,    "8888" },
    { SkBitmap::kARGB_4444_Config, kRaster_Backend, kDontCare_GLContextType,                  0, kRW_ConfigFlag,    "4444" },
    { SkBitmap::kRGB_565_Config,   kRaster_Backend, kDontCare_GLContextType,                  0, kRW_ConfigFlag,    "565" },
#if defined(SK_SCALAR_IS_FLOAT) && SK_SUPPORT_GPU
    { SkBitmap::kARGB_8888_Config, kGPU_Backend,    GrContextFactory::kNative_GLContextType,  0, kRW_ConfigFlag,    "gpu" },
#ifndef SK_BUILD_FOR_ANDROID
    // currently we don't want to run MSAA tests on Android
    { SkBitmap::kARGB_8888_Config, kGPU_Backend,    GrContextFactory::kNative_GLContextType, 16, kRW_ConfigFlag,    "msaa16" },
#endif
    /* The debug context does not generate images */
    { SkBitmap::kARGB_8888_Config, kGPU_Backend,    GrContextFactory::kDebug_GLContextType,   0, kNone_ConfigFlag,  "debug" },
#if SK_ANGLE
    { SkBitmap::kARGB_8888_Config, kGPU_Backend,    GrContextFactory::kANGLE_GLContextType,   0, kRW_ConfigFlag,    "angle" },
    { SkBitmap::kARGB_8888_Config, kGPU_Backend,    GrContextFactory::kANGLE_GLContextType,  16, kRW_ConfigFlag,    "anglemsaa16" },
#endif // SK_ANGLE
#ifdef SK_MESA
    { SkBitmap::kARGB_8888_Config, kGPU_Backend,    GrContextFactory::kMESA_GLContextType,    0, kRW_ConfigFlag,    "mesa" },
#endif // SK_MESA
#endif // defined(SK_SCALAR_IS_FLOAT) && SK_SUPPORT_GPU
#ifdef SK_SUPPORT_XPS
    /* At present we have no way of comparing XPS files (either natively or by converting to PNG). */
    { SkBitmap::kARGB_8888_Config, kXPS_Backend,    kDontCare_GLContextType,                  0, kWrite_ConfigFlag, "xps" },
#endif // SK_SUPPORT_XPS
#ifdef SK_SUPPORT_PDF
    { SkBitmap::kARGB_8888_Config, kPDF_Backend,    kDontCare_GLContextType,                  0, kPDFConfigFlags,   "pdf" },
#endif // SK_SUPPORT_PDF
};

static void usage(const char * argv0) {
    SkDebugf("%s\n", argv0);
    SkDebugf("    [--config ");
    for (size_t i = 0; i < SK_ARRAY_COUNT(gRec); ++i) {
        if (i > 0) {
            SkDebugf("|");
        }
        SkDebugf(gRec[i].fName);
    }
    SkDebugf("]:\n        run these configurations\n");
    SkDebugf(
// Alphabetized ignoring "no" prefix ("readPath", "noreplay", "resourcePath").
// It would probably be better if we allowed both yes-and-no settings for each
// one, e.g.:
// [--replay|--noreplay]: whether to exercise SkPicture replay; default is yes
"    [--nodeferred]: skip the deferred rendering test pass\n"
"    [--diffPath|-d <path>]: write difference images into this directory\n"
"    [--disable-missing-warning]: don't print a message to stderr if\n"
"        unable to read a reference image for any tests (NOT default behavior)\n"
"    [--enable-missing-warning]: print message to stderr (but don't fail) if\n"
"        unable to read a reference image for any tests (default behavior)\n"
"    [--forceBWtext]: disable text anti-aliasing\n"
"    [--help|-h]: show this help message\n"
"    [--hierarchy|--nohierarchy]: whether to use multilevel directory structure\n"
"        when reading/writing files; default is no\n"
"    [--match <substring>]: only run tests whose name includes this substring\n"
"    [--modulo <remainder> <divisor>]: only run tests for which \n"
"        testIndex %% divisor == remainder\n"
"    [--nopdf]: skip the pdf rendering test pass\n"
"    [--nopipe]: Skip SkGPipe replay\n"
"    [--readPath|-r <path>]: read reference images from this dir, and report\n"
"        any differences between those and the newly generated ones\n"
"    [--noreplay]: do not exercise SkPicture replay\n"
"    [--resourcePath|-i <path>]: directory that stores image resources\n"
"    [--noserialize]: do not exercise SkPicture serialization & deserialization\n"
"    [--notexturecache]: disable the gpu texture cache\n"
"    [--tiledPipe]: Exercise tiled SkGPipe replay\n"
"    [--writePath|-w <path>]: write rendered images into this directory\n"
"    [--writePicturePath|-wp <path>]: write .skp files into this directory\n"
             );
}

static int findConfig(const char config[]) {
    for (size_t i = 0; i < SK_ARRAY_COUNT(gRec); i++) {
        if (!strcmp(config, gRec[i].fName)) {
            return i;
        }
    }
    return -1;
}

static bool skip_name(const SkTDArray<const char*> array, const char name[]) {
    if (0 == array.count()) {
        // no names, so don't skip anything
        return false;
    }
    for (int i = 0; i < array.count(); ++i) {
        if (strstr(name, array[i])) {
            // found the name, so don't skip
            return false;
        }
    }
    return true;
}

namespace skiagm {
#if SK_SUPPORT_GPU
SkAutoTUnref<GrContext> gGrContext;
/**
 * Sets the global GrContext, accessible by individual GMs
 */
static void SetGr(GrContext* grContext) {
    SkSafeRef(grContext);
    gGrContext.reset(grContext);
}

/**
 * Gets the global GrContext, can be called by GM tests.
 */
GrContext* GetGr();
GrContext* GetGr() {
    return gGrContext.get();
}

/**
 * Sets the global GrContext and then resets it to its previous value at
 * destruction.
 */
class AutoResetGr : SkNoncopyable {
public:
    AutoResetGr() : fOld(NULL) {}
    void set(GrContext* context) {
        SkASSERT(NULL == fOld);
        fOld = GetGr();
        SkSafeRef(fOld);
        SetGr(context);
    }
    ~AutoResetGr() { SetGr(fOld); SkSafeUnref(fOld); }
private:
    GrContext* fOld;
};
#else
GrContext* GetGr() { return NULL; }
#endif
}

int tool_main(int argc, char** argv);
int tool_main(int argc, char** argv) {

#ifdef SK_ENABLE_INST_COUNT
    gPrintInstCount = true;
#endif

    SkGraphics::Init();
    // we don't need to see this during a run
    gSkSuppressFontCachePurgeSpew = true;

    setSystemPreferences();
    GMMain gmmain;

    const char* writePath = NULL;   // if non-null, where we write the originals
    const char* writePicturePath = NULL;    // if non-null, where we write serialized pictures
    const char* readPath = NULL;    // if non-null, were we read from to compare
    const char* diffPath = NULL;    // if non-null, where we write our diffs (from compare)
    const char* resourcePath = NULL;// if non-null, where we read from for image resources

    SkTDArray<const char*> fMatches;

    bool doPDF = true;
    bool doReplay = true;
    bool doPipe = true;
    bool doTiledPipe = false;
    bool doSerialize = true;
    bool doDeferred = true;
    bool disableTextureCache = false;
    SkTDArray<size_t> configs;
    bool userConfig = false;

    int moduloRemainder = -1;
    int moduloDivisor = -1;

    const char* const commandName = argv[0];
    char* const* stop = argv + argc;
    for (++argv; argv < stop; ++argv) {
        if (strcmp(*argv, "--config") == 0) {
            argv++;
            if (argv < stop) {
                int index = findConfig(*argv);
                if (index >= 0) {
                    *configs.append() = index;
                    userConfig = true;
                } else {
                    SkString str;
                    str.printf("unrecognized config %s\n", *argv);
                    SkDebugf(str.c_str());
                    usage(commandName);
                    return -1;
                }
            } else {
                SkDebugf("missing arg for --config\n");
                usage(commandName);
                return -1;
            }
        } else if (strcmp(*argv, "--nodeferred") == 0) {
            doDeferred = false;
        } else if ((0 == strcmp(*argv, "--diffPath")) ||
                   (0 == strcmp(*argv, "-d"))) {
            argv++;
            if (argv < stop && **argv) {
                diffPath = *argv;
            }
        } else if (strcmp(*argv, "--disable-missing-warning") == 0) {
            gmmain.fNotifyMissingReadReference = false;
        } else if (strcmp(*argv, "--enable-missing-warning") == 0) {
            gmmain.fNotifyMissingReadReference = true;
        } else if (strcmp(*argv, "--forceBWtext") == 0) {
            gForceBWtext = true;
        } else if (strcmp(*argv, "--help") == 0 || strcmp(*argv, "-h") == 0) {
            usage(commandName);
            return -1;
        } else if (strcmp(*argv, "--hierarchy") == 0) {
            gmmain.fUseFileHierarchy = true;
        } else if (strcmp(*argv, "--nohierarchy") == 0) {
            gmmain.fUseFileHierarchy = false;
        } else if (strcmp(*argv, "--match") == 0) {
            ++argv;
            if (argv < stop && **argv) {
                // just record the ptr, no need for a deep copy
                *fMatches.append() = *argv;
            }
        } else if (strcmp(*argv, "--modulo") == 0) {
            ++argv;
            if (argv >= stop) {
                continue;
            }
            moduloRemainder = atoi(*argv);

            ++argv;
            if (argv >= stop) {
                continue;
            }
            moduloDivisor = atoi(*argv);
            if (moduloRemainder < 0 || moduloDivisor <= 0 || moduloRemainder >= moduloDivisor) {
                SkDebugf("invalid modulo values.");
                return -1;
            }
        } else if (strcmp(*argv, "--nopdf") == 0) {
            doPDF = false;
        } else if (strcmp(*argv, "--nopipe") == 0) {
            doPipe = false;
        } else if ((0 == strcmp(*argv, "--readPath")) ||
                   (0 == strcmp(*argv, "-r"))) {
            argv++;
            if (argv < stop && **argv) {
                readPath = *argv;
            }
        } else if (strcmp(*argv, "--noreplay") == 0) {
            doReplay = false;
        } else if ((0 == strcmp(*argv, "--resourcePath")) ||
                   (0 == strcmp(*argv, "-i"))) {
            argv++;
            if (argv < stop && **argv) {
                resourcePath = *argv;
            }
        } else if (strcmp(*argv, "--serialize") == 0) {
            doSerialize = true;
        } else if (strcmp(*argv, "--noserialize") == 0) {
            doSerialize = false;
        } else if (strcmp(*argv, "--notexturecache") == 0) {
            disableTextureCache = true;
        } else if (strcmp(*argv, "--tiledPipe") == 0) {
            doTiledPipe = true;
        } else if ((0 == strcmp(*argv, "--writePath")) ||
            (0 == strcmp(*argv, "-w"))) {
            argv++;
            if (argv < stop && **argv) {
                writePath = *argv;
            }
        } else if ((0 == strcmp(*argv, "--writePicturePath")) ||
                   (0 == strcmp(*argv, "-wp"))) {
            argv++;
            if (argv < stop && **argv) {
                writePicturePath = *argv;
            }
        } else {
            usage(commandName);
            return -1;
        }
    }
    if (argv != stop) {
        usage(commandName);
        return -1;
    }

    if (!userConfig) {
        // if no config is specified by user, we add them all.
        for (size_t i = 0; i < SK_ARRAY_COUNT(gRec); ++i) {
            *configs.append() = i;
        }
    }

    GM::SetResourcePath(resourcePath);

    if (readPath) {
        fprintf(stderr, "reading from %s\n", readPath);
    }
    if (writePath) {
        fprintf(stderr, "writing to %s\n", writePath);
    }
    if (writePicturePath) {
        fprintf(stderr, "writing pictures to %s\n", writePicturePath);
    }
    if (resourcePath) {
        fprintf(stderr, "reading resources from %s\n", resourcePath);
    }

    if (moduloDivisor <= 0) {
        moduloRemainder = -1;
    }
    if (moduloRemainder < 0 || moduloRemainder >= moduloDivisor) {
        moduloRemainder = -1;
    }

    // Accumulate success of all tests.
    int testsRun = 0;
    int testsPassed = 0;
    int testsFailed = 0;
    int testsMissingReferenceImages = 0;

#if SK_SUPPORT_GPU
    GrContextFactory* grFactory = new GrContextFactory;
    if (disableTextureCache) {
        skiagm::GetGr()->setTextureCacheLimits(0, 0);
    }
#endif

    int gmIndex = -1;
    SkString moduloStr;

    // If we will be writing out files, prepare subdirectories.
    if (writePath) {
        if (!sk_mkdir(writePath)) {
            return -1;
        }
        if (gmmain.fUseFileHierarchy) {
            for (int i = 0; i < configs.count(); i++) {
                ConfigData config = gRec[configs[i]];
                SkString subdir;
                subdir.appendf("%s%c%s", writePath, SkPATH_SEPARATOR,
                               config.fName);
                if (!sk_mkdir(subdir.c_str())) {
                    return -1;
                }
            }
        }
    }

    Iter iter;
    GM* gm;
    while ((gm = iter.next()) != NULL) {

        ++gmIndex;
        if (moduloRemainder >= 0) {
            if ((gmIndex % moduloDivisor) != moduloRemainder) {
                continue;
            }
            moduloStr.printf("[%d.%d] ", gmIndex, moduloDivisor);
        }

        const char* shortName = gm->shortName();
        if (skip_name(fMatches, shortName)) {
            SkDELETE(gm);
            continue;
        }

        SkISize size = gm->getISize();
        SkDebugf("%sdrawing... %s [%d %d]\n", moduloStr.c_str(), shortName,
                 size.width(), size.height());

        ErrorBitfield testErrors = ERROR_NONE;
        uint32_t gmFlags = gm->getFlags();

        for (int i = 0; i < configs.count(); i++) {
            ConfigData config = gRec[configs[i]];

            // Skip any tests that we don't even need to try.
            if ((kPDF_Backend == config.fBackend) &&
                (!doPDF || (gmFlags & GM::kSkipPDF_Flag)))
                {
                    continue;
                }
            if ((gmFlags & GM::kSkip565_Flag) &&
                (kRaster_Backend == config.fBackend) &&
                (SkBitmap::kRGB_565_Config == config.fConfig)) {
                continue;
            }

            // Now we know that we want to run this test and record its
            // success or failure.
            ErrorBitfield renderErrors = ERROR_NONE;
            GrRenderTarget* renderTarget = NULL;
#if SK_SUPPORT_GPU
            SkAutoTUnref<GrRenderTarget> rt;
            AutoResetGr autogr;
            if ((ERROR_NONE == renderErrors) &&
                kGPU_Backend == config.fBackend) {
                GrContext* gr = grFactory->get(config.fGLContextType);
                bool grSuccess = false;
                if (gr) {
                    // create a render target to back the device
                    GrTextureDesc desc;
                    desc.fConfig = kSkia8888_PM_GrPixelConfig;
                    desc.fFlags = kRenderTarget_GrTextureFlagBit;
                    desc.fWidth = gm->getISize().width();
                    desc.fHeight = gm->getISize().height();
                    desc.fSampleCnt = config.fSampleCnt;
                    GrTexture* tex = gr->createUncachedTexture(desc, NULL, 0);
                    if (tex) {
                        rt.reset(tex->asRenderTarget());
                        rt.get()->ref();
                        tex->unref();
                        autogr.set(gr);
                        renderTarget = rt.get();
                        grSuccess = NULL != renderTarget;
                    }
                }
                if (!grSuccess) {
                    renderErrors |= ERROR_NO_GPU_CONTEXT;
                }
            }
#endif

            SkBitmap comparisonBitmap;

            if (ERROR_NONE == renderErrors) {
                renderErrors |= gmmain.test_drawing(gm, config, writePath,
                                                    readPath, diffPath, GetGr(),
                                                    renderTarget,
                                                    &comparisonBitmap);
            }

            if (doDeferred && !renderErrors &&
                (kGPU_Backend == config.fBackend ||
                 kRaster_Backend == config.fBackend)) {
                renderErrors |= gmmain.test_deferred_drawing(gm, config,
                                                             comparisonBitmap,
                                                             diffPath, GetGr(),
                                                             renderTarget);
            }

            testErrors |= renderErrors;
        }

        SkBitmap comparisonBitmap;
        const ConfigData compareConfig =
            { SkBitmap::kARGB_8888_Config, kRaster_Backend, kDontCare_GLContextType, 0, kRW_ConfigFlag, "comparison" };
        testErrors |= gmmain.generate_image(gm, compareConfig, NULL, NULL, &comparisonBitmap, false);

        // run the picture centric GM steps
        if (!(gmFlags & GM::kSkipPicture_Flag)) {

            ErrorBitfield pictErrors = ERROR_NONE;

            //SkAutoTUnref<SkPicture> pict(generate_new_picture(gm));
            SkPicture* pict = gmmain.generate_new_picture(gm);
            SkAutoUnref aur(pict);

            if ((ERROR_NONE == testErrors) && doReplay) {
                SkBitmap bitmap;
                gmmain.generate_image_from_picture(gm, compareConfig, pict,
                                                   &bitmap);
                pictErrors |= gmmain.handle_test_results(gm, compareConfig,
                                                         NULL, NULL, diffPath,
                                                         "-replay", bitmap,
                                                         NULL,
                                                         &comparisonBitmap);
            }

            if ((ERROR_NONE == testErrors) &&
                (ERROR_NONE == pictErrors) &&
                doSerialize) {
                SkPicture* repict = gmmain.stream_to_new_picture(*pict);
                SkAutoUnref aurr(repict);

                SkBitmap bitmap;
                gmmain.generate_image_from_picture(gm, compareConfig, repict,
                                                   &bitmap);
                pictErrors |= gmmain.handle_test_results(gm, compareConfig,
                                                         NULL, NULL, diffPath,
                                                         "-serialize", bitmap,
                                                         NULL,
                                                         &comparisonBitmap);
            }

            if (writePicturePath) {
                const char* pictureSuffix = "skp";
                SkString path = gmmain.make_filename(writePicturePath, "",
                                                     SkString(gm->shortName()),
                                                     pictureSuffix);
                SkFILEWStream stream(path.c_str());
                pict->serialize(&stream);
            }

            testErrors |= pictErrors;
        }

        // run the pipe centric GM steps
        if (!(gmFlags & GM::kSkipPipe_Flag)) {

            ErrorBitfield pipeErrors = ERROR_NONE;

            if ((ERROR_NONE == testErrors) && doPipe) {
                pipeErrors |= gmmain.test_pipe_playback(gm, compareConfig,
                                                        comparisonBitmap,
                                                        readPath, diffPath);
            }

            if ((ERROR_NONE == testErrors) &&
                (ERROR_NONE == pipeErrors) &&
                doTiledPipe && !(gmFlags & GM::kSkipTiled_Flag)) {
                pipeErrors |= gmmain.test_tiled_pipe_playback(gm, compareConfig,
                                                              comparisonBitmap,
                                                              readPath,
                                                              diffPath);
            }

            testErrors |= pipeErrors;
        }

        // Update overall results.
        // We only tabulate the particular error types that we currently
        // care about (e.g., missing reference images). Later on, if we
        // want to also tabulate pixel mismatches vs dimension mistmatches
        // (or whatever else), we can do so.
        testsRun++;
        if (ERROR_NONE == testErrors) {
            testsPassed++;
        } else if (ERROR_READING_REFERENCE_IMAGE & testErrors) {
            testsMissingReferenceImages++;
        } else {
            testsFailed++;
        }

        SkDELETE(gm);
    }
    SkDebugf("Ran %d tests: %d passed, %d failed, %d missing reference images\n",
             testsRun, testsPassed, testsFailed, testsMissingReferenceImages);
    gmmain.ListErrors();

#if SK_SUPPORT_GPU

#if GR_CACHE_STATS
    for (int i = 0; i < configs.count(); i++) {
        ConfigData config = gRec[configs[i]];

        if (kGPU_Backend == config.fBackend) {
            GrContext* gr = grFactory->get(config.fGLContextType);

            SkDebugf("config: %s %x\n", config.fName, gr);
            gr->printCacheStats();
        }
    }
#endif

    delete grFactory;
#endif
    SkGraphics::Term();

    return (0 == testsFailed) ? 0 : -1;
}

#if !defined(SK_BUILD_FOR_IOS) && !defined(SK_BUILD_FOR_NACL)
int main(int argc, char * const argv[]) {
    return tool_main(argc, (char**) argv);
}
#endif


/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkLinearGradient.h"

static inline int repeat_bits(int x, const int bits) {
    return x & ((1 << bits) - 1);
}

static inline int repeat_8bits(int x) {
    return x & 0xFF;
}

// Visual Studio 2010 (MSC_VER=1600) optimizes bit-shift code incorrectly.
// See http://code.google.com/p/skia/issues/detail?id=472
#if defined(_MSC_VER) && (_MSC_VER >= 1600)
#pragma optimize("", off)
#endif

static inline int mirror_bits(int x, const int bits) {
#ifdef SK_CPU_HAS_CONDITIONAL_INSTR
    if (x & (1 << bits))
        x = ~x;
    return x & ((1 << bits) - 1);
#else
    int s = x << (31 - bits) >> 31;
    return (x ^ s) & ((1 << bits) - 1);
#endif
}

static inline int mirror_8bits(int x) {
#ifdef SK_CPU_HAS_CONDITIONAL_INSTR
    if (x & 256) {
        x = ~x;
    }
    return x & 255;
#else
    int s = x << 23 >> 31;
    return (x ^ s) & 0xFF;
#endif
}

#if defined(_MSC_VER) && (_MSC_VER >= 1600)
#pragma optimize("", on)
#endif

static void pts_to_unit_matrix(const SkPoint pts[2], SkMatrix* matrix) {
    SkVector    vec = pts[1] - pts[0];
    SkScalar    mag = vec.length();
    SkScalar    inv = mag ? SkScalarInvert(mag) : 0;

    vec.scale(inv);
    matrix->setSinCos(-vec.fY, vec.fX, pts[0].fX, pts[0].fY);
    matrix->postTranslate(-pts[0].fX, -pts[0].fY);
    matrix->postScale(inv, inv);
}

///////////////////////////////////////////////////////////////////////////////

SkLinearGradient::SkLinearGradient(const SkPoint pts[2],
                                   const SkColor colors[],
                                   const SkScalar pos[],
                                   int colorCount,
                                   SkShader::TileMode mode,
                                   SkUnitMapper* mapper)
    : SkGradientShaderBase(colors, pos, colorCount, mode, mapper)
    , fStart(pts[0])
    , fEnd(pts[1]) {
    pts_to_unit_matrix(pts, &fPtsToUnit);
}

SkLinearGradient::SkLinearGradient(SkFlattenableReadBuffer& buffer)
    : INHERITED(buffer)
    , fStart(buffer.readPoint())
    , fEnd(buffer.readPoint()) {
}

void SkLinearGradient::flatten(SkFlattenableWriteBuffer& buffer) const {
    this->INHERITED::flatten(buffer);
    buffer.writePoint(fStart);
    buffer.writePoint(fEnd);
}

bool SkLinearGradient::setContext(const SkBitmap& device, const SkPaint& paint,
                                 const SkMatrix& matrix) {
    if (!this->INHERITED::setContext(device, paint, matrix)) {
        return false;
    }

    unsigned mask = SkMatrix::kTranslate_Mask | SkMatrix::kScale_Mask;
    if ((fDstToIndex.getType() & ~mask) == 0) {
        fFlags |= SkShader::kConstInY32_Flag;
        if ((fFlags & SkShader::kHasSpan16_Flag) && !paint.isDither()) {
            // only claim this if we do have a 16bit mode (i.e. none of our
            // colors have alpha), and if we are not dithering (which obviously
            // is not const in Y).
            fFlags |= SkShader::kConstInY16_Flag;
        }
    }
    return true;
}

#define NO_CHECK_ITER               \
    do {                            \
    unsigned fi = fx >> SkGradientShaderBase::kCache32Shift; \
    SkASSERT(fi <= 0xFF);           \
    fx += dx;                       \
    *dstC++ = cache[toggle + fi];   \
    toggle ^= SkGradientShaderBase::kDitherStride32; \
    } while (0)

namespace {

typedef void (*LinearShadeProc)(TileProc proc, SkFixed dx, SkFixed fx,
                                SkPMColor* dstC, const SkPMColor* cache,
                                int toggle, int count);

// Linear interpolation (lerp) is unnecessary if there are no sharp
// discontinuities in the gradient - which must be true if there are
// only 2 colors - but it's cheap.
void shadeSpan_linear_vertical_lerp(TileProc proc, SkFixed dx, SkFixed fx,
                                    SkPMColor* SK_RESTRICT dstC,
                                    const SkPMColor* SK_RESTRICT cache,
                                    int toggle, int count) {
    // We're a vertical gradient, so no change in a span.
    // If colors change sharply across the gradient, dithering is
    // insufficient (it subsamples the color space) and we need to lerp.
    unsigned fullIndex = proc(fx);
    unsigned fi = fullIndex >> (16 - SkGradientShaderBase::kCache32Bits);
    unsigned remainder = fullIndex & SkGradientShaderBase::kLerpRemainderMask32;
    SkPMColor lerp =
        SkFastFourByteInterp(
            cache[toggle + fi + 1],
            cache[toggle + fi], remainder);
    SkPMColor dlerp =
        SkFastFourByteInterp(
            cache[(toggle ^ SkGradientShaderBase::kDitherStride32) + fi + 1],
            cache[(toggle ^ SkGradientShaderBase::kDitherStride32) + fi], remainder);
    sk_memset32_dither(dstC, lerp, dlerp, count);
}

void shadeSpan_linear_clamp(TileProc proc, SkFixed dx, SkFixed fx,
                            SkPMColor* SK_RESTRICT dstC,
                            const SkPMColor* SK_RESTRICT cache,
                            int toggle, int count) {
    SkClampRange range;
    range.init(fx, dx, count, 0, SkGradientShaderBase::kGradient32Length);

    if ((count = range.fCount0) > 0) {
        sk_memset32_dither(dstC,
            cache[toggle + range.fV0],
            cache[(toggle ^ SkGradientShaderBase::kDitherStride32) + range.fV0],
            count);
        dstC += count;
    }
    if ((count = range.fCount1) > 0) {
        int unroll = count >> 3;
        fx = range.fFx1;
        for (int i = 0; i < unroll; i++) {
            NO_CHECK_ITER;  NO_CHECK_ITER;
            NO_CHECK_ITER;  NO_CHECK_ITER;
            NO_CHECK_ITER;  NO_CHECK_ITER;
            NO_CHECK_ITER;  NO_CHECK_ITER;
        }
        if ((count &= 7) > 0) {
            do {
                NO_CHECK_ITER;
            } while (--count != 0);
        }
    }
    if ((count = range.fCount2) > 0) {
        sk_memset32_dither(dstC,
            cache[toggle + range.fV1],
            cache[(toggle ^ SkGradientShaderBase::kDitherStride32) + range.fV1],
            count);
    }
}

void shadeSpan_linear_mirror(TileProc proc, SkFixed dx, SkFixed fx,
                             SkPMColor* SK_RESTRICT dstC,
                             const SkPMColor* SK_RESTRICT cache,
                             int toggle, int count) {
    do {
        unsigned fi = mirror_8bits(fx >> 8);
        SkASSERT(fi <= 0xFF);
        fx += dx;
        *dstC++ = cache[toggle + fi];
        toggle ^= SkGradientShaderBase::kDitherStride32;
    } while (--count != 0);
}

void shadeSpan_linear_repeat(TileProc proc, SkFixed dx, SkFixed fx,
        SkPMColor* SK_RESTRICT dstC,
        const SkPMColor* SK_RESTRICT cache,
        int toggle, int count) {
    do {
        unsigned fi = repeat_8bits(fx >> 8);
        SkASSERT(fi <= 0xFF);
        fx += dx;
        *dstC++ = cache[toggle + fi];
        toggle ^= SkGradientShaderBase::kDitherStride32;
    } while (--count != 0);
}

}

void SkLinearGradient::shadeSpan(int x, int y, SkPMColor* SK_RESTRICT dstC,
                                int count) {
    SkASSERT(count > 0);

    SkPoint             srcPt;
    SkMatrix::MapXYProc dstProc = fDstToIndexProc;
    TileProc            proc = fTileProc;
    const SkPMColor* SK_RESTRICT cache = this->getCache32();
#ifdef USE_DITHER_32BIT_GRADIENT
    int                 toggle = ((x ^ y) & 1) * kDitherStride32;
#else
    int toggle = 0;
#endif

    if (fDstToIndexClass != kPerspective_MatrixClass) {
        dstProc(fDstToIndex, SkIntToScalar(x) + SK_ScalarHalf,
                             SkIntToScalar(y) + SK_ScalarHalf, &srcPt);
        SkFixed dx, fx = SkScalarToFixed(srcPt.fX);

        if (fDstToIndexClass == kFixedStepInX_MatrixClass) {
            SkFixed dxStorage[1];
            (void)fDstToIndex.fixedStepInX(SkIntToScalar(y), dxStorage, NULL);
            dx = dxStorage[0];
        } else {
            SkASSERT(fDstToIndexClass == kLinear_MatrixClass);
            dx = SkScalarToFixed(fDstToIndex.getScaleX());
        }

        LinearShadeProc shadeProc = shadeSpan_linear_repeat;
        if (SkFixedNearlyZero(dx)) {
            shadeProc = shadeSpan_linear_vertical_lerp;
        } else if (SkShader::kClamp_TileMode == fTileMode) {
            shadeProc = shadeSpan_linear_clamp;
        } else if (SkShader::kMirror_TileMode == fTileMode) {
            shadeProc = shadeSpan_linear_mirror;
        } else {
            SkASSERT(SkShader::kRepeat_TileMode == fTileMode);
        }
        (*shadeProc)(proc, dx, fx, dstC, cache, toggle, count);
    } else {
        SkScalar    dstX = SkIntToScalar(x);
        SkScalar    dstY = SkIntToScalar(y);
        do {
            dstProc(fDstToIndex, dstX, dstY, &srcPt);
            unsigned fi = proc(SkScalarToFixed(srcPt.fX));
            SkASSERT(fi <= 0xFFFF);
            *dstC++ = cache[toggle + (fi >> kCache32Shift)];
            toggle ^= SkGradientShaderBase::kDitherStride32;
            dstX += SK_Scalar1;
        } while (--count != 0);
    }
}

SkShader::BitmapType SkLinearGradient::asABitmap(SkBitmap* bitmap,
                                                SkMatrix* matrix,
                                                TileMode xy[]) const {
    if (bitmap) {
        this->getGradientTableBitmap(bitmap);
    }
    if (matrix) {
        matrix->preConcat(fPtsToUnit);
    }
    if (xy) {
        xy[0] = fTileMode;
        xy[1] = kClamp_TileMode;
    }
    return kLinear_BitmapType;
}

SkShader::GradientType SkLinearGradient::asAGradient(GradientInfo* info) const {
    if (info) {
        commonAsAGradient(info);
        info->fPoint[0] = fStart;
        info->fPoint[1] = fEnd;
    }
    return kLinear_GradientType;
}

static void dither_memset16(uint16_t dst[], uint16_t value, uint16_t other,
                            int count) {
    if (reinterpret_cast<uintptr_t>(dst) & 2) {
        *dst++ = value;
        count -= 1;
        SkTSwap(value, other);
    }

    sk_memset32((uint32_t*)dst, (value << 16) | other, count >> 1);

    if (count & 1) {
        dst[count - 1] = value;
    }
}

#define NO_CHECK_ITER_16                \
    do {                                \
    unsigned fi = fx >> SkGradientShaderBase::kCache16Shift;  \
    SkASSERT(fi < SkGradientShaderBase::kCache16Count);       \
    fx += dx;                           \
    *dstC++ = cache[toggle + fi];       \
    toggle ^= SkGradientShaderBase::kDitherStride16;            \
    } while (0)

namespace {

typedef void (*LinearShade16Proc)(TileProc proc, SkFixed dx, SkFixed fx,
                                  uint16_t* dstC, const uint16_t* cache,
                                  int toggle, int count);

void shadeSpan16_linear_vertical(TileProc proc, SkFixed dx, SkFixed fx,
                                 uint16_t* SK_RESTRICT dstC,
                                 const uint16_t* SK_RESTRICT cache,
                                 int toggle, int count) {
    // we're a vertical gradient, so no change in a span
    unsigned fi = proc(fx) >> SkGradientShaderBase::kCache16Shift;
    SkASSERT(fi < SkGradientShaderBase::kCache16Count);
    dither_memset16(dstC, cache[toggle + fi],
        cache[(toggle ^ SkGradientShaderBase::kDitherStride16) + fi], count);

}

void shadeSpan16_linear_clamp(TileProc proc, SkFixed dx, SkFixed fx,
                              uint16_t* SK_RESTRICT dstC,
                              const uint16_t* SK_RESTRICT cache,
                              int toggle, int count) {
    SkClampRange range;
    range.init(fx, dx, count, 0, SkGradientShaderBase::kGradient16Length);

    if ((count = range.fCount0) > 0) {
        dither_memset16(dstC,
            cache[toggle + range.fV0],
            cache[(toggle ^ SkGradientShaderBase::kDitherStride16) + range.fV0],
            count);
        dstC += count;
    }
    if ((count = range.fCount1) > 0) {
        int unroll = count >> 3;
        fx = range.fFx1;
        for (int i = 0; i < unroll; i++) {
            NO_CHECK_ITER_16;  NO_CHECK_ITER_16;
            NO_CHECK_ITER_16;  NO_CHECK_ITER_16;
            NO_CHECK_ITER_16;  NO_CHECK_ITER_16;
            NO_CHECK_ITER_16;  NO_CHECK_ITER_16;
        }
        if ((count &= 7) > 0) {
            do {
                NO_CHECK_ITER_16;
            } while (--count != 0);
        }
    }
    if ((count = range.fCount2) > 0) {
        dither_memset16(dstC,
            cache[toggle + range.fV1],
            cache[(toggle ^ SkGradientShaderBase::kDitherStride16) + range.fV1],
            count);
    }
}

void shadeSpan16_linear_mirror(TileProc proc, SkFixed dx, SkFixed fx,
                               uint16_t* SK_RESTRICT dstC,
                               const uint16_t* SK_RESTRICT cache,
                               int toggle, int count) {
    do {
        unsigned fi = mirror_bits(fx >> SkGradientShaderBase::kCache16Shift,
                                        SkGradientShaderBase::kCache16Bits);
        SkASSERT(fi < SkGradientShaderBase::kCache16Count);
        fx += dx;
        *dstC++ = cache[toggle + fi];
        toggle ^= SkGradientShaderBase::kDitherStride16;
    } while (--count != 0);
}

void shadeSpan16_linear_repeat(TileProc proc, SkFixed dx, SkFixed fx,
                               uint16_t* SK_RESTRICT dstC,
                               const uint16_t* SK_RESTRICT cache,
                               int toggle, int count) {
    do {
        unsigned fi = repeat_bits(fx >> SkGradientShaderBase::kCache16Shift,
                                  SkGradientShaderBase::kCache16Bits);
        SkASSERT(fi < SkGradientShaderBase::kCache16Count);
        fx += dx;
        *dstC++ = cache[toggle + fi];
        toggle ^= SkGradientShaderBase::kDitherStride16;
    } while (--count != 0);
}
}

void SkLinearGradient::shadeSpan16(int x, int y,
                                  uint16_t* SK_RESTRICT dstC, int count) {
    SkASSERT(count > 0);

    SkPoint             srcPt;
    SkMatrix::MapXYProc dstProc = fDstToIndexProc;
    TileProc            proc = fTileProc;
    const uint16_t* SK_RESTRICT cache = this->getCache16();
    int                 toggle = ((x ^ y) & 1) * kDitherStride16;

    if (fDstToIndexClass != kPerspective_MatrixClass) {
        dstProc(fDstToIndex, SkIntToScalar(x) + SK_ScalarHalf,
                             SkIntToScalar(y) + SK_ScalarHalf, &srcPt);
        SkFixed dx, fx = SkScalarToFixed(srcPt.fX);

        if (fDstToIndexClass == kFixedStepInX_MatrixClass) {
            SkFixed dxStorage[1];
            (void)fDstToIndex.fixedStepInX(SkIntToScalar(y), dxStorage, NULL);
            dx = dxStorage[0];
        } else {
            SkASSERT(fDstToIndexClass == kLinear_MatrixClass);
            dx = SkScalarToFixed(fDstToIndex.getScaleX());
        }

        LinearShade16Proc shadeProc = shadeSpan16_linear_repeat;
        if (SkFixedNearlyZero(dx)) {
            shadeProc = shadeSpan16_linear_vertical;
        } else if (SkShader::kClamp_TileMode == fTileMode) {
            shadeProc = shadeSpan16_linear_clamp;
        } else if (SkShader::kMirror_TileMode == fTileMode) {
            shadeProc = shadeSpan16_linear_mirror;
        } else {
            SkASSERT(SkShader::kRepeat_TileMode == fTileMode);
        }
        (*shadeProc)(proc, dx, fx, dstC, cache, toggle, count);
    } else {
        SkScalar    dstX = SkIntToScalar(x);
        SkScalar    dstY = SkIntToScalar(y);
        do {
            dstProc(fDstToIndex, dstX, dstY, &srcPt);
            unsigned fi = proc(SkScalarToFixed(srcPt.fX));
            SkASSERT(fi <= 0xFFFF);

            int index = fi >> kCache16Shift;
            *dstC++ = cache[toggle + index];
            toggle ^= SkGradientShaderBase::kDitherStride16;

            dstX += SK_Scalar1;
        } while (--count != 0);
    }
}

#if SK_SUPPORT_GPU

#include "GrTBackendEffectFactory.h"

/////////////////////////////////////////////////////////////////////

class GrGLLinearGradient : public GrGLGradientEffect {
public:

    GrGLLinearGradient(const GrBackendEffectFactory& factory,
                       const GrEffect&)
                       : INHERITED (factory) { }

    virtual ~GrGLLinearGradient() { }

    virtual void emitCode(GrGLShaderBuilder*,
                          const GrEffectStage&,
                          EffectKey,
                          const char* vertexCoords,
                          const char* outputColor,
                          const char* inputColor,
                          const TextureSamplerArray&) SK_OVERRIDE;

    static EffectKey GenKey(const GrEffectStage& stage, const GrGLCaps&) {
        return GenMatrixKey(stage);
    }

private:

    typedef GrGLGradientEffect INHERITED;
};

/////////////////////////////////////////////////////////////////////

class GrLinearGradient : public GrGradientEffect {
public:

    GrLinearGradient(GrContext* ctx,
                     const SkLinearGradient& shader,
                     const SkMatrix& matrix,
                     SkShader::TileMode tm)
        : INHERITED(ctx, shader, matrix, tm) { }
    virtual ~GrLinearGradient() { }

    static const char* Name() { return "Linear Gradient"; }
    const GrBackendEffectFactory& getFactory() const SK_OVERRIDE {
        return GrTBackendEffectFactory<GrLinearGradient>::getInstance();
    }

    typedef GrGLLinearGradient GLEffect;

private:
    GR_DECLARE_EFFECT_TEST;

    typedef GrGradientEffect INHERITED;
};

/////////////////////////////////////////////////////////////////////

GR_DEFINE_EFFECT_TEST(GrLinearGradient);

GrEffect* GrLinearGradient::TestCreate(SkRandom* random,
                                       GrContext* context,
                                       GrTexture**) {
    SkPoint points[] = {{random->nextUScalar1(), random->nextUScalar1()},
                        {random->nextUScalar1(), random->nextUScalar1()}};

    SkColor colors[kMaxRandomGradientColors];
    SkScalar stopsArray[kMaxRandomGradientColors];
    SkScalar* stops = stopsArray;
    SkShader::TileMode tm;
    int colorCount = RandomGradientParams(random, colors, &stops, &tm);
    SkAutoTUnref<SkShader> shader(SkGradientShader::CreateLinear(points,
                                                                 colors, stops, colorCount,
                                                                 tm));
    SkPaint paint;
    return shader->asNewEffect(context, paint);
}

/////////////////////////////////////////////////////////////////////

void GrGLLinearGradient::emitCode(GrGLShaderBuilder* builder,
                                  const GrEffectStage& stage,
                                  EffectKey key,
                                  const char* vertexCoords,
                                  const char* outputColor,
                                  const char* inputColor,
                                  const TextureSamplerArray& samplers) {
    this->emitYCoordUniform(builder);
    const char* coords;
    this->setupMatrix(builder, key, vertexCoords, &coords);
    SkString t;
    t.append(coords);
    t.append(".x");
    this->emitColorLookup(builder, t.c_str(), outputColor, inputColor, samplers[0]);
}

/////////////////////////////////////////////////////////////////////

GrEffect* SkLinearGradient::asNewEffect(GrContext* context, const SkPaint&) const {
    SkASSERT(NULL != context);
    SkMatrix matrix;
    if (!this->getLocalMatrix().invert(&matrix)) {
        return false;
    }
    matrix.postConcat(fPtsToUnit);
    return SkNEW_ARGS(GrLinearGradient, (context, *this, matrix, fTileMode));
}

#else

GrEffect* SkLinearGradient::asNewEffect(GrContext* context, const SkPaint&) const {
    SkDEBUGFAIL("Should not call in GPU-less build");
    return NULL;
}

#endif

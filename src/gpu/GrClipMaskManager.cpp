
/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrClipMaskManager.h"
#include "effects/GrTextureDomainEffect.h"
#include "GrGpu.h"
#include "GrRenderTarget.h"
#include "GrStencilBuffer.h"
#include "GrPathRenderer.h"
#include "GrPaint.h"
#include "SkRasterClip.h"
#include "SkStroke.h"
#include "GrAAConvexPathRenderer.h"
#include "GrAAHairLinePathRenderer.h"
#include "GrSWMaskHelper.h"
#include "GrCacheID.h"

GR_DEFINE_RESOURCE_CACHE_DOMAIN(GrClipMaskManager, GetAlphaMaskDomain)

#define GR_AA_CLIP 1

typedef SkClipStack::Element Element;

using namespace GrReducedClip;

////////////////////////////////////////////////////////////////////////////////
namespace {
// set up the draw state to enable the aa clipping mask. Besides setting up the
// stage matrix this also alters the vertex layout
void setup_drawstate_aaclip(GrGpu* gpu,
                            GrTexture* result,
                            const GrIRect &devBound) {
    GrDrawState* drawState = gpu->drawState();
    GrAssert(drawState);

    static const int kMaskStage = GrPaint::kTotalStages+1;

    SkMatrix mat;
    mat.setIDiv(result->width(), result->height());
    mat.preTranslate(SkIntToScalar(-devBound.fLeft),
                     SkIntToScalar(-devBound.fTop));
    mat.preConcat(drawState->getViewMatrix());

    drawState->stage(kMaskStage)->reset();

    SkIRect domainTexels = SkIRect::MakeWH(devBound.width(), devBound.height());
    // This could be a long-lived effect that is cached with the alpha-mask.
    drawState->stage(kMaskStage)->setEffect(
        GrTextureDomainEffect::Create(result,
                                      mat,
                                      GrTextureDomainEffect::MakeTexelDomain(result, domainTexels),
                                      GrTextureDomainEffect::kDecal_WrapMode))->unref();
}

bool path_needs_SW_renderer(GrContext* context,
                            GrGpu* gpu,
                            const SkPath& path,
                            const SkStroke& stroke,
                            bool doAA) {
    // last (false) parameter disallows use of the SW path renderer
    return NULL == context->getPathRenderer(path, stroke, gpu, doAA, false);
}

}

/*
 * This method traverses the clip stack to see if the GrSoftwarePathRenderer
 * will be used on any element. If so, it returns true to indicate that the
 * entire clip should be rendered in SW and then uploaded en masse to the gpu.
 */
bool GrClipMaskManager::useSWOnlyPath(const ElementList& elements) {

    // TODO: generalize this function so that when
    // a clip gets complex enough it can just be done in SW regardless
    // of whether it would invoke the GrSoftwarePathRenderer.
    bool useSW = false;
    SkStroke stroke;
    stroke.setDoFill(true);

    for (ElementList::Iter iter(elements.headIter()); iter.get(); iter.next()) {
        const Element* element = iter.get();
        // rects can always be drawn directly w/o using the software path
        // so only paths need to be checked
        if (Element::kPath_Type == element->getType() &&
            path_needs_SW_renderer(this->getContext(), fGpu,
                                   element->getPath(),
                                   stroke,
                                   element->isAA())) {
            return true;
        }
    }
    return false;
}

////////////////////////////////////////////////////////////////////////////////
// sort out what kind of clip mask needs to be created: alpha, stencil,
// scissor, or entirely software
bool GrClipMaskManager::setupClipping(const GrClipData* clipDataIn) {
    fCurrClipMaskType = kNone_ClipMaskType;

    ElementList elements(16);
    InitialState initialState;
    SkIRect clipSpaceIBounds;
    bool requiresAA;
    bool isRect = false;

    GrDrawState* drawState = fGpu->drawState();

    const GrRenderTarget* rt = drawState->getRenderTarget();
    // GrDrawTarget should have filtered this for us
    GrAssert(NULL != rt);

    bool ignoreClip = !drawState->isClipState() || clipDataIn->fClipStack->isWideOpen();

    if (!ignoreClip) {
        SkIRect clipSpaceRTIBounds = SkIRect::MakeWH(rt->width(), rt->height());
        clipSpaceRTIBounds.offset(clipDataIn->fOrigin);
        ReduceClipStack(*clipDataIn->fClipStack,
                        clipSpaceRTIBounds,
                        &elements,
                        &initialState,
                        &clipSpaceIBounds,
                        &requiresAA);
        if (elements.isEmpty()) {
            if (kAllIn_InitialState == initialState) {
                ignoreClip = clipSpaceIBounds == clipSpaceRTIBounds;
                isRect = true;
            } else {
                return false;
            }
        }
    }

    if (ignoreClip) {
        fGpu->disableScissor();
        this->setGpuStencil();
        return true;
    }

#if GR_AA_CLIP
    // TODO: catch isRect && requiresAA and use clip planes if available rather than a mask.

    // If MSAA is enabled we can do everything in the stencil buffer.
    if (0 == rt->numSamples() && requiresAA) {
        int32_t genID = clipDataIn->fClipStack->getTopmostGenID();
        GrTexture* result = NULL;

        if (this->useSWOnlyPath(elements)) {
            // The clip geometry is complex enough that it will be more efficient to create it
            // entirely in software
            result = this->createSoftwareClipMask(genID,
                                                  initialState,
                                                  elements,
                                                  clipSpaceIBounds);
        } else {
            result = this->createAlphaClipMask(genID,
                                               initialState,
                                               elements,
                                               clipSpaceIBounds);
        }

        if (NULL != result) {
            // The mask's top left coord should be pinned to the rounded-out top left corner of
            // clipSpace bounds. We determine the mask's position WRT to the render target here.
            SkIRect rtSpaceMaskBounds = clipSpaceIBounds;
            rtSpaceMaskBounds.offset(-clipDataIn->fOrigin);
            setup_drawstate_aaclip(fGpu, result, rtSpaceMaskBounds);
            fGpu->disableScissor();
            this->setGpuStencil();
            return true;
        }
        // if alpha clip mask creation fails fall through to the non-AA code paths
    }
#endif // GR_AA_CLIP

    // Either a hard (stencil buffer) clip was explicitly requested or an anti-aliased clip couldn't
    // be created. In either case, free up the texture in the anti-aliased mask cache.
    // TODO: this may require more investigation. Ganesh performs a lot of utility draws (e.g.,
    // clears, InOrderDrawBuffer playbacks) that hit the stencil buffer path. These may be
    // "incorrectly" clearing the AA cache.
    fAACache.reset();

    // If the clip is a rectangle then just set the scissor. Otherwise, create
    // a stencil mask.
    if (isRect) {
        SkIRect clipRect = clipSpaceIBounds;
        clipRect.offset(-clipDataIn->fOrigin);
        fGpu->enableScissor(clipRect);
        this->setGpuStencil();
        return true;
    }

    // use the stencil clip if we can't represent the clip as a rectangle.
    SkIPoint clipSpaceToStencilSpaceOffset = -clipDataIn->fOrigin;
    this->createStencilClipMask(initialState,
                                elements,
                                clipSpaceIBounds,
                                clipSpaceToStencilSpaceOffset);

    // This must occur after createStencilClipMask. That function may change the scissor. Also, it
    // only guarantees that the stencil mask is correct within the bounds it was passed, so we must
    // use both stencil and scissor test to the bounds for the final draw.
    SkIRect scissorSpaceIBounds(clipSpaceIBounds);
    scissorSpaceIBounds.offset(clipSpaceToStencilSpaceOffset);
    fGpu->enableScissor(scissorSpaceIBounds);
    this->setGpuStencil();
    return true;
}

#define VISUALIZE_COMPLEX_CLIP 0

#if VISUALIZE_COMPLEX_CLIP
    #include "SkRandom.h"
    SkRandom gRandom;
    #define SET_RANDOM_COLOR drawState->setColor(0xff000000 | gRandom.nextU());
#else
    #define SET_RANDOM_COLOR
#endif

namespace {

////////////////////////////////////////////////////////////////////////////////
// set up the OpenGL blend function to perform the specified
// boolean operation for alpha clip mask creation
void setup_boolean_blendcoeffs(GrDrawState* drawState, SkRegion::Op op) {

    switch (op) {
        case SkRegion::kReplace_Op:
            drawState->setBlendFunc(kOne_GrBlendCoeff, kZero_GrBlendCoeff);
            break;
        case SkRegion::kIntersect_Op:
            drawState->setBlendFunc(kDC_GrBlendCoeff, kZero_GrBlendCoeff);
            break;
        case SkRegion::kUnion_Op:
            drawState->setBlendFunc(kOne_GrBlendCoeff, kISC_GrBlendCoeff);
            break;
        case SkRegion::kXOR_Op:
            drawState->setBlendFunc(kIDC_GrBlendCoeff, kISC_GrBlendCoeff);
            break;
        case SkRegion::kDifference_Op:
            drawState->setBlendFunc(kZero_GrBlendCoeff, kISC_GrBlendCoeff);
            break;
        case SkRegion::kReverseDifference_Op:
            drawState->setBlendFunc(kIDC_GrBlendCoeff, kZero_GrBlendCoeff);
            break;
        default:
            GrAssert(false);
            break;
    }
}

////////////////////////////////////////////////////////////////////////////////
bool draw_path_in_software(GrContext* context,
                           GrGpu* gpu,
                           const SkPath& path,
                           bool doAA,
                           const GrIRect& resultBounds) {
    SkStroke stroke;
    stroke.setDoFill(true);

    SkAutoTUnref<GrTexture> texture(
                GrSWMaskHelper::DrawPathMaskToTexture(context, path,
                                                      stroke,
                                                      resultBounds,
                                                      doAA, NULL));
    if (NULL == texture) {
        return false;
    }

    // The ClipMaskManager accumulates the clip mask in the UL corner
    GrIRect rect = GrIRect::MakeWH(resultBounds.width(), resultBounds.height());

    GrSWMaskHelper::DrawToTargetWithPathMask(texture, gpu, rect);

    GrAssert(!path.isInverseFillType());
    return true;
}
}

////////////////////////////////////////////////////////////////////////////////
bool GrClipMaskManager::drawClipShape(GrTexture* target, const SkClipStack::Element* element) {
    GrDrawState* drawState = fGpu->drawState();
    GrAssert(NULL != drawState);

    drawState->setRenderTarget(target->asRenderTarget());

    switch (element->getType()) {
        case Element::kRect_Type:
            // TODO: Do rects directly to the accumulator using a aa-rect GrEffect that covers the
            // entire mask bounds and writes 0 outside the rect.
            if (element->isAA()) {
                getContext()->getAARectRenderer()->fillAARect(fGpu, fGpu, element->getRect(), true);
            } else {
                fGpu->drawSimpleRect(element->getRect(), NULL);
            }
            return true;
        case Element::kPath_Type: {
            SkStroke stroke;
            stroke.setDoFill(true);
            GrPathRenderer* pr = this->getContext()->getPathRenderer(element->getPath(),
                                                                     stroke,
                                                                     fGpu,
                                                                     element->isAA(), false);
            if (NULL == pr) {
                return false;
            }
            pr->drawPath(element->getPath(), stroke, fGpu, element->isAA());
            break;
        }
        default:
            // something is wrong if we're trying to draw an empty element.
            GrCrash("Unexpected element type");
            return false;
    }
    return true;
}

void GrClipMaskManager::mergeMask(GrTexture* dstMask,
                                  GrTexture* srcMask,
                                  SkRegion::Op op,
                                  const GrIRect& dstBound,
                                  const GrIRect& srcBound) {
    GrDrawState* drawState = fGpu->drawState();
    GrAssert(NULL != drawState);
    SkMatrix oldMatrix = drawState->getViewMatrix();
    drawState->viewMatrix()->reset();

    drawState->setRenderTarget(dstMask->asRenderTarget());

    setup_boolean_blendcoeffs(drawState, op);

    SkMatrix sampleM;
    sampleM.setIDiv(srcMask->width(), srcMask->height());
    drawState->stage(0)->setEffect(
        GrTextureDomainEffect::Create(srcMask,
                                      sampleM,
                                      GrTextureDomainEffect::MakeTexelDomain(srcMask, srcBound),
                                      GrTextureDomainEffect::kDecal_WrapMode))->unref();
    fGpu->drawSimpleRect(SkRect::MakeFromIRect(dstBound), NULL);

    drawState->disableStage(0);
    drawState->setViewMatrix(oldMatrix);
}

// get a texture to act as a temporary buffer for AA clip boolean operations
// TODO: given the expense of createTexture we may want to just cache this too
void GrClipMaskManager::getTemp(int width, int height, GrAutoScratchTexture* temp) {
    if (NULL != temp->texture()) {
        // we've already allocated the temp texture
        return;
    }

    GrTextureDesc desc;
    desc.fFlags = kRenderTarget_GrTextureFlagBit|kNoStencil_GrTextureFlagBit;
    desc.fWidth = width;
    desc.fHeight = height;
    desc.fConfig = kAlpha_8_GrPixelConfig;

    temp->set(this->getContext(), desc);
}

////////////////////////////////////////////////////////////////////////////////
// Handles caching & allocation (if needed) of a clip alpha-mask texture for both the sw-upload
// or gpu-rendered cases. Returns true if there is no more work to be done (i.e., we got a cache
// hit)
bool GrClipMaskManager::getMaskTexture(int32_t clipStackGenID,
                                       const SkIRect& clipSpaceIBounds,
                                       GrTexture** result) {
    bool cached = fAACache.canReuse(clipStackGenID, clipSpaceIBounds);
    if (!cached) {

        // There isn't a suitable entry in the cache so we create a new texture to store the mask.
        // Since we are setting up the cache we know the last lookup was a miss. Free up the
        // currently cached mask so it can be reused.
        fAACache.reset();

        GrTextureDesc desc;
        desc.fFlags = kRenderTarget_GrTextureFlagBit|kNoStencil_GrTextureFlagBit;
        desc.fWidth = clipSpaceIBounds.width();
        desc.fHeight = clipSpaceIBounds.height();
        desc.fConfig = kAlpha_8_GrPixelConfig;

        fAACache.acquireMask(clipStackGenID, desc, clipSpaceIBounds);
    }

    *result = fAACache.getLastMask();
    return cached;
}

////////////////////////////////////////////////////////////////////////////////
// Create a 8-bit clip mask in alpha
GrTexture* GrClipMaskManager::createAlphaClipMask(int32_t clipStackGenID,
                                                  InitialState initialState,
                                                  const ElementList& elements,
                                                  const SkIRect& clipSpaceIBounds) {
    GrAssert(kNone_ClipMaskType == fCurrClipMaskType);

    GrTexture* result;
    if (this->getMaskTexture(clipStackGenID, clipSpaceIBounds, &result)) {
        fCurrClipMaskType = kAlpha_ClipMaskType;
        return result;
    }

    if (NULL == result) {
        fAACache.reset();
        return NULL;
    }

    GrDrawTarget::AutoStateRestore asr(fGpu, GrDrawTarget::kReset_ASRInit);
    GrDrawState* drawState = fGpu->drawState();

    GrDrawTarget::AutoGeometryPush agp(fGpu);

    // The top-left of the mask corresponds to the top-left corner of the bounds.
    SkVector clipToMaskOffset = {
        SkIntToScalar(-clipSpaceIBounds.fLeft),
        SkIntToScalar(-clipSpaceIBounds.fTop)
    };
    // The texture may be larger than necessary, this rect represents the part of the texture
    // we populate with a rasterization of the clip.
    SkIRect maskSpaceIBounds = SkIRect::MakeWH(clipSpaceIBounds.width(), clipSpaceIBounds.height());

    // Set the matrix so that rendered clip elements are transformed to mask space from clip space.
    drawState->viewMatrix()->setTranslate(clipToMaskOffset);

    // The scratch texture that we are drawing into can be substantially larger than the mask. Only
    // clear the part that we care about.
    fGpu->clear(&maskSpaceIBounds,
                kAllIn_InitialState == initialState ? 0xffffffff : 0x00000000,
                result->asRenderTarget());

    GrAutoScratchTexture temp;
    // walk through each clip element and perform its set op
    for (ElementList::Iter iter = elements.headIter(); iter.get(); iter.next()) {

        const Element* element = iter.get();
        SkRegion::Op op = element->getOp();

        if (SkRegion::kReplace_Op == op) {
            setup_boolean_blendcoeffs(drawState, op);
            this->drawClipShape(result, element);

        } else if (SkRegion::kReverseDifference_Op == op ||
                   SkRegion::kIntersect_Op == op) {
            this->getTemp(maskSpaceIBounds.fRight, maskSpaceIBounds.fBottom, &temp);
            if (NULL == temp.texture()) {
                fAACache.reset();
                return NULL;
            }

            // This is the bounds of the clip element in the space of the alpha-mask. The temporary
            // mask buffer can be substantially larger than the actually clip stack element. We
            // touch the minimum number of pixels necessary and use decal mode to combine it with
            // the accumulator
            GrIRect maskSpaceElementIBounds;
            if (element->isInverseFilled()) {
                maskSpaceElementIBounds = maskSpaceIBounds;
            } else {
                GrRect elementBounds = element->getBounds();
                elementBounds.offset(clipToMaskOffset);
                elementBounds.roundOut(&maskSpaceElementIBounds);
            }

            // clear the temp target & draw into it
            fGpu->clear(&maskSpaceElementIBounds, 0x00000000, temp.texture()->asRenderTarget());

            setup_boolean_blendcoeffs(drawState, SkRegion::kReplace_Op);
            if (!this->drawClipShape(temp.texture(), element)) {
                fAACache.reset();
                return NULL;
            }

            // Now draw into the accumulator using the real operation
            // and the temp buffer as a texture
            this->mergeMask(result, temp.texture(), op, maskSpaceIBounds, maskSpaceElementIBounds);
        } else {
            // all the remaining ops can just be directly draw into
            // the accumulation buffer
            setup_boolean_blendcoeffs(drawState, op);
            this->drawClipShape(result, element);
        }
    }

    fCurrClipMaskType = kAlpha_ClipMaskType;
    return result;
}

////////////////////////////////////////////////////////////////////////////////
// Create a 1-bit clip mask in the stencil buffer. 'devClipBounds' are in device
// (as opposed to canvas) coordinates
bool GrClipMaskManager::createStencilClipMask(InitialState initialState,
                                              const ElementList& elements,
                                              const SkIRect& clipSpaceIBounds,
                                              const SkIPoint& clipSpaceToStencilOffset) {

    GrAssert(kNone_ClipMaskType == fCurrClipMaskType);

    GrDrawState* drawState = fGpu->drawState();
    GrAssert(drawState->isClipState());

    GrRenderTarget* rt = drawState->getRenderTarget();
    GrAssert(NULL != rt);

    // TODO: dynamically attach a SB when needed.
    GrStencilBuffer* stencilBuffer = rt->getStencilBuffer();
    if (NULL == stencilBuffer) {
        return false;
    }
    int32_t genID = elements.tail()->getGenID();

    if (stencilBuffer->mustRenderClip(genID, clipSpaceIBounds, clipSpaceToStencilOffset)) {

        stencilBuffer->setLastClip(genID, clipSpaceIBounds, clipSpaceToStencilOffset);

        // we set the current clip to the bounds so that our recursive draws are scissored to them.
        // We use the copy of the GrClipData we just stashed on the SB to render from. We set it
        // back after we finish drawing it into the stencil.
        const GrClipData* oldClipData = fGpu->getClip();

        SkIRect stencilSpaceIBounds(clipSpaceIBounds);
        stencilSpaceIBounds.offset(clipSpaceToStencilOffset);

        SkClipStack newClipStack(stencilSpaceIBounds);
        GrClipData newClipData; // origin defaults to (0,0)
        newClipData.fClipStack = &newClipStack;

        fGpu->setClip(&newClipData);

        GrDrawTarget::AutoStateRestore asr(fGpu, GrDrawTarget::kReset_ASRInit);
        drawState = fGpu->drawState();
        drawState->setRenderTarget(rt);
        GrDrawTarget::AutoGeometryPush agp(fGpu);

        // Set the matrix so that rendered clip elements are transformed from clip to stencil space.
        SkVector translate = {
            SkIntToScalar(clipSpaceToStencilOffset.fX),
            SkIntToScalar(clipSpaceToStencilOffset.fY)
        };
        drawState->viewMatrix()->setTranslate(translate);

#if !VISUALIZE_COMPLEX_CLIP
        drawState->enableState(GrDrawState::kNoColorWrites_StateBit);
#endif

        int clipBit = stencilBuffer->bits();
        SkASSERT((clipBit <= 16) && "Ganesh only handles 16b or smaller stencil buffers");
        clipBit = (1 << (clipBit-1));

        GrIRect devRTRect = GrIRect::MakeWH(rt->width(), rt->height());

        fGpu->clearStencilClip(stencilSpaceIBounds, kAllIn_InitialState == initialState);

        // walk through each clip element and perform its set op
        // with the existing clip.
        for (ElementList::Iter iter(elements.headIter()); NULL != iter.get(); iter.next()) {
            const Element* element = iter.get();
            SkPath::FillType fill;
            bool fillInverted = false;
            // enabled at bottom of loop
            drawState->disableState(GrGpu::kModifyStencilClip_StateBit);
            // if the target is MSAA then we want MSAA enabled when the clip is soft
            if (rt->isMultisampled()) {
                drawState->setState(GrDrawState::kHWAntialias_StateBit, element->isAA());
            }

            // Can the clip element be drawn directly to the stencil buffer
            // with a non-inverted fill rule without extra passes to
            // resolve in/out status?
            bool canRenderDirectToStencil = false;

            SkStroke stroke;
            stroke.setDoFill(true);

            SkRegion::Op op = element->getOp();

            GrPathRenderer* pr = NULL;
            SkPath clipPath;
            if (Element::kRect_Type == element->getType()) {
                canRenderDirectToStencil = true;
                fill = SkPath::kEvenOdd_FillType;
                fillInverted = false;
            } else {
                GrAssert(Element::kPath_Type == element->getType());
                clipPath = element->getPath();
                fill = clipPath.getFillType();
                fillInverted = clipPath.isInverseFillType();
                fill = SkPath::NonInverseFill(fill);
                clipPath.setFillType(fill);
                pr = this->getContext()->getPathRenderer(clipPath, stroke, fGpu, false, true);
                if (NULL == pr) {
                    fGpu->setClip(oldClipData);
                    return false;
                }
                canRenderDirectToStencil = !pr->requiresStencilPass(clipPath, stroke, fGpu);
            }

            int passes;
            GrStencilSettings stencilSettings[GrStencilSettings::kMaxStencilClipPasses];

            bool canDrawDirectToClip; // Given the renderer, the element,
                                      // fill rule, and set operation can
                                      // we render the element directly to
                                      // stencil bit used for clipping.
            canDrawDirectToClip = GrStencilSettings::GetClipPasses(op,
                                                                   canRenderDirectToStencil,
                                                                   clipBit,
                                                                   fillInverted,
                                                                   &passes,
                                                                   stencilSettings);

            // draw the element to the client stencil bits if necessary
            if (!canDrawDirectToClip) {
                GR_STATIC_CONST_SAME_STENCIL(gDrawToStencil,
                                             kIncClamp_StencilOp,
                                             kIncClamp_StencilOp,
                                             kAlways_StencilFunc,
                                             0xffff,
                                             0x0000,
                                             0xffff);
                SET_RANDOM_COLOR
                if (Element::kRect_Type == element->getType()) {
                    *drawState->stencil() = gDrawToStencil;
                    fGpu->drawSimpleRect(element->getRect(), NULL);
                } else {
                    GrAssert(Element::kPath_Type == element->getType());
                    if (canRenderDirectToStencil) {
                        *drawState->stencil() = gDrawToStencil;
                        pr->drawPath(clipPath, stroke, fGpu, false);
                    } else {
                        pr->drawPathToStencil(clipPath, stroke, fGpu);
                    }
                }
            }

            // now we modify the clip bit by rendering either the clip
            // element directly or a bounding rect of the entire clip.
            drawState->enableState(GrGpu::kModifyStencilClip_StateBit);
            for (int p = 0; p < passes; ++p) {
                *drawState->stencil() = stencilSettings[p];
                if (canDrawDirectToClip) {
                    if (Element::kRect_Type == element->getType()) {
                        SET_RANDOM_COLOR
                        fGpu->drawSimpleRect(element->getRect(), NULL);
                    } else {
                        GrAssert(Element::kPath_Type == element->getType());
                        SET_RANDOM_COLOR
                        pr->drawPath(clipPath, stroke, fGpu, false);
                    }
                } else {
                    SET_RANDOM_COLOR
                    // The view matrix is setup to do clip space -> stencil space translation, so
                    // draw rect in clip space.
                    fGpu->drawSimpleRect(SkRect::MakeFromIRect(clipSpaceIBounds), NULL);
                }
            }
        }
        // restore clip
        fGpu->setClip(oldClipData);
    }
    // set this last because recursive draws may overwrite it back to kNone.
    GrAssert(kNone_ClipMaskType == fCurrClipMaskType);
    fCurrClipMaskType = kStencil_ClipMaskType;
    return true;
}


// mapping of clip-respecting stencil funcs to normal stencil funcs
// mapping depends on whether stencil-clipping is in effect.
static const GrStencilFunc
    gSpecialToBasicStencilFunc[2][kClipStencilFuncCount] = {
    {// Stencil-Clipping is DISABLED,  we are effectively always inside the clip
        // In the Clip Funcs
        kAlways_StencilFunc,          // kAlwaysIfInClip_StencilFunc
        kEqual_StencilFunc,           // kEqualIfInClip_StencilFunc
        kLess_StencilFunc,            // kLessIfInClip_StencilFunc
        kLEqual_StencilFunc,          // kLEqualIfInClip_StencilFunc
        // Special in the clip func that forces user's ref to be 0.
        kNotEqual_StencilFunc,        // kNonZeroIfInClip_StencilFunc
                                      // make ref 0 and do normal nequal.
    },
    {// Stencil-Clipping is ENABLED
        // In the Clip Funcs
        kEqual_StencilFunc,           // kAlwaysIfInClip_StencilFunc
                                      // eq stencil clip bit, mask
                                      // out user bits.

        kEqual_StencilFunc,           // kEqualIfInClip_StencilFunc
                                      // add stencil bit to mask and ref

        kLess_StencilFunc,            // kLessIfInClip_StencilFunc
        kLEqual_StencilFunc,          // kLEqualIfInClip_StencilFunc
                                      // for both of these we can add
                                      // the clip bit to the mask and
                                      // ref and compare as normal
        // Special in the clip func that forces user's ref to be 0.
        kLess_StencilFunc,            // kNonZeroIfInClip_StencilFunc
                                      // make ref have only the clip bit set
                                      // and make comparison be less
                                      // 10..0 < 1..user_bits..
    }
};

namespace {
// Sets the settings to clip against the stencil buffer clip while ignoring the
// client bits.
const GrStencilSettings& basic_apply_stencil_clip_settings() {
    // stencil settings to use when clip is in stencil
    GR_STATIC_CONST_SAME_STENCIL_STRUCT(gSettings,
        kKeep_StencilOp,
        kKeep_StencilOp,
        kAlwaysIfInClip_StencilFunc,
        0x0000,
        0x0000,
        0x0000);
    return *GR_CONST_STENCIL_SETTINGS_PTR_FROM_STRUCT_PTR(&gSettings);
}
}

void GrClipMaskManager::setGpuStencil() {
    // We make two copies of the StencilSettings here (except in the early
    // exit scenario. One copy from draw state to the stack var. Then another
    // from the stack var to the gpu. We could make this class hold a ptr to
    // GrGpu's fStencilSettings and eliminate the stack copy here.

    const GrDrawState& drawState = fGpu->getDrawState();

    // use stencil for clipping if clipping is enabled and the clip
    // has been written into the stencil.
    GrClipMaskManager::StencilClipMode clipMode;
    if (this->isClipInStencil() && drawState.isClipState()) {
        clipMode = GrClipMaskManager::kRespectClip_StencilClipMode;
        // We can't be modifying the clip and respecting it at the same time.
        GrAssert(!drawState.isStateFlagEnabled(
                    GrGpu::kModifyStencilClip_StateBit));
    } else if (drawState.isStateFlagEnabled(
                    GrGpu::kModifyStencilClip_StateBit)) {
        clipMode = GrClipMaskManager::kModifyClip_StencilClipMode;
    } else {
        clipMode = GrClipMaskManager::kIgnoreClip_StencilClipMode;
    }

    GrStencilSettings settings;
    // The GrGpu client may not be using the stencil buffer but we may need to
    // enable it in order to respect a stencil clip.
    if (drawState.getStencil().isDisabled()) {
        if (GrClipMaskManager::kRespectClip_StencilClipMode == clipMode) {
            settings = basic_apply_stencil_clip_settings();
        } else {
            fGpu->disableStencil();
            return;
        }
    } else {
        settings = drawState.getStencil();
    }

    // TODO: dynamically attach a stencil buffer
    int stencilBits = 0;
    GrStencilBuffer* stencilBuffer =
        drawState.getRenderTarget()->getStencilBuffer();
    if (NULL != stencilBuffer) {
        stencilBits = stencilBuffer->bits();
    }

    GrAssert(fGpu->getCaps().stencilWrapOpsSupport() || !settings.usesWrapOp());
    GrAssert(fGpu->getCaps().twoSidedStencilSupport() || !settings.isTwoSided());
    this->adjustStencilParams(&settings, clipMode, stencilBits);
    fGpu->setStencilSettings(settings);
}

void GrClipMaskManager::adjustStencilParams(GrStencilSettings* settings,
                                            StencilClipMode mode,
                                            int stencilBitCnt) {
    GrAssert(stencilBitCnt > 0);

    if (kModifyClip_StencilClipMode == mode) {
        // We assume that this clip manager itself is drawing to the GrGpu and
        // has already setup the correct values.
        return;
    }

    unsigned int clipBit = (1 << (stencilBitCnt - 1));
    unsigned int userBits = clipBit - 1;

    GrStencilSettings::Face face = GrStencilSettings::kFront_Face;
    bool twoSided = fGpu->getCaps().twoSidedStencilSupport();

    bool finished = false;
    while (!finished) {
        GrStencilFunc func = settings->func(face);
        uint16_t writeMask = settings->writeMask(face);
        uint16_t funcMask = settings->funcMask(face);
        uint16_t funcRef = settings->funcRef(face);

        GrAssert((unsigned) func < kStencilFuncCount);

        writeMask &= userBits;

        if (func >= kBasicStencilFuncCount) {
            int respectClip = kRespectClip_StencilClipMode == mode;
            if (respectClip) {
                // The GrGpu class should have checked this
                GrAssert(this->isClipInStencil());
                switch (func) {
                    case kAlwaysIfInClip_StencilFunc:
                        funcMask = clipBit;
                        funcRef = clipBit;
                        break;
                    case kEqualIfInClip_StencilFunc:
                    case kLessIfInClip_StencilFunc:
                    case kLEqualIfInClip_StencilFunc:
                        funcMask = (funcMask & userBits) | clipBit;
                        funcRef  = (funcRef  & userBits) | clipBit;
                        break;
                    case kNonZeroIfInClip_StencilFunc:
                        funcMask = (funcMask & userBits) | clipBit;
                        funcRef = clipBit;
                        break;
                    default:
                        GrCrash("Unknown stencil func");
                }
            } else {
                funcMask &= userBits;
                funcRef &= userBits;
            }
            const GrStencilFunc* table =
                gSpecialToBasicStencilFunc[respectClip];
            func = table[func - kBasicStencilFuncCount];
            GrAssert(func >= 0 && func < kBasicStencilFuncCount);
        } else {
            funcMask &= userBits;
            funcRef &= userBits;
        }

        settings->setFunc(face, func);
        settings->setWriteMask(face, writeMask);
        settings->setFuncMask(face, funcMask);
        settings->setFuncRef(face, funcRef);

        if (GrStencilSettings::kFront_Face == face) {
            face = GrStencilSettings::kBack_Face;
            finished = !twoSided;
        } else {
            finished = true;
        }
    }
    if (!twoSided) {
        settings->copyFrontSettingsToBack();
    }
}

////////////////////////////////////////////////////////////////////////////////
GrTexture* GrClipMaskManager::createSoftwareClipMask(int32_t clipStackGenID,
                                                     GrReducedClip::InitialState initialState,
                                                     const GrReducedClip::ElementList& elements,
                                                     const SkIRect& clipSpaceIBounds) {
    GrAssert(kNone_ClipMaskType == fCurrClipMaskType);

    GrTexture* result;
    if (this->getMaskTexture(clipStackGenID, clipSpaceIBounds, &result)) {
        return result;
    }

    if (NULL == result) {
        fAACache.reset();
        return NULL;
    }

    // The mask texture may be larger than necessary. We round out the clip space bounds and pin
    // the top left corner of the resulting rect to the top left of the texture.
    SkIRect maskSpaceIBounds = SkIRect::MakeWH(clipSpaceIBounds.width(), clipSpaceIBounds.height());

    GrSWMaskHelper helper(this->getContext());

    SkMatrix matrix;
    matrix.setTranslate(SkIntToScalar(-clipSpaceIBounds.fLeft),
                        SkIntToScalar(-clipSpaceIBounds.fTop));
    helper.init(maskSpaceIBounds, &matrix);

    helper.clear(kAllIn_InitialState == initialState ? 0xFF : 0x00);

    SkStroke stroke;
    stroke.setDoFill(true);

    for (ElementList::Iter iter(elements.headIter()) ; NULL != iter.get(); iter.next()) {

        const Element* element = iter.get();
        SkRegion::Op op = element->getOp();

        if (SkRegion::kIntersect_Op == op || SkRegion::kReverseDifference_Op == op) {
            // Intersect and reverse difference require modifying pixels outside of the geometry
            // that is being "drawn". In both cases we erase all the pixels outside of the geometry
            // but leave the pixels inside the geometry alone. For reverse difference we invert all
            // the pixels before clearing the ones outside the geometry.
            if (SkRegion::kReverseDifference_Op == op) {
                SkRect temp = SkRect::MakeFromIRect(clipSpaceIBounds);
                // invert the entire scene
                helper.draw(temp, SkRegion::kXOR_Op, false, 0xFF);
            }

            if (Element::kRect_Type == element->getType()) {
                // convert the rect to a path so we can invert the fill
                SkPath temp;
                temp.addRect(element->getRect());
                temp.setFillType(SkPath::kInverseEvenOdd_FillType);

                helper.draw(temp, stroke, SkRegion::kReplace_Op,
                            element->isAA(),
                            0x00);
            } else {
                GrAssert(Element::kPath_Type == element->getType());
                SkPath clipPath = element->getPath();
                clipPath.toggleInverseFillType();
                helper.draw(clipPath, stroke,
                            SkRegion::kReplace_Op,
                            element->isAA(),
                            0x00);
            }

            continue;
        }

        // The other ops (union, xor, diff) only affect pixels inside
        // the geometry so they can just be drawn normally
        if (Element::kRect_Type == element->getType()) {
            helper.draw(element->getRect(), op, element->isAA(), 0xFF);
        } else {
            GrAssert(Element::kPath_Type == element->getType());
            helper.draw(element->getPath(), stroke, op, element->isAA(), 0xFF);
        }
    }

    helper.toTexture(result, kAllIn_InitialState == initialState ? 0xFF : 0x00);

    fCurrClipMaskType = kAlpha_ClipMaskType;
    return result;
}

////////////////////////////////////////////////////////////////////////////////
void GrClipMaskManager::releaseResources() {
    fAACache.releaseResources();
}

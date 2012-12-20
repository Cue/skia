
/*
 * Copyright 2011 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */


#include "GrTexture.h"

#include "GrContext.h"
#include "GrGpu.h"
#include "GrRenderTarget.h"
#include "GrResourceCache.h"

SK_DEFINE_INST_COUNT(GrTexture)

/**
 * This method allows us to interrupt the normal deletion process and place
 * textures back in the texture cache when their ref count goes to zero.
 */
void GrTexture::internal_dispose() const {

    if (this->isSetFlag((GrTextureFlags) kReturnToCache_FlagBit) &&
        NULL != this->INHERITED::getContext()) {
        GrTexture* nonConstThis = const_cast<GrTexture *>(this);
        this->fRefCnt = 1;      // restore ref count to initial setting

        nonConstThis->resetFlag((GrTextureFlags) kReturnToCache_FlagBit);
        nonConstThis->INHERITED::getContext()->addExistingTextureToCache(nonConstThis);

        // Note: "this" texture might be freed inside addExistingTextureToCache
        // if it is purged.
        return;
    }

    this->INHERITED::internal_dispose();
}

bool GrTexture::readPixels(int left, int top, int width, int height,
                           GrPixelConfig config, void* buffer,
                           size_t rowBytes, uint32_t pixelOpsFlags) {
    // go through context so that all necessary flushing occurs
    GrContext* context = this->getContext();
    if (NULL == context) {
        return false;
    }
    return context->readTexturePixels(this,
                                      left, top, width, height,
                                      config, buffer, rowBytes,
                                      pixelOpsFlags);
}

void GrTexture::writePixels(int left, int top, int width, int height,
                            GrPixelConfig config, const void* buffer,
                            size_t rowBytes, uint32_t pixelOpsFlags) {
    // go through context so that all necessary flushing occurs
    GrContext* context = this->getContext();
    if (NULL == context) {
        return;
    }
    context->writeTexturePixels(this,
                                left, top, width, height,
                                config, buffer, rowBytes,
                                pixelOpsFlags);
}

void GrTexture::releaseRenderTarget() {
    if (NULL != fRenderTarget) {
        GrAssert(fRenderTarget->asTexture() == this);
        GrAssert(fDesc.fFlags & kRenderTarget_GrTextureFlagBit);

        fRenderTarget->onTextureReleaseRenderTarget();
        fRenderTarget->unref();
        fRenderTarget = NULL;

        fDesc.fFlags = fDesc.fFlags &
            ~(kRenderTarget_GrTextureFlagBit|kNoStencil_GrTextureFlagBit);
        fDesc.fSampleCnt = 0;
    }
}

void GrTexture::onRelease() {
    GrAssert(!this->isSetFlag((GrTextureFlags) kReturnToCache_FlagBit));
    this->releaseRenderTarget();

    INHERITED::onRelease();
}

void GrTexture::onAbandon() {
    if (NULL != fRenderTarget) {
        fRenderTarget->abandon();
    }

    INHERITED::onAbandon();
}

void GrTexture::validateDesc() const {
    if (NULL != this->asRenderTarget()) {
        // This texture has a render target
        GrAssert(0 != (fDesc.fFlags & kRenderTarget_GrTextureFlagBit));

        if (NULL != this->asRenderTarget()->getStencilBuffer()) {
            GrAssert(0 != (fDesc.fFlags & kNoStencil_GrTextureFlagBit));
        } else {
            GrAssert(0 == (fDesc.fFlags & kNoStencil_GrTextureFlagBit));
        }

        GrAssert(fDesc.fSampleCnt == this->asRenderTarget()->numSamples());
    } else {
        GrAssert(0 == (fDesc.fFlags & kRenderTarget_GrTextureFlagBit));
        GrAssert(0 == (fDesc.fFlags & kNoStencil_GrTextureFlagBit));
        GrAssert(0 == fDesc.fSampleCnt);
    }
}

// These flags need to fit in a GrResourceKey::ResourceFlags so they can be folded into the texture
// key
enum TextureFlags {
    /**
     * The kStretchToPOT bit is set when the texture is NPOT and is being repeated but the
     * hardware doesn't support that feature.
     */
    kStretchToPOT_TextureFlag = 0x1,
    /**
     * The kFilter bit can only be set when the kStretchToPOT flag is set and indicates whether the
     * stretched texture should be bilerp filtered or point sampled. 
     */
    kFilter_TextureFlag       = 0x2,
};

namespace {
GrResourceKey::ResourceFlags get_texture_flags(const GrGpu* gpu,
                                               const GrTextureParams* params,
                                               const GrTextureDesc& desc) {
    GrResourceKey::ResourceFlags flags = 0;
    bool tiled = NULL != params && params->isTiled();
    if (tiled & !gpu->getCaps().npotTextureTileSupport()) {
        if (!GrIsPow2(desc.fWidth) || GrIsPow2(desc.fHeight)) {
            flags |= kStretchToPOT_TextureFlag;
            if (params->isBilerp()) {
                flags |= kFilter_TextureFlag;
            }
        }
    }
    return flags;
}

GrResourceKey::ResourceType texture_resource_type() {
    static const GrResourceKey::ResourceType gType = GrResourceKey::GenerateResourceType();
    return gType;
}
}

GrResourceKey GrTexture::ComputeKey(const GrGpu* gpu,
                                    const GrTextureParams* params,
                                    const GrTextureDesc& desc,
                                    const GrCacheID& cacheID) {
    GrResourceKey::ResourceFlags flags = get_texture_flags(gpu, params, desc);
    return GrResourceKey(cacheID, texture_resource_type(), flags);
}

GrResourceKey GrTexture::ComputeScratchKey(const GrTextureDesc& desc) {
    GrCacheID::Key idKey;
    // Instead of a client-provided key of the texture contents we create a key from the
    // descriptor.
    GR_STATIC_ASSERT(sizeof(idKey) >= 12);
    GrAssert(desc.fHeight < (1 << 16));
    GrAssert(desc.fWidth < (1 << 16));
    idKey.fData32[0] = (desc.fWidth) | (desc.fHeight << 16);
    idKey.fData32[1] = desc.fConfig | desc.fSampleCnt << 16;
    idKey.fData32[2] = desc.fFlags;
    static const int kPadSize = sizeof(idKey) - 12;
    memset(idKey.fData8 + 12, 0, kPadSize);

    GrCacheID cacheID(GrResourceKey::ScratchDomain(), idKey);
    return GrResourceKey(cacheID, texture_resource_type(), 0);
}

bool GrTexture::NeedsResizing(const GrResourceKey& key) {
    return SkToBool(key.getResourceFlags() & kStretchToPOT_TextureFlag);
}

bool GrTexture::NeedsFiltering(const GrResourceKey& key) {
    return SkToBool(key.getResourceFlags() & kFilter_TextureFlag);
}

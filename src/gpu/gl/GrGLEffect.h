/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef GrGLEffect_DEFINED
#define GrGLEffect_DEFINED

#include "GrBackendEffectFactory.h"
#include "GrGLProgram.h"
#include "GrGLShaderBuilder.h"
#include "GrGLShaderVar.h"
#include "GrGLSL.h"

class GrEffectStage;
class GrGLTexture;

/** @file
    This file contains specializations for OpenGL of the shader stages declared in
    include/gpu/GrEffect.h. Objects of type GrGLEffect are responsible for emitting the
    GLSL code that implements a GrEffect and for uploading uniforms at draw time. They also
    must have a function:
        static inline EffectKey GenKey(const GrEffectStage&, const GrGLCaps&)
    that is used to implement a program cache. When two GrEffects produce the same key this means
    that their GrGLEffects would emit the same GLSL code.

    These objects are created by the factory object returned by the GrEffect::getFactory().
*/

class GrGLEffect {

public:
    typedef GrBackendEffectFactory::EffectKey EffectKey;

    enum {
        // the number of bits in EffectKey available to GenKey
        kEffectKeyBits = GrBackendEffectFactory::kEffectKeyBits,
    };

    typedef GrGLShaderBuilder::TextureSamplerArray TextureSamplerArray;

    GrGLEffect(const GrBackendEffectFactory&);

    virtual ~GrGLEffect();

    /** Called when the program stage should insert its code into the shaders. The code in each
        shader will be in its own block ({}) and so locally scoped names will not collide across
        stages.

        @param builder      Interface used to emit code in the shaders.
        @param stage        The effect stage that generated this program stage.
        @param key          The key that was computed by GenKey() from the generating GrEffect.
                            Only the bits indicated by GrBackendEffectFactory::kEffectKeyBits are
                            guaranteed to match the value produced by GenKey();
        @param vertexCoords A vec2 of texture coordinates in the VS, which may be altered. This will
                            be removed soon and stages will be responsible for computing their own
                            coords.
        @param outputColor  A predefined vec4 in the FS in which the stage should place its output
                            color (or coverage).
        @param inputColor   A vec4 that holds the input color to the stage in the FS. This may be
                            NULL in which case the implied input is solid white (all ones).
                            TODO: Better system for communicating optimization info (e.g. input
                            color is solid white, trans black, known to be opaque, etc.) that allows
                            the effect to communicate back similar known info about its output.
        @param samplers     One entry for each GrTextureAccess of the GrEffect that generated the
                            GrGLEffect. These can be passed to the builder to emit texture
                            reads in the generated code.
        */
    virtual void emitCode(GrGLShaderBuilder* builder,
                          const GrEffectStage& stage,
                          EffectKey key,
                          const char* vertexCoords,
                          const char* outputColor,
                          const char* inputColor,
                          const TextureSamplerArray& samplers) = 0;

    /** A GrGLEffect instance can be reused with any GrEffect that produces the same stage
        key; this function reads data from a stage and uploads any uniform variables required
        by the shaders created in emitCode(). The GrEffect installed in the GrEffectStage is
        guaranteed to be of the same type that created this GrGLEffect and to have an identical
        EffectKey as the one that created this GrGLEffect. */
    virtual void setData(const GrGLUniformManager&, const GrEffectStage&);

    const char* name() const { return fFactory.name(); }

    static EffectKey GenTextureKey(const GrEffect&, const GrGLCaps&);

    bool requiresTextureMatrix() const { return fRequiresTextureMatrix; }


protected:
    // HACK: This is a temporary field that allows GrGLEffect subclasses to opt into the new
    // shader gen where a texture matrix is not automatically inserted. It defaults to true and is
    // set to false in a subclass to opt into the new behavior.
    bool fRequiresTextureMatrix;

    const GrBackendEffectFactory& fFactory;
};

#endif

/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef GrEffect_DEFINED
#define GrEffect_DEFINED

#include "GrRefCnt.h"
#include "GrNoncopyable.h"
#include "GrEffectUnitTest.h"
#include "GrTexture.h"
#include "GrTextureAccess.h"

class GrBackendEffectFactory;
class GrContext;
class SkString;

/** Provides custom vertex shader, fragment shader, uniform data for a
    particular stage of the Ganesh shading pipeline.
    Subclasses must have a function that produces a human-readable name:
        static const char* Name();
    GrEffect objects *must* be immutable: after being constructed,
    their fields may not change.  (Immutability isn't actually required
    until they've been used in a draw call, but supporting that would require
    setters and getters that could fail, copy-on-write, or deep copying of these
    objects when they're stored by a GrGLEffect.)
  */
class GrEffect : public GrRefCnt {

public:
    SK_DECLARE_INST_COUNT(GrEffect)

    explicit GrEffect(int numTextures);
    virtual ~GrEffect();

    /** If given an input texture that is/is not opaque, is this
        effect guaranteed to produce an opaque output? */
    virtual bool isOpaque(bool inputTextureIsOpaque) const;

    /** This object, besides creating back-end-specific helper objects, is used for run-time-type-
        identification. The factory should be an instance of templated class,
        GrTBackendEffectFactory. It is templated on the subclass of GrEffect. The subclass must have
        a nested type (or typedef) named GLEffect which will be the subclass of GrGLEffect created
        by the factory.

        Example:
        class MyCustomEffect : public GrEffect {
        ...
            virtual const GrBackendEffectFactory& getFactory() const SK_OVERRIDE {
                return GrTBackendEffectFactory<MyCustomEffect>::getInstance();
            }
        ...
        };
     */
    virtual const GrBackendEffectFactory& getFactory() const = 0;

    /** Returns true if the other effect will generate identical output.
        Must only be called if the two are already known to be of the
        same type (i.e.  they return the same value from getFactory()).

        Equality is not the same thing as equivalence.
        To test for equivalence (that they will generate the same
        shader code, but may have different uniforms), check equality
        of the EffectKey produced by the GrBackendEffectFactory:
        a.getFactory().glEffectKey(a) == b.getFactory().glEffectKey(b).

        The default implementation of this function returns true iff
        the two stages have the same return value for numTextures() and
        for texture() over all valid indices.
     */
    virtual bool isEqual(const GrEffect&) const;

    /** Human-meaningful string to identify this effect; may be embedded
        in generated shader code. */
    const char* name() const;

    int numTextures() const { return fNumTextures; }

    /** Returns the access pattern for the texture at index. index must be valid according to
        numTextures(). */
    virtual const GrTextureAccess& textureAccess(int index) const;

    /** Shortcut for textureAccess(index).texture(); */
    GrTexture* texture(int index) const { return this->textureAccess(index).getTexture(); }

    /** Useful for effects that want to insert a texture matrix that is implied by the texture
        dimensions */
    static inline SkMatrix MakeDivByTextureWHMatrix(const GrTexture* texture) {
        GrAssert(NULL != texture);
        SkMatrix mat;
        mat.setIDiv(texture->width(), texture->height());
        return mat;
    }

    void* operator new(size_t size);
    void operator delete(void* target);

private:
    int fNumTextures;
    typedef GrRefCnt INHERITED;
};

#endif

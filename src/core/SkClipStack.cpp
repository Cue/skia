
/*
 * Copyright 2011 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "SkClipStack.h"
#include "SkPath.h"
#include "SkThread.h"

#include <new>


// 0-2 are reserved for invalid, empty & wide-open
static const int32_t kFirstUnreservedGenID = 3;
int32_t SkClipStack::gGenID = kFirstUnreservedGenID;

struct SkClipStack::Element {
    enum Type {
        //!< This element makes the clip empty (regardless of previous elements).
        kEmpty_Type,
        //!< This element combines a rect with the current clip using a set operation
        kRect_Type,
        //!< This element combines a path with the current clip using a set operation
        kPath_Type,
    };

    SkPath          fPath;
    SkRect          fRect;
    int             fSaveCount;
    SkRegion::Op    fOp;
    Type            fType;
    bool            fDoAA;

    // fFiniteBoundType and fFiniteBound are used to incrementally update
    // the clip stack's bound. When fFiniteBoundType is kNormal_BoundsType,
    // fFiniteBound represents the  conservative bounding box of the pixels
    // that aren't clipped (i.e., any pixels that can be drawn to are inside
    // the bound). When fFiniteBoundType is kInsideOut_BoundsType (which occurs
    // when a clip is inverse filled), fFiniteBound represents the
    // conservative bounding box of the pixels that _are_ clipped (i.e., any
    // pixels that cannot be drawn to are inside the bound). When
    // fFiniteBoundType is kInsideOut_BoundsType the actual bound is
    // the infinite plane. This behavior of fFiniteBoundType and
    // fFiniteBound is required so that we can capture the cancelling out
    // of the extensions to infinity when two inverse filled clips are
    // Booleaned together.
    SkClipStack::BoundsType fFiniteBoundType;
    SkRect                  fFiniteBound;
    bool                    fIsIntersectionOfRects;

    int                     fGenID;

    Element(int saveCount)
    : fGenID(kInvalidGenID) {
        fSaveCount = saveCount;
        this->setEmpty();
    }

    Element(int saveCount, const SkRect& rect, SkRegion::Op op, bool doAA)
        : fRect(rect)
        , fGenID(kInvalidGenID) {
        fSaveCount = saveCount;
        fOp = op;
        fType = kRect_Type;
        fDoAA = doAA;
        // bounding box members are updated in a following updateBoundAndGenID call
    }

    Element(int saveCount, const SkPath& path, SkRegion::Op op, bool doAA)
        : fPath(path)
        , fGenID(kInvalidGenID) {
        fRect.setEmpty();
        fSaveCount = saveCount;
        fOp = op;
        fType = kPath_Type;
        fDoAA = doAA;
        // bounding box members are updated in a following updateBoundAndGenID call
    }

    void setEmpty() {
        fType = kEmpty_Type;
        fFiniteBound.setEmpty();
        fFiniteBoundType = kNormal_BoundsType;
        fIsIntersectionOfRects = false;
        fGenID = kEmptyGenID;
    }

    void checkEmpty() const {
        SkASSERT(fFiniteBound.isEmpty());
        SkASSERT(kNormal_BoundsType == fFiniteBoundType);
        SkASSERT(!fIsIntersectionOfRects);
        SkASSERT(kEmptyGenID == fGenID);
    }

    bool operator==(const Element& b) const {
        if (fSaveCount != b.fSaveCount ||
            fOp != b.fOp ||
            fType != b.fType ||
            fDoAA != b.fDoAA) {
            return false;
        }
        switch (fType) {
            case kEmpty_Type:
                return true;
            case kRect_Type:
                return fRect == b.fRect;
            case kPath_Type:
                return fPath == b.fPath;
        }
        return false;  // Silence the compiler.
    }

    bool operator!=(const Element& b) const {
        return !(*this == b);
    }


    /**
     *  Returns true if this Element can be intersected in place with a new clip
     */
    bool canBeIntersectedInPlace(int saveCount, SkRegion::Op op) const {
        if (kEmpty_Type == fType && (
                    SkRegion::kDifference_Op == op ||
                    SkRegion::kIntersect_Op == op)) {
            return true;
        }
        // Only clips within the same save/restore frame (as captured by
        // the save count) can be merged
        return  fSaveCount == saveCount &&
                SkRegion::kIntersect_Op == op &&
                (SkRegion::kIntersect_Op == fOp || SkRegion::kReplace_Op == fOp);
    }

    /**
     * This method checks to see if two rect clips can be safely merged into
     * one. The issue here is that to be strictly correct all the edges of
     * the resulting rect must have the same anti-aliasing.
     */
    bool rectRectIntersectAllowed(const SkRect& newR, bool newAA) const {
        SkASSERT(kRect_Type == fType);

        if (fDoAA == newAA) {
            // if the AA setting is the same there is no issue
            return true;
        }

        if (!SkRect::Intersects(fRect, newR)) {
            // The calling code will correctly set the result to the empty clip
            return true;
        }

        if (fRect.contains(newR)) {
            // if the new rect carves out a portion of the old one there is no
            // issue
            return true;
        }

        // So either the two overlap in some complex manner or newR contains oldR.
        // In the first, case the edges will require different AA. In the second,
        // the AA setting that would be carried forward is incorrect (e.g., oldR
        // is AA while newR is BW but since newR contains oldR, oldR will be
        // drawn BW) since the new AA setting will predominate.
        return false;
    }


    /**
     * The different combination of fill & inverse fill when combining
     * bounding boxes
     */
    enum FillCombo {
        kPrev_Cur_FillCombo,
        kPrev_InvCur_FillCombo,
        kInvPrev_Cur_FillCombo,
        kInvPrev_InvCur_FillCombo
    };

    // a mirror of combineBoundsRevDiff
    void combineBoundsDiff(FillCombo combination, const SkRect& prevFinite) {
        switch (combination) {
            case kInvPrev_InvCur_FillCombo:
                // In this case the only pixels that can remain set
                // are inside the current clip rect since the extensions
                // to infinity of both clips cancel out and whatever
                // is outside of the current clip is removed
                fFiniteBoundType = kNormal_BoundsType;
                break;
            case kInvPrev_Cur_FillCombo:
                // In this case the current op is finite so the only pixels
                // that aren't set are whatever isn't set in the previous
                // clip and whatever this clip carves out
                fFiniteBound.join(prevFinite);
                fFiniteBoundType = kInsideOut_BoundsType;
                break;
            case kPrev_InvCur_FillCombo:
                // In this case everything outside of this clip's bound
                // is erased, so the only pixels that can remain set
                // occur w/in the intersection of the two finite bounds
                if (!fFiniteBound.intersect(prevFinite)) {
                    fFiniteBound.setEmpty();
                    fGenID = kEmptyGenID;
                }
                fFiniteBoundType = kNormal_BoundsType;
                break;
            case kPrev_Cur_FillCombo:
                // The most conservative result bound is that of the
                // prior clip. This could be wildly incorrect if the
                // second clip either exactly matches the first clip
                // (which should yield the empty set) or reduces the
                // size of the prior bound (e.g., if the second clip
                // exactly matched the bottom half of the prior clip).
                // We ignore these two possibilities.
                fFiniteBound = prevFinite;
                break;
            default:
                SkDEBUGFAIL("SkClipStack::Element::combineBoundsDiff Invalid fill combination");
                break;
        }
    }

    void combineBoundsXOR(int combination, const SkRect& prevFinite) {

        switch (combination) {
            case kInvPrev_Cur_FillCombo:       // fall through
            case kPrev_InvCur_FillCombo:
                // With only one of the clips inverted the result will always
                // extend to infinity. The only pixels that may be un-writeable
                // lie within the union of the two finite bounds
                fFiniteBound.join(prevFinite);
                fFiniteBoundType = kInsideOut_BoundsType;
                break;
            case kInvPrev_InvCur_FillCombo:
                // The only pixels that can survive are within the
                // union of the two bounding boxes since the extensions
                // to infinity of both clips cancel out
                // fall through!
            case kPrev_Cur_FillCombo:
                // The most conservative bound for xor is the
                // union of the two bounds. If the two clips exactly overlapped
                // the xor could yield the empty set. Similarly the xor
                // could reduce the size of the original clip's bound (e.g.,
                // if the second clip exactly matched the bottom half of the
                // first clip). We ignore these two cases.
                fFiniteBound.join(prevFinite);
                fFiniteBoundType = kNormal_BoundsType;
                break;
            default:
                SkDEBUGFAIL("SkClipStack::Element::combineBoundsXOR Invalid fill combination");
                break;
        }
    }

    // a mirror of combineBoundsIntersection
    void combineBoundsUnion(int combination, const SkRect& prevFinite) {

        switch (combination) {
            case kInvPrev_InvCur_FillCombo:
                if (!fFiniteBound.intersect(prevFinite)) {
                    fFiniteBound.setEmpty();
                    fGenID = kWideOpenGenID;
                }
                fFiniteBoundType = kInsideOut_BoundsType;
                break;
            case kInvPrev_Cur_FillCombo:
                // The only pixels that won't be drawable are inside
                // the prior clip's finite bound
                fFiniteBound = prevFinite;
                fFiniteBoundType = kInsideOut_BoundsType;
                break;
            case kPrev_InvCur_FillCombo:
                // The only pixels that won't be drawable are inside
                // this clip's finite bound
                break;
            case kPrev_Cur_FillCombo:
                fFiniteBound.join(prevFinite);
                break;
            default:
                SkDEBUGFAIL("SkClipStack::Element::combineBoundsUnion Invalid fill combination");
                break;
        }
    }

    // a mirror of combineBoundsUnion
    void combineBoundsIntersection(int combination, const SkRect& prevFinite) {

        switch (combination) {
            case kInvPrev_InvCur_FillCombo:
                // The only pixels that aren't writable in this case
                // occur in the union of the two finite bounds
                fFiniteBound.join(prevFinite);
                fFiniteBoundType = kInsideOut_BoundsType;
                break;
            case kInvPrev_Cur_FillCombo:
                // In this case the only pixels that will remain writeable
                // are within the current clip
                break;
            case kPrev_InvCur_FillCombo:
                // In this case the only pixels that will remain writeable
                // are with the previous clip
                fFiniteBound = prevFinite;
                fFiniteBoundType = kNormal_BoundsType;
                break;
            case kPrev_Cur_FillCombo:
                if (!fFiniteBound.intersect(prevFinite)) {
                    fFiniteBound.setEmpty();
                    fGenID = kEmptyGenID;
                }
                break;
            default:
                SkDEBUGFAIL("SkClipStack::Element::combineBoundsIntersection Invalid fill combination");
                break;
        }
    }

    // a mirror of combineBoundsDiff
    void combineBoundsRevDiff(int combination, const SkRect& prevFinite) {

        switch (combination) {
            case kInvPrev_InvCur_FillCombo:
                // The only pixels that can survive are in the
                // previous bound since the extensions to infinity in
                // both clips cancel out
                fFiniteBound = prevFinite;
                fFiniteBoundType = kNormal_BoundsType;
                break;
            case kInvPrev_Cur_FillCombo:
                if (!fFiniteBound.intersect(prevFinite)) {
                    fFiniteBound.setEmpty();
                    fGenID = kEmptyGenID;
                }
                fFiniteBoundType = kNormal_BoundsType;
                break;
            case kPrev_InvCur_FillCombo:
                fFiniteBound.join(prevFinite);
                fFiniteBoundType = kInsideOut_BoundsType;
                break;
            case kPrev_Cur_FillCombo:
                // Fall through - as with the kDifference_Op case, the
                // most conservative result bound is the bound of the
                // current clip. The prior clip could reduce the size of this
                // bound (as in the kDifference_Op case) but we are ignoring
                // those cases.
                break;
            default:
                SkDEBUGFAIL("SkClipStack::Element::combineBoundsRevDiff Invalid fill combination");
                break;
        }
    }

    void updateBoundAndGenID(const Element* prior) {
        // We set this first here but we may overwrite it later if we determine that the clip is
        // either wide-open or empty.
        fGenID = GetNextGenID();

        // First, optimistically update the current Element's bound information
        // with the current clip's bound
        fIsIntersectionOfRects = false;
        if (kRect_Type == fType) {
            fFiniteBound = fRect;
            fFiniteBoundType = kNormal_BoundsType;

            if (SkRegion::kReplace_Op == fOp ||
                (SkRegion::kIntersect_Op == fOp && NULL == prior) ||
                (SkRegion::kIntersect_Op == fOp && prior->fIsIntersectionOfRects &&
                 prior->rectRectIntersectAllowed(fRect, fDoAA))) {
                fIsIntersectionOfRects = true;
            }

        } else {
            SkASSERT(kPath_Type == fType);

            fFiniteBound = fPath.getBounds();

            if (fPath.isInverseFillType()) {
                fFiniteBoundType = kInsideOut_BoundsType;
            } else {
                fFiniteBoundType = kNormal_BoundsType;
            }
        }

        if (!fDoAA) {
            // Here we mimic a non-anti-aliased scanline system. If there is
            // no anti-aliasing we can integerize the bounding box to exclude
            // fractional parts that won't be rendered.
            // Note: the left edge is handled slightly differently below. We
            // are a bit more generous in the rounding since we don't want to
            // risk missing the left pixels when fLeft is very close to .5
            fFiniteBound.set(SkIntToScalar(SkScalarFloorToInt(fFiniteBound.fLeft+0.45f)),
                             SkIntToScalar(SkScalarRound(fFiniteBound.fTop)),
                             SkIntToScalar(SkScalarRound(fFiniteBound.fRight)),
                             SkIntToScalar(SkScalarRound(fFiniteBound.fBottom)));
        }

        // Now set up the previous Element's bound information taking into
        // account that there may be no previous clip
        SkRect prevFinite;
        SkClipStack::BoundsType prevType;

        if (NULL == prior) {
            // no prior clip means the entire plane is writable
            prevFinite.setEmpty();   // there are no pixels that cannot be drawn to
            prevType = kInsideOut_BoundsType;
        } else {
            prevFinite = prior->fFiniteBound;
            prevType = prior->fFiniteBoundType;
        }

        FillCombo combination = kPrev_Cur_FillCombo;
        if (kInsideOut_BoundsType == fFiniteBoundType) {
            combination = (FillCombo) (combination | 0x01);
        }
        if (kInsideOut_BoundsType == prevType) {
            combination = (FillCombo) (combination | 0x02);
        }

        SkASSERT(kInvPrev_InvCur_FillCombo == combination ||
                 kInvPrev_Cur_FillCombo == combination ||
                 kPrev_InvCur_FillCombo == combination ||
                 kPrev_Cur_FillCombo == combination);

        // Now integrate with clip with the prior clips
        switch (fOp) {
            case SkRegion::kDifference_Op:
                this->combineBoundsDiff(combination, prevFinite);
                break;
            case SkRegion::kXOR_Op:
                this->combineBoundsXOR(combination, prevFinite);
                break;
            case SkRegion::kUnion_Op:
                this->combineBoundsUnion(combination, prevFinite);
                break;
            case SkRegion::kIntersect_Op:
                this->combineBoundsIntersection(combination, prevFinite);
                break;
            case SkRegion::kReverseDifference_Op:
                this->combineBoundsRevDiff(combination, prevFinite);
                break;
            case SkRegion::kReplace_Op:
                // Replace just ignores everything prior
                // The current clip's bound information is already filled in
                // so nothing to do
                break;
            default:
                SkDebugf("SkRegion::Op error/n");
                SkASSERT(0);
                break;
        }
    }
};

// This constant determines how many Element's are allocated together as a block in
// the deque. As such it needs to balance allocating too much memory vs.
// incurring allocation/deallocation thrashing. It should roughly correspond to
// the deepest save/restore stack we expect to see.
static const int kDefaultElementAllocCnt = 8;

SkClipStack::SkClipStack()
    : fDeque(sizeof(Element), kDefaultElementAllocCnt)
    , fSaveCount(0) {
}

SkClipStack::SkClipStack(const SkClipStack& b)
    : fDeque(sizeof(Element), kDefaultElementAllocCnt) {
    *this = b;
}

SkClipStack::SkClipStack(const SkRect& r)
    : fDeque(sizeof(Element), kDefaultElementAllocCnt)
    , fSaveCount(0) {
    if (!r.isEmpty()) {
        this->clipDevRect(r, SkRegion::kReplace_Op, false);
    }
}

SkClipStack::SkClipStack(const SkIRect& r)
    : fDeque(sizeof(Element), kDefaultElementAllocCnt)
    , fSaveCount(0) {
    if (!r.isEmpty()) {
        SkRect temp;
        temp.set(r);
        this->clipDevRect(temp, SkRegion::kReplace_Op, false);
    }
}

SkClipStack::~SkClipStack() {
    reset();
}

SkClipStack& SkClipStack::operator=(const SkClipStack& b) {
    if (this == &b) {
        return *this;
    }
    reset();

    fSaveCount = b.fSaveCount;
    SkDeque::F2BIter recIter(b.fDeque);
    for (const Element* element = (const Element*)recIter.next();
         element != NULL;
         element = (const Element*)recIter.next()) {
        new (fDeque.push_back()) Element(*element);
    }

    return *this;
}

bool SkClipStack::operator==(const SkClipStack& b) const {
    if (fSaveCount != b.fSaveCount ||
        fDeque.count() != b.fDeque.count()) {
        return false;
    }
    SkDeque::F2BIter myIter(fDeque);
    SkDeque::F2BIter bIter(b.fDeque);
    const Element* myElement = (const Element*)myIter.next();
    const Element* bElement = (const Element*)bIter.next();

    while (myElement != NULL && bElement != NULL) {
        if (*myElement != *bElement) {
            return false;
        }
        myElement = (const Element*)myIter.next();
        bElement = (const Element*)bIter.next();
    }
    return myElement == NULL && bElement == NULL;
}

void SkClipStack::reset() {
    // We used a placement new for each object in fDeque, so we're responsible
    // for calling the destructor on each of them as well.
    while (!fDeque.empty()) {
        Element* element = (Element*)fDeque.back();
        element->~Element();
        fDeque.pop_back();
    }

    fSaveCount = 0;
}

void SkClipStack::save() {
    fSaveCount += 1;
}

void SkClipStack::restore() {
    fSaveCount -= 1;
    while (!fDeque.empty()) {
        Element* element = (Element*)fDeque.back();
        if (element->fSaveCount <= fSaveCount) {
            break;
        }
        this->purgeClip(element);
        element->~Element();
        fDeque.pop_back();
    }
}

void SkClipStack::getBounds(SkRect* canvFiniteBound,
                            BoundsType* boundType,
                            bool* isIntersectionOfRects) const {
    SkASSERT(NULL != canvFiniteBound && NULL != boundType);

    Element* element = (Element*)fDeque.back();

    if (NULL == element) {
        // the clip is wide open - the infinite plane w/ no pixels un-writeable
        canvFiniteBound->setEmpty();
        *boundType = kInsideOut_BoundsType;
        if (NULL != isIntersectionOfRects) {
            *isIntersectionOfRects = false;
        }
        return;
    }

    *canvFiniteBound = element->fFiniteBound;
    *boundType = element->fFiniteBoundType;
    if (NULL != isIntersectionOfRects) {
        *isIntersectionOfRects = element->fIsIntersectionOfRects;
    }
}

bool SkClipStack::intersectRectWithClip(SkRect* rect) const {
    SkASSERT(NULL != rect);

    SkRect bounds;
    SkClipStack::BoundsType bt;
    this->getBounds(&bounds, &bt);
    if (bt == SkClipStack::kInsideOut_BoundsType) {
        if (bounds.contains(*rect)) {
            return false;
        } else {
            // If rect's x values are both within bound's x range we
            // could clip here. Same for y. But we don't bother to check.
            return true;
        }
    } else {
        return rect->intersect(bounds);
    }
}

void SkClipStack::clipDevRect(const SkRect& rect, SkRegion::Op op, bool doAA) {

    // Use reverse iterator instead of back because Rect path may need previous
    SkDeque::Iter iter(fDeque, SkDeque::Iter::kBack_IterStart);
    Element* element = (Element*) iter.prev();

    if (element && element->canBeIntersectedInPlace(fSaveCount, op)) {
        switch (element->fType) {
            case Element::kEmpty_Type:
                element->checkEmpty();
                return;
            case Element::kRect_Type:
                if (element->rectRectIntersectAllowed(rect, doAA)) {
                    this->purgeClip(element);
                    if (!element->fRect.intersect(rect)) {
                        element->setEmpty();
                        return;
                    }

                    element->fDoAA = doAA;
                    Element* prev = (Element*) iter.prev();
                    element->updateBoundAndGenID(prev);
                    return;
                }
                break;
            case Element::kPath_Type:
                if (!SkRect::Intersects(element->fPath.getBounds(), rect)) {
                    this->purgeClip(element);
                    element->setEmpty();
                    return;
                }
                break;
        }
    }
    new (fDeque.push_back()) Element(fSaveCount, rect, op, doAA);
    ((Element*) fDeque.back())->updateBoundAndGenID(element);

    if (element && element->fSaveCount == fSaveCount) {
        this->purgeClip(element);
    }
}

void SkClipStack::clipDevPath(const SkPath& path, SkRegion::Op op, bool doAA) {
    SkRect alt;
    if (path.isRect(&alt)) {
        return this->clipDevRect(alt, op, doAA);
    }

    Element* element = (Element*)fDeque.back();
    if (element && element->canBeIntersectedInPlace(fSaveCount, op)) {
        const SkRect& pathBounds = path.getBounds();
        switch (element->fType) {
            case Element::kEmpty_Type:
                element->checkEmpty();
                return;
            case Element::kRect_Type:
                if (!SkRect::Intersects(element->fRect, pathBounds)) {
                    this->purgeClip(element);
                    element->setEmpty();
                    return;
                }
                break;
            case Element::kPath_Type:
                if (!SkRect::Intersects(element->fPath.getBounds(), pathBounds)) {
                    this->purgeClip(element);
                    element->setEmpty();
                    return;
                }
                break;
        }
    }
    new (fDeque.push_back()) Element(fSaveCount, path, op, doAA);
    ((Element*) fDeque.back())->updateBoundAndGenID(element);

    if (element && element->fSaveCount == fSaveCount) {
        this->purgeClip(element);
    }
}

void SkClipStack::clipEmpty() {

    Element* element = (Element*) fDeque.back();

    if (element && element->canBeIntersectedInPlace(fSaveCount, SkRegion::kIntersect_Op)) {
        switch (element->fType) {
            case Element::kEmpty_Type:
                element->checkEmpty();
                return;
            case Element::kRect_Type:
            case Element::kPath_Type:
                this->purgeClip(element);
                element->setEmpty();
                return;
        }
    }
    new (fDeque.push_back()) Element(fSaveCount);

    if (element && element->fSaveCount == fSaveCount) {
        this->purgeClip(element);
    }
    ((Element*)fDeque.back())->fGenID = kEmptyGenID;
}

bool SkClipStack::isWideOpen() const {
    if (0 == fDeque.count()) {
        return true;
    }

    const Element* back = (const Element*) fDeque.back();
    return kWideOpenGenID == back->fGenID ||
           (kInsideOut_BoundsType == back->fFiniteBoundType && back->fFiniteBound.isEmpty());
}

///////////////////////////////////////////////////////////////////////////////

SkClipStack::Iter::Iter() : fStack(NULL) {
}

bool operator==(const SkClipStack::Iter::Clip& a,
                const SkClipStack::Iter::Clip& b) {
    return a.fOp == b.fOp && a.fDoAA == b.fDoAA &&
           ((a.fRect == NULL && b.fRect == NULL) ||
               (a.fRect != NULL && b.fRect != NULL && *a.fRect == *b.fRect)) &&
           ((a.fPath == NULL && b.fPath == NULL) ||
               (a.fPath != NULL && b.fPath != NULL && *a.fPath == *b.fPath));
}

bool operator!=(const SkClipStack::Iter::Clip& a,
                const SkClipStack::Iter::Clip& b) {
    return !(a == b);
}

const SkRect& SkClipStack::Iter::Clip::getBounds() const {
    if (NULL != fRect) {
        return *fRect;
    } else if (NULL != fPath) {
        return fPath->getBounds();
    } else {
        static const SkRect kEmpty = {0, 0, 0, 0};
        return kEmpty;
    }
}

bool SkClipStack::Iter::Clip::contains(const SkRect& rect) const {
    if (NULL != fRect) {
        return fRect->contains(rect);
    } else if (NULL != fPath) {
        return fPath->conservativelyContainsRect(rect);
    } else {
        return false;
    }
}

bool SkClipStack::Iter::Clip::isInverseFilled() const {
    return NULL != fPath && fPath->isInverseFillType();
}

SkClipStack::Iter::Iter(const SkClipStack& stack, IterStart startLoc)
    : fStack(&stack) {
    this->reset(stack, startLoc);
}

const SkClipStack::Iter::Clip* SkClipStack::Iter::updateClip(
                        const SkClipStack::Element* element) {
    switch (element->fType) {
        case SkClipStack::Element::kEmpty_Type:
            fClip.fRect = NULL;
            fClip.fPath = NULL;
            element->checkEmpty();
            break;
        case SkClipStack::Element::kRect_Type:
            fClip.fRect = &element->fRect;
            fClip.fPath = NULL;
            break;
        case SkClipStack::Element::kPath_Type:
            fClip.fRect = NULL;
            fClip.fPath = &element->fPath;
            break;
    }
    fClip.fOp = element->fOp;
    fClip.fDoAA = element->fDoAA;
    fClip.fGenID = element->fGenID;
    return &fClip;
}

const SkClipStack::Iter::Clip* SkClipStack::Iter::next() {
    const SkClipStack::Element* element = (const SkClipStack::Element*)fIter.next();
    if (NULL == element) {
        return NULL;
    }

    return this->updateClip(element);
}

const SkClipStack::Iter::Clip* SkClipStack::Iter::prev() {
    const SkClipStack::Element* element = (const SkClipStack::Element*)fIter.prev();
    if (NULL == element) {
        return NULL;
    }

    return this->updateClip(element);
}

const SkClipStack::Iter::Clip* SkClipStack::Iter::skipToTopmost(SkRegion::Op op) {

    if (NULL == fStack) {
        return NULL;
    }

    fIter.reset(fStack->fDeque, SkDeque::Iter::kBack_IterStart);

    const SkClipStack::Element* element = NULL;

    for (element = (const SkClipStack::Element*) fIter.prev();
         NULL != element;
         element = (const SkClipStack::Element*) fIter.prev()) {

        if (op == element->fOp) {
            // The Deque's iterator is actually one pace ahead of the
            // returned value. So while "element" is the element we want to
            // return, the iterator is actually pointing at (and will
            // return on the next "next" or "prev" call) the element
            // in front of it in the deque. Bump the iterator forward a
            // step so we get the expected result.
            if (NULL == fIter.next()) {
                // The reverse iterator has run off the front of the deque
                // (i.e., the "op" clip is the first clip) and can't
                // recover. Reset the iterator to start at the front.
                fIter.reset(fStack->fDeque, SkDeque::Iter::kFront_IterStart);
            }
            break;
        }
    }

    if (NULL == element) {
        // There were no "op" clips
        fIter.reset(fStack->fDeque, SkDeque::Iter::kFront_IterStart);
    }

    return this->next();
}

const SkClipStack::Iter::Clip* SkClipStack::Iter::nextCombined() {
    const Clip* clip;

    if (NULL != (clip = this->next()) &&
        SkRegion::kIntersect_Op == clip->fOp &&
        NULL != clip->fRect) {
        fCombinedRect = *clip->fRect;
        bool doAA = clip->fDoAA;

        while(NULL != (clip = this->next()) &&
              SkRegion::kIntersect_Op == clip->fOp &&
              NULL != clip->fRect) { // backup if non-null
            /**
             * If the AA settings don't match on consecutive rects we can still continue if
             * either contains the other. Otherwise, we must stop.
             */
            if (doAA != clip->fDoAA) {
                if (fCombinedRect.contains(*clip->fRect)) {
                    fCombinedRect = *clip->fRect;
                    doAA = clip->fDoAA;
                } else if (!clip->fRect->contains(fCombinedRect)) {
                    break;
                }
            } else if (!fCombinedRect.intersect(*fClip.fRect)) {
                fCombinedRect.setEmpty();
                clip = NULL; // prevents unnecessary rewind below.
                break;
            }
        }
        // If we got here and clip is non-NULL then we got an element that we weren't able to
        // combine. We need to backup one to ensure that the callers next next() call returns it.
        if (NULL != clip) {
            // If next() above returned the last element then due to Iter's internal workings prev()
            // will return NULL. In that case we reset to the last element.
            if (NULL == this->prev()) {
                this->reset(*fStack, SkClipStack::Iter::kTop_IterStart);
            }
        }

        // Must do this last because it is overwritten in the above backup.
        fClip.fRect = &fCombinedRect;
        fClip.fPath = NULL;
        fClip.fOp = SkRegion::kIntersect_Op;
        fClip.fDoAA = doAA;
        fClip.fGenID = kInvalidGenID;
        return &fClip;
    } else {
        return clip;
    }
}

void SkClipStack::Iter::reset(const SkClipStack& stack, IterStart startLoc) {
    fStack = &stack;
    fIter.reset(stack.fDeque, static_cast<SkDeque::Iter::IterStart>(startLoc));
}

// helper method
void SkClipStack::getConservativeBounds(int offsetX,
                                        int offsetY,
                                        int maxWidth,
                                        int maxHeight,
                                        SkRect* devBounds,
                                        bool* isIntersectionOfRects) const {
    SkASSERT(NULL != devBounds);

    devBounds->setLTRB(0, 0,
                       SkIntToScalar(maxWidth), SkIntToScalar(maxHeight));

    SkRect temp;
    SkClipStack::BoundsType boundType;

    // temp starts off in canvas space here
    this->getBounds(&temp, &boundType, isIntersectionOfRects);
    if (SkClipStack::kInsideOut_BoundsType == boundType) {
        return;
    }

    // but is converted to device space here
    temp.offset(SkIntToScalar(offsetX), SkIntToScalar(offsetY));

    if (!devBounds->intersect(temp)) {
        devBounds->setEmpty();
    }
}

void SkClipStack::addPurgeClipCallback(PFPurgeClipCB callback, void* data) const {
    ClipCallbackData temp = { callback, data };
    fCallbackData.append(1, &temp);
}

void SkClipStack::removePurgeClipCallback(PFPurgeClipCB callback, void* data) const {
    ClipCallbackData temp = { callback, data };
    int index = fCallbackData.find(temp);
    if (index >= 0) {
        fCallbackData.removeShuffle(index);
    }
}

// The clip state represented by 'element' will never be used again. Purge it.
void SkClipStack::purgeClip(Element* element) {
    SkASSERT(NULL != element);
    if (element->fGenID >= 0 && element->fGenID < kFirstUnreservedGenID) {
        return;
    }

    for (int i = 0; i < fCallbackData.count(); ++i) {
        (*fCallbackData[i].fCallback)(element->fGenID, fCallbackData[i].fData);
    }

    // Invalidate element's gen ID so handlers can detect already handled records
    element->fGenID = kInvalidGenID;
}

int32_t SkClipStack::GetNextGenID() {
    // TODO: handle overflow.
    return sk_atomic_inc(&gGenID);
}

int32_t SkClipStack::getTopmostGenID() const {

    if (fDeque.empty()) {
        return kInvalidGenID;
    }

    Element* element = (Element*)fDeque.back();
    return element->fGenID;
}

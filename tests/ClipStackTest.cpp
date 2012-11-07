
/*
 * Copyright 2011 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "Test.h"
#include "SkClipStack.h"
#include "SkPath.h"
#include "SkRect.h"



static void test_assign_and_comparison(skiatest::Reporter* reporter) {
    SkClipStack s;
    bool doAA = false;

    REPORTER_ASSERT(reporter, 0 == s.getSaveCount());

    // Build up a clip stack with a path, an empty clip, and a rect.
    s.save();
    REPORTER_ASSERT(reporter, 1 == s.getSaveCount());

    SkPath p;
    p.moveTo(5, 6);
    p.lineTo(7, 8);
    p.lineTo(5, 9);
    p.close();
    s.clipDevPath(p, SkRegion::kIntersect_Op, doAA);

    s.save();
    REPORTER_ASSERT(reporter, 2 == s.getSaveCount());

    SkRect r = SkRect::MakeLTRB(1, 2, 3, 4);
    s.clipDevRect(r, SkRegion::kIntersect_Op, doAA);
    r = SkRect::MakeLTRB(10, 11, 12, 13);
    s.clipDevRect(r, SkRegion::kIntersect_Op, doAA);

    s.save();
    REPORTER_ASSERT(reporter, 3 == s.getSaveCount());

    r = SkRect::MakeLTRB(14, 15, 16, 17);
    s.clipDevRect(r, SkRegion::kUnion_Op, doAA);

    // Test that assignment works.
    SkClipStack copy = s;
    REPORTER_ASSERT(reporter, s == copy);

    // Test that different save levels triggers not equal.
    s.restore();
    REPORTER_ASSERT(reporter, 2 == s.getSaveCount());
    REPORTER_ASSERT(reporter, s != copy);

    // Test that an equal, but not copied version is equal.
    s.save();
    REPORTER_ASSERT(reporter, 3 == s.getSaveCount());

    r = SkRect::MakeLTRB(14, 15, 16, 17);
    s.clipDevRect(r, SkRegion::kUnion_Op, doAA);
    REPORTER_ASSERT(reporter, s == copy);

    // Test that a different op on one level triggers not equal.
    s.restore();
    REPORTER_ASSERT(reporter, 2 == s.getSaveCount());
    s.save();
    REPORTER_ASSERT(reporter, 3 == s.getSaveCount());

    r = SkRect::MakeLTRB(14, 15, 16, 17);
    s.clipDevRect(r, SkRegion::kIntersect_Op, doAA);
    REPORTER_ASSERT(reporter, s != copy);

    // Test that different state (clip type) triggers not equal.
    // NO LONGER VALID: if a path contains only a rect, we turn
    // it into a bare rect for performance reasons (working
    // around Chromium/JavaScript bad pattern).
/*
    s.restore();
    s.save();
    SkPath rp;
    rp.addRect(r);
    s.clipDevPath(rp, SkRegion::kUnion_Op, doAA);
    REPORTER_ASSERT(reporter, s != copy);
*/

    // Test that different rects triggers not equal.
    s.restore();
    REPORTER_ASSERT(reporter, 2 == s.getSaveCount());
    s.save();
    REPORTER_ASSERT(reporter, 3 == s.getSaveCount());

    r = SkRect::MakeLTRB(24, 25, 26, 27);
    s.clipDevRect(r, SkRegion::kUnion_Op, doAA);
    REPORTER_ASSERT(reporter, s != copy);

    // Sanity check
    s.restore();
    REPORTER_ASSERT(reporter, 2 == s.getSaveCount());

    copy.restore();
    REPORTER_ASSERT(reporter, 2 == copy.getSaveCount());
    REPORTER_ASSERT(reporter, s == copy);
    s.restore();
    REPORTER_ASSERT(reporter, 1 == s.getSaveCount());
    copy.restore();
    REPORTER_ASSERT(reporter, 1 == copy.getSaveCount());
    REPORTER_ASSERT(reporter, s == copy);

    // Test that different paths triggers not equal.
    s.restore();
    REPORTER_ASSERT(reporter, 0 == s.getSaveCount());
    s.save();
    REPORTER_ASSERT(reporter, 1 == s.getSaveCount());

    p.addRect(r);
    s.clipDevPath(p, SkRegion::kIntersect_Op, doAA);
    REPORTER_ASSERT(reporter, s != copy);
}

static void assert_count(skiatest::Reporter* reporter, const SkClipStack& stack,
                         int count) {
    SkClipStack::B2TIter iter(stack);
    int counter = 0;
    while (iter.next()) {
        counter += 1;
    }
    REPORTER_ASSERT(reporter, count == counter);
}

// Exercise the SkClipStack's bottom to top and bidirectional iterators
// (including the skipToTopmost functionality)
static void test_iterators(skiatest::Reporter* reporter) {
    SkClipStack stack;

    static const SkRect gRects[] = {
        { 0,   0,  40,  40 },
        { 60,  0, 100,  40 },
        { 0,  60,  40, 100 },
        { 60, 60, 100, 100 }
    };

    for (size_t i = 0; i < SK_ARRAY_COUNT(gRects); i++) {
        // the union op will prevent these from being fused together
        stack.clipDevRect(gRects[i], SkRegion::kUnion_Op, false);
    }

    assert_count(reporter, stack, 4);

    // bottom to top iteration
    {
        const SkClipStack::B2TIter::Clip* clip = NULL;

        SkClipStack::B2TIter iter(stack);
        int i;

        for (i = 0, clip = iter.next(); clip; ++i, clip = iter.next()) {
            REPORTER_ASSERT(reporter, *clip->fRect == gRects[i]);
        }

        SkASSERT(i == 4);
    }

    // top to bottom iteration
    {
        const SkClipStack::Iter::Clip* clip = NULL;

        SkClipStack::Iter iter(stack, SkClipStack::Iter::kTop_IterStart);
        int i;

        for (i = 3, clip = iter.prev(); clip; --i, clip = iter.prev()) {
            REPORTER_ASSERT(reporter, *clip->fRect == gRects[i]);
        }

        SkASSERT(i == -1);
    }

    // skipToTopmost
    {
        const SkClipStack::Iter::Clip*clip = NULL;

        SkClipStack::Iter iter(stack, SkClipStack::Iter::kBottom_IterStart);

        clip = iter.skipToTopmost(SkRegion::kUnion_Op);
        REPORTER_ASSERT(reporter, *clip->fRect == gRects[3]);
    }
}

// Exercise the SkClipStack's getConservativeBounds computation
static void test_bounds(skiatest::Reporter* reporter, bool useRects) {

    static const int gNumCases = 20;
    static const SkRect gAnswerRectsBW[gNumCases] = {
        // A op B
        { 40, 40, 50, 50 },
        { 10, 10, 50, 50 },
        { 10, 10, 80, 80 },
        { 10, 10, 80, 80 },
        { 40, 40, 80, 80 },

        // invA op B
        { 40, 40, 80, 80 },
        { 0, 0, 100, 100 },
        { 0, 0, 100, 100 },
        { 0, 0, 100, 100 },
        { 40, 40, 50, 50 },

        // A op invB
        { 10, 10, 50, 50 },
        { 40, 40, 50, 50 },
        { 0, 0, 100, 100 },
        { 0, 0, 100, 100 },
        { 0, 0, 100, 100 },

        // invA op invB
        { 0, 0, 100, 100 },
        { 40, 40, 80, 80 },
        { 0, 0, 100, 100 },
        { 10, 10, 80, 80 },
        { 10, 10, 50, 50 },
    };

    static const SkRegion::Op gOps[] = {
        SkRegion::kIntersect_Op,
        SkRegion::kDifference_Op,
        SkRegion::kUnion_Op,
        SkRegion::kXOR_Op,
        SkRegion::kReverseDifference_Op
    };

    SkRect rectA, rectB;

    rectA.iset(10, 10, 50, 50);
    rectB.iset(40, 40, 80, 80);

    SkPath clipA, clipB;

    clipA.addRoundRect(rectA, SkIntToScalar(5), SkIntToScalar(5));
    clipB.addRoundRect(rectB, SkIntToScalar(5), SkIntToScalar(5));

    SkClipStack stack;
    SkRect devClipBound;
    bool isIntersectionOfRects = false;

    int testCase = 0;
    int numBitTests = useRects ? 1 : 4;
    for (int invBits = 0; invBits < numBitTests; ++invBits) {
        for (size_t op = 0; op < SK_ARRAY_COUNT(gOps); ++op) {

            stack.save();
            bool doInvA = SkToBool(invBits & 1);
            bool doInvB = SkToBool(invBits & 2);

            clipA.setFillType(doInvA ? SkPath::kInverseEvenOdd_FillType :
                                       SkPath::kEvenOdd_FillType);
            clipB.setFillType(doInvB ? SkPath::kInverseEvenOdd_FillType :
                                       SkPath::kEvenOdd_FillType);

            if (useRects) {
                stack.clipDevRect(rectA, SkRegion::kIntersect_Op, false);
                stack.clipDevRect(rectB, gOps[op], false);
            } else {
                stack.clipDevPath(clipA, SkRegion::kIntersect_Op, false);
                stack.clipDevPath(clipB, gOps[op], false);
            }

            REPORTER_ASSERT(reporter, !stack.isWideOpen());

            stack.getConservativeBounds(0, 0, 100, 100, &devClipBound,
                                        &isIntersectionOfRects);

            if (useRects) {
                REPORTER_ASSERT(reporter, isIntersectionOfRects ==
                        (gOps[op] == SkRegion::kIntersect_Op));
            } else {
                REPORTER_ASSERT(reporter, !isIntersectionOfRects);
            }

            SkASSERT(testCase < gNumCases);
            REPORTER_ASSERT(reporter, devClipBound == gAnswerRectsBW[testCase]);
            ++testCase;

            stack.restore();
        }
    }
}

// Test out 'isWideOpen' entry point
static void test_isWideOpen(skiatest::Reporter* reporter) {

    SkRect rectA, rectB;

    rectA.iset(10, 10, 40, 40);
    rectB.iset(50, 50, 80, 80);

    // Stack should initially be wide open
    {
        SkClipStack stack;

        REPORTER_ASSERT(reporter, stack.isWideOpen());
    }

    // Test out case where the user specifies a union that includes everything
    {
        SkClipStack stack;

        SkPath clipA, clipB;

        clipA.addRoundRect(rectA, SkIntToScalar(5), SkIntToScalar(5));
        clipA.setFillType(SkPath::kInverseEvenOdd_FillType);

        clipB.addRoundRect(rectB, SkIntToScalar(5), SkIntToScalar(5));
        clipB.setFillType(SkPath::kInverseEvenOdd_FillType);

        stack.clipDevPath(clipA, SkRegion::kReplace_Op, false);
        stack.clipDevPath(clipB, SkRegion::kUnion_Op, false);

        REPORTER_ASSERT(reporter, stack.isWideOpen());
    }

    // Test out union w/ a wide open clip
    {
        SkClipStack stack;

        stack.clipDevRect(rectA, SkRegion::kUnion_Op, false);

        REPORTER_ASSERT(reporter, stack.isWideOpen());
    }

    // Test out empty difference from a wide open clip
    {
        SkClipStack stack;

        SkRect emptyRect;
        emptyRect.setEmpty();

        stack.clipDevRect(emptyRect, SkRegion::kDifference_Op, false);

        REPORTER_ASSERT(reporter, stack.isWideOpen());
    }

    // Test out return to wide open
    {
        SkClipStack stack;

        stack.save();

        stack.clipDevRect(rectA, SkRegion::kReplace_Op, false);

        REPORTER_ASSERT(reporter, !stack.isWideOpen());

        stack.restore();

        REPORTER_ASSERT(reporter, stack.isWideOpen());
    }
}

static int count(const SkClipStack& stack) {

    SkClipStack::Iter iter(stack, SkClipStack::Iter::kTop_IterStart);

    const SkClipStack::Iter::Clip* clip = NULL;
    int count = 0;

    for (clip = iter.prev(); clip; clip = iter.prev(), ++count) {
        ;
    }

    return count;
}

// Test out SkClipStack's merging of rect clips. In particular exercise
// merging of aa vs. bw rects.
static void test_rect_merging(skiatest::Reporter* reporter) {

    SkRect overlapLeft  = SkRect::MakeLTRB(10, 10, 50, 50);
    SkRect overlapRight = SkRect::MakeLTRB(40, 40, 80, 80);

    SkRect nestedParent = SkRect::MakeLTRB(10, 10, 90, 90);
    SkRect nestedChild  = SkRect::MakeLTRB(40, 40, 60, 60);

    SkRect bound;
    SkClipStack::BoundsType type;
    bool isIntersectionOfRects;

    // all bw overlapping - should merge
    {
        SkClipStack stack;

        stack.clipDevRect(overlapLeft, SkRegion::kReplace_Op, false);

        stack.clipDevRect(overlapRight, SkRegion::kIntersect_Op, false);

        REPORTER_ASSERT(reporter, 1 == count(stack));

        stack.getBounds(&bound, &type, &isIntersectionOfRects);

        REPORTER_ASSERT(reporter, isIntersectionOfRects);
    }

    // all aa overlapping - should merge
    {
        SkClipStack stack;

        stack.clipDevRect(overlapLeft, SkRegion::kReplace_Op, true);

        stack.clipDevRect(overlapRight, SkRegion::kIntersect_Op, true);

        REPORTER_ASSERT(reporter, 1 == count(stack));

        stack.getBounds(&bound, &type, &isIntersectionOfRects);

        REPORTER_ASSERT(reporter, isIntersectionOfRects);
    }

    // mixed overlapping - should _not_ merge
    {
        SkClipStack stack;

        stack.clipDevRect(overlapLeft, SkRegion::kReplace_Op, true);

        stack.clipDevRect(overlapRight, SkRegion::kIntersect_Op, false);

        REPORTER_ASSERT(reporter, 2 == count(stack));

        stack.getBounds(&bound, &type, &isIntersectionOfRects);

        REPORTER_ASSERT(reporter, !isIntersectionOfRects);
    }

    // mixed nested (bw inside aa) - should merge
    {
        SkClipStack stack;

        stack.clipDevRect(nestedParent, SkRegion::kReplace_Op, true);

        stack.clipDevRect(nestedChild, SkRegion::kIntersect_Op, false);

        REPORTER_ASSERT(reporter, 1 == count(stack));

        stack.getBounds(&bound, &type, &isIntersectionOfRects);

        REPORTER_ASSERT(reporter, isIntersectionOfRects);
    }

    // mixed nested (aa inside bw) - should merge
    {
        SkClipStack stack;

        stack.clipDevRect(nestedParent, SkRegion::kReplace_Op, false);

        stack.clipDevRect(nestedChild, SkRegion::kIntersect_Op, true);

        REPORTER_ASSERT(reporter, 1 == count(stack));

        stack.getBounds(&bound, &type, &isIntersectionOfRects);

        REPORTER_ASSERT(reporter, isIntersectionOfRects);
    }

    // reverse nested (aa inside bw) - should _not_ merge
    {
        SkClipStack stack;

        stack.clipDevRect(nestedChild, SkRegion::kReplace_Op, false);

        stack.clipDevRect(nestedParent, SkRegion::kIntersect_Op, true);

        REPORTER_ASSERT(reporter, 2 == count(stack));

        stack.getBounds(&bound, &type, &isIntersectionOfRects);

        REPORTER_ASSERT(reporter, !isIntersectionOfRects);
    }
}


// This is similar to the above test but tests the iterator's ability to merge rects in the
// middle of a clip stack's sequence using nextCombined(). There is a save after every clip
// element to prevent the clip stack from merging the rectangles as they are added.
static void test_iter_rect_merging(skiatest::Reporter* reporter) {

    SkRect overlapLeft  = SkRect::MakeLTRB(10, 10, 50, 50);
    SkRect overlapRight = SkRect::MakeLTRB(40, 40, 80, 80);

    SkRect nestedParent = SkRect::MakeLTRB(10, 10, 90, 90);
    SkRect nestedChild  = SkRect::MakeLTRB(40, 40, 60, 60);

    SkRect farAway      = SkRect::MakeLTRB(1000, 1000, 1010, 1010);

    SkRect overlapIntersect;
    overlapIntersect.intersect(overlapLeft, overlapRight);

    SkPath path1, path2;
    path1.addCircle(SkIntToScalar(30), SkIntToScalar(30), SkIntToScalar(1000));
    path2.addOval(SkRect::MakeWH(500, 600));

    const SkClipStack::Iter::Clip* clip;

    // call nextCombined with an empty clip stack
    {
        SkClipStack stack;
        SkClipStack::Iter iter(stack, SkClipStack::Iter::kBottom_IterStart);
        REPORTER_ASSERT(reporter, NULL == iter.nextCombined());
    }

    // two bw overlapping - should merge, bracketed by paths
    {
        SkClipStack stack;
        stack.clipDevPath(path1, SkRegion::kIntersect_Op, false); stack.save();

        stack.clipDevRect(overlapLeft, SkRegion::kIntersect_Op, false); stack.save();

        stack.clipDevRect(overlapRight, SkRegion::kIntersect_Op, false); stack.save();

        stack.clipDevPath(path2, SkRegion::kIntersect_Op, false); stack.save();

        SkClipStack::Iter iter(stack, SkClipStack::Iter::kBottom_IterStart);

        clip = iter.nextCombined();
        REPORTER_ASSERT(reporter, *clip->fPath == path1 && !clip->fDoAA);

        clip = iter.nextCombined();
        REPORTER_ASSERT(reporter, !clip->fDoAA && *clip->fRect == overlapIntersect);

        clip = iter.nextCombined();
        REPORTER_ASSERT(reporter, *clip->fPath == path2 && !clip->fDoAA);

        clip = iter.nextCombined();
        REPORTER_ASSERT(reporter, NULL == clip);
    }

    // same as above but rects are aa and no final path.
    {
        SkClipStack stack;
        stack.clipDevPath(path1, SkRegion::kIntersect_Op, false); stack.save();

        stack.clipDevRect(overlapLeft, SkRegion::kIntersect_Op, true); stack.save();

        stack.clipDevRect(overlapRight, SkRegion::kIntersect_Op, true); stack.save();

        SkClipStack::Iter iter(stack, SkClipStack::Iter::kBottom_IterStart);

        clip = iter.nextCombined();
        REPORTER_ASSERT(reporter, *clip->fPath == path1 && !clip->fDoAA);

        clip = iter.nextCombined();
        REPORTER_ASSERT(reporter, clip->fDoAA && *clip->fRect == overlapIntersect);

        clip = iter.nextCombined();
        REPORTER_ASSERT(reporter, NULL == clip);
    }

    // mixed overlapping - no paths - should _not_ merge
    {
        SkClipStack stack;

        stack.clipDevRect(overlapLeft, SkRegion::kIntersect_Op, true); stack.save();
        stack.clipDevRect(overlapRight, SkRegion::kIntersect_Op, false); stack.save();

        SkClipStack::Iter iter(stack, SkClipStack::Iter::kBottom_IterStart);

        clip = iter.nextCombined();
        REPORTER_ASSERT(reporter, clip->fDoAA && *clip->fRect == overlapLeft);

        clip = iter.nextCombined();
        REPORTER_ASSERT(reporter, !clip->fDoAA && *clip->fRect == overlapRight);

        clip = iter.nextCombined();
        REPORTER_ASSERT(reporter, NULL == clip);
    }

    // three rects in a row where the third rect uses a non-intersect op.
    {
        SkClipStack stack;

        stack.clipDevRect(overlapLeft, SkRegion::kIntersect_Op, true); stack.save();
        stack.clipDevRect(overlapRight, SkRegion::kIntersect_Op, true); stack.save();
        stack.clipDevRect(nestedParent, SkRegion::kXOR_Op, true); stack.save();

        SkClipStack::Iter iter(stack, SkClipStack::Iter::kBottom_IterStart);

        clip = iter.nextCombined();
        REPORTER_ASSERT(reporter, clip->fDoAA && *clip->fRect == overlapIntersect);
        clip = iter.nextCombined();
        REPORTER_ASSERT(reporter, clip->fDoAA && *clip->fRect == nestedParent);
        clip = iter.nextCombined();
        REPORTER_ASSERT(reporter, NULL == clip);
    }

    // mixed nested (bw inside aa) - should merge
    {
        SkClipStack stack;
        stack.clipDevRect(nestedParent, SkRegion::kIntersect_Op, false); stack.save();

        stack.clipDevRect(nestedChild, SkRegion::kIntersect_Op, true); stack.save();

        SkClipStack::Iter iter(stack, SkClipStack::Iter::kBottom_IterStart);
        
        clip = iter.nextCombined();
        REPORTER_ASSERT(reporter, clip->fDoAA && *clip->fRect == nestedChild);

        clip = iter.nextCombined();
        REPORTER_ASSERT(reporter, NULL == clip);
    }

    // mixed nested (aa inside bw) - should merge
    {
        SkClipStack stack;
        stack.clipDevRect(nestedChild, SkRegion::kIntersect_Op, false); stack.save();

        stack.clipDevRect(nestedParent, SkRegion::kIntersect_Op, true); stack.save();

        SkClipStack::Iter iter(stack, SkClipStack::Iter::kBottom_IterStart);
        
        clip = iter.nextCombined();
        REPORTER_ASSERT(reporter, !clip->fDoAA && *clip->fRect == nestedChild);

        clip = iter.nextCombined();
        REPORTER_ASSERT(reporter, NULL == clip);
    }

    // three rect intersects in a row where result is empty after the second.
    {
        SkClipStack stack;

        stack.clipDevRect(overlapLeft, SkRegion::kIntersect_Op, false); stack.save();
        stack.clipDevRect(farAway, SkRegion::kIntersect_Op, false); stack.save();
        stack.clipDevRect(overlapRight, SkRegion::kIntersect_Op, false); stack.save();

        SkClipStack::Iter iter(stack, SkClipStack::Iter::kBottom_IterStart);

        clip = iter.nextCombined();
        REPORTER_ASSERT(reporter, clip->fRect->isEmpty());

        clip = iter.nextCombined();
        REPORTER_ASSERT(reporter, *clip->fRect == overlapRight);

        clip = iter.nextCombined();
        REPORTER_ASSERT(reporter, NULL == clip);
    }
}

static void TestClipStack(skiatest::Reporter* reporter) {
    SkClipStack stack;

    REPORTER_ASSERT(reporter, 0 == stack.getSaveCount());
    assert_count(reporter, stack, 0);

    static const SkIRect gRects[] = {
        { 0, 0, 100, 100 },
        { 25, 25, 125, 125 },
        { 0, 0, 1000, 1000 },
        { 0, 0, 75, 75 }
    };
    for (size_t i = 0; i < SK_ARRAY_COUNT(gRects); i++) {
        stack.clipDevRect(gRects[i], SkRegion::kIntersect_Op);
    }

    // all of the above rects should have been intersected, leaving only 1 rect
    SkClipStack::B2TIter iter(stack);
    const SkClipStack::B2TIter::Clip* clip = iter.next();
    SkRect answer;
    answer.iset(25, 25, 75, 75);

    REPORTER_ASSERT(reporter, clip);
    REPORTER_ASSERT(reporter, clip->fRect);
    REPORTER_ASSERT(reporter, !clip->fPath);
    REPORTER_ASSERT(reporter, SkRegion::kIntersect_Op == clip->fOp);
    REPORTER_ASSERT(reporter, *clip->fRect == answer);
    // now check that we only had one in our iterator
    REPORTER_ASSERT(reporter, !iter.next());

    stack.reset();
    REPORTER_ASSERT(reporter, 0 == stack.getSaveCount());
    assert_count(reporter, stack, 0);

    test_assign_and_comparison(reporter);
    test_iterators(reporter);
    test_bounds(reporter, true);        // once with rects
    test_bounds(reporter, false);       // once with paths
    test_isWideOpen(reporter);
    test_rect_merging(reporter);
    test_iter_rect_merging(reporter);
}

#include "TestClassDef.h"
DEFINE_TESTCLASS("ClipStack", TestClipStackClass, TestClipStack)

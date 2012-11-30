/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "Simplify.h"

#undef SkASSERT
#define SkASSERT(cond) while (!(cond)) { sk_throw(); }

// Terminology:
// A Path contains one of more Contours
// A Contour is made up of Segment array
// A Segment is described by a Verb and a Point array with 2, 3, or 4 points
// A Verb is one of Line, Quad(ratic), or Cubic
// A Segment contains a Span array
// A Span is describes a portion of a Segment using starting and ending T
// T values range from 0 to 1, where 0 is the first Point in the Segment
// An Edge is a Segment generated from a Span

// FIXME: remove once debugging is complete
#ifdef SK_DEBUG
int gDebugMaxWindSum = SK_MaxS32;
int gDebugMaxWindValue = SK_MaxS32;
#endif

#define PIN_ADD_T 0
#define TRY_ROTATE 1

#define DEBUG_UNUSED 0 // set to expose unused functions
#define FORCE_RELEASE 1  // set force release to 1 for multiple thread -- no debugging

#if FORCE_RELEASE || defined SK_RELEASE

const bool gRunTestsInOneThread = false;

#define DEBUG_ACTIVE_SPANS 0
#define DEBUG_ADD_INTERSECTING_TS 0
#define DEBUG_ADD_T_PAIR 0
#define DEBUG_ANGLE 0
#define DEBUG_CONCIDENT 0
#define DEBUG_CROSS 0
#define DEBUG_MARK_DONE 0
#define DEBUG_PATH_CONSTRUCTION 0
#define DEBUG_SHOW_WINDING 0
#define DEBUG_SORT 0
#define DEBUG_WIND_BUMP 0
#define DEBUG_WINDING 0

#else

const bool gRunTestsInOneThread = true;

#define DEBUG_ACTIVE_SPANS 1
#define DEBUG_ADD_INTERSECTING_TS 1
#define DEBUG_ADD_T_PAIR 1
#define DEBUG_ANGLE 1
#define DEBUG_CONCIDENT 1
#define DEBUG_CROSS 0
#define DEBUG_MARK_DONE 1
#define DEBUG_PATH_CONSTRUCTION 1
#define DEBUG_SHOW_WINDING 0
#define DEBUG_SORT 1
#define DEBUG_WIND_BUMP 0
#define DEBUG_WINDING 1

#endif

#define DEBUG_DUMP (DEBUG_ACTIVE_SPANS | DEBUG_CONCIDENT | DEBUG_SORT | DEBUG_PATH_CONSTRUCTION)

#if DEBUG_DUMP
static const char* kLVerbStr[] = {"", "line", "quad", "cubic"};
// static const char* kUVerbStr[] = {"", "Line", "Quad", "Cubic"};
static int gContourID;
static int gSegmentID;
#endif

#ifndef DEBUG_TEST
#define DEBUG_TEST 0
#endif

#define MAKE_CONST_LINE(line, pts) \
    const _Line line = {{pts[0].fX, pts[0].fY}, {pts[1].fX, pts[1].fY}}
#define MAKE_CONST_QUAD(quad, pts) \
    const Quadratic quad = {{pts[0].fX, pts[0].fY}, {pts[1].fX, pts[1].fY}, \
            {pts[2].fX, pts[2].fY}}
#define MAKE_CONST_CUBIC(cubic, pts) \
    const Cubic cubic = {{pts[0].fX, pts[0].fY}, {pts[1].fX, pts[1].fY}, \
            {pts[2].fX, pts[2].fY}, {pts[3].fX, pts[3].fY}}

static int LineIntersect(const SkPoint a[2], const SkPoint b[2],
        Intersections& intersections) {
    MAKE_CONST_LINE(aLine, a);
    MAKE_CONST_LINE(bLine, b);
    return intersect(aLine, bLine, intersections.fT[0], intersections.fT[1]);
}

static int QuadLineIntersect(const SkPoint a[3], const SkPoint b[2],
        Intersections& intersections) {
    MAKE_CONST_QUAD(aQuad, a);
    MAKE_CONST_LINE(bLine, b);
    return intersect(aQuad, bLine, intersections);
}

static int CubicLineIntersect(const SkPoint a[4], const SkPoint b[2],
        Intersections& intersections) {
    MAKE_CONST_CUBIC(aCubic, a);
    MAKE_CONST_LINE(bLine, b);
    return intersect(aCubic, bLine, intersections.fT[0], intersections.fT[1]);
}

static int QuadIntersect(const SkPoint a[3], const SkPoint b[3],
        Intersections& intersections) {
    MAKE_CONST_QUAD(aQuad, a);
    MAKE_CONST_QUAD(bQuad, b);
#define TRY_QUARTIC_SOLUTION 1
#if TRY_QUARTIC_SOLUTION
    intersect2(aQuad, bQuad, intersections);
#else
    intersect(aQuad, bQuad, intersections);
#endif
    return intersections.fUsed ? intersections.fUsed : intersections.fCoincidentUsed;
}

static int CubicIntersect(const SkPoint a[4], const SkPoint b[4],
        Intersections& intersections) {
    MAKE_CONST_CUBIC(aCubic, a);
    MAKE_CONST_CUBIC(bCubic, b);
    intersect(aCubic, bCubic, intersections);
    return intersections.fUsed;
}

static int HLineIntersect(const SkPoint a[2], SkScalar left, SkScalar right,
        SkScalar y, bool flipped, Intersections& intersections) {
    MAKE_CONST_LINE(aLine, a);
    return horizontalIntersect(aLine, left, right, y, flipped, intersections);
}

static int HQuadIntersect(const SkPoint a[3], SkScalar left, SkScalar right,
        SkScalar y, bool flipped, Intersections& intersections) {
    MAKE_CONST_QUAD(aQuad, a);
    return horizontalIntersect(aQuad, left, right, y, flipped, intersections);
}

static int HCubicIntersect(const SkPoint a[4], SkScalar left, SkScalar right,
        SkScalar y, bool flipped, Intersections& intersections) {
    MAKE_CONST_CUBIC(aCubic, a);
    return horizontalIntersect(aCubic, left, right, y, flipped, intersections);
}

static int VLineIntersect(const SkPoint a[2], SkScalar top, SkScalar bottom,
        SkScalar x, bool flipped, Intersections& intersections) {
    MAKE_CONST_LINE(aLine, a);
    return verticalIntersect(aLine, top, bottom, x, flipped, intersections);
}

static int VQuadIntersect(const SkPoint a[3], SkScalar top, SkScalar bottom,
        SkScalar x, bool flipped, Intersections& intersections) {
    MAKE_CONST_QUAD(aQuad, a);
    return verticalIntersect(aQuad, top, bottom, x, flipped, intersections);
}

static int VCubicIntersect(const SkPoint a[4], SkScalar top, SkScalar bottom,
        SkScalar x, bool flipped, Intersections& intersections) {
    MAKE_CONST_CUBIC(aCubic, a);
    return verticalIntersect(aCubic, top, bottom, x, flipped, intersections);
}

static int (* const VSegmentIntersect[])(const SkPoint [], SkScalar ,
        SkScalar , SkScalar , bool , Intersections& ) = {
    NULL,
    VLineIntersect,
    VQuadIntersect,
    VCubicIntersect
};

static void LineXYAtT(const SkPoint a[2], double t, SkPoint* out) {
    MAKE_CONST_LINE(line, a);
    double x, y;
    xy_at_t(line, t, x, y);
    out->fX = SkDoubleToScalar(x);
    out->fY = SkDoubleToScalar(y);
}

static void QuadXYAtT(const SkPoint a[3], double t, SkPoint* out) {
    MAKE_CONST_QUAD(quad, a);
    double x, y;
    xy_at_t(quad, t, x, y);
    out->fX = SkDoubleToScalar(x);
    out->fY = SkDoubleToScalar(y);
}

static void QuadXYAtT(const SkPoint a[3], double t, _Point* out) {
    MAKE_CONST_QUAD(quad, a);
    xy_at_t(quad, t, out->x, out->y);
}

static void CubicXYAtT(const SkPoint a[4], double t, SkPoint* out) {
    MAKE_CONST_CUBIC(cubic, a);
    double x, y;
    xy_at_t(cubic, t, x, y);
    out->fX = SkDoubleToScalar(x);
    out->fY = SkDoubleToScalar(y);
}

static void (* const SegmentXYAtT[])(const SkPoint [], double , SkPoint* ) = {
    NULL,
    LineXYAtT,
    QuadXYAtT,
    CubicXYAtT
};

static SkScalar LineXAtT(const SkPoint a[2], double t) {
    MAKE_CONST_LINE(aLine, a);
    double x;
    xy_at_t(aLine, t, x, *(double*) 0);
    return SkDoubleToScalar(x);
}

static SkScalar QuadXAtT(const SkPoint a[3], double t) {
    MAKE_CONST_QUAD(quad, a);
    double x;
    xy_at_t(quad, t, x, *(double*) 0);
    return SkDoubleToScalar(x);
}

static SkScalar CubicXAtT(const SkPoint a[4], double t) {
    MAKE_CONST_CUBIC(cubic, a);
    double x;
    xy_at_t(cubic, t, x, *(double*) 0);
    return SkDoubleToScalar(x);
}

static SkScalar (* const SegmentXAtT[])(const SkPoint [], double ) = {
    NULL,
    LineXAtT,
    QuadXAtT,
    CubicXAtT
};

static SkScalar LineYAtT(const SkPoint a[2], double t) {
    MAKE_CONST_LINE(aLine, a);
    double y;
    xy_at_t(aLine, t, *(double*) 0, y);
    return SkDoubleToScalar(y);
}

static SkScalar QuadYAtT(const SkPoint a[3], double t) {
    MAKE_CONST_QUAD(quad, a);
    double y;
    xy_at_t(quad, t, *(double*) 0, y);
    return SkDoubleToScalar(y);
}

static SkScalar CubicYAtT(const SkPoint a[4], double t) {
    MAKE_CONST_CUBIC(cubic, a);
    double y;
    xy_at_t(cubic, t, *(double*) 0, y);
    return SkDoubleToScalar(y);
}

static SkScalar (* const SegmentYAtT[])(const SkPoint [], double ) = {
    NULL,
    LineYAtT,
    QuadYAtT,
    CubicYAtT
};

static SkScalar LineDXAtT(const SkPoint a[2], double ) {
    return a[1].fX - a[0].fX;
}

static SkScalar QuadDXAtT(const SkPoint a[3], double t) {
    MAKE_CONST_QUAD(quad, a);
    double x;
    dxdy_at_t(quad, t, x, *(double*) 0);
    return SkDoubleToScalar(x);
}

static SkScalar CubicDXAtT(const SkPoint a[4], double t) {
    MAKE_CONST_CUBIC(cubic, a);
    double x;
    dxdy_at_t(cubic, t, x, *(double*) 0);
    return SkDoubleToScalar(x);
}

static SkScalar (* const SegmentDXAtT[])(const SkPoint [], double ) = {
    NULL,
    LineDXAtT,
    QuadDXAtT,
    CubicDXAtT
};

static void LineSubDivide(const SkPoint a[2], double startT, double endT,
        SkPoint sub[2]) {
    MAKE_CONST_LINE(aLine, a);
    _Line dst;
    sub_divide(aLine, startT, endT, dst);
    sub[0].fX = SkDoubleToScalar(dst[0].x);
    sub[0].fY = SkDoubleToScalar(dst[0].y);
    sub[1].fX = SkDoubleToScalar(dst[1].x);
    sub[1].fY = SkDoubleToScalar(dst[1].y);
}

static void QuadSubDivide(const SkPoint a[3], double startT, double endT,
        SkPoint sub[3]) {
    MAKE_CONST_QUAD(aQuad, a);
    Quadratic dst;
    sub_divide(aQuad, startT, endT, dst);
    sub[0].fX = SkDoubleToScalar(dst[0].x);
    sub[0].fY = SkDoubleToScalar(dst[0].y);
    sub[1].fX = SkDoubleToScalar(dst[1].x);
    sub[1].fY = SkDoubleToScalar(dst[1].y);
    sub[2].fX = SkDoubleToScalar(dst[2].x);
    sub[2].fY = SkDoubleToScalar(dst[2].y);
}

static void CubicSubDivide(const SkPoint a[4], double startT, double endT,
        SkPoint sub[4]) {
    MAKE_CONST_CUBIC(aCubic, a);
    Cubic dst;
    sub_divide(aCubic, startT, endT, dst);
    sub[0].fX = SkDoubleToScalar(dst[0].x);
    sub[0].fY = SkDoubleToScalar(dst[0].y);
    sub[1].fX = SkDoubleToScalar(dst[1].x);
    sub[1].fY = SkDoubleToScalar(dst[1].y);
    sub[2].fX = SkDoubleToScalar(dst[2].x);
    sub[2].fY = SkDoubleToScalar(dst[2].y);
    sub[3].fX = SkDoubleToScalar(dst[3].x);
    sub[3].fY = SkDoubleToScalar(dst[3].y);
}

static void (* const SegmentSubDivide[])(const SkPoint [], double , double ,
        SkPoint []) = {
    NULL,
    LineSubDivide,
    QuadSubDivide,
    CubicSubDivide
};

static void LineSubDivideHD(const SkPoint a[2], double startT, double endT,
        _Line sub) {
    MAKE_CONST_LINE(aLine, a);
    _Line dst;
    sub_divide(aLine, startT, endT, dst);
    sub[0] = dst[0];
    sub[1] = dst[1];
}

static void QuadSubDivideHD(const SkPoint a[3], double startT, double endT,
        Quadratic sub) {
    MAKE_CONST_QUAD(aQuad, a);
    Quadratic dst;
    sub_divide(aQuad, startT, endT, dst);
    sub[0] = dst[0];
    sub[1] = dst[1];
    sub[2] = dst[2];
}

static void CubicSubDivideHD(const SkPoint a[4], double startT, double endT,
        Cubic sub) {
    MAKE_CONST_CUBIC(aCubic, a);
    Cubic dst;
    sub_divide(aCubic, startT, endT, dst);
    sub[0] = dst[0];
    sub[1] = dst[1];
    sub[2] = dst[2];
    sub[3] = dst[3];
}

#if DEBUG_UNUSED
static void QuadSubBounds(const SkPoint a[3], double startT, double endT,
        SkRect& bounds) {
    SkPoint dst[3];
    QuadSubDivide(a, startT, endT, dst);
    bounds.fLeft = bounds.fRight = dst[0].fX;
    bounds.fTop = bounds.fBottom = dst[0].fY;
    for (int index = 1; index < 3; ++index) {
        bounds.growToInclude(dst[index].fX, dst[index].fY);
    }
}

static void CubicSubBounds(const SkPoint a[4], double startT, double endT,
        SkRect& bounds) {
    SkPoint dst[4];
    CubicSubDivide(a, startT, endT, dst);
    bounds.fLeft = bounds.fRight = dst[0].fX;
    bounds.fTop = bounds.fBottom = dst[0].fY;
    for (int index = 1; index < 4; ++index) {
        bounds.growToInclude(dst[index].fX, dst[index].fY);
    }
}
#endif

static SkPath::Verb QuadReduceOrder(const SkPoint a[3],
        SkTDArray<SkPoint>& reducePts) {
    MAKE_CONST_QUAD(aQuad, a);
    Quadratic dst;
    int order = reduceOrder(aQuad, dst);
    if (order == 2) { // quad became line
        for (int index = 0; index < order; ++index) {
            SkPoint* pt = reducePts.append();
            pt->fX = SkDoubleToScalar(dst[index].x);
            pt->fY = SkDoubleToScalar(dst[index].y);
        }
    }
    return (SkPath::Verb) (order - 1);
}

static SkPath::Verb CubicReduceOrder(const SkPoint a[4],
        SkTDArray<SkPoint>& reducePts) {
    MAKE_CONST_CUBIC(aCubic, a);
    Cubic dst;
    int order = reduceOrder(aCubic, dst, kReduceOrder_QuadraticsAllowed);
    if (order == 2 || order == 3) { // cubic became line or quad
        for (int index = 0; index < order; ++index) {
            SkPoint* pt = reducePts.append();
            pt->fX = SkDoubleToScalar(dst[index].x);
            pt->fY = SkDoubleToScalar(dst[index].y);
        }
    }
    return (SkPath::Verb) (order - 1);
}

static bool QuadIsLinear(const SkPoint a[3]) {
    MAKE_CONST_QUAD(aQuad, a);
    return isLinear(aQuad, 0, 2);
}

static bool CubicIsLinear(const SkPoint a[4]) {
    MAKE_CONST_CUBIC(aCubic, a);
    return isLinear(aCubic, 0, 3);
}

static SkScalar LineLeftMost(const SkPoint a[2], double startT, double endT) {
    MAKE_CONST_LINE(aLine, a);
    double x[2];
    xy_at_t(aLine, startT, x[0], *(double*) 0);
    xy_at_t(aLine, endT, x[1], *(double*) 0);
    return SkMinScalar((float) x[0], (float) x[1]);
}

static SkScalar QuadLeftMost(const SkPoint a[3], double startT, double endT) {
    MAKE_CONST_QUAD(aQuad, a);
    return (float) leftMostT(aQuad, startT, endT);
}

static SkScalar CubicLeftMost(const SkPoint a[4], double startT, double endT) {
    MAKE_CONST_CUBIC(aCubic, a);
    return (float) leftMostT(aCubic, startT, endT);
}

static SkScalar (* const SegmentLeftMost[])(const SkPoint [], double , double) = {
    NULL,
    LineLeftMost,
    QuadLeftMost,
    CubicLeftMost
};

#if 0 // currently unused
static int QuadRayIntersect(const SkPoint a[3], const SkPoint b[2],
        Intersections& intersections) {
    MAKE_CONST_QUAD(aQuad, a);
    MAKE_CONST_LINE(bLine, b);
    return intersectRay(aQuad, bLine, intersections);
}
#endif

static int QuadRayIntersect(const SkPoint a[3], const _Line& bLine,
        Intersections& intersections) {
    MAKE_CONST_QUAD(aQuad, a);
    return intersectRay(aQuad, bLine, intersections);
}

class Segment;

struct Span {
    Segment* fOther;
    mutable SkPoint fPt; // lazily computed as needed
    double fT;
    double fOtherT; // value at fOther[fOtherIndex].fT
    int fOtherIndex;  // can't be used during intersection
    int fWindSum; // accumulated from contours surrounding this one.
    int fOppSum; // for binary operators: the opposite winding sum
    int fWindValue; // 0 == canceled; 1 == normal; >1 == coincident
    int fOppValue; // normally 0 -- when binary coincident edges combine, opp value goes here
    bool fDone; // if set, this span to next higher T has been processed
    bool fUnsortableStart; // set when start is part of an unsortable pair
    bool fUnsortableEnd; // set when end is part of an unsortable pair
    bool fTiny; // if set, span may still be considered once for edge following
};

// sorting angles
// given angles of {dx dy ddx ddy dddx dddy} sort them
class Angle {
public:
    // FIXME: this is bogus for quads and cubics
    // if the quads and cubics' line from end pt to ctrl pt are coincident,
    // there's no obvious way to determine the curve ordering from the
    // derivatives alone. In particular, if one quadratic's coincident tangent
    // is longer than the other curve, the final control point can place the
    // longer curve on either side of the shorter one.
    // Using Bezier curve focus http://cagd.cs.byu.edu/~tom/papers/bezclip.pdf
    // may provide some help, but nothing has been figured out yet.

    /*(
    for quads and cubics, set up a parameterized line (e.g. LineParameters )
    for points [0] to [1]. See if point [2] is on that line, or on one side
    or the other. If it both quads' end points are on the same side, choose
    the shorter tangent. If the tangents are equal, choose the better second
    tangent angle

    maybe I could set up LineParameters lazily
    */
    bool operator<(const Angle& rh) const {
        double y = dy();
        double ry = rh.dy();
        if ((y < 0) ^ (ry < 0)) { // OPTIMIZATION: better to use y * ry < 0 ?
            return y < 0;
        }
        double x = dx();
        double rx = rh.dx();
        if (y == 0 && ry == 0 && x * rx < 0) {
            return x < rx;
        }
        double x_ry = x * ry;
        double rx_y = rx * y;
        double cmp = x_ry - rx_y;
        if (!approximately_zero(cmp)) {
            return cmp < 0;
        }
        if (approximately_zero(x_ry) && approximately_zero(rx_y)
                && !approximately_zero_squared(cmp)) {
            return cmp < 0;
        }
        // at this point, the initial tangent line is coincident
        if (fSide * rh.fSide <= 0 && (!approximately_zero(fSide)
                || !approximately_zero(rh.fSide))) {
            // FIXME: running demo will trigger this assertion
            // (don't know if commenting out will trigger further assertion or not)
            // commenting it out allows demo to run in release, though
     //       SkASSERT(fSide != rh.fSide);
            return fSide < rh.fSide;
        }
        // see if either curve can be lengthened and try the tangent compare again
        if (cmp && (*fSpans)[fEnd].fOther != rh.fSegment // tangents not absolutely identical
                && (*rh.fSpans)[rh.fEnd].fOther != fSegment) { // and not intersecting
            Angle longer = *this;
            Angle rhLonger = rh;
            if (longer.lengthen() | rhLonger.lengthen()) {
                return longer < rhLonger;
            }
            // what if we extend in the other direction?
            longer = *this;
            rhLonger = rh;
            if (longer.reverseLengthen() | rhLonger.reverseLengthen()) {
                return longer < rhLonger;
            }
        }
        if ((fVerb == SkPath::kLine_Verb && approximately_zero(x) && approximately_zero(y))
                || (rh.fVerb == SkPath::kLine_Verb
                && approximately_zero(rx) && approximately_zero(ry))) {
            // See general unsortable comment below. This case can happen when
            // one line has a non-zero change in t but no change in x and y.
            fUnsortable = true;
            rh.fUnsortable = true;
            return this < &rh; // even with no solution, return a stable sort
        }
        SkASSERT(fVerb == SkPath::kQuad_Verb); // worry about cubics later
        SkASSERT(rh.fVerb == SkPath::kQuad_Verb);
        // FIXME: until I can think of something better, project a ray from the
        // end of the shorter tangent to midway between the end points
        // through both curves and use the resulting angle to sort
        // FIXME: some of this setup can be moved to set() if it works, or cached if it's expensive
        double len = fTangent1.normalSquared();
        double rlen = rh.fTangent1.normalSquared();
        _Line ray;
        Intersections i, ri;
        int roots, rroots;
        bool flip = false;
        do {
            const Quadratic& q = (len < rlen) ^ flip ? fQ : rh.fQ;
            double midX = (q[0].x + q[2].x) / 2;
            double midY = (q[0].y + q[2].y) / 2;
            ray[0] = q[1];
            ray[1].x = midX;
            ray[1].y = midY;
            SkASSERT(ray[0] != ray[1]);
            roots = QuadRayIntersect(fPts, ray, i);
            rroots = QuadRayIntersect(rh.fPts, ray, ri);
        } while ((roots == 0 || rroots == 0) && (flip ^= true));
        if (roots == 0 || rroots == 0) {
            // FIXME: we don't have a solution in this case. The interim solution
            // is to mark the edges as unsortable, exclude them from this and
            // future computations, and allow the returned path to be fragmented
            fUnsortable = true;
            rh.fUnsortable = true;
            return this < &rh; // even with no solution, return a stable sort
        }
        _Point loc;
        double best = SK_ScalarInfinity;
        double dx, dy, dist;
        int index;
        for (index = 0; index < roots; ++index) {
            QuadXYAtT(fPts, i.fT[0][index], &loc);
            dx = loc.x - ray[0].x;
            dy = loc.y - ray[0].y;
            dist = dx * dx + dy * dy;
            if (best > dist) {
                best = dist;
            }
        }
        for (index = 0; index < rroots; ++index) {
            QuadXYAtT(rh.fPts, ri.fT[0][index], &loc);
            dx = loc.x - ray[0].x;
            dy = loc.y - ray[0].y;
            dist = dx * dx + dy * dy;
            if (best > dist) {
                return fSide < 0;
            }
        }
        return fSide > 0;
    }

    double dx() const {
        return fTangent1.dx();
    }

    double dy() const {
        return fTangent1.dy();
    }

    int end() const {
        return fEnd;
    }

    bool isHorizontal() const {
        return dy() == 0 && fVerb == SkPath::kLine_Verb;
    }

    bool lengthen() {
        int newEnd = fEnd;
        if (fStart < fEnd ? ++newEnd < fSpans->count() : --newEnd >= 0) {
            fEnd = newEnd;
            setSpans();
            return true;
        }
        return false;
    }

    bool reverseLengthen() {
        if (fReversed) {
            return false;
        }
        int newEnd = fStart;
        if (fStart > fEnd ? ++newEnd < fSpans->count() : --newEnd >= 0) {
            fEnd = newEnd;
            fReversed = true;
            setSpans();
            return true;
        }
        return false;
    }

    void set(const SkPoint* orig, SkPath::Verb verb, const Segment* segment,
            int start, int end, const SkTDArray<Span>& spans) {
        fSegment = segment;
        fStart = start;
        fEnd = end;
        fPts = orig;
        fVerb = verb;
        fSpans = &spans;
        fReversed = false;
        fUnsortable = false;
        setSpans();
    }

    void setSpans() {
        double startT = (*fSpans)[fStart].fT;
        double endT = (*fSpans)[fEnd].fT;
        switch (fVerb) {
        case SkPath::kLine_Verb:
            _Line l;
            LineSubDivideHD(fPts, startT, endT, l);
            // OPTIMIZATION: for pure line compares, we never need fTangent1.c
            fTangent1.lineEndPoints(l);
            fUnsortable = dx() == 0 && dy() == 0;
            fSide = 0;
            break;
        case SkPath::kQuad_Verb:
            QuadSubDivideHD(fPts, startT, endT, fQ);
            fTangent1.quadEndPoints(fQ, 0, 1);
            fSide = -fTangent1.pointDistance(fQ[2]); // not normalized -- compare sign only
            break;
        case SkPath::kCubic_Verb:
            Cubic c;
            CubicSubDivideHD(fPts, startT, endT, c);
            fTangent1.cubicEndPoints(c, 0, 1);
            fSide = -fTangent1.pointDistance(c[2]); // not normalized -- compare sign only
            break;
        default:
            SkASSERT(0);
        }
        if (fUnsortable) {
            return;
        }
        SkASSERT(fStart != fEnd);
        int step = fStart < fEnd ? 1 : -1; // OPTIMIZE: worth fStart - fEnd >> 31 type macro?
        for (int index = fStart; index != fEnd; index += step) {
            if ((*fSpans)[index].fUnsortableStart) {
                fUnsortable = true;
                return;
            }
            if (index != fStart && (*fSpans)[index].fUnsortableEnd) {
                fUnsortable = true;
                return;
            }
        }
    }

    Segment* segment() const {
        return const_cast<Segment*>(fSegment);
    }

    int sign() const {
        return SkSign32(fStart - fEnd);
    }

    const SkTDArray<Span>* spans() const {
        return fSpans;
    }

    int start() const {
        return fStart;
    }

    bool unsortable() const {
        return fUnsortable;
    }

#if DEBUG_ANGLE
    const SkPoint* pts() const {
        return fPts;
    }

    SkPath::Verb verb() const {
        return fVerb;
    }

    void debugShow(const SkPoint& a) const {
        SkDebugf("    d=(%1.9g,%1.9g) side=%1.9g\n", dx(), dy(), fSide);
    }
#endif

private:
    const SkPoint* fPts;
    Quadratic fQ;
    SkPath::Verb fVerb;
    double fSide;
    LineParameters fTangent1;
    const SkTDArray<Span>* fSpans;
    const Segment* fSegment;
    int fStart;
    int fEnd;
    bool fReversed;
    mutable bool fUnsortable; // this alone is editable by the less than operator
};

// Bounds, unlike Rect, does not consider a line to be empty.
struct Bounds : public SkRect {
    static bool Intersects(const Bounds& a, const Bounds& b) {
        return a.fLeft <= b.fRight && b.fLeft <= a.fRight &&
                a.fTop <= b.fBottom && b.fTop <= a.fBottom;
    }

    void add(SkScalar left, SkScalar top, SkScalar right, SkScalar bottom) {
        if (left < fLeft) {
            fLeft = left;
        }
        if (top < fTop) {
            fTop = top;
        }
        if (right > fRight) {
            fRight = right;
        }
        if (bottom > fBottom) {
            fBottom = bottom;
        }
    }

    void add(const Bounds& toAdd) {
        add(toAdd.fLeft, toAdd.fTop, toAdd.fRight, toAdd.fBottom);
    }

    bool isEmpty() {
        return fLeft > fRight || fTop > fBottom
                || (fLeft == fRight && fTop == fBottom)
                || isnan(fLeft) || isnan(fRight)
                || isnan(fTop) || isnan(fBottom);
    }

    void setCubicBounds(const SkPoint a[4]) {
        _Rect dRect;
        MAKE_CONST_CUBIC(cubic, a);
        dRect.setBounds(cubic);
        set((float) dRect.left, (float) dRect.top, (float) dRect.right,
                (float) dRect.bottom);
    }

    void setQuadBounds(const SkPoint a[3]) {
        MAKE_CONST_QUAD(quad, a);
        _Rect dRect;
        dRect.setBounds(quad);
        set((float) dRect.left, (float) dRect.top, (float) dRect.right,
                (float) dRect.bottom);
    }
};

// OPTIMIZATION: does the following also work, and is it any faster?
// return outerWinding * innerWinding > 0
//      || ((outerWinding + innerWinding < 0) ^ ((outerWinding - innerWinding) < 0)))
static bool useInnerWinding(int outerWinding, int innerWinding) {
    SkASSERT(outerWinding != innerWinding);
    int absOut = abs(outerWinding);
    int absIn = abs(innerWinding);
    bool result = absOut == absIn ? outerWinding < 0 : absOut < absIn;
    if (outerWinding * innerWinding < 0) {
#if DEBUG_WINDING
        SkDebugf("%s outer=%d inner=%d result=%s\n", __FUNCTION__,
                outerWinding, innerWinding, result ? "true" : "false");
#endif
    }
    return result;
}

static const bool gOpLookup[][2][2] = {
    //     ==0             !=0
    //  b      a        b      a
    {{true , false}, {false, true }}, // a - b
    {{false, false}, {true , true }}, // a & b
    {{true , true }, {false, false}}, // a | b
    {{true , true }, {true , true }}, // a ^ b
};

static bool isActiveOp(bool angleIsOp, int otherNonZero, ShapeOp op) {
    return gOpLookup[op][otherNonZero][angleIsOp];
}

// wrap path to keep track of whether the contour is initialized and non-empty
class PathWrapper {
public:
    PathWrapper(SkPath& path)
        : fPathPtr(&path)
    {
        init();
    }

    void close() {
        if (!fHasMove) {
            return;
        }
        bool callClose = isClosed();
        lineTo();
        if (fEmpty) {
            return;
        }
        if (callClose) {
    #if DEBUG_PATH_CONSTRUCTION
            SkDebugf("path.close();\n");
    #endif
            fPathPtr->close();
        }
        init();
    }

    void cubicTo(const SkPoint& pt1, const SkPoint& pt2, const SkPoint& pt3) {
        lineTo();
        moveTo();
#if DEBUG_PATH_CONSTRUCTION
        SkDebugf("path.cubicTo(%1.9g,%1.9g, %1.9g,%1.9g, %1.9g,%1.9g);\n",
                pt1.fX, pt1.fY, pt2.fX, pt2.fY, pt3.fX, pt3.fY);
#endif
        fPathPtr->cubicTo(pt1.fX, pt1.fY, pt2.fX, pt2.fY, pt3.fX, pt3.fY);
        fDefer[0] = fDefer[1] = pt3;
        fEmpty = false;
    }

    void deferredLine(const SkPoint& pt) {
        if (pt == fDefer[1]) {
            return;
        }
        if (changedSlopes(pt)) {
            lineTo();
            fDefer[0] = fDefer[1];
        }
        fDefer[1] = pt;
    }

    void deferredMove(const SkPoint& pt) {
        fMoved = true;
        fHasMove = true;
        fEmpty = true;
        fDefer[0] = fDefer[1] = pt;
    }

    void deferredMoveLine(const SkPoint& pt) {
        if (!fHasMove) {
            deferredMove(pt);
        }
        deferredLine(pt);
    }

    bool hasMove() const {
        return fHasMove;
    }

    void init() {
        fEmpty = true;
        fHasMove = false;
        fMoved = false;
    }

    bool isClosed() const {
        return !fEmpty && fFirstPt == fDefer[1];
    }

    void lineTo() {
        if (fDefer[0] == fDefer[1]) {
            return;
        }
        moveTo();
        fEmpty = false;
#if DEBUG_PATH_CONSTRUCTION
        SkDebugf("path.lineTo(%1.9g,%1.9g);\n", fDefer[1].fX, fDefer[1].fY);
#endif
        fPathPtr->lineTo(fDefer[1].fX, fDefer[1].fY);
        fDefer[0] = fDefer[1];
    }

    const SkPath* nativePath() const {
        return fPathPtr;
    }

    void quadTo(const SkPoint& pt1, const SkPoint& pt2) {
        lineTo();
        moveTo();
#if DEBUG_PATH_CONSTRUCTION
        SkDebugf("path.quadTo(%1.9g,%1.9g, %1.9g,%1.9g);\n",
                pt1.fX, pt1.fY, pt2.fX, pt2.fY);
#endif
        fPathPtr->quadTo(pt1.fX, pt1.fY, pt2.fX, pt2.fY);
        fDefer[0] = fDefer[1] = pt2;
        fEmpty = false;
    }

protected:
    bool changedSlopes(const SkPoint& pt) const {
        if (fDefer[0] == fDefer[1]) {
            return false;
        }
        SkScalar deferDx = fDefer[1].fX - fDefer[0].fX;
        SkScalar deferDy = fDefer[1].fY - fDefer[0].fY;
        SkScalar lineDx = pt.fX - fDefer[1].fX;
        SkScalar lineDy = pt.fY - fDefer[1].fY;
        return deferDx * lineDy != deferDy * lineDx;
    }

    void moveTo() {
        if (!fMoved) {
            return;
        }
        fFirstPt = fDefer[0];
#if DEBUG_PATH_CONSTRUCTION
        SkDebugf("path.moveTo(%1.9g,%1.9g);\n", fDefer[0].fX, fDefer[0].fY);
#endif
        fPathPtr->moveTo(fDefer[0].fX, fDefer[0].fY);
        fMoved = false;
    }

private:
    SkPath* fPathPtr;
    SkPoint fDefer[2];
    SkPoint fFirstPt;
    bool fEmpty;
    bool fHasMove;
    bool fMoved;
};

class Segment {
public:
    Segment() {
#if DEBUG_DUMP
        fID = ++gSegmentID;
#endif
    }

    bool operator<(const Segment& rh) const {
        return fBounds.fTop < rh.fBounds.fTop;
    }

    bool activeAngle(int index, int& done, SkTDArray<Angle>& angles) const {
        if (activeAngleInner(index, done, angles)) {
            return true;
        }
        double referenceT = fTs[index].fT;
        int lesser = index;
        while (--lesser >= 0 && approximately_negative(referenceT - fTs[lesser].fT)) {
            if (activeAngleOther(lesser, done, angles)) {
                return true;
            }
        }
        do {
            if (activeAngleOther(index, done, angles)) {
                return true;
            }
        } while (++index < fTs.count() && approximately_negative(fTs[index].fT - referenceT));
        return false;
    }

    bool activeAngleOther(int index, int& done, SkTDArray<Angle>& angles) const {
        Span* span = &fTs[index];
        Segment* other = span->fOther;
        int oIndex = span->fOtherIndex;
        return other->activeAngleInner(oIndex, done, angles);
    }

    bool activeAngleInner(int index, int& done, SkTDArray<Angle>& angles) const {
        int next = nextExactSpan(index, 1);
        if (next > 0) {
            const Span& upSpan = fTs[index];
            if (upSpan.fWindValue) {
                addAngle(angles, index, next);
                if (upSpan.fDone || upSpan.fUnsortableEnd) {
                    done++;
                } else if (upSpan.fWindSum != SK_MinS32) {
                    return true;
                }
            }
        }
        int prev = nextExactSpan(index, -1);
        // edge leading into junction
        if (prev >= 0) {
            const Span& downSpan = fTs[prev];
            if (downSpan.fWindValue) {
                addAngle(angles, index, prev);
                if (downSpan.fDone) {
                    done++;
                 } else if (downSpan.fWindSum != SK_MinS32) {
                    return true;
                }
            }
        }
        return false;
    }

    void activeLeftTop(SkPoint& result) const {
        SkASSERT(!done());
        int count = fTs.count();
        result.fY = SK_ScalarMax;
        bool lastDone = true;
        bool lastUnsortable = false;
        for (int index = 0; index < count; ++index) {
            const Span& span = fTs[index];
            if (span.fUnsortableStart | lastUnsortable) {
                goto next;
            }
            if (!span.fDone | !lastDone) {
                const SkPoint& xy = xyAtT(index);
                if (result.fY < xy.fY) {
                    goto next;
                }
                if (result.fY == xy.fY && result.fX < xy.fX) {
                    goto next;
                }
                result = xy;
            }
    next:
            lastDone = span.fDone;
            lastUnsortable = span.fUnsortableEnd;
        }
        SkASSERT(result.fY < SK_ScalarMax);
    }

    bool activeOp(int index, int endIndex, int xorMiMask, int xorSuMask, ShapeOp op) {
        int sumMiWinding = updateWinding(endIndex, index);
        int sumSuWinding = updateOppWinding(endIndex, index);
        if (fOperand) {
            SkTSwap<int>(sumMiWinding, sumSuWinding);
        }
        int maxWinding, sumWinding, oppMaxWinding, oppSumWinding;
        return activeOp(xorMiMask, xorSuMask, index, endIndex, op, sumMiWinding, sumSuWinding,
            maxWinding, sumWinding, oppMaxWinding, oppSumWinding);
    }

    bool activeOp(int xorMiMask, int xorSuMask,
            int index, int endIndex, ShapeOp op,
            int& sumMiWinding, int& sumSuWinding,
            int& maxWinding, int& sumWinding, int& oppMaxWinding, int& oppSumWinding) {
        setUpWindings(index, endIndex, sumMiWinding, sumSuWinding,
                maxWinding, sumWinding, oppMaxWinding, oppSumWinding);
        int mask, oppMask;
        if (operand()) {
            mask = xorSuMask;
            oppMask = xorMiMask;
        } else {
            mask = xorMiMask;
            oppMask = xorSuMask;
        }
        if ((sumWinding & mask) && (maxWinding & mask)) {
            return false;
        }
        int oppCoin = oppSign(index, endIndex) & oppMask;
        if (oppCoin) {
            return op == kIntersect_Op || op == kUnion_Op
                    || (oppSumWinding & oppMask && oppMaxWinding & oppMask);
        }
        bool oppNonZero = oppMaxWinding & oppMask;
        return isActiveOp(operand(), oppNonZero, op);
    }

    void addAngle(SkTDArray<Angle>& angles, int start, int end) const {
        SkASSERT(start != end);
        Angle* angle = angles.append();
#if DEBUG_ANGLE
        if (angles.count() > 1 && !fTs[start].fTiny) {
            SkPoint angle0Pt, newPt;
            (*SegmentXYAtT[angles[0].verb()])(angles[0].pts(),
                    (*angles[0].spans())[angles[0].start()].fT, &angle0Pt);
            (*SegmentXYAtT[fVerb])(fPts, fTs[start].fT, &newPt);
            SkASSERT(approximately_equal(angle0Pt.fX, newPt.fX));
            SkASSERT(approximately_equal(angle0Pt.fY, newPt.fY));
        }
#endif
        angle->set(fPts, fVerb, this, start, end, fTs);
    }

    void addCancelOutsides(double tStart, double oStart, Segment& other,
            double oEnd) {
        int tIndex = -1;
        int tCount = fTs.count();
        int oIndex = -1;
        int oCount = other.fTs.count();
        do {
            ++tIndex;
        } while (!approximately_negative(tStart - fTs[tIndex].fT) && tIndex < tCount);
        int tIndexStart = tIndex;
        do {
            ++oIndex;
        } while (!approximately_negative(oStart - other.fTs[oIndex].fT) && oIndex < oCount);
        int oIndexStart = oIndex;
        double nextT;
        do {
            nextT = fTs[++tIndex].fT;
        } while (nextT < 1 && approximately_negative(nextT - tStart));
        double oNextT;
        do {
            oNextT = other.fTs[++oIndex].fT;
        } while (oNextT < 1 && approximately_negative(oNextT - oStart));
        // at this point, spans before and after are at:
        //  fTs[tIndexStart - 1], fTs[tIndexStart], fTs[tIndex]
        // if tIndexStart == 0, no prior span
        // if nextT == 1, no following span

        // advance the span with zero winding
        // if the following span exists (not past the end, non-zero winding)
        // connect the two edges
        if (!fTs[tIndexStart].fWindValue) {
            if (tIndexStart > 0 && fTs[tIndexStart - 1].fWindValue) {
    #if DEBUG_CONCIDENT
                SkDebugf("%s 1 this=%d other=%d t [%d] %1.9g (%1.9g,%1.9g)\n",
                        __FUNCTION__, fID, other.fID, tIndexStart - 1,
                        fTs[tIndexStart].fT, xyAtT(tIndexStart).fX,
                        xyAtT(tIndexStart).fY);
    #endif
                addTPair(fTs[tIndexStart].fT, other, other.fTs[oIndex].fT, false);
            }
            if (nextT < 1 && fTs[tIndex].fWindValue) {
    #if DEBUG_CONCIDENT
                SkDebugf("%s 2 this=%d other=%d t [%d] %1.9g (%1.9g,%1.9g)\n",
                        __FUNCTION__, fID, other.fID, tIndex,
                        fTs[tIndex].fT, xyAtT(tIndex).fX,
                        xyAtT(tIndex).fY);
    #endif
                addTPair(fTs[tIndex].fT, other, other.fTs[oIndexStart].fT, false);
            }
        } else {
            SkASSERT(!other.fTs[oIndexStart].fWindValue);
            if (oIndexStart > 0 && other.fTs[oIndexStart - 1].fWindValue) {
    #if DEBUG_CONCIDENT
                SkDebugf("%s 3 this=%d other=%d t [%d] %1.9g (%1.9g,%1.9g)\n",
                        __FUNCTION__, fID, other.fID, oIndexStart - 1,
                        other.fTs[oIndexStart].fT, other.xyAtT(oIndexStart).fX,
                        other.xyAtT(oIndexStart).fY);
                other.debugAddTPair(other.fTs[oIndexStart].fT, *this, fTs[tIndex].fT);
    #endif
            }
            if (oNextT < 1 && other.fTs[oIndex].fWindValue) {
    #if DEBUG_CONCIDENT
                SkDebugf("%s 4 this=%d other=%d t [%d] %1.9g (%1.9g,%1.9g)\n",
                        __FUNCTION__, fID, other.fID, oIndex,
                        other.fTs[oIndex].fT, other.xyAtT(oIndex).fX,
                        other.xyAtT(oIndex).fY);
                other.debugAddTPair(other.fTs[oIndex].fT, *this, fTs[tIndexStart].fT);
    #endif
            }
        }
    }

    void addCoinOutsides(const SkTDArray<double>& outsideTs, Segment& other,
            double oEnd) {
        // walk this to outsideTs[0]
        // walk other to outsideTs[1]
        // if either is > 0, add a pointer to the other, copying adjacent winding
        int tIndex = -1;
        int oIndex = -1;
        double tStart = outsideTs[0];
        double oStart = outsideTs[1];
        do {
            ++tIndex;
        } while (!approximately_negative(tStart - fTs[tIndex].fT));
        do {
            ++oIndex;
        } while (!approximately_negative(oStart - other.fTs[oIndex].fT));
        if (tIndex > 0 || oIndex > 0) {
            addTPair(tStart, other, oStart, false);
        }
        tStart = fTs[tIndex].fT;
        oStart = other.fTs[oIndex].fT;
        do {
            double nextT;
            do {
                nextT = fTs[++tIndex].fT;
            } while (approximately_negative(nextT - tStart));
            tStart = nextT;
            do {
                nextT = other.fTs[++oIndex].fT;
            } while (approximately_negative(nextT - oStart));
            oStart = nextT;
            if (tStart == 1 && oStart == 1) {
                break;
            }
            addTPair(tStart, other, oStart, false);
        } while (tStart < 1 && oStart < 1 && !approximately_negative(oEnd - oStart));
    }

    void addCubic(const SkPoint pts[4], bool operand) {
        init(pts, SkPath::kCubic_Verb, operand);
        fBounds.setCubicBounds(pts);
    }

    /* SkPoint */ void addCurveTo(int start, int end, PathWrapper& path, bool active) const {
        SkPoint edge[4];
        const SkPoint* ePtr;
        int lastT = fTs.count() - 1;
        if (lastT < 0 || (start == 0 && end == lastT) || (start == lastT && end == 0)) {
            ePtr = fPts;
        } else {
        // OPTIMIZE? if not active, skip remainder and return xy_at_t(end)
            (*SegmentSubDivide[fVerb])(fPts, fTs[start].fT, fTs[end].fT, edge);
            ePtr = edge;
        }
        if (active) {
            bool reverse = ePtr == fPts && start != 0;
            if (reverse) {
                path.deferredMoveLine(ePtr[fVerb]);
                switch (fVerb) {
                    case SkPath::kLine_Verb:
                        path.deferredLine(ePtr[0]);
                        break;
                    case SkPath::kQuad_Verb:
                        path.quadTo(ePtr[1], ePtr[0]);
                        break;
                    case SkPath::kCubic_Verb:
                        path.cubicTo(ePtr[2], ePtr[1], ePtr[0]);
                        break;
                    default:
                        SkASSERT(0);
                }
       //         return ePtr[0];
           } else {
                path.deferredMoveLine(ePtr[0]);
                switch (fVerb) {
                    case SkPath::kLine_Verb:
                        path.deferredLine(ePtr[1]);
                        break;
                    case SkPath::kQuad_Verb:
                        path.quadTo(ePtr[1], ePtr[2]);
                        break;
                    case SkPath::kCubic_Verb:
                        path.cubicTo(ePtr[1], ePtr[2], ePtr[3]);
                        break;
                    default:
                        SkASSERT(0);
                }
            }
        }
      //  return ePtr[fVerb];
    }

    void addLine(const SkPoint pts[2], bool operand) {
        init(pts, SkPath::kLine_Verb, operand);
        fBounds.set(pts, 2);
    }

#if 0
    const SkPoint& addMoveTo(int tIndex, PathWrapper& path, bool active) const {
        const SkPoint& pt = xyAtT(tIndex);
        if (active) {
            path.deferredMove(pt);
        }
        return pt;
    }
#endif

    // add 2 to edge or out of range values to get T extremes
    void addOtherT(int index, double otherT, int otherIndex) {
        Span& span = fTs[index];
    #if PIN_ADD_T
        if (precisely_less_than_zero(otherT)) {
            otherT = 0;
        } else if (precisely_greater_than_one(otherT)) {
            otherT = 1;
        }
    #endif
        span.fOtherT = otherT;
        span.fOtherIndex = otherIndex;
    }

    void addQuad(const SkPoint pts[3], bool operand) {
        init(pts, SkPath::kQuad_Verb, operand);
        fBounds.setQuadBounds(pts);
    }

    // Defer all coincident edge processing until
    // after normal intersections have been computed

// no need to be tricky; insert in normal T order
// resolve overlapping ts when considering coincidence later

    // add non-coincident intersection. Resulting edges are sorted in T.
    int addT(double newT, Segment* other) {
        // FIXME: in the pathological case where there is a ton of intercepts,
        //  binary search?
        int insertedAt = -1;
        size_t tCount = fTs.count();
    #if PIN_ADD_T
        // FIXME: only do this pinning here (e.g. this is done also in quad/line intersect)
        if (precisely_less_than_zero(newT)) {
            newT = 0;
        } else if (precisely_greater_than_one(newT)) {
            newT = 1;
        }
    #endif
        for (size_t index = 0; index < tCount; ++index) {
            // OPTIMIZATION: if there are three or more identical Ts, then
            // the fourth and following could be further insertion-sorted so
            // that all the edges are clockwise or counterclockwise.
            // This could later limit segment tests to the two adjacent
            // neighbors, although it doesn't help with determining which
            // circular direction to go in.
            if (newT < fTs[index].fT) {
                insertedAt = index;
                break;
            }
        }
        Span* span;
        if (insertedAt >= 0) {
            span = fTs.insert(insertedAt);
        } else {
            insertedAt = tCount;
            span = fTs.append();
        }
        span->fT = newT;
        span->fOther = other;
        span->fPt.fX = SK_ScalarNaN;
        span->fWindSum = SK_MinS32;
        span->fOppSum = SK_MinS32;
        span->fWindValue = 1;
        span->fOppValue = 0;
        span->fTiny = false;
        if ((span->fDone = newT == 1)) {
            ++fDoneSpans;
        }
        span->fUnsortableStart = false;
        span->fUnsortableEnd = false;
        if (span - fTs.begin() > 0 && !span[-1].fDone
                && !precisely_negative(newT - span[-1].fT)
 //               && approximately_negative(newT - span[-1].fT)
                && xyAtT(&span[-1]) == xyAtT(span)) {
            span[-1].fTiny = true;
            span[-1].fDone = true;
            if (approximately_negative(newT - span[-1].fT)) {
                if (approximately_greater_than_one(newT)) {
                    span[-1].fUnsortableStart = true;
                    span[-2].fUnsortableEnd = true;
                }
                if (approximately_less_than_zero(span[-1].fT)) {
                    span->fUnsortableStart = true;
                    span[-1].fUnsortableEnd = true;
                }
            }
            ++fDoneSpans;
        }
        if (fTs.end() - span > 1 && !span->fDone
                && !precisely_negative(span[1].fT - newT)
 //               && approximately_negative(span[1].fT - newT)
                && xyAtT(&span[1]) == xyAtT(span)) {
            span->fTiny = true;
            span->fDone = true;
            if (approximately_negative(span[1].fT - newT)) {
                if (approximately_greater_than_one(span[1].fT)) {
                    span->fUnsortableStart = true;
                    span[-1].fUnsortableEnd = true;
                }
                if (approximately_less_than_zero(newT)) {
                    span[1].fUnsortableStart = true;
                    span->fUnsortableEnd = true;
                }
            }
            ++fDoneSpans;
        }
        return insertedAt;
    }

    // set spans from start to end to decrement by one
    // note this walks other backwards
    // FIMXE: there's probably an edge case that can be constructed where
    // two span in one segment are separated by float epsilon on one span but
    // not the other, if one segment is very small. For this
    // case the counts asserted below may or may not be enough to separate the
    // spans. Even if the counts work out, what if the spans aren't correctly
    // sorted? It feels better in such a case to match the span's other span
    // pointer since both coincident segments must contain the same spans.
    void addTCancel(double startT, double endT, Segment& other,
            double oStartT, double oEndT) {
        SkASSERT(!approximately_negative(endT - startT));
        SkASSERT(!approximately_negative(oEndT - oStartT));
        bool binary = fOperand != other.fOperand;
        int index = 0;
        while (!approximately_negative(startT - fTs[index].fT)) {
            ++index;
        }
        int oIndex = other.fTs.count();
        while (approximately_positive(other.fTs[--oIndex].fT - oEndT))
            ;
        double tRatio = (oEndT - oStartT) / (endT - startT);
        Span* test = &fTs[index];
        Span* oTest = &other.fTs[oIndex];
        SkTDArray<double> outsideTs;
        SkTDArray<double> oOutsideTs;
        do {
            bool decrement = test->fWindValue && oTest->fWindValue && !binary;
            bool track = test->fWindValue || oTest->fWindValue;
            double testT = test->fT;
            double oTestT = oTest->fT;
            Span* span = test;
            do {
                if (decrement) {
                    decrementSpan(span);
                } else if (track && span->fT < 1 && oTestT < 1) {
                    TrackOutside(outsideTs, span->fT, oTestT);
                }
                span = &fTs[++index];
            } while (approximately_negative(span->fT - testT));
            Span* oSpan = oTest;
            double otherTMatchStart = oEndT - (span->fT - startT) * tRatio;
            double otherTMatchEnd = oEndT - (test->fT - startT) * tRatio;
            SkDEBUGCODE(int originalWindValue = oSpan->fWindValue);
            while (approximately_negative(otherTMatchStart - oSpan->fT)
                    && !approximately_negative(otherTMatchEnd - oSpan->fT)) {
        #ifdef SK_DEBUG
                SkASSERT(originalWindValue == oSpan->fWindValue);
        #endif
                if (decrement) {
                    other.decrementSpan(oSpan);
                } else if (track && oSpan->fT < 1 && testT < 1) {
                    TrackOutside(oOutsideTs, oSpan->fT, testT);
                }
                if (!oIndex) {
                    break;
                }
                oSpan = &other.fTs[--oIndex];
            }
            test = span;
            oTest = oSpan;
        } while (!approximately_negative(endT - test->fT));
        SkASSERT(!oIndex || approximately_negative(oTest->fT - oStartT));
        // FIXME: determine if canceled edges need outside ts added
        if (!done() && outsideTs.count()) {
            double tStart = outsideTs[0];
            double oStart = outsideTs[1];
            addCancelOutsides(tStart, oStart, other, oEndT);
            int count = outsideTs.count();
            if (count > 2) {
                double tStart = outsideTs[count - 2];
                double oStart = outsideTs[count - 1];
                addCancelOutsides(tStart, oStart, other, oEndT);
            }
        }
        if (!other.done() && oOutsideTs.count()) {
            double tStart = oOutsideTs[0];
            double oStart = oOutsideTs[1];
            other.addCancelOutsides(tStart, oStart, *this, endT);
        }
    }

    int bumpCoincidentThis(const Span* oTest, const bool transfer, const bool decrementThis,
            const bool thisXor, const int oXorMask, const bool opp, int index,
            SkTDArray<double>& outsideTs, SkTDArray<double>& xOutsideTs)
    {
        Span* const test = &fTs[index];
        Span* end = test;
        const int startIndex = index;
        const double oStartT = oTest->fT;
        do {
            if (transfer) {
                if (opp) {
                    if (decrementThis) {
                        zeroSpan(end, oStartT);
                        TrackOutside(outsideTs, end->fT, oStartT);
                    } else {
                        end->fWindValue += oTest->fOppValue;
                        end->fOppValue = (end->fOppValue + oTest->fWindValue) & oXorMask;
                        if (thisXor) {
                            SkASSERT(end->fWindValue);
                            if (!(end->fWindValue & 1)) {
                                zeroSpan(end, oStartT);
                                TrackOutside(outsideTs, end->fT, oStartT);
                            }
                        }
                    }
                } else if (!decrementThis & !thisXor) {
            #ifdef SK_DEBUG
                    SkASSERT(abs(end->fWindValue) < gDebugMaxWindValue);
            #endif
                    ++(end->fWindValue);
                } else if (decrementSpan(end)) {
                    TrackOutside(outsideTs, end->fT, oStartT);
                }
            } else if (oTest->fWindValue) {
                SkASSERT(decrementThis);
                if (startIndex > 0 && fTs[startIndex - 1].fWindValue) {
                    TrackOutside(xOutsideTs, end->fT, oStartT);
                }
            }
            end = &fTs[++index];
        } while (approximately_negative(end->fT - test->fT));
        return index;
    }

    // because of the order in which coincidences are resolved, this and other
    // may not have the same intermediate points. Compute the corresponding
    // intermediate T values (using this as the master, other as the follower)
    // and walk other conditionally -- hoping that it catches up in the end
    int bumpCoincidentOther(const Span* test, const bool transfer, const bool decrementThis,
            const bool otherXor, const int xorMask, const bool opp, const double tRatio,
            const double oEndT, int& oIndex, SkTDArray<double>& oOutsideTs)
    {
        Span* const oTest = &fTs[oIndex];
        Span* oEnd = oTest;
        const double startT = test->fT;
        const int oStartIndex = oIndex;
        const double oStartT = oTest->fT;
        double otherTMatch = (test->fT - startT) * tRatio + oStartT;
        while (!approximately_negative(oEndT - oEnd->fT)
                && approximately_negative(oEnd->fT - otherTMatch)) {
            if (transfer) {
                if (opp) {
                    if (decrementThis) {
                        oEnd->fWindValue += test->fOppValue;
                        oEnd->fOppValue = (oEnd->fOppValue + test->fWindValue) & xorMask;
                        if (otherXor) {
                            SkASSERT(oEnd->fWindValue);
                            if (!(oEnd->fWindValue & 1)) {
                                zeroSpan(oEnd, startT);
                                TrackOutside(oOutsideTs, oEnd->fT, startT);
                            }
                        }
                    } else {
                        zeroSpan(oEnd, startT);
                        TrackOutside(oOutsideTs, oEnd->fT, startT);
                    }
                } else if (decrementThis & !otherXor) {
             #ifdef SK_DEBUG
                   SkASSERT(abs(oEnd->fWindValue) < gDebugMaxWindValue);
            #endif
                    ++(oEnd->fWindValue);
                } else if (decrementSpan(oEnd)) {
                    TrackOutside(oOutsideTs, oEnd->fT, startT);
                }
            } else if (test->fWindValue) {
                SkASSERT(decrementThis);
                if (oStartIndex > 0 && fTs[oStartIndex - 1].fWindValue) {
                    SkASSERT(0); // track for later?
                }
            }
            oEnd = &fTs[++oIndex];
        }
        return oIndex;
    }

    // FIXME: need to test this case:
    // contourA has two segments that are coincident
    // contourB has two segments that are coincident in the same place
    // each ends up with +2/0 pairs for winding count
    // since logic below doesn't transfer count (only increments/decrements) can this be
    // resolved to +4/0 ?

    // set spans from start to end to increment the greater by one and decrement
    // the lesser
    void addTCoincident(bool thisXor, bool otherXor, double startT, double endT,
            Segment& other, double oStartT, double oEndT) {
        SkASSERT(!approximately_negative(endT - startT));
        SkASSERT(!approximately_negative(oEndT - oStartT));
        bool opp = fOperand ^ other.fOperand;
        if (!opp) {
            otherXor = thisXor;
        }
        int index = 0;
        while (!approximately_negative(startT - fTs[index].fT)) {
            ++index;
        }
        int oIndex = 0;
        while (!approximately_negative(oStartT - other.fTs[oIndex].fT)) {
            ++oIndex;
        }
        double tRatio = (oEndT - oStartT) / (endT - startT);
        Span* test = &fTs[index];
        Span* oTest = &other.fTs[oIndex];
        SkTDArray<double> outsideTs;
        SkTDArray<double> xOutsideTs;
        SkTDArray<double> oOutsideTs;
        int xorMask = thisXor ? 1 : -1;
        int oXorMask = otherXor ? 1 : -1;
        do {
            bool transfer = test->fWindValue && oTest->fWindValue;
            bool decrementThis = test->fWindValue < oTest->fWindValue ||
                    (test->fWindValue == oTest->fWindValue && thisXor);
            if (decrementThis) {
                oIndex = other.bumpCoincidentOther(test, transfer, decrementThis, otherXor, xorMask,
                        opp, tRatio, oEndT, oIndex, oOutsideTs);
                index = bumpCoincidentThis(oTest, transfer, decrementThis, thisXor, oXorMask, opp,
                        index, outsideTs, xOutsideTs);
            } else {
                index = bumpCoincidentThis(oTest, transfer, decrementThis, thisXor, oXorMask, opp,
                        index, outsideTs, xOutsideTs);
                oIndex = other.bumpCoincidentOther(test, transfer, decrementThis, otherXor, xorMask,
                        opp, tRatio, oEndT, oIndex, oOutsideTs);
            }
            test = &fTs[index];
            oTest = &other.fTs[oIndex];
        } while (!approximately_negative(endT - test->fT));
        SkASSERT(approximately_negative(oTest->fT - oEndT));
        SkASSERT(approximately_negative(oEndT - oTest->fT));
        if (!done()) {
            if (outsideTs.count()) {
                addCoinOutsides(outsideTs, other, oEndT);
            }
            if (xOutsideTs.count()) {
                addCoinOutsides(xOutsideTs, other, oEndT);
            }
        }
        if (!other.done() && oOutsideTs.count()) {
            other.addCoinOutsides(oOutsideTs, *this, endT);
        }
    }

    // FIXME: this doesn't prevent the same span from being added twice
    // fix in caller, assert here?
    void addTPair(double t, Segment& other, double otherT, bool borrowWind) {
        int tCount = fTs.count();
        for (int tIndex = 0; tIndex < tCount; ++tIndex) {
            const Span& span = fTs[tIndex];
            if (!approximately_negative(span.fT - t)) {
                break;
            }
            if (approximately_negative(span.fT - t) && span.fOther == &other
                    && approximately_equal(span.fOtherT, otherT)) {
#if DEBUG_ADD_T_PAIR
                SkDebugf("%s addTPair duplicate this=%d %1.9g other=%d %1.9g\n",
                        __FUNCTION__, fID, t, other.fID, otherT);
#endif
                return;
            }
        }
#if DEBUG_ADD_T_PAIR
        SkDebugf("%s addTPair this=%d %1.9g other=%d %1.9g\n",
                __FUNCTION__, fID, t, other.fID, otherT);
#endif
        int insertedAt = addT(t, &other);
        int otherInsertedAt = other.addT(otherT, this);
        addOtherT(insertedAt, otherT, otherInsertedAt);
        other.addOtherT(otherInsertedAt, t, insertedAt);
        matchWindingValue(insertedAt, t, borrowWind);
        other.matchWindingValue(otherInsertedAt, otherT, borrowWind);
    }

    void addTwoAngles(int start, int end, SkTDArray<Angle>& angles) const {
        // add edge leading into junction
        if (fTs[SkMin32(end, start)].fWindValue > 0) {
            addAngle(angles, end, start);
        }
        // add edge leading away from junction
        int step = SkSign32(end - start);
        int tIndex = nextExactSpan(end, step);
        if (tIndex >= 0 && fTs[SkMin32(end, tIndex)].fWindValue > 0) {
            addAngle(angles, end, tIndex);
        }
    }

    const Bounds& bounds() const {
        return fBounds;
    }

    void buildAngles(int index, SkTDArray<Angle>& angles, bool includeOpp) const {
        double referenceT = fTs[index].fT;
        int lesser = index;
        while (--lesser >= 0 && (includeOpp || fTs[lesser].fOther->fOperand == fOperand)
                && precisely_negative(referenceT - fTs[lesser].fT)) {
            buildAnglesInner(lesser, angles);
        }
        do {
            buildAnglesInner(index, angles);
        } while (++index < fTs.count() && (includeOpp || fTs[index].fOther->fOperand == fOperand)
                && precisely_negative(fTs[index].fT - referenceT));
    }

    void buildAnglesInner(int index, SkTDArray<Angle>& angles) const {
        Span* span = &fTs[index];
        Segment* other = span->fOther;
    // if there is only one live crossing, and no coincidence, continue
    // in the same direction
    // if there is coincidence, the only choice may be to reverse direction
        // find edge on either side of intersection
        int oIndex = span->fOtherIndex;
        // if done == -1, prior span has already been processed
        int step = 1;
        int next = other->nextExactSpan(oIndex, step);
       if (next < 0) {
            step = -step;
            next = other->nextExactSpan(oIndex, step);
        }
        // add candidate into and away from junction
        other->addTwoAngles(next, oIndex, angles);
    }

    int computeSum(int startIndex, int endIndex, bool binary) {
        SkTDArray<Angle> angles;
        addTwoAngles(startIndex, endIndex, angles);
        buildAngles(endIndex, angles, false);
        // OPTIMIZATION: check all angles to see if any have computed wind sum
        // before sorting (early exit if none)
        SkTDArray<Angle*> sorted;
        bool sortable = SortAngles(angles, sorted);
#if DEBUG_SORT
        sorted[0]->segment()->debugShowSort(__FUNCTION__, sorted, 0, 0, 0);
#endif
        if (!sortable) {
            return SK_MinS32;
        }
        int angleCount = angles.count();
        const Angle* angle;
        const Segment* base;
        int winding;
        int oWinding;
        int firstIndex = 0;
        do {
            angle = sorted[firstIndex];
            base = angle->segment();
            winding = base->windSum(angle);
            if (winding != SK_MinS32) {
                oWinding = base->oppSum(angle);
                break;
            }
            if (++firstIndex == angleCount) {
                return SK_MinS32;
            }
        } while (true);
        // turn winding into contourWinding
        int spanWinding = base->spanSign(angle);
        bool inner = useInnerWinding(winding + spanWinding, winding);
    #if DEBUG_WINDING
        SkDebugf("%s spanWinding=%d winding=%d sign=%d inner=%d result=%d\n", __FUNCTION__,
            spanWinding, winding, angle->sign(), inner,
            inner ? winding + spanWinding : winding);
    #endif
        if (inner) {
            winding += spanWinding;
        }
    #if DEBUG_SORT
        base->debugShowSort(__FUNCTION__, sorted, firstIndex, winding, oWinding);
    #endif
        int nextIndex = firstIndex + 1;
        int lastIndex = firstIndex != 0 ? firstIndex : angleCount;
        winding -= base->spanSign(angle);
        oWinding -= base->oppSign(angle);
        do {
            if (nextIndex == angleCount) {
                nextIndex = 0;
            }
            angle = sorted[nextIndex];
            Segment* segment = angle->segment();
            bool opp = base->fOperand ^ segment->fOperand;
            int maxWinding, oMaxWinding;
            int spanSign = segment->spanSign(angle);
            int oppoSign = segment->oppSign(angle);
            if (opp) {
                oMaxWinding = oWinding;
                oWinding -= spanSign;
                maxWinding = winding;
                if (oppoSign) {
                    winding -= oppoSign;
                }
            } else {
                maxWinding = winding;
                winding -= spanSign;
                oMaxWinding = oWinding;
                if (oppoSign) {
                    oWinding -= oppoSign;
                }
            }
            if (segment->windSum(angle) == SK_MinS32) {
                if (opp) {
                    if (useInnerWinding(oMaxWinding, oWinding)) {
                        oMaxWinding = oWinding;
                    }
                    if (oppoSign && useInnerWinding(maxWinding, winding)) {
                        maxWinding = winding;
                    }
                    segment->markAndChaseWinding(angle, oMaxWinding, maxWinding);
                } else {
                    if (useInnerWinding(maxWinding, winding)) {
                        maxWinding = winding;
                    }
                    if (oppoSign && useInnerWinding(oMaxWinding, oWinding)) {
                        oMaxWinding = oWinding;
                    }
                    segment->markAndChaseWinding(angle, maxWinding, binary ? oMaxWinding : 0);
                }
            }
        } while (++nextIndex != lastIndex);
        int minIndex = SkMin32(startIndex, endIndex);
        return windSum(minIndex);
    }

    int crossedSpan(const SkPoint& basePt, SkScalar& bestY, double& hitT) const {
        int bestT = -1;
        SkScalar top = bounds().fTop;
        SkScalar bottom = bounds().fBottom;
        int end = 0;
        do {
            int start = end;
            end = nextSpan(start, 1);
            if (fTs[start].fWindValue == 0) {
                continue;
            }
            SkPoint edge[4];
            double startT = fTs[start].fT;
            double endT = fTs[end].fT;
            (*SegmentSubDivide[fVerb])(fPts, startT, endT, edge);
            // intersect ray starting at basePt with edge
            Intersections intersections;
            // FIXME: always use original and limit results to T values within
            // start t and end t.
            // OPTIMIZE: use specialty function that intersects ray with curve,
            // returning t values only for curve (we don't care about t on ray)
            int pts = (*VSegmentIntersect[fVerb])(edge, top, bottom, basePt.fX,
                    false, intersections);
            if (pts == 0) {
                continue;
            }
            if (pts > 1 && fVerb == SkPath::kLine_Verb) {
            // if the intersection is edge on, wait for another one
                continue;
            }
            for (int index = 0; index < pts; ++index) {
                SkPoint pt;
                double foundT = intersections.fT[0][index];
                double testT = startT + (endT - startT) * foundT;
                (*SegmentXYAtT[fVerb])(fPts, testT, &pt);
                if (bestY < pt.fY && pt.fY < basePt.fY) {
                    if (fVerb > SkPath::kLine_Verb
                            && !approximately_less_than_zero(foundT)
                            && !approximately_greater_than_one(foundT)) {
                        SkScalar dx = (*SegmentDXAtT[fVerb])(fPts, testT);
                        if (approximately_zero(dx)) {
                            continue;
                        }
                    }
                    bestY = pt.fY;
                    bestT = foundT < 1 ? start : end;
                    hitT = testT;
                }
            }
        } while (fTs[end].fT != 1);
        return bestT;
    }

    bool crossedSpanHalves(const SkPoint& basePt, bool leftHalf, bool rightHalf) {
        // if a segment is connected to this one, consider it crossing
        int tIndex;
        if (fPts[0].fX == basePt.fX) {
            tIndex = 0;
            do {
                const Span& sSpan = fTs[tIndex];
                const Segment* sOther = sSpan.fOther;
                if (!sOther->fTs[sSpan.fOtherIndex].fWindValue) {
                    continue;
                }
                if (leftHalf ? sOther->fBounds.fLeft < basePt.fX
                        : sOther->fBounds.fRight > basePt.fX) {
                    return true;
                }
            } while (fTs[++tIndex].fT == 0);
        }
        if (fPts[fVerb].fX == basePt.fX) {
            tIndex = fTs.count() - 1;
            do {
                const Span& eSpan = fTs[tIndex];
                const Segment* eOther = eSpan.fOther;
                if (!eOther->fTs[eSpan.fOtherIndex].fWindValue) {
                    continue;
                }
                if (leftHalf ? eOther->fBounds.fLeft < basePt.fX
                        : eOther->fBounds.fRight > basePt.fX) {
                    return true;
                }
            } while (fTs[--tIndex].fT == 1);
        }
        return false;
    }

    bool decrementSpan(Span* span) {
        SkASSERT(span->fWindValue > 0);
        if (--(span->fWindValue) == 0) {
            if (!span->fDone) {
                span->fDone = true;
                ++fDoneSpans;
            }
            return true;
        }
        return false;
    }

    bool done() const {
        SkASSERT(fDoneSpans <= fTs.count());
        return fDoneSpans == fTs.count();
    }

    bool done(int min) const {
        return fTs[min].fDone;
    }

    bool done(const Angle* angle) const {
        return done(SkMin32(angle->start(), angle->end()));
    }

    /*
     The M and S variable name parts stand for the operators.
       Mi stands for Minuend (see wiki subtraction, analogous to difference)
       Su stands for Subtrahend
     The Opp variable name part designates that the value is for the Opposite operator.
     Opposite values result from combining coincident spans.
     */

    Segment* findNextOp(SkTDArray<Span*>& chase, int& nextStart, int& nextEnd,
            bool& unsortable, ShapeOp op, const int xorMiMask, const int xorSuMask) {
        const int startIndex = nextStart;
        const int endIndex = nextEnd;
        SkASSERT(startIndex != endIndex);
        const int count = fTs.count();
        SkASSERT(startIndex < endIndex ? startIndex < count - 1 : startIndex > 0);
        const int step = SkSign32(endIndex - startIndex);
        const int end = nextExactSpan(startIndex, step);
        SkASSERT(end >= 0);
        Span* endSpan = &fTs[end];
        Segment* other;
        if (isSimple(end)) {
        // mark the smaller of startIndex, endIndex done, and all adjacent
        // spans with the same T value (but not 'other' spans)
    #if DEBUG_WINDING
            SkDebugf("%s simple\n", __FUNCTION__);
    #endif
            markDoneBinary(SkMin32(startIndex, endIndex));
            other = endSpan->fOther;
            nextStart = endSpan->fOtherIndex;
            double startT = other->fTs[nextStart].fT;
            nextEnd = nextStart;
            do {
                nextEnd += step;
            }
            while (precisely_zero(startT - other->fTs[nextEnd].fT));
            SkASSERT(step < 0 ? nextEnd >= 0 : nextEnd < other->fTs.count());
            return other;
        }
        // more than one viable candidate -- measure angles to find best
        SkTDArray<Angle> angles;
        SkASSERT(startIndex - endIndex != 0);
        SkASSERT((startIndex - endIndex < 0) ^ (step < 0));
        addTwoAngles(startIndex, end, angles);
        buildAngles(end, angles, true);
        SkTDArray<Angle*> sorted;
        bool sortable = SortAngles(angles, sorted);
        int angleCount = angles.count();
        int firstIndex = findStartingEdge(sorted, startIndex, end);
        SkASSERT(firstIndex >= 0);
    #if DEBUG_SORT
        debugShowSort(__FUNCTION__, sorted, firstIndex);
    #endif
        if (!sortable) {
            unsortable = true;
            return NULL;
        }
        SkASSERT(sorted[firstIndex]->segment() == this);
    #if DEBUG_WINDING
        SkDebugf("%s firstIndex=[%d] sign=%d\n", __FUNCTION__, firstIndex,
                sorted[firstIndex]->sign());
    #endif
        int sumMiWinding = updateWinding(endIndex, startIndex);
        int sumSuWinding = updateOppWinding(endIndex, startIndex);
        if (operand()) {
            SkTSwap<int>(sumMiWinding, sumSuWinding);
        }
        int nextIndex = firstIndex + 1;
        int lastIndex = firstIndex != 0 ? firstIndex : angleCount;
        const Angle* foundAngle = NULL;
        bool foundDone = false;
        // iterate through the angle, and compute everyone's winding
        Segment* nextSegment;
        do {
            SkASSERT(nextIndex != firstIndex);
            if (nextIndex == angleCount) {
                nextIndex = 0;
            }
            const Angle* nextAngle = sorted[nextIndex];
            nextSegment = nextAngle->segment();
            int maxWinding, sumWinding, oppMaxWinding, oppSumWinding;
            bool activeAngle = nextSegment->activeOp(xorMiMask, xorSuMask, nextAngle->start(),
                    nextAngle->end(), op, sumMiWinding, sumSuWinding,
                    maxWinding, sumWinding, oppMaxWinding, oppSumWinding);
            if (activeAngle && (!foundAngle || foundDone)) {
                foundAngle = nextAngle;
                foundDone = nextSegment->done(nextAngle) && !nextSegment->tiny(nextAngle);
            }
            if (nextSegment->done()) {
                continue;
            }
            if (nextSegment->windSum(nextAngle) != SK_MinS32) {
                continue;
            }
            Span* last = nextSegment->markAngle(maxWinding, sumWinding, oppMaxWinding,
                    oppSumWinding, activeAngle, nextAngle);
            if (last) {
                *chase.append() = last;
#if DEBUG_WINDING
                SkDebugf("%s chase.append id=%d\n", __FUNCTION__,
                        last->fOther->fTs[last->fOtherIndex].fOther->debugID());
#endif
            }
        } while (++nextIndex != lastIndex);
        markDoneBinary(SkMin32(startIndex, endIndex));
        if (!foundAngle) {
            return NULL;
        }
        nextStart = foundAngle->start();
        nextEnd = foundAngle->end();
        nextSegment = foundAngle->segment();

    #if DEBUG_WINDING
        SkDebugf("%s from:[%d] to:[%d] start=%d end=%d\n",
                __FUNCTION__, debugID(), nextSegment->debugID(), nextStart, nextEnd);
     #endif
        return nextSegment;
    }

    // so the span needs to contain the pairing info found here
    // this should include the winding computed for the edge, and
    //  what edge it connects to, and whether it is discarded
    //  (maybe discarded == abs(winding) > 1) ?
    // only need derivatives for duration of sorting, add a new struct
    // for pairings, remove extra spans that have zero length and
    //  reference an unused other
    // for coincident, the last span on the other may be marked done
    //  (always?)

    // if loop is exhausted, contour may be closed.
    // FIXME: pass in close point so we can check for closure

    // given a segment, and a sense of where 'inside' is, return the next
    // segment. If this segment has an intersection, or ends in multiple
    // segments, find the mate that continues the outside.
    // note that if there are multiples, but no coincidence, we can limit
    // choices to connections in the correct direction

    // mark found segments as done

    // start is the index of the beginning T of this edge
    // it is guaranteed to have an end which describes a non-zero length (?)
    // winding -1 means ccw, 1 means cw
    Segment* findNextWinding(SkTDArray<Span*>& chase, bool active,
            int& nextStart, int& nextEnd, int& winding, int& spanWinding,
            bool& unsortable) {
        const int startIndex = nextStart;
        const int endIndex = nextEnd;
        int outerWinding = winding;
        int innerWinding = winding + spanWinding;
    #if DEBUG_WINDING
        SkDebugf("%s winding=%d spanWinding=%d outerWinding=%d innerWinding=%d\n",
                __FUNCTION__, winding, spanWinding, outerWinding, innerWinding);
    #endif
        if (useInnerWinding(outerWinding, innerWinding)) {
            outerWinding = innerWinding;
        }
        SkASSERT(startIndex != endIndex);
        int count = fTs.count();
        SkASSERT(startIndex < endIndex ? startIndex < count - 1
                : startIndex > 0);
        int step = SkSign32(endIndex - startIndex);
        int end = nextExactSpan(startIndex, step);
        SkASSERT(end >= 0);
        Span* endSpan = &fTs[end];
        Segment* other;
        if (isSimple(end)) {
        // mark the smaller of startIndex, endIndex done, and all adjacent
        // spans with the same T value (but not 'other' spans)
    #if DEBUG_WINDING
            SkDebugf("%s simple\n", __FUNCTION__);
    #endif
            markDone(SkMin32(startIndex, endIndex), outerWinding);
            other = endSpan->fOther;
            nextStart = endSpan->fOtherIndex;
            double startT = other->fTs[nextStart].fT;
            nextEnd = nextStart;
            do {
                nextEnd += step;
            }
             while (precisely_zero(startT - other->fTs[nextEnd].fT));
            SkASSERT(step < 0 ? nextEnd >= 0 : nextEnd < other->fTs.count());
            return other;
        }
        // more than one viable candidate -- measure angles to find best
        SkTDArray<Angle> angles;
        SkASSERT(startIndex - endIndex != 0);
        SkASSERT((startIndex - endIndex < 0) ^ (step < 0));
        addTwoAngles(startIndex, end, angles);
        buildAngles(end, angles, false);
        SkTDArray<Angle*> sorted;
        bool sortable = SortAngles(angles, sorted);
        int angleCount = angles.count();
        int firstIndex = findStartingEdge(sorted, startIndex, end);
        SkASSERT(firstIndex >= 0);
    #if DEBUG_SORT
        debugShowSort(__FUNCTION__, sorted, firstIndex, winding, 0);
    #endif
        if (!sortable) {
            unsortable = true;
            return NULL;
        }
        SkASSERT(sorted[firstIndex]->segment() == this);
    #if DEBUG_WINDING
        SkDebugf("%s [%d] sign=%d\n", __FUNCTION__, firstIndex, sorted[firstIndex]->sign());
    #endif
        int sumWinding = winding - spanSign(sorted[firstIndex]);
        int nextIndex = firstIndex + 1;
        int lastIndex = firstIndex != 0 ? firstIndex : angleCount;
        const Angle* foundAngle = NULL;
        // FIXME: found done logic probably fails if there are more than 4
        // sorted angles. It should bias towards the first and last undone
        // edges -- but not sure that it won't choose a middle (incorrect)
        // edge if one is undone
        bool foundDone = false;
        bool foundDone2 = false;
        // iterate through the angle, and compute everyone's winding
        bool altFlipped = false;
        bool foundFlipped = false;
        int foundSum = SK_MinS32;
        Segment* nextSegment;
        int lastNonZeroSum = winding;
        do {
            if (nextIndex == angleCount) {
                nextIndex = 0;
            }
            const Angle* nextAngle = sorted[nextIndex];
            int maxWinding = sumWinding;
            if (sumWinding) {
                lastNonZeroSum = sumWinding;
            }
            nextSegment = nextAngle->segment();
            bool nextDone = nextSegment->done(nextAngle);
            bool nextTiny = nextSegment->tiny(nextAngle);
            sumWinding -= nextSegment->spanSign(nextAngle);
            altFlipped ^= lastNonZeroSum * sumWinding < 0; // flip if different signs
    #if 0 && DEBUG_WINDING
            SkASSERT(abs(sumWinding) <= gDebugMaxWindSum);
            SkDebugf("%s [%d] maxWinding=%d sumWinding=%d sign=%d altFlipped=%d\n", __FUNCTION__,
                    nextIndex, maxWinding, sumWinding, nextAngle->sign(), altFlipped);
    #endif
           if (!sumWinding) {
                if (!active) {
                    // FIXME: shouldn't this call mark and chase done ?
                    markDone(SkMin32(startIndex, endIndex), outerWinding);
                    // FIXME: shouldn't this call mark and chase winding ?
                    nextSegment->markWinding(SkMin32(nextAngle->start(),
                                nextAngle->end()), maxWinding);
    #if DEBUG_WINDING
                    SkDebugf("%s [%d] inactive\n", __FUNCTION__, nextIndex);
    #endif
                    return NULL;
                }
                if (!foundAngle || foundDone) {
                    foundAngle = nextAngle;
                    foundDone = nextDone && !nextTiny;
                    foundFlipped = altFlipped;
                }
                continue;
            }

            if (!maxWinding && (!foundAngle || foundDone2)) {
        #if DEBUG_WINDING
                if (foundAngle && foundDone2) {
                    SkDebugf("%s [%d] !foundAngle && foundDone2\n", __FUNCTION__, nextIndex);
                }
        #endif
                foundAngle = nextAngle;
                foundDone2 = nextDone && !nextTiny;
                foundFlipped = altFlipped;
                foundSum = sumWinding;
            }
            if (nextSegment->done()) {
                continue;
            }
            // if the winding is non-zero, nextAngle does not connect to
            // current chain. If we haven't done so already, mark the angle
            // as done, record the winding value, and mark connected unambiguous
            // segments as well.
            if (nextSegment->windSum(nextAngle) == SK_MinS32) {
                if (useInnerWinding(maxWinding, sumWinding)) {
                    maxWinding = sumWinding;
                }
                Span* last;
                if (foundAngle) {
                    last = nextSegment->markAndChaseWinding(nextAngle, maxWinding);
                } else {
                    last = nextSegment->markAndChaseDone(nextAngle, maxWinding);
                }
                if (last) {
                    *chase.append() = last;
    #if DEBUG_WINDING
                    SkDebugf("%s chase.append id=%d\n", __FUNCTION__,
                            last->fOther->fTs[last->fOtherIndex].fOther->debugID());
    #endif
                }
            }
        } while (++nextIndex != lastIndex);
        markDone(SkMin32(startIndex, endIndex), outerWinding);
        if (!foundAngle) {
            return NULL;
        }
        nextStart = foundAngle->start();
        nextEnd = foundAngle->end();
        nextSegment = foundAngle->segment();
        int flipped = foundFlipped ? -1 : 1;
        spanWinding = SkSign32(spanWinding) * flipped * nextSegment->windValue(
                SkMin32(nextStart, nextEnd));
        if (winding) {
    #if DEBUG_WINDING
            SkDebugf("%s ---6 winding=%d foundSum=", __FUNCTION__, winding);
            if (foundSum == SK_MinS32) {
                SkDebugf("?");
            } else {
                SkDebugf("%d", foundSum);
            }
            SkDebugf("\n");
     #endif
            winding = foundSum;
        }
    #if DEBUG_WINDING
        SkDebugf("%s spanWinding=%d flipped=%d\n", __FUNCTION__, spanWinding, flipped);
    #endif
        return nextSegment;
    }

    Segment* findNextXor(int& nextStart, int& nextEnd, bool& unsortable) {
        const int startIndex = nextStart;
        const int endIndex = nextEnd;
        SkASSERT(startIndex != endIndex);
        int count = fTs.count();
        SkASSERT(startIndex < endIndex ? startIndex < count - 1
                : startIndex > 0);
        int step = SkSign32(endIndex - startIndex);
        int end = nextExactSpan(startIndex, step);
        SkASSERT(end >= 0);
        Span* endSpan = &fTs[end];
        Segment* other;
        markDone(SkMin32(startIndex, endIndex), 1);
        if (isSimple(end)) {
    #if DEBUG_WINDING
            SkDebugf("%s simple\n", __FUNCTION__);
    #endif
            other = endSpan->fOther;
            nextStart = endSpan->fOtherIndex;
            double startT = other->fTs[nextStart].fT;
            SkDEBUGCODE(bool firstLoop = true;)
            if ((approximately_less_than_zero(startT) && step < 0)
                    || (approximately_greater_than_one(startT) && step > 0)) {
                step = -step;
                SkDEBUGCODE(firstLoop = false;)
            }
            do {
                nextEnd = nextStart;
                do {
                    nextEnd += step;
                }
                 while (precisely_zero(startT - other->fTs[nextEnd].fT));
                if (other->fTs[SkMin32(nextStart, nextEnd)].fWindValue) {
                    break;
                }
 #ifdef SK_DEBUG
                SkASSERT(firstLoop);
 #endif
                SkDEBUGCODE(firstLoop = false;)
                step = -step;
            } while (true);
            SkASSERT(step < 0 ? nextEnd >= 0 : nextEnd < other->fTs.count());
            return other;
        }
        SkTDArray<Angle> angles;
        SkASSERT(startIndex - endIndex != 0);
        SkASSERT((startIndex - endIndex < 0) ^ (step < 0));
        addTwoAngles(startIndex, end, angles);
        buildAngles(end, angles, false);
        SkTDArray<Angle*> sorted;
        bool sortable = SortAngles(angles, sorted);
        int angleCount = angles.count();
        int firstIndex = findStartingEdge(sorted, startIndex, end);
        SkASSERT(firstIndex >= 0);
    #if DEBUG_SORT
        debugShowSort(__FUNCTION__, sorted, firstIndex, 0, 0);
    #endif
        if (!sortable) {
            unsortable = true;
            return NULL;
        }
        SkASSERT(sorted[firstIndex]->segment() == this);
        int nextIndex = firstIndex + 1;
        int lastIndex = firstIndex != 0 ? firstIndex : angleCount;
        const Angle* nextAngle;
        Segment* nextSegment;
        do {
            if (nextIndex == angleCount) {
                nextIndex = 0;
            }
            nextAngle = sorted[nextIndex];
            nextSegment = nextAngle->segment();
            if (!nextSegment->done(nextAngle)) {
                break;
            }
            if (++nextIndex == lastIndex) {
                return NULL;
            }
        } while (true);
        nextStart = nextAngle->start();
        nextEnd = nextAngle->end();
        return nextSegment;
    }

    int findStartingEdge(SkTDArray<Angle*>& sorted, int start, int end) {
        int angleCount = sorted.count();
        int firstIndex = -1;
        for (int angleIndex = 0; angleIndex < angleCount; ++angleIndex) {
            const Angle* angle = sorted[angleIndex];
            if (angle->segment() == this && angle->start() == end &&
                    angle->end() == start) {
                firstIndex = angleIndex;
                break;
            }
        }
        return firstIndex;
    }

    // FIXME: this is tricky code; needs its own unit test
    void findTooCloseToCall(bool thisXor, bool otherXor) {
        int count = fTs.count();
        if (count < 3) { // require t=0, x, 1 at minimum
            return;
        }
        int matchIndex = 0;
        int moCount;
        Span* match;
        Segment* mOther;
        do {
            match = &fTs[matchIndex];
            mOther = match->fOther;
            // FIXME: allow quads, cubics to be near coincident?
            if (mOther->fVerb == SkPath::kLine_Verb) {
                moCount = mOther->fTs.count();
                if (moCount >= 3) {
                    break;
                }
            }
            if (++matchIndex >= count) {
                return;
            }
        } while (true); // require t=0, x, 1 at minimum
        // OPTIMIZATION: defer matchPt until qualifying toCount is found?
        const SkPoint* matchPt = &xyAtT(match);
        // look for a pair of nearby T values that map to the same (x,y) value
        // if found, see if the pair of other segments share a common point. If
        // so, the span from here to there is coincident.
        for (int index = matchIndex + 1; index < count; ++index) {
            Span* test = &fTs[index];
            if (test->fDone) {
                continue;
            }
            Segment* tOther = test->fOther;
            if (tOther->fVerb != SkPath::kLine_Verb) {
                continue; // FIXME: allow quads, cubics to be near coincident?
            }
            int toCount = tOther->fTs.count();
            if (toCount < 3) { // require t=0, x, 1 at minimum
                continue;
            }
            const SkPoint* testPt = &xyAtT(test);
            if (*matchPt != *testPt) {
                matchIndex = index;
                moCount = toCount;
                match = test;
                mOther = tOther;
                matchPt = testPt;
                continue;
            }
            int moStart = -1;
            int moEnd = -1;
            double moStartT, moEndT;
            for (int moIndex = 0; moIndex < moCount; ++moIndex) {
                Span& moSpan = mOther->fTs[moIndex];
                if (moSpan.fDone) {
                    continue;
                }
                if (moSpan.fOther == this) {
                    if (moSpan.fOtherT == match->fT) {
                        moStart = moIndex;
                        moStartT = moSpan.fT;
                    }
                    continue;
                }
                if (moSpan.fOther == tOther) {
                    if (tOther->fTs[moSpan.fOtherIndex].fWindValue == 0) {
                        moStart = -1;
                        break;
                    }
                    SkASSERT(moEnd == -1);
                    moEnd = moIndex;
                    moEndT = moSpan.fT;
                }
            }
            if (moStart < 0 || moEnd < 0) {
                continue;
            }
            // FIXME: if moStartT, moEndT are initialized to NaN, can skip this test
            if (approximately_equal(moStartT, moEndT)) {
                continue;
            }
            int toStart = -1;
            int toEnd = -1;
            double toStartT, toEndT;
            for (int toIndex = 0; toIndex < toCount; ++toIndex) {
                Span& toSpan = tOther->fTs[toIndex];
                if (toSpan.fDone) {
                    continue;
                }
                if (toSpan.fOther == this) {
                    if (toSpan.fOtherT == test->fT) {
                        toStart = toIndex;
                        toStartT = toSpan.fT;
                    }
                    continue;
                }
                if (toSpan.fOther == mOther && toSpan.fOtherT == moEndT) {
                    if (mOther->fTs[toSpan.fOtherIndex].fWindValue == 0) {
                        moStart = -1;
                        break;
                    }
                    SkASSERT(toEnd == -1);
                    toEnd = toIndex;
                    toEndT = toSpan.fT;
                }
            }
            // FIXME: if toStartT, toEndT are initialized to NaN, can skip this test
            if (toStart <= 0 || toEnd <= 0) {
                continue;
            }
            if (approximately_equal(toStartT, toEndT)) {
                continue;
            }
            // test to see if the segment between there and here is linear
            if (!mOther->isLinear(moStart, moEnd)
                    || !tOther->isLinear(toStart, toEnd)) {
                continue;
            }
            bool flipped = (moStart - moEnd) * (toStart - toEnd) < 1;
            if (flipped) {
                mOther->addTCancel(moStartT, moEndT, *tOther, toEndT, toStartT);
            } else {
                // FIXME: this is bogus for multiple ops
                // the xorMask needs to be accumulated from the union of the two
                // edges -- which means that the segment must have its own copy of the mask
                mOther->addTCoincident(thisXor, otherXor,
                        moStartT, moEndT, *tOther, toStartT, toEndT);
            }
        }
    }

 //   start here;
    // either:
    // a) mark spans with either end unsortable as done, or
    // b) rewrite findTop / findTopSegment / findTopContour to iterate further
    //    when encountering an unsortable span

    // OPTIMIZATION : for a pair of lines, can we compute points at T (cached)
    // and use more concise logic like the old edge walker code?
    // FIXME: this needs to deal with coincident edges
    Segment* findTop(int& tIndex, int& endIndex) {
        // iterate through T intersections and return topmost
        // topmost tangent from y-min to first pt is closer to horizontal
        SkASSERT(!done());
        int firstT;
        int lastT;
        SkPoint topPt;
        topPt.fY = SK_ScalarMax;
        int count = fTs.count();
        // see if either end is not done since we want smaller Y of the pair
        bool lastDone = true;
        bool lastUnsortable = false;
        for (int index = 0; index < count; ++index) {
            const Span& span = fTs[index];
            if (span.fUnsortableStart | lastUnsortable) {
                goto next;
            }
            if (!span.fDone | !lastDone) {
                const SkPoint& intercept = xyAtT(&span);
                if (topPt.fY > intercept.fY || (topPt.fY == intercept.fY
                        && topPt.fX > intercept.fX)) {
                    topPt = intercept;
                    firstT = lastT = index;
                } else if (topPt == intercept) {
                    lastT = index;
                }
            }
    next:
            lastDone = span.fDone;
            lastUnsortable = span.fUnsortableEnd;
        }
        // sort the edges to find the leftmost
        int step = 1;
        int end = nextSpan(firstT, step);
        if (end == -1) {
            step = -1;
            end = nextSpan(firstT, step);
            SkASSERT(end != -1);
        }
        // if the topmost T is not on end, or is three-way or more, find left
        // look for left-ness from tLeft to firstT (matching y of other)
        SkTDArray<Angle> angles;
        SkASSERT(firstT - end != 0);
        addTwoAngles(end, firstT, angles);
        buildAngles(firstT, angles, true);
        SkTDArray<Angle*> sorted;
        bool sortable = SortAngles(angles, sorted);
    #if DEBUG_SORT
        sorted[0]->segment()->debugShowSort(__FUNCTION__, sorted, 0, 0, 0);
    #endif
        if (!sortable) {
            return NULL;
        }
        // skip edges that have already been processed
        firstT = -1;
        Segment* leftSegment;
        do {
            const Angle* angle = sorted[++firstT];
            SkASSERT(!angle->unsortable());
            leftSegment = angle->segment();
            tIndex = angle->end();
            endIndex = angle->start();
        } while (leftSegment->fTs[SkMin32(tIndex, endIndex)].fDone);
        return leftSegment;
    }

    // FIXME: not crazy about this
    // when the intersections are performed, the other index is into an
    // incomplete array. as the array grows, the indices become incorrect
    // while the following fixes the indices up again, it isn't smart about
    // skipping segments whose indices are already correct
    // assuming we leave the code that wrote the index in the first place
    void fixOtherTIndex() {
        int iCount = fTs.count();
        for (int i = 0; i < iCount; ++i) {
            Span& iSpan = fTs[i];
            double oT = iSpan.fOtherT;
            Segment* other = iSpan.fOther;
            int oCount = other->fTs.count();
            for (int o = 0; o < oCount; ++o) {
                Span& oSpan = other->fTs[o];
                if (oT == oSpan.fT && this == oSpan.fOther) {
                    iSpan.fOtherIndex = o;
                    break;
                }
            }
        }
    }

    // OPTIMIZATION: uses tail recursion. Unwise?
    Span* innerChaseDone(int index, int step, int winding) {
        int end = nextExactSpan(index, step);
        SkASSERT(end >= 0);
        if (multipleSpans(end)) {
            return &fTs[end];
        }
        const Span& endSpan = fTs[end];
        Segment* other = endSpan.fOther;
        index = endSpan.fOtherIndex;
        int otherEnd = other->nextExactSpan(index, step);
        Span* last = other->innerChaseDone(index, step, winding);
        other->markDone(SkMin32(index, otherEnd), winding);
        return last;
    }

    Span* innerChaseDoneBinary(int index, int step, int winding, int oppWinding) {
        int end = nextExactSpan(index, step);
        SkASSERT(end >= 0);
        if (multipleSpans(end)) {
            return &fTs[end];
        }
        const Span& endSpan = fTs[end];
        Segment* other = endSpan.fOther;
        index = endSpan.fOtherIndex;
        int otherEnd = other->nextExactSpan(index, step);
        Span* last = other->innerChaseDoneBinary(index, step, winding, oppWinding);
        other->markDoneBinary(SkMin32(index, otherEnd), winding, oppWinding);
        return last;
    }

    Span* innerChaseDoneBinary(int index, int step) {
        int end = nextExactSpan(index, step);
        SkASSERT(end >= 0);
        if (multipleSpans(end)) {
            return &fTs[end];
        }
        const Span& endSpan = fTs[end];
        Segment* other = endSpan.fOther;
        index = endSpan.fOtherIndex;
        int otherEnd = other->nextExactSpan(index, step);
        Span* last = other->innerChaseDoneBinary(index, step);
        other->markDoneBinary(SkMin32(index, otherEnd));
        return last;
    }

    Span* innerChaseWinding(int index, int step, int winding) {
        int end = nextExactSpan(index, step);
        SkASSERT(end >= 0);
        if (multipleSpans(end)) {
            return &fTs[end];
        }
        const Span& endSpan = fTs[end];
        Segment* other = endSpan.fOther;
        index = endSpan.fOtherIndex;
        int otherEnd = other->nextExactSpan(index, step);
        int min = SkMin32(index, otherEnd);
        if (other->fTs[min].fWindSum != SK_MinS32) {
            SkASSERT(other->fTs[min].fWindSum == winding);
            return NULL;
        }
        Span* last = other->innerChaseWinding(index, step, winding);
        other->markWinding(min, winding);
        return last;
    }

    Span* innerChaseWinding(int index, int step, int winding, int oppWinding) {
        int end = nextExactSpan(index, step);
        SkASSERT(end >= 0);
        if (multipleSpans(end)) {
            return &fTs[end];
        }
        const Span& endSpan = fTs[end];
        Segment* other = endSpan.fOther;
        index = endSpan.fOtherIndex;
        int otherEnd = other->nextExactSpan(index, step);
        int min = SkMin32(index, otherEnd);
        if (other->fTs[min].fWindSum != SK_MinS32) {
            SkASSERT(other->fTs[min].fWindSum == winding);
            return NULL;
        }
        Span* last = other->innerChaseWinding(index, step, winding, oppWinding);
        other->markWinding(min, winding, oppWinding);
        return last;
    }

    void init(const SkPoint pts[], SkPath::Verb verb, bool operand) {
        fDoneSpans = 0;
        fOperand = operand;
        fPts = pts;
        fVerb = verb;
    }

    void initWinding(int start, int end, int winding, int oppWinding) {
        int local = spanSign(start, end);
        if (local * winding >= 0) {
            winding += local;
        }
        local = oppSign(start, end);
        if (local * oppWinding >= 0) {
            oppWinding += local;
        }
        markAndChaseWinding(start, end, winding, oppWinding);
    }

    bool intersected() const {
        return fTs.count() > 0;
    }

    bool isConnected(int startIndex, int endIndex) const {
        return fTs[startIndex].fWindSum != SK_MinS32
                || fTs[endIndex].fWindSum != SK_MinS32;
    }

    bool isHorizontal() const {
        return fBounds.fTop == fBounds.fBottom;
    }

    bool isLinear(int start, int end) const {
        if (fVerb == SkPath::kLine_Verb) {
            return true;
        }
        if (fVerb == SkPath::kQuad_Verb) {
            SkPoint qPart[3];
            QuadSubDivide(fPts, fTs[start].fT, fTs[end].fT, qPart);
            return QuadIsLinear(qPart);
        } else {
            SkASSERT(fVerb == SkPath::kCubic_Verb);
            SkPoint cPart[4];
            CubicSubDivide(fPts, fTs[start].fT, fTs[end].fT, cPart);
            return CubicIsLinear(cPart);
        }
    }

    // OPTIMIZE: successive calls could start were the last leaves off
    // or calls could specialize to walk forwards or backwards
    bool isMissing(double startT) const {
        size_t tCount = fTs.count();
        for (size_t index = 0; index < tCount; ++index) {
            if (approximately_zero(startT - fTs[index].fT)) {
                return false;
            }
        }
        return true;
    }

    bool isSimple(int end) const {
        int count = fTs.count();
        if (count == 2) {
            return true;
        }
        double t = fTs[end].fT;
        if (approximately_less_than_zero(t)) {
            return !approximately_less_than_zero(fTs[1].fT);
        }
        if (approximately_greater_than_one(t)) {
            return !approximately_greater_than_one(fTs[count - 2].fT);
        }
        return false;
    }

    bool isVertical() const {
        return fBounds.fLeft == fBounds.fRight;
    }

    SkScalar leftMost(int start, int end) const {
        return (*SegmentLeftMost[fVerb])(fPts, fTs[start].fT, fTs[end].fT);
    }

    // this span is excluded by the winding rule -- chase the ends
    // as long as they are unambiguous to mark connections as done
    // and give them the same winding value
    Span* markAndChaseDone(const Angle* angle, int winding) {
        int index = angle->start();
        int endIndex = angle->end();
        return markAndChaseDone(index, endIndex, winding);
    }

    Span* markAndChaseDone(int index, int endIndex, int winding) {
        int step = SkSign32(endIndex - index);
        Span* last = innerChaseDone(index, step, winding);
        markDone(SkMin32(index, endIndex), winding);
        return last;
    }

    Span* markAndChaseDoneBinary(const Angle* angle, int winding, int oppWinding) {
        int index = angle->start();
        int endIndex = angle->end();
        int step = SkSign32(endIndex - index);
        Span* last = innerChaseDoneBinary(index, step, winding, oppWinding);
        markDoneBinary(SkMin32(index, endIndex), winding, oppWinding);
        return last;
    }

    Span* markAndChaseDoneBinary(int index, int endIndex) {
        int step = SkSign32(endIndex - index);
        Span* last = innerChaseDoneBinary(index, step);
        markDoneBinary(SkMin32(index, endIndex));
        return last;
    }

    Span* markAndChaseWinding(const Angle* angle, int winding) {
        int index = angle->start();
        int endIndex = angle->end();
        int min = SkMin32(index, endIndex);
        int step = SkSign32(endIndex - index);
        Span* last = innerChaseWinding(index, step, winding);
        markWinding(min, winding);
        return last;
    }

    Span* markAndChaseWinding(int index, int endIndex, int winding, int oppWinding) {
        int min = SkMin32(index, endIndex);
        int step = SkSign32(endIndex - index);
        Span* last = innerChaseWinding(index, step, winding, oppWinding);
        markWinding(min, winding, oppWinding);
        return last;
    }

    Span* markAndChaseWinding(const Angle* angle, int winding, int oppWinding) {
        int start = angle->start();
        int end = angle->end();
        return markAndChaseWinding(start, end, winding, oppWinding);
    }

    Span* markAngle(int maxWinding, int sumWinding, int oppMaxWinding, int oppSumWinding,
            bool activeAngle, const Angle* angle) {
        SkASSERT(angle->segment() == this);
        if (useInnerWinding(maxWinding, sumWinding)) {
            maxWinding = sumWinding;
        }
        if (oppMaxWinding != oppSumWinding && useInnerWinding(oppMaxWinding, oppSumWinding)) {
            oppMaxWinding = oppSumWinding;
        }
        Span* last;
        if (activeAngle) {
            last = markAndChaseWinding(angle, maxWinding, oppMaxWinding);
        } else {
            last = markAndChaseDoneBinary(angle, maxWinding, oppMaxWinding);
        }
        return last;
    }

    // FIXME: this should also mark spans with equal (x,y)
    // This may be called when the segment is already marked done. While this
    // wastes time, it shouldn't do any more than spin through the T spans.
    // OPTIMIZATION: abort on first done found (assuming that this code is
    // always called to mark segments done).
    void markDone(int index, int winding) {
      //  SkASSERT(!done());
        SkASSERT(winding);
        double referenceT = fTs[index].fT;
        int lesser = index;
        while (--lesser >= 0 && precisely_negative(referenceT - fTs[lesser].fT)) {
            markOneDone(__FUNCTION__, lesser, winding);
        }
        do {
            markOneDone(__FUNCTION__, index, winding);
        } while (++index < fTs.count() && precisely_negative(fTs[index].fT - referenceT));
    }

    void markDoneBinary(int index, int winding, int oppWinding) {
      //  SkASSERT(!done());
        SkASSERT(winding);
        double referenceT = fTs[index].fT;
        int lesser = index;
        while (--lesser >= 0 && precisely_negative(referenceT - fTs[lesser].fT)) {
            markOneDoneBinary(__FUNCTION__, lesser, winding, oppWinding);
        }
        do {
            markOneDoneBinary(__FUNCTION__, index, winding, oppWinding);
        } while (++index < fTs.count() && precisely_negative(fTs[index].fT - referenceT));
    }

    void markDoneBinary(int index) {
        double referenceT = fTs[index].fT;
        int lesser = index;
        while (--lesser >= 0 && precisely_negative(referenceT - fTs[lesser].fT)) {
            markOneDoneBinary(__FUNCTION__, lesser);
        }
        do {
            markOneDoneBinary(__FUNCTION__, index);
        } while (++index < fTs.count() && precisely_negative(fTs[index].fT - referenceT));
    }

    void markOneDone(const char* funName, int tIndex, int winding) {
        Span* span = markOneWinding(funName, tIndex, winding);
        if (!span) {
            return;
        }
        span->fDone = true;
        fDoneSpans++;
    }

    void markOneDoneBinary(const char* funName, int tIndex) {
        Span* span = verifyOneWinding(funName, tIndex);
        if (!span) {
            return;
        }
        span->fDone = true;
        fDoneSpans++;
    }

    void markOneDoneBinary(const char* funName, int tIndex, int winding, int oppWinding) {
        Span* span = markOneWinding(funName, tIndex, winding, oppWinding);
        if (!span) {
            return;
        }
        span->fDone = true;
        fDoneSpans++;
    }

    Span* markOneWinding(const char* funName, int tIndex, int winding) {
        Span& span = fTs[tIndex];
        if (span.fDone) {
            return NULL;
        }
    #if DEBUG_MARK_DONE
        debugShowNewWinding(funName, span, winding);
    #endif
        SkASSERT(span.fWindSum == SK_MinS32 || span.fWindSum == winding);
   #ifdef SK_DEBUG
        SkASSERT(abs(winding) <= gDebugMaxWindSum);
   #endif
        span.fWindSum = winding;
        return &span;
    }

    Span* markOneWinding(const char* funName, int tIndex, int winding, int oppWinding) {
        Span& span = fTs[tIndex];
        if (span.fDone) {
            return NULL;
        }
    #if DEBUG_MARK_DONE
        debugShowNewWinding(funName, span, winding, oppWinding);
    #endif
        SkASSERT(span.fWindSum == SK_MinS32 || span.fWindSum == winding);
   #ifdef SK_DEBUG
        SkASSERT(abs(winding) <= gDebugMaxWindSum);
   #endif
        span.fWindSum = winding;
        SkASSERT(span.fOppSum == SK_MinS32 || span.fOppSum == oppWinding);
   #ifdef SK_DEBUG
        SkASSERT(abs(oppWinding) <= gDebugMaxWindSum);
   #endif
        span.fOppSum = oppWinding;
        return &span;
    }

    Span* verifyOneWinding(const char* funName, int tIndex) {
        Span& span = fTs[tIndex];
        if (span.fDone) {
            return NULL;
        }
    #if DEBUG_MARK_DONE
        debugShowNewWinding(funName, span, span.fWindSum, span.fOppSum);
    #endif
        SkASSERT(span.fWindSum != SK_MinS32);
        SkASSERT(span.fOppSum != SK_MinS32);
        return &span;
    }

    // note that just because a span has one end that is unsortable, that's
    // not enough to mark it done. The other end may be sortable, allowing the
    // span to be added.
    void markUnsortable(int start, int end) {
        Span* span = &fTs[start];
        if (start < end) {
            span->fUnsortableStart = true;
        } else {
            --span;
            span->fUnsortableEnd = true;
        }
        if (!span->fUnsortableStart || !span->fUnsortableEnd || span->fDone) {
            return;
        }
        span->fDone = true;
        fDoneSpans++;
    }

    void markWinding(int index, int winding) {
    //    SkASSERT(!done());
        SkASSERT(winding);
        double referenceT = fTs[index].fT;
        int lesser = index;
        while (--lesser >= 0 && precisely_negative(referenceT - fTs[lesser].fT)) {
            markOneWinding(__FUNCTION__, lesser, winding);
        }
        do {
            markOneWinding(__FUNCTION__, index, winding);
       } while (++index < fTs.count() && precisely_negative(fTs[index].fT - referenceT));
    }

    void markWinding(int index, int winding, int oppWinding) {
    //    SkASSERT(!done());
        SkASSERT(winding);
        double referenceT = fTs[index].fT;
        int lesser = index;
        while (--lesser >= 0 && precisely_negative(referenceT - fTs[lesser].fT)) {
            markOneWinding(__FUNCTION__, lesser, winding, oppWinding);
        }
        do {
            markOneWinding(__FUNCTION__, index, winding, oppWinding);
       } while (++index < fTs.count() && precisely_negative(fTs[index].fT - referenceT));
    }

    void matchWindingValue(int tIndex, double t, bool borrowWind) {
        int nextDoorWind = SK_MaxS32;
        int nextOppWind = SK_MaxS32;
        if (tIndex > 0) {
            const Span& below = fTs[tIndex - 1];
            if (approximately_negative(t - below.fT)) {
                nextDoorWind = below.fWindValue;
                nextOppWind = below.fOppValue;
            }
        }
        if (nextDoorWind == SK_MaxS32 && tIndex + 1 < fTs.count()) {
            const Span& above = fTs[tIndex + 1];
            if (approximately_negative(above.fT - t)) {
                nextDoorWind = above.fWindValue;
                nextOppWind = above.fOppValue;
            }
        }
        if (nextDoorWind == SK_MaxS32 && borrowWind && tIndex > 0 && t < 1) {
            const Span& below = fTs[tIndex - 1];
            nextDoorWind = below.fWindValue;
            nextOppWind = below.fOppValue;
        }
        if (nextDoorWind != SK_MaxS32) {
            Span& newSpan = fTs[tIndex];
            newSpan.fWindValue = nextDoorWind;
            newSpan.fOppValue = nextOppWind;
            if (!nextDoorWind && !newSpan.fDone) {
                newSpan.fDone = true;
                ++fDoneSpans;
            }
        }
    }

    // return span if when chasing, two or more radiating spans are not done
    // OPTIMIZATION: ? multiple spans is detected when there is only one valid
    // candidate and the remaining spans have windValue == 0 (canceled by
    // coincidence). The coincident edges could either be removed altogether,
    // or this code could be more complicated in detecting this case. Worth it?
    bool multipleSpans(int end) const {
        return end > 0 && end < fTs.count() - 1;
    }

    // This has callers for two different situations: one establishes the end
    // of the current span, and one establishes the beginning of the next span
    // (thus the name). When this is looking for the end of the current span,
    // coincidence is found when the beginning Ts contain -step and the end
    // contains step. When it is looking for the beginning of the next, the
    // first Ts found can be ignored and the last Ts should contain -step.
    // OPTIMIZATION: probably should split into two functions
    int nextSpan(int from, int step) const {
        const Span& fromSpan = fTs[from];
        int count = fTs.count();
        int to = from;
        while (step > 0 ? ++to < count : --to >= 0) {
            const Span& span = fTs[to];
            if (approximately_zero(span.fT - fromSpan.fT)) {
                continue;
            }
            return to;
        }
        return -1;
    }

    // FIXME
    // this returns at any difference in T, vs. a preset minimum. It may be
    // that all callers to nextSpan should use this instead.
    // OPTIMIZATION splitting this into separate loops for up/down steps
    // would allow using precisely_negative instead of precisely_zero
    int nextExactSpan(int from, int step) const {
        const Span& fromSpan = fTs[from];
        int count = fTs.count();
        int to = from;
        while (step > 0 ? ++to < count : --to >= 0) {
            const Span& span = fTs[to];
            if (precisely_zero(span.fT - fromSpan.fT)) {
                continue;
            }
            return to;
        }
        return -1;
    }

    bool operand() const {
        return fOperand;
    }

    int oppSign(const Angle* angle) const {
        SkASSERT(angle->segment() == this);
        return oppSign(angle->start(), angle->end());
    }

    int oppSign(int startIndex, int endIndex) const {
        int result = startIndex < endIndex ? -fTs[startIndex].fOppValue
                : fTs[endIndex].fOppValue;
#if DEBUG_WIND_BUMP
        SkDebugf("%s oppSign=%d\n", __FUNCTION__, result);
#endif
        return result;
    }

    int oppSum(int tIndex) const {
        return fTs[tIndex].fOppSum;
    }

    int oppSum(const Angle* angle) const {
        int lesser = SkMin32(angle->start(), angle->end());
        return fTs[lesser].fOppSum;
    }

    int oppValue(int tIndex) const {
        return fTs[tIndex].fOppValue;
    }

    const SkPoint* pts() const {
        return fPts;
    }

    void reset() {
        init(NULL, (SkPath::Verb) -1, false);
        fBounds.set(SK_ScalarMax, SK_ScalarMax, SK_ScalarMax, SK_ScalarMax);
        fTs.reset();
    }

    void setUpWindings(int index, int endIndex, int& sumMiWinding, int& sumSuWinding,
            int& maxWinding, int& sumWinding, int& oppMaxWinding, int& oppSumWinding) {
        int deltaSum = spanSign(index, endIndex);
        int oppDeltaSum = oppSign(index, endIndex);
        if (operand()) {
            maxWinding = sumSuWinding;
            sumWinding = sumSuWinding -= deltaSum;
            oppMaxWinding = sumMiWinding;
            oppSumWinding = sumMiWinding -= oppDeltaSum;
        } else {
            maxWinding = sumMiWinding;
            sumWinding = sumMiWinding -= deltaSum;
            oppMaxWinding = sumSuWinding;
            oppSumWinding = sumSuWinding -= oppDeltaSum;
        }
    }

    // This marks all spans unsortable so that this info is available for early
    // exclusion in find top and others. This could be optimized to only mark
    // adjacent spans that unsortable. However, this makes it difficult to later
    // determine starting points for edge detection in find top and the like.
    static bool SortAngles(SkTDArray<Angle>& angles, SkTDArray<Angle*>& angleList) {
        bool sortable = true;
        int angleCount = angles.count();
        int angleIndex;
        angleList.setReserve(angleCount);
        for (angleIndex = 0; angleIndex < angleCount; ++angleIndex) {
            Angle& angle = angles[angleIndex];
            *angleList.append() = &angle;
            sortable &= !angle.unsortable();
        }
        if (sortable) {
            QSort<Angle>(angleList.begin(), angleList.end() - 1);
            for (angleIndex = 0; angleIndex < angleCount; ++angleIndex) {
                if (angles[angleIndex].unsortable()) {
                    sortable = false;
                    break;
                }
            }
        }
        if (!sortable) {
            for (angleIndex = 0; angleIndex < angleCount; ++angleIndex) {
                Angle& angle = angles[angleIndex];
                angle.segment()->markUnsortable(angle.start(), angle.end());
            }
        }
        return sortable;
    }

    // OPTIMIZATION: mark as debugging only if used solely by tests
    const Span& span(int tIndex) const {
        return fTs[tIndex];
    }

    int spanSign(const Angle* angle) const {
        SkASSERT(angle->segment() == this);
        return spanSign(angle->start(), angle->end());
    }

    int spanSign(int startIndex, int endIndex) const {
        int result = startIndex < endIndex ? -fTs[startIndex].fWindValue
                : fTs[endIndex].fWindValue;
#if DEBUG_WIND_BUMP
        SkDebugf("%s spanSign=%d\n", __FUNCTION__, result);
#endif
        return result;
    }

    // OPTIMIZATION: mark as debugging only if used solely by tests
    double t(int tIndex) const {
        return fTs[tIndex].fT;
    }

    bool tiny(const Angle* angle) const {
        int start = angle->start();
        int end = angle->end();
        const Span& mSpan = fTs[SkMin32(start, end)];
        return mSpan.fTiny;
    }

    static void TrackOutside(SkTDArray<double>& outsideTs, double end,
            double start) {
        int outCount = outsideTs.count();
        if (outCount == 0 || !approximately_negative(end - outsideTs[outCount - 2])) {
            *outsideTs.append() = end;
            *outsideTs.append() = start;
        }
    }

    void undoneSpan(int& start, int& end) {
        size_t tCount = fTs.count();
        size_t index;
        for (index = 0; index < tCount; ++index) {
            if (!fTs[index].fDone) {
                break;
            }
        }
        SkASSERT(index < tCount - 1);
        start = index;
        double startT = fTs[index].fT;
        while (approximately_negative(fTs[++index].fT - startT))
            SkASSERT(index < tCount);
        SkASSERT(index < tCount);
        end = index;
    }

    bool unsortable(int index) const {
        return fTs[index].fUnsortableStart || fTs[index].fUnsortableEnd;
    }

    void updatePts(const SkPoint pts[]) {
        fPts = pts;
    }

    int updateOppWinding(int index, int endIndex) const {
        int lesser = SkMin32(index, endIndex);
        int oppWinding = oppSum(lesser);
        int oppSpanWinding = oppSign(index, endIndex);
        if (oppSpanWinding && useInnerWinding(oppWinding - oppSpanWinding, oppWinding)) {
            oppWinding -= oppSpanWinding;
        }
        return oppWinding;
    }

    int updateOppWinding(const Angle* angle) const {
        int startIndex = angle->start();
        int endIndex = angle->end();
        return updateOppWinding(endIndex, startIndex);
    }

    int updateOppWindingReverse(const Angle* angle) const {
        int startIndex = angle->start();
        int endIndex = angle->end();
        return updateOppWinding(startIndex, endIndex);
    }

    int updateWinding(int index, int endIndex) const {
        int lesser = SkMin32(index, endIndex);
        int winding = windSum(lesser);
        int spanWinding = spanSign(index, endIndex);
        if (useInnerWinding(winding - spanWinding, winding)) {
            winding -= spanWinding;
        }
        return winding;
    }

    int updateWinding(const Angle* angle) const {
        int startIndex = angle->start();
        int endIndex = angle->end();
        return updateWinding(endIndex, startIndex);
    }

    int updateWindingReverse(const Angle* angle) const {
        int startIndex = angle->start();
        int endIndex = angle->end();
        return updateWinding(startIndex, endIndex);
    }

    SkPath::Verb verb() const {
        return fVerb;
    }

    int windSum(int tIndex) const {
        return fTs[tIndex].fWindSum;
    }

    int windSum(const Angle* angle) const {
        int start = angle->start();
        int end = angle->end();
        int index = SkMin32(start, end);
        return windSum(index);
    }

    int windValue(int tIndex) const {
        return fTs[tIndex].fWindValue;
    }

    int windValue(const Angle* angle) const {
        int start = angle->start();
        int end = angle->end();
        int index = SkMin32(start, end);
        return windValue(index);
    }

    SkScalar xAtT(const Span* span) const {
        return xyAtT(span).fX;
    }

    const SkPoint& xyAtT(int index) const {
        return xyAtT(&fTs[index]);
    }

    const SkPoint& xyAtT(const Span* span) const {
        if (SkScalarIsNaN(span->fPt.fX)) {
            if (span->fT == 0) {
                span->fPt = fPts[0];
            } else if (span->fT == 1) {
                span->fPt = fPts[fVerb];
            } else {
                (*SegmentXYAtT[fVerb])(fPts, span->fT, &span->fPt);
            }
        }
        return span->fPt;
    }

    SkScalar yAtT(int index) const {
        return yAtT(&fTs[index]);
    }

    SkScalar yAtT(const Span* span) const {
        return xyAtT(span).fY;
    }

    void zeroSpan(Span* span, double otherT) {
        SkASSERT(span->fWindValue > 0);
        span->fWindValue = 0;
        if (!span->fDone) {
            span->fDone = true;
            ++fDoneSpans;
        }
        int oppValue = span->fOppValue;
        if (!oppValue) {
            return;
        }
        span->fOppValue = 0;
        Segment* other = span->fOther;
        Span& oSpan = other->fTs[span->fOtherIndex];
        SkASSERT(0);
    }

#if DEBUG_DUMP
    void dump() const {
        const char className[] = "Segment";
        const int tab = 4;
        for (int i = 0; i < fTs.count(); ++i) {
            SkPoint out;
            (*SegmentXYAtT[fVerb])(fPts, t(i), &out);
            SkDebugf("%*s [%d] %s.fTs[%d]=%1.9g (%1.9g,%1.9g) other=%d"
                    " otherT=%1.9g windSum=%d\n",
                    tab + sizeof(className), className, fID,
                    kLVerbStr[fVerb], i, fTs[i].fT, out.fX, out.fY,
                    fTs[i].fOther->fID, fTs[i].fOtherT, fTs[i].fWindSum);
        }
        SkDebugf("%*s [%d] fBounds=(l:%1.9g, t:%1.9g r:%1.9g, b:%1.9g)",
                tab + sizeof(className), className, fID,
                fBounds.fLeft, fBounds.fTop, fBounds.fRight, fBounds.fBottom);
    }
#endif

#if DEBUG_CONCIDENT
    // assert if pair has not already been added
     void debugAddTPair(double t, const Segment& other, double otherT) const {
        for (int i = 0; i < fTs.count(); ++i) {
            if (fTs[i].fT == t && fTs[i].fOther == &other && fTs[i].fOtherT == otherT) {
                return;
            }
        }
        SkASSERT(0);
     }
#endif

#if DEBUG_DUMP
    int debugID() const {
        return fID;
    }
#endif

#if DEBUG_WINDING
    void debugShowSums() const {
        SkDebugf("%s id=%d (%1.9g,%1.9g %1.9g,%1.9g)", __FUNCTION__, fID,
            fPts[0].fX, fPts[0].fY, fPts[fVerb].fX, fPts[fVerb].fY);
        for (int i = 0; i < fTs.count(); ++i) {
            const Span& span = fTs[i];
            SkDebugf(" [t=%1.3g %1.9g,%1.9g w=", span.fT, xAtT(&span), yAtT(&span));
            if (span.fWindSum == SK_MinS32) {
                SkDebugf("?");
            } else {
                SkDebugf("%d", span.fWindSum);
            }
            SkDebugf("]");
        }
        SkDebugf("\n");
    }
#endif

#if DEBUG_CONCIDENT
    void debugShowTs() const {
        SkDebugf("%s id=%d", __FUNCTION__, fID);
        for (int i = 0; i < fTs.count(); ++i) {
            SkDebugf(" [o=%d t=%1.3g %1.9g,%1.9g w=%d]", fTs[i].fOther->fID,
                    fTs[i].fT, xAtT(&fTs[i]), yAtT(&fTs[i]), fTs[i].fWindValue);
        }
        SkDebugf("\n");
    }
#endif

#if DEBUG_ACTIVE_SPANS
    void debugShowActiveSpans() const {
        if (done()) {
            return;
        }
        for (int i = 0; i < fTs.count(); ++i) {
            if (fTs[i].fDone) {
                continue;
            }
            SkDebugf("%s id=%d", __FUNCTION__, fID);
            SkDebugf(" (%1.9g,%1.9g", fPts[0].fX, fPts[0].fY);
            for (int vIndex = 1; vIndex <= fVerb; ++vIndex) {
                SkDebugf(" %1.9g,%1.9g", fPts[vIndex].fX, fPts[vIndex].fY);
            }
            const Span* span = &fTs[i];
            SkDebugf(") t=%1.9g (%1.9g,%1.9g)", fTs[i].fT,
                     xAtT(span), yAtT(span));
            const Segment* other = fTs[i].fOther;
            SkDebugf(" other=%d otherT=%1.9g otherIndex=%d windSum=",
                    other->fID, fTs[i].fOtherT, fTs[i].fOtherIndex);
            if (fTs[i].fWindSum == SK_MinS32) {
                SkDebugf("?");
            } else {
                SkDebugf("%d", fTs[i].fWindSum);
            }
            SkDebugf(" windValue=%d\n", fTs[i].fWindValue);
        }
    }

    // This isn't useful yet -- but leaving it in for now in case i think of something
    // to use it for
    void validateActiveSpans() const {
        if (done()) {
            return;
        }
        int tCount = fTs.count();
        for (int index = 0; index < tCount; ++index) {
            if (fTs[index].fDone) {
                continue;
            }
            // count number of connections which are not done
            int first = index;
            double baseT = fTs[index].fT;
            while (first > 0 && approximately_equal(fTs[first - 1].fT, baseT)) {
                --first;
            }
            int last = index;
            while (last < tCount - 1 && approximately_equal(fTs[last + 1].fT, baseT)) {
                ++last;
            }
            int connections = 0;
            connections += first > 0 && !fTs[first - 1].fDone;
            for (int test = first; test <= last; ++test) {
                connections += !fTs[test].fDone;
                const Segment* other = fTs[test].fOther;
                int oIndex = fTs[test].fOtherIndex;
                connections += !other->fTs[oIndex].fDone;
                connections += oIndex > 0 && !other->fTs[oIndex - 1].fDone;
            }
      //      SkASSERT(!(connections & 1));
        }
    }
#endif

#if DEBUG_MARK_DONE
    void debugShowNewWinding(const char* fun, const Span& span, int winding) {
        const SkPoint& pt = xyAtT(&span);
        SkDebugf("%s id=%d", fun, fID);
        SkDebugf(" (%1.9g,%1.9g", fPts[0].fX, fPts[0].fY);
        for (int vIndex = 1; vIndex <= fVerb; ++vIndex) {
            SkDebugf(" %1.9g,%1.9g", fPts[vIndex].fX, fPts[vIndex].fY);
        }
        SkASSERT(&span == &span.fOther->fTs[span.fOtherIndex].fOther->
                fTs[span.fOther->fTs[span.fOtherIndex].fOtherIndex]);
        SkDebugf(") t=%1.9g [%d] (%1.9g,%1.9g) newWindSum=%d windSum=",
                span.fT, span.fOther->fTs[span.fOtherIndex].fOtherIndex, pt.fX, pt.fY, winding);
        if (span.fWindSum == SK_MinS32) {
            SkDebugf("?");
        } else {
            SkDebugf("%d", span.fWindSum);
        }
        SkDebugf(" windValue=%d\n", span.fWindValue);
    }

    void debugShowNewWinding(const char* fun, const Span& span, int winding, int oppWinding) {
        const SkPoint& pt = xyAtT(&span);
        SkDebugf("%s id=%d", fun, fID);
        SkDebugf(" (%1.9g,%1.9g", fPts[0].fX, fPts[0].fY);
        for (int vIndex = 1; vIndex <= fVerb; ++vIndex) {
            SkDebugf(" %1.9g,%1.9g", fPts[vIndex].fX, fPts[vIndex].fY);
        }
        SkASSERT(&span == &span.fOther->fTs[span.fOtherIndex].fOther->
                fTs[span.fOther->fTs[span.fOtherIndex].fOtherIndex]);
        SkDebugf(") t=%1.9g [%d] (%1.9g,%1.9g) newWindSum=%d newOppSum=%d oppSum=",
                span.fT, span.fOther->fTs[span.fOtherIndex].fOtherIndex, pt.fX, pt.fY,
                winding, oppWinding);
        if (span.fOppSum == SK_MinS32) {
            SkDebugf("?");
        } else {
            SkDebugf("%d", span.fOppSum);
        }
        SkDebugf(" windSum=");
        if (span.fWindSum == SK_MinS32) {
            SkDebugf("?");
        } else {
            SkDebugf("%d", span.fWindSum);
        }
        SkDebugf(" windValue=%d\n", span.fWindValue);
    }
#endif

#if DEBUG_SORT
    void debugShowSort(const char* fun, const SkTDArray<Angle*>& angles, int first,
            const int contourWinding, const int oppContourWinding) const {
        SkASSERT(angles[first]->segment() == this);
        SkASSERT(angles.count() > 1);
        int lastSum = contourWinding;
        int oppLastSum = oppContourWinding;
        const Angle* firstAngle = angles[first];
        int windSum = lastSum - spanSign(firstAngle);
        int oppoSign = oppSign(firstAngle);
        int oppWindSum = oppLastSum - oppoSign;
        SkDebugf("%s %s contourWinding=%d oppContourWinding=%d sign=%d\n", fun, __FUNCTION__,
                contourWinding, oppContourWinding, spanSign(angles[first]));
        int index = first;
        bool firstTime = true;
        do {
            const Angle& angle = *angles[index];
            const Segment& segment = *angle.segment();
            int start = angle.start();
            int end = angle.end();
            const Span& sSpan = segment.fTs[start];
            const Span& eSpan = segment.fTs[end];
            const Span& mSpan = segment.fTs[SkMin32(start, end)];
            bool opp = segment.fOperand ^ fOperand;
            if (!firstTime) {
                oppoSign = segment.oppSign(&angle);
                if (opp) {
                    oppLastSum = oppWindSum;
                    oppWindSum -= segment.spanSign(&angle);
                    if (oppoSign) {
                        lastSum = windSum;
                        windSum -= oppoSign;
                    }
                } else {
                    lastSum = windSum;
                    windSum -= segment.spanSign(&angle);
                    if (oppoSign) {
                        oppLastSum = oppWindSum;
                        oppWindSum -= oppoSign;
                    }
                }
            }
            SkDebugf("%s [%d] %sid=%d %s start=%d (%1.9g,%,1.9g) end=%d (%1.9g,%,1.9g)"
                    " sign=%d windValue=%d windSum=",
                    __FUNCTION__, index, angle.unsortable() ? "*** UNSORTABLE *** " : "",
                    segment.fID, kLVerbStr[segment.fVerb],
                    start, segment.xAtT(&sSpan), segment.yAtT(&sSpan), end,
                    segment.xAtT(&eSpan), segment.yAtT(&eSpan), angle.sign(),
                    mSpan.fWindValue);
            if (mSpan.fWindSum == SK_MinS32) {
                SkDebugf("?");
            } else {
                SkDebugf("%d", mSpan.fWindSum);
            }
            int last, wind;
            if (opp) {
                last = oppLastSum;
                wind = oppWindSum;
            } else {
                last = lastSum;
                wind = windSum;
            }
            if (!oppoSign) {
                SkDebugf(" %d->%d (max=%d)", last, wind,
                        useInnerWinding(last, wind) ? wind : last);
            } else {
                SkDebugf(" %d->%d (%d->%d)", last, wind, opp ? lastSum : oppLastSum,
                        opp ? windSum : oppWindSum);
            }
            SkDebugf(" done=%d tiny=%d opp=%d\n", mSpan.fDone, mSpan.fTiny, opp);
#if false && DEBUG_ANGLE
            angle.debugShow(segment.xyAtT(&sSpan));
#endif
            ++index;
            if (index == angles.count()) {
                index = 0;
            }
            if (firstTime) {
                firstTime = false;
            }
        } while (index != first);
    }

    void debugShowSort(const char* fun, const SkTDArray<Angle*>& angles, int first) {
        const Angle* firstAngle = angles[first];
        const Segment* segment = firstAngle->segment();
        int winding = segment->updateWinding(firstAngle);
        int oppWinding = segment->updateOppWinding(firstAngle);
        debugShowSort(fun, angles, first, winding, oppWinding);
    }

#endif

#if DEBUG_WINDING
    static char as_digit(int value) {
        return value < 0 ? '?' : value <= 9 ? '0' + value : '+';
    }
#endif

#if DEBUG_SHOW_WINDING
    int debugShowWindingValues(int slotCount, int ofInterest) const {
        if (!(1 << fID & ofInterest)) {
            return 0;
        }
        int sum = 0;
        SkTDArray<char> slots;
        slots.setCount(slotCount * 2);
        memset(slots.begin(), ' ', slotCount * 2);
        for (int i = 0; i < fTs.count(); ++i) {
       //     if (!(1 << fTs[i].fOther->fID & ofInterest)) {
       //         continue;
       //     }
            sum += fTs[i].fWindValue;
            slots[fTs[i].fOther->fID - 1] = as_digit(fTs[i].fWindValue);
            sum += fTs[i].fOppValue;
            slots[slotCount + fTs[i].fOther->fID - 1] = as_digit(fTs[i].fOppValue);
        }
        SkDebugf("%s id=%2d %.*s | %.*s\n", __FUNCTION__, fID, slotCount, slots.begin(), slotCount,
                slots.begin() + slotCount);
        return sum;
    }
#endif

#if DEBUG_WINDING
    bool debugVerifyWinding(int start, int end, int winding) const {
        const Span& span = fTs[SkMin32(start, end)];
        int spanWinding = span.fWindSum;
        if (spanWinding == SK_MinS32) {
            return true;
        }
        int spanSign = SkSign32(start - end);
        int signedVal = spanSign * span.fWindValue;
        if (signedVal < 0) {
            spanWinding -= signedVal;
        }
        return span.fWindSum == winding;
    }
#endif

private:
    const SkPoint* fPts;
    SkPath::Verb fVerb;
    Bounds fBounds;
    SkTDArray<Span> fTs; // two or more (always includes t=0 t=1)
    int fDoneSpans; // quick check that segment is finished
    bool fOperand;
#if DEBUG_DUMP
    int fID;
#endif
};

class Contour;

struct Coincidence {
    Contour* fContours[2];
    int fSegments[2];
    double fTs[2][2];
};

class Contour {
public:
    Contour() {
        reset();
#if DEBUG_DUMP
        fID = ++gContourID;
#endif
    }

    bool operator<(const Contour& rh) const {
        return fBounds.fTop == rh.fBounds.fTop
                ? fBounds.fLeft < rh.fBounds.fLeft
                : fBounds.fTop < rh.fBounds.fTop;
    }

    void addCoincident(int index, Contour* other, int otherIndex,
            const Intersections& ts, bool swap) {
        Coincidence& coincidence = *fCoincidences.append();
        coincidence.fContours[0] = this; // FIXME: no need to store
        coincidence.fContours[1] = other;
        coincidence.fSegments[0] = index;
        coincidence.fSegments[1] = otherIndex;
        if (fSegments[index].verb() == SkPath::kLine_Verb &&
                other->fSegments[otherIndex].verb() == SkPath::kLine_Verb) {
            // FIXME: coincident lines use legacy Ts instead of coincident Ts
            coincidence.fTs[swap][0] = ts.fT[0][0];
            coincidence.fTs[swap][1] = ts.fT[0][1];
            coincidence.fTs[!swap][0] = ts.fT[1][0];
            coincidence.fTs[!swap][1] = ts.fT[1][1];
        } else if (fSegments[index].verb() == SkPath::kQuad_Verb &&
                other->fSegments[otherIndex].verb() == SkPath::kQuad_Verb) {
            coincidence.fTs[swap][0] = ts.fCoincidentT[0][0];
            coincidence.fTs[swap][1] = ts.fCoincidentT[0][1];
            coincidence.fTs[!swap][0] = ts.fCoincidentT[1][0];
            coincidence.fTs[!swap][1] = ts.fCoincidentT[1][1];
        }
    }

    void addCross(const Contour* crosser) {
#ifdef DEBUG_CROSS
        for (int index = 0; index < fCrosses.count(); ++index) {
            SkASSERT(fCrosses[index] != crosser);
        }
#endif
        *fCrosses.append() = crosser;
    }

    void addCubic(const SkPoint pts[4]) {
        fSegments.push_back().addCubic(pts, fOperand);
        fContainsCurves = true;
    }

    int addLine(const SkPoint pts[2]) {
        fSegments.push_back().addLine(pts, fOperand);
        return fSegments.count();
    }

    void addOtherT(int segIndex, int tIndex, double otherT, int otherIndex) {
        fSegments[segIndex].addOtherT(tIndex, otherT, otherIndex);
    }

    int addQuad(const SkPoint pts[3]) {
        fSegments.push_back().addQuad(pts, fOperand);
        fContainsCurves = true;
        return fSegments.count();
    }

    int addT(int segIndex, double newT, Contour* other, int otherIndex) {
        containsIntercepts();
        return fSegments[segIndex].addT(newT, &other->fSegments[otherIndex]);
    }

    const Bounds& bounds() const {
        return fBounds;
    }

    void complete() {
        setBounds();
        fContainsIntercepts = false;
    }

    void containsIntercepts() {
        fContainsIntercepts = true;
    }

    const Segment* crossedSegment(const SkPoint& basePt, SkScalar& bestY,
            int &tIndex, double& hitT) {
        int segmentCount = fSegments.count();
        const Segment* bestSegment = NULL;
        for (int test = 0; test < segmentCount; ++test) {
            Segment* testSegment = &fSegments[test];
            const SkRect& bounds = testSegment->bounds();
            if (bounds.fBottom <= bestY) {
                continue;
            }
            if (bounds.fTop >= basePt.fY) {
                continue;
            }
            if (bounds.fLeft > basePt.fX) {
                continue;
            }
            if (bounds.fRight < basePt.fX) {
                continue;
            }
            if (bounds.fLeft == bounds.fRight) {
                continue;
            }
     #if 0
            bool leftHalf = bounds.fLeft == basePt.fX;
            bool rightHalf = bounds.fRight == basePt.fX;
            if ((leftHalf || rightHalf) && !testSegment->crossedSpanHalves(
                    basePt, leftHalf, rightHalf)) {
                continue;
            }
     #endif
            double testHitT;
            int testT = testSegment->crossedSpan(basePt, bestY, testHitT);
            if (testT >= 0) {
                bestSegment = testSegment;
                tIndex = testT;
                hitT = testHitT;
            }
        }
        return bestSegment;
    }

    bool crosses(const Contour* crosser) const {
        for (int index = 0; index < fCrosses.count(); ++index) {
            if (fCrosses[index] == crosser) {
                return true;
            }
        }
        return false;
    }

    const SkPoint& end() const {
        const Segment& segment = fSegments.back();
        return segment.pts()[segment.verb()];
    }

    void findTooCloseToCall(bool otherXor) {
        int segmentCount = fSegments.count();
        otherXor ^= fXor;
        for (int sIndex = 0; sIndex < segmentCount; ++sIndex) {
            fSegments[sIndex].findTooCloseToCall(fXor, otherXor);
        }
    }

    void fixOtherTIndex() {
        int segmentCount = fSegments.count();
        for (int sIndex = 0; sIndex < segmentCount; ++sIndex) {
            fSegments[sIndex].fixOtherTIndex();
        }
    }

    bool operand() const {
        return fOperand;
    }

    void reset() {
        fSegments.reset();
        fBounds.set(SK_ScalarMax, SK_ScalarMax, SK_ScalarMax, SK_ScalarMax);
        fContainsCurves = fContainsIntercepts = false;
    }

    void resolveCoincidence(SkTDArray<Contour*>& contourList) {
        int count = fCoincidences.count();
        for (int index = 0; index < count; ++index) {
            Coincidence& coincidence = fCoincidences[index];
            Contour* thisContour = coincidence.fContours[0];
            SkASSERT(thisContour == this);
            Contour* otherContour = coincidence.fContours[1];
            int thisIndex = coincidence.fSegments[0];
            int otherIndex = coincidence.fSegments[1];
            Segment& thisOne = thisContour->fSegments[thisIndex];
            Segment& other = otherContour->fSegments[otherIndex];
        #if DEBUG_CONCIDENT
            thisOne.debugShowTs();
            other.debugShowTs();
        #endif
            double startT = coincidence.fTs[0][0];
            double endT = coincidence.fTs[0][1];
            bool cancelers = false;
            if (startT > endT) {
                SkTSwap<double>(startT, endT);
                cancelers ^= true; // FIXME: just assign true
            }
            SkASSERT(!approximately_negative(endT - startT));
            double oStartT = coincidence.fTs[1][0];
            double oEndT = coincidence.fTs[1][1];
            if (oStartT > oEndT) {
                SkTSwap<double>(oStartT, oEndT);
                cancelers ^= true;
            }
            SkASSERT(!approximately_negative(oEndT - oStartT));
            bool opp = thisContour->fOperand ^ otherContour->fOperand;
            if (cancelers && !opp) {
                // make sure startT and endT have t entries
                if (startT > 0 || oEndT < 1
                        || thisOne.isMissing(startT) || other.isMissing(oEndT)) {
                    thisOne.addTPair(startT, other, oEndT, true);
                }
                if (oStartT > 0 || endT < 1
                        || thisOne.isMissing(endT) || other.isMissing(oStartT)) {
                    other.addTPair(oStartT, thisOne, endT, true);
                }
                thisOne.addTCancel(startT, endT, other, oStartT, oEndT);
            } else {
                if (startT > 0 || oStartT > 0
                        || thisOne.isMissing(startT) || other.isMissing(oStartT)) {
                    thisOne.addTPair(startT, other, oStartT, true);
                }
                if (endT < 1 || oEndT < 1
                        || thisOne.isMissing(endT) || other.isMissing(oEndT)) {
                    other.addTPair(oEndT, thisOne, endT, true);
                }
                thisOne.addTCoincident(thisContour->fXor, otherContour->fXor,
                        startT, endT, other, oStartT, oEndT);
            }
        #if DEBUG_CONCIDENT
            thisOne.debugShowTs();
            other.debugShowTs();
        #endif
        #if DEBUG_SHOW_WINDING
            debugShowWindingValues(contourList);
        #endif
        }
    }

    const SkTArray<Segment>& segments() {
        return fSegments;
    }

    void setOperand(bool isOp) {
        fOperand = isOp;
    }

    void setXor(bool isXor) {
        fXor = isXor;
    }

    void sortSegments() {
        int segmentCount = fSegments.count();
        fSortedSegments.setReserve(segmentCount);
        for (int test = 0; test < segmentCount; ++test) {
            *fSortedSegments.append() = &fSegments[test];
        }
        QSort<Segment>(fSortedSegments.begin(), fSortedSegments.end() - 1);
        fFirstSorted = 0;
    }

    const SkPoint& start() const {
        return fSegments.front().pts()[0];
    }

    void toPath(PathWrapper& path) const {
        int segmentCount = fSegments.count();
        const SkPoint& pt = fSegments.front().pts()[0];
        path.deferredMove(pt);
        for (int test = 0; test < segmentCount; ++test) {
            fSegments[test].addCurveTo(0, 1, path, true);
        }
        path.close();
    }

    void toPartialBackward(PathWrapper& path) const {
        int segmentCount = fSegments.count();
        for (int test = segmentCount - 1; test >= 0; --test) {
            fSegments[test].addCurveTo(1, 0, path, true);
        }
    }

    void toPartialForward(PathWrapper& path) const {
        int segmentCount = fSegments.count();
        for (int test = 0; test < segmentCount; ++test) {
            fSegments[test].addCurveTo(0, 1, path, true);
        }
    }

#if 0 // FIXME: obsolete, remove
    // OPTIMIZATION: feel pretty uneasy about this. It seems like once again
    // we need to sort and walk edges in y, but that on the surface opens the
    // same can of worms as before. But then, this is a rough sort based on
    // segments' top, and not a true sort, so it could be ameniable to regular
    // sorting instead of linear searching. Still feel like I'm missing something
    Segment* topSegment(SkScalar& bestY) {
        int segmentCount = fSegments.count();
        SkASSERT(segmentCount > 0);
        int best = -1;
        Segment* bestSegment = NULL;
        while (++best < segmentCount) {
            Segment* testSegment = &fSegments[best];
            if (testSegment->done()) {
                continue;
            }
            bestSegment = testSegment;
            break;
        }
        if (!bestSegment) {
            return NULL;
        }
        SkScalar bestTop = bestSegment->activeTop();
        for (int test = best + 1; test < segmentCount; ++test) {
            Segment* testSegment = &fSegments[test];
            if (testSegment->done()) {
                continue;
            }
            if (testSegment->bounds().fTop > bestTop) {
                continue;
            }
            SkScalar testTop = testSegment->activeTop();
            if (bestTop > testTop) {
                bestTop = testTop;
                bestSegment = testSegment;
            }
        }
        bestY = bestTop;
        return bestSegment;
    }
#endif

    Segment* topSortableSegment(const SkPoint& topLeft, SkPoint& bestXY) {
        int segmentCount = fSortedSegments.count();
        SkASSERT(segmentCount > 0);
        Segment* bestSegment = NULL;
        int sortedIndex = fFirstSorted;
        for ( ; sortedIndex < segmentCount; ++sortedIndex) {
            Segment* testSegment = fSortedSegments[sortedIndex];
            if (testSegment->done()) {
                if (sortedIndex == fFirstSorted) {
                    ++fFirstSorted;
                }
                continue;
            }
            SkPoint testXY;
            testSegment->activeLeftTop(testXY);
            if (testXY.fY < topLeft.fY) {
                continue;
            }
            if (testXY.fY == topLeft.fY && testXY.fX < topLeft.fX) {
                continue;
            }
            if (bestXY.fY < testXY.fY) {
                continue;
            }
            if (bestXY.fY == testXY.fY && bestXY.fX < testXY.fX) {
                continue;
            }
            bestSegment = testSegment;
            bestXY = testXY;
        }
        return bestSegment;
    }

    Segment* undoneSegment(int& start, int& end) {
        int segmentCount = fSegments.count();
        for (int test = 0; test < segmentCount; ++test) {
            Segment* testSegment = &fSegments[test];
            if (testSegment->done()) {
                continue;
            }
            testSegment->undoneSpan(start, end);
            return testSegment;
        }
        return NULL;
    }

    int updateSegment(int index, const SkPoint* pts) {
        Segment& segment = fSegments[index];
        segment.updatePts(pts);
        return segment.verb() + 1;
    }

#if DEBUG_TEST
    SkTArray<Segment>& debugSegments() {
        return fSegments;
    }
#endif

#if DEBUG_DUMP
    void dump() {
        int i;
        const char className[] = "Contour";
        const int tab = 4;
        SkDebugf("%s %p (contour=%d)\n", className, this, fID);
        for (i = 0; i < fSegments.count(); ++i) {
            SkDebugf("%*s.fSegments[%d]:\n", tab + sizeof(className),
                    className, i);
            fSegments[i].dump();
        }
        SkDebugf("%*s.fBounds=(l:%1.9g, t:%1.9g r:%1.9g, b:%1.9g)\n",
                tab + sizeof(className), className,
                fBounds.fLeft, fBounds.fTop,
                fBounds.fRight, fBounds.fBottom);
        SkDebugf("%*s.fContainsIntercepts=%d\n", tab + sizeof(className),
                className, fContainsIntercepts);
        SkDebugf("%*s.fContainsCurves=%d\n", tab + sizeof(className),
                className, fContainsCurves);
    }
#endif

#if DEBUG_ACTIVE_SPANS
    void debugShowActiveSpans() {
        for (int index = 0; index < fSegments.count(); ++index) {
            fSegments[index].debugShowActiveSpans();
        }
    }

    void validateActiveSpans() {
        for (int index = 0; index < fSegments.count(); ++index) {
            fSegments[index].validateActiveSpans();
        }
    }
#endif

#if DEBUG_SHOW_WINDING
    int debugShowWindingValues(int totalSegments, int ofInterest) {
        int count = fSegments.count();
        int sum = 0;
        for (int index = 0; index < count; ++index) {
            sum += fSegments[index].debugShowWindingValues(totalSegments, ofInterest);
        }
  //      SkDebugf("%s sum=%d\n", __FUNCTION__, sum);
        return sum;
    }

    static void debugShowWindingValues(SkTDArray<Contour*>& contourList) {
   //     int ofInterest = 1 << 1 | 1 << 5 | 1 << 9 | 1 << 13;
    //    int ofInterest = 1 << 4 | 1 << 8 | 1 << 12 | 1 << 16;
        int ofInterest = 1 << 5 | 1 << 8;
        int total = 0;
        int index;
        for (index = 0; index < contourList.count(); ++index) {
            total += contourList[index]->segments().count();
        }
        int sum = 0;
        for (index = 0; index < contourList.count(); ++index) {
            sum += contourList[index]->debugShowWindingValues(total, ofInterest);
        }
 //       SkDebugf("%s total=%d\n", __FUNCTION__, sum);
    }
#endif

protected:
    void setBounds() {
        int count = fSegments.count();
        if (count == 0) {
            SkDebugf("%s empty contour\n", __FUNCTION__);
            SkASSERT(0);
            // FIXME: delete empty contour?
            return;
        }
        fBounds = fSegments.front().bounds();
        for (int index = 1; index < count; ++index) {
            fBounds.add(fSegments[index].bounds());
        }
    }

private:
    SkTArray<Segment> fSegments;
    SkTDArray<Segment*> fSortedSegments;
    int fFirstSorted;
    SkTDArray<Coincidence> fCoincidences;
    SkTDArray<const Contour*> fCrosses;
    Bounds fBounds;
    bool fContainsIntercepts;
    bool fContainsCurves;
    bool fOperand; // true for the second argument to a binary operator
    bool fXor;
#if DEBUG_DUMP
    int fID;
#endif
};

class EdgeBuilder {
public:

EdgeBuilder(const PathWrapper& path, SkTArray<Contour>& contours)
    : fPath(path.nativePath())
    , fContours(contours)
{
    init();
}

EdgeBuilder(const SkPath& path, SkTArray<Contour>& contours)
    : fPath(&path)
    , fContours(contours)
{
    init();
}

void init() {
    fCurrentContour = NULL;
    fOperand = false;
    fXorMask[0] = fXorMask[1] = (fPath->getFillType() & 1) ? kEvenOdd_Mask : kWinding_Mask;
#if DEBUG_DUMP
    gContourID = 0;
    gSegmentID = 0;
#endif
    fSecondHalf = preFetch();
}

void addOperand(const SkPath& path) {
    SkASSERT(fPathVerbs.count() > 0 && fPathVerbs.end()[-1] == SkPath::kDone_Verb);
    fPathVerbs.pop();
    fPath = &path;
    fXorMask[1] = (fPath->getFillType() & 1) ? kEvenOdd_Mask : kWinding_Mask;
    preFetch();
}

void finish() {
    walk();
    complete();
    if (fCurrentContour && !fCurrentContour->segments().count()) {
        fContours.pop_back();
    }
    // correct pointers in contours since fReducePts may have moved as it grew
    int cIndex = 0;
    int extraCount = fExtra.count();
    SkASSERT(extraCount == 0 || fExtra[0] == -1);
    int eIndex = 0;
    int rIndex = 0;
    while (++eIndex < extraCount) {
        int offset = fExtra[eIndex];
        if (offset < 0) {
            ++cIndex;
            continue;
        }
        fCurrentContour = &fContours[cIndex];
        rIndex += fCurrentContour->updateSegment(offset - 1,
                &fReducePts[rIndex]);
    }
    fExtra.reset(); // we're done with this
}

ShapeOpMask xorMask() const {
    return fXorMask[fOperand];
}

protected:

void complete() {
    if (fCurrentContour && fCurrentContour->segments().count()) {
        fCurrentContour->complete();
        fCurrentContour = NULL;
    }
}

// FIXME:remove once we can access path pts directly
int preFetch() {
    SkPath::RawIter iter(*fPath); // FIXME: access path directly when allowed
    SkPoint pts[4];
    SkPath::Verb verb;
    do {
        verb = iter.next(pts);
        *fPathVerbs.append() = verb;
        if (verb == SkPath::kMove_Verb) {
            *fPathPts.append() = pts[0];
        } else if (verb >= SkPath::kLine_Verb && verb <= SkPath::kCubic_Verb) {
            fPathPts.append(verb, &pts[1]);
        }
    } while (verb != SkPath::kDone_Verb);
    return fPathVerbs.count() - 1;
}

void walk() {
    SkPath::Verb reducedVerb;
    uint8_t* verbPtr = fPathVerbs.begin();
    uint8_t* endOfFirstHalf = &verbPtr[fSecondHalf];
    const SkPoint* pointsPtr = fPathPts.begin();
    const SkPoint* finalCurveStart = NULL;
    const SkPoint* finalCurveEnd = NULL;
    SkPath::Verb verb;
    while ((verb = (SkPath::Verb) *verbPtr++) != SkPath::kDone_Verb) {
        switch (verb) {
            case SkPath::kMove_Verb:
                complete();
                if (!fCurrentContour) {
                    fCurrentContour = fContours.push_back_n(1);
                    fCurrentContour->setOperand(fOperand);
                    fCurrentContour->setXor(fXorMask[fOperand] == kEvenOdd_Mask);
                    *fExtra.append() = -1; // start new contour
                }
                finalCurveEnd = pointsPtr++;
                goto nextVerb;
            case SkPath::kLine_Verb:
                // skip degenerate points
                if (pointsPtr[-1].fX != pointsPtr[0].fX
                        || pointsPtr[-1].fY != pointsPtr[0].fY) {
                    fCurrentContour->addLine(&pointsPtr[-1]);
                }
                break;
            case SkPath::kQuad_Verb:

                reducedVerb = QuadReduceOrder(&pointsPtr[-1], fReducePts);
                if (reducedVerb == 0) {
                    break; // skip degenerate points
                }
                if (reducedVerb == 1) {
                    *fExtra.append() =
                            fCurrentContour->addLine(fReducePts.end() - 2);
                    break;
                }
                fCurrentContour->addQuad(&pointsPtr[-1]);
                break;
            case SkPath::kCubic_Verb:
                reducedVerb = CubicReduceOrder(&pointsPtr[-1], fReducePts);
                if (reducedVerb == 0) {
                    break; // skip degenerate points
                }
                if (reducedVerb == 1) {
                    *fExtra.append() =
                            fCurrentContour->addLine(fReducePts.end() - 2);
                    break;
                }
                if (reducedVerb == 2) {
                    *fExtra.append() =
                            fCurrentContour->addQuad(fReducePts.end() - 3);
                    break;
                }
                fCurrentContour->addCubic(&pointsPtr[-1]);
                break;
            case SkPath::kClose_Verb:
                SkASSERT(fCurrentContour);
                if (finalCurveStart && finalCurveEnd
                        && *finalCurveStart != *finalCurveEnd) {
                    *fReducePts.append() = *finalCurveStart;
                    *fReducePts.append() = *finalCurveEnd;
                    *fExtra.append() =
                            fCurrentContour->addLine(fReducePts.end() - 2);
                }
                complete();
                goto nextVerb;
            default:
                SkDEBUGFAIL("bad verb");
                return;
        }
        finalCurveStart = &pointsPtr[verb - 1];
        pointsPtr += verb;
        SkASSERT(fCurrentContour);
    nextVerb:
        if (verbPtr == endOfFirstHalf) {
            fOperand = true;
        }
    }
}

private:
    const SkPath* fPath;
    SkTDArray<SkPoint> fPathPts; // FIXME: point directly to path pts instead
    SkTDArray<uint8_t> fPathVerbs; // FIXME: remove
    Contour* fCurrentContour;
    SkTArray<Contour>& fContours;
    SkTDArray<SkPoint> fReducePts; // segments created on the fly
    SkTDArray<int> fExtra; // -1 marks new contour, > 0 offsets into contour
    ShapeOpMask fXorMask[2];
    int fSecondHalf;
    bool fOperand;
};

class Work {
public:
    enum SegmentType {
        kHorizontalLine_Segment = -1,
        kVerticalLine_Segment = 0,
        kLine_Segment = SkPath::kLine_Verb,
        kQuad_Segment = SkPath::kQuad_Verb,
        kCubic_Segment = SkPath::kCubic_Verb,
    };

    void addCoincident(Work& other, const Intersections& ts, bool swap) {
        fContour->addCoincident(fIndex, other.fContour, other.fIndex, ts, swap);
    }

    // FIXME: does it make sense to write otherIndex now if we're going to
    // fix it up later?
    void addOtherT(int index, double otherT, int otherIndex) {
        fContour->addOtherT(fIndex, index, otherT, otherIndex);
    }

    // Avoid collapsing t values that are close to the same since
    // we walk ts to describe consecutive intersections. Since a pair of ts can
    // be nearly equal, any problems caused by this should be taken care
    // of later.
    // On the edge or out of range values are negative; add 2 to get end
    int addT(double newT, const Work& other) {
        return fContour->addT(fIndex, newT, other.fContour, other.fIndex);
    }

    bool advance() {
        return ++fIndex < fLast;
    }

    SkScalar bottom() const {
        return bounds().fBottom;
    }

    const Bounds& bounds() const {
        return fContour->segments()[fIndex].bounds();
    }

    const SkPoint* cubic() const {
        return fCubic;
    }

    void init(Contour* contour) {
        fContour = contour;
        fIndex = 0;
        fLast = contour->segments().count();
    }

    bool isAdjacent(const Work& next) {
        return fContour == next.fContour && fIndex + 1 == next.fIndex;
    }

    bool isFirstLast(const Work& next) {
        return fContour == next.fContour && fIndex == 0
                && next.fIndex == fLast - 1;
    }

    SkScalar left() const {
        return bounds().fLeft;
    }

    void promoteToCubic() {
        fCubic[0] = pts()[0];
        fCubic[2] = pts()[1];
        fCubic[3] = pts()[2];
        fCubic[1].fX = (fCubic[0].fX + fCubic[2].fX * 2) / 3;
        fCubic[1].fY = (fCubic[0].fY + fCubic[2].fY * 2) / 3;
        fCubic[2].fX = (fCubic[3].fX + fCubic[2].fX * 2) / 3;
        fCubic[2].fY = (fCubic[3].fY + fCubic[2].fY * 2) / 3;
    }

    const SkPoint* pts() const {
        return fContour->segments()[fIndex].pts();
    }

    SkScalar right() const {
        return bounds().fRight;
    }

    ptrdiff_t segmentIndex() const {
        return fIndex;
    }

    SegmentType segmentType() const {
        const Segment& segment = fContour->segments()[fIndex];
        SegmentType type = (SegmentType) segment.verb();
        if (type != kLine_Segment) {
            return type;
        }
        if (segment.isHorizontal()) {
            return kHorizontalLine_Segment;
        }
        if (segment.isVertical()) {
            return kVerticalLine_Segment;
        }
        return kLine_Segment;
    }

    bool startAfter(const Work& after) {
        fIndex = after.fIndex;
        return advance();
    }

    SkScalar top() const {
        return bounds().fTop;
    }

    SkPath::Verb verb() const {
        return fContour->segments()[fIndex].verb();
    }

    SkScalar x() const {
        return bounds().fLeft;
    }

    bool xFlipped() const {
        return x() != pts()[0].fX;
    }

    SkScalar y() const {
        return bounds().fTop;
    }

    bool yFlipped() const {
        return y() != pts()[0].fY;
    }

protected:
    Contour* fContour;
    SkPoint fCubic[4];
    int fIndex;
    int fLast;
};

#if DEBUG_ADD_INTERSECTING_TS
static void debugShowLineIntersection(int pts, const Work& wt,
        const Work& wn, const double wtTs[2], const double wnTs[2]) {
    return;
    if (!pts) {
        SkDebugf("%s no intersect (%1.9g,%1.9g %1.9g,%1.9g) (%1.9g,%1.9g %1.9g,%1.9g)\n",
                __FUNCTION__, wt.pts()[0].fX, wt.pts()[0].fY,
                wt.pts()[1].fX, wt.pts()[1].fY, wn.pts()[0].fX, wn.pts()[0].fY,
                wn.pts()[1].fX, wn.pts()[1].fY);
        return;
    }
    SkPoint wtOutPt, wnOutPt;
    LineXYAtT(wt.pts(), wtTs[0], &wtOutPt);
    LineXYAtT(wn.pts(), wnTs[0], &wnOutPt);
    SkDebugf("%s wtTs[0]=%1.9g (%1.9g,%1.9g %1.9g,%1.9g) (%1.9g,%1.9g)",
            __FUNCTION__,
            wtTs[0], wt.pts()[0].fX, wt.pts()[0].fY,
            wt.pts()[1].fX, wt.pts()[1].fY, wtOutPt.fX, wtOutPt.fY);
    if (pts == 2) {
        SkDebugf(" wtTs[1]=%1.9g", wtTs[1]);
    }
    SkDebugf(" wnTs[0]=%g (%1.9g,%1.9g %1.9g,%1.9g) (%1.9g,%1.9g)",
            wnTs[0], wn.pts()[0].fX, wn.pts()[0].fY,
            wn.pts()[1].fX, wn.pts()[1].fY, wnOutPt.fX, wnOutPt.fY);
    if (pts == 2) {
        SkDebugf(" wnTs[1]=%1.9g", wnTs[1]);
    }
    SkDebugf("\n");
}

static void debugShowQuadLineIntersection(int pts, const Work& wt,
        const Work& wn, const double wtTs[2], const double wnTs[2]) {
    if (!pts) {
        SkDebugf("%s no intersect (%1.9g,%1.9g %1.9g,%1.9g %1.9g,%1.9g)"
                " (%1.9g,%1.9g %1.9g,%1.9g)\n",
                __FUNCTION__, wt.pts()[0].fX, wt.pts()[0].fY,
                wt.pts()[1].fX, wt.pts()[1].fY, wt.pts()[2].fX, wt.pts()[2].fY,
                wn.pts()[0].fX, wn.pts()[0].fY, wn.pts()[1].fX, wn.pts()[1].fY);
        return;
    }
    SkPoint wtOutPt, wnOutPt;
    QuadXYAtT(wt.pts(), wtTs[0], &wtOutPt);
    LineXYAtT(wn.pts(), wnTs[0], &wnOutPt);
    SkDebugf("%s wtTs[0]=%1.9g (%1.9g,%1.9g %1.9g,%1.9g %1.9g,%1.9g) (%1.9g,%1.9g)",
            __FUNCTION__,
            wtTs[0], wt.pts()[0].fX, wt.pts()[0].fY,
            wt.pts()[1].fX, wt.pts()[1].fY, wt.pts()[2].fX, wt.pts()[2].fY,
            wtOutPt.fX, wtOutPt.fY);
    if (pts == 2) {
        QuadXYAtT(wt.pts(), wtTs[1], &wtOutPt);
        SkDebugf(" wtTs[1]=%1.9g (%1.9g,%1.9g)", wtTs[1], wtOutPt.fX, wtOutPt.fY);
    }
    SkDebugf(" wnTs[0]=%g (%1.9g,%1.9g %1.9g,%1.9g) (%1.9g,%1.9g)",
            wnTs[0], wn.pts()[0].fX, wn.pts()[0].fY,
            wn.pts()[1].fX, wn.pts()[1].fY, wnOutPt.fX, wnOutPt.fY);
    if (pts == 2) {
        LineXYAtT(wn.pts(), wnTs[1], &wnOutPt);
        SkDebugf(" wnTs[1]=%1.9g (%1.9g,%1.9g)", wnTs[1], wnOutPt.fX, wnOutPt.fY);
    }
    SkDebugf("\n");
}

static void debugShowQuadIntersection(int pts, const Work& wt,
        const Work& wn, const double wtTs[2], const double wnTs[2]) {
    if (!pts) {
        SkDebugf("%s no intersect (%1.9g,%1.9g %1.9g,%1.9g %1.9g,%1.9g)"
                " (%1.9g,%1.9g %1.9g,%1.9g %1.9g,%1.9g)\n",
                __FUNCTION__, wt.pts()[0].fX, wt.pts()[0].fY,
                wt.pts()[1].fX, wt.pts()[1].fY, wt.pts()[2].fX, wt.pts()[2].fY,
                wn.pts()[0].fX, wn.pts()[0].fY, wn.pts()[1].fX, wn.pts()[1].fY,
                wn.pts()[2].fX, wn.pts()[2].fY );
        return;
    }
    SkPoint wtOutPt, wnOutPt;
    QuadXYAtT(wt.pts(), wtTs[0], &wtOutPt);
    QuadXYAtT(wn.pts(), wnTs[0], &wnOutPt);
    SkDebugf("%s wtTs[0]=%1.9g (%1.9g,%1.9g %1.9g,%1.9g %1.9g,%1.9g) (%1.9g,%1.9g)",
            __FUNCTION__,
            wtTs[0], wt.pts()[0].fX, wt.pts()[0].fY,
            wt.pts()[1].fX, wt.pts()[1].fY, wt.pts()[2].fX, wt.pts()[2].fY,
            wtOutPt.fX, wtOutPt.fY);
    if (pts == 2) {
        SkDebugf(" wtTs[1]=%1.9g", wtTs[1]);
    }
    SkDebugf(" wnTs[0]=%g (%1.9g,%1.9g %1.9g,%1.9g %1.9g,%1.9g) (%1.9g,%1.9g)",
            wnTs[0], wn.pts()[0].fX, wn.pts()[0].fY,
            wn.pts()[1].fX, wn.pts()[1].fY, wn.pts()[2].fX, wn.pts()[2].fY,
            wnOutPt.fX, wnOutPt.fY);
    if (pts == 2) {
        SkDebugf(" wnTs[1]=%1.9g", wnTs[1]);
    }
    SkDebugf("\n");
}
#else
static void debugShowLineIntersection(int , const Work& ,
        const Work& , const double [2], const double [2]) {
}

static void debugShowQuadLineIntersection(int , const Work& ,
        const Work& , const double [2], const double [2]) {
}

static void debugShowQuadIntersection(int , const Work& ,
        const Work& , const double [2], const double [2]) {
}
#endif

static bool addIntersectTs(Contour* test, Contour* next) {

    if (test != next) {
        if (test->bounds().fBottom < next->bounds().fTop) {
            return false;
        }
        if (!Bounds::Intersects(test->bounds(), next->bounds())) {
            return true;
        }
    }
    Work wt;
    wt.init(test);
    bool foundCommonContour = test == next;
    do {
        Work wn;
        wn.init(next);
        if (test == next && !wn.startAfter(wt)) {
            continue;
        }
        do {
            if (!Bounds::Intersects(wt.bounds(), wn.bounds())) {
                continue;
            }
            int pts;
            Intersections ts;
            bool swap = false;
            switch (wt.segmentType()) {
                case Work::kHorizontalLine_Segment:
                    swap = true;
                    switch (wn.segmentType()) {
                        case Work::kHorizontalLine_Segment:
                        case Work::kVerticalLine_Segment:
                        case Work::kLine_Segment: {
                            pts = HLineIntersect(wn.pts(), wt.left(),
                                    wt.right(), wt.y(), wt.xFlipped(), ts);
                            debugShowLineIntersection(pts, wt, wn,
                                    ts.fT[1], ts.fT[0]);
                            break;
                        }
                        case Work::kQuad_Segment: {
                            pts = HQuadIntersect(wn.pts(), wt.left(),
                                    wt.right(), wt.y(), wt.xFlipped(), ts);
                            break;
                        }
                        case Work::kCubic_Segment: {
                            pts = HCubicIntersect(wn.pts(), wt.left(),
                                    wt.right(), wt.y(), wt.xFlipped(), ts);
                            break;
                        }
                        default:
                            SkASSERT(0);
                    }
                    break;
                case Work::kVerticalLine_Segment:
                    swap = true;
                    switch (wn.segmentType()) {
                        case Work::kHorizontalLine_Segment:
                        case Work::kVerticalLine_Segment:
                        case Work::kLine_Segment: {
                            pts = VLineIntersect(wn.pts(), wt.top(),
                                    wt.bottom(), wt.x(), wt.yFlipped(), ts);
                            debugShowLineIntersection(pts, wt, wn,
                                    ts.fT[1], ts.fT[0]);
                            break;
                        }
                        case Work::kQuad_Segment: {
                            pts = VQuadIntersect(wn.pts(), wt.top(),
                                    wt.bottom(), wt.x(), wt.yFlipped(), ts);
                            break;
                        }
                        case Work::kCubic_Segment: {
                            pts = VCubicIntersect(wn.pts(), wt.top(),
                                    wt.bottom(), wt.x(), wt.yFlipped(), ts);
                            break;
                        }
                        default:
                            SkASSERT(0);
                    }
                    break;
                case Work::kLine_Segment:
                    switch (wn.segmentType()) {
                        case Work::kHorizontalLine_Segment:
                            pts = HLineIntersect(wt.pts(), wn.left(),
                                    wn.right(), wn.y(), wn.xFlipped(), ts);
                            debugShowLineIntersection(pts, wt, wn,
                                    ts.fT[1], ts.fT[0]);
                            break;
                        case Work::kVerticalLine_Segment:
                            pts = VLineIntersect(wt.pts(), wn.top(),
                                    wn.bottom(), wn.x(), wn.yFlipped(), ts);
                            debugShowLineIntersection(pts, wt, wn,
                                    ts.fT[1], ts.fT[0]);
                            break;
                        case Work::kLine_Segment: {
                            pts = LineIntersect(wt.pts(), wn.pts(), ts);
                            debugShowLineIntersection(pts, wt, wn,
                                    ts.fT[1], ts.fT[0]);
                            break;
                        }
                        case Work::kQuad_Segment: {
                            swap = true;
                            pts = QuadLineIntersect(wn.pts(), wt.pts(), ts);
                            debugShowQuadLineIntersection(pts, wn, wt,
                                    ts.fT[0], ts.fT[1]);
                            break;
                        }
                        case Work::kCubic_Segment: {
                            swap = true;
                            pts = CubicLineIntersect(wn.pts(), wt.pts(), ts);
                            break;
                        }
                        default:
                            SkASSERT(0);
                    }
                    break;
                case Work::kQuad_Segment:
                    switch (wn.segmentType()) {
                        case Work::kHorizontalLine_Segment:
                            pts = HQuadIntersect(wt.pts(), wn.left(),
                                    wn.right(), wn.y(), wn.xFlipped(), ts);
                            break;
                        case Work::kVerticalLine_Segment:
                            pts = VQuadIntersect(wt.pts(), wn.top(),
                                    wn.bottom(), wn.x(), wn.yFlipped(), ts);
                            break;
                        case Work::kLine_Segment: {
                            pts = QuadLineIntersect(wt.pts(), wn.pts(), ts);
                            debugShowQuadLineIntersection(pts, wt, wn,
                                    ts.fT[0], ts.fT[1]);
                            break;
                        }
                        case Work::kQuad_Segment: {
                            pts = QuadIntersect(wt.pts(), wn.pts(), ts);
                            debugShowQuadIntersection(pts, wt, wn,
                                    ts.fT[0], ts.fT[1]);
                            break;
                        }
                        case Work::kCubic_Segment: {
                            wt.promoteToCubic();
                            pts = CubicIntersect(wt.cubic(), wn.pts(), ts);
                            break;
                        }
                        default:
                            SkASSERT(0);
                    }
                    break;
                case Work::kCubic_Segment:
                    switch (wn.segmentType()) {
                        case Work::kHorizontalLine_Segment:
                            pts = HCubicIntersect(wt.pts(), wn.left(),
                                    wn.right(), wn.y(), wn.xFlipped(), ts);
                            break;
                        case Work::kVerticalLine_Segment:
                            pts = VCubicIntersect(wt.pts(), wn.top(),
                                    wn.bottom(), wn.x(), wn.yFlipped(), ts);
                            break;
                        case Work::kLine_Segment: {
                            pts = CubicLineIntersect(wt.pts(), wn.pts(), ts);
                            break;
                        }
                        case Work::kQuad_Segment: {
                            wn.promoteToCubic();
                            pts = CubicIntersect(wt.pts(), wn.cubic(), ts);
                            break;
                        }
                        case Work::kCubic_Segment: {
                            pts = CubicIntersect(wt.pts(), wn.pts(), ts);
                            break;
                        }
                        default:
                            SkASSERT(0);
                    }
                    break;
                default:
                    SkASSERT(0);
            }
            if (!foundCommonContour && pts > 0) {
                test->addCross(next);
                next->addCross(test);
                foundCommonContour = true;
            }
            // in addition to recording T values, record matching segment
            if (pts == 2) {
                if (wn.segmentType() <= Work::kLine_Segment
                        && wt.segmentType() <= Work::kLine_Segment) {
                    wt.addCoincident(wn, ts, swap);
                    continue;
                }
                if (wn.segmentType() == Work::kQuad_Segment
                        && wt.segmentType() == Work::kQuad_Segment
                        && ts.coincidentUsed() == 2) {
                    wt.addCoincident(wn, ts, swap);
                    continue;
                }

            }
            for (int pt = 0; pt < pts; ++pt) {
                SkASSERT(ts.fT[0][pt] >= 0 && ts.fT[0][pt] <= 1);
                SkASSERT(ts.fT[1][pt] >= 0 && ts.fT[1][pt] <= 1);
                int testTAt = wt.addT(ts.fT[swap][pt], wn);
                int nextTAt = wn.addT(ts.fT[!swap][pt], wt);
                wt.addOtherT(testTAt, ts.fT[!swap][pt ^ ts.fFlip], nextTAt);
                wn.addOtherT(nextTAt, ts.fT[swap][pt ^ ts.fFlip], testTAt);
            }
        } while (wn.advance());
    } while (wt.advance());
    return true;
}

// resolve any coincident pairs found while intersecting, and
// see if coincidence is formed by clipping non-concident segments
static void coincidenceCheck(SkTDArray<Contour*>& contourList, bool otherXor, int total) {
    int contourCount = contourList.count();
    for (int cIndex = 0; cIndex < contourCount; ++cIndex) {
        Contour* contour = contourList[cIndex];
        contour->resolveCoincidence(contourList);
    }
    for (int cIndex = 0; cIndex < contourCount; ++cIndex) {
        Contour* contour = contourList[cIndex];
        contour->findTooCloseToCall(otherXor);
    }
}

// project a ray from the top of the contour up and see if it hits anything
// note: when we compute line intersections, we keep track of whether
// two contours touch, so we need only look at contours not touching this one.
// OPTIMIZATION: sort contourList vertically to avoid linear walk
static int innerContourCheck(SkTDArray<Contour*>& contourList,
        const Segment* current, int index, int endIndex, bool opp) {
    const SkPoint& basePt = current->xyAtT(endIndex);
    int contourCount = contourList.count();
    SkScalar bestY = SK_ScalarMin;
    const Segment* test = NULL;
    int tIndex;
    double tHit;
    for (int cTest = 0; cTest < contourCount; ++cTest) {
        Contour* contour = contourList[cTest];
        if ((contour->operand() ^ current->operand()) != opp) {
            continue;
        }
        if (basePt.fY < contour->bounds().fTop) {
            continue;
        }
        if (bestY > contour->bounds().fBottom) {
            continue;
        }
        const Segment* next = contour->crossedSegment(basePt, bestY, tIndex, tHit);
        if (next) {
            test = next;
        }
    }
    if (!test) {
        return 0;
    }
    int winding, windValue;
    // If the ray hit the end of a span, we need to construct the wheel of
    // angles to find the span closest to the ray -- even if there are just
    // two spokes on the wheel.
    const Angle* angle = NULL;
    if (approximately_zero(tHit - test->t(tIndex))) {
        SkTDArray<Angle> angles;
        int end = test->nextSpan(tIndex, 1);
        if (end < 0) {
            end = test->nextSpan(tIndex, -1);
        }
        test->addTwoAngles(end, tIndex, angles);
        SkASSERT(angles.count() > 0);
        if (angles[0].segment()->yAtT(angles[0].start()) >= basePt.fY) {
#if DEBUG_SORT
            SkDebugf("%s early return\n", __FUNCTION__);
#endif
            return 0;
        }
        test->buildAngles(tIndex, angles, false);
        SkTDArray<Angle*> sorted;
        // OPTIMIZATION: call a sort that, if base point is the leftmost,
        // returns the first counterclockwise hour before 6 o'clock,
        // or if the base point is rightmost, returns the first clockwise
        // hour after 6 o'clock
        (void) Segment::SortAngles(angles, sorted);
#if DEBUG_SORT
        sorted[0]->segment()->debugShowSort(__FUNCTION__, sorted, 0, 0, 0);
#endif
        // walk the sorted angle fan to find the lowest angle
        // above the base point. Currently, the first angle in the sorted array
        // is 12 noon or an earlier hour (the next counterclockwise)
        int count = sorted.count();
        int left = -1;
        int mid = -1;
        int right = -1;
        bool baseMatches = test->yAtT(tIndex) == basePt.fY;
        for (int index = 0; index < count; ++index) {
            angle = sorted[index];
            if (angle->unsortable()) {
                continue;
            }
            if (baseMatches && angle->isHorizontal()) {
                continue;
            }
            double indexDx = angle->dx();
            test = angle->segment();
            if (test->verb() > SkPath::kLine_Verb && approximately_zero(indexDx)) {
                const SkPoint* pts = test->pts();
                indexDx = pts[2].fX - pts[1].fX - indexDx;
            }
            if (indexDx < 0) {
                left = index;
            } else if (indexDx > 0) {
                right = index;
                int previous = index - 1;
                if (previous < 0) {
                    previous = count - 1;
                }
                const Angle* prev = sorted[previous];
                if (prev->dy() >= 0 && prev->dx() > 0 && angle->dy() < 0) {
#if DEBUG_SORT
                    SkDebugf("%s use prev\n", __FUNCTION__);
#endif
                    right = previous;
                }
                break;
            } else {
                mid = index;
            }
        }
        if (left < 0 && right < 0) {
            left = mid;
        }
        SkASSERT(left >= 0 || right >= 0);
        if (left < 0) {
            left = right;
        } else if (left >= 0 && mid >= 0 && right >= 0
                && sorted[mid]->sign() == sorted[right]->sign()) {
            left = right;
        }
        angle = sorted[left];
        test = angle->segment();
        winding = test->windSum(angle);
        SkASSERT(winding != SK_MinS32);
        windValue = test->windValue(angle);
#if DEBUG_WINDING
        SkDebugf("%s angle winding=%d windValue=%d sign=%d\n", __FUNCTION__, winding,
                windValue, angle->sign());
#endif
    } else {
        winding = test->windSum(tIndex);
        SkASSERT(winding != SK_MinS32);
        windValue = test->windValue(tIndex);
#if DEBUG_WINDING
        SkDebugf("%s single winding=%d windValue=%d\n", __FUNCTION__, winding,
                windValue);
#endif
    }
    // see if a + change in T results in a +/- change in X (compute x'(T))
    SkScalar dx = (*SegmentDXAtT[test->verb()])(test->pts(), tHit);
    if (test->verb() > SkPath::kLine_Verb && approximately_zero(dx)) {
        const SkPoint* pts = test->pts();
        dx = pts[2].fX - pts[1].fX - dx;
    }
#if DEBUG_WINDING
    SkDebugf("%s dx=%1.9g\n", __FUNCTION__, dx);
#endif
    SkASSERT(dx != 0);
    if (winding * dx > 0) { // if same signs, result is negative
        winding += dx > 0 ? -windValue : windValue;
#if DEBUG_WINDING
        SkDebugf("%s final winding=%d\n", __FUNCTION__, winding);
#endif
    }
    return winding;
}

static Segment* findUndone(SkTDArray<Contour*>& contourList, int& start, int& end) {
    int contourCount = contourList.count();
    Segment* result;
    for (int cIndex = 0; cIndex < contourCount; ++cIndex) {
        Contour* contour = contourList[cIndex];
        result = contour->undoneSegment(start, end);
        if (result) {
            return result;
        }
    }
    return NULL;
}



static Segment* findChase(SkTDArray<Span*>& chase, int& tIndex, int& endIndex) {
    while (chase.count()) {
        Span* span;
        chase.pop(&span);
        const Span& backPtr = span->fOther->span(span->fOtherIndex);
        Segment* segment = backPtr.fOther;
        tIndex = backPtr.fOtherIndex;
        SkTDArray<Angle> angles;
        int done = 0;
        if (segment->activeAngle(tIndex, done, angles)) {
            Angle* last = angles.end() - 1;
            tIndex = last->start();
            endIndex = last->end();
   #if TRY_ROTATE
            *chase.insert(0) = span;
   #else
            *chase.append() = span;
   #endif
            return last->segment();
        }
        if (done == angles.count()) {
            continue;
        }
        SkTDArray<Angle*> sorted;
        bool sortable = Segment::SortAngles(angles, sorted);
#if DEBUG_SORT
        sorted[0]->segment()->debugShowSort(__FUNCTION__, sorted, 0, 0, 0);
#endif
        if (!sortable) {
            continue;
        }
        // find first angle, initialize winding to computed fWindSum
        int firstIndex = -1;
        const Angle* angle;
        int winding;
        do {
            angle = sorted[++firstIndex];
            segment = angle->segment();
            winding = segment->windSum(angle);
        } while (winding == SK_MinS32);
        int spanWinding = segment->spanSign(angle->start(), angle->end());
    #if DEBUG_WINDING
        SkDebugf("%s winding=%d spanWinding=%d\n",
                __FUNCTION__, winding, spanWinding);
    #endif
        // turn span winding into contour winding
        if (spanWinding * winding < 0) {
            winding += spanWinding;
        }
    #if DEBUG_SORT
        segment->debugShowSort(__FUNCTION__, sorted, firstIndex, winding, 0);
    #endif
        // we care about first sign and whether wind sum indicates this
        // edge is inside or outside. Maybe need to pass span winding
        // or first winding or something into this function?
        // advance to first undone angle, then return it and winding
        // (to set whether edges are active or not)
        int nextIndex = firstIndex + 1;
        int angleCount = sorted.count();
        int lastIndex = firstIndex != 0 ? firstIndex : angleCount;
        angle = sorted[firstIndex];
        winding -= angle->segment()->spanSign(angle);
        do {
            SkASSERT(nextIndex != firstIndex);
            if (nextIndex == angleCount) {
                nextIndex = 0;
            }
            angle = sorted[nextIndex];
            segment = angle->segment();
            int maxWinding = winding;
            winding -= segment->spanSign(angle);
    #if DEBUG_SORT
            SkDebugf("%s id=%d maxWinding=%d winding=%d sign=%d\n", __FUNCTION__,
                    segment->debugID(), maxWinding, winding, angle->sign());
    #endif
            tIndex = angle->start();
            endIndex = angle->end();
            int lesser = SkMin32(tIndex, endIndex);
            const Span& nextSpan = segment->span(lesser);
            if (!nextSpan.fDone) {
#if 1
            // FIXME: this be wrong. assign startWinding if edge is in
            // same direction. If the direction is opposite, winding to
            // assign is flipped sign or +/- 1?
                if (useInnerWinding(maxWinding, winding)) {
                    maxWinding = winding;
                }
                segment->markWinding(lesser, maxWinding);
#endif
                break;
            }
        } while (++nextIndex != lastIndex);
   #if TRY_ROTATE
        *chase.insert(0) = span;
   #else
        *chase.append() = span;
   #endif
        return segment;
    }
    return NULL;
}

#if DEBUG_ACTIVE_SPANS
static void debugShowActiveSpans(SkTDArray<Contour*>& contourList) {
    int index;
    for (index = 0; index < contourList.count(); ++ index) {
        contourList[index]->debugShowActiveSpans();
    }
    for (index = 0; index < contourList.count(); ++ index) {
        contourList[index]->validateActiveSpans();
    }
}
#endif

static bool windingIsActive(int winding, int spanWinding) {
    // FIXME: !spanWinding test must be superflorous, true?
    return winding * spanWinding <= 0 && abs(winding) <= abs(spanWinding)
            && (!winding || !spanWinding || winding == -spanWinding);
}

static Segment* findSortableTop(SkTDArray<Contour*>& contourList, int& index,
        int& endIndex, SkPoint& topLeft) {
    Segment* result;
    do {
        SkPoint bestXY = {SK_ScalarMax, SK_ScalarMax};
        int contourCount = contourList.count();
        Segment* topStart = NULL;
        for (int cIndex = 0; cIndex < contourCount; ++cIndex) {
            Contour* contour = contourList[cIndex];
            const Bounds& bounds = contour->bounds();
            if (bounds.fBottom < topLeft.fY) {
                continue;
            }
            if (bounds.fBottom == topLeft.fY && bounds.fRight < topLeft.fX) {
                continue;
            }
            Segment* test = contour->topSortableSegment(topLeft, bestXY);
            if (test) {
                topStart = test;
            }
        }
        if (!topStart) {
            return NULL;
        }
        topLeft = bestXY;
        result = topStart->findTop(index, endIndex);
    } while (!result);
    return result;
}

static int updateWindings(const Segment* current, int index, int endIndex, int& spanWinding) {
    int lesser = SkMin32(index, endIndex);
    spanWinding = current->spanSign(index, endIndex);
    int winding = current->windSum(lesser);
    bool inner = useInnerWinding(winding - spanWinding, winding);
#if DEBUG_WINDING
    SkDebugf("%s id=%d t=%1.9g spanWinding=%d winding=%d sign=%d"
            " inner=%d result=%d\n",
            __FUNCTION__, current->debugID(), current->t(lesser),
            spanWinding, winding, SkSign32(index - endIndex),
            useInnerWinding(winding - spanWinding, winding),
            inner ? winding - spanWinding : winding);
#endif
    if (inner) {
        winding -= spanWinding;
    }
    return winding;
}

// Each segment may have an inside or an outside. Segments contained within
// winding may have insides on either side, and form a contour that should be
// ignored. Segments that are coincident with opposing direction segments may
// have outsides on either side, and should also disappear.
// 'Normal' segments will have one inside and one outside. Subsequent connections
// when winding should follow the intersection direction. If more than one edge
// is an option, choose first edge that continues the inside.
    // since we start with leftmost top edge, we'll traverse through a
    // smaller angle counterclockwise to get to the next edge.
// returns true if all edges were processed
static bool bridgeWinding(SkTDArray<Contour*>& contourList, PathWrapper& simple) {
    bool firstContour = true;
    bool unsortable = false;
    bool closable = true;
    SkPoint topLeft = {SK_ScalarMin, SK_ScalarMin};
    do {
        int index, endIndex;
        // iterates while top is unsortable
        Segment* current = findSortableTop(contourList, index, endIndex, topLeft);
        if (!current) {
            break;
        }
        int contourWinding;
        if (firstContour) {
            contourWinding = 0;
            firstContour = false;
        } else {
            int sumWinding = current->windSum(SkMin32(index, endIndex));
            // FIXME: don't I have to adjust windSum to get contourWinding?
            if (sumWinding == SK_MinS32) {
                sumWinding = current->computeSum(index, endIndex, false);
            }
            if (sumWinding == SK_MinS32) {
                contourWinding = innerContourCheck(contourList, current,
                        index, endIndex, false);
            } else {
                contourWinding = sumWinding;
                int spanWinding = current->spanSign(index, endIndex);
                bool inner = useInnerWinding(sumWinding - spanWinding, sumWinding);
                if (inner) {
                    contourWinding -= spanWinding;
                }
#if DEBUG_WINDING
                SkDebugf("%s sumWinding=%d spanWinding=%d sign=%d inner=%d result=%d\n",
                        __FUNCTION__, sumWinding, spanWinding, SkSign32(index - endIndex),
                        inner, contourWinding);
#endif
            }
#if DEBUG_WINDING
         //   SkASSERT(current->debugVerifyWinding(index, endIndex, contourWinding));
            SkDebugf("%s contourWinding=%d\n", __FUNCTION__, contourWinding);
#endif
        }
        int winding = contourWinding;
        int spanWinding = current->spanSign(index, endIndex);
        // FIXME: needs work. While it works in limited situations, it does
        // not always compute winding correctly. Active should be removed and instead
        // the initial winding should be correctly passed in so that if the
        // inner contour is wound the same way, it never finds an accumulated
        // winding of zero. Inside 'find next', we need to look for transitions
        // other than zero when resolving sorted angles.
        SkTDArray<Span*> chaseArray;
        do {
            bool active = windingIsActive(winding, spanWinding);
        #if DEBUG_WINDING
            SkDebugf("%s active=%s winding=%d spanWinding=%d\n",
                    __FUNCTION__, active ? "true" : "false",
                    winding, spanWinding);
        #endif
            do {
                SkASSERT(unsortable || !current->done());
                int nextStart = index;
                int nextEnd = endIndex;
                Segment* next = current->findNextWinding(chaseArray, active,
                        nextStart, nextEnd, winding, spanWinding, unsortable);
                if (!next) {
                    if (active && !unsortable && simple.hasMove()
                            && current->verb() != SkPath::kLine_Verb
                            && !simple.isClosed()) {
                        current->addCurveTo(index, endIndex, simple, true);
                        SkASSERT(simple.isClosed());
                    }
                    break;
                }
                current->addCurveTo(index, endIndex, simple, active);
                current = next;
                index = nextStart;
                endIndex = nextEnd;
            } while (!simple.isClosed()
                    && ((active && !unsortable) || !current->done()));
            if (active) {
                if (!simple.isClosed()) {
                    SkASSERT(unsortable);
                    int min = SkMin32(index, endIndex);
                    if (!current->done(min)) {
                        current->addCurveTo(index, endIndex, simple, true);
                        current->markDone(SkMin32(index, endIndex),
                                winding ? winding : spanWinding);
                    }
                    closable = false;
                }
                simple.close();
            }
            current = findChase(chaseArray, index, endIndex);
        #if DEBUG_ACTIVE_SPANS
            debugShowActiveSpans(contourList);
        #endif
            if (!current) {
                break;
            }
            winding = updateWindings(current, index, endIndex, spanWinding);
        } while (true);
    } while (true);
    return closable;
}

// returns true if all edges were processed
static bool bridgeXor(SkTDArray<Contour*>& contourList, PathWrapper& simple) {
    Segment* current;
    int start, end;
    bool unsortable = false;
    while ((current = findUndone(contourList, start, end))) {
        do {
            SkASSERT(unsortable || !current->done());
            int nextStart = start;
            int nextEnd = end;
            Segment* next = current->findNextXor(nextStart, nextEnd, unsortable);
            if (!next) {
                if (simple.hasMove()
                        && current->verb() != SkPath::kLine_Verb
                        && !simple.isClosed()) {
                    current->addCurveTo(start, end, simple, true);
                    SkASSERT(simple.isClosed());
                }
                break;
            }
            current->addCurveTo(start, end, simple, true);
            current = next;
            start = nextStart;
            end = nextEnd;
        } while (!simple.isClosed());
        // FIXME: add unsortable test
        if (simple.hasMove()) {
            simple.close();
        }
    #if DEBUG_ACTIVE_SPANS
        debugShowActiveSpans(contourList);
    #endif
    }
    return !unsortable;
}

static void fixOtherTIndex(SkTDArray<Contour*>& contourList) {
    int contourCount = contourList.count();
    for (int cTest = 0; cTest < contourCount; ++cTest) {
        Contour* contour = contourList[cTest];
        contour->fixOtherTIndex();
    }
}

static void sortSegments(SkTDArray<Contour*>& contourList) {
    int contourCount = contourList.count();
    for (int cTest = 0; cTest < contourCount; ++cTest) {
        Contour* contour = contourList[cTest];
        contour->sortSegments();
    }
}

static void makeContourList(SkTArray<Contour>& contours,
        SkTDArray<Contour*>& list) {
    int count = contours.count();
    if (count == 0) {
        return;
    }
    for (int index = 0; index < count; ++index) {
        *list.append() = &contours[index];
    }
    QSort<Contour>(list.begin(), list.end() - 1);
}

static bool approximatelyEqual(const SkPoint& a, const SkPoint& b) {
    return AlmostEqualUlps(a.fX, b.fX) && AlmostEqualUlps(a.fY, b.fY);
}

    /*
        check start and end of each contour
        if not the same, record them
        match them up
        connect closest
        reassemble contour pieces into new path
    */
static void assemble(const PathWrapper& path, PathWrapper& simple) {
#if DEBUG_PATH_CONSTRUCTION
    SkDebugf("%s\n", __FUNCTION__);
#endif
    SkTArray<Contour> contours;
    EdgeBuilder builder(path, contours);
    builder.finish();
    int count = contours.count();
    int outer;
    SkTDArray<int> runs; // indices of partial contours
    for (outer = 0; outer < count; ++outer) {
        const Contour& eContour = contours[outer];
        const SkPoint& eStart = eContour.start();
        const SkPoint& eEnd = eContour.end();
        if (approximatelyEqual(eStart, eEnd)) {
            eContour.toPath(simple);
            continue;
        }
        *runs.append() = outer;
    }
    count = runs.count();
    if (count == 0) {
        return;
    }
    SkTDArray<int> sLink, eLink;
    sLink.setCount(count);
    eLink.setCount(count);
    SkTDArray<double> sBest, eBest;
    sBest.setCount(count);
    eBest.setCount(count);
    int rIndex;
    for (rIndex = 0; rIndex < count; ++rIndex) {
        outer = runs[rIndex];
        const Contour& oContour = contours[outer];
        const SkPoint& oStart = oContour.start();
        const SkPoint& oEnd = oContour.end();
        double dx = oEnd.fX - oStart.fX;
        double dy = oEnd.fY - oStart.fY;
        double dist = dx * dx + dy * dy;
        sBest[rIndex] = eBest[rIndex] = dist;
        sLink[rIndex] = eLink[rIndex] = rIndex;
    }
    for (rIndex = 0; rIndex < count - 1; ++rIndex) {
        outer = runs[rIndex];
        const Contour& oContour = contours[outer];
        const SkPoint& oStart = oContour.start();
        const SkPoint& oEnd = oContour.end();
        double bestStartDist = sBest[rIndex];
        double bestEndDist = eBest[rIndex];
        for (int iIndex = rIndex + 1; iIndex < count; ++iIndex) {
            int inner = runs[iIndex];
            const Contour& iContour = contours[inner];
            const SkPoint& iStart = iContour.start();
            const SkPoint& iEnd = iContour.end();
            double dx = iStart.fX - oStart.fX;
            double dy = iStart.fY - oStart.fY;
            double dist = dx * dx + dy * dy;
            if (bestStartDist > dist) {
                bestStartDist = dist;
                sLink[rIndex] = ~iIndex;
                sLink[iIndex] = ~rIndex;
            }
            dx = iEnd.fX - oStart.fX;
            dy = iEnd.fY - oStart.fY;
            dist = dx * dx + dy * dy;
            if (bestStartDist > dist) {
                bestStartDist = dist;
                sLink[rIndex] = iIndex;
                eLink[iIndex] = rIndex;
            }
            dx = iStart.fX - oEnd.fX;
            dy = iStart.fY - oEnd.fY;
            dist = dx * dx + dy * dy;
            if (bestEndDist > dist) {
                bestEndDist = dist;
                sLink[iIndex] = rIndex;
                eLink[rIndex] = iIndex;
            }
            dx = iEnd.fX - oEnd.fX;
            dy = iEnd.fY - oEnd.fY;
            dist = dx * dx + dy * dy;
            if (bestEndDist > dist) {
                bestEndDist = dist;
                eLink[iIndex] = ~rIndex;
                eLink[rIndex] = ~iIndex;
            }
       }
    }
    rIndex = 0;
    bool forward = true;
    bool first = true;
    const SkPoint* startPtr;
    int sIndex = sLink[rIndex];
    SkASSERT(sIndex != INT_MAX);
    sLink[rIndex] = INT_MAX;
    int eIndex;
    if (sIndex < 0) {
        eIndex = sLink[~sIndex];
        sLink[~sIndex] = INT_MAX;
    } else {
        eIndex = eLink[sIndex];
        eLink[sIndex] = INT_MAX;
    }
    SkASSERT(eIndex != INT_MAX);
    do {
        do {
            outer = runs[rIndex];
            const Contour& contour = contours[outer];
            if (first) {
                startPtr = &contour.start();
                first = false;
                simple.deferredMove(startPtr[0]);
            }
            const SkPoint* endPtr;
            if (forward) {
                contour.toPartialForward(simple);
                endPtr = &contour.end();
            } else {
                contour.toPartialBackward(simple);
                endPtr = &contour.start();
            }
            if (sIndex == eIndex) {
                simple.close();
                first = forward = true;
                break;
            }
            if (forward) {
                eIndex = eLink[rIndex];
                SkASSERT(eIndex != INT_MAX);
                eLink[rIndex] = INT_MAX;
                if (eIndex >= 0) {
                    SkASSERT(sLink[eIndex] == rIndex);
                    sLink[eIndex] = INT_MAX;
                } else {
                    SkASSERT(eLink[~eIndex] == ~rIndex);
                    eLink[~eIndex] = INT_MAX;
                }
            } else {
                eIndex = sLink[rIndex];
                SkASSERT(eIndex != INT_MAX);
                sLink[rIndex] = INT_MAX;
                if (eIndex >= 0) {
                    SkASSERT(eLink[eIndex] == rIndex);
                    eLink[eIndex] = INT_MAX;
                } else {
                    SkASSERT(sLink[~eIndex] == ~rIndex);
                    sLink[~eIndex] = INT_MAX;
                }
            }
            rIndex = eIndex;
            if (rIndex < 0) {
                forward ^= 1;
                rIndex = ~rIndex;
            }
        } while (true);
        for (rIndex = 0; rIndex < count; ++rIndex) {
            if (sLink[rIndex] != INT_MAX) {
                break;
            }
        }
    } while (rIndex < count);
    SkASSERT(first);
}

void simplifyx(const SkPath& path, SkPath& result) {
    // returns 1 for evenodd, -1 for winding, regardless of inverse-ness
    result.reset();
    result.setFillType(SkPath::kEvenOdd_FillType);
    PathWrapper simple(result);

    // turn path into list of segments
    SkTArray<Contour> contours;
    // FIXME: add self-intersecting cubics' T values to segment
    EdgeBuilder builder(path, contours);
    builder.finish();
    SkTDArray<Contour*> contourList;
    makeContourList(contours, contourList);
    Contour** currentPtr = contourList.begin();
    if (!currentPtr) {
        return;
    }
    Contour** listEnd = contourList.end();
    // find all intersections between segments
    do {
        Contour** nextPtr = currentPtr;
        Contour* current = *currentPtr++;
        Contour* next;
        do {
            next = *nextPtr++;
        } while (addIntersectTs(current, next) && nextPtr != listEnd);
    } while (currentPtr != listEnd);
    // eat through coincident edges
    coincidenceCheck(contourList, false, 0);
    fixOtherTIndex(contourList);
    sortSegments(contourList);
#if DEBUG_ACTIVE_SPANS
    debugShowActiveSpans(contourList);
#endif
    // construct closed contours
    if (builder.xorMask() == kWinding_Mask
                ? !bridgeWinding(contourList, simple)
                : !bridgeXor(contourList, simple))
    { // if some edges could not be resolved, assemble remaining fragments
        SkPath temp;
        temp.setFillType(SkPath::kEvenOdd_FillType);
        PathWrapper assembled(temp);
        assemble(simple, assembled);
        result = *assembled.nativePath();
    }
}


/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "EdgeWalker_Test.h"
#include "Intersection_Tests.h"
#include "SkBitmap.h"
#include "SkCanvas.h"
#include "SkPaint.h"
#include "SkStream.h"

#include <algorithm>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/sysctl.h>

#undef SkASSERT
#define SkASSERT(cond) while (!(cond)) { sk_throw(); }

static const char marker[] =
    "</div>\n"
    "\n"
    "<script type=\"text/javascript\">\n"
    "\n"
    "var testDivs = [\n";

static const char* opStrs[] = {
    "kDifference_Op",
    "kIntersect_Op",
    "kUnion_Op",
    "kXor_Op",
};

static const char* opSuffixes[] = {
    "d",
    "i",
    "u",
    "x",
};

static const char preferredFilename[] = "/flash/debug/XX.txt";
static const char backupFilename[] = "../../experimental/Intersection/debugXX.txt";

static bool gShowPath = false;
static bool gComparePaths = true;
static bool gShowOutputProgress = false;
static bool gComparePathsAssert = true;
static bool gPathStrAssert = true;
static bool gUsePhysicalFiles = false;

static bool isRectContour(SkPath::Iter& iter, SkRect& rect, SkPath::Direction& direction) {
    int corners = 0;
    SkPoint first, last;
    first.set(0, 0);
    last.set(0, 0);
    int firstDirection = 0;
    int lastDirection = 0;
    int nextDirection = 0;
    bool closedOrMoved = false;
    bool autoClose = false;
    rect.setEmpty();
    uint8_t verb;
    SkPoint data[4];
    SkTDArray<SkPoint> sides;
    bool empty = true;
    while ((verb = iter.next(data)) != SkPath::kDone_Verb && !autoClose) {
        empty = false;
        SkPoint* pts = &data[1];
        switch (verb) {
            case SkPath::kClose_Verb:
                pts = &last;
                autoClose = true;
            case SkPath::kLine_Verb: {
                SkScalar left = last.fX;
                SkScalar top = last.fY;
                SkScalar right = pts->fX;
                SkScalar bottom = pts->fY;
                *sides.append() = *pts;
                ++pts;
                if (left != right && top != bottom) {
                    return false; // diagonal
                }
                if (left == right && top == bottom) {
                    break; // single point on side OK
                }
                nextDirection = (left != right) << 0 |
                    (left < right || top < bottom) << 1;
                if (0 == corners) {
                    firstDirection = nextDirection;
                    first = last;
                    last = pts[-1];
                    corners = 1;
                    closedOrMoved = false;
                    break;
                }
                if (closedOrMoved) {
                    return false; // closed followed by a line
                }
                if (autoClose && nextDirection == firstDirection) {
                    break; // colinear with first
                }
                closedOrMoved = autoClose;
                if (lastDirection != nextDirection) {
                    if (++corners > 4) {
                        return false; // too many direction changes
                    }
                }
                last = pts[-1];
                if (lastDirection == nextDirection) {
                    break; // colinear segment
                }
                // Possible values for corners are 2, 3, and 4.
                // When corners == 3, nextDirection opposes firstDirection.
                // Otherwise, nextDirection at corner 2 opposes corner 4.
                int turn = firstDirection ^ (corners - 1);
                int directionCycle = 3 == corners ? 0 : nextDirection ^ turn;
                if ((directionCycle ^ turn) != nextDirection) {
                    return false; // direction didn't follow cycle
                }
                break;
            }
            case SkPath::kQuad_Verb:
            case SkPath::kCubic_Verb:
                return false; // quadratic, cubic not allowed
            case SkPath::kMove_Verb:
                last = *pts++;
                *sides.append() = last;
                closedOrMoved = true;
                break;
        }
        lastDirection = nextDirection;
    }
    // Success if 4 corners and first point equals last
    bool result = 4 == corners && (first == last || autoClose);
    if (result) {
        direction = firstDirection == (lastDirection + 1 & 3) ? SkPath::kCCW_Direction
                : SkPath::kCW_Direction;
        rect.set(&sides[0], sides.count());
    } else {
        rect.setEmpty();
    }
    return !empty;
}

static void showPathContour(SkPath::Iter& iter, bool skip) {
    uint8_t verb;
    SkPoint pts[4];
    while ((verb = iter.next(pts)) != SkPath::kDone_Verb) {
        if (skip) {
            if (verb == SkPath::kClose_Verb) {
                return;
            }
        }
        switch (verb) {
            case SkPath::kMove_Verb:
                SkDebugf("path.moveTo(%1.9g, %1.9g);\n", pts[0].fX, pts[0].fY);
                continue;
            case SkPath::kLine_Verb:
                SkDebugf("path.lineTo(%1.9g, %1.9g);\n", pts[1].fX, pts[1].fY);
                break;
            case SkPath::kQuad_Verb:
                SkDebugf("path.quadTo(%1.9g, %1.9g, %1.9g, %1.9g);\n",
                    pts[1].fX, pts[1].fY, pts[2].fX, pts[2].fY);
                break;
            case SkPath::kCubic_Verb:
                SkDebugf("path.cubicTo(%1.9g, %1.9g, %1.9g, %1.9g);\n",
                    pts[1].fX, pts[1].fY, pts[2].fX, pts[2].fY,
                    pts[3].fX, pts[3].fY);
                break;
            case SkPath::kClose_Verb:
                SkDebugf("path.close();\n");
                return;
            default:
                SkDEBUGFAIL("bad verb");
                return;
        }
    }
}

void showPath(const SkPath& path, const char* str) {
    SkDebugf("%s\n", !str ? "original:" : str);
    SkPath::Iter iter(path, true);
    SkTDArray<SkRect> rects;
    SkTDArray<SkPath::Direction> directions;
    SkRect rect;
    SkPath::Direction direction;
    while (isRectContour(iter, rect, direction)) {
        *rects.append() = rect;
        *directions.append() = direction;
    }
    iter.setPath(path, true);
    for (int contour = 0; contour < rects.count(); ++contour) {
        const SkRect& rect = rects[contour];
        bool useRect = !rect.isEmpty();
        showPathContour(iter, useRect);
        if (useRect) {
            SkDebugf("path.addRect(%1.9g, %1.9g, %1.9g, %1.9g, %s);\n", rect.fLeft, rect.fTop,
                    rect.fRight, rect.fBottom, directions[contour] == SkPath::kCCW_Direction
                    ? "SkPath::kCCW_Direction" : "SkPath::kCW_Direction");
        }
    }
}

static int pathsDrawTheSame(const SkPath& one, const SkPath& two,
        SkBitmap& bits, SkPath& scaledOne, SkPath& scaledTwo, int& error2x2) {
    const int bitWidth = 64;
    const int bitHeight = 64;
    if (bits.width() == 0) {
        bits.setConfig(SkBitmap::kARGB_8888_Config, bitWidth * 2, bitHeight);
        bits.allocPixels();
    }

    SkRect larger = one.getBounds();
    larger.join(two.getBounds());
    SkScalar largerWidth = larger.width();
    if (largerWidth < 4) {
        largerWidth = 4;
    }
    SkScalar largerHeight = larger.height();
    if (largerHeight < 4) {
        largerHeight = 4;
    }
    SkScalar hScale = (bitWidth - 2) / largerWidth;
    SkScalar vScale = (bitHeight - 2) / largerHeight;
    SkMatrix scale;
    scale.reset();
    scale.preScale(hScale, vScale);
    one.transform(scale, &scaledOne);
    two.transform(scale, &scaledTwo);
    const SkRect& bounds1 = scaledOne.getBounds();

    SkCanvas canvas(bits);
    canvas.drawColor(SK_ColorWHITE);
    SkPaint paint;
    canvas.save();
    canvas.translate(-bounds1.fLeft + 1, -bounds1.fTop + 1);
    canvas.drawPath(scaledOne, paint);
    canvas.restore();
    canvas.save();
    canvas.translate(-bounds1.fLeft + 1 + bitWidth, -bounds1.fTop + 1);
    canvas.drawPath(scaledTwo, paint);
    canvas.restore();
    int errors2 = 0;
    int errors = 0;
    for (int y = 0; y < bitHeight - 1; ++y) {
        uint32_t* addr1 = bits.getAddr32(0, y);
        uint32_t* addr2 = bits.getAddr32(0, y + 1);
        uint32_t* addr3 = bits.getAddr32(bitWidth, y);
        uint32_t* addr4 = bits.getAddr32(bitWidth, y + 1);
        for (int x = 0; x < bitWidth - 1; ++x) {
            // count 2x2 blocks
            bool err = addr1[x] != addr3[x];
            if (err) {
                errors2 += addr1[x + 1] != addr3[x + 1]
                        && addr2[x] != addr4[x] && addr2[x + 1] != addr4[x + 1];
                errors++;
            }
        }
    }
    if (errors2 >= 6 || errors > 160) {
        SkDebugf("%s errors2=%d errors=%d\n", __FUNCTION__, errors2, errors);
    }
    error2x2 = errors2;
    return errors;
}

bool drawAsciiPaths(const SkPath& one, const SkPath& two, bool drawPaths) {
    if (!drawPaths) {
        return true;
    }
    const SkRect& bounds1 = one.getBounds();
    const SkRect& bounds2 = two.getBounds();
    SkRect larger = bounds1;
    larger.join(bounds2);
    SkBitmap bits;
    char out[256];
    int bitWidth = SkScalarCeil(larger.width()) + 2;
    if (bitWidth * 2 + 1 >= (int) sizeof(out)) {
        return false;
    }
    int bitHeight = SkScalarCeil(larger.height()) + 2;
    if (bitHeight >= (int) sizeof(out)) {
        return false;
    }
    bits.setConfig(SkBitmap::kARGB_8888_Config, bitWidth * 2, bitHeight);
    bits.allocPixels();
    SkCanvas canvas(bits);
    canvas.drawColor(SK_ColorWHITE);
    SkPaint paint;
    canvas.save();
    canvas.translate(-bounds1.fLeft + 1, -bounds1.fTop + 1);
    canvas.drawPath(one, paint);
    canvas.restore();
    canvas.save();
    canvas.translate(-bounds1.fLeft + 1 + bitWidth, -bounds1.fTop + 1);
    canvas.drawPath(two, paint);
    canvas.restore();
    for (int y = 0; y < bitHeight; ++y) {
        uint32_t* addr1 = bits.getAddr32(0, y);
        int x;
        char* outPtr = out;
        for (x = 0; x < bitWidth; ++x) {
            *outPtr++ = addr1[x] == (uint32_t) -1 ? '_' : 'x';
        }
        *outPtr++ = '|';
        for (x = bitWidth; x < bitWidth * 2; ++x) {
            *outPtr++ = addr1[x] == (uint32_t) -1 ? '_' : 'x';
        }
        *outPtr++ = '\0';
        SkDebugf("%s\n", out);
    }
    return true;
}

static void showSimplifiedPath(const SkPath& one, const SkPath& two,
        const SkPath& scaledOne, const SkPath& scaledTwo) {
    showPath(one, "original:");
    showPath(two, "simplified:");
    drawAsciiPaths(scaledOne, scaledTwo, true);
}

int comparePaths(const SkPath& one, const SkPath& two, SkBitmap& bitmap) {
    int errors2x2;
    SkPath scaledOne, scaledTwo;
    int errors = pathsDrawTheSame(one, two, bitmap, scaledOne, scaledTwo, errors2x2);
    if (errors2x2 == 0) {
        return 0;
    }
    const int MAX_ERRORS = 8;
    if (errors2x2 == MAX_ERRORS || errors2x2 == MAX_ERRORS - 1) {
        showSimplifiedPath(one, two, scaledOne, scaledTwo);
    }
    if (errors2x2 > MAX_ERRORS && gComparePathsAssert) {
        SkDebugf("%s errors=%d\n", __FUNCTION__, errors);
        showSimplifiedPath(one, two, scaledOne, scaledTwo);
        SkASSERT(0);
    }
    return errors2x2 > MAX_ERRORS ? errors2x2 : 0;
}

static void showShapeOpPath(const SkPath& one, const SkPath& two, const SkPath& a, const SkPath& b,
        const SkPath& scaledOne, const SkPath& scaledTwo, const ShapeOp shapeOp) {
    SkASSERT((unsigned) shapeOp < sizeof(opStrs) / sizeof(opStrs[0]));
    showPath(a, "minuend:");
    SkDebugf("op: %s\n", opStrs[shapeOp]);
    showPath(b, "subtrahend:");
    showPath(one, "region:");
    showPath(two, "op result:");
    drawAsciiPaths(scaledOne, scaledTwo, true);
}

int comparePaths(const SkPath& one, const SkPath& two, SkBitmap& bitmap,
        const SkPath& a, const SkPath& b, const ShapeOp shapeOp) {
    int errors2x2;
    SkPath scaledOne, scaledTwo;
    int errors = pathsDrawTheSame(one, two, bitmap, scaledOne, scaledTwo, errors2x2);
    if (errors2x2 == 0) {
        return 0;
    }
    const int MAX_ERRORS = 8;
    if (errors2x2 == MAX_ERRORS || errors2x2 == MAX_ERRORS - 1) {
        showShapeOpPath(one, two, a, b, scaledOne, scaledTwo, shapeOp);
    }
    if (errors2x2 > MAX_ERRORS && gComparePathsAssert) {
        SkDebugf("%s errors=%d\n", __FUNCTION__, errors);
        showShapeOpPath(one, two, a, b, scaledOne, scaledTwo, shapeOp);
        SkASSERT(0);
    }
    return errors2x2 > MAX_ERRORS ? errors2x2 : 0;
}

// doesn't work yet
void comparePathsTiny(const SkPath& one, const SkPath& two) {
    const SkRect& bounds1 = one.getBounds();
    const SkRect& bounds2 = two.getBounds();
    SkRect larger = bounds1;
    larger.join(bounds2);
    SkBitmap bits;
    int bitWidth = SkScalarCeil(larger.width()) + 2;
    int bitHeight = SkScalarCeil(larger.height()) + 2;
    bits.setConfig(SkBitmap::kA1_Config, bitWidth * 2, bitHeight);
    bits.allocPixels();
    SkCanvas canvas(bits);
    canvas.drawColor(SK_ColorWHITE);
    SkPaint paint;
    canvas.save();
    canvas.translate(-bounds1.fLeft + 1, -bounds1.fTop + 1);
    canvas.drawPath(one, paint);
    canvas.restore();
    canvas.save();
    canvas.translate(-bounds2.fLeft + 1, -bounds2.fTop + 1);
    canvas.drawPath(two, paint);
    canvas.restore();
    for (int y = 0; y < bitHeight; ++y) {
        uint8_t* addr1 = bits.getAddr1(0, y);
        uint8_t* addr2 = bits.getAddr1(bitWidth, y);
        for (int x = 0; x < bits.rowBytes(); ++x) {
            SkASSERT(addr1[x] == addr2[x]);
        }
    }
}

bool testSimplify(const SkPath& path, bool fill, SkPath& out, SkBitmap& bitmap) {
    if (gShowPath) {
        showPath(path);
    }
    simplify(path, fill, out);
    if (!gComparePaths) {
        return true;
    }
    return comparePaths(path, out, bitmap) == 0;
}

bool testSimplifyx(SkPath& path, bool useXor, SkPath& out, State4& state,
        const char* pathStr) {
    SkPath::FillType fillType = useXor ? SkPath::kEvenOdd_FillType : SkPath::kWinding_FillType;
    path.setFillType(fillType);
    if (gShowPath) {
        showPath(path);
    }
    simplifyx(path, out);
    if (!gComparePaths) {
        return true;
    }
    int result = comparePaths(path, out, state.bitmap);
    if (result && gPathStrAssert) {
        SkDebugf("addTest %s\n", state.filename);
        char temp[8192];
        bzero(temp, sizeof(temp));
        SkMemoryWStream stream(temp, sizeof(temp));
        const char* pathPrefix = NULL;
        const char* nameSuffix = NULL;
        if (fillType == SkPath::kEvenOdd_FillType) {
            pathPrefix = "    path.setFillType(SkPath::kEvenOdd_FillType);\n";
            nameSuffix = "x";
        }
        const char testFunction[] = "testSimplifyx(path);";
        outputToStream(state, pathStr, pathPrefix, nameSuffix, testFunction, stream);
        SkDebugf(temp);
        SkASSERT(0);
    }
    return result == 0;
}

bool testSimplifyx(const SkPath& path) {
    SkPath out;
    simplifyx(path, out);
    SkBitmap bitmap;
    int result = comparePaths(path, out, bitmap);
    if (result && gPathStrAssert) {
        SkASSERT(0);
    }
    return result == 0;
}

bool testShapeOp(const SkPath& a, const SkPath& b, const ShapeOp shapeOp) {
    SkPath out;
    operate(a, b, shapeOp, out);
    SkPath pathOut;
    SkRegion rgnA, rgnB, openClip, rgnOut;
    openClip.setRect(-16000, -16000, 16000, 16000);
    rgnA.setPath(a, openClip);
    rgnB.setPath(b, openClip);
    rgnOut.op(rgnA, rgnB, (SkRegion::Op) shapeOp);
    rgnOut.getBoundaryPath(&pathOut);
    SkBitmap bitmap;
    int result = comparePaths(pathOut, out, bitmap, a, b, shapeOp);
    if (result && gPathStrAssert) {
        SkASSERT(0);
    }
    return result == 0;
}

const int maxThreadsAllocated = 64;
static int maxThreads = 1;
static int threadIndex;
State4 threadState[maxThreadsAllocated];
static int testNumber;
static const char* testName;
static bool debugThreads = false;

State4* State4::queue = NULL;
pthread_mutex_t State4::addQueue = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t State4::checkQueue = PTHREAD_COND_INITIALIZER;

State4::State4() {
    bitmap.setConfig(SkBitmap::kARGB_8888_Config, 150 * 2, 100);
    bitmap.allocPixels();
}

void createThread(State4* statePtr, void* (*testFun)(void* )) {
    int threadError = pthread_create(&statePtr->threadID, NULL, testFun,
            (void*) statePtr);
    SkASSERT(!threadError);
}

int dispatchTest4(void* (*testFun)(void* ), int a, int b, int c, int d) {
    int testsRun = 0;
    State4* statePtr;
    if (!gRunTestsInOneThread) {
        pthread_mutex_lock(&State4::addQueue);
        if (threadIndex < maxThreads) {
            statePtr = &threadState[threadIndex];
            statePtr->testsRun = 0;
            statePtr->a = a;
            statePtr->b = b;
            statePtr->c = c;
            statePtr->d = d;
            statePtr->done = false;
            statePtr->index = threadIndex;
            statePtr->last = false;
            if (debugThreads) SkDebugf("%s %d create done=%d last=%d\n", __FUNCTION__,
                    statePtr->index, statePtr->done, statePtr->last);
            pthread_cond_init(&statePtr->initialized, NULL);
            ++threadIndex;
            createThread(statePtr, testFun);
        } else {
            while (!State4::queue) {
                if (debugThreads) SkDebugf("%s checkQueue\n", __FUNCTION__);
                pthread_cond_wait(&State4::checkQueue, &State4::addQueue);
            }
            statePtr = State4::queue;
            testsRun += statePtr->testsRun;
            statePtr->testsRun = 0;
            statePtr->a = a;
            statePtr->b = b;
            statePtr->c = c;
            statePtr->d = d;
            statePtr->done = false;
            State4::queue = NULL;
            for (int index = 0; index < maxThreads; ++index) {
                if (threadState[index].done) {
                    State4::queue = &threadState[index];
                }
            }
            if (debugThreads) SkDebugf("%s %d init done=%d last=%d queued=%d\n", __FUNCTION__,
                    statePtr->index, statePtr->done, statePtr->last,
                    State4::queue ? State4::queue->index : -1);
            pthread_cond_signal(&statePtr->initialized);
        }
        pthread_mutex_unlock(&State4::addQueue);
    } else {
        statePtr = &threadState[0];
        testsRun += statePtr->testsRun;
        statePtr->testsRun = 0;
        statePtr->a = a;
        statePtr->b = b;
        statePtr->c = c;
        statePtr->d = d;
        statePtr->done = false;
        statePtr->index = threadIndex;
        statePtr->last = false;
        (*testFun)(statePtr);
    }
    return testsRun;
}

void initializeTests(const char* test, size_t testNameSize) {
    testName = test;
    if (!gRunTestsInOneThread) {
        int threads = -1;
        size_t size = sizeof(threads);
        sysctlbyname("hw.logicalcpu_max", &threads, &size, NULL, 0);
        if (threads > 0) {
            maxThreads = threads;
        } else {
            maxThreads = 8;
        }
    }
    SkFILEStream inFile("../../experimental/Intersection/op.htm");
    if (inFile.isValid()) {
        SkTDArray<char> inData;
        inData.setCount(inFile.getLength());
        size_t inLen = inData.count();
        inFile.read(inData.begin(), inLen);
        inFile.setPath(NULL);
        char* insert = strstr(inData.begin(), marker);
        if (insert) {
            insert += sizeof(marker) - 1;
            const char* numLoc = insert + 4 /* indent spaces */ + testNameSize - 1;
            testNumber = atoi(numLoc) + 1;
        }
    }
    const char* filename = preferredFilename;
    SkFILEWStream preferredTest(filename);
    if (!preferredTest.isValid()) {
        filename = backupFilename;
        SkFILEWStream backupTest(filename);
        SkASSERT(backupTest.isValid());
    }
    for (int index = 0; index < maxThreads; ++index) {
        State4* statePtr = &threadState[index];
        strcpy(statePtr->filename, filename);
        size_t len = strlen(filename);
        SkASSERT(statePtr->filename[len - 6] == 'X');
        SkASSERT(statePtr->filename[len - 5] == 'X');
        statePtr->filename[len - 6] = '0' + index / 10;
        statePtr->filename[len - 5] = '0' + index % 10;
    }
    threadIndex = 0;
}

void outputProgress(const State4& state, const char* pathStr, SkPath::FillType pathFillType) {
    if (gRunTestsInOneThread && gShowOutputProgress) {
        if (pathFillType == SkPath::kEvenOdd_FillType) {
            SkDebugf("    path.setFillType(SkPath::kEvenOdd_FillType);\n", pathStr);
        }
        SkDebugf("%s\n", pathStr);
    }
    const char testFunction[] = "testSimplifyx(path);";
    const char* pathPrefix = NULL;
    const char* nameSuffix = NULL;
    if (pathFillType == SkPath::kEvenOdd_FillType) {
        pathPrefix = "    path.setFillType(SkPath::kEvenOdd_FillType);\n";
        nameSuffix = "x";
    }
    if (gUsePhysicalFiles) {
        SkFILEWStream outFile(state.filename);
        if (!outFile.isValid()) {
            SkASSERT(0);
            return;
        }
        outputToStream(state, pathStr, pathPrefix, nameSuffix, testFunction, outFile);
        return;
    }
    SkFILEWStream outRam(state.filename);
    outputToStream(state, pathStr, pathPrefix, nameSuffix, testFunction, outRam);
}

void outputProgress(const State4& state, const char* pathStr, ShapeOp op) {
    SkString testFunc("testShapeOp(path, pathB, ");
    testFunc += opStrs[op];
    testFunc += ");";
    const char* testFunction = testFunc.c_str();
    if (gRunTestsInOneThread && gShowOutputProgress) {
        SkDebugf("%s\n", pathStr);
        SkDebugf("    %s\n", testFunction);
    }
    const char* nameSuffix = opSuffixes[op];
    if (gUsePhysicalFiles) {
        SkFILEWStream outFile(state.filename);
        if (!outFile.isValid()) {
            SkASSERT(0);
            return;
        }
        outputToStream(state, pathStr, NULL, nameSuffix, testFunction, outFile);
        return;
    }
    SkFILEWStream outRam(state.filename);
    outputToStream(state, pathStr, NULL, nameSuffix, testFunction, outRam);
}

static void writeTestName(const char* nameSuffix, SkWStream& outFile) {
    outFile.writeText(testName);
    outFile.writeDecAsText(testNumber);
    if (nameSuffix) {
        outFile.writeText(nameSuffix);
    }
}

void outputToStream(const State4& state, const char* pathStr, const char* pathPrefix,
        const char* nameSuffix,
        const char* testFunction, SkWStream& outFile) {
    outFile.writeText("<div id=\"");
    writeTestName(nameSuffix, outFile);
    outFile.writeText("\">\n");
    if (pathPrefix) {
        outFile.writeText(pathPrefix);
    }
    outFile.writeText(pathStr);
    outFile.writeText("</div>\n\n");

    outFile.writeText(marker);
    outFile.writeText("    ");
    writeTestName(nameSuffix, outFile);
    outFile.writeText(",\n\n\n");

    outFile.writeText("static void ");
    writeTestName(nameSuffix, outFile);
    outFile.writeText("() {\n    SkPath path, pathB;\n");
    if (pathPrefix) {
        outFile.writeText(pathPrefix);
    }
    outFile.writeText(pathStr);
    outFile.writeText("    ");
    outFile.writeText(testFunction);
    outFile.writeText("\n}\n\n");
    outFile.writeText("static void (*firstTest)() = ");
    writeTestName(nameSuffix, outFile);
    outFile.writeText(";\n\n");

    outFile.writeText("static struct {\n");
    outFile.writeText("    void (*fun)();\n");
    outFile.writeText("    const char* str;\n");
    outFile.writeText("} tests[] = {\n");
    outFile.writeText("    TEST(");
    writeTestName(pathPrefix, outFile);
    outFile.writeText("),\n");
    outFile.flush();
}

bool runNextTestSet(State4& state) {
    if (gRunTestsInOneThread) {
        return false;
    }
    pthread_mutex_lock(&State4::addQueue);
    state.done = true;
    State4::queue = &state;
    if (debugThreads) SkDebugf("%s %d checkQueue done=%d last=%d\n", __FUNCTION__, state.index,
        state.done, state.last);
    pthread_cond_signal(&State4::checkQueue);
    while (state.done && !state.last) {
        if (debugThreads) SkDebugf("%s %d done=%d last=%d\n", __FUNCTION__, state.index, state.done, state.last);
        pthread_cond_wait(&state.initialized, &State4::addQueue);
    }
    pthread_mutex_unlock(&State4::addQueue);
    return !state.last;
}

int waitForCompletion() {
    int testsRun = 0;
    if (!gRunTestsInOneThread) {
        pthread_mutex_lock(&State4::addQueue);
        int runningThreads = maxThreads;
        int index;
        while (runningThreads > 0) {
            while (!State4::queue) {
                if (debugThreads) SkDebugf("%s checkQueue\n", __FUNCTION__);
                pthread_cond_wait(&State4::checkQueue, &State4::addQueue);
            }
            while (State4::queue) {
                --runningThreads;
                SkDebugf("•");
                State4::queue->last = true;
                State4* next = NULL;
                for (index = 0; index < maxThreads; ++index) {
                    State4& test = threadState[index];
                    if (test.done && !test.last) {
                        next = &test;
                    }
                }
                if (debugThreads) SkDebugf("%s %d next=%d deQueue\n", __FUNCTION__,
                    State4::queue->index, next ? next->index : -1);
                pthread_cond_signal(&State4::queue->initialized);
                State4::queue = next;
            }
        }
        pthread_mutex_unlock(&State4::addQueue);
        for (index = 0; index < maxThreads; ++index) {
            pthread_join(threadState[index].threadID, NULL);
            testsRun += threadState[index].testsRun;
        }
        SkDebugf("\n");
    }
#ifdef SK_DEBUG
    gDebugMaxWindSum = SK_MaxS32;
    gDebugMaxWindValue = SK_MaxS32;
#endif
    return testsRun;
}

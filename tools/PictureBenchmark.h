/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef PictureBenchmark_DEFINED
#define PictureBenchmark_DEFINED

#include "SkTypes.h"
#include "PictureRenderer.h"

class BenchTimer;
class SkBenchLogger;
class SkPicture;
class SkString;

namespace sk_tools {

class PictureBenchmark {
public:
    PictureBenchmark();

    ~PictureBenchmark();

    /**
     * Draw the provided SkPicture fRepeats times while collecting timing data, and log the output
     * via fLogger.
     */
    void run(SkPicture* pict);

    void setRepeats(int repeats) {
        fRepeats = repeats;
    }

    /**
     * If true, tells run to log separate timing data for each individual tile. Each tile will be
     * drawn fRepeats times. Requires the PictureRenderer set by setRenderer to be a
     * TiledPictureRenderer.
     */
    void setTimeIndividualTiles(bool indiv) { fTimeIndividualTiles = true; }

    bool timeIndividualTiles() { return fTimeIndividualTiles; }

    PictureRenderer* setRenderer(PictureRenderer*);

    void setDeviceType(PictureRenderer::SkDeviceTypes deviceType) {
        if (fRenderer != NULL) {
            fRenderer->setDeviceType(deviceType);
        }
    }

    void setLogPerIter(bool log) { fLogPerIter = log; }

    void setPrintMin(bool min) { fPrintMin = min; }

    void setTimersToShow(bool wall, bool truncatedWall, bool cpu, bool truncatedCpu, bool gpu) {
        fShowWallTime = wall;
        fShowTruncatedWallTime = truncatedWall;
        fShowCpuTime = cpu;
        fShowTruncatedCpuTime = truncatedCpu;
        fShowGpuTime = gpu;
    }

    void setLogger(SkBenchLogger* logger) { fLogger = logger; }

private:
    int              fRepeats;
    SkBenchLogger*   fLogger;
    PictureRenderer* fRenderer;
    bool             fLogPerIter;
    bool             fPrintMin;
    bool             fShowWallTime;
    bool             fShowTruncatedWallTime;
    bool             fShowCpuTime;
    bool             fShowTruncatedCpuTime;
    bool             fShowGpuTime;
    bool             fTimeIndividualTiles;

    void logProgress(const char msg[]);

    BenchTimer* setupTimer();
};

}

#endif  // PictureBenchmark_DEFINED

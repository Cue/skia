/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "DataTypes.h"
#include "Intersections.h"

void Intersections::addCoincident(double s1, double e1, double s2, double e2) {
    assert((fCoincidentUsed & 1) != 1);
    for (int index = 0; index < fCoincidentUsed; index += 2) {
        double cs1 = fCoincidentT[fSwap][index];
        double ce1 = fCoincidentT[fSwap][index + 1];
        bool s1in = approximately_between(cs1, s1, ce1);
        bool e1in = approximately_between(cs1, e1, ce1);
        double cs2 = fCoincidentT[fSwap ^ 1][index];
        double ce2 = fCoincidentT[fSwap ^ 1][index + 1];
        bool s2in = approximately_between(cs2, s2, ce2);
        bool e2in = approximately_between(cs2, e2, ce2);
        if ((s1in | e1in) & (s2in | e2in)) {
            double lesser1 = std::min(cs1, ce1);
            index += cs1 > ce1;
            if (s1in < lesser1) {
                fCoincidentT[fSwap][index] = s1in;
            } else if (e1in < lesser1) {
                fCoincidentT[fSwap][index] = e1in;
            }
            index ^= 1;
            double greater1 = fCoincidentT[fSwap][index];
            if (s1in > greater1) {
                fCoincidentT[fSwap][index] = s1in;
            } else if (e1in > greater1) {
                fCoincidentT[fSwap][index] = e1in;
            }
            index &= ~1;
            double lesser2 = std::min(cs2, ce2);
            index += cs2 > ce2;
            if (s2in < lesser2) {
                fCoincidentT[fSwap ^ 1][index] = s2in;
            } else if (e2in < lesser2) {
                fCoincidentT[fSwap ^ 1][index] = e2in;
            }
            index ^= 1;
            double greater2 = fCoincidentT[fSwap ^ 1][index];
            if (s2in > greater2) {
                fCoincidentT[fSwap ^ 1][index] = s2in;
            } else if (e2in > greater2) {
                fCoincidentT[fSwap ^ 1][index] = e2in;
            }
            return;
        }
    }
    assert(fCoincidentUsed < 9);
    fCoincidentT[fSwap][fCoincidentUsed] = s1;
    fCoincidentT[fSwap ^ 1][fCoincidentUsed] = s2;
    ++fCoincidentUsed;
    fCoincidentT[fSwap][fCoincidentUsed] = e1;
    fCoincidentT[fSwap ^ 1][fCoincidentUsed] = e2;
    ++fCoincidentUsed;
}

void Intersections::cleanUp() {
    assert(fCoincidentUsed);
    assert(fUsed);
    // find any entries in fT that could be part of the coincident range

}

// FIXME: this doesn't respect swap, but add coincident does -- seems inconsistent
void Intersections::insert(double one, double two) {
    assert(fUsed <= 1 || fT[0][0] < fT[0][1]);
    int index;
    for (index = 0; index < fUsed; ++index) {
        if (approximately_equal(fT[0][index], one)
                && approximately_equal(fT[1][index], two)) {
            return;
        }
        if (fT[0][index] > one) {
            break;
        }
    }
    assert(fUsed < 9);
    int remaining = fUsed - index;
    if (remaining > 0) {
        memmove(&fT[0][index + 1], &fT[0][index], sizeof(fT[0][0]) * remaining);
        memmove(&fT[1][index + 1], &fT[1][index], sizeof(fT[1][0]) * remaining);
    }
    fT[0][index] = one;
    fT[1][index] = two;
    ++fUsed;
}

// FIXME: all callers should be moved to regular insert. Failures are likely
// if two separate callers differ on whether ts are equal or not
void Intersections::insertOne(double t, int side) {
    int used = side ? fUsed2 : fUsed;
    assert(used <= 1 || fT[side][0] < fT[side][1]);
    int index;
    for (index = 0; index < used; ++index) {
        if (approximately_equal(fT[side][index], t)) {
            return;
        }
        if (fT[side][index] > t) {
            break;
        }
    }
    assert(used < 9);
    int remaining = used - index;
    if (remaining > 0) {
        memmove(&fT[side][index + 1], &fT[side][index], sizeof(fT[side][0]) * remaining);
    }
    fT[side][index] = t;
    side ? ++fUsed2 : ++fUsed;
}

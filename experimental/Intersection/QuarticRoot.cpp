// from http://tog.acm.org/resources/GraphicsGems/gems/Roots3And4.c
/*
 *  Roots3And4.c
 *
 *  Utility functions to find cubic and quartic roots,
 *  coefficients are passed like this:
 *
 *      c[0] + c[1]*x + c[2]*x^2 + c[3]*x^3 + c[4]*x^4 = 0
 *
 *  The functions return the number of non-complex roots and
 *  put the values into the s array.
 *
 *  Author:         Jochen Schwarze (schwarze@isa.de)
 *
 *  Jan 26, 1990    Version for Graphics Gems
 *  Oct 11, 1990    Fixed sign problem for negative q's in SolveQuartic
 *                  (reported by Mark Podlipec),
 *                  Old-style function definitions,
 *                  IsZero() as a macro
 *  Nov 23, 1990    Some systems do not declare acos() and cbrt() in
 *                  <math.h>, though the functions exist in the library.
 *                  If large coefficients are used, EQN_EPS should be
 *                  reduced considerably (e.g. to 1E-30), results will be
 *                  correct but multiple roots might be reported more
 *                  than once.
 */

#include    <math.h>
#include "CubicUtilities.h"
#include "QuarticRoot.h"

const double PI = 4 * atan(1);

// unlike quadraticRoots in QuadraticUtilities.cpp, this does not discard
// real roots <= 0 or >= 1
static int quadraticRootsX(const double A, const double B, const double C,
        double s[2]) {
    if (approximately_zero(A)) {
        if (approximately_zero(B)) {
            s[0] = 0;
            return C == 0;
        }
        s[0] = -C / B;
        return 1;
    }
    /* normal form: x^2 + px + q = 0 */
    const double p = B / (2 * A);
    const double q = C / A;
    double D = p * p - q;
    if (D < 0) {
        if (approximately_positive_squared(D)) {
            D = 0;
        } else {
            return 0;
        }
    }
    double sqrt_D = sqrt(D);
    if (approximately_less_than_zero(sqrt_D)) {
        s[0] = -p;
        return 1;
    }
    s[0] = sqrt_D - p;
    s[1] = -sqrt_D - p;
    return 2;
}

#define USE_GEMS 0
#if USE_GEMS
// unlike cubicRoots in CubicUtilities.cpp, this does not discard
// real roots <= 0 or >= 1
static int cubicRootsX(const double A, const double B, const double C,
        const double D, double s[3]) {
    int num;
    /* normal form: x^3 + Ax^2 + Bx + C = 0 */
    const double invA = 1 / A;
    const double a = B * invA;
    const double b = C * invA;
    const double c = D * invA;
    /*  substitute x = y - a/3 to eliminate quadric term:
    x^3 +px + q = 0 */
    const double a2 = a * a;
    const double Q = (-a2 + b * 3) / 9;
    const double R = (2 * a2 * a - 9 * a * b + 27 * c) / 54;
    /* use Cardano's formula */
    const double Q3 = Q * Q * Q;
    const double R2plusQ3 = R * R + Q3;
    if (approximately_zero(R2plusQ3)) {
        if (approximately_zero(R)) {/* one triple solution */
            s[0] = 0;
            num = 1;
        } else { /* one single and one double solution */

            double u = cube_root(-R);
            s[0] = 2 * u;
            s[1] = -u;
            num = 2;
        }
    }
    else if (R2plusQ3 < 0) { /* Casus irreducibilis: three real solutions */
        const double theta = acos(-R / sqrt(-Q3)) / 3;
        const double _2RootQ = 2 * sqrt(-Q);
        s[0] = _2RootQ * cos(theta);
        s[1] = -_2RootQ * cos(theta + PI / 3);
        s[2] = -_2RootQ * cos(theta - PI / 3);
        num = 3;
    } else { /* one real solution */
        const double sqrt_D = sqrt(R2plusQ3);
        const double u = cube_root(sqrt_D - R);
        const double v = -cube_root(sqrt_D + R);
        s[0] = u + v;
        num = 1;
    }
    /* resubstitute */
    const double sub = a / 3;
    for (int i = 0; i < num; ++i) {
        s[i] -= sub;
    }
    return num;
}
#else

static int cubicRootsX(double A, double B, double C, double D, double s[3]) {
    if (approximately_zero(A)) {  // we're just a quadratic
        return quadraticRootsX(B, C, D, s);
    }
    if (approximately_zero(D)) { // 0 is one root
        int num = quadraticRootsX(A, B, C, s);
        for (int i = 0; i < num; ++i) {
            if (approximately_zero(s[i])) {
                return num;
            }
        }
        s[num++] = 0;
        return num;
    }
    if (approximately_zero(A + B + C + D)) { // 1 is one root
        int num = quadraticRootsX(A, A + B, -D, s);
        for (int i = 0; i < num; ++i) {
            if (approximately_equal(s[i], 1)) {
                return num;
            }
        }
        s[num++] = 1;
        return num;
    }
    double a, b, c;
    {
        double invA = 1 / A;
        a = B * invA;
        b = C * invA;
        c = D * invA;
    }
    double a2 = a * a;
    double Q = (a2 - b * 3) / 9;
    double R = (2 * a2 * a - 9 * a * b + 27 * c) / 54;
    double Q3 = Q * Q * Q;
    double R2MinusQ3 = R * R - Q3;
    double adiv3 = a / 3;
    double r;
    double* roots = s;

    if (approximately_zero_squared(R2MinusQ3)) {
        if (approximately_zero(R)) {/* one triple solution */
            *roots++ = -adiv3;
        } else { /* one single and one double solution */

            double u = cube_root(-R);
            *roots++ = 2 * u - adiv3;
            *roots++ = -u - adiv3;
        }
    }
    else if (R2MinusQ3 < 0)   // we have 3 real roots
    {
        double theta = acos(R / sqrt(Q3));
        double neg2RootQ = -2 * sqrt(Q);

        r = neg2RootQ * cos(theta / 3) - adiv3;
        *roots++ = r;

        r = neg2RootQ * cos((theta + 2 * PI) / 3) - adiv3;
        *roots++ = r;

        r = neg2RootQ * cos((theta - 2 * PI) / 3) - adiv3;
        *roots++ = r;
    }
    else                // we have 1 real root
    {
        double A = fabs(R) + sqrt(R2MinusQ3);
        A = cube_root(A);
        if (R > 0) {
            A = -A;
        }
        if (A != 0) {
            A += Q / A;
        }
        r = A - adiv3;
        *roots++ = r;
    }
    return (int)(roots - s);
}
#endif

int quarticRoots(const double A, const double B, const double C, const double D,
        const double E, double s[4]) {
    if (approximately_zero(A)) {
        if (approximately_zero(B)) {
            return quadraticRootsX(C, D, E, s);
        }
        return cubicRootsX(B, C, D, E, s);
    }
    int num;
    int i;
    if (approximately_zero(E)) { // 0 is one root
        num = cubicRootsX(A, B, C, D, s);
        for (i = 0; i < num; ++i) {
            if (approximately_zero(s[i])) {
                return num;
            }
        }
        s[num++] = 0;
        return num;
    }
    if (approximately_zero_squared(A + B + C + D + E)) { // 1 is one root
        num = cubicRootsX(A, A + B, -(D + E), -E, s); // note that -C==A+B+D+E
        for (i = 0; i < num; ++i) {
            if (approximately_equal(s[i], 1)) {
                return num;
            }
        }
        s[num++] = 1;
        return num;
    }
    double  u, v;
    /* normal form: x^4 + Ax^3 + Bx^2 + Cx + D = 0 */
    const double invA = 1 / A;
    const double a = B * invA;
    const double b = C * invA;
    const double c = D * invA;
    const double d = E * invA;
    /*  substitute x = y - a/4 to eliminate cubic term:
    x^4 + px^2 + qx + r = 0 */
    const double a2 = a * a;
    const double p = -3 * a2 / 8 + b;
    const double q = a2 * a / 8 - a * b / 2 + c;
    const double r = -3 * a2 * a2 / 256 + a2 * b / 16 - a * c / 4 + d;
    if (approximately_zero(r)) {
    /* no absolute term: y(y^3 + py + q) = 0 */
        num = cubicRootsX(1, 0, p, q, s);
        s[num++] = 0;
    } else {
        /* solve the resolvent cubic ... */
        (void) cubicRootsX(1, -p / 2, -r, r * p / 2 - q * q / 8, s);
        /* ... and take the one real solution ... */
        const double z = s[0];
        /* ... to build two quadric equations */
        u = z * z - r;
        v = 2 * z - p;
        if (approximately_zero(u)) {
            u = 0;
        } else if (u > 0) {
            u = sqrt(u);
        } else {
            return 0;
        }
        if (approximately_zero(v)) {
            v = 0;
        } else if (v > 0) {
            v = sqrt(v);
        } else {
            return 0;
        }
        num = quadraticRootsX(1, q < 0 ? -v : v, z - u, s);
        num += quadraticRootsX(1, q < 0 ? v : -v, z + u, s + num);
    }
    // eliminate duplicates
    for (i = 0; i < num - 1; ++i) {
        for (int j = i + 1; j < num; ) {
            if (approximately_equal(s[i], s[j])) {
                if (j < --num) {
                    s[j] = s[num];
                }
            } else {
                ++j;
            }
        }
    }
    /* resubstitute */
    const double sub = a / 4;
    for (i = 0; i < num; ++i) {
        s[i] -= sub;
    }
    return num;
}



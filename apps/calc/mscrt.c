/*
 * The Microsoft C 6 run-time pieces CALC.EXE links (its segment 4), ported from the disassembly
 * of the user's CALC.EXE: the 8087 math library, gcvt with the $I8_OUTPUT conversion behind it,
 * and the integer conversions.
 *
 * Calculator results depend on this code bit for bit, so it is ported routine by routine:
 *  - the arithmetic is the x87's. `ext` is the 80-bit x87 format and every operation runs on the
 *    host FPU with 64-bit precision and round-to-nearest, the control word the 16-bit library
 *    loads (0x13xx); the transcendental instructions (fsin, fptan, fpatan, fyl2x, f2xm1 ...) are
 *    the host's own, as on a 387 or later (the library uses fsin/fcos only on those, seg4:0D3B).
 *  - the library's checks, special cases and error values (types 1-6 for matherr, the default
 *    results 0 / +-HUGE_VAL) follow the per-function tables at ds:04FE-06F1 and the dispatcher at
 *    seg4:1D42 / 15A6.
 *  - doubles are formatted the library's way: the value is scaled into [0.1, 1) with its table of
 *    powers of ten (the correctly rounded 80-bit values, rebuilt here with strtold) and 16 digits
 *    are taken from the 64-bit mantissa plus a bias of 0x39A units (seg4:1E8F), then rounded on
 *    the digit string (seg4:1C2E).
 * Non-x86 hosts fall back to libm's long double functions: UNTESTED, and their last digits may
 * differ from 3.1's.
 */
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mscrt.h"

static int (*matherr_fn)(int);
static void (*sigfpe_fn)(int);

/* ------------------------------------------------------------------ x87 primitives */
#if defined(__x86_64__) || defined(__i386__)
static inline ext x_sin(ext x) { __asm__("fsin" : "+t"(x)); return x; }
static inline ext x_cos(ext x) { __asm__("fcos" : "+t"(x)); return x; }
static inline ext x_sqrt(ext x) { __asm__("fsqrt" : "+t"(x)); return x; }
static inline ext x_f2xm1(ext x) { __asm__("f2xm1" : "+t"(x)); return x; }
static inline ext x_fscale(ext x, ext n) { __asm__("fscale" : "+t"(x) : "u"(n)); return x; }
/* y * log2(x) */
static inline ext x_fyl2x(ext y, ext x) { ext r; __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r; }
/* atan(y / x) */
static inline ext x_fpatan(ext y, ext x) { ext r; __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r; }
/* tan(x); a 387 pushes 1.0 above it */
static inline ext x_fptan(ext x, ext *one) { ext t, o; __asm__("fptan" : "=t"(o), "=u"(t) : "0"(x)); *one = o; return t; }
static inline ext x_ldln2(void) { ext r; __asm__("fldln2" : "=t"(r)); return r; }
static inline ext x_ldlg2(void) { ext r; __asm__("fldlg2" : "=t"(r)); return r; }
static inline ext x_ldl2e(void) { ext r; __asm__("fldl2e" : "=t"(r)); return r; }
static inline ext x_ldpi(void) { ext r; __asm__("fldpi" : "=t"(r)); return r; }
/* one FPREM: the truncating partial remainder, the status word holds the quotient's low bits */
static inline ext x_fprem(ext x, ext y, unsigned *sw)
{
    unsigned short s;
    __asm__("fprem\n\tfnstsw %1" : "+t"(x), "=a"(s) : "u"(y));
    *sw = s;
    return x;
}
/* FRNDINT under rounding control rc (0 nearest, 1 down, 3 chop) */
static ext x_rndint(ext x, unsigned rc)
{
    unsigned short cw, ncw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    ncw = (unsigned short)((cw & ~0x0C00u) | (rc << 10));
    __asm__ volatile("fldcw %2\n\tfrndint\n\tfldcw %3" : "=t"(x) : "0"(x), "m"(ncw), "m"(cw));
    return x;
}
static void x_setcw(void)
{
    /* 64-bit precision, round to nearest; exceptions masked - the library's error checks are
     * done in software below */
    unsigned short cw = 0x037F;
    __asm__ volatile("fldcw %0" : : "m"(cw));
}
#else
#include <fenv.h>
static inline ext x_sin(ext x) { return sinl(x); }
static inline ext x_cos(ext x) { return cosl(x); }
static inline ext x_sqrt(ext x) { return sqrtl(x); }
static inline ext x_f2xm1(ext x) { return expm1l(x * 0.693147180559945309417232121458176568L); }
static inline ext x_fscale(ext x, ext n) { return scalbnl(x, (int)truncl(n)); }
static inline ext x_fyl2x(ext y, ext x) { return y * log2l(x); }
static inline ext x_fpatan(ext y, ext x) { return atan2l(y, x); }
static inline ext x_fptan(ext x, ext *one) { *one = 1.0L; return tanl(x); }
static inline ext x_ldln2(void) { return 0.693147180559945309417232121458176568L; }
static inline ext x_ldlg2(void) { return 0.301029995663981195213738894724493027L; }
static inline ext x_ldl2e(void) { return 1.442695040888963407359924681001892137L; }
static inline ext x_ldpi(void) { return 3.141592653589793238462643383279502884L; }
static inline ext x_fprem(ext x, ext y, unsigned *sw)
{
    ext r = fmodl(x, y);
    unsigned long long q = (unsigned long long)fabsl(truncl((x - r) / y));
    *sw = (unsigned)(((q & 1) << 9) | ((q & 2) << 13) | ((q & 4) >> 2) << 8);
    return r;
}
static ext x_rndint(ext x, unsigned rc)
{
    return rc == 0 ? nearbyintl(x) : rc == 1 ? floorl(x) : truncl(x);
}
static void x_setcw(void) { fesetround(FE_TONEAREST); }
#endif

/* an 80-bit value from its mantissa and sign/exponent word */
static ext mkext(uint64_t man, unsigned se)
{
    ext v = ldexpl((ext)man, (int)(se & 0x7FFF) - 16383 - 63);
    return (se & 0x8000) ? -v : v;
}
static unsigned ext_se(ext x)
{
#if LDBL_MANT_DIG == 64
    uint16_t se;
    memcpy(&se, (char *)&x + 8, 2);
    return se;
#else
    int e;
    frexpl(x, &e);
    return (unsigned)((signbit(x) ? 0x8000 : 0) | ((e - 1 + 16383) & 0x7FFF));
#endif
}
static uint64_t ext_man(ext x)
{
#if LDBL_MANT_DIG == 64
    uint64_t m;
    memcpy(&m, &x, 8);
    return m;
#else
    int e;
    ext f = frexpl(fabsl(x), &e);
    return (uint64_t)ldexpl(f, 64);
#endif
}

/* ------------------------------------------------------------------ constants */
static ext PI4, PI2;      /* pi/4 (ds:05C0), pi/2 (ds:0762), as the FPU's fldpi */
static ext P10[2][3][8];  /* [negative][group][digit]: 10^(+-d*8^g), seg4:240C / 24CA */
static ext ONE_MINUS;     /* 1 - 0x39A*2^-64 (seg4:1E41) */
static ext TENTH;         /* 0.1 (seg4:1E4B) */
static ext TANH_BIG;      /* ds:0520: tanh is +-1 from here on */

void rt_init(int (*matherr)(int), void (*sigfpe)(int))
{
    matherr_fn = matherr;
    sigfpe_fn = sigfpe;
    x_setcw();
    ext pi = x_ldpi();
    PI4 = pi / 4;
    PI2 = pi / 2;
    for (int neg = 0; neg < 2; neg++)
        for (int g = 0; g < 3; g++)
            for (int d = 1; d < 8; d++) {
                char s[16];
                int k = d << (3 * g);
                snprintf(s, sizeof s, "1e%s%d", neg ? "-" : "", k);
                P10[neg][g][d] = strtold(s, NULL);
            }
    ONE_MINUS = mkext(0xFFFFFFFFFFFFFC66ull, 0x3FFE);
    TENTH = mkext(0xCCCCCCCCCCCCCCCDull, 0x3FFB);
    TANH_BIG = mkext(0xB1716685B9D7A7DCull, 0x400D);
}

/* ------------------------------------------------------------------ the function dispatcher */
/* fxam through the class table at ds:076D: byte offsets into each function's jump table */
enum { CL_NORMAL = 0, CL_ZERO = 2, CL_NAN = 4, CL_INF = 6 };
static int fclass(ext x)
{
    switch (fpclassify(x)) {
    case FP_NORMAL: return CL_NORMAL;
    case FP_ZERO:
    case FP_SUBNORMAL: return CL_ZERO;
    case FP_INFINITE: return CL_INF;
    default: return CL_NAN;
    }
}

/* the per-function table: default results for error types 1-6 (offsets into ds:071A:
 * 0 keep the value, 2 zero, 4 +-HUGE_VAL by the value's sign, 6 -HUGE_VAL) */
typedef struct { unsigned char ret[7]; } MFn;
static const MFn F_SQRT = {{0, 2, 0, 4, 0, 0, 0}}, F_POW = {{0, 2, 0, 4, 0, 0, 0}},
                 F_LOG = {{0, 6, 6, 4, 0, 0, 0}}, F_SIN = {{0, 0, 0, 0, 0, 2, 0}},
                 F_TAN = {{0, 0, 0, 4, 0, 2, 0}}, F_ASIN = {{0, 2, 0, 0, 0, 0, 0}},
                 F_SINH = {{0, 0, 0, 4, 0, 0, 0}}, F_TANH = {{0, 0, 0, 0, 0, 0, 0}};

static int mtype; /* [bp-11h]: the error type a routine reports; -1 marks the exp core */

/* seg4:1E1B: replace the value by the FPU's indefinite NaN, a DOMAIN error unless one is set */
static ext dom_nan(void)
{
    if (mtype <= 0) mtype = M_DOMAIN;
    return -mkext(0xC000000000000000ull, 0x7FFF);
}

/* seg4:15BB-16D9: after the routine. The result is stored to a double (fst [__fac]); an overflow
 * there is an OVERFLOW error. Errors call matherr with the table's default result, which is what
 * the function then returns (rounded to a double, ds:0712). */
static ext finish(const MFn *f, ext r)
{
    int t = mtype;
    if (t > 0 && t != M_PLOSS) goto error;
    if (isfinite(r) && isinf((double)r)) { t = M_OVERFLOW; goto error; }
    if (t == M_PLOSS) goto error;
    return r;
error: {
        ext rv;
        switch (f->ret[t]) {
        case 0: rv = r; break;
        case 2: rv = 0.0L; break;
        case 4: rv = (r < 0 || isnan(r)) ? -DBL_MAX : DBL_MAX; break; /* ftst: C0 also when unordered */
        default: rv = -DBL_MAX; break;
        }
        double d = (double)rv;
        if (matherr_fn) matherr_fn(t);
        return d;
    }
}

/* ------------------------------------------------------------------ exponential core */
typedef struct { ext u, n; int fneg, nzero, done; } E2;
/* seg4:0CEA: 2^t = 2^n * 2^f with n = round(t), |f| <= 1/2; u = 2^|f| - 1 (f2xm1). From
 * |t| >= 16384 on the routine ends here (0CB1): 0 for t < 0, +inf with OVERFLOW otherwise. */
static E2 exp2core(ext t)
{
    E2 e = {0, 0, 0, 0, 0};
    mtype = -1;
    if (!(fabsl(t) < 16384.0L)) {
        e.done = 1;
        if (t < 0 || isnan(t)) e.u = 0.0L;
        else { e.u = INFINITY; mtype = M_OVERFLOW; }
        return e;
    }
    e.n = x_rndint(t, 0);
    e.nzero = e.n == 0;
    ext f = t - e.n;
    e.fneg = f < 0;
    e.u = x_f2xm1(fabsl(f));
    return e;
}

/* seg4:0BCA: 2^t from the core, negated when asked (odd powers of negative numbers) */
static ext expcore(ext t, int negate)
{
    E2 e = exp2core(t);
    if (e.done) return e.u;
    ext r = e.u + 1.0L;
    if (e.fneg) r = 1.0L / r;
    if (!e.nzero) r = x_fscale(r, e.n);
    if (negate) r = -r;
    return r;
}

/* ------------------------------------------------------------------ the routines */
/* sqrt: jump table 0BA0 / 1DF9 / 1E29 / 0BA8 */
static ext m_sqrt(ext x)
{
    switch (fclass(x)) {
    case CL_NORMAL: return signbit(x) ? dom_nan() : x_sqrt(x);
    case CL_ZERO: return 0.0L;
    case CL_NAN: mtype = M_DOMAIN; return x;
    default:
        if (signbit(x)) return dom_nan();
        mtype = M_OVERFLOW;
        return x;
    }
}

/* log, log10: 0C8D / 0C88 (k = ln 2 or log10 2), 0C4E, 1E29, 0CE3 */
static ext m_log(ext x, ext k)
{
    switch (fclass(x)) {
    case CL_NORMAL: return signbit(x) ? dom_nan() : x_fyl2x(k, x);
    case CL_ZERO: mtype = M_SING; return dom_nan();
    case CL_NAN: mtype = M_DOMAIN; return x;
    default:
        if (signbit(x)) return dom_nan();
        mtype = M_OVERFLOW;
        return x;
    }
}

/* the exponent check for negative bases (0BF1): y must be an integer below 2^64; odd sets the sign */
static int int_exponent(ext y, int *odd)
{
    int e = (int)(ext_se(y) & 0x7FFF) - 0x3FFF;
    if (e < 0 || e >= 0x40) return 0;
    uint64_t m = ext_man(y);
    unsigned char b[8];
    for (int i = 0; i < 8; i++) b[i] = (unsigned char)(m >> (8 * i));
    int nb = 0x3F - e, k = nb & 7, bi = nb >> 3;
    unsigned char unit = (unsigned char)(1u << k), lower = (unsigned char)(unit - 1);
    unsigned char ch = unit & b[bi], cl = lower & b[bi];
    for (int i = bi - 1; i >= 0; i--) cl |= b[i];
    if (cl) return 0;
    *odd = ch != 0;
    return 1;
}

/* pow: the two-argument table at ds:0548 (by the classes of x and y) */
static ext m_pow(ext x, ext y)
{
    int cx = fclass(x), cy = fclass(y);
    int sx = signbit(x) != 0, sy = signbit(y) != 0;
    if (cx == CL_NAN) {
        mtype = M_DOMAIN;
        return cy == CL_NAN ? x + y : x; /* 1E0A / 1DF1 */
    }
    if (cy == CL_NAN) { mtype = M_DOMAIN; return y; } /* 1DE7 */
    if (cx == CL_NORMAL && cy == CL_NORMAL) {         /* 0BB6 */
        if (sx) {
            int odd;
            if (!int_exponent(y, &odd)) return dom_nan();
            return expcore(x_fyl2x(y, fabsl(x)), odd);
        }
        return expcore(x_fyl2x(y, x), 0);
    }
    if (cx == CL_NORMAL && cy == CL_ZERO) return 1.0L; /* 0C9B -> 1E00 */
    if (cx == CL_ZERO && cy == CL_NORMAL) return sy ? dom_nan() : 0.0L; /* 0CA1; ds:076C = 1 */
    if (cx == CL_ZERO && cy == CL_ZERO) return dom_nan();              /* 0C44 */
    if (cx == CL_ZERO && cy == CL_INF) return sy ? dom_nan() : 0.0L;  /* 0C9E */
    if (cx == CL_INF && cy == CL_ZERO) return dom_nan();               /* 1E18 */
    /* normal or infinite x with an infinite y, infinite x with a normal y: 0CC3 */
    if (sx) return dom_nan();
    if (sy) return 0.0L;
    mtype = M_OVERFLOW;
    return INFINITY;
}

/* seg4:0E19: from 2^27 on sin and cos lose precision (PLOSS), from 2^31 they give up (TLOSS) */
static int trig_arg_ok(ext x)
{
    unsigned e = ext_se(x) & 0x7FFF;
    if (e < 0x401A) return 1;
    mtype = M_PLOSS;
    return e < 0x401E;
}

/* sin, cos on a 387 or later: 0D54 / 0D3B, 1DF0 / 1E03, 1E29, 0D9E */
static ext m_sincos(ext x, int cosine)
{
    switch (fclass(x)) {
    case CL_NORMAL:
        if (!trig_arg_ok(x)) { mtype = M_TLOSS; return dom_nan(); }
        return cosine ? x_cos(x) : x_sin(x);
    case CL_ZERO: return cosine ? 1.0L : x;
    case CL_NAN: mtype = M_DOMAIN; return x;
    default: mtype = M_TLOSS; return dom_nan();
    }
}

/* tan: 0D78 - reduction by pi/4 with FPREM, FPTAN, the octant fix-up (0DE9-0E18) */
static ext m_tan(ext x)
{
    switch (fclass(x)) {
    case CL_ZERO: return x;
    case CL_NAN: mtype = M_DOMAIN; return x;
    case CL_INF: mtype = M_TLOSS; return dom_nan();
    default: break;
    }
    mtype = -1;
    int neg = signbit(x) != 0;
    ext a = fabsl(x);
    if (!(a <= 134217728.0L)) {
        mtype = M_PLOSS;
        if (!(a <= 2147483648.0L)) { mtype = M_TLOSS; return dom_nan(); }
    }
    unsigned sw;
    ext r = x_fprem(a, PI4, &sw);
    unsigned h = sw >> 8; /* C0 bit 0, C1 bit 1, C3 bit 6 */
    unsigned q = ((h >> 1) & 1) | (((h >> 6) & 1) << 1) | ((h & 1) << 2);
    if (q & 1) r = PI4 - r;
    ext one, t = x_fptan(r, &one);
    ext s0 = one, s1 = t;
    unsigned char al = (unsigned char)q;
    al = (unsigned char)(al - 1);
    if (!(al & 2)) { ext tmp = s0; s0 = s1; s1 = tmp; }
    al = (unsigned char)(al - 1);
    if (!(al & 4)) s0 = -s0;
    al = (unsigned char)(al - 2);
    if (!(al & 4)) s1 = -s1;
    ext res = s1 / s0;
    return neg ? -res : res;
}

/* the arctangent core (0E6B): ST0 = s0, ST1 = s1 (both >= 0) */
static ext atan_core(ext s0, ext s1, int cl, int ch)
{
    int dl = s0 < s1;
    if (dl) { ext t = s0; s0 = s1; s1 = t; }
    ext r = x_fpatan(s1, s0);
    if (dl) r = PI2 - r;
    if (cl) r = x_ldpi() - r;
    if (ch) r = -r;
    return r;
}

/* asin, acos: 0E44 / 0E4B with the root sqrt((1-|x|)(1+|x|)) of 0EA0 */
static ext m_asincos(ext x, int acos_)
{
    switch (fclass(x)) {
    case CL_ZERO: return acos_ ? PI2 : x; /* 0ED4 / 1DF0 */
    case CL_NAN: mtype = M_DOMAIN; return x;
    case CL_INF: return dom_nan();
    default: break;
    }
    int sign = signbit(x) != 0;
    ext a = fabsl(x);
    ext p = (1.0L - a) * (a + 1.0L);
    if (p < 0) return dom_nan();
    ext root = x_sqrt(p);
    if (acos_) return atan_core(a, root, sign, 0);
    return atan_core(root, a, 0, sign);
}

/* atan: 0E53, 1DF0, 1E29, 0F01 */
static ext m_atan(ext x)
{
    switch (fclass(x)) {
    case CL_ZERO: return x;
    case CL_NAN: mtype = M_DOMAIN; return x;
    case CL_INF: return signbit(x) ? -PI2 : PI2;
    default: return atan_core(1.0L, fabsl(x), 0, signbit(x) != 0);
    }
}

/* seg4:0F7A: e^x (big = 1) or, when the power of two is 0, e^x - 1 (big = 0); ok = 0 when the
 * exponent core gave up */
typedef struct { ext v; int big, ok; } EX;
static EX expx(ext x)
{
    EX r = {0, 0, 0};
    E2 e = exp2core(x * x_ldl2e());
    if (e.done) { r.v = e.u; return r; }
    r.ok = 1;
    ext v = e.u;
    if (e.fneg) v = v / (-(1.0L + v)); /* 0FBD: 2^-|f| - 1 */
    if (!e.nzero) { r.big = 1; v = x_fscale(v + 1.0L, e.n); }
    r.v = v;
    return r;
}
/* seg4:0FBD: e^-x - 1 from e^x - 1, or 1/e^x */
static ext recip_part(ext v, int big) { return big ? 1.0L / v : v / (-(1.0L + v)); }
/* seg4:0FA9: e^x + e^-x */
static ext two_cosh(ext v, int big)
{
    ext s = v + recip_part(v, big);
    if (!big) { s = s + 1.0L; s = s + 1.0L; }
    return s;
}

/* sinh 0F0A, cosh 0F4D, tanh 0F19 */
static ext m_sinh(ext x)
{
    switch (fclass(x)) {
    case CL_ZERO: return x;
    case CL_NAN: mtype = M_DOMAIN; return x;
    case CL_INF: mtype = M_OVERFLOW; return signbit(x) ? -INFINITY : INFINITY;
    default: break;
    }
    EX e = expx(x);
    if (!e.ok) { mtype = M_OVERFLOW; return signbit(x) ? -INFINITY : INFINITY; }
    return x_fscale(e.v - recip_part(e.v, e.big), -1.0L);
}
static ext m_cosh(ext x)
{
    switch (fclass(x)) {
    case CL_ZERO: return 1.0L;
    case CL_NAN: mtype = M_DOMAIN; return x;
    case CL_INF: mtype = M_OVERFLOW; return INFINITY;
    default: break;
    }
    EX e = expx(x);
    if (!e.ok) { mtype = M_OVERFLOW; return INFINITY; }
    return x_fscale(two_cosh(e.v, e.big), -1.0L);
}
static ext m_tanh(ext x)
{
    switch (fclass(x)) {
    case CL_ZERO: return x;
    case CL_NAN: mtype = M_DOMAIN; return x;
    case CL_INF: return signbit(x) ? -1.0L : 1.0L;
    default: break;
    }
    if (!(TANH_BIG > fabsl(x))) return signbit(x) ? -1.0L : 1.0L;
    EX e = expx(x);
    if (!e.ok) return signbit(x) ? -1.0L : 1.0L;
    ext d = two_cosh(e.v, e.big);
    ext n = e.v - recip_part(e.v, e.big);
    return n / d;
}

/* ------------------------------------------------------------------ entry points */
#define CALL1(F, expr) do { mtype = 0; ext r_ = (expr); return (double)finish(&(F), r_); } while (0)
double rt_sqrt(double x) { CALL1(F_SQRT, m_sqrt(x)); }
double rt_log(double x) { CALL1(F_LOG, m_log(x, x_ldln2())); }
double rt_log10(double x) { CALL1(F_LOG, m_log(x, x_ldlg2())); }
double rt_pow(double x, double y) { CALL1(F_POW, m_pow(x, y)); }
double rt_sin(double x) { CALL1(F_SIN, m_sincos(x, 0)); }
double rt_cos(double x) { CALL1(F_SIN, m_sincos(x, 1)); }
double rt_tan(double x) { CALL1(F_TAN, m_tan(x)); }
double rt_asin(double x) { CALL1(F_ASIN, m_asincos(x, 0)); }
double rt_acos(double x) { CALL1(F_ASIN, m_asincos(x, 1)); }
double rt_atan(double x) { CALL1(F_ASIN, m_atan(x)); }
double rt_sinh(double x) { CALL1(F_SINH, m_sinh(x)); }
double rt_cosh(double x) { CALL1(F_SINH, m_cosh(x)); }
double rt_tanh(double x) { CALL1(F_TANH, m_tanh(x)); }
/* seg4:1530: the intrinsic leaves the unrounded result on the stack (errors give the double) */
ext rt_cipow(ext x, ext y)
{
    mtype = 0;
    ext r = m_pow(x, y);
    return finish(&F_POW, r);
}

/* seg4:0666 / 069B: FRNDINT rounding down */
double rt_floor(double x) { return (double)x_rndint(x, 1); }
/* seg4:0764: FRNDINT chopping; the fraction is x minus it */
double rt_modf(double x, double *ipart)
{
    ext t = x_rndint(x, 3);
    *ipart = (double)t;
    return (double)((ext)x - t);
}
/* seg4:06C0 / 06FD: FPREM until complete; a zero divisor returns the divisor */
double rt_fmod(double x, double y)
{
    if (y == 0.0) return y;
    ext r = x;
    unsigned sw;
    do r = x_fprem(r, y, &sw); while (sw & 0x0400);
    return (double)r;
}
/* seg4:07B0 */
double rt_fabs(double x) { return fabs(x); }
/* seg4:0726: FISTP qword with chopping, DX:AX = the low half. Out of range the FPU stores the
 * integer indefinite (low half 0) and, the invalid exception being unmasked in MS C's control
 * word, raises SIGFPE (UNTESTED: not reachable through Calculator's checks). */
int32_t rt_ftol(ext x)
{
    ext t = x_rndint(x, 3);
    if (isnan(t) || t >= 9223372036854775808.0L || t < -9223372036854775808.0L) {
        if (sigfpe_fn) sigfpe_fn(FPE_INVALID_);
        return 0;
    }
    return (int32_t)(uint32_t)(uint64_t)(int64_t)t;
}

/* The program's own `fstp qword`: MS C's control word (0x1332) leaves the overflow exception
 * unmasked, so a value too large for a double is not stored and SIGFPE reaches the handler. */
int rt_st64(ext v, double *dst)
{
    double d = (double)v;
    if (isfinite(v) && isinf(d)) {
        if (sigfpe_fn) sigfpe_fn(FPE_OVERFLOW_);
        return 0;
    }
    *dst = d;
    return 1;
}

/* ------------------------------------------------------------------ double -> digits */
typedef struct { int sign, decpt, flag; char man[24]; } STRFLT;

/* seg4:2588: x * 10^k, one table factor per octal digit of |k|, units first */
static ext scale10(ext x, int k)
{
    int neg = k < 0;
    if (neg) k = -k;
    for (int g = 0; k && g < 3; g++, k >>= 3)
        if (k & 7) x *= P10[neg][g][k & 7];
    return x;
}

/* seg4:1E8F (with _fltout, 16DC): sign, decimal exponent and up to 16 digits of a double,
 * value = 0.DDDD * 10^decpt */
static void fltout(double v, STRFLT *f)
{
    uint64_t bits;
    memcpy(&bits, &v, 8);
    unsigned hi = (unsigned)(bits >> 48);
    bits &= ~(1ull << 63);
    f->sign = ' ';
    if (!bits) { f->decpt = 0; f->flag = 1; strcpy(f->man, "0"); return; }
    if (hi & 0x8000) f->sign = '-';
    if ((hi & 0x7FF0) == 0x7FF0) { /* 1E65: infinity and NaNs */
        uint64_t low48 = bits & 0xFFFFFFFFFFFFull;
        const char *s;
        if (!low48 && hi == 0xFFF8) s = "1#IND";
        else if (!low48 && !(hi & 0xF)) s = "1#INF";
        else s = (hi & 8) ? "1#QNAN" : "1#SNAN";
        f->decpt = 1;
        f->flag = 0;
        strcpy(f->man, s);
        return;
    }
    ext x = fabs(v);
    /* the decimal exponent from the binary one and the top mantissa byte (1EE7-1F0C) */
    uint32_t ew = ext_se(x), bh = (uint32_t)(ext_man(x) >> 56);
    uint32_t p = ew * 0x4D10u + (ew >> 8) * 0x4Du + bh * 0x9Au - 0x134312F4u;
    int e = (int16_t)(p >> 16);
    x = scale10(x, -e);
    if (ONE_MINUS <= x) { e++; x *= TENTH; }
    /* 64-bit fixed point below the binary point, 8 more bits, a bias of 0x39A (1F48-1F72) */
    uint64_t m = ext_man(x);
    int sh = 0x3FFE - (int)(ext_se(x) & 0x7FFF);
    unsigned ext8 = 0, cf = 0;
    for (int i = 0; i < sh; i++) {
        unsigned out = (unsigned)(m & 1);
        m >>= 1;
        cf = ext8 & 1;
        ext8 = (ext8 >> 1) | (out << 7);
    }
    m += 0x39A + cf;
    /* 16 digits: multiply the 72-bit fraction by ten, the overflow is the digit (1F7A-1FBB) */
    for (int i = 0; i < 16; i++) {
        unsigned __int128 n = ((unsigned __int128)m << 8) | ext8;
        n *= 10;
        f->man[i] = (char)('0' + (int)(n >> 72));
        n &= (((unsigned __int128)1) << 72) - 1;
        m = (uint64_t)(n >> 8);
        ext8 = (unsigned)(n & 0xFF);
    }
    int nd = 16;
    while (nd > 1 && f->man[nd - 1] == '0') nd--;
    f->man[nd] = 0;
    f->decpt = e;
    f->flag = 1;
}

/* seg4:1C2E: ndig digits of the mantissa into buf, rounded half up on the digit string */
static void fptostr(char *buf, int ndig, STRFLT *f)
{
    const char *mant = f->man;
    char *p = buf + 1;
    buf[0] = '0';
    while (ndig > 0) {
        *p++ = *mant ? *mant++ : '0';
        ndig--;
    }
    *p = 0;
    if (ndig >= 0 && *mant >= '5') {
        p--;
        while (*p == '9') *p-- = '0';
        (*p)++;
    }
    if (buf[0] == '1') f->decpt++;
    else memmove(buf, buf + 1, strlen(buf + 1) + 1);
}

/* seg4:1502: open a gap of n characters at p */
static void shift_right(char *p, int n)
{
    if (n) memmove(p + n, p, strlen(p) + 1);
}

/* seg4:115A: _cftoe, d.ddde+XXX */
static char *cftoe(double v, char *buf, int ndec, int caps)
{
    STRFLT f;
    fltout(v, &f);
    fptostr(buf + (f.sign == '-') + (ndec > 0), ndec + 1, &f);
    char *p = buf;
    if (f.sign == '-') *p++ = '-';
    if (ndec > 0) {
        *p = p[1];
        p++;
        *p = '.';
    }
    p = strcpy(p + ndec + 1, "e+000");
    if (caps) *p = 'E';
    p++;
    if (f.man[0] != '0') {
        int e = f.decpt - 1;
        if (e < 0) { e = -e; *p = '-'; }
        p++;
        if (e >= 100) { *p = (char)(*p + e / 100); e %= 100; }
        p++;
        if (e >= 10) { *p = (char)(*p + e / 10); e %= 10; }
        p++;
        *p = (char)(*p + e);
    }
    return buf;
}

/* seg4:12C0: _cftof, ddd.ddd */
static char *cftof(double v, char *buf, int ndec)
{
    STRFLT f;
    fltout(v, &f);
    fptostr(buf + (f.sign == '-'), f.decpt + ndec, &f);
    char *p = buf;
    if (f.sign == '-') *p++ = '-';
    if (f.decpt <= 0) {
        shift_right(p, 1);
        *p++ = '0';
    } else p += f.decpt;
    if (ndec > 0) {
        shift_right(p, 1);
        *p++ = '.';
        if (f.decpt < 0) {
            int n = -f.decpt;
            if (n > ndec) n = ndec;
            shift_right(p, n);
            memset(p, '0', (size_t)n);
        }
    }
    return buf;
}

/* seg4:0292: gcvt - ndigit significant digits, e-format below 0.01 or from 10^ndigit on, trailing
 * zeros of the fraction removed (the point stays) */
char *rt_gcvt(double v, int ndigit, char *buf)
{
    STRFLT f;
    fltout(v, &f);
    int mag = f.decpt - 1;
    if (mag < -1 || mag > ndigit - 1) cftoe(v, buf, ndigit - 1, 0);
    else cftof(v, buf, ndigit - f.decpt);
    char *s = buf;
    while (*s && *s != '.') s++;
    if (!*s++) return buf;
    while (*s && *s != 'e') s++;
    char *d = s;
    do s--; while (*s == '0');
    do *++s = *d; while (*d++);
    return buf;
}

/* ------------------------------------------------------------------ integers */
/* seg4:0B0E: digits of a 32-bit value (lower-case letters), a '-' for negative radix-10 values */
static char *xtoa(uint32_t v, char *buf, unsigned radix, int is_neg)
{
    char *p = buf, *first;
    if (is_neg) { *p++ = '-'; v = (uint32_t)-v; }
    first = p;
    do {
        unsigned d = v % radix;
        v /= radix;
        *p++ = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    } while (v);
    *p-- = 0;
    while (first < p) { char t = *first; *first++ = *p; *p-- = t; }
    return buf;
}
/* seg4:026C _itoa: a 16-bit int, signed only in radix 10 */
char *rt_itoa(int v, char *buf, int radix)
{
    int16_t w = (int16_t)v;
    if (radix == 10) return xtoa((uint32_t)(int32_t)w, buf, 10, w < 0);
    return xtoa((uint16_t)w, buf, (unsigned)radix, 0);
}
/* seg4:0288 _ltoa */
char *rt_ltoa(int32_t v, char *buf, int radix)
{
    return xtoa((uint32_t)v, buf, (unsigned)radix, radix == 10 && v < 0);
}
/* seg4:0ABA atol: blanks, a sign, decimal digits; 32-bit wrap-around */
int32_t rt_atol(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    int neg = *s == '-';
    if (*s == '-' || *s == '+') s++;
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (uint32_t)(*s++ - '0');
    return (int32_t)(neg ? (uint32_t)-v : v);
}

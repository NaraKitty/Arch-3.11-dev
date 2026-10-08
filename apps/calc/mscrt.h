/*
 * The parts of the Microsoft C 6 run-time library that CALC.EXE links, ported: the 8087 math
 * library (pow, log, sin ... with its matherr behaviour), the double-to-text conversion behind
 * gcvt, and the integer conversions. See mscrt.c.
 */
#ifndef CALC_MSCRT_H
#define CALC_MSCRT_H
#include <stdint.h>

typedef long double ext; /* an x87 register: 64-bit mantissa, 15-bit exponent */

/* struct exception.type, as passed to matherr */
enum { M_DOMAIN = 1, M_SING, M_OVERFLOW, M_UNDERFLOW, M_TLOSS, M_PLOSS };
/* the codes a SIGFPE handler receives as its second argument */
enum { FPE_INVALID_ = 0x81, FPE_ZERODIVIDE_ = 0x83, FPE_OVERFLOW_ = 0x84, FPE_UNDERFLOW_ = 0x85 };

/* the program's _matherr (called for every math library error) and SIGFPE handler */
void rt_init(int (*matherr)(int type), void (*sigfpe)(int code));

/* the C-stack entry points: double arguments, the result rounded to a double */
double rt_sqrt(double x);
double rt_log(double x);
double rt_log10(double x);
double rt_pow(double x, double y);
double rt_sin(double x);
double rt_cos(double x);
double rt_tan(double x);
double rt_asin(double x);
double rt_acos(double x);
double rt_atan(double x);
double rt_sinh(double x);
double rt_cosh(double x);
double rt_tanh(double x);
/* _CIpow, the compiler's intrinsic form: operands and result stay on the FPU stack */
ext rt_cipow(ext x, ext y);

double rt_floor(double x);
double rt_modf(double x, double *ipart);
double rt_fmod(double x, double y);
double rt_fabs(double x);
int32_t rt_ftol(ext x); /* __ftol: truncation, the low 32 bits of the 64-bit integer */

/* an FSTP to a double in the program's own code; returns 0 (and raises SIGFPE) on overflow */
int rt_st64(ext v, double *dst);

char *rt_gcvt(double v, int ndigit, char *buf);
char *rt_itoa(int v, char *buf, int radix);
char *rt_ltoa(int32_t v, char *buf, int radix);
int32_t rt_atol(const char *s);

#endif

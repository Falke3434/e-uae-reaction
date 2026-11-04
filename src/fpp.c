 /*
  * UAE - The Un*x Amiga Emulator
  *
  * MC68881 emulation
  *
  * Copyright 1996 Herman ten Brugge
  * Modified 2005 Peter Keunecke
  */

#define __USE_ISOC9X  /* We might be able to pick up a NaN */
#include <math.h>

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "memory.h"
#include "custom.h"
#include "events.h"
#include "newcpu.h"
#include "ersatz.h"
#include "md-fpp.h"
#include "savestate.h"

#if 1

#define	DEBUG_FPP	0

#define FFLAG_Z   0x4000
#define FFLAG_N   0x0100
#define FFLAG_NAN 0x0400

#define MAKE_FPSR(regs, r) (regs)->fp_result = (r)

typedef enum {
    FPP_ROUND_TO_NEAREST = 0,
    FPP_ROUND_TO_ZERO,
    FPP_ROUND_DOWN,
    FPP_ROUND_UP
} fpp_rounding_mode;

typedef enum {
    FPP_EXTENDED_PRECISION = 0,
    FPP_SINGLE_PRECISION,
    FPP_DOUBLE_PRECISION
} fpp_precision;


STATIC_INLINE void native_set_fpucw (uae_u32 m68k_cw)
{
#if USE_X86_FPUCW
    fpp_precision     m68k_prec  = (m68k_cw >> 6) & 3;
    fpp_rounding_mode m68k_round = (m68k_cw >> 4) & 3;

    unsigned int x86_prec;
    unsigned int x86_round;

    uae_u16 x;

    switch (m68k_prec) {
	case FPP_EXTENDED_PRECISION: default:	x86_prec = 3; break;
	case FPP_SINGLE_PRECISION:		x86_prec = 0; break;
	case FPP_DOUBLE_PRECISION:		x86_prec = 1; break;
    }

    switch (m68k_round) {
	case FPP_ROUND_TO_NEAREST: default:	x86_round = 0; break;
	case FPP_ROUND_TO_ZERO:			x86_round = 3; break;
	case FPP_ROUND_DOWN:			x86_round = 1; break;
	case FPP_ROUND_UP:			x86_round = 2; break;
    }

    x = 0x107f + (x86_prec << 8) + (x86_round << 10);

#ifdef _MSC_VER
    __asm {
	fldcw x
    }
#else
    __asm__ ("fldcw %0" : : "m" (*&x));
#endif
#endif /* USE_X86_FPUCW */
}

#if defined(uae_s64) /* Close enough for government work? */
typedef uae_s64 tointtype;
#else
typedef uae_s32 tointtype;
#endif

/* v19: the rounding mode is in the FPCR (bits 5-4); it was read from the
 * FPSR, where these bits are the accrued UNFL and DZ exceptions */
STATIC_INLINE fpp_rounding_mode get_rounding_mode (const struct regstruct *regs)
{
    return (regs->fpcr >> 4) & 3;
}

/* Round to nearest, ties to even (what the 68881/68882 does in RN mode).
 * The old "(tointtype)(src + 0.5)" truncates towards zero, so it was wrong
 * for every negative value (e.g. -2.7 gave -2, -1.5 gave -1).
 * Idea of the fix: FlynnTheAvatar / MorphUAE. */
STATIC_INLINE fptype round_nearest_even (fptype src)
{
    fptype r = floor (src + 0.5);
    if (r - src == 0.5 && fmod (r, 2.0) != 0.0)
	r -= 1.0;	/* exact tie -> pick the even neighbour */
    return r;
}

/* FINT: round to integral value but keep it a float (no int cast). */
STATIC_INLINE fptype fint_mode (fpp_rounding_mode mode, fptype src)
{
    switch (mode) {
	case FPP_ROUND_TO_NEAREST: return round_nearest_even (src);
	case FPP_ROUND_TO_ZERO:    return src >= 0.0 ? floor (src) : ceil (src);
	case FPP_ROUND_DOWN:       return floor (src);
	case FPP_ROUND_UP:         return ceil (src);
    }
    return src;
}

STATIC_INLINE tointtype toint (fpp_rounding_mode mode, fptype src)
{
    switch (mode) {
	case FPP_ROUND_TO_NEAREST:
#if USE_X86_FPUCW
	    return (tointtype) (src);
#else
	    return (tointtype) round_nearest_even (src);
#endif
	case FPP_ROUND_TO_ZERO:
	    return (tointtype) src;
	case FPP_ROUND_DOWN:
	    return (tointtype) floor (src);
	case FPP_ROUND_UP:
	    return (tointtype) ceil (src);
    }
    return (tointtype) src;
}

/* ---- v19 (TODO 6.7): FPSR exception bits and FPU exceptions ----------
 *
 * The exception status byte (EXC, FPSR bits 15-8) is set by every
 * arithmetic instruction, the accrued exception byte (AEXC, bits 7-3)
 * collects them until the program clears it, the quotient byte (bits
 * 23-16) is set by FMOD/FREM. An exception enabled in the FPCR (bits 15-8)
 * is taken on the 68881/68882 (vectors 48-54). The results are calculated
 * in double precision (the 6888x has 64 bit mantissas and a larger
 * exponent range), the exception bits describe that calculation.
 *
 * The condition codes (bits 31-24) are not stored: get_fpsr() derives them
 * from fp_result, as before.
 *
 * isnan()/isinf() are used directly: they are macros in math.h. Before,
 * they were only used with HAVE_ISNAN/HAVE_ISINF from configure, whose link
 * test finds no function behind a macro - then the NaN bit of the FPSR was
 * never set and FBcc/FScc compared a NaN like a positive number.
 */
#define FPSR_BSUN	0x8000
#define FPSR_SNAN	0x4000
#define FPSR_OPERR	0x2000
#define FPSR_OVFL	0x1000
#define FPSR_UNFL	0x0800
#define FPSR_DZ		0x0400
#define FPSR_INEX2	0x0200
#define FPSR_INEX1	0x0100

#define FPSR_AE_IOP	0x0080
#define FPSR_AE_OVFL	0x0040
#define FPSR_AE_UNFL	0x0020
#define FPSR_AE_DZ	0x0010
#define FPSR_AE_INEX	0x0008

#define FPSR_QUOT	0x00ff0000

/* The JIT translates no FPU instructions (comp_fpu_compilable()) while an
 * exception is enabled on a 6888x (the exception is taken in the
 * interpreter) or the FPCR asks for single precision results (the
 * translated code calculates in double precision). See fpcr_changed(). */
int fpp_jit_strict;

/* Signalling NaN: quiet bit (bit 51 of the double) clear */
STATIC_INLINE int fp_is_snan (fptype x)
{
    union {
	double d;
	uae_u32 u[2];
    } v;

    if (!isnan (x))
	return 0;
    v.d = x;
#ifdef WORDS_BIGENDIAN
    return !(v.u[0] & 0x00080000);
#else
    return !(v.u[1] & 0x00080000);
#endif
}

/* Sign bit (also of -0 and -NaN) */
STATIC_INLINE int fp_sign (fptype x)
{
    union {
	double d;
	uae_u32 u[2];
    } v;

    v.d = x;
#ifdef WORDS_BIGENDIAN
    return (v.u[0] & 0x80000000) != 0;
#else
    return (v.u[1] & 0x80000000) != 0;
#endif
}

/* EXC byte = exc, plus the matching AEXC bits (MC68881 UM, 4.5.2) */
STATIC_INLINE void fpsr_set_exc (struct regstruct *regs, uae_u32 exc)
{
    uae_u32 ae = 0;

    if (exc & (FPSR_BSUN | FPSR_SNAN | FPSR_OPERR))
	ae |= FPSR_AE_IOP;
    if (exc & FPSR_OVFL)
	ae |= FPSR_AE_OVFL;
    if ((exc & FPSR_UNFL) && (exc & FPSR_INEX2))
	ae |= FPSR_AE_UNFL;
    if (exc & FPSR_DZ)
	ae |= FPSR_AE_DZ;
    if (exc & (FPSR_OVFL | FPSR_INEX2 | FPSR_INEX1))
	ae |= FPSR_AE_INEX;
    regs->fpsr = (regs->fpsr & ~0xff00) | exc | ae;
}

/*
 * Takes the FPU exception for an exception bit set in the EXC byte whose
 * enable bit is set in the FPCR. Returns 1 if an exception was taken.
 *
 * Only on the 68881/68882: there the exception is pending and taken at the
 * next FPU instruction, here it is taken right after the instruction
 * (format 0 frame, return address = next instruction); for the handler that
 * is the same (FSAVE/FRESTORE see an idle frame, RTE continues). The
 * 68040/68060 report these exceptions through their FPSP
 * (68040.library/68060.library) with exceptional FSAVE frames, which are not
 * emulated: there only the FPSR bits are set (as before: no exception).
 */
static int fpsr_take_exception (struct regstruct *regs)
{
    uae_u32 e = regs->fpsr & regs->fpcr & 0xff00;
    int vector;

    if (!e || currprefs.cpu_level != 3)
	return 0;
    if (e & FPSR_BSUN)
	vector = 48;
    else if (e & FPSR_SNAN)
	vector = 54;
    else if (e & FPSR_OPERR)
	vector = 52;
    else if (e & FPSR_OVFL)
	vector = 53;
    else if (e & FPSR_UNFL)
	vector = 51;
    else if (e & FPSR_DZ)
	vector = 50;
    else
	vector = 49;		/* INEX1, INEX2 */
    Exception (vector, regs, m68k_getpc (regs));
    return 1;
}

/* After every change of the FPCR: rounding of the host FPU (x86 only) and
 * the JIT mode (fpp_jit_strict). Translated FPU code is thrown away when the
 * mode changes. */
static void fpcr_changed (struct regstruct *regs)
{
    int strict;

    native_set_fpucw (regs->fpcr);
    strict = ((regs->fpcr & 0xff00) && currprefs.cpu_level == 3)
	     || ((regs->fpcr >> 6) & 3) == FPP_SINGLE_PRECISION;
    if (strict != fpp_jit_strict) {
	fpp_jit_strict = strict;
#ifdef JIT
	if (currprefs.cachesize)
	    flush_icache_hard ("FPCR");
#endif
    }
}

/* Reset state of the FPU (hardware reset, FRESTORE of a NULL frame):
 * FPCR, FPSR, FPIAR 0, registers NaN */
static void fpu_null (struct regstruct *regs)
{
    int i;

    for (i = 0; i < 8; i++)
	regs->fp[i] = to_exten (0x7fff0000, 0xffffffff, 0xffffffff);
    regs->fpcr = regs->fpsr = regs->fpiar = 0;
    regs->fp_result = 1;
    fpcr_changed (regs);
}

void fpp_reset (void)
{
    fpu_null (&regs);
}

/* Rounding precision of an arithmetic result: 1 single, 2 double,
 * 0 extended (= double here). FSxxx/FDxxx choose it themselves, FSGLMUL
 * and FSGLDIV always round to single, the others follow the FPCR. */
STATIC_INLINE int fp_result_precision (const struct regstruct *regs, uae_u16 extra)
{
    int op = extra & 0x7f;

    if (op == 0x24 || op == 0x27)
	return FPP_SINGLE_PRECISION;
    if (op & 0x40)
	return (extra & 0x04) ? FPP_DOUBLE_PRECISION : FPP_SINGLE_PRECISION;
    return ((regs->fpcr >> 6) & 3) == FPP_SINGLE_PRECISION ? FPP_SINGLE_PRECISION : FPP_EXTENDED_PRECISION;
}

/* Rounds *v to single precision; returns the exception bits of that */
STATIC_INLINE uae_u32 fp_round_single (fptype *v)
{
    fptype r;

    if (isnan (*v) || isinf (*v))
	return 0;
    r = (float) *v;
    if (r == *v)
	return 0;
    if (isinf (r)) {
	*v = r;
	return FPSR_OVFL | FPSR_INEX2;
    }
    *v = r;
    if (r == 0 || fabs (r) < 1.17549435e-38)
	return FPSR_UNFL | FPSR_INEX2;
    return FPSR_INEX2;
}

/* Operations with the destination register as second operand */
STATIC_INLINE int fp_op_dyadic (int op)
{
    switch (op) {
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25:
    case 0x26: case 0x27: case 0x28: case 0x38:
    case 0x60: case 0x62: case 0x63: case 0x64: case 0x66: case 0x67:
    case 0x68: case 0x6c:
	return 1;
    }
    return 0;
}

/* Error of the rounded sum s = a + b (0 if the sum is exact) */
STATIC_INLINE fptype fp_sum_error (fptype a, fptype b, fptype s)
{
    fptype bb = s - a;
    return (a - (s - bb)) + (b - bb);
}

/*
 * Exception bits of an arithmetic operation (op = extra & 0x7f) with the
 * operands src, dst (the destination register before the operation) and
 * the double precision result res:
 *   SNAN   a signalling NaN as operand
 *   OPERR  NaN result from operands that are no NaNs (sqrt(-9), 0/0,
 *          inf-inf, 0*inf, log(-1), asin(2), fmod(x,0), ...)
 *   DZ     infinite result from finite operands: x/0, log(0), atanh(1)
 *   OVFL   infinite result from finite operands otherwise
 *   UNFL   denormal result, or 0 where the exact result is not 0
 *   INEX2  the result is rounded
 */
static uae_u32 fp_arith_exc (int op, fptype src, fptype dst, fptype res)
{
    int dyadic = fp_op_dyadic (op);
    uae_u32 exc = 0;

    if (fp_is_snan (src) || (dyadic && fp_is_snan (dst)))
	exc |= FPSR_SNAN;
    if (isnan (src) || (dyadic && isnan (dst)))
	return exc;			/* NaN in, NaN out: nothing else */
    if (isnan (res))
	return exc | FPSR_OPERR;
    if (isinf (res)) {
	if (isinf (src) || (dyadic && isinf (dst)))
	    return exc;			/* exact infinity arithmetic */
	switch (op) {
	case 0x20: case 0x24: case 0x60: case 0x64:	/* FDIV, FSGLDIV */
	case 0x14: case 0x15: case 0x16:		/* FLOGN, FLOG10, FLOG2 */
	    if (src == 0)
		return exc | FPSR_DZ;
	    break;
	case 0x06:					/* FLOGNP1 */
	    if (src == -1.0)
		return exc | FPSR_DZ;
	    break;
	case 0x0d:					/* FATANH */
	    if (src == 1.0 || src == -1.0)
		return exc | FPSR_DZ;
	    break;
	}
	return exc | FPSR_OVFL | FPSR_INEX2;
    }

    /* underflow: denormal result, or 0 instead of a tiny result */
    if (res != 0 && fabs (res) < 2.2250738585072014e-308)
	exc |= FPSR_UNFL;
    else if (res == 0) {
	switch (op) {
	case 0x23: case 0x27: case 0x63: case 0x67:	/* FMUL, FSGLMUL */
	    if (src != 0 && dst != 0)
		exc |= FPSR_UNFL | FPSR_INEX2;
	    break;
	case 0x20: case 0x24: case 0x60: case 0x64:	/* FDIV, FSGLDIV */
	    if (dst != 0 && !isinf (src))
		exc |= FPSR_UNFL | FPSR_INEX2;
	    break;
	case 0x26:					/* FSCALE */
	    if (dst != 0)
		exc |= FPSR_UNFL | FPSR_INEX2;
	    break;
	case 0x10: case 0x11: case 0x12:		/* FETOX, FTWOTOX, FTENTOX */
	    exc |= FPSR_UNFL | FPSR_INEX2;
	    break;
	}
    }
    if (exc & FPSR_INEX2)
	return exc;

    /* inexact: exact tests for the basic operations, for the
     * transcendental functions every non-trivial result counts */
    switch (op) {
    case 0x00: case 0x40: case 0x44:		/* FMOVE */
    case 0x18: case 0x58: case 0x5c:		/* FABS */
    case 0x1a: case 0x5a: case 0x5e:		/* FNEG */
    case 0x1e: case 0x1f:			/* FGETEXP, FGETMAN */
    case 0x21: case 0x25:			/* FMOD, FREM: always exact */
    case 0x26:					/* FSCALE */
	break;
    case 0x01: case 0x03:			/* FINT, FINTRZ */
	if (res != src)
	    exc |= FPSR_INEX2;
	break;
    case 0x22: case 0x62: case 0x66:		/* FADD */
	if (fp_sum_error (dst, src, res) != 0)
	    exc |= FPSR_INEX2;
	break;
    case 0x28: case 0x68: case 0x6c:		/* FSUB */
	if (fp_sum_error (dst, -src, res) != 0)
	    exc |= FPSR_INEX2;
	break;
    case 0x23: case 0x27: case 0x63: case 0x67:	/* FMUL, FSGLMUL */
	if (__builtin_fma (dst, src, -res) != 0)
	    exc |= FPSR_INEX2;
	break;
    case 0x20: case 0x24: case 0x60: case 0x64:	/* FDIV, FSGLDIV */
	if (!isinf (src) && __builtin_fma (-res, src, dst) != 0)
	    exc |= FPSR_INEX2;
	break;
    case 0x04: case 0x41: case 0x45:		/* FSQRT */
	if (__builtin_fma (-res, res, src) != 0)
	    exc |= FPSR_INEX2;
	break;
    case 0x11:					/* FTWOTOX: exact for integers */
	if (src != floor (src))
	    exc |= FPSR_INEX2;
	break;
    case 0x12:					/* FTENTOX: exact for 10^0..10^22 */
	if (src != floor (src) || src < 0 || src > 22)
	    exc |= FPSR_INEX2;
	break;
    case 0x14: case 0x15: case 0x16:		/* FLOGN, FLOG10, FLOG2 */
	if (res != floor (res))
	    exc |= FPSR_INEX2;
	break;
    default:					/* other transcendental functions */
	if (src != 0 && res != 0)
	    exc |= FPSR_INEX2;
	break;
    }
    return exc;
}

STATIC_INLINE uae_u32 get_fpsr (const struct regstruct *regs)
{
    uae_u32 answer = regs->fpsr & 0x00ffffff;

    if (isnan (regs->fp_result))
	answer |= 0x01000000;
    else {
	/* -0 and -inf are negative too (N and Z / N and I) */
	if (regs->fp_result == 0)
	    answer |= 0x04000000;
	if (fp_sign (regs->fp_result))
	    answer |= 0x08000000;
	if (isinf (regs->fp_result))
	    answer |= 0x02000000;
    }
    return answer;
}

uae_u32 fpp_get_fpsr (const struct regstruct *regs)
{
    return get_fpsr (regs);
}

STATIC_INLINE void set_fpsr (struct regstruct *regs, uae_u32 x)
{
    /* v19: the condition codes are kept in fp_result, all four of them
     * (I and N with Z were lost before) */
    uae_u32 sign = (x & 0x08000000) ? 0x80000000 : 0;

    regs->fpsr = x;

    if (x & 0x01000000)
	regs->fp_result = to_exten (sign | 0x7fff0000, 0xffffffff, 0xffffffff);
    else if (x & 0x04000000)
	regs->fp_result = sign ? -0.0 : 0.0;
    else if (x & 0x02000000)
	regs->fp_result = to_exten (sign | 0x7fff0000, 0, 0);
    else if (x & 0x08000000)
	regs->fp_result = -1;
    else
	regs->fp_result = 1;
}


/* single   : S  8*E 23*F */
/* double   : S 11*E 52*F */
/* extended : S 15*E 64*F */
/* E = 0 & F = 0 -> 0 */
/* E = MAX & F = 0 -> Infin */
/* E = MAX & F # 0 -> NotANumber */
/* E = biased by 127 (single) ,1023 (double) ,16383 (extended) */

STATIC_INLINE fptype to_pack (uae_u32 wrd1, uae_u32 wrd2, uae_u32 wrd3)
{
    fptype d;
    char *cp;
    char str[100];

    /* v19: infinity and NaN (exponent 0x7fff as in the extended format) */
    if ((wrd1 & 0x7fff0000) == 0x7fff0000)
	return to_exten (wrd1 & 0xffff0000, wrd2, wrd3);
    cp = str;
    if (wrd1 & 0x80000000)
	*cp++ = '-';
    *cp++ = (wrd1 & 0xf) + '0';
    *cp++ = '.';
    *cp++ = ((wrd2 >> 28) & 0xf) + '0';
    *cp++ = ((wrd2 >> 24) & 0xf) + '0';
    *cp++ = ((wrd2 >> 20) & 0xf) + '0';
    *cp++ = ((wrd2 >> 16) & 0xf) + '0';
    *cp++ = ((wrd2 >> 12) & 0xf) + '0';
    *cp++ = ((wrd2 >> 8) & 0xf) + '0';
    *cp++ = ((wrd2 >> 4) & 0xf) + '0';
    *cp++ = ((wrd2 >> 0) & 0xf) + '0';
    *cp++ = ((wrd3 >> 28) & 0xf) + '0';
    *cp++ = ((wrd3 >> 24) & 0xf) + '0';
    *cp++ = ((wrd3 >> 20) & 0xf) + '0';
    *cp++ = ((wrd3 >> 16) & 0xf) + '0';
    *cp++ = ((wrd3 >> 12) & 0xf) + '0';
    *cp++ = ((wrd3 >> 8) & 0xf) + '0';
    *cp++ = ((wrd3 >> 4) & 0xf) + '0';
    *cp++ = ((wrd3 >> 0) & 0xf) + '0';
    *cp++ = 'E';
    if (wrd1 & 0x40000000)
	*cp++ = '-';
    *cp++ = ((wrd1 >> 24) & 0xf) + '0';
    *cp++ = ((wrd1 >> 20) & 0xf) + '0';
    *cp++ = ((wrd1 >> 16) & 0xf) + '0';
    *cp = 0;
    sscanf (str, "%le", &d);
    return d;
}

STATIC_INLINE void from_pack (fptype src, uae_u32 * wrd1, uae_u32 * wrd2, uae_u32 * wrd3)
{
    int i;
    int t;
    char *cp;
    char str[100];

    /* v19: infinity and NaN: sprintf() gives "inf"/"nan", which was read
     * as digits */
    if (isnan (src) || isinf (src)) {
	from_exten (src, wrd1, wrd2, wrd3);
	*wrd1 &= 0xffff0000;
	return;
    }
    sprintf (str, "%.16e", src);
    cp = str;
    *wrd1 = *wrd2 = *wrd3 = 0;
    if (*cp == '-') {
	cp++;
	*wrd1 = 0x80000000;
    }
    if (*cp == '+')
	cp++;
    *wrd1 |= (*cp++ - '0');
    if (*cp == '.')
	cp++;
    for (i = 0; i < 8; i++) {
	*wrd2 <<= 4;
	if (*cp >= '0' && *cp <= '9')
	    *wrd2 |= *cp++ - '0';
    }
    for (i = 0; i < 8; i++) {
	*wrd3 <<= 4;
	if (*cp >= '0' && *cp <= '9')
	    *wrd3 |= *cp++ - '0';
    }
    if (*cp == 'e' || *cp == 'E') {
	cp++;
	if (*cp == '-') {
	    cp++;
	    *wrd1 |= 0x40000000;
	}
	if (*cp == '+')
	    cp++;
	t = 0;
	for (i = 0; i < 3; i++) {
	    if (*cp >= '0' && *cp <= '9')
		t = (t << 4) | (*cp++ - '0');
	}
	*wrd1 |= t << 16;
    }
}

STATIC_INLINE int get_fp_value (uae_u32 opcode, uae_u16 extra, fptype *src)
{
    uaecptr tmppc;
    uae_u16 tmp;
    int size;
    int mode;
    int reg;
    uae_u32 ad = 0;
    static int sz1[8] = { 4, 4, 12, 12, 2, 8, 1, 0 };
    static int sz2[8] = { 4, 4, 12, 12, 2, 8, 2, 0 };

    if ((extra & 0x4000) == 0) {
	*src = regs.fp[(extra >> 10) & 7];
	return 1;
    }
    mode = (opcode >> 3) & 7;
    reg = opcode & 7;
    size = (extra >> 10) & 7;
    switch (mode) {
    case 0:
	switch (size) {
	case 6:
	    *src = (fptype) (uae_s8) m68k_dreg (&regs, reg);
	    break;
	case 4:
	    *src = (fptype) (uae_s16) m68k_dreg (&regs, reg);
	    break;
	case 0:
	    *src = (fptype) (uae_s32) m68k_dreg (&regs, reg);
	    break;
	case 1:
	    *src = to_single (m68k_dreg (&regs, reg));
	    break;
	default:
	    return 0;
	}
	return 1;
    case 1:
	return 0;
    case 2:
	ad = m68k_areg (&regs, reg);
	break;
    case 3:
	ad = m68k_areg (&regs, reg);
	m68k_areg (&regs, reg) += reg == 7 ? sz2[size] : sz1[size];
	break;
    case 4:
	m68k_areg (&regs, reg) -= reg == 7 ? sz2[size] : sz1[size];
	ad = m68k_areg (&regs, reg);
	break;
    case 5:
	ad = m68k_areg (&regs, reg) + (uae_s32) (uae_s16) next_iword (&regs);
	break;
    case 6:
	ad = get_disp_ea_020 (&regs, m68k_areg (&regs, reg), next_iword (&regs));
	break;
    case 7:
	switch (reg) {
	case 0:
	    ad = (uae_s32) (uae_s16) next_iword (&regs);
	    break;
	case 1:
	    ad = next_ilong (&regs);
	    break;
	case 2:
	    ad = m68k_getpc (&regs);
	    ad += (uae_s32) (uae_s16) next_iword (&regs);
	    break;
	case 3:
	    tmppc = m68k_getpc (&regs);
	    tmp = next_iword (&regs);
	    ad = get_disp_ea_020 (&regs, tmppc, tmp);
	    break;
	case 4:
	    ad = m68k_getpc (&regs);
	    m68k_setpc (&regs, ad + sz2[size]);
	    if (size == 6)
		ad++;
	    break;
	default:
	    return 0;
	}
    }
    switch (size) {
    case 0:
	*src = (fptype) (uae_s32) get_long (ad);
	break;
    case 1:
	*src = to_single (get_long (ad));
	break;
    case 2:{
	    uae_u32 wrd1, wrd2, wrd3;
	    wrd1 = get_long (ad);
	    ad += 4;
	    wrd2 = get_long (ad);
	    ad += 4;
	    wrd3 = get_long (ad);
	    *src = to_exten (wrd1, wrd2, wrd3);
	}
	break;
    case 3:{
	    uae_u32 wrd1, wrd2, wrd3;
	    wrd1 = get_long (ad);
	    ad += 4;
	    wrd2 = get_long (ad);
	    ad += 4;
	    wrd3 = get_long (ad);
	    *src = to_pack (wrd1, wrd2, wrd3);
	}
	break;
    case 4:
	*src = (fptype) (uae_s16) get_word (ad);
	break;
    case 5:{
	    uae_u32 wrd1, wrd2;
	    wrd1 = get_long (ad);
	    ad += 4;
	    wrd2 = get_long (ad);
	    *src = to_double (wrd1, wrd2);
	}
	break;
    case 6:
	*src = (fptype) (uae_s8) get_byte (ad);
	break;
    default:
	return 0;
    }
    return 1;
}

/* v19 (TODO 6.7): FMOVE to an integer format (size 0 = .L, 4 = .W, 6 = .B)
 * like the 6888x (and WinUAE): a value out of range gives the largest
 * positive/negative integer and OPERR (before, the C cast wrapped around:
 * 300.0 as a byte gave 44, 1e10 as a long was undefined), a NaN gives the
 * upper bits of its mantissa and OPERR, a dropped fraction sets INEX2. */
static uae_s32 fp_to_int (const struct regstruct *regs, fptype src, int size, uae_u32 *exc)
{
    fptype lo, hi;
    tointtype r;

    switch (size) {
    case 6:  lo = -128.0;        hi = 127.0;        break;
    case 4:  lo = -32768.0;      hi = 32767.0;      break;
    default: lo = -2147483648.0; hi = 2147483647.0; break;
    }
    if (isnan (src)) {
	uae_u32 w1, w2, w3;
	from_exten (src, &w1, &w2, &w3);
	*exc |= FPSR_OPERR | (fp_is_snan (src) ? FPSR_SNAN : 0);
	return (uae_s32) (size == 6 ? (w2 >> 24) : size == 4 ? (w2 >> 16) : w2);
    }
    /* far out of range (also infinity): no conversion (undefined in C) */
    if (src > hi + 1.0) {
	*exc |= FPSR_OPERR;
	return (uae_s32) hi;
    }
    if (src < lo - 1.0) {
	*exc |= FPSR_OPERR;
	return (uae_s32) lo;
    }
    r = toint (get_rounding_mode (regs), src);
    if (r > (tointtype) hi) {
	*exc |= FPSR_OPERR;
	return (uae_s32) hi;
    }
    if (r < (tointtype) lo) {
	*exc |= FPSR_OPERR;
	return (uae_s32) lo;
    }
    if ((fptype) r != src)
	*exc |= FPSR_INEX2;
    return (uae_s32) r;
}

/* v19: FMOVE to single precision: exception bits of the conversion */
STATIC_INLINE uae_u32 fp_single_exc (fptype value)
{
    fptype v = value;

    if (isnan (value))
	return fp_is_snan (value) ? FPSR_SNAN : 0;
    return fp_round_single (&v);
}

/* Writes value in the format of extra to the destination of opcode
 * (FMOVE FPn,<ea>). v19: the exception bits of the conversion are added to
 * *exc. */
STATIC_INLINE int put_fp_value (struct regstruct *regs, fptype value, uae_u32 opcode, uae_u16 extra, uae_u32 *exc)
{
    uae_u16 tmp;
    uaecptr tmppc;
    int     size;
    int     mode;
    int     reg;
    uae_u32 ad;

    static const int sz1[8] = { 4, 4, 12, 12, 2, 8, 1, 0 };
    static const int sz2[8] = { 4, 4, 12, 12, 2, 8, 2, 0 };

    if ((extra & 0x4000) == 0) {
	regs->fp[(extra >> 10) & 7] = value;
	return 1;
    }
    mode = (opcode >> 3) & 7;
    reg  = opcode & 7;
    size = (extra >> 10) & 7;
    ad = -1;

    switch (mode) {
    case 0:
	switch (size) {
	case 6:
	    m68k_dreg (regs, reg) = (uae_u32)(((fp_to_int (regs, value, 6, exc) & 0xff)
				     | (m68k_dreg (regs, reg) & ~0xff)));
	    break;
	case 4:
	    m68k_dreg (regs, reg) = (uae_u32)(((fp_to_int (regs, value, 4, exc) & 0xffff)
				     | (m68k_dreg (regs, reg) & ~0xffff)));
	    break;
	case 0:
	    m68k_dreg (regs, reg) = (uae_u32) fp_to_int (regs, value, 0, exc);
	    break;
	case 1:
	    *exc |= fp_single_exc (value);
	    m68k_dreg (regs, reg) = from_single (value);
	    break;
	default:
	    return 0;
	}
	return 1;
    case 1:
	return 0;
    case 2:
	ad = m68k_areg (regs, reg);
	break;
    case 3:
	ad = m68k_areg (regs, reg);
	m68k_areg (regs, reg) += reg == 7 ? sz2[size] : sz1[size];
	break;
    case 4:
	m68k_areg (regs, reg) -= reg == 7 ? sz2[size] : sz1[size];
	ad = m68k_areg (regs, reg);
	break;
    case 5:
	ad = m68k_areg (regs, reg) + (uae_s32) (uae_s16) next_iword (regs);
	break;
    case 6:
	ad = get_disp_ea_020 (regs, m68k_areg (regs, reg), next_iword (regs));
	break;
    case 7:
	switch (reg) {
	case 0:
	    ad = (uae_s32) (uae_s16) next_iword (regs);
	    break;
	case 1:
	    ad = next_ilong (regs);
	    break;
	case 2:
	    ad = m68k_getpc (regs);
	    ad += (uae_s32) (uae_s16) next_iword (regs);
	    break;
	case 3:
	    tmppc = m68k_getpc (regs);
	    tmp = next_iword (regs);
	    ad = get_disp_ea_020 (regs, tmppc, tmp);
	    break;
	case 4:
	    ad = m68k_getpc (regs);
	    m68k_setpc (regs, ad + sz2[size]);
	    break;
	default:
	    return 0;
	}
    }
    switch (size) {
    case 0:
	put_long (ad, (uae_u32) fp_to_int (regs, value, 0, exc));
	break;
    case 1:
	*exc |= fp_single_exc (value);
	put_long (ad, from_single (value));
	break;
    case 2:
	{
	    uae_u32 wrd1, wrd2, wrd3;
	    from_exten (value, &wrd1, &wrd2, &wrd3);
	    put_long (ad, wrd1);
	    ad += 4;
	    put_long (ad, wrd2);
	    ad += 4;
	    put_long (ad, wrd3);
	}
	break;
    case 3:
	{
	    uae_u32 wrd1, wrd2, wrd3;
	    from_pack (value, &wrd1, &wrd2, &wrd3);
	    put_long (ad, wrd1);
	    ad += 4;
	    put_long (ad, wrd2);
	    ad += 4;
	    put_long (ad, wrd3);
	}
	break;
    case 4:
	put_word (ad, (uae_s16) fp_to_int (regs, value, 4, exc));
	break;
    case 5:{
	    uae_u32 wrd1, wrd2;
	    if (fp_is_snan (value))
		*exc |= FPSR_SNAN;
	    from_double (value, &wrd1, &wrd2);
	    put_long (ad, wrd1);
	    ad += 4;
	    put_long (ad, wrd2);
	}
	break;
    case 6:
	put_byte (ad, (uae_s8) fp_to_int (regs, value, 6, exc));
	break;
    default:
	return 0;
    }
    return 1;
}

STATIC_INLINE int get_fp_ad (uae_u32 opcode, uae_u32 * ad)
{
    uae_u16 tmp;
    uaecptr tmppc;
    int mode;
    int reg;

    mode = (opcode >> 3) & 7;
    reg = opcode & 7;
    switch (mode) {
    case 0:
    case 1:
	return 0;
    case 2:
	*ad = m68k_areg (&regs, reg);
	break;
    case 3:
	*ad = m68k_areg (&regs, reg);
	break;
    case 4:
	*ad = m68k_areg (&regs, reg);
	break;
    case 5:
	*ad = m68k_areg (&regs, reg) + (uae_s32) (uae_s16) next_iword (&regs);
	break;
    case 6:
	*ad = get_disp_ea_020 (&regs, m68k_areg (&regs, reg), next_iword (&regs));
	break;
    case 7:
	switch (reg) {
	case 0:
	    *ad = (uae_s32) (uae_s16) next_iword (&regs);
	    break;
	case 1:
	    *ad = next_ilong (&regs);
	    break;
	case 2:
	    *ad = m68k_getpc (&regs);
	    *ad += (uae_s32) (uae_s16) next_iword (&regs);
	    break;
	case 3:
	    tmppc = m68k_getpc (&regs);
	    tmp = next_iword (&regs);
	    *ad = get_disp_ea_020 (&regs, tmppc, tmp);
	    break;
	default:
	    return 0;
	}
    }
    return 1;
}

STATIC_INLINE int fpp_cond (uae_u32 opcode, int contition)
{
    int N = (regs.fp_result<0);
    int Z = (regs.fp_result==0);
    /* int I = (regs.fpsr & 0x2000000) != 0; */
    int NotANumber = isnan (regs.fp_result) ? 1 : 0;

    if (NotANumber) {
	N=Z=0;
	/* v19 (TODO 6.7): the IEEE-nonaware predicates (0x10-0x1f) set BSUN
	 * for an unordered result; the caller takes the exception if it is
	 * enabled (fpp_bsun()) */
	if (contition & 0x10)
	    fpsr_set_exc (&regs, FPSR_BSUN);
    }

    switch (contition) {
    case 0x00:
	return 0;
    case 0x01:
	return Z;
    case 0x02:
	return !(NotANumber || Z || N);
    case 0x03:
	return Z || !(NotANumber || N);
    case 0x04:
	return N && !(NotANumber || Z);
    case 0x05:
	return Z || (N && !NotANumber);
    case 0x06:
	return !(NotANumber || Z);
    case 0x07:
	return !NotANumber;
    case 0x08:
	return NotANumber;
    case 0x09:
	return NotANumber || Z;
    case 0x0a:
	return NotANumber || !(N || Z);
    case 0x0b:
	return NotANumber || Z || !N;
    case 0x0c:
	return NotANumber || (N && !Z);
    case 0x0d:
	return NotANumber || Z || N;
    case 0x0e:
	return !Z;
    case 0x0f:
	return 1;
    case 0x10:
	return 0;
    case 0x11:
	return Z;
    case 0x12:
	return !(NotANumber || Z || N);
    case 0x13:
	return Z || !(NotANumber || N);
    case 0x14:
	return N && !(NotANumber || Z);
    case 0x15:
	return Z || (N && !NotANumber);
    case 0x16:
	return !(NotANumber || Z);
    case 0x17:
	return !NotANumber;
    case 0x18:
	return NotANumber;
    case 0x19:
	return NotANumber || Z;
    case 0x1a:
	return NotANumber || !(N || Z);
    case 0x1b:
	return NotANumber || Z || !N;
    case 0x1c:
#if 0
	return NotANumber || (Z && N); /* This is wrong, compare 0x0c */
#else
	return NotANumber || (N && !Z);
#endif
    case 0x1d:
	return NotANumber || Z || N;
    case 0x1e:
	return !Z;
    case 0x1f:
	return 1;
    }
    return -1;
}

/* v19 (TODO 6.7): BSUN enabled in the FPCR: the exception is taken before
 * the conditional instruction (return address = the instruction itself, as
 * on the 6888x; the handler has to clear the NaN or the enable bit) */
STATIC_INLINE int fpp_bsun (struct regstruct *regs, uaecptr instrpc)
{
    if (!(regs->fpsr & regs->fpcr & FPSR_BSUN) || currprefs.cpu_level != 3)
	return 0;
    m68k_setpc (regs, instrpc);
    Exception (48, regs, instrpc);
    return 1;
}

void fdbcc_opp (uae_u32 opcode, struct regstruct *regs, uae_u16 extra)
{
    uaecptr pc = (uae_u32) m68k_getpc (regs);
    uae_s32 disp = (uae_s32) (uae_s16) next_iword (regs);
    int cc;

#if DEBUG_FPP
    write_log ("FPU: fdbcc_opp at %08lx\n", m68k_getpc (regs));
    flush_log ();
#endif
    cc = fpp_cond (opcode, extra & 0x3f);
    if (cc == -1) {
	m68k_setpc (regs, pc - 4);
	op_illg (opcode, regs);
    } else if (fpp_bsun (regs, pc - 4)) {
	return;
    } else if (!cc) {
	int reg = opcode & 0x7;

	m68k_dreg (regs, reg) = ((m68k_dreg (regs, reg) & ~0xffff)
				 | ((m68k_dreg (regs, reg) - 1) & 0xffff));
	if ((m68k_dreg (regs, reg) & 0xffff) != 0xffff)
	    m68k_setpc (regs, pc + disp);
    }
}

void fscc_opp (uae_u32 opcode, struct regstruct *regs, uae_u16 extra)
{
    uaecptr instrpc = m68k_getpc (regs) - 4;
    uae_u32 ad;
    int cc;

#if DEBUG_FPP
    write_log ("FPU: fscc_opp at %08lx\n", m68k_getpc (regs));
    flush_log ();
#endif
    cc = fpp_cond (opcode, extra & 0x3f);
    if (cc == -1) {
	m68k_setpc (regs, m68k_getpc (regs) - 4);
	op_illg (opcode, regs);
    } else if (fpp_bsun (regs, instrpc)) {
	return;
    } else if ((opcode & 0x38) == 0) {
	m68k_dreg (regs, opcode & 7) = (m68k_dreg (regs, opcode & 7) & ~0xff) | (cc ? 0xff : 0x00);
    } else {
	if (get_fp_ad (opcode, &ad) == 0) {
	    m68k_setpc (regs, m68k_getpc (regs) - 4);
	    op_illg (opcode, regs);
	} else
	    put_byte (ad, cc ? 0xff : 0x00);
    }
}

/* oldpc: address after the opcode word. v19: the condition is in the
 * extension word that follows (was taken from the opcode), the operand
 * (FTRAPcc.W #, FTRAPcc.L #) comes after it - the generated code read the
 * extension word as the operand and continued 2 bytes too early. */
void ftrapcc_opp (uae_u32 opcode, struct regstruct *regs, uaecptr oldpc)
{
    uae_u16 extra = get_word (oldpc);
    int cc;

#if DEBUG_FPP
    write_log ("FPU: ftrapcc_opp at %08lx\n", m68k_getpc (regs));
    flush_log ();
#endif
    m68k_setpc (regs, oldpc + 2 + ((opcode & 7) == 2 ? 2 : (opcode & 7) == 3 ? 4 : 0));
    cc = fpp_cond (opcode, extra & 0x3f);
    if (cc == -1) {
	m68k_setpc (regs, oldpc - 2);
	op_illg (opcode, regs);
	return;
    }
    if (fpp_bsun (regs, oldpc - 2))
	return;
    if (cc)
	Exception (7, regs, oldpc - 2);
}

void fbcc_opp (uae_u32 opcode, struct regstruct *regs, uaecptr pc, uae_u32 extra)
{
    int cc;

#if DEBUG_FPP
    write_log ("FPU: fbcc_opp at %08lx\n", m68k_getpc (regs));
    flush_log ();
#endif
    cc = fpp_cond (opcode, opcode & 0x3f);
    if (cc == -1) {
	m68k_setpc (regs, pc);
	op_illg (opcode, regs);
    } else if (fpp_bsun (regs, pc - 2)) {
	return;
    } else if (cc) {
	if ((opcode & 0x40) == 0)
	    extra = (uae_s32) (uae_s16) extra;
	m68k_setpc (regs, pc + extra);
    }
}

/*
 * v19 (TODO 6.7): FSAVE frames of the FPU models (always the idle frame:
 * the FPU never stops in the middle of an instruction here)
 *   68881   version $1F, 28 bytes ($18 bytes after the format word)
 *   68882   version $1F, 60 bytes ($38) - before, the 68882 was never
 *           reported: exec recognises the 68882 by this size (AFF_68882)
 *   68040   version $41, 4 bytes
 *   68060   12 bytes, frame type $60 in bits 15-8 of the first long (was
 *           the 68040 frame)
 * Contents of the 6888x frame as in WinUAE: command/condition register,
 * (68882: 8 internal longs), exceptional operand, operand register, BIU
 * flags ($5C0EFFFF: no exception pending).
 */
void fsave_opp (uae_u32 opcode, struct regstruct *regs)
{
    uae_u32 ad;
    int incr = (opcode & 0x38) == 0x20 ? -1 : 1;
    int i, size;

#if DEBUG_FPP
    write_log ("FPU: fsave_opp at %08lx\n", m68k_getpc (regs));
    flush_log ();
#endif
    if (get_fp_ad (opcode, &ad) == 0) {
	m68k_setpc (regs, m68k_getpc (regs) - 2);
	op_illg (opcode, regs);
	return;
    }

    if (currprefs.cpu_level >= 6)
	size = 12;
    else if (currprefs.cpu_level >= 4)
	size = 4;
    else
	size = currprefs.fpu_model == 68882 ? 0x3c : 0x1c;
    if (incr < 0)
	ad -= size;
    if ((opcode & 0x38) == 0x20)
	m68k_areg (regs, opcode & 7) = ad;

    if (currprefs.cpu_level >= 6) {
	put_long (ad, 0x00006000);
	put_long (ad + 4, 0);
	put_long (ad + 8, 0);
    } else if (currprefs.cpu_level >= 4) {
	put_long (ad, 0x41000000);
    } else {
	put_long (ad, 0x1f000000 | ((uae_u32) (size - 4) << 16));
	for (i = 4; i < size - 4; i += 4)
	    put_long (ad + i, 0);	/* CIR, (68882 internal), operands */
	put_long (ad + size - 4, 0x5c0effff);	/* BIU flags */
    }
    ad += size;
    if ((opcode & 0x38) == 0x18)
	m68k_areg (regs, opcode & 7) = ad;
}

/* v19: a NULL frame resets the FPU (FPCR, FPSR, FPIAR 0, registers NaN),
 * as the real FPU does; exec restores a NULL frame for tasks that have not
 * used the FPU yet. The 6888x frames are skipped by their size byte (any
 * size, not only $18/$38/$B4), the 68060 frame is 12 bytes. */
void frestore_opp (uae_u32 opcode, struct regstruct *regs)
{
    uae_u32 ad;
    uae_u32 d;
    int incr = (opcode & 0x38) == 0x20 ? -1 : 1;

#if DEBUG_FPP
    write_log ("FPU: frestore_opp at %08lx\n", m68k_getpc (regs));
    flush_log ();
#endif
    if (get_fp_ad (opcode, &ad) == 0) {
	m68k_setpc (regs, m68k_getpc (regs) - 2);
	op_illg (opcode, regs);
	return;
    }
    if (currprefs.cpu_level >= 6) {
	/* 68060 */
	if (incr < 0) {
	    ad -= 12;
	    d = get_long (ad);
	} else {
	    d = get_long (ad);
	    ad += 12;
	}
	if ((d & 0x0000ff00) == 0)
	    fpu_null (regs);
    } else if (currprefs.cpu_level >= 4) {
	/* 68040 */
	if (incr < 0) {
	    /* @@@ This may be wrong.  */
	    ad -= 4;
	    d = get_long (ad);
	    if ((d & 0xff000000) != 0) { /* Not a NULL frame? */
		if ((d & 0x00ff0000) == 0) { /* IDLE */
		} else if ((d & 0x00ff0000) == 0x00300000) { /* UNIMP */
		    ad -= 44;
		} else if ((d & 0x00ff0000) == 0x00600000) { /* BUSY */
		    ad -= 92;
		}
	    }
	} else {
	    d = get_long (ad);
	    ad += 4;
	    if ((d & 0xff000000) != 0) { /* Not a NULL frame? */
		if ((d & 0x00ff0000) == 0) { /* IDLE */
		} else if ((d & 0x00ff0000) == 0x00300000) { /* UNIMP */
		    ad += 44;
		} else if ((d & 0x00ff0000) == 0x00600000) { /* BUSY */
		    ad += 92;
		}
	    }
	}
	if ((d & 0xff000000) == 0)
	    fpu_null (regs);
    } else {
	/* 68881/68882 */
	if (incr < 0) {
	    ad -= 4;
	    d = get_long (ad);
	    if ((d & 0xff000000) != 0)
		ad -= (d >> 16) & 0xff;
	} else {
	    d = get_long (ad);
	    ad += 4;
	    if ((d & 0xff000000) != 0)
		ad += (d >> 16) & 0xff;
	}
	if ((d & 0xff000000) == 0)
	    fpu_null (regs);
    }
    if ((opcode & 0x38) == 0x18)
	m68k_areg (regs, opcode & 7) = ad;
    if ((opcode & 0x38) == 0x20)
	m68k_areg (regs, opcode & 7) = ad;
}

/* The arithmetic of the general FPU instructions (FMOVE, FADD, ... to an
 * FPU register) with the source operand already fetched. Returns 0 for an
 * illegal operation. v18: moved out of fpp_opp() for the JIT (TODO 7.3),
 * which fetches the source itself and calls fpp_jit_arith().
 * v19 (TODO 6.7): sets the exception bits of the FPSR (fp_arith_exc()),
 * rounds to the result precision (FSxxx, FSGLMUL/FSGLDIV, FPCR precision
 * single) in one place, FMOD/FREM set the quotient byte, FREM rounds the
 * quotient to even (IEEE remainder), FGETMAN of infinity is a NaN. The
 * exception itself is taken by the caller (fpp_opp()). */
static int fpp_arith (struct regstruct *regs, uae_u16 extra, fptype src)
{
	int reg = (extra >> 7) & 7;
	int op = extra & 0x7f;
	fptype dst = regs->fp[reg];
	uae_u32 exc;

	switch (op) {
	case 0x00: /* FMOVE */
	case 0x40: /* FSMOVE */
	case 0x44: /* FDMOVE */
	    regs->fp[reg] = src;        /* Brian King was here. */
	    /*<ea> to register needs FPSR updated. See Motorola 68K Manual. */
	    break;
	case 0x01: /* FINT */
	    /* need to take the current rounding mode into account */
	    regs->fp[reg] = fint_mode (get_rounding_mode (regs), src);
	    break;
	case 0x02: /* FSINH */
	    regs->fp[reg] = sinh (src);
	    break;
	case 0x03: /* FINTRZ */
	    if (src >= 0.0)
		regs->fp[reg] = floor (src);
	    else
		regs->fp[reg] = ceil (src);
	    break;
	case 0x04: /* FSQRT */
	case 0x41:
	case 0x45:
	    regs->fp[reg] = sqrt (src);
	    break;
	case 0x06: /* FLOGNP1 */
	    regs->fp[reg] = log (src + 1.0);
	    break;
	case 0x08: /* FETOXM1 */
	    regs->fp[reg] = exp (src) - 1.0;
	    break;
	case 0x09: /* FTANH */
	    regs->fp[reg] = tanh (src);
	    break;
	case 0x0a: /* FATAN */
	    regs->fp[reg] = atan (src);
	    break;
	case 0x0c: /* FASIN */
	    regs->fp[reg] = asin (src);
	    break;
	case 0x0d: /* FATANH */
#if 1	/* The BeBox doesn't have atanh, and it isn't in the HPUX libm either */
	    regs->fp[reg] = 0.5 * log ((1 + src) / (1 - src));
#else
	    regs->fp[reg] = atanh (src);
#endif
	    break;
	case 0x0e: /* FSIN */
	    regs->fp[reg] = sin (src);
	    break;
	case 0x0f: /* FTAN */
	    regs->fp[reg] = tan (src);
	    break;
	case 0x10: /* FETOX */
	    regs->fp[reg] = exp (src);
	    break;
	case 0x11: /* FTWOTOX */
	    regs->fp[reg] = pow (2.0, src);
	    break;
	case 0x12: /* FTENTOX */
	    regs->fp[reg] = pow (10.0, src);
	    break;
	case 0x14: /* FLOGN */
	    regs->fp[reg] = log (src);
	    break;
	case 0x15: /* FLOG10 */
	    regs->fp[reg] = log10 (src);
	    break;
	case 0x16: /* FLOG2 */
	    regs->fp[reg] = log (src) / log (2.0);
	    break;
	case 0x18: /* FABS */
	case 0x58:
	case 0x5c:
	    regs->fp[reg] = fabs (src);
	    break;
	case 0x19: /* FCOSH */
	    regs->fp[reg] = cosh (src);
	    break;
	case 0x1a: /* FNEG */
	case 0x5a:
	case 0x5e:
	    regs->fp[reg] = -src;
	    break;
	case 0x1c: /* FACOS */
	    regs->fp[reg] = acos (src);
	    break;
	case 0x1d: /* FCOS */
	    regs->fp[reg] = cos (src);
	    break;
	case 0x1e: /* FGETEXP */
	    {
	      int expon;
	      if (src == 0.0)
		  regs->fp[reg] = src;
	      else if (isnan (src) || isinf (src))
		  regs->fp[reg] = src - src;	/* NaN */
	      else {
		  frexp (src, &expon);
		  regs->fp[reg] = (double) (expon - 1);
	      }
	    }
	    break;
	case 0x1f: /* FGETMAN */
	    {
	      int expon;
	      if (isnan (src) || isinf (src))
		  regs->fp[reg] = src - src;	/* NaN (v19: was infinity) */
	      else
		  regs->fp[reg] = frexp (src, &expon) * 2.0;
	    }
	    break;
	case 0x20: /* FDIV */
	case 0x60:
	case 0x64:
	case 0x24: /* FSGLDIV */
	    regs->fp[reg] /= src;
	    break;
	case 0x21: /* FMOD: remainder with the quotient rounded to zero */
	case 0x25: /* FREM: quotient rounded to nearest (even) */
	    {
	      fptype r, q;

	      r = fmod (dst, src);		/* exact, sign of dst */
	      if (op == 0x25 && !isnan (r) && r != 0) {
		  /* IEEE remainder from fmod(): one step further when the
		   * rest is more than half the divisor (or exactly half and
		   * the truncated quotient is odd) */
		  fptype half = fabs (src) * 0.5;
		  fptype qt = floor (fabs ((dst - r) / src) + 0.5);
		  if (fabs (r) > half || (fabs (r) == half && fmod (qt, 2.0) != 0))
		      r = r > 0 ? r - fabs (src) : r + fabs (src);
	      }
	      regs->fp[reg] = r;
	      /* quotient byte: sign and the low 7 bits of the quotient */
	      regs->fpsr &= ~FPSR_QUOT;
	      if (!isnan (r) && !isinf (dst) && src != 0 && !isinf (src)) {
		  q = fabs ((dst - r) / src);
		  q = floor (q + 0.5);
		  if (q < 4294967296.0)
		      regs->fpsr |= ((uae_u32) q & 0x7f) << 16;
		  else
		      regs->fpsr |= ((uae_u32) fmod (q, 128.0)) << 16;
		  if (fp_sign (dst) != fp_sign (src))
		      regs->fpsr |= 0x00800000;
	      }
	    }
	    break;
	case 0x22: /* FADD */
	case 0x62:
	case 0x66:
	    regs->fp[reg] += src;
	    break;
	case 0x23: /* FMUL */
	case 0x63:
	case 0x67:
	case 0x27: /* FSGLMUL */
	    regs->fp[reg] *= src;
	    break;
	case 0x26: /* FSCALE */
	    /* scale by 2^(integer part of src), exact (was exp(log(2)*src)) */
	    if (isnan (src) || isinf (src) || isnan (regs->fp[reg]))
		regs->fp[reg] = regs->fp[reg] + src * 0.0;	/* NaN */
	    else {
		fptype n = src >= 0.0 ? floor (src) : ceil (src);
		if (n > 100000.0) n = 100000.0;
		if (n < -100000.0) n = -100000.0;
		regs->fp[reg] = ldexp (regs->fp[reg], (int) n);
	    }
	    break;
	case 0x28: /* FSUB */
	case 0x68:
	case 0x6c:
	    regs->fp[reg] -= src;
	    break;
	case 0x30: /* FSINCOS */
	case 0x31:
	case 0x32:
	case 0x33:
	case 0x34:
	case 0x35:
	case 0x36:
	case 0x37:
	    regs->fp[extra & 7] = cos (src);
	    regs->fp[reg] = sin (src);
	    if (fp_result_precision (regs, extra) == FPP_SINGLE_PRECISION)
		fp_round_single (&regs->fp[extra & 7]);
	    break;
	case 0x38: /* FCMP */
	    {
	      fptype tmp = regs->fp[reg] == src ? 0.0 : regs->fp[reg] - src;
	      exc = 0;
	      if (fp_is_snan (src) || fp_is_snan (dst))
		  exc = FPSR_SNAN;
	      fpsr_set_exc (regs, exc);
	      MAKE_FPSR (regs, tmp);
	    }
	    return 1;
	case 0x3a: /* FTST */
	    fpsr_set_exc (regs, fp_is_snan (src) ? FPSR_SNAN : 0);
	    MAKE_FPSR (regs, src);
	    return 1;
	default:
	    return 0;
	}
	exc = fp_arith_exc (op, src, dst, regs->fp[reg]);
	if (fp_result_precision (regs, extra) == FPP_SINGLE_PRECISION)
	    exc |= fp_round_single (&regs->fp[reg]);
	fpsr_set_exc (regs, exc);
	MAKE_FPSR (regs, regs->fp[reg]);
	return 1;
}

void fpp_opp (uae_u32 opcode, struct regstruct *regs, uae_u16 extra)
{
    int reg;
    fptype src;

#if DEBUG_FPP
    write_log ("FPU: %04lx %04x at %08lx\n", opcode & 0xffff, extra & 0xffff, m68k_getpc (regs) - 4);
    flush_log ();
#endif
    switch ((extra >> 13) & 0x7) {
    case 3:
	{
	    /* v19: FMOVE FPn,<ea> sets the exception bits of the conversion
	     * (OPERR for an integer out of range, OVFL/UNFL/INEX2 for single
	     * precision, ...), FPIAR = this instruction */
	    uae_u32 exc = 0;
	    uaecptr instrpc = m68k_getpc (regs) - 4;
	    if (put_fp_value (regs, regs->fp[(extra >> 7) & 7], opcode, extra, &exc) == 0) {
		m68k_setpc (regs, m68k_getpc (regs) - 4);
		op_illg (opcode, regs);
		return;
	    }
	    regs->fpiar = instrpc;
	    fpsr_set_exc (regs, exc);
	    fpsr_take_exception (regs);
	}
	return;
    case 4:
    case 5:
	if ((opcode & 0x38) == 0) {
	    if (extra & 0x2000) {
		if (extra & 0x1000)
		    m68k_dreg (regs, opcode & 7) = regs->fpcr;
		if (extra & 0x0800)
		    m68k_dreg (regs, opcode & 7) = get_fpsr (regs);
		if (extra & 0x0400)
		    m68k_dreg (regs, opcode & 7) = regs->fpiar;
	    } else {
		if (extra & 0x1000) {
		    regs->fpcr = m68k_dreg (regs, opcode & 7);
		    fpcr_changed (regs);
		}
		if (extra & 0x0800)
		    set_fpsr (regs, m68k_dreg (regs, opcode & 7));
		if (extra & 0x0400)
		    regs->fpiar = m68k_dreg (regs, opcode & 7);
	    }
	} else if ((opcode & 0x38) == 0x08) {
	    if (extra & 0x2000) {
		if (extra & 0x1000)
		    m68k_areg (regs, opcode & 7) = regs->fpcr;
		if (extra & 0x0800)
		    m68k_areg (regs, opcode & 7) = get_fpsr (regs);
		if (extra & 0x0400)
		    m68k_areg (regs, opcode & 7) = regs->fpiar;
	    } else {
		if (extra & 0x1000) {
		    regs->fpcr = m68k_areg (regs, opcode & 7);
		    fpcr_changed (regs);
		}
		if (extra & 0x0800)
		    set_fpsr (regs, m68k_areg (regs, opcode & 7));
		if (extra & 0x0400)
		    regs->fpiar = m68k_areg (regs, opcode & 7);
	    }
	} else if ((opcode & 0x3f) == 0x3c) {
	    if ((extra & 0x2000) == 0) {
		if (extra & 0x1000) {
		    regs->fpcr = next_ilong (regs);
		    fpcr_changed (regs);
		}
		if (extra & 0x0800)
		    set_fpsr (regs, next_ilong (regs));
		if (extra & 0x0400)
		    regs->fpiar = next_ilong (regs);
	    }
	} else if (extra & 0x2000) {
	    /* FMOVEM FPP->memory */
	    uae_u32 ad;
	    int incr = 0;

	    if (get_fp_ad (opcode, &ad) == 0) {
		m68k_setpc (regs, m68k_getpc (regs) - 4);
		op_illg (opcode, regs);
		return;
	    }
	    if ((opcode & 0x38) == 0x20) {
		if (extra & 0x1000)
		    incr += 4;
		if (extra & 0x0800)
		    incr += 4;
		if (extra & 0x0400)
		    incr += 4;
	    }
	    ad -= incr;
	    if (extra & 0x1000) {
		put_long (ad, regs->fpcr);
		ad += 4;
	    }
	    if (extra & 0x0800) {
		put_long (ad, get_fpsr (regs));
		ad += 4;
	    }
	    if (extra & 0x0400) {
		put_long (ad, regs->fpiar);
		ad += 4;
	    }
	    ad -= incr;
	    if ((opcode & 0x38) == 0x18)
		m68k_areg (regs, opcode & 7) = ad;
	    if ((opcode & 0x38) == 0x20)
		m68k_areg (regs, opcode & 7) = ad;
	} else {
	    /* FMOVEM memory->FPP */
	    uae_u32 ad;

	    if (get_fp_ad (opcode, &ad) == 0) {
		m68k_setpc (regs, m68k_getpc (regs) - 4);
		op_illg (opcode, regs);
		return;
	    }
	    ad = (opcode & 0x38) == 0x20 ? ad - 12 : ad;
	    if (extra & 0x1000) {
		regs->fpcr = get_long (ad);
		fpcr_changed (regs);
		ad += 4;
	    }
	    if (extra & 0x0800) {
		set_fpsr (regs, get_long (ad));
		ad += 4;
	    }
	    if (extra & 0x0400) {
		regs->fpiar = get_long (ad);
		ad += 4;
	    }
	    if ((opcode & 0x38) == 0x18)
		m68k_areg (regs, opcode & 7) = ad;
	    if ((opcode & 0x38) == 0x20)
		m68k_areg (regs, opcode & 7) = ad - 12;
	}
	return;
    case 6:
    case 7:
    {
	uae_u32 ad, list = 0;
	int incr = 0;
	if (extra & 0x2000) {
	    /* FMOVEM FPP->memory */
	    if (get_fp_ad (opcode, &ad) == 0) {
		m68k_setpc (regs, m68k_getpc (regs) - 4);
		op_illg (opcode, regs);
		return;
	    }
	    switch ((extra >> 11) & 3) {
	    case 0:	/* static pred */
		list = extra & 0xff;
		incr = -1;
		break;
	    case 1:	/* dynamic pred */
		list = m68k_dreg (regs, (extra >> 4) & 3) & 0xff;
		incr = -1;
		break;
	    case 2:	/* static postinc */
		list = extra & 0xff;
		incr = 1;
		break;
	    case 3:	/* dynamic postinc */
		list = m68k_dreg (regs, (extra >> 4) & 3) & 0xff;
		incr = 1;
		break;
	    }
	    while (list) {
		uae_u32 wrd1, wrd2, wrd3;
		if (incr < 0) {
		    from_exten (regs->fp[fpp_movem_index2[list]], &wrd1, &wrd2, &wrd3);
		    ad -= 4;
		    put_long (ad, wrd3);
		    ad -= 4;
		    put_long (ad, wrd2);
		    ad -= 4;
		    put_long (ad, wrd1);
		} else {
		    from_exten (regs->fp[fpp_movem_index1[list]], &wrd1, &wrd2, &wrd3);
		    put_long (ad, wrd1);
		    ad += 4;
		    put_long (ad, wrd2);
		    ad += 4;
		    put_long (ad, wrd3);
		    ad += 4;
		}
		list = fpp_movem_next[list];
	    }
	    if ((opcode & 0x38) == 0x18)
		m68k_areg (regs, opcode & 7) = ad;
	    if ((opcode & 0x38) == 0x20)
		m68k_areg (regs, opcode & 7) = ad;
	} else {
	    /* FMOVEM memory->FPP */
	    if (get_fp_ad (opcode, &ad) == 0) {
		m68k_setpc (regs, m68k_getpc (regs) - 4);
		op_illg (opcode, regs);
		return;
	    }
	    switch ((extra >> 11) & 3) {
	    case 0:	/* static pred */
		list = extra & 0xff;
		incr = -1;
		break;
	    case 1:	/* dynamic pred */
		list = m68k_dreg (regs, (extra >> 4) & 3) & 0xff;
		incr = -1;
		break;
	    case 2:	/* static postinc */
		list = extra & 0xff;
		incr = 1;
		break;
	    case 3:	/* dynamic postinc */
		list = m68k_dreg (regs, (extra >> 4) & 3) & 0xff;
		incr = 1;
		break;
	    }
	    while (list) {
		uae_u32 wrd1, wrd2, wrd3;
		if (incr < 0) {
		    ad -= 4;
		    wrd3 = get_long (ad);
		    ad -= 4;
		    wrd2 = get_long (ad);
		    ad -= 4;
		    wrd1 = get_long (ad);
		    regs->fp[fpp_movem_index2[list]] = to_exten (wrd1, wrd2, wrd3);
		} else {
		    wrd1 = get_long (ad);
		    ad += 4;
		    wrd2 = get_long (ad);
		    ad += 4;
		    wrd3 = get_long (ad);
		    ad += 4;
		    regs->fp[fpp_movem_index1[list]] = to_exten (wrd1, wrd2, wrd3);
		}
		list = fpp_movem_next[list];
	    }
	    if ((opcode & 0x38) == 0x18)
		m68k_areg (regs, opcode & 7) = ad;
	    if ((opcode & 0x38) == 0x20)
		m68k_areg (regs, opcode & 7) = ad;
	}
    }
    return;
    case 0:
    case 2: /* Extremely common */
	reg = (extra >> 7) & 7;
	/* v19: FPIAR = address of the instruction (for exception handlers) */
	regs->fpiar = m68k_getpc (regs) - 4;
	if ((extra & 0xfc00) == 0x5c00) {
	    /* FMOVECR: constant ROM of the 6888x. v19: undefined offsets give
	     * 0 (were an illegal instruction exception - the 6888x has no such
	     * exception for them), 1e512 and up are infinite in a double (no
	     * more "constant exceeds range" warnings), INEX2 for the inexact
	     * constants, rounding precision of the FPCR */
	    uae_u32 exc = FPSR_INEX2;
	    switch (extra & 0x7f) {
	    case 0x00:
		regs->fp[reg] = 4.0 * atan (1.0);
		break;
	    case 0x0b:
		regs->fp[reg] = log10 (2.0);
		break;
	    case 0x0c:
		regs->fp[reg] = 2.71828182845904523536;
		break;
	    case 0x0d:
		regs->fp[reg] = 1.44269504088896340736;
		break;
	    case 0x0e:
		regs->fp[reg] = 0.43429448190325182765;
		break;
	    case 0x30:
		regs->fp[reg] = log (2.0);
		break;
	    case 0x31:
		regs->fp[reg] = log (10.0);
		break;
	    case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
		/* 1e0, 1e1, 1e2, 1e4, 1e8, 1e16: exact */
		{
		    static const double tens[6] = { 1.0e0, 1.0e1, 1.0e2, 1.0e4, 1.0e8, 1.0e16 };
		    regs->fp[reg] = tens[(extra & 0x7f) - 0x32];
		}
		exc = 0;
		break;
	    case 0x38:
		regs->fp[reg] = 1.0e32;
		break;
	    case 0x39:
		regs->fp[reg] = 1.0e64;
		break;
	    case 0x3a:
		regs->fp[reg] = 1.0e128;
		break;
	    case 0x3b:
		regs->fp[reg] = 1.0e256;
		break;
	    case 0x3c: case 0x3d: case 0x3e: case 0x3f:
		/* 1e512 .. 1e4096: beyond the range of a double */
		regs->fp[reg] = to_exten (0x7fff0000, 0, 0);
		exc = FPSR_OVFL | FPSR_INEX2;
		break;
	    default:
		/* 0.0 (0x0f) and the undefined offsets */
		regs->fp[reg] = 0.0;
		exc = 0;
		break;
	    }
	    if (((regs->fpcr >> 6) & 3) == FPP_SINGLE_PRECISION)
		exc |= fp_round_single (&regs->fp[reg]);
	    fpsr_set_exc (regs, exc);
	    MAKE_FPSR (regs, regs->fp[reg]); /* see Motorola 68k Manual */
	    fpsr_take_exception (regs);
	    return;
	}
	if (get_fp_value (opcode, extra, &src) == 0
	    || !fpp_arith (regs, extra, src)) {
	    m68k_setpc (regs, m68k_getpc (regs) - 4);
	    op_illg (opcode, regs);
	    return;
	}
	fpsr_take_exception (regs);
	return;
    }
    m68k_setpc (regs, m68k_getpc (regs) - 4);
    op_illg (opcode, regs);
}

#ifdef JIT
/* v18 (TODO 7.3): entry points for the translated FPU instructions. The
 * translated code fetches the source operand (any format the JIT handles
 * converts exactly like the interpreter: lfs/lfd, int to double), does
 * FMOVE/FADD/FSUB/FMUL/FDIV/FABS/FNEG/FCMP/FTST itself and calls these for
 * the rest, so the results are the same as in the interpreter. */
void fpp_jit_arith (uae_u32 extra, fptype src)
{
    /* v19: sets the exception bits; no exception is taken (fpp_jit_strict) */
    fpp_arith (&regs, (uae_u16) extra, src);
}

/* FMOVE.L/.W/.B to a data register or memory: the integer value as the
 * interpreter calculates it (put_fp_value()). v19: fmt = format of the
 * extension word (0 .L, 4 .W, 6 .B): out of range saturates to that size,
 * the exception bits are set (no exception is taken: while one is enabled
 * the JIT translates no FPU instructions, fpp_jit_strict) */
uae_u32 fpp_jit_toint (fptype src, uae_u32 fmt)
{
    uae_u32 exc = 0;
    uae_u32 v = (uae_u32) fp_to_int (&regs, src, (int) fmt, &exc);

    fpsr_set_exc (&regs, exc);
    return v;
}

/* Is the operation of a general FPU instruction known (fpp_arith())? */
int fpp_jit_arith_valid (uae_u16 extra)
{
    switch (extra & 0x7f) {
    case 0x00: case 0x40: case 0x44: case 0x01: case 0x02: case 0x03:
    case 0x04: case 0x41: case 0x45: case 0x06: case 0x08: case 0x09:
    case 0x0a: case 0x0c: case 0x0d: case 0x0e: case 0x0f: case 0x10:
    case 0x11: case 0x12: case 0x14: case 0x15: case 0x16: case 0x18:
    case 0x58: case 0x5c: case 0x19: case 0x1a: case 0x5a: case 0x5e:
    case 0x1c: case 0x1d: case 0x1e: case 0x1f: case 0x20: case 0x60:
    case 0x64: case 0x21: case 0x22: case 0x62: case 0x66: case 0x23:
    case 0x63: case 0x67: case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x28: case 0x68: case 0x6c: case 0x30: case 0x31: case 0x32:
    case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38:
    case 0x3a:
	return 1;
    }
    return 0;
}
#endif

#endif


#ifdef SAVESTATE

/*
 * v19 (TODO 6.7): the registers are stored as in WinUAE: 16 bit sign and
 * exponent, 32 + 32 bit mantissa (10 bytes). Before, the 32 bit high word
 * (sign/exponent and 16 zero bits), the middle long and only the low 16 bits
 * of the low long were stored: 16 mantissa bits were lost (the value changed
 * after loading a state). Old states are recognised by the zero bits after
 * the exponent (in the new layout they are the top of the mantissa, which is
 * 0 only for 0 and infinity - for those both layouts give the same value).
 * The model (68881/68882/68040/68060) is stored as before.
 */
const uae_u8 *restore_fpu (const uae_u8 *src)
{
    unsigned int model, i;
    const uae_u8 *regs_src;
    int oldformat = 1;

    model = restore_u32 ();
    restore_u32 ();
    if (currprefs.cpu_level == 2) {
	currprefs.cpu_level++;
	init_m68k ();
    }
    changed_prefs.cpu_level = currprefs.cpu_level;
    if (currprefs.cpu_level == 3 && (model == 68881 || model == 68882))
	changed_prefs.fpu_model = currprefs.fpu_model = model;

    regs_src = src;
    for (i = 0; i < 8; i++) {
	if (regs_src[i * 10 + 2] || regs_src[i * 10 + 3])
	    oldformat = 0;
    }
    for (i = 0; i < 8; i++) {
	uae_u32 w1, w2, w3;
	if (oldformat) {
	    w1 = restore_u32 ();
	    w2 = restore_u32 ();
	    w3 = restore_u16 ();
	} else {
	    w1 = (uae_u32) restore_u16 () << 16;
	    w2 = restore_u32 ();
	    w3 = restore_u32 ();
	}
	regs.fp[i] = to_exten (w1, w2, w3);
    }
    regs.fpcr = restore_u32 ();
    regs.fpsr = restore_u32 ();
    regs.fpiar = restore_u32 ();
    fpcr_changed (&regs);
    return src;
}

uae_u8 *save_fpu (uae_u32 *len, uae_u8 *dstptr)
{
    uae_u8 *dstbak, *dst;
    unsigned int model, i;

    *len = 0;
    switch (currprefs.cpu_level)
    {
	case 3:
	model = currprefs.fpu_model == 68882 ? 68882 : 68881;
	break;
	case 4:
	model = 68040;
	break;
	case 6:
	model = 68060;
	break;
	default:
	return 0;
    }
    if (dstptr)
	dstbak = dst = dstptr;
    else
	dstbak = dst = malloc(4+4+8*10+4+4+4);
    save_u32 (model);
    save_u32 (0);
    for (i = 0; i < 8; i++) {
	uae_u32 w1, w2, w3;
	from_exten (regs.fp[i], &w1, &w2, &w3);
	save_u16 (w1 >> 16);
	save_u32 (w2);
	save_u32 (w3);
    }
    save_u32 (regs.fpcr);
    save_u32 (regs.fpsr);
    save_u32 (regs.fpiar);
    *len = dst - dstbak;
    return dstbak;
}

#endif /* SAVESTATE */

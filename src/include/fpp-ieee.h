 /*
  * UAE - The Un*x Amiga Emulator
  *
  * MC68881 emulation
  * Support functions for IEEE 754-compatible host CPUs.
  * These functions use a GCC extension (type punning through unions) and
  * should only be compiled with compilers that support this.
  *
  * Copyright 1999 Sam Jordan
  * Copyright 2007 Richard Drummond
  */

STATIC_INLINE double to_single (uae_u32 value)
{
    union {
	float f;
	uae_u32 u;
    } val;

    val.u = value;
    return val.f;
}

STATIC_INLINE uae_u32 from_single (double src)
{
    union {
	float f;
	uae_u32 u;
    } val;

    val.f = src;
    return val.u;
}

STATIC_INLINE double to_double (uae_u32 wrd1, uae_u32 wrd2)
{
    union {
	double d;
	uae_u32 u[2];
    } val;

#ifdef WORDS_BIGENDIAN
    val.u[0] = wrd1;
    val.u[1] = wrd2;
#else
    val.u[1] = wrd1;
    val.u[0] = wrd2;
#endif
    return val.d;
}

STATIC_INLINE void from_double (double src, uae_u32 * wrd1, uae_u32 * wrd2)
{
    union {
	double d;
	uae_u32 u[2];
    } val;

    val.d = src;
#ifdef WORDS_BIGENDIAN
    *wrd1 = val.u[0];
    *wrd2 = val.u[1];
#else
    *wrd1 = val.u[1];
    *wrd2 = val.u[0];
#endif
}

/* v19 (TODO 6.7): 96-bit extended format <-> double, bit exact. The
 * generic versions in fpp-unknown.h calculate with frexp()/ldexp() and know
 * neither infinity nor NaN: a NaN written with FMOVE.X/FMOVEM.X came back as
 * 0.5 (sqrt(-9) "was" 0.5), an infinity read with FMOVE.X became a number,
 * -0 lost its sign. Now: infinity and NaN (payload and quiet bit kept),
 * signed zero, denormals (double) and unnormals (extended) are converted. */
STATIC_INLINE double to_exten (uae_u32 wrd1, uae_u32 wrd2, uae_u32 wrd3)
{
    union {
	double d;
	uae_u32 u[2];
    } val;
    uae_u32 sign = wrd1 & 0x80000000;
    int expon = (wrd1 >> 16) & 0x7fff;

    if (expon == 0x7fff) {
	/* infinity or NaN: the explicit integer bit (bit 63) does not count */
	uae_u32 hi = (wrd2 & 0x7fffffff) >> 11;
	uae_u32 lo = (wrd2 << 21) | (wrd3 >> 11);

	if ((wrd2 & 0x7fffffff) == 0 && wrd3 == 0) {
	    hi = lo = 0;			/* infinity */
	} else if (hi == 0 && lo == 0) {
	    lo = 1;				/* NaN with payload in the low bits only */
	}
#ifdef WORDS_BIGENDIAN
	val.u[0] = sign | 0x7ff00000 | hi;
	val.u[1] = lo;
#else
	val.u[1] = sign | 0x7ff00000 | hi;
	val.u[0] = lo;
#endif
	return val.d;
    }
    if (wrd2 == 0 && wrd3 == 0)
	return sign ? -0.0 : 0.0;
    /* exponent 0 is 2^-16383 on the 6888x/68040 (no implicit bit); the
     * smallest values underflow to 0 in a double anyway */
    val.d = ldexp ((double) wrd2 * 4294967296.0 + (double) wrd3, expon - 16383 - 63);
    return sign ? -val.d : val.d;
}

STATIC_INLINE void from_exten (double src, uae_u32 * wrd1, uae_u32 * wrd2, uae_u32 * wrd3)
{
    union {
	double d;
	uae_u32 u[2];
    } val;
    uae_u32 hi, lo, sign;
    int expon;

    val.d = src;
#ifdef WORDS_BIGENDIAN
    hi = val.u[0];
    lo = val.u[1];
#else
    hi = val.u[1];
    lo = val.u[0];
#endif
    sign = hi & 0x80000000;
    expon = (hi >> 20) & 0x7ff;
    hi &= 0x000fffff;

    if (expon == 0x7ff) {
	/* infinity: mantissa 0; NaN: payload and quiet bit (bit 62) kept,
	 * integer bit set as the 6888x writes it */
	*wrd1 = sign | 0x7fff0000;
	if (hi == 0 && lo == 0) {
	    *wrd2 = *wrd3 = 0;
	} else {
	    *wrd2 = 0x80000000 | (hi << 11) | (lo >> 21);
	    *wrd3 = lo << 11;
	}
	return;
    }
    if (expon == 0) {
	if (hi == 0 && lo == 0) {
	    *wrd1 = sign;			/* +0 / -0 */
	    *wrd2 = *wrd3 = 0;
	    return;
	}
	/* double denormal: normalise (the extended format has the range) */
	expon = 1;
	while (!(hi & 0x00100000)) {
	    hi = (hi << 1) | (lo >> 31);
	    lo <<= 1;
	    expon--;
	}
	hi &= 0x000fffff;
    }
    *wrd1 = sign | ((uae_u32) (expon - 1023 + 16383) << 16);
    *wrd2 = 0x80000000 | (hi << 11) | (lo >> 21);
    *wrd3 = lo << 11;
}

#define HAVE_from_exten
#define HAVE_to_exten
#define HAVE_from_double
#define HAVE_to_double
#define HAVE_from_single
#define HAVE_to_single

/* Get the rest of the conversion functions defined.  */
#include "fpp-unknown.h"

/*
 * QEMU float support
 *
 * The code in this source file is derived from release 2a of the SoftFloat
 * IEC/IEEE Floating-point Arithmetic Package. Those parts of the code (and
 * some later contributions) are provided under that license, as detailed below.
 * It has subsequently been modified by contributors to the QEMU Project,
 * so some portions are provided under:
 *  the SoftFloat-2a license
 *  the BSD license
 *  GPL-v2-or-later
 *
 * Any future contributions to this file after December 1st 2014 will be
 * taken to be licensed under the Softfloat-2a license unless specifically
 * indicated otherwise.
 */

/*
===============================================================================
This C source file is part of the SoftFloat IEC/IEEE Floating-point
Arithmetic Package, Release 2a.

Written by John R. Hauser.  This work was made possible in part by the
International Computer Science Institute, located at Suite 600, 1947 Center
Street, Berkeley, California 94704.  Funding was partially provided by the
National Science Foundation under grant MIP-9311980.  The original version
of this code was written as part of a project to build a fixed-point vector
processor in collaboration with the University of California at Berkeley,
overseen by Profs. Nelson Morgan and John Wawrzynek.  More information
is available through the Web page `http://HTTP.CS.Berkeley.EDU/~jhauser/
arithmetic/SoftFloat.html'.

THIS SOFTWARE IS DISTRIBUTED AS IS, FOR FREE.  Although reasonable effort
has been made to avoid it, THIS SOFTWARE MAY CONTAIN FAULTS THAT WILL AT
TIMES RESULT IN INCORRECT BEHAVIOR.  USE OF THIS SOFTWARE IS RESTRICTED TO
PERSONS AND ORGANIZATIONS WHO CAN AND WILL TAKE FULL RESPONSIBILITY FOR ANY
AND ALL LOSSES, COSTS, OR OTHER PROBLEMS ARISING FROM ITS USE.

Derivative works are acceptable, even for commercial purposes, so long as
(1) they include prominent notice that the work is derivative, and (2) they
include prominent notice akin to these four paragraphs for those parts of
this code that are retained.

===============================================================================
*/

/* BSD licensing:
 * Copyright (c) 2006, Fabrice Bellard
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Portions of this work are licensed under the terms of the GNU GPL,
 * version 2 or later. See the COPYING file in the top-level directory.
 */

/* softfloat (and in particular the code in softfloat-specialize.h) is
 * target-dependent and needs the TARGET_* macros.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/bitops.h"
#include "fpu/softfloat.h"

/* We only need stdlib for abort() */

/*----------------------------------------------------------------------------
| Primitive arithmetic functions, including multi-word arithmetic, and
| division and square root approximations.  (Can be specialized to target if
| desired.)
*----------------------------------------------------------------------------*/
#include "fpu/softfloat-macros.h"

/*
 * Hardfloat
 *
 * Fast emulation of guest FP instructions is challenging for two reasons.
 * First, FP instruction semantics are similar but not identical, particularly
 * when handling NaNs. Second, emulating at reasonable speed the guest FP
 * exception flags is not trivial: reading the host's flags register with a
 * feclearexcept & fetestexcept pair is slow [slightly slower than soft-fp],
 * and trapping on every FP exception is not fast nor pleasant to work with.
 *
 * We address these challenges by leveraging the host FPU for a subset of the
 * operations. To do this we expand on the idea presented in this paper:
 *
 * Guo, Yu-Chuan, et al. "Translating the ARM Neon and VFP instructions in a
 * binary translator." Software: Practice and Experience 46.12 (2016):1591-1615.
 *
 * The idea is thus to leverage the host FPU to (1) compute FP operations
 * and (2) identify whether FP exceptions occurred while avoiding
 * expensive exception flag register accesses.
 *
 * An important optimization shown in the paper is that given that exception
 * flags are rarely cleared by the guest, we can avoid recomputing some flags.
 * This is particularly useful for the inexact flag, which is very frequently
 * raised in floating-point workloads.
 *
 * We optimize the code further by deferring to soft-fp whenever FP exception
 * detection might get hairy. Two examples: (1) when at least one operand is
 * denormal/inf/NaN; (2) when operands are not guaranteed to lead to a 0 result
 * and the result is < the minimum normal.
 */
#define GEN_INPUT_FLUSH__NOCHECK(name, soft_t)                          \
    static inline void name(soft_t *a, float_status *s)                 \
    {                                                                   \
        if (unlikely(soft_t ## _is_denormal(*a))) {                     \
            *a = soft_t ## _set_sign(soft_t ## _zero,                   \
                                     soft_t ## _is_neg(*a));            \
            float_raise(float_flag_input_denormal, s);                  \
        }                                                               \
    }

GEN_INPUT_FLUSH__NOCHECK(float32_input_flush__nocheck, float32)
GEN_INPUT_FLUSH__NOCHECK(float64_input_flush__nocheck, float64)
#undef GEN_INPUT_FLUSH__NOCHECK

#define GEN_INPUT_FLUSH1(name, soft_t)                  \
    static inline void name(soft_t *a, float_status *s) \
    {                                                   \
        if (likely(!s->flush_inputs_to_zero)) {         \
            return;                                     \
        }                                               \
        soft_t ## _input_flush__nocheck(a, s);          \
    }

GEN_INPUT_FLUSH1(float32_input_flush1, float32)
GEN_INPUT_FLUSH1(float64_input_flush1, float64)
#undef GEN_INPUT_FLUSH1

#define GEN_INPUT_FLUSH2(name, soft_t)                                  \
    static inline void name(soft_t *a, soft_t *b, float_status *s)      \
    {                                                                   \
        if (likely(!s->flush_inputs_to_zero)) {                         \
            return;                                                     \
        }                                                               \
        soft_t ## _input_flush__nocheck(a, s);                          \
        soft_t ## _input_flush__nocheck(b, s);                          \
    }

GEN_INPUT_FLUSH2(float32_input_flush2, float32)
GEN_INPUT_FLUSH2(float64_input_flush2, float64)
#undef GEN_INPUT_FLUSH2

#define GEN_INPUT_FLUSH3(name, soft_t)                                  \
    static inline void name(soft_t *a, soft_t *b, soft_t *c, float_status *s) \
    {                                                                   \
        if (likely(!s->flush_inputs_to_zero)) {                         \
            return;                                                     \
        }                                                               \
        soft_t ## _input_flush__nocheck(a, s);                          \
        soft_t ## _input_flush__nocheck(b, s);                          \
        soft_t ## _input_flush__nocheck(c, s);                          \
    }

GEN_INPUT_FLUSH3(float32_input_flush3, float32)
GEN_INPUT_FLUSH3(float64_input_flush3, float64)
#undef GEN_INPUT_FLUSH3

/*
 * Choose whether to use fpclassify or float32/64_* primitives in the generated
 * hardfloat functions. Each combination of number of inputs and float size
 * gets its own value.
 */
#if defined(__x86_64__)
# define QEMU_HARDFLOAT_1F32_USE_FP 0
# define QEMU_HARDFLOAT_1F64_USE_FP 1
# define QEMU_HARDFLOAT_2F32_USE_FP 0
# define QEMU_HARDFLOAT_2F64_USE_FP 1
# define QEMU_HARDFLOAT_3F32_USE_FP 0
# define QEMU_HARDFLOAT_3F64_USE_FP 1
#else
# define QEMU_HARDFLOAT_1F32_USE_FP 0
# define QEMU_HARDFLOAT_1F64_USE_FP 0
# define QEMU_HARDFLOAT_2F32_USE_FP 0
# define QEMU_HARDFLOAT_2F64_USE_FP 0
# define QEMU_HARDFLOAT_3F32_USE_FP 0
# define QEMU_HARDFLOAT_3F64_USE_FP 0
#endif

/*
 * QEMU_HARDFLOAT_USE_ISINF chooses whether to use isinf() over
 * float{32,64}_is_infinity when !USE_FP.
 * On x86_64/aarch64, using the former over the latter can yield a ~6% speedup.
 * On power64 however, using isinf() reduces fp-bench performance by up to 50%.
 */
#if defined(__x86_64__) || defined(__aarch64__)
# define QEMU_HARDFLOAT_USE_ISINF   1
#else
# define QEMU_HARDFLOAT_USE_ISINF   0
#endif

/*
 * Some targets clear the FP flags before most FP operations. This prevents
 * the use of hardfloat, since hardfloat relies on the inexact flag being
 * already set.
 */
#if defined(TARGET_PPC) || defined(__FAST_MATH__)
# if defined(__FAST_MATH__)
#  warning disabling hardfloat due to -ffast-math: hardfloat requires an exact \
    IEEE implementation
# endif
# define QEMU_NO_HARDFLOAT 1
# define QEMU_SOFTFLOAT_ATTR QEMU_FLATTEN
#else
# define QEMU_NO_HARDFLOAT 0
# define QEMU_SOFTFLOAT_ATTR QEMU_FLATTEN __attribute__((noinline))
#endif

static inline bool can_use_fpu(const float_status *s)
{
    if (QEMU_NO_HARDFLOAT) {
        return false;
    }
    return likely(s->float_exception_flags & float_flag_inexact &&
                  s->float_rounding_mode == float_round_nearest_even);
}

/*
 * Hardfloat generation functions. Each operation can have two flavors:
 * either using softfloat primitives (e.g. float32_is_zero_or_normal) for
 * most condition checks, or native ones (e.g. fpclassify).
 *
 * The flavor is chosen by the callers. Instead of using macros, we rely on the
 * compiler to propagate constants and inline everything into the callers.
 *
 * We only generate functions for operations with two inputs, since only
 * these are common enough to justify consolidating them into common code.
 */

typedef union {
    float32 s;
    float h;
} union_float32;

typedef union {
    float64 s;
    double h;
} union_float64;

typedef bool (*f32_check_fn)(union_float32 a, union_float32 b);
typedef bool (*f64_check_fn)(union_float64 a, union_float64 b);

typedef float32 (*soft_f32_op2_fn)(float32 a, float32 b, float_status *s);
typedef float64 (*soft_f64_op2_fn)(float64 a, float64 b, float_status *s);
typedef float   (*hard_f32_op2_fn)(float a, float b);
typedef double  (*hard_f64_op2_fn)(double a, double b);

/* 2-input is-zero-or-normal */
static inline bool f32_is_zon2(union_float32 a, union_float32 b)
{
    if (QEMU_HARDFLOAT_2F32_USE_FP) {
        /*
         * Not using a temp variable for consecutive fpclassify calls ends up
         * generating faster code.
         */
        return (fpclassify(a.h) == FP_NORMAL || fpclassify(a.h) == FP_ZERO) &&
               (fpclassify(b.h) == FP_NORMAL || fpclassify(b.h) == FP_ZERO);
    }
    return float32_is_zero_or_normal(a.s) &&
           float32_is_zero_or_normal(b.s);
}

static inline bool f64_is_zon2(union_float64 a, union_float64 b)
{
    if (QEMU_HARDFLOAT_2F64_USE_FP) {
        return (fpclassify(a.h) == FP_NORMAL || fpclassify(a.h) == FP_ZERO) &&
               (fpclassify(b.h) == FP_NORMAL || fpclassify(b.h) == FP_ZERO);
    }
    return float64_is_zero_or_normal(a.s) &&
           float64_is_zero_or_normal(b.s);
}

/* 3-input is-zero-or-normal */
static inline
bool f32_is_zon3(union_float32 a, union_float32 b, union_float32 c)
{
    if (QEMU_HARDFLOAT_3F32_USE_FP) {
        return (fpclassify(a.h) == FP_NORMAL || fpclassify(a.h) == FP_ZERO) &&
               (fpclassify(b.h) == FP_NORMAL || fpclassify(b.h) == FP_ZERO) &&
               (fpclassify(c.h) == FP_NORMAL || fpclassify(c.h) == FP_ZERO);
    }
    return float32_is_zero_or_normal(a.s) &&
           float32_is_zero_or_normal(b.s) &&
           float32_is_zero_or_normal(c.s);
}

static inline
bool f64_is_zon3(union_float64 a, union_float64 b, union_float64 c)
{
    if (QEMU_HARDFLOAT_3F64_USE_FP) {
        return (fpclassify(a.h) == FP_NORMAL || fpclassify(a.h) == FP_ZERO) &&
               (fpclassify(b.h) == FP_NORMAL || fpclassify(b.h) == FP_ZERO) &&
               (fpclassify(c.h) == FP_NORMAL || fpclassify(c.h) == FP_ZERO);
    }
    return float64_is_zero_or_normal(a.s) &&
           float64_is_zero_or_normal(b.s) &&
           float64_is_zero_or_normal(c.s);
}

static inline bool f32_is_inf(union_float32 a)
{
    if (QEMU_HARDFLOAT_USE_ISINF) {
        return isinf(a.h);
    }
    return float32_is_infinity(a.s);
}

static inline bool f64_is_inf(union_float64 a)
{
    if (QEMU_HARDFLOAT_USE_ISINF) {
        return isinf(a.h);
    }
    return float64_is_infinity(a.s);
}

static inline float32
float32_gen2(float32 xa, float32 xb, float_status *s,
             hard_f32_op2_fn hard, soft_f32_op2_fn soft,
             f32_check_fn pre, f32_check_fn post)
{
    union_float32 ua, ub, ur;

    ua.s = xa;
    ub.s = xb;

    if (unlikely(!can_use_fpu(s))) {
        goto soft;
    }

    float32_input_flush2(&ua.s, &ub.s, s);
    if (unlikely(!pre(ua, ub))) {
        goto soft;
    }

    ur.h = hard(ua.h, ub.h);
    if (unlikely(f32_is_inf(ur))) {
        float_raise(float_flag_overflow, s);
    } else if (unlikely(fabsf(ur.h) <= FLT_MIN) && post(ua, ub)) {
        goto soft;
    }
    return ur.s;

 soft:
    return soft(ua.s, ub.s, s);
}

static inline float64
float64_gen2(float64 xa, float64 xb, float_status *s,
             hard_f64_op2_fn hard, soft_f64_op2_fn soft,
             f64_check_fn pre, f64_check_fn post)
{
    union_float64 ua, ub, ur;

    ua.s = xa;
    ub.s = xb;

    if (unlikely(!can_use_fpu(s))) {
        goto soft;
    }

    float64_input_flush2(&ua.s, &ub.s, s);
    if (unlikely(!pre(ua, ub))) {
        goto soft;
    }

    ur.h = hard(ua.h, ub.h);
    if (unlikely(f64_is_inf(ur))) {
        float_raise(float_flag_overflow, s);
    } else if (unlikely(fabs(ur.h) <= DBL_MIN) && post(ua, ub)) {
        goto soft;
    }
    return ur.s;

 soft:
    return soft(ua.s, ub.s, s);
}

/*----------------------------------------------------------------------------
| Returns the fraction bits of the single-precision floating-point value `a'.
*----------------------------------------------------------------------------*/

static inline uint32_t extractFloat32Frac(float32 a)
{
    return float32_val(a) & 0x007FFFFF;
}

/*----------------------------------------------------------------------------
| Returns the exponent bits of the single-precision floating-point value `a'.
*----------------------------------------------------------------------------*/

static inline int extractFloat32Exp(float32 a)
{
    return (float32_val(a) >> 23) & 0xFF;
}

/*----------------------------------------------------------------------------
| Returns the sign bit of the single-precision floating-point value `a'.
*----------------------------------------------------------------------------*/

static inline bool extractFloat32Sign(float32 a)
{
    return float32_val(a) >> 31;
}

/*----------------------------------------------------------------------------
| Returns the fraction bits of the double-precision floating-point value `a'.
*----------------------------------------------------------------------------*/

static inline uint64_t extractFloat64Frac(float64 a)
{
    return float64_val(a) & UINT64_C(0x000FFFFFFFFFFFFF);
}

/*----------------------------------------------------------------------------
| Returns the exponent bits of the double-precision floating-point value `a'.
*----------------------------------------------------------------------------*/

static inline int extractFloat64Exp(float64 a)
{
    return (float64_val(a) >> 52) & 0x7FF;
}

/*----------------------------------------------------------------------------
| Returns the sign bit of the double-precision floating-point value `a'.
*----------------------------------------------------------------------------*/

static inline bool extractFloat64Sign(float64 a)
{
    return float64_val(a) >> 63;
}

/*
 * Classify a floating point number. Everything above float_class_qnan
 * is a NaN so cls >= float_class_qnan is any NaN.
 */

typedef enum __attribute__ ((__packed__)) {
    float_class_unclassified,
    float_class_zero,
    float_class_normal,
    float_class_inf,
    float_class_qnan,  /* all NaNs from here */
    float_class_snan,
} FloatClass;

#define float_cmask(bit)  (1u << (bit))

enum {
    float_cmask_zero    = float_cmask(float_class_zero),
    float_cmask_normal  = float_cmask(float_class_normal),
    float_cmask_inf     = float_cmask(float_class_inf),
    float_cmask_qnan    = float_cmask(float_class_qnan),
    float_cmask_snan    = float_cmask(float_class_snan),

    float_cmask_infzero = float_cmask_zero | float_cmask_inf,
    float_cmask_anynan  = float_cmask_qnan | float_cmask_snan,
};

/* Flags for parts_minmax. */
enum {
    /* Set for minimum; clear for maximum. */
    minmax_ismin = 1,
    /* Set for the IEEE 754-2008 minNum() and maxNum() operations. */
    minmax_isnum = 2,
    /* Set for the IEEE 754-2008 minNumMag() and minNumMag() operations. */
    minmax_ismag = 4,
    /*
     * Set for the IEEE 754-2019 minimumNumber() and maximumNumber()
     * operations.
     */
    minmax_isnumber = 8,
};

/* Simple helpers for checking if, or what kind of, NaN we have */
static inline __attribute__((unused)) bool is_nan(FloatClass c)
{
    return unlikely(c >= float_class_qnan);
}

static inline __attribute__((unused)) bool is_snan(FloatClass c)
{
    return c == float_class_snan;
}

static inline __attribute__((unused)) bool is_qnan(FloatClass c)
{
    return c == float_class_qnan;
}

/*
 * Structure holding all of the decomposed parts of a float.
 * The exponent is unbiased and the fraction is normalized.
 *
 * The fraction words are stored in big-endian word ordering,
 * so that truncation from a larger format to a smaller format
 * can be done simply by ignoring subsequent elements.
 */

typedef struct {
    FloatClass cls;
    bool sign;
    int32_t exp;
    union {
        /* Routines that know the structure may reference the singular name. */
        uint64_t frac;
        /*
         * Routines expanded with multiple structures reference "hi" and "lo"
         * depending on the operation.  In FloatParts64, "hi" and "lo" are
         * both the same word and aliased here.
         */
        uint64_t frac_hi;
        uint64_t frac_lo;
    };
} FloatParts64;

typedef struct {
    FloatClass cls;
    bool sign;
    int32_t exp;
    uint64_t frac_hi;
    uint64_t frac_lo;
} FloatParts128;

typedef struct {
    FloatClass cls;
    bool sign;
    int32_t exp;
    uint64_t frac_hi;
    uint64_t frac_hm;  /* high-middle */
    uint64_t frac_lm;  /* low-middle */
    uint64_t frac_lo;
} FloatParts256;

/* These apply to the most significant word of each FloatPartsN. */
#define DECOMPOSED_BINARY_POINT    63
#define DECOMPOSED_IMPLICIT_BIT    (1ull << DECOMPOSED_BINARY_POINT)

/* Structure holding all of the relevant parameters for a format.
 *   exp_size: the size of the exponent field
 *   exp_bias: the offset applied to the exponent field
 *   exp_max: the maximum normalised exponent
 *   frac_size: the size of the fraction field
 *   frac_shift: shift to normalise the fraction with DECOMPOSED_BINARY_POINT
 * The following are computed based the size of fraction
 *   round_mask: bits below lsb which must be rounded
 * The following optional modifiers are available:
 *   arm_althp: handle ARM Alternative Half Precision
 *   m68k_denormal: explicit integer bit for extended precision may be 1
 */
typedef struct {
    int exp_size;
    int exp_bias;
    int exp_re_bias;
    int exp_max;
    int frac_size;
    int frac_shift;
    bool arm_althp;
    bool m68k_denormal;
    uint64_t round_mask;
} FloatFmt;

/* Expand fields based on the size of exponent and fraction */
#define FLOAT_PARAMS_(E, F)                             \
    .exp_size       = E,                                \
    .exp_bias       = ((1 << E) - 1) >> 1,              \
    .exp_max        = (1 << E) - 1,                     \
    .frac_size      = F

#define FLOAT_PARAMS(E, F)                              \
    FLOAT_PARAMS_(E, F),                                \
    .frac_shift     = (-F - 1) & 63,                    \
    .round_mask     = (1ull << ((-F - 1) & 63)) - 1

static const FloatFmt float16_params = {
    FLOAT_PARAMS(5, 10)
};

static const FloatFmt float16_params_ahp = {
    FLOAT_PARAMS(5, 10),
    .arm_althp = true
};

static const FloatFmt bfloat16_params = {
    FLOAT_PARAMS(8, 7)
};

static const FloatFmt float32_params = {
    FLOAT_PARAMS(8, 23)
};

static const FloatFmt float64_params = {
    FLOAT_PARAMS(11, 52)
};

static const FloatFmt float128_params = {
    FLOAT_PARAMS(15, 112)
};

/* Unpack a float to parts, but do not canonicalize.  */
static void unpack_raw64(FloatParts64 *r, const FloatFmt *fmt, uint64_t raw)
{
    const int f_size = fmt->frac_size;
    const int e_size = fmt->exp_size;

    *r = (FloatParts64) {
        .cls = float_class_unclassified,
        .sign = extract64(raw, f_size + e_size, 1),
        .exp = extract64(raw, f_size, e_size),
        .frac = extract64(raw, 0, f_size)
    };
}

static void QEMU_FLATTEN float16_unpack_raw(FloatParts64 *p, float16 f)
{
    unpack_raw64(p, &float16_params, f);
}

static void QEMU_FLATTEN bfloat16_unpack_raw(FloatParts64 *p, bfloat16 f)
{
    unpack_raw64(p, &bfloat16_params, f);
}

static void QEMU_FLATTEN float32_unpack_raw(FloatParts64 *p, float32 f)
{
    unpack_raw64(p, &float32_params, f);
}

static void QEMU_FLATTEN float64_unpack_raw(FloatParts64 *p, float64 f)
{
    unpack_raw64(p, &float64_params, f);
}

static void float128_unpack_raw(FloatParts128 *p, float128 f)
{
    const int f_size = float128_params.frac_size - 64;
    const int e_size = float128_params.exp_size;

    *p = (FloatParts128) {
        .cls = float_class_unclassified,
        .sign = extract64(f.high, f_size + e_size, 1),
        .exp = extract64(f.high, f_size, e_size),
        .frac_hi = extract64(f.high, 0, f_size),
        .frac_lo = f.low,
    };
}

/* Pack a float from parts, but do not canonicalize.  */
static uint64_t pack_raw64(const FloatParts64 *p, const FloatFmt *fmt)
{
    const int f_size = fmt->frac_size;
    const int e_size = fmt->exp_size;
    uint64_t ret;

    ret = (uint64_t)p->sign << (f_size + e_size);
    ret = deposit64(ret, f_size, e_size, p->exp);
    ret = deposit64(ret, 0, f_size, p->frac);
    return ret;
}

static float16 QEMU_FLATTEN float16_pack_raw(const FloatParts64 *p)
{
    return make_float16(pack_raw64(p, &float16_params));
}

static bfloat16 QEMU_FLATTEN bfloat16_pack_raw(const FloatParts64 *p)
{
    return pack_raw64(p, &bfloat16_params);
}

static float32 QEMU_FLATTEN float32_pack_raw(const FloatParts64 *p)
{
    return make_float32(pack_raw64(p, &float32_params));
}

static float64 QEMU_FLATTEN float64_pack_raw(const FloatParts64 *p)
{
    return make_float64(pack_raw64(p, &float64_params));
}

static float128 QEMU_FLATTEN float128_pack_raw(const FloatParts128 *p)
{
    const int f_size = float128_params.frac_size - 64;
    const int e_size = float128_params.exp_size;
    uint64_t hi;

    hi = (uint64_t)p->sign << (f_size + e_size);
    hi = deposit64(hi, f_size, e_size, p->exp);
    hi = deposit64(hi, 0, f_size, p->frac_hi);
    return make_float128(hi, p->frac_lo);
}

/*----------------------------------------------------------------------------
| Functions and definitions to determine:  (1) whether tininess for underflow
| is detected before or after rounding by default, (2) what (if anything)
| happens when exceptions are raised, (3) how signaling NaNs are distinguished
| from quiet NaNs, (4) the default generated quiet NaNs, and (5) how NaNs
| are propagated from function inputs to output.  These details are target-
| specific.
*----------------------------------------------------------------------------*/
#include "softfloat-specialize.c.inc"

#define PARTS_GENERIC_64_128(NAME, P) \
    _Generic((P), FloatParts64 *: parts64_##NAME, \
                  FloatParts128 *: parts128_##NAME)

#define PARTS_GENERIC_64_128_256(NAME, P) \
    _Generic((P), FloatParts64 *: parts64_##NAME, \
                  FloatParts128 *: parts128_##NAME, \
                  FloatParts256 *: parts256_##NAME)

#define parts_default_nan(P, S)    PARTS_GENERIC_64_128(default_nan, P)(P, S)
#define parts_silence_nan(P, S)    PARTS_GENERIC_64_128(silence_nan, P)(P, S)

static void parts64_return_nan(FloatParts64 *a, float_status *s);
static void parts128_return_nan(FloatParts128 *a, float_status *s);

#define parts_return_nan(P, S)     PARTS_GENERIC_64_128(return_nan, P)(P, S)

static FloatParts64 *parts64_pick_nan(FloatParts64 *a, FloatParts64 *b,
                                      float_status *s);
static FloatParts128 *parts128_pick_nan(FloatParts128 *a, FloatParts128 *b,
                                        float_status *s);

#define parts_pick_nan(A, B, S)    PARTS_GENERIC_64_128(pick_nan, A)(A, B, S)

static FloatParts64 *parts64_pick_nan_muladd(FloatParts64 *a, FloatParts64 *b,
                                             FloatParts64 *c, float_status *s,
                                             int ab_mask, int abc_mask);
static FloatParts128 *parts128_pick_nan_muladd(FloatParts128 *a,
                                               FloatParts128 *b,
                                               FloatParts128 *c,
                                               float_status *s,
                                               int ab_mask, int abc_mask);

#define parts_pick_nan_muladd(A, B, C, S, ABM, ABCM) \
    PARTS_GENERIC_64_128(pick_nan_muladd, A)(A, B, C, S, ABM, ABCM)

static void parts64_canonicalize(FloatParts64 *p, float_status *status,
                                 const FloatFmt *fmt);
static void parts128_canonicalize(FloatParts128 *p, float_status *status,
                                  const FloatFmt *fmt);

#define parts_canonicalize(A, S, F) \
    PARTS_GENERIC_64_128(canonicalize, A)(A, S, F)

static void parts64_uncanon_normal(FloatParts64 *p, float_status *status,
                                   const FloatFmt *fmt);
static void parts128_uncanon_normal(FloatParts128 *p, float_status *status,
                                    const FloatFmt *fmt);

#define parts_uncanon_normal(A, S, F) \
    PARTS_GENERIC_64_128(uncanon_normal, A)(A, S, F)

static void parts64_uncanon(FloatParts64 *p, float_status *status,
                            const FloatFmt *fmt);
static void parts128_uncanon(FloatParts128 *p, float_status *status,
                             const FloatFmt *fmt);

#define parts_uncanon(A, S, F) \
    PARTS_GENERIC_64_128(uncanon, A)(A, S, F)

static void parts64_add_normal(FloatParts64 *a, FloatParts64 *b);
static void parts128_add_normal(FloatParts128 *a, FloatParts128 *b);
static void parts256_add_normal(FloatParts256 *a, FloatParts256 *b);

#define parts_add_normal(A, B) \
    PARTS_GENERIC_64_128_256(add_normal, A)(A, B)

static bool parts64_sub_normal(FloatParts64 *a, FloatParts64 *b);
static bool parts128_sub_normal(FloatParts128 *a, FloatParts128 *b);
static bool parts256_sub_normal(FloatParts256 *a, FloatParts256 *b);

#define parts_sub_normal(A, B) \
    PARTS_GENERIC_64_128_256(sub_normal, A)(A, B)

static FloatParts64 *parts64_addsub(FloatParts64 *a, FloatParts64 *b,
                                    float_status *s, bool subtract);
static FloatParts128 *parts128_addsub(FloatParts128 *a, FloatParts128 *b,
                                      float_status *s, bool subtract);

#define parts_addsub(A, B, S, Z) \
    PARTS_GENERIC_64_128(addsub, A)(A, B, S, Z)

static FloatParts64 *parts64_mul(FloatParts64 *a, FloatParts64 *b,
                                 float_status *s);
static FloatParts128 *parts128_mul(FloatParts128 *a, FloatParts128 *b,
                                   float_status *s);

#define parts_mul(A, B, S) \
    PARTS_GENERIC_64_128(mul, A)(A, B, S)

static FloatParts64 *parts64_muladd(FloatParts64 *a, FloatParts64 *b,
                                    FloatParts64 *c, int flags,
                                    float_status *s);
static FloatParts128 *parts128_muladd(FloatParts128 *a, FloatParts128 *b,
                                      FloatParts128 *c, int flags,
                                      float_status *s);

#define parts_muladd(A, B, C, Z, S) \
    PARTS_GENERIC_64_128(muladd, A)(A, B, C, Z, S)

static FloatParts64 *parts64_div(FloatParts64 *a, FloatParts64 *b,
                                 float_status *s);
static FloatParts128 *parts128_div(FloatParts128 *a, FloatParts128 *b,
                                   float_status *s);

#define parts_div(A, B, S) \
    PARTS_GENERIC_64_128(div, A)(A, B, S)

static void parts64_sqrt(FloatParts64 *a, float_status *s, const FloatFmt *f);
static void parts128_sqrt(FloatParts128 *a, float_status *s, const FloatFmt *f);

#define parts_sqrt(A, S, F) \
    PARTS_GENERIC_64_128(sqrt, A)(A, S, F)

static bool parts64_round_to_int_normal(FloatParts64 *a, FloatRoundMode rm,
                                        int scale, int frac_size);
static bool parts128_round_to_int_normal(FloatParts128 *a, FloatRoundMode r,
                                         int scale, int frac_size);

#define parts_round_to_int_normal(A, R, C, F) \
    PARTS_GENERIC_64_128(round_to_int_normal, A)(A, R, C, F)

static void parts64_round_to_int(FloatParts64 *a, FloatRoundMode rm,
                                 int scale, float_status *s,
                                 const FloatFmt *fmt);
static void parts128_round_to_int(FloatParts128 *a, FloatRoundMode r,
                                  int scale, float_status *s,
                                  const FloatFmt *fmt);

#define parts_round_to_int(A, R, C, S, F) \
    PARTS_GENERIC_64_128(round_to_int, A)(A, R, C, S, F)

static int64_t parts64_float_to_sint(FloatParts64 *p, FloatRoundMode rmode,
                                     int scale, int64_t min, int64_t max,
                                     float_status *s);
static int64_t parts128_float_to_sint(FloatParts128 *p, FloatRoundMode rmode,
                                     int scale, int64_t min, int64_t max,
                                     float_status *s);

#define parts_float_to_sint(P, R, Z, MN, MX, S) \
    PARTS_GENERIC_64_128(float_to_sint, P)(P, R, Z, MN, MX, S)

static uint64_t parts64_float_to_uint(FloatParts64 *p, FloatRoundMode rmode,
                                      int scale, uint64_t max,
                                      float_status *s);
static uint64_t parts128_float_to_uint(FloatParts128 *p, FloatRoundMode rmode,
                                       int scale, uint64_t max,
                                       float_status *s);

#define parts_float_to_uint(P, R, Z, M, S) \
    PARTS_GENERIC_64_128(float_to_uint, P)(P, R, Z, M, S)

static int64_t parts64_float_to_sint_modulo(FloatParts64 *p,
                                            FloatRoundMode rmode,
                                            int bitsm1, float_status *s);
static int64_t parts128_float_to_sint_modulo(FloatParts128 *p,
                                             FloatRoundMode rmode,
                                             int bitsm1, float_status *s);

#define parts_float_to_sint_modulo(P, R, M, S) \
    PARTS_GENERIC_64_128(float_to_sint_modulo, P)(P, R, M, S)

static void parts64_sint_to_float(FloatParts64 *p, int64_t a,
                                  int scale, float_status *s);
static void parts128_sint_to_float(FloatParts128 *p, int64_t a,
                                   int scale, float_status *s);

#define parts_float_to_sint(P, R, Z, MN, MX, S) \
    PARTS_GENERIC_64_128(float_to_sint, P)(P, R, Z, MN, MX, S)

#define parts_sint_to_float(P, I, Z, S) \
    PARTS_GENERIC_64_128(sint_to_float, P)(P, I, Z, S)

static void parts64_uint_to_float(FloatParts64 *p, uint64_t a,
                                  int scale, float_status *s);
static void parts128_uint_to_float(FloatParts128 *p, uint64_t a,
                                   int scale, float_status *s);

#define parts_uint_to_float(P, I, Z, S) \
    PARTS_GENERIC_64_128(uint_to_float, P)(P, I, Z, S)

static FloatParts64 *parts64_minmax(FloatParts64 *a, FloatParts64 *b,
                                    float_status *s, int flags);
static FloatParts128 *parts128_minmax(FloatParts128 *a, FloatParts128 *b,
                                      float_status *s, int flags);

#define parts_minmax(A, B, S, F) \
    PARTS_GENERIC_64_128(minmax, A)(A, B, S, F)

static FloatRelation parts64_compare(FloatParts64 *a, FloatParts64 *b,
                                     float_status *s, bool q);
static FloatRelation parts128_compare(FloatParts128 *a, FloatParts128 *b,
                                      float_status *s, bool q);

#define parts_compare(A, B, S, Q) \
    PARTS_GENERIC_64_128(compare, A)(A, B, S, Q)

static void parts64_scalbn(FloatParts64 *a, int n, float_status *s);
static void parts128_scalbn(FloatParts128 *a, int n, float_status *s);

#define parts_scalbn(A, N, S) \
    PARTS_GENERIC_64_128(scalbn, A)(A, N, S)

/*
 * Helper functions for softfloat-parts.c.inc, per-size operations.
 */

#define FRAC_GENERIC_64_128(NAME, P) \
    _Generic((P), FloatParts64 *: frac64_##NAME, \
                  FloatParts128 *: frac128_##NAME)

#define FRAC_GENERIC_64_128_256(NAME, P) \
    _Generic((P), FloatParts64 *: frac64_##NAME, \
                  FloatParts128 *: frac128_##NAME, \
                  FloatParts256 *: frac256_##NAME)

static bool frac64_add(FloatParts64 *r, FloatParts64 *a, FloatParts64 *b)
{
    return uadd64_overflow(a->frac, b->frac, &r->frac);
}

static bool frac128_add(FloatParts128 *r, FloatParts128 *a, FloatParts128 *b)
{
    bool c = 0;
    r->frac_lo = uadd64_carry(a->frac_lo, b->frac_lo, &c);
    r->frac_hi = uadd64_carry(a->frac_hi, b->frac_hi, &c);
    return c;
}

static bool frac256_add(FloatParts256 *r, FloatParts256 *a, FloatParts256 *b)
{
    bool c = 0;
    r->frac_lo = uadd64_carry(a->frac_lo, b->frac_lo, &c);
    r->frac_lm = uadd64_carry(a->frac_lm, b->frac_lm, &c);
    r->frac_hm = uadd64_carry(a->frac_hm, b->frac_hm, &c);
    r->frac_hi = uadd64_carry(a->frac_hi, b->frac_hi, &c);
    return c;
}

#define frac_add(R, A, B)  FRAC_GENERIC_64_128_256(add, R)(R, A, B)

static bool frac64_addi(FloatParts64 *r, FloatParts64 *a, uint64_t c)
{
    return uadd64_overflow(a->frac, c, &r->frac);
}

static bool frac128_addi(FloatParts128 *r, FloatParts128 *a, uint64_t c)
{
    c = uadd64_overflow(a->frac_lo, c, &r->frac_lo);
    return uadd64_overflow(a->frac_hi, c, &r->frac_hi);
}

#define frac_addi(R, A, C)  FRAC_GENERIC_64_128(addi, R)(R, A, C)

static void frac64_allones(FloatParts64 *a)
{
    a->frac = -1;
}

static void frac128_allones(FloatParts128 *a)
{
    a->frac_hi = a->frac_lo = -1;
}

#define frac_allones(A)  FRAC_GENERIC_64_128(allones, A)(A)

static FloatRelation frac64_cmp(FloatParts64 *a, FloatParts64 *b)
{
    return (a->frac == b->frac ? float_relation_equal
            : a->frac < b->frac ? float_relation_less
            : float_relation_greater);
}

static FloatRelation frac128_cmp(FloatParts128 *a, FloatParts128 *b)
{
    uint64_t ta = a->frac_hi, tb = b->frac_hi;
    if (ta == tb) {
        ta = a->frac_lo, tb = b->frac_lo;
        if (ta == tb) {
            return float_relation_equal;
        }
    }
    return ta < tb ? float_relation_less : float_relation_greater;
}

#define frac_cmp(A, B)  FRAC_GENERIC_64_128(cmp, A)(A, B)

static void frac64_clear(FloatParts64 *a)
{
    a->frac = 0;
}

static void frac128_clear(FloatParts128 *a)
{
    a->frac_hi = a->frac_lo = 0;
}

#define frac_clear(A)  FRAC_GENERIC_64_128(clear, A)(A)

static bool frac64_div(FloatParts64 *a, FloatParts64 *b)
{
    uint64_t n1, n0, r, q;
    bool ret;

    /*
     * We want a 2*N / N-bit division to produce exactly an N-bit
     * result, so that we do not lose any precision and so that we
     * do not have to renormalize afterward.  If A.frac < B.frac,
     * then division would produce an (N-1)-bit result; shift A left
     * by one to produce the an N-bit result, and return true to
     * decrement the exponent to match.
     *
     * The udiv_qrnnd algorithm that we're using requires normalization,
     * i.e. the msb of the denominator must be set, which is already true.
     */
    ret = a->frac < b->frac;
    if (ret) {
        n0 = a->frac;
        n1 = 0;
    } else {
        n0 = a->frac >> 1;
        n1 = a->frac << 63;
    }
    q = udiv_qrnnd(&r, n0, n1, b->frac);

    /* Set lsb if there is a remainder, to set inexact. */
    a->frac = q | (r != 0);

    return ret;
}

static bool frac128_div(FloatParts128 *a, FloatParts128 *b)
{
    uint64_t q0, q1, a0, a1, b0, b1;
    uint64_t r0, r1, r2, r3, t0, t1, t2, t3;
    bool ret = false;

    a0 = a->frac_hi, a1 = a->frac_lo;
    b0 = b->frac_hi, b1 = b->frac_lo;

    ret = lt128(a0, a1, b0, b1);
    if (!ret) {
        a1 = shr_double(a0, a1, 1);
        a0 = a0 >> 1;
    }

    /* Use 128/64 -> 64 division as estimate for 192/128 -> 128 division. */
    q0 = estimateDiv128To64(a0, a1, b0);

    /*
     * Estimate is high because B1 was not included (unless B1 == 0).
     * Reduce quotient and increase remainder until remainder is non-negative.
     * This loop will execute 0 to 2 times.
     */
    mul128By64To192(b0, b1, q0, &t0, &t1, &t2);
    sub192(a0, a1, 0, t0, t1, t2, &r0, &r1, &r2);
    while (r0 != 0) {
        q0--;
        add192(r0, r1, r2, 0, b0, b1, &r0, &r1, &r2);
    }

    /* Repeat using the remainder, producing a second word of quotient. */
    q1 = estimateDiv128To64(r1, r2, b0);
    mul128By64To192(b0, b1, q1, &t1, &t2, &t3);
    sub192(r1, r2, 0, t1, t2, t3, &r1, &r2, &r3);
    while (r1 != 0) {
        q1--;
        add192(r1, r2, r3, 0, b0, b1, &r1, &r2, &r3);
    }

    /* Any remainder indicates inexact; set sticky bit. */
    q1 |= (r2 | r3) != 0;

    a->frac_hi = q0;
    a->frac_lo = q1;
    return ret;
}

#define frac_div(A, B)  FRAC_GENERIC_64_128(div, A)(A, B)

static bool frac64_eqz(FloatParts64 *a)
{
    return a->frac == 0;
}

static bool frac128_eqz(FloatParts128 *a)
{
    return (a->frac_hi | a->frac_lo) == 0;
}

#define frac_eqz(A)  FRAC_GENERIC_64_128(eqz, A)(A)

static void frac64_mulw(FloatParts128 *r, FloatParts64 *a, FloatParts64 *b)
{
    mulu64(&r->frac_lo, &r->frac_hi, a->frac, b->frac);
}

static void frac128_mulw(FloatParts256 *r, FloatParts128 *a, FloatParts128 *b)
{
    mul128To256(a->frac_hi, a->frac_lo, b->frac_hi, b->frac_lo,
                &r->frac_hi, &r->frac_hm, &r->frac_lm, &r->frac_lo);
}

#define frac_mulw(R, A, B)  FRAC_GENERIC_64_128(mulw, A)(R, A, B)

static void frac64_neg(FloatParts64 *a)
{
    a->frac = -a->frac;
}

static void frac128_neg(FloatParts128 *a)
{
    bool c = 0;
    a->frac_lo = usub64_borrow(0, a->frac_lo, &c);
    a->frac_hi = usub64_borrow(0, a->frac_hi, &c);
}

static void frac256_neg(FloatParts256 *a)
{
    bool c = 0;
    a->frac_lo = usub64_borrow(0, a->frac_lo, &c);
    a->frac_lm = usub64_borrow(0, a->frac_lm, &c);
    a->frac_hm = usub64_borrow(0, a->frac_hm, &c);
    a->frac_hi = usub64_borrow(0, a->frac_hi, &c);
}

#define frac_neg(A)  FRAC_GENERIC_64_128_256(neg, A)(A)

static int frac64_normalize(FloatParts64 *a)
{
    if (a->frac) {
        int shift = clz64(a->frac);
        a->frac <<= shift;
        return shift;
    }
    return 64;
}

static int frac128_normalize(FloatParts128 *a)
{
    if (a->frac_hi) {
        int shl = clz64(a->frac_hi);
        a->frac_hi = shl_double(a->frac_hi, a->frac_lo, shl);
        a->frac_lo <<= shl;
        return shl;
    } else if (a->frac_lo) {
        int shl = clz64(a->frac_lo);
        a->frac_hi = a->frac_lo << shl;
        a->frac_lo = 0;
        return shl + 64;
    }
    return 128;
}

static int frac256_normalize(FloatParts256 *a)
{
    uint64_t a0 = a->frac_hi, a1 = a->frac_hm;
    uint64_t a2 = a->frac_lm, a3 = a->frac_lo;
    int ret, shl;

    if (likely(a0)) {
        shl = clz64(a0);
        if (shl == 0) {
            return 0;
        }
        ret = shl;
    } else {
        if (a1) {
            ret = 64;
            a0 = a1, a1 = a2, a2 = a3, a3 = 0;
        } else if (a2) {
            ret = 128;
            a0 = a2, a1 = a3, a2 = 0, a3 = 0;
        } else if (a3) {
            ret = 192;
            a0 = a3, a1 = 0, a2 = 0, a3 = 0;
        } else {
            ret = 256;
            a0 = 0, a1 = 0, a2 = 0, a3 = 0;
            goto done;
        }
        shl = clz64(a0);
        if (shl == 0) {
            goto done;
        }
        ret += shl;
    }

    a0 = shl_double(a0, a1, shl);
    a1 = shl_double(a1, a2, shl);
    a2 = shl_double(a2, a3, shl);
    a3 <<= shl;

 done:
    a->frac_hi = a0;
    a->frac_hm = a1;
    a->frac_lm = a2;
    a->frac_lo = a3;
    return ret;
}

#define frac_normalize(A)  FRAC_GENERIC_64_128_256(normalize, A)(A)

static void frac64_shl(FloatParts64 *a, int c)
{
    a->frac <<= c;
}

static void frac128_shl(FloatParts128 *a, int c)
{
    uint64_t a0 = a->frac_hi, a1 = a->frac_lo;

    if (c & 64) {
        a0 = a1, a1 = 0;
    }

    c &= 63;
    if (c) {
        a0 = shl_double(a0, a1, c);
        a1 = a1 << c;
    }

    a->frac_hi = a0;
    a->frac_lo = a1;
}

#define frac_shl(A, C)  FRAC_GENERIC_64_128(shl, A)(A, C)

static void frac64_shr(FloatParts64 *a, int c)
{
    a->frac >>= c;
}

static void frac128_shr(FloatParts128 *a, int c)
{
    uint64_t a0 = a->frac_hi, a1 = a->frac_lo;

    if (c & 64) {
        a1 = a0, a0 = 0;
    }

    c &= 63;
    if (c) {
        a1 = shr_double(a0, a1, c);
        a0 = a0 >> c;
    }

    a->frac_hi = a0;
    a->frac_lo = a1;
}

#define frac_shr(A, C)  FRAC_GENERIC_64_128(shr, A)(A, C)

static void frac64_shrjam(FloatParts64 *a, int c)
{
    uint64_t a0 = a->frac;

    if (likely(c != 0)) {
        if (likely(c < 64)) {
            a0 = (a0 >> c) | (shr_double(a0, 0, c) != 0);
        } else {
            a0 = a0 != 0;
        }
        a->frac = a0;
    }
}

static void frac128_shrjam(FloatParts128 *a, int c)
{
    uint64_t a0 = a->frac_hi, a1 = a->frac_lo;
    uint64_t sticky = 0;

    if (unlikely(c == 0)) {
        return;
    } else if (likely(c < 64)) {
        /* nothing */
    } else if (likely(c < 128)) {
        sticky = a1;
        a1 = a0;
        a0 = 0;
        c &= 63;
        if (c == 0) {
            goto done;
        }
    } else {
        sticky = a0 | a1;
        a0 = a1 = 0;
        goto done;
    }

    sticky |= shr_double(a1, 0, c);
    a1 = shr_double(a0, a1, c);
    a0 = a0 >> c;

 done:
    a->frac_lo = a1 | (sticky != 0);
    a->frac_hi = a0;
}

static void frac256_shrjam(FloatParts256 *a, int c)
{
    uint64_t a0 = a->frac_hi, a1 = a->frac_hm;
    uint64_t a2 = a->frac_lm, a3 = a->frac_lo;
    uint64_t sticky = 0;

    if (unlikely(c == 0)) {
        return;
    } else if (likely(c < 64)) {
        /* nothing */
    } else if (likely(c < 256)) {
        if (unlikely(c & 128)) {
            sticky |= a2 | a3;
            a3 = a1, a2 = a0, a1 = 0, a0 = 0;
        }
        if (unlikely(c & 64)) {
            sticky |= a3;
            a3 = a2, a2 = a1, a1 = a0, a0 = 0;
        }
        c &= 63;
        if (c == 0) {
            goto done;
        }
    } else {
        sticky = a0 | a1 | a2 | a3;
        a0 = a1 = a2 = a3 = 0;
        goto done;
    }

    sticky |= shr_double(a3, 0, c);
    a3 = shr_double(a2, a3, c);
    a2 = shr_double(a1, a2, c);
    a1 = shr_double(a0, a1, c);
    a0 = a0 >> c;

 done:
    a->frac_lo = a3 | (sticky != 0);
    a->frac_lm = a2;
    a->frac_hm = a1;
    a->frac_hi = a0;
}

#define frac_shrjam(A, C)  FRAC_GENERIC_64_128_256(shrjam, A)(A, C)

static bool frac64_sub(FloatParts64 *r, FloatParts64 *a, FloatParts64 *b)
{
    return usub64_overflow(a->frac, b->frac, &r->frac);
}

static bool frac128_sub(FloatParts128 *r, FloatParts128 *a, FloatParts128 *b)
{
    bool c = 0;
    r->frac_lo = usub64_borrow(a->frac_lo, b->frac_lo, &c);
    r->frac_hi = usub64_borrow(a->frac_hi, b->frac_hi, &c);
    return c;
}

static bool frac256_sub(FloatParts256 *r, FloatParts256 *a, FloatParts256 *b)
{
    bool c = 0;
    r->frac_lo = usub64_borrow(a->frac_lo, b->frac_lo, &c);
    r->frac_lm = usub64_borrow(a->frac_lm, b->frac_lm, &c);
    r->frac_hm = usub64_borrow(a->frac_hm, b->frac_hm, &c);
    r->frac_hi = usub64_borrow(a->frac_hi, b->frac_hi, &c);
    return c;
}

#define frac_sub(R, A, B)  FRAC_GENERIC_64_128_256(sub, R)(R, A, B)

static void frac64_truncjam(FloatParts64 *r, FloatParts128 *a)
{
    r->frac = a->frac_hi | (a->frac_lo != 0);
}

static void frac128_truncjam(FloatParts128 *r, FloatParts256 *a)
{
    r->frac_hi = a->frac_hi;
    r->frac_lo = a->frac_hm | ((a->frac_lm | a->frac_lo) != 0);
}

#define frac_truncjam(R, A)  FRAC_GENERIC_64_128(truncjam, R)(R, A)

static void frac64_widen(FloatParts128 *r, FloatParts64 *a)
{
    r->frac_hi = a->frac;
    r->frac_lo = 0;
}

static void frac128_widen(FloatParts256 *r, FloatParts128 *a)
{
    r->frac_hi = a->frac_hi;
    r->frac_hm = a->frac_lo;
    r->frac_lm = 0;
    r->frac_lo = 0;
}

#define frac_widen(A, B)  FRAC_GENERIC_64_128(widen, B)(A, B)

/*
 * Reciprocal sqrt table.  1 bit of exponent, 6-bits of mantessa.
 * From https://git.musl-libc.org/cgit/musl/tree/src/math/sqrt_data.c
 * and thus MIT licenced.
 */
static const uint16_t rsqrt_tab[128] = {
    0xb451, 0xb2f0, 0xb196, 0xb044, 0xaef9, 0xadb6, 0xac79, 0xab43,
    0xaa14, 0xa8eb, 0xa7c8, 0xa6aa, 0xa592, 0xa480, 0xa373, 0xa26b,
    0xa168, 0xa06a, 0x9f70, 0x9e7b, 0x9d8a, 0x9c9d, 0x9bb5, 0x9ad1,
    0x99f0, 0x9913, 0x983a, 0x9765, 0x9693, 0x95c4, 0x94f8, 0x9430,
    0x936b, 0x92a9, 0x91ea, 0x912e, 0x9075, 0x8fbe, 0x8f0a, 0x8e59,
    0x8daa, 0x8cfe, 0x8c54, 0x8bac, 0x8b07, 0x8a64, 0x89c4, 0x8925,
    0x8889, 0x87ee, 0x8756, 0x86c0, 0x862b, 0x8599, 0x8508, 0x8479,
    0x83ec, 0x8361, 0x82d8, 0x8250, 0x81c9, 0x8145, 0x80c2, 0x8040,
    0xff02, 0xfd0e, 0xfb25, 0xf947, 0xf773, 0xf5aa, 0xf3ea, 0xf234,
    0xf087, 0xeee3, 0xed47, 0xebb3, 0xea27, 0xe8a3, 0xe727, 0xe5b2,
    0xe443, 0xe2dc, 0xe17a, 0xe020, 0xdecb, 0xdd7d, 0xdc34, 0xdaf1,
    0xd9b3, 0xd87b, 0xd748, 0xd61a, 0xd4f1, 0xd3cd, 0xd2ad, 0xd192,
    0xd07b, 0xcf69, 0xce5b, 0xcd51, 0xcc4a, 0xcb48, 0xca4a, 0xc94f,
    0xc858, 0xc764, 0xc674, 0xc587, 0xc49d, 0xc3b7, 0xc2d4, 0xc1f4,
    0xc116, 0xc03c, 0xbf65, 0xbe90, 0xbdbe, 0xbcef, 0xbc23, 0xbb59,
    0xba91, 0xb9cc, 0xb90a, 0xb84a, 0xb78c, 0xb6d0, 0xb617, 0xb560,
};

#define partsN(NAME)   glue(glue(glue(parts,N),_),NAME)
#define FloatPartsN    glue(FloatParts,N)
#define FloatPartsW    glue(FloatParts,W)

#define N 64
#define W 128

#include "softfloat-parts-addsub.c.inc"
#include "softfloat-parts.c.inc"

#undef  N
#undef  W
#define N 128
#define W 256

#include "softfloat-parts-addsub.c.inc"
#include "softfloat-parts.c.inc"

#undef  N
#undef  W
#define N            256

#include "softfloat-parts-addsub.c.inc"

#undef  N
#undef  W
#undef  partsN
#undef  FloatPartsN
#undef  FloatPartsW

/*
 * Pack/unpack routines with a specific FloatFmt.
 */

static void float16a_unpack_canonical(FloatParts64 *p, float16 f,
                                      float_status *s, const FloatFmt *params)
{
    float16_unpack_raw(p, f);
    parts_canonicalize(p, s, params);
}

static void float16_unpack_canonical(FloatParts64 *p, float16 f,
                                     float_status *s)
{
    float16a_unpack_canonical(p, f, s, &float16_params);
}

static void bfloat16_unpack_canonical(FloatParts64 *p, bfloat16 f,
                                      float_status *s)
{
    bfloat16_unpack_raw(p, f);
    parts_canonicalize(p, s, &bfloat16_params);
}

static float16 float16a_round_pack_canonical(FloatParts64 *p,
                                             float_status *s,
                                             const FloatFmt *params)
{
    parts_uncanon(p, s, params);
    return float16_pack_raw(p);
}

static float16 float16_round_pack_canonical(FloatParts64 *p,
                                            float_status *s)
{
    return float16a_round_pack_canonical(p, s, &float16_params);
}

static bfloat16 bfloat16_round_pack_canonical(FloatParts64 *p,
                                              float_status *s)
{
    parts_uncanon(p, s, &bfloat16_params);
    return bfloat16_pack_raw(p);
}

static void float32_unpack_canonical(FloatParts64 *p, float32 f,
                                     float_status *s)
{
    float32_unpack_raw(p, f);
    parts_canonicalize(p, s, &float32_params);
}

static float32 float32_round_pack_canonical(FloatParts64 *p,
                                            float_status *s)
{
    parts_uncanon(p, s, &float32_params);
    return float32_pack_raw(p);
}

static void float64_unpack_canonical(FloatParts64 *p, float64 f,
                                     float_status *s)
{
    float64_unpack_raw(p, f);
    parts_canonicalize(p, s, &float64_params);
}

static float64 float64_round_pack_canonical(FloatParts64 *p,
                                            float_status *s)
{
    parts_uncanon(p, s, &float64_params);
    return float64_pack_raw(p);
}

static float64 float64r32_round_pack_canonical(FloatParts64 *p,
                                               float_status *s)
{
    parts_uncanon(p, s, &float32_params);

    /*
     * In parts_uncanon, we placed the fraction for float32 at the lsb.
     * We need to adjust the fraction higher so that the least N bits are
     * zero, and the fraction is adjacent to the float64 implicit bit.
     */
    switch (p->cls) {
    case float_class_normal:
        if (unlikely(p->exp == 0)) {
            /*
             * The result is denormal for float32, but can be represented
             * in normalized form for float64.  Adjust, per canonicalize.
             */
            int shift = frac_normalize(p);
            p->exp = (float32_params.frac_shift -
                      float32_params.exp_bias - shift + 1 +
                      float64_params.exp_bias);
            frac_shr(p, float64_params.frac_shift);
        } else {
            frac_shl(p, float32_params.frac_shift - float64_params.frac_shift);
            p->exp += float64_params.exp_bias - float32_params.exp_bias;
        }
        break;
    case float_class_snan:
    case float_class_qnan:
        frac_shl(p, float32_params.frac_shift - float64_params.frac_shift);
        p->exp = float64_params.exp_max;
        break;
    case float_class_inf:
        p->exp = float64_params.exp_max;
        break;
    case float_class_zero:
        break;
    default:
        g_assert_not_reached();
    }

    return float64_pack_raw(p);
}

static void float128_unpack_canonical(FloatParts128 *p, float128 f,
                                      float_status *s)
{
    float128_unpack_raw(p, f);
    parts_canonicalize(p, s, &float128_params);
}

static float128 float128_round_pack_canonical(FloatParts128 *p,
                                              float_status *s)
{
    parts_uncanon(p, s, &float128_params);
    return float128_pack_raw(p);
}

/*
 * Addition and subtraction
 */

static float16 QEMU_FLATTEN
float16_addsub(float16 a, float16 b, float_status *status, bool subtract)
{
    FloatParts64 pa, pb, *pr;

    float16_unpack_canonical(&pa, a, status);
    float16_unpack_canonical(&pb, b, status);
    pr = parts_addsub(&pa, &pb, status, subtract);

    return float16_round_pack_canonical(pr, status);
}

float16 float16_add(float16 a, float16 b, float_status *status)
{
    return float16_addsub(a, b, status, false);
}

float16 float16_sub(float16 a, float16 b, float_status *status)
{
    return float16_addsub(a, b, status, true);
}

static float32 QEMU_SOFTFLOAT_ATTR
soft_f32_addsub(float32 a, float32 b, float_status *status, bool subtract)
{
    FloatParts64 pa, pb, *pr;

    float32_unpack_canonical(&pa, a, status);
    float32_unpack_canonical(&pb, b, status);
    pr = parts_addsub(&pa, &pb, status, subtract);

    return float32_round_pack_canonical(pr, status);
}

static float32 soft_f32_add(float32 a, float32 b, float_status *status)
{
    return soft_f32_addsub(a, b, status, false);
}

static float32 soft_f32_sub(float32 a, float32 b, float_status *status)
{
    return soft_f32_addsub(a, b, status, true);
}

static float64 QEMU_SOFTFLOAT_ATTR
soft_f64_addsub(float64 a, float64 b, float_status *status, bool subtract)
{
    FloatParts64 pa, pb, *pr;

    float64_unpack_canonical(&pa, a, status);
    float64_unpack_canonical(&pb, b, status);
    pr = parts_addsub(&pa, &pb, status, subtract);

    return float64_round_pack_canonical(pr, status);
}

static float64 soft_f64_add(float64 a, float64 b, float_status *status)
{
    return soft_f64_addsub(a, b, status, false);
}

static float64 soft_f64_sub(float64 a, float64 b, float_status *status)
{
    return soft_f64_addsub(a, b, status, true);
}

static float hard_f32_add(float a, float b)
{
    return a + b;
}

static float hard_f32_sub(float a, float b)
{
    return a - b;
}

static double hard_f64_add(double a, double b)
{
    return a + b;
}

static double hard_f64_sub(double a, double b)
{
    return a - b;
}

static bool f32_addsubmul_post(union_float32 a, union_float32 b)
{
    if (QEMU_HARDFLOAT_2F32_USE_FP) {
        return !(fpclassify(a.h) == FP_ZERO && fpclassify(b.h) == FP_ZERO);
    }
    return !(float32_is_zero(a.s) && float32_is_zero(b.s));
}

static bool f64_addsubmul_post(union_float64 a, union_float64 b)
{
    if (QEMU_HARDFLOAT_2F64_USE_FP) {
        return !(fpclassify(a.h) == FP_ZERO && fpclassify(b.h) == FP_ZERO);
    } else {
        return !(float64_is_zero(a.s) && float64_is_zero(b.s));
    }
}

static float32 float32_addsub(float32 a, float32 b, float_status *s,
                              hard_f32_op2_fn hard, soft_f32_op2_fn soft)
{
    return float32_gen2(a, b, s, hard, soft,
                        f32_is_zon2, f32_addsubmul_post);
}

static float64 float64_addsub(float64 a, float64 b, float_status *s,
                              hard_f64_op2_fn hard, soft_f64_op2_fn soft)
{
    return float64_gen2(a, b, s, hard, soft,
                        f64_is_zon2, f64_addsubmul_post);
}

float32 QEMU_FLATTEN
float32_add(float32 a, float32 b, float_status *s)
{
    return float32_addsub(a, b, s, hard_f32_add, soft_f32_add);
}

float32 QEMU_FLATTEN
float32_sub(float32 a, float32 b, float_status *s)
{
    return float32_addsub(a, b, s, hard_f32_sub, soft_f32_sub);
}

float64 QEMU_FLATTEN
float64_add(float64 a, float64 b, float_status *s)
{
    return float64_addsub(a, b, s, hard_f64_add, soft_f64_add);
}

float64 QEMU_FLATTEN
float64_sub(float64 a, float64 b, float_status *s)
{
    return float64_addsub(a, b, s, hard_f64_sub, soft_f64_sub);
}

static float64 float64r32_addsub(float64 a, float64 b, float_status *status,
                                 bool subtract)
{
    FloatParts64 pa, pb, *pr;

    float64_unpack_canonical(&pa, a, status);
    float64_unpack_canonical(&pb, b, status);
    pr = parts_addsub(&pa, &pb, status, subtract);

    return float64r32_round_pack_canonical(pr, status);
}

float64 float64r32_add(float64 a, float64 b, float_status *status)
{
    return float64r32_addsub(a, b, status, false);
}

float64 float64r32_sub(float64 a, float64 b, float_status *status)
{
    return float64r32_addsub(a, b, status, true);
}

static bfloat16 QEMU_FLATTEN
bfloat16_addsub(bfloat16 a, bfloat16 b, float_status *status, bool subtract)
{
    FloatParts64 pa, pb, *pr;

    bfloat16_unpack_canonical(&pa, a, status);
    bfloat16_unpack_canonical(&pb, b, status);
    pr = parts_addsub(&pa, &pb, status, subtract);

    return bfloat16_round_pack_canonical(pr, status);
}

bfloat16 bfloat16_add(bfloat16 a, bfloat16 b, float_status *status)
{
    return bfloat16_addsub(a, b, status, false);
}

bfloat16 bfloat16_sub(bfloat16 a, bfloat16 b, float_status *status)
{
    return bfloat16_addsub(a, b, status, true);
}

static float128 QEMU_FLATTEN
float128_addsub(float128 a, float128 b, float_status *status, bool subtract)
{
    FloatParts128 pa, pb, *pr;

    float128_unpack_canonical(&pa, a, status);
    float128_unpack_canonical(&pb, b, status);
    pr = parts_addsub(&pa, &pb, status, subtract);

    return float128_round_pack_canonical(pr, status);
}

float128 float128_add(float128 a, float128 b, float_status *status)
{
    return float128_addsub(a, b, status, false);
}

float128 float128_sub(float128 a, float128 b, float_status *status)
{
    return float128_addsub(a, b, status, true);
}

/*
 * Multiplication
 */

float16 QEMU_FLATTEN float16_mul(float16 a, float16 b, float_status *status)
{
    FloatParts64 pa, pb, *pr;

    float16_unpack_canonical(&pa, a, status);
    float16_unpack_canonical(&pb, b, status);
    pr = parts_mul(&pa, &pb, status);

    return float16_round_pack_canonical(pr, status);
}

static float32 QEMU_SOFTFLOAT_ATTR
soft_f32_mul(float32 a, float32 b, float_status *status)
{
    FloatParts64 pa, pb, *pr;

    float32_unpack_canonical(&pa, a, status);
    float32_unpack_canonical(&pb, b, status);
    pr = parts_mul(&pa, &pb, status);

    return float32_round_pack_canonical(pr, status);
}

static float64 QEMU_SOFTFLOAT_ATTR
soft_f64_mul(float64 a, float64 b, float_status *status)
{
    FloatParts64 pa, pb, *pr;

    float64_unpack_canonical(&pa, a, status);
    float64_unpack_canonical(&pb, b, status);
    pr = parts_mul(&pa, &pb, status);

    return float64_round_pack_canonical(pr, status);
}

static float hard_f32_mul(float a, float b)
{
    return a * b;
}

static double hard_f64_mul(double a, double b)
{
    return a * b;
}

float32 QEMU_FLATTEN
float32_mul(float32 a, float32 b, float_status *s)
{
    return float32_gen2(a, b, s, hard_f32_mul, soft_f32_mul,
                        f32_is_zon2, f32_addsubmul_post);
}

float64 QEMU_FLATTEN
float64_mul(float64 a, float64 b, float_status *s)
{
    return float64_gen2(a, b, s, hard_f64_mul, soft_f64_mul,
                        f64_is_zon2, f64_addsubmul_post);
}

float64 float64r32_mul(float64 a, float64 b, float_status *status)
{
    FloatParts64 pa, pb, *pr;

    float64_unpack_canonical(&pa, a, status);
    float64_unpack_canonical(&pb, b, status);
    pr = parts_mul(&pa, &pb, status);

    return float64r32_round_pack_canonical(pr, status);
}

bfloat16 QEMU_FLATTEN
bfloat16_mul(bfloat16 a, bfloat16 b, float_status *status)
{
    FloatParts64 pa, pb, *pr;

    bfloat16_unpack_canonical(&pa, a, status);
    bfloat16_unpack_canonical(&pb, b, status);
    pr = parts_mul(&pa, &pb, status);

    return bfloat16_round_pack_canonical(pr, status);
}

float128 QEMU_FLATTEN
float128_mul(float128 a, float128 b, float_status *status)
{
    FloatParts128 pa, pb, *pr;

    float128_unpack_canonical(&pa, a, status);
    float128_unpack_canonical(&pb, b, status);
    pr = parts_mul(&pa, &pb, status);

    return float128_round_pack_canonical(pr, status);
}

/*
 * Fused multiply-add
 */

float16 QEMU_FLATTEN float16_muladd(float16 a, float16 b, float16 c,
                                    int flags, float_status *status)
{
    FloatParts64 pa, pb, pc, *pr;

    float16_unpack_canonical(&pa, a, status);
    float16_unpack_canonical(&pb, b, status);
    float16_unpack_canonical(&pc, c, status);
    pr = parts_muladd(&pa, &pb, &pc, flags, status);

    return float16_round_pack_canonical(pr, status);
}

static float32 QEMU_SOFTFLOAT_ATTR
soft_f32_muladd(float32 a, float32 b, float32 c, int flags,
                float_status *status)
{
    FloatParts64 pa, pb, pc, *pr;

    float32_unpack_canonical(&pa, a, status);
    float32_unpack_canonical(&pb, b, status);
    float32_unpack_canonical(&pc, c, status);
    pr = parts_muladd(&pa, &pb, &pc, flags, status);

    return float32_round_pack_canonical(pr, status);
}

static float64 QEMU_SOFTFLOAT_ATTR
soft_f64_muladd(float64 a, float64 b, float64 c, int flags,
                float_status *status)
{
    FloatParts64 pa, pb, pc, *pr;

    float64_unpack_canonical(&pa, a, status);
    float64_unpack_canonical(&pb, b, status);
    float64_unpack_canonical(&pc, c, status);
    pr = parts_muladd(&pa, &pb, &pc, flags, status);

    return float64_round_pack_canonical(pr, status);
}

static bool force_soft_fma;

float32 QEMU_FLATTEN
float32_muladd(float32 xa, float32 xb, float32 xc, int flags, float_status *s)
{
    union_float32 ua, ub, uc, ur;

    ua.s = xa;
    ub.s = xb;
    uc.s = xc;

    if (unlikely(!can_use_fpu(s))) {
        goto soft;
    }
    if (unlikely(flags & float_muladd_halve_result)) {
        goto soft;
    }

    float32_input_flush3(&ua.s, &ub.s, &uc.s, s);
    if (unlikely(!f32_is_zon3(ua, ub, uc))) {
        goto soft;
    }

    if (unlikely(force_soft_fma)) {
        goto soft;
    }

    /*
     * When (a || b) == 0, there's no need to check for under/over flow,
     * since we know the addend is (normal || 0) and the product is 0.
     */
    if (float32_is_zero(ua.s) || float32_is_zero(ub.s)) {
        union_float32 up;
        bool prod_sign;

        prod_sign = float32_is_neg(ua.s) ^ float32_is_neg(ub.s);
        prod_sign ^= !!(flags & float_muladd_negate_product);
        up.s = float32_set_sign(float32_zero, prod_sign);

        if (flags & float_muladd_negate_c) {
            uc.h = -uc.h;
        }
        ur.h = up.h + uc.h;
    } else {
        union_float32 ua_orig = ua;
        union_float32 uc_orig = uc;

        if (flags & float_muladd_negate_product) {
            ua.h = -ua.h;
        }
        if (flags & float_muladd_negate_c) {
            uc.h = -uc.h;
        }

        ur.h = fmaf(ua.h, ub.h, uc.h);

        if (unlikely(f32_is_inf(ur))) {
            float_raise(float_flag_overflow, s);
        } else if (unlikely(fabsf(ur.h) <= FLT_MIN)) {
            ua = ua_orig;
            uc = uc_orig;
            goto soft;
        }
    }
    if (flags & float_muladd_negate_result) {
        return float32_chs(ur.s);
    }
    return ur.s;

 soft:
    return soft_f32_muladd(ua.s, ub.s, uc.s, flags, s);
}

float64 QEMU_FLATTEN
float64_muladd(float64 xa, float64 xb, float64 xc, int flags, float_status *s)
{
    union_float64 ua, ub, uc, ur;

    ua.s = xa;
    ub.s = xb;
    uc.s = xc;

    if (unlikely(!can_use_fpu(s))) {
        goto soft;
    }
    if (unlikely(flags & float_muladd_halve_result)) {
        goto soft;
    }

    float64_input_flush3(&ua.s, &ub.s, &uc.s, s);
    if (unlikely(!f64_is_zon3(ua, ub, uc))) {
        goto soft;
    }

    if (unlikely(force_soft_fma)) {
        goto soft;
    }

    /*
     * When (a || b) == 0, there's no need to check for under/over flow,
     * since we know the addend is (normal || 0) and the product is 0.
     */
    if (float64_is_zero(ua.s) || float64_is_zero(ub.s)) {
        union_float64 up;
        bool prod_sign;

        prod_sign = float64_is_neg(ua.s) ^ float64_is_neg(ub.s);
        prod_sign ^= !!(flags & float_muladd_negate_product);
        up.s = float64_set_sign(float64_zero, prod_sign);

        if (flags & float_muladd_negate_c) {
            uc.h = -uc.h;
        }
        ur.h = up.h + uc.h;
    } else {
        union_float64 ua_orig = ua;
        union_float64 uc_orig = uc;

        if (flags & float_muladd_negate_product) {
            ua.h = -ua.h;
        }
        if (flags & float_muladd_negate_c) {
            uc.h = -uc.h;
        }

        ur.h = fma(ua.h, ub.h, uc.h);

        if (unlikely(f64_is_inf(ur))) {
            float_raise(float_flag_overflow, s);
        } else if (unlikely(fabs(ur.h) <= FLT_MIN)) {
            ua = ua_orig;
            uc = uc_orig;
            goto soft;
        }
    }
    if (flags & float_muladd_negate_result) {
        return float64_chs(ur.s);
    }
    return ur.s;

 soft:
    return soft_f64_muladd(ua.s, ub.s, uc.s, flags, s);
}

float64 float64r32_muladd(float64 a, float64 b, float64 c,
                          int flags, float_status *status)
{
    FloatParts64 pa, pb, pc, *pr;

    float64_unpack_canonical(&pa, a, status);
    float64_unpack_canonical(&pb, b, status);
    float64_unpack_canonical(&pc, c, status);
    pr = parts_muladd(&pa, &pb, &pc, flags, status);

    return float64r32_round_pack_canonical(pr, status);
}

bfloat16 QEMU_FLATTEN bfloat16_muladd(bfloat16 a, bfloat16 b, bfloat16 c,
                                      int flags, float_status *status)
{
    FloatParts64 pa, pb, pc, *pr;

    bfloat16_unpack_canonical(&pa, a, status);
    bfloat16_unpack_canonical(&pb, b, status);
    bfloat16_unpack_canonical(&pc, c, status);
    pr = parts_muladd(&pa, &pb, &pc, flags, status);

    return bfloat16_round_pack_canonical(pr, status);
}

float128 QEMU_FLATTEN float128_muladd(float128 a, float128 b, float128 c,
                                      int flags, float_status *status)
{
    FloatParts128 pa, pb, pc, *pr;

    float128_unpack_canonical(&pa, a, status);
    float128_unpack_canonical(&pb, b, status);
    float128_unpack_canonical(&pc, c, status);
    pr = parts_muladd(&pa, &pb, &pc, flags, status);

    return float128_round_pack_canonical(pr, status);
}

/*
 * Division
 */

float16 float16_div(float16 a, float16 b, float_status *status)
{
    FloatParts64 pa, pb, *pr;

    float16_unpack_canonical(&pa, a, status);
    float16_unpack_canonical(&pb, b, status);
    pr = parts_div(&pa, &pb, status);

    return float16_round_pack_canonical(pr, status);
}

static float32 QEMU_SOFTFLOAT_ATTR
soft_f32_div(float32 a, float32 b, float_status *status)
{
    FloatParts64 pa, pb, *pr;

    float32_unpack_canonical(&pa, a, status);
    float32_unpack_canonical(&pb, b, status);
    pr = parts_div(&pa, &pb, status);

    return float32_round_pack_canonical(pr, status);
}

static float64 QEMU_SOFTFLOAT_ATTR
soft_f64_div(float64 a, float64 b, float_status *status)
{
    FloatParts64 pa, pb, *pr;

    float64_unpack_canonical(&pa, a, status);
    float64_unpack_canonical(&pb, b, status);
    pr = parts_div(&pa, &pb, status);

    return float64_round_pack_canonical(pr, status);
}

static float hard_f32_div(float a, float b)
{
    return a / b;
}

static double hard_f64_div(double a, double b)
{
    return a / b;
}

static bool f32_div_pre(union_float32 a, union_float32 b)
{
    if (QEMU_HARDFLOAT_2F32_USE_FP) {
        return (fpclassify(a.h) == FP_NORMAL || fpclassify(a.h) == FP_ZERO) &&
               fpclassify(b.h) == FP_NORMAL;
    }
    return float32_is_zero_or_normal(a.s) && float32_is_normal(b.s);
}

static bool f64_div_pre(union_float64 a, union_float64 b)
{
    if (QEMU_HARDFLOAT_2F64_USE_FP) {
        return (fpclassify(a.h) == FP_NORMAL || fpclassify(a.h) == FP_ZERO) &&
               fpclassify(b.h) == FP_NORMAL;
    }
    return float64_is_zero_or_normal(a.s) && float64_is_normal(b.s);
}

static bool f32_div_post(union_float32 a, union_float32 b)
{
    if (QEMU_HARDFLOAT_2F32_USE_FP) {
        return fpclassify(a.h) != FP_ZERO;
    }
    return !float32_is_zero(a.s);
}

static bool f64_div_post(union_float64 a, union_float64 b)
{
    if (QEMU_HARDFLOAT_2F64_USE_FP) {
        return fpclassify(a.h) != FP_ZERO;
    }
    return !float64_is_zero(a.s);
}

float32 QEMU_FLATTEN
float32_div(float32 a, float32 b, float_status *s)
{
    return float32_gen2(a, b, s, hard_f32_div, soft_f32_div,
                        f32_div_pre, f32_div_post);
}

float64 QEMU_FLATTEN
float64_div(float64 a, float64 b, float_status *s)
{
    return float64_gen2(a, b, s, hard_f64_div, soft_f64_div,
                        f64_div_pre, f64_div_post);
}

float64 float64r32_div(float64 a, float64 b, float_status *status)
{
    FloatParts64 pa, pb, *pr;

    float64_unpack_canonical(&pa, a, status);
    float64_unpack_canonical(&pb, b, status);
    pr = parts_div(&pa, &pb, status);

    return float64r32_round_pack_canonical(pr, status);
}

bfloat16 QEMU_FLATTEN
bfloat16_div(bfloat16 a, bfloat16 b, float_status *status)
{
    FloatParts64 pa, pb, *pr;

    bfloat16_unpack_canonical(&pa, a, status);
    bfloat16_unpack_canonical(&pb, b, status);
    pr = parts_div(&pa, &pb, status);

    return bfloat16_round_pack_canonical(pr, status);
}

float128 QEMU_FLATTEN
float128_div(float128 a, float128 b, float_status *status)
{
    FloatParts128 pa, pb, *pr;

    float128_unpack_canonical(&pa, a, status);
    float128_unpack_canonical(&pb, b, status);
    pr = parts_div(&pa, &pb, status);

    return float128_round_pack_canonical(pr, status);
}

/*
 * Float to Float conversions
 *
 * Returns the result of converting one float format to another. The
 * conversion is performed according to the IEC/IEEE Standard for
 * Binary Floating-Point Arithmetic.
 *
 * Usually this only needs to take care of raising invalid exceptions
 * and handling the conversion on NaNs.
 */

static void parts_float_to_ahp(FloatParts64 *a, float_status *s)
{
    switch (a->cls) {
    case float_class_snan:
        float_raise(float_flag_invalid_snan, s);
        /* fall through */
    case float_class_qnan:
        /*
         * There is no NaN in the destination format.  Raise Invalid
         * and return a zero with the sign of the input NaN.
         */
        float_raise(float_flag_invalid, s);
        a->cls = float_class_zero;
        break;

    case float_class_inf:
        /*
         * There is no Inf in the destination format.  Raise Invalid
         * and return the maximum normal with the correct sign.
         */
        float_raise(float_flag_invalid, s);
        a->cls = float_class_normal;
        a->exp = float16_params_ahp.exp_max;
        a->frac = MAKE_64BIT_MASK(float16_params_ahp.frac_shift,
                                  float16_params_ahp.frac_size + 1);
        break;

    case float_class_normal:
    case float_class_zero:
        break;

    default:
        g_assert_not_reached();
    }
}

static void parts64_float_to_float(FloatParts64 *a, float_status *s)
{
    if (is_nan(a->cls)) {
        parts_return_nan(a, s);
    }
}

static void parts128_float_to_float(FloatParts128 *a, float_status *s)
{
    if (is_nan(a->cls)) {
        parts_return_nan(a, s);
    }
}

#define parts_float_to_float(P, S) \
    PARTS_GENERIC_64_128(float_to_float, P)(P, S)

static void parts_float_to_float_narrow(FloatParts64 *a, FloatParts128 *b,
                                        float_status *s)
{
    a->cls = b->cls;
    a->sign = b->sign;
    a->exp = b->exp;

    if (a->cls == float_class_normal) {
        frac_truncjam(a, b);
    } else if (is_nan(a->cls)) {
        /* Discard the low bits of the NaN. */
        a->frac = b->frac_hi;
        parts_return_nan(a, s);
    }
}

static void parts_float_to_float_widen(FloatParts128 *a, FloatParts64 *b,
                                       float_status *s)
{
    a->cls = b->cls;
    a->sign = b->sign;
    a->exp = b->exp;
    frac_widen(a, b);

    if (is_nan(a->cls)) {
        parts_return_nan(a, s);
    }
}

float32 float16_to_float32(float16 a, bool ieee, float_status *s)
{
    const FloatFmt *fmt16 = ieee ? &float16_params : &float16_params_ahp;
    FloatParts64 p;

    float16a_unpack_canonical(&p, a, s, fmt16);
    parts_float_to_float(&p, s);
    return float32_round_pack_canonical(&p, s);
}

float64 float16_to_float64(float16 a, bool ieee, float_status *s)
{
    const FloatFmt *fmt16 = ieee ? &float16_params : &float16_params_ahp;
    FloatParts64 p;

    float16a_unpack_canonical(&p, a, s, fmt16);
    parts_float_to_float(&p, s);
    return float64_round_pack_canonical(&p, s);
}

float16 float32_to_float16(float32 a, bool ieee, float_status *s)
{
    FloatParts64 p;
    const FloatFmt *fmt;

    float32_unpack_canonical(&p, a, s);
    if (ieee) {
        parts_float_to_float(&p, s);
        fmt = &float16_params;
    } else {
        parts_float_to_ahp(&p, s);
        fmt = &float16_params_ahp;
    }
    return float16a_round_pack_canonical(&p, s, fmt);
}

static float64 QEMU_SOFTFLOAT_ATTR
soft_float32_to_float64(float32 a, float_status *s)
{
    FloatParts64 p;

    float32_unpack_canonical(&p, a, s);
    parts_float_to_float(&p, s);
    return float64_round_pack_canonical(&p, s);
}

float64 float32_to_float64(float32 a, float_status *s)
{
    if (likely(float32_is_normal(a))) {
        /* Widening conversion can never produce inexact results.  */
        union_float32 uf;
        union_float64 ud;
        uf.s = a;
        ud.h = uf.h;
        return ud.s;
    } else if (float32_is_zero(a)) {
        return float64_set_sign(float64_zero, float32_is_neg(a));
    } else {
        return soft_float32_to_float64(a, s);
    }
}

float16 float64_to_float16(float64 a, bool ieee, float_status *s)
{
    FloatParts64 p;
    const FloatFmt *fmt;

    float64_unpack_canonical(&p, a, s);
    if (ieee) {
        parts_float_to_float(&p, s);
        fmt = &float16_params;
    } else {
        parts_float_to_ahp(&p, s);
        fmt = &float16_params_ahp;
    }
    return float16a_round_pack_canonical(&p, s, fmt);
}

float32 float64_to_float32(float64 a, float_status *s)
{
    FloatParts64 p;

    float64_unpack_canonical(&p, a, s);
    parts_float_to_float(&p, s);
    return float32_round_pack_canonical(&p, s);
}

float32 bfloat16_to_float32(bfloat16 a, float_status *s)
{
    FloatParts64 p;

    bfloat16_unpack_canonical(&p, a, s);
    parts_float_to_float(&p, s);
    return float32_round_pack_canonical(&p, s);
}

float64 bfloat16_to_float64(bfloat16 a, float_status *s)
{
    FloatParts64 p;

    bfloat16_unpack_canonical(&p, a, s);
    parts_float_to_float(&p, s);
    return float64_round_pack_canonical(&p, s);
}

bfloat16 float32_to_bfloat16(float32 a, float_status *s)
{
    FloatParts64 p;

    float32_unpack_canonical(&p, a, s);
    parts_float_to_float(&p, s);
    return bfloat16_round_pack_canonical(&p, s);
}

bfloat16 float64_to_bfloat16(float64 a, float_status *s)
{
    FloatParts64 p;

    float64_unpack_canonical(&p, a, s);
    parts_float_to_float(&p, s);
    return bfloat16_round_pack_canonical(&p, s);
}

float32 float128_to_float32(float128 a, float_status *s)
{
    FloatParts64 p64;
    FloatParts128 p128;

    float128_unpack_canonical(&p128, a, s);
    parts_float_to_float_narrow(&p64, &p128, s);
    return float32_round_pack_canonical(&p64, s);
}

float64 float128_to_float64(float128 a, float_status *s)
{
    FloatParts64 p64;
    FloatParts128 p128;

    float128_unpack_canonical(&p128, a, s);
    parts_float_to_float_narrow(&p64, &p128, s);
    return float64_round_pack_canonical(&p64, s);
}

float128 float32_to_float128(float32 a, float_status *s)
{
    FloatParts64 p64;
    FloatParts128 p128;

    float32_unpack_canonical(&p64, a, s);
    parts_float_to_float_widen(&p128, &p64, s);
    return float128_round_pack_canonical(&p128, s);
}

float128 float64_to_float128(float64 a, float_status *s)
{
    FloatParts64 p64;
    FloatParts128 p128;

    float64_unpack_canonical(&p64, a, s);
    parts_float_to_float_widen(&p128, &p64, s);
    return float128_round_pack_canonical(&p128, s);
}

/*
 * Round to integral value
 */

float16 float16_round_to_int(float16 a, float_status *s)
{
    FloatParts64 p;

    float16_unpack_canonical(&p, a, s);
    parts_round_to_int(&p, s->float_rounding_mode, 0, s, &float16_params);
    return float16_round_pack_canonical(&p, s);
}

float32 float32_round_to_int(float32 a, float_status *s)
{
    FloatParts64 p;

    float32_unpack_canonical(&p, a, s);
    parts_round_to_int(&p, s->float_rounding_mode, 0, s, &float32_params);
    return float32_round_pack_canonical(&p, s);
}

float64 float64_round_to_int(float64 a, float_status *s)
{
    FloatParts64 p;

    float64_unpack_canonical(&p, a, s);
    parts_round_to_int(&p, s->float_rounding_mode, 0, s, &float64_params);
    return float64_round_pack_canonical(&p, s);
}

bfloat16 bfloat16_round_to_int(bfloat16 a, float_status *s)
{
    FloatParts64 p;

    bfloat16_unpack_canonical(&p, a, s);
    parts_round_to_int(&p, s->float_rounding_mode, 0, s, &bfloat16_params);
    return bfloat16_round_pack_canonical(&p, s);
}

float128 float128_round_to_int(float128 a, float_status *s)
{
    FloatParts128 p;

    float128_unpack_canonical(&p, a, s);
    parts_round_to_int(&p, s->float_rounding_mode, 0, s, &float128_params);
    return float128_round_pack_canonical(&p, s);
}

/*
 * Floating-point to signed integer conversions
 */

int8_t float16_to_int8_scalbn(float16 a, FloatRoundMode rmode, int scale,
                              float_status *s)
{
    FloatParts64 p;

    float16_unpack_canonical(&p, a, s);
    return parts_float_to_sint(&p, rmode, scale, INT8_MIN, INT8_MAX, s);
}

int16_t float16_to_int16_scalbn(float16 a, FloatRoundMode rmode, int scale,
                                float_status *s)
{
    FloatParts64 p;

    float16_unpack_canonical(&p, a, s);
    return parts_float_to_sint(&p, rmode, scale, INT16_MIN, INT16_MAX, s);
}

int32_t float16_to_int32_scalbn(float16 a, FloatRoundMode rmode, int scale,
                                float_status *s)
{
    FloatParts64 p;

    float16_unpack_canonical(&p, a, s);
    return parts_float_to_sint(&p, rmode, scale, INT32_MIN, INT32_MAX, s);
}

int64_t float16_to_int64_scalbn(float16 a, FloatRoundMode rmode, int scale,
                                float_status *s)
{
    FloatParts64 p;

    float16_unpack_canonical(&p, a, s);
    return parts_float_to_sint(&p, rmode, scale, INT64_MIN, INT64_MAX, s);
}

int16_t float32_to_int16_scalbn(float32 a, FloatRoundMode rmode, int scale,
                                float_status *s)
{
    FloatParts64 p;

    float32_unpack_canonical(&p, a, s);
    return parts_float_to_sint(&p, rmode, scale, INT16_MIN, INT16_MAX, s);
}

int32_t float32_to_int32_scalbn(float32 a, FloatRoundMode rmode, int scale,
                                float_status *s)
{
    FloatParts64 p;

    float32_unpack_canonical(&p, a, s);
    return parts_float_to_sint(&p, rmode, scale, INT32_MIN, INT32_MAX, s);
}

int64_t float32_to_int64_scalbn(float32 a, FloatRoundMode rmode, int scale,
                                float_status *s)
{
    FloatParts64 p;

    float32_unpack_canonical(&p, a, s);
    return parts_float_to_sint(&p, rmode, scale, INT64_MIN, INT64_MAX, s);
}

int16_t float64_to_int16_scalbn(float64 a, FloatRoundMode rmode, int scale,
                                float_status *s)
{
    FloatParts64 p;

    float64_unpack_canonical(&p, a, s);
    return parts_float_to_sint(&p, rmode, scale, INT16_MIN, INT16_MAX, s);
}

int32_t float64_to_int32_scalbn(float64 a, FloatRoundMode rmode, int scale,
                                float_status *s)
{
    FloatParts64 p;

    float64_unpack_canonical(&p, a, s);
    return parts_float_to_sint(&p, rmode, scale, INT32_MIN, INT32_MAX, s);
}

int64_t float64_to_int64_scalbn(float64 a, FloatRoundMode rmode, int scale,
                                float_status *s)
{
    FloatParts64 p;

    float64_unpack_canonical(&p, a, s);
    return parts_float_to_sint(&p, rmode, scale, INT64_MIN, INT64_MAX, s);
}

int8_t bfloat16_to_int8_scalbn(bfloat16 a, FloatRoundMode rmode, int scale,
                               float_status *s)
{
    FloatParts64 p;

    bfloat16_unpack_canonical(&p, a, s);
    return parts_float_to_sint(&p, rmode, scale, INT8_MIN, INT8_MAX, s);
}

int16_t bfloat16_to_int16_scalbn(bfloat16 a, FloatRoundMode rmode, int scale,
                                 float_status *s)
{
    FloatParts64 p;

    bfloat16_unpack_canonical(&p, a, s);
    return parts_float_to_sint(&p, rmode, scale, INT16_MIN, INT16_MAX, s);
}

int32_t bfloat16_to_int32_scalbn(bfloat16 a, FloatRoundMode rmode, int scale,
                                 float_status *s)
{
    FloatParts64 p;

    bfloat16_unpack_canonical(&p, a, s);
    return parts_float_to_sint(&p, rmode, scale, INT32_MIN, INT32_MAX, s);
}

int64_t bfloat16_to_int64_scalbn(bfloat16 a, FloatRoundMode rmode, int scale,
                                 float_status *s)
{
    FloatParts64 p;

    bfloat16_unpack_canonical(&p, a, s);
    return parts_float_to_sint(&p, rmode, scale, INT64_MIN, INT64_MAX, s);
}

static int32_t float128_to_int32_scalbn(float128 a, FloatRoundMode rmode,
                                        int scale, float_status *s)
{
    FloatParts128 p;

    float128_unpack_canonical(&p, a, s);
    return parts_float_to_sint(&p, rmode, scale, INT32_MIN, INT32_MAX, s);
}

static int64_t float128_to_int64_scalbn(float128 a, FloatRoundMode rmode,
                                        int scale, float_status *s)
{
    FloatParts128 p;

    float128_unpack_canonical(&p, a, s);
    return parts_float_to_sint(&p, rmode, scale, INT64_MIN, INT64_MAX, s);
}

int8_t float16_to_int8(float16 a, float_status *s)
{
    return float16_to_int8_scalbn(a, s->float_rounding_mode, 0, s);
}

int16_t float16_to_int16(float16 a, float_status *s)
{
    return float16_to_int16_scalbn(a, s->float_rounding_mode, 0, s);
}

int32_t float16_to_int32(float16 a, float_status *s)
{
    return float16_to_int32_scalbn(a, s->float_rounding_mode, 0, s);
}

int64_t float16_to_int64(float16 a, float_status *s)
{
    return float16_to_int64_scalbn(a, s->float_rounding_mode, 0, s);
}

int16_t float32_to_int16(float32 a, float_status *s)
{
    return float32_to_int16_scalbn(a, s->float_rounding_mode, 0, s);
}

int32_t float32_to_int32(float32 a, float_status *s)
{
    return float32_to_int32_scalbn(a, s->float_rounding_mode, 0, s);
}

int64_t float32_to_int64(float32 a, float_status *s)
{
    return float32_to_int64_scalbn(a, s->float_rounding_mode, 0, s);
}

int16_t float64_to_int16(float64 a, float_status *s)
{
    return float64_to_int16_scalbn(a, s->float_rounding_mode, 0, s);
}

int32_t float64_to_int32(float64 a, float_status *s)
{
    return float64_to_int32_scalbn(a, s->float_rounding_mode, 0, s);
}

int64_t float64_to_int64(float64 a, float_status *s)
{
    return float64_to_int64_scalbn(a, s->float_rounding_mode, 0, s);
}

int32_t float128_to_int32(float128 a, float_status *s)
{
    return float128_to_int32_scalbn(a, s->float_rounding_mode, 0, s);
}

int64_t float128_to_int64(float128 a, float_status *s)
{
    return float128_to_int64_scalbn(a, s->float_rounding_mode, 0, s);
}

int16_t float16_to_int16_round_to_zero(float16 a, float_status *s)
{
    return float16_to_int16_scalbn(a, float_round_to_zero, 0, s);
}

int32_t float16_to_int32_round_to_zero(float16 a, float_status *s)
{
    return float16_to_int32_scalbn(a, float_round_to_zero, 0, s);
}

int64_t float16_to_int64_round_to_zero(float16 a, float_status *s)
{
    return float16_to_int64_scalbn(a, float_round_to_zero, 0, s);
}

int16_t float32_to_int16_round_to_zero(float32 a, float_status *s)
{
    return float32_to_int16_scalbn(a, float_round_to_zero, 0, s);
}

int32_t float32_to_int32_round_to_zero(float32 a, float_status *s)
{
    return float32_to_int32_scalbn(a, float_round_to_zero, 0, s);
}

int64_t float32_to_int64_round_to_zero(float32 a, float_status *s)
{
    return float32_to_int64_scalbn(a, float_round_to_zero, 0, s);
}

int16_t float64_to_int16_round_to_zero(float64 a, float_status *s)
{
    return float64_to_int16_scalbn(a, float_round_to_zero, 0, s);
}

int32_t float64_to_int32_round_to_zero(float64 a, float_status *s)
{
    return float64_to_int32_scalbn(a, float_round_to_zero, 0, s);
}

int64_t float64_to_int64_round_to_zero(float64 a, float_status *s)
{
    return float64_to_int64_scalbn(a, float_round_to_zero, 0, s);
}

int32_t float128_to_int32_round_to_zero(float128 a, float_status *s)
{
    return float128_to_int32_scalbn(a, float_round_to_zero, 0, s);
}

int64_t float128_to_int64_round_to_zero(float128 a, float_status *s)
{
    return float128_to_int64_scalbn(a, float_round_to_zero, 0, s);
}

int8_t bfloat16_to_int8(bfloat16 a, float_status *s)
{
    return bfloat16_to_int8_scalbn(a, s->float_rounding_mode, 0, s);
}

int16_t bfloat16_to_int16(bfloat16 a, float_status *s)
{
    return bfloat16_to_int16_scalbn(a, s->float_rounding_mode, 0, s);
}

int32_t bfloat16_to_int32(bfloat16 a, float_status *s)
{
    return bfloat16_to_int32_scalbn(a, s->float_rounding_mode, 0, s);
}

int64_t bfloat16_to_int64(bfloat16 a, float_status *s)
{
    return bfloat16_to_int64_scalbn(a, s->float_rounding_mode, 0, s);
}

int8_t bfloat16_to_int8_round_to_zero(bfloat16 a, float_status *s)
{
    return bfloat16_to_int8_scalbn(a, float_round_to_zero, 0, s);
}

int16_t bfloat16_to_int16_round_to_zero(bfloat16 a, float_status *s)
{
    return bfloat16_to_int16_scalbn(a, float_round_to_zero, 0, s);
}

int32_t bfloat16_to_int32_round_to_zero(bfloat16 a, float_status *s)
{
    return bfloat16_to_int32_scalbn(a, float_round_to_zero, 0, s);
}

int64_t bfloat16_to_int64_round_to_zero(bfloat16 a, float_status *s)
{
    return bfloat16_to_int64_scalbn(a, float_round_to_zero, 0, s);
}

int32_t float64_to_int32_modulo(float64 a, FloatRoundMode rmode,
                                float_status *s)
{
    FloatParts64 p;

    float64_unpack_canonical(&p, a, s);
    return parts_float_to_sint_modulo(&p, rmode, 31, s);
}

int64_t float64_to_int64_modulo(float64 a, FloatRoundMode rmode,
                                float_status *s)
{
    FloatParts64 p;

    float64_unpack_canonical(&p, a, s);
    return parts_float_to_sint_modulo(&p, rmode, 63, s);
}

/*
 * Floating-point to unsigned integer conversions
 */

uint8_t float16_to_uint8_scalbn(float16 a, FloatRoundMode rmode, int scale,
                                float_status *s)
{
    FloatParts64 p;

    float16_unpack_canonical(&p, a, s);
    return parts_float_to_uint(&p, rmode, scale, UINT8_MAX, s);
}

uint16_t float16_to_uint16_scalbn(float16 a, FloatRoundMode rmode, int scale,
                                  float_status *s)
{
    FloatParts64 p;

    float16_unpack_canonical(&p, a, s);
    return parts_float_to_uint(&p, rmode, scale, UINT16_MAX, s);
}

uint32_t float16_to_uint32_scalbn(float16 a, FloatRoundMode rmode, int scale,
                                  float_status *s)
{
    FloatParts64 p;

    float16_unpack_canonical(&p, a, s);
    return parts_float_to_uint(&p, rmode, scale, UINT32_MAX, s);
}

uint64_t float16_to_uint64_scalbn(float16 a, FloatRoundMode rmode, int scale,
                                  float_status *s)
{
    FloatParts64 p;

    float16_unpack_canonical(&p, a, s);
    return parts_float_to_uint(&p, rmode, scale, UINT64_MAX, s);
}

uint16_t float32_to_uint16_scalbn(float32 a, FloatRoundMode rmode, int scale,
                                  float_status *s)
{
    FloatParts64 p;

    float32_unpack_canonical(&p, a, s);
    return parts_float_to_uint(&p, rmode, scale, UINT16_MAX, s);
}

uint32_t float32_to_uint32_scalbn(float32 a, FloatRoundMode rmode, int scale,
                                  float_status *s)
{
    FloatParts64 p;

    float32_unpack_canonical(&p, a, s);
    return parts_float_to_uint(&p, rmode, scale, UINT32_MAX, s);
}

uint64_t float32_to_uint64_scalbn(float32 a, FloatRoundMode rmode, int scale,
                                  float_status *s)
{
    FloatParts64 p;

    float32_unpack_canonical(&p, a, s);
    return parts_float_to_uint(&p, rmode, scale, UINT64_MAX, s);
}

uint16_t float64_to_uint16_scalbn(float64 a, FloatRoundMode rmode, int scale,
                                  float_status *s)
{
    FloatParts64 p;

    float64_unpack_canonical(&p, a, s);
    return parts_float_to_uint(&p, rmode, scale, UINT16_MAX, s);
}

uint32_t float64_to_uint32_scalbn(float64 a, FloatRoundMode rmode, int scale,
                                  float_status *s)
{
    FloatParts64 p;

    float64_unpack_canonical(&p, a, s);
    return parts_float_to_uint(&p, rmode, scale, UINT32_MAX, s);
}

uint64_t float64_to_uint64_scalbn(float64 a, FloatRoundMode rmode, int scale,
                                  float_status *s)
{
    FloatParts64 p;

    float64_unpack_canonical(&p, a, s);
    return parts_float_to_uint(&p, rmode, scale, UINT64_MAX, s);
}

uint8_t bfloat16_to_uint8_scalbn(bfloat16 a, FloatRoundMode rmode,
                                 int scale, float_status *s)
{
    FloatParts64 p;

    bfloat16_unpack_canonical(&p, a, s);
    return parts_float_to_uint(&p, rmode, scale, UINT8_MAX, s);
}

uint16_t bfloat16_to_uint16_scalbn(bfloat16 a, FloatRoundMode rmode,
                                   int scale, float_status *s)
{
    FloatParts64 p;

    bfloat16_unpack_canonical(&p, a, s);
    return parts_float_to_uint(&p, rmode, scale, UINT16_MAX, s);
}

uint32_t bfloat16_to_uint32_scalbn(bfloat16 a, FloatRoundMode rmode,
                                   int scale, float_status *s)
{
    FloatParts64 p;

    bfloat16_unpack_canonical(&p, a, s);
    return parts_float_to_uint(&p, rmode, scale, UINT32_MAX, s);
}

uint64_t bfloat16_to_uint64_scalbn(bfloat16 a, FloatRoundMode rmode,
                                   int scale, float_status *s)
{
    FloatParts64 p;

    bfloat16_unpack_canonical(&p, a, s);
    return parts_float_to_uint(&p, rmode, scale, UINT64_MAX, s);
}

static uint32_t float128_to_uint32_scalbn(float128 a, FloatRoundMode rmode,
                                          int scale, float_status *s)
{
    FloatParts128 p;

    float128_unpack_canonical(&p, a, s);
    return parts_float_to_uint(&p, rmode, scale, UINT32_MAX, s);
}

static uint64_t float128_to_uint64_scalbn(float128 a, FloatRoundMode rmode,
                                          int scale, float_status *s)
{
    FloatParts128 p;

    float128_unpack_canonical(&p, a, s);
    return parts_float_to_uint(&p, rmode, scale, UINT64_MAX, s);
}

uint8_t float16_to_uint8(float16 a, float_status *s)
{
    return float16_to_uint8_scalbn(a, s->float_rounding_mode, 0, s);
}

uint16_t float16_to_uint16(float16 a, float_status *s)
{
    return float16_to_uint16_scalbn(a, s->float_rounding_mode, 0, s);
}

uint32_t float16_to_uint32(float16 a, float_status *s)
{
    return float16_to_uint32_scalbn(a, s->float_rounding_mode, 0, s);
}

uint64_t float16_to_uint64(float16 a, float_status *s)
{
    return float16_to_uint64_scalbn(a, s->float_rounding_mode, 0, s);
}

uint16_t float32_to_uint16(float32 a, float_status *s)
{
    return float32_to_uint16_scalbn(a, s->float_rounding_mode, 0, s);
}

uint32_t float32_to_uint32(float32 a, float_status *s)
{
    return float32_to_uint32_scalbn(a, s->float_rounding_mode, 0, s);
}

uint64_t float32_to_uint64(float32 a, float_status *s)
{
    return float32_to_uint64_scalbn(a, s->float_rounding_mode, 0, s);
}

uint16_t float64_to_uint16(float64 a, float_status *s)
{
    return float64_to_uint16_scalbn(a, s->float_rounding_mode, 0, s);
}

uint32_t float64_to_uint32(float64 a, float_status *s)
{
    return float64_to_uint32_scalbn(a, s->float_rounding_mode, 0, s);
}

uint64_t float64_to_uint64(float64 a, float_status *s)
{
    return float64_to_uint64_scalbn(a, s->float_rounding_mode, 0, s);
}

uint32_t float128_to_uint32(float128 a, float_status *s)
{
    return float128_to_uint32_scalbn(a, s->float_rounding_mode, 0, s);
}

uint64_t float128_to_uint64(float128 a, float_status *s)
{
    return float128_to_uint64_scalbn(a, s->float_rounding_mode, 0, s);
}

uint16_t float16_to_uint16_round_to_zero(float16 a, float_status *s)
{
    return float16_to_uint16_scalbn(a, float_round_to_zero, 0, s);
}

uint32_t float16_to_uint32_round_to_zero(float16 a, float_status *s)
{
    return float16_to_uint32_scalbn(a, float_round_to_zero, 0, s);
}

uint64_t float16_to_uint64_round_to_zero(float16 a, float_status *s)
{
    return float16_to_uint64_scalbn(a, float_round_to_zero, 0, s);
}

uint16_t float32_to_uint16_round_to_zero(float32 a, float_status *s)
{
    return float32_to_uint16_scalbn(a, float_round_to_zero, 0, s);
}

uint32_t float32_to_uint32_round_to_zero(float32 a, float_status *s)
{
    return float32_to_uint32_scalbn(a, float_round_to_zero, 0, s);
}

uint64_t float32_to_uint64_round_to_zero(float32 a, float_status *s)
{
    return float32_to_uint64_scalbn(a, float_round_to_zero, 0, s);
}

uint16_t float64_to_uint16_round_to_zero(float64 a, float_status *s)
{
    return float64_to_uint16_scalbn(a, float_round_to_zero, 0, s);
}

uint32_t float64_to_uint32_round_to_zero(float64 a, float_status *s)
{
    return float64_to_uint32_scalbn(a, float_round_to_zero, 0, s);
}

uint64_t float64_to_uint64_round_to_zero(float64 a, float_status *s)
{
    return float64_to_uint64_scalbn(a, float_round_to_zero, 0, s);
}

uint32_t float128_to_uint32_round_to_zero(float128 a, float_status *s)
{
    return float128_to_uint32_scalbn(a, float_round_to_zero, 0, s);
}

uint64_t float128_to_uint64_round_to_zero(float128 a, float_status *s)
{
    return float128_to_uint64_scalbn(a, float_round_to_zero, 0, s);
}

uint8_t bfloat16_to_uint8(bfloat16 a, float_status *s)
{
    return bfloat16_to_uint8_scalbn(a, s->float_rounding_mode, 0, s);
}

uint16_t bfloat16_to_uint16(bfloat16 a, float_status *s)
{
    return bfloat16_to_uint16_scalbn(a, s->float_rounding_mode, 0, s);
}

uint32_t bfloat16_to_uint32(bfloat16 a, float_status *s)
{
    return bfloat16_to_uint32_scalbn(a, s->float_rounding_mode, 0, s);
}

uint64_t bfloat16_to_uint64(bfloat16 a, float_status *s)
{
    return bfloat16_to_uint64_scalbn(a, s->float_rounding_mode, 0, s);
}

uint8_t bfloat16_to_uint8_round_to_zero(bfloat16 a, float_status *s)
{
    return bfloat16_to_uint8_scalbn(a, float_round_to_zero, 0, s);
}

uint16_t bfloat16_to_uint16_round_to_zero(bfloat16 a, float_status *s)
{
    return bfloat16_to_uint16_scalbn(a, float_round_to_zero, 0, s);
}

uint32_t bfloat16_to_uint32_round_to_zero(bfloat16 a, float_status *s)
{
    return bfloat16_to_uint32_scalbn(a, float_round_to_zero, 0, s);
}

uint64_t bfloat16_to_uint64_round_to_zero(bfloat16 a, float_status *s)
{
    return bfloat16_to_uint64_scalbn(a, float_round_to_zero, 0, s);
}

/*
 * Signed integer to floating-point conversions
 */

float16 int64_to_float16_scalbn(int64_t a, int scale, float_status *status)
{
    FloatParts64 p;

    parts_sint_to_float(&p, a, scale, status);
    return float16_round_pack_canonical(&p, status);
}

float16 int32_to_float16_scalbn(int32_t a, int scale, float_status *status)
{
    return int64_to_float16_scalbn(a, scale, status);
}

float16 int16_to_float16_scalbn(int16_t a, int scale, float_status *status)
{
    return int64_to_float16_scalbn(a, scale, status);
}

float16 int64_to_float16(int64_t a, float_status *status)
{
    return int64_to_float16_scalbn(a, 0, status);
}

float16 int32_to_float16(int32_t a, float_status *status)
{
    return int64_to_float16_scalbn(a, 0, status);
}

float16 int16_to_float16(int16_t a, float_status *status)
{
    return int64_to_float16_scalbn(a, 0, status);
}

float16 int8_to_float16(int8_t a, float_status *status)
{
    return int64_to_float16_scalbn(a, 0, status);
}

float32 int64_to_float32_scalbn(int64_t a, int scale, float_status *status)
{
    FloatParts64 p;

    /* Without scaling, there are no overflow concerns. */
    if (likely(scale == 0) && can_use_fpu(status)) {
        union_float32 ur;
        ur.h = a;
        return ur.s;
    }

    parts64_sint_to_float(&p, a, scale, status);
    return float32_round_pack_canonical(&p, status);
}

float32 int32_to_float32_scalbn(int32_t a, int scale, float_status *status)
{
    return int64_to_float32_scalbn(a, scale, status);
}

float32 int16_to_float32_scalbn(int16_t a, int scale, float_status *status)
{
    return int64_to_float32_scalbn(a, scale, status);
}

float32 int64_to_float32(int64_t a, float_status *status)
{
    return int64_to_float32_scalbn(a, 0, status);
}

float32 int32_to_float32(int32_t a, float_status *status)
{
    return int64_to_float32_scalbn(a, 0, status);
}

float32 int16_to_float32(int16_t a, float_status *status)
{
    return int64_to_float32_scalbn(a, 0, status);
}

float64 int64_to_float64_scalbn(int64_t a, int scale, float_status *status)
{
    FloatParts64 p;

    /* Without scaling, there are no overflow concerns. */
    if (likely(scale == 0) && can_use_fpu(status)) {
        union_float64 ur;
        ur.h = a;
        return ur.s;
    }

    parts_sint_to_float(&p, a, scale, status);
    return float64_round_pack_canonical(&p, status);
}

float64 int32_to_float64_scalbn(int32_t a, int scale, float_status *status)
{
    return int64_to_float64_scalbn(a, scale, status);
}

float64 int16_to_float64_scalbn(int16_t a, int scale, float_status *status)
{
    return int64_to_float64_scalbn(a, scale, status);
}

float64 int64_to_float64(int64_t a, float_status *status)
{
    return int64_to_float64_scalbn(a, 0, status);
}

float64 int32_to_float64(int32_t a, float_status *status)
{
    return int64_to_float64_scalbn(a, 0, status);
}

float64 int16_to_float64(int16_t a, float_status *status)
{
    return int64_to_float64_scalbn(a, 0, status);
}

bfloat16 int64_to_bfloat16_scalbn(int64_t a, int scale, float_status *status)
{
    FloatParts64 p;

    parts_sint_to_float(&p, a, scale, status);
    return bfloat16_round_pack_canonical(&p, status);
}

bfloat16 int32_to_bfloat16_scalbn(int32_t a, int scale, float_status *status)
{
    return int64_to_bfloat16_scalbn(a, scale, status);
}

bfloat16 int16_to_bfloat16_scalbn(int16_t a, int scale, float_status *status)
{
    return int64_to_bfloat16_scalbn(a, scale, status);
}

bfloat16 int8_to_bfloat16_scalbn(int8_t a, int scale, float_status *status)
{
    return int64_to_bfloat16_scalbn(a, scale, status);
}

bfloat16 int64_to_bfloat16(int64_t a, float_status *status)
{
    return int64_to_bfloat16_scalbn(a, 0, status);
}

bfloat16 int32_to_bfloat16(int32_t a, float_status *status)
{
    return int64_to_bfloat16_scalbn(a, 0, status);
}

bfloat16 int16_to_bfloat16(int16_t a, float_status *status)
{
    return int64_to_bfloat16_scalbn(a, 0, status);
}

bfloat16 int8_to_bfloat16(int8_t a, float_status *status)
{
    return int64_to_bfloat16_scalbn(a, 0, status);
}

float128 int64_to_float128(int64_t a, float_status *status)
{
    FloatParts128 p;

    parts_sint_to_float(&p, a, 0, status);
    return float128_round_pack_canonical(&p, status);
}

float128 int32_to_float128(int32_t a, float_status *status)
{
    return int64_to_float128(a, status);
}

/*
 * Unsigned Integer to floating-point conversions
 */

float16 uint64_to_float16_scalbn(uint64_t a, int scale, float_status *status)
{
    FloatParts64 p;

    parts_uint_to_float(&p, a, scale, status);
    return float16_round_pack_canonical(&p, status);
}

float16 uint32_to_float16_scalbn(uint32_t a, int scale, float_status *status)
{
    return uint64_to_float16_scalbn(a, scale, status);
}

float16 uint16_to_float16_scalbn(uint16_t a, int scale, float_status *status)
{
    return uint64_to_float16_scalbn(a, scale, status);
}

float16 uint64_to_float16(uint64_t a, float_status *status)
{
    return uint64_to_float16_scalbn(a, 0, status);
}

float16 uint32_to_float16(uint32_t a, float_status *status)
{
    return uint64_to_float16_scalbn(a, 0, status);
}

float16 uint16_to_float16(uint16_t a, float_status *status)
{
    return uint64_to_float16_scalbn(a, 0, status);
}

float16 uint8_to_float16(uint8_t a, float_status *status)
{
    return uint64_to_float16_scalbn(a, 0, status);
}

float32 uint64_to_float32_scalbn(uint64_t a, int scale, float_status *status)
{
    FloatParts64 p;

    /* Without scaling, there are no overflow concerns. */
    if (likely(scale == 0) && can_use_fpu(status)) {
        union_float32 ur;
        ur.h = a;
        return ur.s;
    }

    parts_uint_to_float(&p, a, scale, status);
    return float32_round_pack_canonical(&p, status);
}

float32 uint32_to_float32_scalbn(uint32_t a, int scale, float_status *status)
{
    return uint64_to_float32_scalbn(a, scale, status);
}

float32 uint16_to_float32_scalbn(uint16_t a, int scale, float_status *status)
{
    return uint64_to_float32_scalbn(a, scale, status);
}

float32 uint64_to_float32(uint64_t a, float_status *status)
{
    return uint64_to_float32_scalbn(a, 0, status);
}

float32 uint32_to_float32(uint32_t a, float_status *status)
{
    return uint64_to_float32_scalbn(a, 0, status);
}

float32 uint16_to_float32(uint16_t a, float_status *status)
{
    return uint64_to_float32_scalbn(a, 0, status);
}

float64 uint64_to_float64_scalbn(uint64_t a, int scale, float_status *status)
{
    FloatParts64 p;

    /* Without scaling, there are no overflow concerns. */
    if (likely(scale == 0) && can_use_fpu(status)) {
        union_float64 ur;
        ur.h = a;
        return ur.s;
    }

    parts_uint_to_float(&p, a, scale, status);
    return float64_round_pack_canonical(&p, status);
}

float64 uint32_to_float64_scalbn(uint32_t a, int scale, float_status *status)
{
    return uint64_to_float64_scalbn(a, scale, status);
}

float64 uint16_to_float64_scalbn(uint16_t a, int scale, float_status *status)
{
    return uint64_to_float64_scalbn(a, scale, status);
}

float64 uint64_to_float64(uint64_t a, float_status *status)
{
    return uint64_to_float64_scalbn(a, 0, status);
}

float64 uint32_to_float64(uint32_t a, float_status *status)
{
    return uint64_to_float64_scalbn(a, 0, status);
}

float64 uint16_to_float64(uint16_t a, float_status *status)
{
    return uint64_to_float64_scalbn(a, 0, status);
}

bfloat16 uint64_to_bfloat16_scalbn(uint64_t a, int scale, float_status *status)
{
    FloatParts64 p;

    parts_uint_to_float(&p, a, scale, status);
    return bfloat16_round_pack_canonical(&p, status);
}

bfloat16 uint32_to_bfloat16_scalbn(uint32_t a, int scale, float_status *status)
{
    return uint64_to_bfloat16_scalbn(a, scale, status);
}

bfloat16 uint16_to_bfloat16_scalbn(uint16_t a, int scale, float_status *status)
{
    return uint64_to_bfloat16_scalbn(a, scale, status);
}

bfloat16 uint8_to_bfloat16_scalbn(uint8_t a, int scale, float_status *status)
{
    return uint64_to_bfloat16_scalbn(a, scale, status);
}

bfloat16 uint64_to_bfloat16(uint64_t a, float_status *status)
{
    return uint64_to_bfloat16_scalbn(a, 0, status);
}

bfloat16 uint32_to_bfloat16(uint32_t a, float_status *status)
{
    return uint64_to_bfloat16_scalbn(a, 0, status);
}

bfloat16 uint16_to_bfloat16(uint16_t a, float_status *status)
{
    return uint64_to_bfloat16_scalbn(a, 0, status);
}

bfloat16 uint8_to_bfloat16(uint8_t a, float_status *status)
{
    return uint64_to_bfloat16_scalbn(a, 0, status);
}

float128 uint64_to_float128(uint64_t a, float_status *status)
{
    FloatParts128 p;

    parts_uint_to_float(&p, a, 0, status);
    return float128_round_pack_canonical(&p, status);
}

/*
 * Minimum and maximum
 */

static float16 float16_minmax(float16 a, float16 b, float_status *s, int flags)
{
    FloatParts64 pa, pb, *pr;

    float16_unpack_canonical(&pa, a, s);
    float16_unpack_canonical(&pb, b, s);
    pr = parts_minmax(&pa, &pb, s, flags);

    return float16_round_pack_canonical(pr, s);
}

static bfloat16 bfloat16_minmax(bfloat16 a, bfloat16 b,
                                float_status *s, int flags)
{
    FloatParts64 pa, pb, *pr;

    bfloat16_unpack_canonical(&pa, a, s);
    bfloat16_unpack_canonical(&pb, b, s);
    pr = parts_minmax(&pa, &pb, s, flags);

    return bfloat16_round_pack_canonical(pr, s);
}

static float32 float32_minmax(float32 a, float32 b, float_status *s, int flags)
{
    FloatParts64 pa, pb, *pr;

    float32_unpack_canonical(&pa, a, s);
    float32_unpack_canonical(&pb, b, s);
    pr = parts_minmax(&pa, &pb, s, flags);

    return float32_round_pack_canonical(pr, s);
}

static float64 float64_minmax(float64 a, float64 b, float_status *s, int flags)
{
    FloatParts64 pa, pb, *pr;

    float64_unpack_canonical(&pa, a, s);
    float64_unpack_canonical(&pb, b, s);
    pr = parts_minmax(&pa, &pb, s, flags);

    return float64_round_pack_canonical(pr, s);
}

static float128 float128_minmax(float128 a, float128 b,
                                float_status *s, int flags)
{
    FloatParts128 pa, pb, *pr;

    float128_unpack_canonical(&pa, a, s);
    float128_unpack_canonical(&pb, b, s);
    pr = parts_minmax(&pa, &pb, s, flags);

    return float128_round_pack_canonical(pr, s);
}

#define MINMAX_1(type, name, flags) \
    type type##_##name(type a, type b, float_status *s) \
    { return type##_minmax(a, b, s, flags); }

#define MINMAX_2(type) \
    MINMAX_1(type, max, 0)                                                \
    MINMAX_1(type, maxnum, minmax_isnum)                                  \
    MINMAX_1(type, maxnummag, minmax_isnum | minmax_ismag)                \
    MINMAX_1(type, maximum_number, minmax_isnumber)                       \
    MINMAX_1(type, min, minmax_ismin)                                     \
    MINMAX_1(type, minnum, minmax_ismin | minmax_isnum)                   \
    MINMAX_1(type, minnummag, minmax_ismin | minmax_isnum | minmax_ismag) \
    MINMAX_1(type, minimum_number, minmax_ismin | minmax_isnumber)        \

MINMAX_2(float16)
MINMAX_2(bfloat16)
MINMAX_2(float32)
MINMAX_2(float64)
MINMAX_2(float128)

#undef MINMAX_1
#undef MINMAX_2

/*
 * Floating point compare
 */

static FloatRelation QEMU_FLATTEN
float16_do_compare(float16 a, float16 b, float_status *s, bool is_quiet)
{
    FloatParts64 pa, pb;

    float16_unpack_canonical(&pa, a, s);
    float16_unpack_canonical(&pb, b, s);
    return parts_compare(&pa, &pb, s, is_quiet);
}

FloatRelation float16_compare(float16 a, float16 b, float_status *s)
{
    return float16_do_compare(a, b, s, false);
}

FloatRelation float16_compare_quiet(float16 a, float16 b, float_status *s)
{
    return float16_do_compare(a, b, s, true);
}

static FloatRelation QEMU_SOFTFLOAT_ATTR
float32_do_compare(float32 a, float32 b, float_status *s, bool is_quiet)
{
    FloatParts64 pa, pb;

    float32_unpack_canonical(&pa, a, s);
    float32_unpack_canonical(&pb, b, s);
    return parts_compare(&pa, &pb, s, is_quiet);
}

static FloatRelation QEMU_FLATTEN
float32_hs_compare(float32 xa, float32 xb, float_status *s, bool is_quiet)
{
    union_float32 ua, ub;

    ua.s = xa;
    ub.s = xb;

    if (QEMU_NO_HARDFLOAT) {
        goto soft;
    }

    float32_input_flush2(&ua.s, &ub.s, s);
    if (isgreaterequal(ua.h, ub.h)) {
        if (isgreater(ua.h, ub.h)) {
            return float_relation_greater;
        }
        return float_relation_equal;
    }
    if (likely(isless(ua.h, ub.h))) {
        return float_relation_less;
    }
    /*
     * The only condition remaining is unordered.
     * Fall through to set flags.
     */
 soft:
    return float32_do_compare(ua.s, ub.s, s, is_quiet);
}

FloatRelation float32_compare(float32 a, float32 b, float_status *s)
{
    return float32_hs_compare(a, b, s, false);
}

FloatRelation float32_compare_quiet(float32 a, float32 b, float_status *s)
{
    return float32_hs_compare(a, b, s, true);
}

static FloatRelation QEMU_SOFTFLOAT_ATTR
float64_do_compare(float64 a, float64 b, float_status *s, bool is_quiet)
{
    FloatParts64 pa, pb;

    float64_unpack_canonical(&pa, a, s);
    float64_unpack_canonical(&pb, b, s);
    return parts_compare(&pa, &pb, s, is_quiet);
}

static FloatRelation QEMU_FLATTEN
float64_hs_compare(float64 xa, float64 xb, float_status *s, bool is_quiet)
{
    union_float64 ua, ub;

    ua.s = xa;
    ub.s = xb;

    if (QEMU_NO_HARDFLOAT) {
        goto soft;
    }

    float64_input_flush2(&ua.s, &ub.s, s);
    if (isgreaterequal(ua.h, ub.h)) {
        if (isgreater(ua.h, ub.h)) {
            return float_relation_greater;
        }
        return float_relation_equal;
    }
    if (likely(isless(ua.h, ub.h))) {
        return float_relation_less;
    }
    /*
     * The only condition remaining is unordered.
     * Fall through to set flags.
     */
 soft:
    return float64_do_compare(ua.s, ub.s, s, is_quiet);
}

FloatRelation float64_compare(float64 a, float64 b, float_status *s)
{
    return float64_hs_compare(a, b, s, false);
}

FloatRelation float64_compare_quiet(float64 a, float64 b, float_status *s)
{
    return float64_hs_compare(a, b, s, true);
}

static FloatRelation QEMU_FLATTEN
bfloat16_do_compare(bfloat16 a, bfloat16 b, float_status *s, bool is_quiet)
{
    FloatParts64 pa, pb;

    bfloat16_unpack_canonical(&pa, a, s);
    bfloat16_unpack_canonical(&pb, b, s);
    return parts_compare(&pa, &pb, s, is_quiet);
}

FloatRelation bfloat16_compare(bfloat16 a, bfloat16 b, float_status *s)
{
    return bfloat16_do_compare(a, b, s, false);
}

FloatRelation bfloat16_compare_quiet(bfloat16 a, bfloat16 b, float_status *s)
{
    return bfloat16_do_compare(a, b, s, true);
}

static FloatRelation QEMU_FLATTEN
float128_do_compare(float128 a, float128 b, float_status *s, bool is_quiet)
{
    FloatParts128 pa, pb;

    float128_unpack_canonical(&pa, a, s);
    float128_unpack_canonical(&pb, b, s);
    return parts_compare(&pa, &pb, s, is_quiet);
}

FloatRelation float128_compare(float128 a, float128 b, float_status *s)
{
    return float128_do_compare(a, b, s, false);
}

FloatRelation float128_compare_quiet(float128 a, float128 b, float_status *s)
{
    return float128_do_compare(a, b, s, true);
}

/*
 * Scale by 2**N
 */

float16 float16_scalbn(float16 a, int n, float_status *status)
{
    FloatParts64 p;

    float16_unpack_canonical(&p, a, status);
    parts_scalbn(&p, n, status);
    return float16_round_pack_canonical(&p, status);
}

float32 float32_scalbn(float32 a, int n, float_status *status)
{
    FloatParts64 p;

    float32_unpack_canonical(&p, a, status);
    parts_scalbn(&p, n, status);
    return float32_round_pack_canonical(&p, status);
}

float64 float64_scalbn(float64 a, int n, float_status *status)
{
    FloatParts64 p;

    float64_unpack_canonical(&p, a, status);
    parts_scalbn(&p, n, status);
    return float64_round_pack_canonical(&p, status);
}

bfloat16 bfloat16_scalbn(bfloat16 a, int n, float_status *status)
{
    FloatParts64 p;

    bfloat16_unpack_canonical(&p, a, status);
    parts_scalbn(&p, n, status);
    return bfloat16_round_pack_canonical(&p, status);
}

float128 float128_scalbn(float128 a, int n, float_status *status)
{
    FloatParts128 p;

    float128_unpack_canonical(&p, a, status);
    parts_scalbn(&p, n, status);
    return float128_round_pack_canonical(&p, status);
}

/*
 * Square Root
 */

float16 QEMU_FLATTEN float16_sqrt(float16 a, float_status *status)
{
    FloatParts64 p;

    float16_unpack_canonical(&p, a, status);
    parts_sqrt(&p, status, &float16_params);
    return float16_round_pack_canonical(&p, status);
}

static float32 QEMU_SOFTFLOAT_ATTR
soft_f32_sqrt(float32 a, float_status *status)
{
    FloatParts64 p;

    float32_unpack_canonical(&p, a, status);
    parts_sqrt(&p, status, &float32_params);
    return float32_round_pack_canonical(&p, status);
}

static float64 QEMU_SOFTFLOAT_ATTR
soft_f64_sqrt(float64 a, float_status *status)
{
    FloatParts64 p;

    float64_unpack_canonical(&p, a, status);
    parts_sqrt(&p, status, &float64_params);
    return float64_round_pack_canonical(&p, status);
}

float32 QEMU_FLATTEN float32_sqrt(float32 xa, float_status *s)
{
    union_float32 ua, ur;

    ua.s = xa;
    if (unlikely(!can_use_fpu(s))) {
        goto soft;
    }

    float32_input_flush1(&ua.s, s);
    if (QEMU_HARDFLOAT_1F32_USE_FP) {
        if (unlikely(!(fpclassify(ua.h) == FP_NORMAL ||
                       fpclassify(ua.h) == FP_ZERO) ||
                     signbit(ua.h))) {
            goto soft;
        }
    } else if (unlikely(!float32_is_zero_or_normal(ua.s) ||
                        float32_is_neg(ua.s))) {
        goto soft;
    }
    ur.h = sqrtf(ua.h);
    return ur.s;

 soft:
    return soft_f32_sqrt(ua.s, s);
}

float64 QEMU_FLATTEN float64_sqrt(float64 xa, float_status *s)
{
    union_float64 ua, ur;

    ua.s = xa;
    if (unlikely(!can_use_fpu(s))) {
        goto soft;
    }

    float64_input_flush1(&ua.s, s);
    if (QEMU_HARDFLOAT_1F64_USE_FP) {
        if (unlikely(!(fpclassify(ua.h) == FP_NORMAL ||
                       fpclassify(ua.h) == FP_ZERO) ||
                     signbit(ua.h))) {
            goto soft;
        }
    } else if (unlikely(!float64_is_zero_or_normal(ua.s) ||
                        float64_is_neg(ua.s))) {
        goto soft;
    }
    ur.h = sqrt(ua.h);
    return ur.s;

 soft:
    return soft_f64_sqrt(ua.s, s);
}

float64 float64r32_sqrt(float64 a, float_status *status)
{
    FloatParts64 p;

    float64_unpack_canonical(&p, a, status);
    parts_sqrt(&p, status, &float64_params);
    return float64r32_round_pack_canonical(&p, status);
}

bfloat16 QEMU_FLATTEN bfloat16_sqrt(bfloat16 a, float_status *status)
{
    FloatParts64 p;

    bfloat16_unpack_canonical(&p, a, status);
    parts_sqrt(&p, status, &bfloat16_params);
    return bfloat16_round_pack_canonical(&p, status);
}

float128 QEMU_FLATTEN float128_sqrt(float128 a, float_status *status)
{
    FloatParts128 p;

    float128_unpack_canonical(&p, a, status);
    parts_sqrt(&p, status, &float128_params);
    return float128_round_pack_canonical(&p, status);
}

/*----------------------------------------------------------------------------
| The pattern for a default generated NaN.
*----------------------------------------------------------------------------*/

float16 float16_default_nan(float_status *status)
{
    FloatParts64 p;

    parts_default_nan(&p, status);
    p.frac >>= float16_params.frac_shift;
    return float16_pack_raw(&p);
}

float32 float32_default_nan(float_status *status)
{
    FloatParts64 p;

    parts_default_nan(&p, status);
    p.frac >>= float32_params.frac_shift;
    return float32_pack_raw(&p);
}

float64 float64_default_nan(float_status *status)
{
    FloatParts64 p;

    parts_default_nan(&p, status);
    p.frac >>= float64_params.frac_shift;
    return float64_pack_raw(&p);
}

float128 float128_default_nan(float_status *status)
{
    FloatParts128 p;

    parts_default_nan(&p, status);
    frac_shr(&p, float128_params.frac_shift);
    return float128_pack_raw(&p);
}

bfloat16 bfloat16_default_nan(float_status *status)
{
    FloatParts64 p;

    parts_default_nan(&p, status);
    p.frac >>= bfloat16_params.frac_shift;
    return bfloat16_pack_raw(&p);
}

/*----------------------------------------------------------------------------
| Returns a quiet NaN from a signalling NaN for the floating point value `a'.
*----------------------------------------------------------------------------*/

float16 float16_silence_nan(float16 a, float_status *status)
{
    FloatParts64 p;

    float16_unpack_raw(&p, a);
    p.frac <<= float16_params.frac_shift;
    parts_silence_nan(&p, status);
    p.frac >>= float16_params.frac_shift;
    return float16_pack_raw(&p);
}

float32 float32_silence_nan(float32 a, float_status *status)
{
    FloatParts64 p;

    float32_unpack_raw(&p, a);
    p.frac <<= float32_params.frac_shift;
    parts_silence_nan(&p, status);
    p.frac >>= float32_params.frac_shift;
    return float32_pack_raw(&p);
}

float64 float64_silence_nan(float64 a, float_status *status)
{
    FloatParts64 p;

    float64_unpack_raw(&p, a);
    p.frac <<= float64_params.frac_shift;
    parts_silence_nan(&p, status);
    p.frac >>= float64_params.frac_shift;
    return float64_pack_raw(&p);
}

bfloat16 bfloat16_silence_nan(bfloat16 a, float_status *status)
{
    FloatParts64 p;

    bfloat16_unpack_raw(&p, a);
    p.frac <<= bfloat16_params.frac_shift;
    parts_silence_nan(&p, status);
    p.frac >>= bfloat16_params.frac_shift;
    return bfloat16_pack_raw(&p);
}

float128 float128_silence_nan(float128 a, float_status *status)
{
    FloatParts128 p;

    float128_unpack_raw(&p, a);
    frac_shl(&p, float128_params.frac_shift);
    parts_silence_nan(&p, status);
    frac_shr(&p, float128_params.frac_shift);
    return float128_pack_raw(&p);
}

/*----------------------------------------------------------------------------
| If `a' is denormal and we are in flush-to-zero mode then set the
| input-denormal exception and return zero. Otherwise just return the value.
*----------------------------------------------------------------------------*/

static bool parts_squash_denormal(FloatParts64 p, float_status *status)
{
    if (p.exp == 0 && p.frac != 0) {
        float_raise(float_flag_input_denormal, status);
        return true;
    }

    return false;
}

float16 float16_squash_input_denormal(float16 a, float_status *status)
{
    if (status->flush_inputs_to_zero) {
        FloatParts64 p;

        float16_unpack_raw(&p, a);
        if (parts_squash_denormal(p, status)) {
            return float16_set_sign(float16_zero, p.sign);
        }
    }
    return a;
}

float32 float32_squash_input_denormal(float32 a, float_status *status)
{
    if (status->flush_inputs_to_zero) {
        FloatParts64 p;

        float32_unpack_raw(&p, a);
        if (parts_squash_denormal(p, status)) {
            return float32_set_sign(float32_zero, p.sign);
        }
    }
    return a;
}

float64 float64_squash_input_denormal(float64 a, float_status *status)
{
    if (status->flush_inputs_to_zero) {
        FloatParts64 p;

        float64_unpack_raw(&p, a);
        if (parts_squash_denormal(p, status)) {
            return float64_set_sign(float64_zero, p.sign);
        }
    }
    return a;
}

bfloat16 bfloat16_squash_input_denormal(bfloat16 a, float_status *status)
{
    if (status->flush_inputs_to_zero) {
        FloatParts64 p;

        bfloat16_unpack_raw(&p, a);
        if (parts_squash_denormal(p, status)) {
            return bfloat16_set_sign(bfloat16_zero, p.sign);
        }
    }
    return a;
}

/*----------------------------------------------------------------------------
| Takes a 64-bit fixed-point value `absZ' with binary point between bits 6
| and 7, and returns the properly rounded 32-bit integer corresponding to the
| input.  If `zSign' is 1, the input is negated before being converted to an
| integer.  Bit 63 of `absZ' must be zero.  Ordinarily, the fixed-point input
| is simply rounded to an integer, with the inexact exception raised if the
| input cannot be represented exactly as an integer.  However, if the fixed-
| point input is too large, the invalid exception is raised and the largest
| positive or negative integer is returned.
*----------------------------------------------------------------------------*/

static int32_t roundAndPackInt32(bool zSign, uint64_t absZ,
                                 float_status *status)
{
    int8_t roundingMode;
    bool roundNearestEven;
    int8_t roundIncrement, roundBits;
    int32_t z;

    roundingMode = status->float_rounding_mode;
    roundNearestEven = ( roundingMode == float_round_nearest_even );
    switch (roundingMode) {
    case float_round_nearest_even:
    case float_round_ties_away:
        roundIncrement = 0x40;
        break;
    case float_round_to_zero:
        roundIncrement = 0;
        break;
    case float_round_up:
        roundIncrement = zSign ? 0 : 0x7f;
        break;
    case float_round_down:
        roundIncrement = zSign ? 0x7f : 0;
        break;
    case float_round_to_odd:
        roundIncrement = absZ & 0x80 ? 0 : 0x7f;
        break;
    default:
        abort();
    }
    roundBits = absZ & 0x7F;
    absZ = ( absZ + roundIncrement )>>7;
    if (!(roundBits ^ 0x40) && roundNearestEven) {
        absZ &= ~1;
    }
    z = absZ;
    if ( zSign ) z = - z;
    if ( ( absZ>>32 ) || ( z && ( ( z < 0 ) ^ zSign ) ) ) {
        float_raise(float_flag_invalid, status);
        return zSign ? INT32_MIN : INT32_MAX;
    }
    if (roundBits) {
        float_raise(float_flag_inexact, status);
    }
    return z;

}

/*----------------------------------------------------------------------------
| Takes the 128-bit fixed-point value formed by concatenating `absZ0' and
| `absZ1', with binary point between bits 63 and 64 (between the input words),
| and returns the properly rounded 64-bit integer corresponding to the input.
| If `zSign' is 1, the input is negated before being converted to an integer.
| Ordinarily, the fixed-point input is simply rounded to an integer, with
| the inexact exception raised if the input cannot be represented exactly as
| an integer.  However, if the fixed-point input is too large, the invalid
| exception is raised and the largest positive or negative integer is
| returned.
*----------------------------------------------------------------------------*/

static int64_t roundAndPackInt64(bool zSign, uint64_t absZ0, uint64_t absZ1,
                               float_status *status)
{
    int8_t roundingMode;
    bool roundNearestEven, increment;
    int64_t z;

    roundingMode = status->float_rounding_mode;
    roundNearestEven = ( roundingMode == float_round_nearest_even );
    switch (roundingMode) {
    case float_round_nearest_even:
    case float_round_ties_away:
        increment = ((int64_t) absZ1 < 0);
        break;
    case float_round_to_zero:
        increment = 0;
        break;
    case float_round_up:
        increment = !zSign && absZ1;
        break;
    case float_round_down:
        increment = zSign && absZ1;
        break;
    case float_round_to_odd:
        increment = !(absZ0 & 1) && absZ1;
        break;
    default:
        abort();
    }
    if ( increment ) {
        ++absZ0;
        if ( absZ0 == 0 ) goto overflow;
        if (!(absZ1 << 1) && roundNearestEven) {
            absZ0 &= ~1;
        }
    }
    z = absZ0;
    if ( zSign ) z = - z;
    if ( z && ( ( z < 0 ) ^ zSign ) ) {
 overflow:
        float_raise(float_flag_invalid, status);
        return zSign ? INT64_MIN : INT64_MAX;
    }
    if (absZ1) {
        float_raise(float_flag_inexact, status);
    }
    return z;

}

/*----------------------------------------------------------------------------
| Normalizes the subnormal single-precision floating-point value represented
| by the denormalized significand `aSig'.  The normalized exponent and
| significand are stored at the locations pointed to by `zExpPtr' and
| `zSigPtr', respectively.
*----------------------------------------------------------------------------*/

static void
 normalizeFloat32Subnormal(uint32_t aSig, int *zExpPtr, uint32_t *zSigPtr)
{
    int8_t shiftCount;

    shiftCount = clz32(aSig) - 8;
    *zSigPtr = aSig<<shiftCount;
    *zExpPtr = 1 - shiftCount;

}

/*----------------------------------------------------------------------------
| Takes an abstract floating-point value having sign `zSign', exponent `zExp',
| and significand `zSig', and returns the proper single-precision floating-
| point value corresponding to the abstract input.  Ordinarily, the abstract
| value is simply rounded and packed into the single-precision format, with
| the inexact exception raised if the abstract input cannot be represented
| exactly.  However, if the abstract value is too large, the overflow and
| inexact exceptions are raised and an infinity or maximal finite value is
| returned.  If the abstract value is too small, the input value is rounded to
| a subnormal number, and the underflow and inexact exceptions are raised if
| the abstract input cannot be represented exactly as a subnormal single-
| precision floating-point number.
|     The input significand `zSig' has its binary point between bits 30
| and 29, which is 7 bits to the left of the usual location.  This shifted
| significand must be normalized or smaller.  If `zSig' is not normalized,
| `zExp' must be 0; in that case, the result returned is a subnormal number,
| and it must not require rounding.  In the usual case that `zSig' is
| normalized, `zExp' must be 1 less than the ``true'' floating-point exponent.
| The handling of underflow and overflow follows the IEC/IEEE Standard for
| Binary Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/

static float32 roundAndPackFloat32(bool zSign, int zExp, uint32_t zSig,
                                   float_status *status)
{
    int8_t roundingMode;
    bool roundNearestEven;
    int8_t roundIncrement, roundBits;
    bool isTiny;

    roundingMode = status->float_rounding_mode;
    roundNearestEven = ( roundingMode == float_round_nearest_even );
    switch (roundingMode) {
    case float_round_nearest_even:
    case float_round_ties_away:
        roundIncrement = 0x40;
        break;
    case float_round_to_zero:
        roundIncrement = 0;
        break;
    case float_round_up:
        roundIncrement = zSign ? 0 : 0x7f;
        break;
    case float_round_down:
        roundIncrement = zSign ? 0x7f : 0;
        break;
    case float_round_to_odd:
        roundIncrement = zSig & 0x80 ? 0 : 0x7f;
        break;
    default:
        abort();
        break;
    }
    roundBits = zSig & 0x7F;
    if ( 0xFD <= (uint16_t) zExp ) {
        if (    ( 0xFD < zExp )
             || (    ( zExp == 0xFD )
                  && ( (int32_t) ( zSig + roundIncrement ) < 0 ) )
           ) {
            bool overflow_to_inf = roundingMode != float_round_to_odd &&
                                   roundIncrement != 0;
            float_raise(float_flag_overflow | float_flag_inexact, status);
            return packFloat32(zSign, 0xFF, -!overflow_to_inf);
        }
        if ( zExp < 0 ) {
            if (status->flush_to_zero) {
                float_raise(float_flag_output_denormal, status);
                return packFloat32(zSign, 0, 0);
            }
            isTiny = status->tininess_before_rounding
                  || (zExp < -1)
                  || (zSig + roundIncrement < 0x80000000);
            shift32RightJamming( zSig, - zExp, &zSig );
            zExp = 0;
            roundBits = zSig & 0x7F;
            if (isTiny && roundBits) {
                float_raise(float_flag_underflow, status);
            }
            if (roundingMode == float_round_to_odd) {
                /*
                 * For round-to-odd case, the roundIncrement depends on
                 * zSig which just changed.
                 */
                roundIncrement = zSig & 0x80 ? 0 : 0x7f;
            }
        }
    }
    if (roundBits) {
        float_raise(float_flag_inexact, status);
    }
    zSig = ( zSig + roundIncrement )>>7;
    if (!(roundBits ^ 0x40) && roundNearestEven) {
        zSig &= ~1;
    }
    if ( zSig == 0 ) zExp = 0;
    return packFloat32( zSign, zExp, zSig );

}

/*----------------------------------------------------------------------------
| Takes an abstract floating-point value having sign `zSign', exponent `zExp',
| and significand `zSig', and returns the proper single-precision floating-
| point value corresponding to the abstract input.  This routine is just like
| `roundAndPackFloat32' except that `zSig' does not have to be normalized.
| Bit 31 of `zSig' must be zero, and `zExp' must be 1 less than the ``true''
| floating-point exponent.
*----------------------------------------------------------------------------*/

static float32
 normalizeRoundAndPackFloat32(bool zSign, int zExp, uint32_t zSig,
                              float_status *status)
{
    int8_t shiftCount;

    shiftCount = clz32(zSig) - 1;
    return roundAndPackFloat32(zSign, zExp - shiftCount, zSig<<shiftCount,
                               status);

}

/*----------------------------------------------------------------------------
| Normalizes the subnormal double-precision floating-point value represented
| by the denormalized significand `aSig'.  The normalized exponent and
| significand are stored at the locations pointed to by `zExpPtr' and
| `zSigPtr', respectively.
*----------------------------------------------------------------------------*/

static void
 normalizeFloat64Subnormal(uint64_t aSig, int *zExpPtr, uint64_t *zSigPtr)
{
    int8_t shiftCount;

    shiftCount = clz64(aSig) - 11;
    *zSigPtr = aSig<<shiftCount;
    *zExpPtr = 1 - shiftCount;

}

/*----------------------------------------------------------------------------
| Packs the sign `zSign', exponent `zExp', and significand `zSig' into a
| double-precision floating-point value, returning the result.  After being
| shifted into the proper positions, the three fields are simply added
| together to form the result.  This means that any integer portion of `zSig'
| will be added into the exponent.  Since a properly normalized significand
| will have an integer portion equal to 1, the `zExp' input should be 1 less
| than the desired result exponent whenever `zSig' is a complete, normalized
| significand.
*----------------------------------------------------------------------------*/

static inline float64 packFloat64(bool zSign, int zExp, uint64_t zSig)
{

    return make_float64(
        ( ( (uint64_t) zSign )<<63 ) + ( ( (uint64_t) zExp )<<52 ) + zSig);

}

/*----------------------------------------------------------------------------
| Takes an abstract floating-point value having sign `zSign', exponent `zExp',
| and significand `zSig', and returns the proper double-precision floating-
| point value corresponding to the abstract input.  Ordinarily, the abstract
| value is simply rounded and packed into the double-precision format, with
| the inexact exception raised if the abstract input cannot be represented
| exactly.  However, if the abstract value is too large, the overflow and
| inexact exceptions are raised and an infinity or maximal finite value is
| returned.  If the abstract value is too small, the input value is rounded to
| a subnormal number, and the underflow and inexact exceptions are raised if
| the abstract input cannot be represented exactly as a subnormal double-
| precision floating-point number.
|     The input significand `zSig' has its binary point between bits 62
| and 61, which is 10 bits to the left of the usual location.  This shifted
| significand must be normalized or smaller.  If `zSig' is not normalized,
| `zExp' must be 0; in that case, the result returned is a subnormal number,
| and it must not require rounding.  In the usual case that `zSig' is
| normalized, `zExp' must be 1 less than the ``true'' floating-point exponent.
| The handling of underflow and overflow follows the IEC/IEEE Standard for
| Binary Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/

static float64 roundAndPackFloat64(bool zSign, int zExp, uint64_t zSig,
                                   float_status *status)
{
    int8_t roundingMode;
    bool roundNearestEven;
    int roundIncrement, roundBits;
    bool isTiny;

    roundingMode = status->float_rounding_mode;
    roundNearestEven = ( roundingMode == float_round_nearest_even );
    switch (roundingMode) {
    case float_round_nearest_even:
    case float_round_ties_away:
        roundIncrement = 0x200;
        break;
    case float_round_to_zero:
        roundIncrement = 0;
        break;
    case float_round_up:
        roundIncrement = zSign ? 0 : 0x3ff;
        break;
    case float_round_down:
        roundIncrement = zSign ? 0x3ff : 0;
        break;
    case float_round_to_odd:
        roundIncrement = (zSig & 0x400) ? 0 : 0x3ff;
        break;
    default:
        abort();
    }
    roundBits = zSig & 0x3FF;
    if ( 0x7FD <= (uint16_t) zExp ) {
        if (    ( 0x7FD < zExp )
             || (    ( zExp == 0x7FD )
                  && ( (int64_t) ( zSig + roundIncrement ) < 0 ) )
           ) {
            bool overflow_to_inf = roundingMode != float_round_to_odd &&
                                   roundIncrement != 0;
            float_raise(float_flag_overflow | float_flag_inexact, status);
            return packFloat64(zSign, 0x7FF, -(!overflow_to_inf));
        }
        if ( zExp < 0 ) {
            if (status->flush_to_zero) {
                float_raise(float_flag_output_denormal, status);
                return packFloat64(zSign, 0, 0);
            }
            isTiny = status->tininess_before_rounding
                  || (zExp < -1)
                  || (zSig + roundIncrement < UINT64_C(0x8000000000000000));
            shift64RightJamming( zSig, - zExp, &zSig );
            zExp = 0;
            roundBits = zSig & 0x3FF;
            if (isTiny && roundBits) {
                float_raise(float_flag_underflow, status);
            }
            if (roundingMode == float_round_to_odd) {
                /*
                 * For round-to-odd case, the roundIncrement depends on
                 * zSig which just changed.
                 */
                roundIncrement = (zSig & 0x400) ? 0 : 0x3ff;
            }
        }
    }
    if (roundBits) {
        float_raise(float_flag_inexact, status);
    }
    zSig = ( zSig + roundIncrement )>>10;
    if (!(roundBits ^ 0x200) && roundNearestEven) {
        zSig &= ~1;
    }
    if ( zSig == 0 ) zExp = 0;
    return packFloat64( zSign, zExp, zSig );

}

/*----------------------------------------------------------------------------
| Takes an abstract floating-point value having sign `zSign', exponent `zExp',
| and significand `zSig', and returns the proper double-precision floating-
| point value corresponding to the abstract input.  This routine is just like
| `roundAndPackFloat64' except that `zSig' does not have to be normalized.
| Bit 63 of `zSig' must be zero, and `zExp' must be 1 less than the ``true''
| floating-point exponent.
*----------------------------------------------------------------------------*/

static float64
 normalizeRoundAndPackFloat64(bool zSign, int zExp, uint64_t zSig,
                              float_status *status)
{
    int8_t shiftCount;

    shiftCount = clz64(zSig) - 1;
    return roundAndPackFloat64(zSign, zExp - shiftCount, zSig<<shiftCount,
                               status);

}

/*----------------------------------------------------------------------------
| Normalizes the subnormal extended double-precision floating-point value
| represented by the denormalized significand `aSig'.  The normalized exponent
| and significand are stored at the locations pointed to by `zExpPtr' and
| `zSigPtr', respectively.
*----------------------------------------------------------------------------*/

void normalizeFloatx80Subnormal(uint64_t aSig, int32_t *zExpPtr,
                                uint64_t *zSigPtr)
{
    int8_t shiftCount;

    shiftCount = clz64(aSig);
    *zSigPtr = aSig<<shiftCount;
    *zExpPtr = 1 - shiftCount;
}

/*----------------------------------------------------------------------------
| Takes an abstract floating-point value having sign `zSign', exponent `zExp',
| and extended significand formed by the concatenation of `zSig0' and `zSig1',
| and returns the proper extended double-precision floating-point value
| corresponding to the abstract input.  Ordinarily, the abstract value is
| rounded and packed into the extended double-precision format, with the
| inexact exception raised if the abstract input cannot be represented
| exactly.  However, if the abstract value is too large, the overflow and
| inexact exceptions are raised and an infinity or maximal finite value is
| returned.  If the abstract value is too small, the input value is rounded to
| a subnormal number, and the underflow and inexact exceptions are raised if
| the abstract input cannot be represented exactly as a subnormal extended
| double-precision floating-point number.
|     If `roundingPrecision' is floatx80_precision_s or floatx80_precision_d,
| the result is rounded to the same number of bits as single or double
| precision, respectively.  Otherwise, the result is rounded to the full
| precision of the extended double-precision format.
|     The input significand must be normalized or smaller.  If the input
| significand is not normalized, `zExp' must be 0; in that case, the result
| returned is a subnormal number, and it must not require rounding.  The
| handling of underflow and overflow follows the IEC/IEEE Standard for Binary
| Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/

floatx80 roundAndPackFloatx80(FloatX80RoundPrec roundingPrecision, bool zSign,
                              int32_t zExp, uint64_t zSig0, uint64_t zSig1,
                              float_status *status)
{
    FloatRoundMode roundingMode;
    bool roundNearestEven, increment, isTiny;
    int64_t roundIncrement, roundMask, roundBits;

    roundingMode = status->float_rounding_mode;
    roundNearestEven = ( roundingMode == float_round_nearest_even );
    switch (roundingPrecision) {
    case floatx80_precision_x:
        goto precision80;
    case floatx80_precision_d:
        roundIncrement = UINT64_C(0x0000000000000400);
        roundMask = UINT64_C(0x00000000000007FF);
        break;
    case floatx80_precision_s:
        roundIncrement = UINT64_C(0x0000008000000000);
        roundMask = UINT64_C(0x000000FFFFFFFFFF);
        break;
    default:
        g_assert_not_reached();
    }
    zSig0 |= ( zSig1 != 0 );
    switch (roundingMode) {
    case float_round_nearest_even:
    case float_round_ties_away:
        break;
    case float_round_to_zero:
        roundIncrement = 0;
        break;
    case float_round_up:
        roundIncrement = zSign ? 0 : roundMask;
        break;
    case float_round_down:
        roundIncrement = zSign ? roundMask : 0;
        break;
    default:
        abort();
    }
    roundBits = zSig0 & roundMask;
    if ( 0x7FFD <= (uint32_t) ( zExp - 1 ) ) {
        if (    ( 0x7FFE < zExp )
             || ( ( zExp == 0x7FFE ) && ( zSig0 + roundIncrement < zSig0 ) )
           ) {
            goto overflow;
        }
        if ( zExp <= 0 ) {
            if (status->flush_to_zero) {
                float_raise(float_flag_output_denormal, status);
                return packFloatx80(zSign, 0, 0);
            }
            isTiny = status->tininess_before_rounding
                  || (zExp < 0 )
                  || (zSig0 <= zSig0 + roundIncrement);
            shift64RightJamming( zSig0, 1 - zExp, &zSig0 );
            zExp = 0;
            roundBits = zSig0 & roundMask;
            if (isTiny && roundBits) {
                float_raise(float_flag_underflow, status);
            }
            if (roundBits) {
                float_raise(float_flag_inexact, status);
            }
            zSig0 += roundIncrement;
            if ( (int64_t) zSig0 < 0 ) zExp = 1;
            roundIncrement = roundMask + 1;
            if ( roundNearestEven && ( roundBits<<1 == roundIncrement ) ) {
                roundMask |= roundIncrement;
            }
            zSig0 &= ~ roundMask;
            return packFloatx80( zSign, zExp, zSig0 );
        }
    }
    if (roundBits) {
        float_raise(float_flag_inexact, status);
    }
    zSig0 += roundIncrement;
    if ( zSig0 < roundIncrement ) {
        ++zExp;
        zSig0 = UINT64_C(0x8000000000000000);
    }
    roundIncrement = roundMask + 1;
    if ( roundNearestEven && ( roundBits<<1 == roundIncrement ) ) {
        roundMask |= roundIncrement;
    }
    zSig0 &= ~ roundMask;
    if ( zSig0 == 0 ) zExp = 0;
    return packFloatx80( zSign, zExp, zSig0 );
 precision80:
    switch (roundingMode) {
    case float_round_nearest_even:
    case float_round_ties_away:
        increment = ((int64_t)zSig1 < 0);
        break;
    case float_round_to_zero:
        increment = 0;
        break;
    case float_round_up:
        increment = !zSign && zSig1;
        break;
    case float_round_down:
        increment = zSign && zSig1;
        break;
    default:
        abort();
    }
    if ( 0x7FFD <= (uint32_t) ( zExp - 1 ) ) {
        if (    ( 0x7FFE < zExp )
             || (    ( zExp == 0x7FFE )
                  && ( zSig0 == UINT64_C(0xFFFFFFFFFFFFFFFF) )
                  && increment
                )
           ) {
            roundMask = 0;
 overflow:
            float_raise(float_flag_overflow | float_flag_inexact, status);
            if (    ( roundingMode == float_round_to_zero )
                 || ( zSign && ( roundingMode == float_round_up ) )
                 || ( ! zSign && ( roundingMode == float_round_down ) )
               ) {
                return packFloatx80( zSign, 0x7FFE, ~ roundMask );
            }
            return packFloatx80(zSign,
                                floatx80_infinity_high,
                                floatx80_infinity_low);
        }
        if ( zExp <= 0 ) {
            isTiny = status->tininess_before_rounding
                  || (zExp < 0)
                  || !increment
                  || (zSig0 < UINT64_C(0xFFFFFFFFFFFFFFFF));
            shift64ExtraRightJamming( zSig0, zSig1, 1 - zExp, &zSig0, &zSig1 );
            zExp = 0;
            if (isTiny && zSig1) {
                float_raise(float_flag_underflow, status);
            }
            if (zSig1) {
                float_raise(float_flag_inexact, status);
            }
            switch (roundingMode) {
            case float_round_nearest_even:
            case float_round_ties_away:
                increment = ((int64_t)zSig1 < 0);
                break;
            case float_round_to_zero:
                increment = 0;
                break;
            case float_round_up:
                increment = !zSign && zSig1;
                break;
            case float_round_down:
                increment = zSign && zSig1;
                break;
            default:
                abort();
            }
            if ( increment ) {
                ++zSig0;
                if (!(zSig1 << 1) && roundNearestEven) {
                    zSig0 &= ~1;
                }
                if ( (int64_t) zSig0 < 0 ) zExp = 1;
            }
            return packFloatx80( zSign, zExp, zSig0 );
        }
    }
    if (zSig1) {
        float_raise(float_flag_inexact, status);
    }
    if ( increment ) {
        ++zSig0;
        if ( zSig0 == 0 ) {
            ++zExp;
            zSig0 = UINT64_C(0x8000000000000000);
        }
        else {
            if (!(zSig1 << 1) && roundNearestEven) {
                zSig0 &= ~1;
            }
        }
    }
    else {
        if ( zSig0 == 0 ) zExp = 0;
    }
    return packFloatx80( zSign, zExp, zSig0 );

}

/*----------------------------------------------------------------------------
| Takes an abstract floating-point value having sign `zSign', exponent
| `zExp', and significand formed by the concatenation of `zSig0' and `zSig1',
| and returns the proper extended double-precision floating-point value
| corresponding to the abstract input.  This routine is just like
| `roundAndPackFloatx80' except that the input significand does not have to be
| normalized.
*----------------------------------------------------------------------------*/

floatx80 normalizeRoundAndPackFloatx80(FloatX80RoundPrec roundingPrecision,
                                       bool zSign, int32_t zExp,
                                       uint64_t zSig0, uint64_t zSig1,
                                       float_status *status)
{
    int8_t shiftCount;

    if ( zSig0 == 0 ) {
        zSig0 = zSig1;
        zSig1 = 0;
        zExp -= 64;
    }
    shiftCount = clz64(zSig0);
    shortShift128Left( zSig0, zSig1, shiftCount, &zSig0, &zSig1 );
    zExp -= shiftCount;
    return roundAndPackFloatx80(roundingPrecision, zSign, zExp,
                                zSig0, zSig1, status);

}

/*----------------------------------------------------------------------------
| Returns the least-significant 64 fraction bits of the quadruple-precision
| floating-point value `a'.
*----------------------------------------------------------------------------*/

static inline uint64_t extractFloat128Frac1( float128 a )
{

    return a.low;

}

/*----------------------------------------------------------------------------
| Returns the most-significant 48 fraction bits of the quadruple-precision
| floating-point value `a'.
*----------------------------------------------------------------------------*/

static inline uint64_t extractFloat128Frac0( float128 a )
{

    return a.high & UINT64_C(0x0000FFFFFFFFFFFF);

}

/*----------------------------------------------------------------------------
| Returns the exponent bits of the quadruple-precision floating-point value
| `a'.
*----------------------------------------------------------------------------*/

static inline int32_t extractFloat128Exp( float128 a )
{

    return ( a.high>>48 ) & 0x7FFF;

}

/*----------------------------------------------------------------------------
| Returns the sign bit of the quadruple-precision floating-point value `a'.
*----------------------------------------------------------------------------*/

static inline bool extractFloat128Sign(float128 a)
{
    return a.high >> 63;
}

/*----------------------------------------------------------------------------
| Normalizes the subnormal quadruple-precision floating-point value
| represented by the denormalized significand formed by the concatenation of
| `aSig0' and `aSig1'.  The normalized exponent is stored at the location
| pointed to by `zExpPtr'.  The most significant 49 bits of the normalized
| significand are stored at the location pointed to by `zSig0Ptr', and the
| least significant 64 bits of the normalized significand are stored at the
| location pointed to by `zSig1Ptr'.
*----------------------------------------------------------------------------*/

static void
 normalizeFloat128Subnormal(
     uint64_t aSig0,
     uint64_t aSig1,
     int32_t *zExpPtr,
     uint64_t *zSig0Ptr,
     uint64_t *zSig1Ptr
 )
{
    int8_t shiftCount;

    if ( aSig0 == 0 ) {
        shiftCount = clz64(aSig1) - 15;
        if ( shiftCount < 0 ) {
            *zSig0Ptr = aSig1>>( - shiftCount );
            *zSig1Ptr = aSig1<<( shiftCount & 63 );
        }
        else {
            *zSig0Ptr = aSig1<<shiftCount;
            *zSig1Ptr = 0;
        }
        *zExpPtr = - shiftCount - 63;
    }
    else {
        shiftCount = clz64(aSig0) - 15;
        shortShift128Left( aSig0, aSig1, shiftCount, zSig0Ptr, zSig1Ptr );
        *zExpPtr = 1 - shiftCount;
    }

}

/*----------------------------------------------------------------------------
| Packs the sign `zSign', the exponent `zExp', and the significand formed
| by the concatenation of `zSig0' and `zSig1' into a quadruple-precision
| floating-point value, returning the result.  After being shifted into the
| proper positions, the three fields `zSign', `zExp', and `zSig0' are simply
| added together to form the most significant 32 bits of the result.  This
| means that any integer portion of `zSig0' will be added into the exponent.
| Since a properly normalized significand will have an integer portion equal
| to 1, the `zExp' input should be 1 less than the desired result exponent
| whenever `zSig0' and `zSig1' concatenated form a complete, normalized
| significand.
*----------------------------------------------------------------------------*/

static inline float128
packFloat128(bool zSign, int32_t zExp, uint64_t zSig0, uint64_t zSig1)
{
    float128 z;

    z.low = zSig1;
    z.high = ((uint64_t)zSign << 63) + ((uint64_t)zExp << 48) + zSig0;
    return z;
}

/*----------------------------------------------------------------------------
| Takes an abstract floating-point value having sign `zSign', exponent `zExp',
| and extended significand formed by the concatenation of `zSig0', `zSig1',
| and `zSig2', and returns the proper quadruple-precision floating-point value
| corresponding to the abstract input.  Ordinarily, the abstract value is
| simply rounded and packed into the quadruple-precision format, with the
| inexact exception raised if the abstract input cannot be represented
| exactly.  However, if the abstract value is too large, the overflow and
| inexact exceptions are raised and an infinity or maximal finite value is
| returned.  If the abstract value is too small, the input value is rounded to
| a subnormal number, and the underflow and inexact exceptions are raised if
| the abstract input cannot be represented exactly as a subnormal quadruple-
| precision floating-point number.
|     The input significand must be normalized or smaller.  If the input
| significand is not normalized, `zExp' must be 0; in that case, the result
| returned is a subnormal number, and it must not require rounding.  In the
| usual case that the input significand is normalized, `zExp' must be 1 less
| than the ``true'' floating-point exponent.  The handling of underflow and
| overflow follows the IEC/IEEE Standard for Binary Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/

static float128 roundAndPackFloat128(bool zSign, int32_t zExp,
                                     uint64_t zSig0, uint64_t zSig1,
                                     uint64_t zSig2, float_status *status)
{
    int8_t roundingMode;
    bool roundNearestEven, increment, isTiny;

    roundingMode = status->float_rounding_mode;
    roundNearestEven = ( roundingMode == float_round_nearest_even );
    switch (roundingMode) {
    case float_round_nearest_even:
    case float_round_ties_away:
        increment = ((int64_t)zSig2 < 0);
        break;
    case float_round_to_zero:
        increment = 0;
        break;
    case float_round_up:
        increment = !zSign && zSig2;
        break;
    case float_round_down:
        increment = zSign && zSig2;
        break;
    case float_round_to_odd:
        increment = !(zSig1 & 0x1) && zSig2;
        break;
    default:
        abort();
    }
    if ( 0x7FFD <= (uint32_t) zExp ) {
        if (    ( 0x7FFD < zExp )
             || (    ( zExp == 0x7FFD )
                  && eq128(
                         UINT64_C(0x0001FFFFFFFFFFFF),
                         UINT64_C(0xFFFFFFFFFFFFFFFF),
                         zSig0,
                         zSig1
                     )
                  && increment
                )
           ) {
            float_raise(float_flag_overflow | float_flag_inexact, status);
            if (    ( roundingMode == float_round_to_zero )
                 || ( zSign && ( roundingMode == float_round_up ) )
                 || ( ! zSign && ( roundingMode == float_round_down ) )
                 || (roundingMode == float_round_to_odd)
               ) {
                return
                    packFloat128(
                        zSign,
                        0x7FFE,
                        UINT64_C(0x0000FFFFFFFFFFFF),
                        UINT64_C(0xFFFFFFFFFFFFFFFF)
                    );
            }
            return packFloat128( zSign, 0x7FFF, 0, 0 );
        }
        if ( zExp < 0 ) {
            if (status->flush_to_zero) {
                float_raise(float_flag_output_denormal, status);
                return packFloat128(zSign, 0, 0, 0);
            }
            isTiny = status->tininess_before_rounding
                  || (zExp < -1)
                  || !increment
                  || lt128(zSig0, zSig1,
                           UINT64_C(0x0001FFFFFFFFFFFF),
                           UINT64_C(0xFFFFFFFFFFFFFFFF));
            shift128ExtraRightJamming(
                zSig0, zSig1, zSig2, - zExp, &zSig0, &zSig1, &zSig2 );
            zExp = 0;
            if (isTiny && zSig2) {
                float_raise(float_flag_underflow, status);
            }
            switch (roundingMode) {
            case float_round_nearest_even:
            case float_round_ties_away:
                increment = ((int64_t)zSig2 < 0);
                break;
            case float_round_to_zero:
                increment = 0;
                break;
            case float_round_up:
                increment = !zSign && zSig2;
                break;
            case float_round_down:
                increment = zSign && zSig2;
                break;
            case float_round_to_odd:
                increment = !(zSig1 & 0x1) && zSig2;
                break;
            default:
                abort();
            }
        }
    }
    if (zSig2) {
        float_raise(float_flag_inexact, status);
    }
    if ( increment ) {
        add128( zSig0, zSig1, 0, 1, &zSig0, &zSig1 );
        if ((zSig2 + zSig2 == 0) && roundNearestEven) {
            zSig1 &= ~1;
        }
    }
    else {
        if ( ( zSig0 | zSig1 ) == 0 ) zExp = 0;
    }
    return packFloat128( zSign, zExp, zSig0, zSig1 );

}

/*----------------------------------------------------------------------------
| Takes an abstract floating-point value having sign `zSign', exponent `zExp',
| and significand formed by the concatenation of `zSig0' and `zSig1', and
| returns the proper quadruple-precision floating-point value corresponding
| to the abstract input.  This routine is just like `roundAndPackFloat128'
| except that the input significand has fewer bits and does not have to be
| normalized.  In all cases, `zExp' must be 1 less than the ``true'' floating-
| point exponent.
*----------------------------------------------------------------------------*/

static float128 normalizeRoundAndPackFloat128(bool zSign, int32_t zExp,
                                              uint64_t zSig0, uint64_t zSig1,
                                              float_status *status)
{
    int8_t shiftCount;
    uint64_t zSig2;

    if ( zSig0 == 0 ) {
        zSig0 = zSig1;
        zSig1 = 0;
        zExp -= 64;
    }
    shiftCount = clz64(zSig0) - 15;
    if ( 0 <= shiftCount ) {
        zSig2 = 0;
        shortShift128Left( zSig0, zSig1, shiftCount, &zSig0, &zSig1 );
    }
    else {
        shift128ExtraRightJamming(
            zSig0, zSig1, 0, - shiftCount, &zSig0, &zSig1, &zSig2 );
    }
    zExp -= shiftCount;
    return roundAndPackFloat128(zSign, zExp, zSig0, zSig1, zSig2, status);

}


/*----------------------------------------------------------------------------
| Returns the result of converting the 32-bit two's complement integer `a'
| to the extended double-precision floating-point format.  The conversion
| is performed according to the IEC/IEEE Standard for Binary Floating-Point
| Arithmetic.
*----------------------------------------------------------------------------*/

floatx80 int32_to_floatx80(int32_t a, float_status *status)
{
    bool zSign;
    uint32_t absA;
    int8_t shiftCount;
    uint64_t zSig;

    if ( a == 0 ) return packFloatx80( 0, 0, 0 );
    zSign = ( a < 0 );
    absA = zSign ? - a : a;
    shiftCount = clz32(absA) + 32;
    zSig = absA;
    return packFloatx80( zSign, 0x403E - shiftCount, zSig<<shiftCount );

}

/*----------------------------------------------------------------------------
| Returns the result of converting the 64-bit two's complement integer `a'
| to the extended double-precision floating-point format.  The conversion
| is performed according to the IEC/IEEE Standard for Binary Floating-Point
| Arithmetic.
*----------------------------------------------------------------------------*/

floatx80 int64_to_floatx80(int64_t a, float_status *status)
{
    bool zSign;
    uint64_t absA;
    int8_t shiftCount;

    if ( a == 0 ) return packFloatx80( 0, 0, 0 );
    zSign = ( a < 0 );
    absA = zSign ? - a : a;
    shiftCount = clz64(absA);
    return packFloatx80( zSign, 0x403E - shiftCount, absA<<shiftCount );

}

/*----------------------------------------------------------------------------
| Returns the result of converting the single-precision floating-point value
| `a' to the extended double-precision floating-point format.  The conversion
| is performed according to the IEC/IEEE Standard for Binary Floating-Point
| Arithmetic.
*----------------------------------------------------------------------------*/

floatx80 float32_to_floatx80(float32 a, float_status *status)
{
    bool aSign;
    int aExp;
    uint32_t aSig;

    a = float32_squash_input_denormal(a, status);
    aSig = extractFloat32Frac( a );
    aExp = extractFloat32Exp( a );
    aSign = extractFloat32Sign( a );
    if ( aExp == 0xFF ) {
        if (aSig) {
            floatx80 res = commonNaNToFloatx80(float32ToCommonNaN(a, status),
                                               status);
            return floatx80_silence_nan(res, status);
        }
        return packFloatx80(aSign,
                            floatx80_infinity_high,
                            floatx80_infinity_low);
    }
    if ( aExp == 0 ) {
        if ( aSig == 0 ) return packFloatx80( aSign, 0, 0 );
        normalizeFloat32Subnormal( aSig, &aExp, &aSig );
    }
    aSig |= 0x00800000;
    return packFloatx80( aSign, aExp + 0x3F80, ( (uint64_t) aSig )<<40 );

}

/*----------------------------------------------------------------------------
| Returns the remainder of the single-precision floating-point value `a'
| with respect to the corresponding value `b'.  The operation is performed
| according to the IEC/IEEE Standard for Binary Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/

float32 float32_rem(float32 a, float32 b, float_status *status)
{
    bool aSign, zSign;
    int aExp, bExp, expDiff;
    uint32_t aSig, bSig;
    uint32_t q;
    uint64_t aSig64, bSig64, q64;
    uint32_t alternateASig;
    int32_t sigMean;
    a = float32_squash_input_denormal(a, status);
    b = float32_squash_input_denormal(b, status);

    aSig = extractFloat32Frac( a );
    aExp = extractFloat32Exp( a );
    aSign = extractFloat32Sign( a );
    bSig = extractFloat32Frac( b );
    bExp = extractFloat32Exp( b );
    if ( aExp == 0xFF ) {
        if ( aSig || ( ( bExp == 0xFF ) && bSig ) ) {
            return propagateFloat32NaN(a, b, status);
        }
        float_raise(float_flag_invalid, status);
        return float32_default_nan(status);
    }
    if ( bExp == 0xFF ) {
        if (bSig) {
            return propagateFloat32NaN(a, b, status);
        }
        return a;
    }
    if ( bExp == 0 ) {
        if ( bSig == 0 ) {
            float_raise(float_flag_invalid, status);
            return float32_default_nan(status);
        }
        normalizeFloat32Subnormal( bSig, &bExp, &bSig );
    }
    if ( aExp == 0 ) {
        if ( aSig == 0 ) return a;
        normalizeFloat32Subnormal( aSig, &aExp, &aSig );
    }
    expDiff = aExp - bExp;
    aSig |= 0x00800000;
    bSig |= 0x00800000;
    if ( expDiff < 32 ) {
        aSig <<= 8;
        bSig <<= 8;
        if ( expDiff < 0 ) {
            if ( expDiff < -1 ) return a;
            aSig >>= 1;
        }
        q = ( bSig <= aSig );
        if ( q ) aSig -= bSig;
        if ( 0 < expDiff ) {
            q = ( ( (uint64_t) aSig )<<32 ) / bSig;
            q >>= 32 - expDiff;
            bSig >>= 2;
            aSig = ( ( aSig>>1 )<<( expDiff - 1 ) ) - bSig * q;
        }
        else {
            aSig >>= 2;
            bSig >>= 2;
        }
    }
    else {
        if ( bSig <= aSig ) aSig -= bSig;
        aSig64 = ( (uint64_t) aSig )<<40;
        bSig64 = ( (uint64_t) bSig )<<40;
        expDiff -= 64;
        while ( 0 < expDiff ) {
            q64 = estimateDiv128To64( aSig64, 0, bSig64 );
            q64 = ( 2 < q64 ) ? q64 - 2 : 0;
            aSig64 = - ( ( bSig * q64 )<<38 );
            expDiff -= 62;
        }
        expDiff += 64;
        q64 = estimateDiv128To64( aSig64, 0, bSig64 );
        q64 = ( 2 < q64 ) ? q64 - 2 : 0;
        q = q64>>( 64 - expDiff );
        bSig <<= 6;
        aSig = ( ( aSig64>>33 )<<( expDiff - 1 ) ) - bSig * q;
    }
    do {
        alternateASig = aSig;
        ++q;
        aSig -= bSig;
    } while ( 0 <= (int32_t) aSig );
    sigMean = aSig + alternateASig;
    if ( ( sigMean < 0 ) || ( ( sigMean == 0 ) && ( q & 1 ) ) ) {
        aSig = alternateASig;
    }
    zSign = ( (int32_t) aSig < 0 );
    if ( zSign ) aSig = - aSig;
    return normalizeRoundAndPackFloat32(aSign ^ zSign, bExp, aSig, status);
}



/*----------------------------------------------------------------------------
| Returns the binary exponential of the single-precision floating-point value
| `a'. The operation is performed according to the IEC/IEEE Standard for
| Binary Floating-Point Arithmetic.
|
| Uses the following identities:
|
| 1. -------------------------------------------------------------------------
|      x    x*ln(2)
|     2  = e
|
| 2. -------------------------------------------------------------------------
|                      2     3     4     5           n
|      x        x     x     x     x     x           x
|     e  = 1 + --- + --- + --- + --- + --- + ... + --- + ...
|               1!    2!    3!    4!    5!          n!
*----------------------------------------------------------------------------*/

static const float64 float32_exp2_coefficients[15] =
{
    const_float64( 0x3ff0000000000000ll ), /*  1 */
    const_float64( 0x3fe0000000000000ll ), /*  2 */
    const_float64( 0x3fc5555555555555ll ), /*  3 */
    const_float64( 0x3fa5555555555555ll ), /*  4 */
    const_float64( 0x3f81111111111111ll ), /*  5 */
    const_float64( 0x3f56c16c16c16c17ll ), /*  6 */
    const_float64( 0x3f2a01a01a01a01all ), /*  7 */
    const_float64( 0x3efa01a01a01a01all ), /*  8 */
    const_float64( 0x3ec71de3a556c734ll ), /*  9 */
    const_float64( 0x3e927e4fb7789f5cll ), /* 10 */
    const_float64( 0x3e5ae64567f544e4ll ), /* 11 */
    const_float64( 0x3e21eed8eff8d898ll ), /* 12 */
    const_float64( 0x3de6124613a86d09ll ), /* 13 */
    const_float64( 0x3da93974a8c07c9dll ), /* 14 */
    const_float64( 0x3d6ae7f3e733b81fll ), /* 15 */
};

float32 float32_exp2(float32 a, float_status *status)
{
    bool aSign;
    int aExp;
    uint32_t aSig;
    float64 r, x, xn;
    int i;
    a = float32_squash_input_denormal(a, status);

    aSig = extractFloat32Frac( a );
    aExp = extractFloat32Exp( a );
    aSign = extractFloat32Sign( a );

    if ( aExp == 0xFF) {
        if (aSig) {
            return propagateFloat32NaN(a, float32_zero, status);
        }
        return (aSign) ? float32_zero : a;
    }
    if (aExp == 0) {
        if (aSig == 0) return float32_one;
    }

    float_raise(float_flag_inexact, status);

    /* ******************************* */
    /* using float64 for approximation */
    /* ******************************* */
    x = float32_to_float64(a, status);
    x = float64_mul(x, float64_ln2, status);

    xn = x;
    r = float64_one;
    for (i = 0 ; i < 15 ; i++) {
        float64 f;

        f = float64_mul(xn, float32_exp2_coefficients[i], status);
        r = float64_add(r, f, status);

        xn = float64_mul(xn, x, status);
    }

    return float64_to_float32(r, status);
}

/*----------------------------------------------------------------------------
| Returns the result of converting the double-precision floating-point value
| `a' to the extended double-precision floating-point format.  The conversion
| is performed according to the IEC/IEEE Standard for Binary Floating-Point
| Arithmetic.
*----------------------------------------------------------------------------*/

floatx80 float64_to_floatx80(float64 a, float_status *status)
{
    bool aSign;
    int aExp;
    uint64_t aSig;

    a = float64_squash_input_denormal(a, status);
    aSig = extractFloat64Frac( a );
    aExp = extractFloat64Exp( a );
    aSign = extractFloat64Sign( a );
    if ( aExp == 0x7FF ) {
        if (aSig) {
            floatx80 res = commonNaNToFloatx80(float64ToCommonNaN(a, status),
                                               status);
            return floatx80_silence_nan(res, status);
        }
        return packFloatx80(aSign,
                            floatx80_infinity_high,
                            floatx80_infinity_low);
    }
    if ( aExp == 0 ) {
        if ( aSig == 0 ) return packFloatx80( aSign, 0, 0 );
        normalizeFloat64Subnormal( aSig, &aExp, &aSig );
    }
    return
        packFloatx80(
            aSign, aExp + 0x3C00, (aSig | UINT64_C(0x0010000000000000)) << 11);

}

/*----------------------------------------------------------------------------
| Returns the binary log of the single-precision floating-point value `a'.
| The operation is performed according to the IEC/IEEE Standard for Binary
| Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/
float32 float32_log2(float32 a, float_status *status)
{
    bool aSign, zSign;
    int aExp;
    uint32_t aSig, zSig, i;

    a = float32_squash_input_denormal(a, status);
    aSig = extractFloat32Frac( a );
    aExp = extractFloat32Exp( a );
    aSign = extractFloat32Sign( a );

    if ( aExp == 0 ) {
        if ( aSig == 0 ) return packFloat32( 1, 0xFF, 0 );
        normalizeFloat32Subnormal( aSig, &aExp, &aSig );
    }
    if ( aSign ) {
        float_raise(float_flag_invalid, status);
        return float32_default_nan(status);
    }
    if ( aExp == 0xFF ) {
        if (aSig) {
            return propagateFloat32NaN(a, float32_zero, status);
        }
        return a;
    }

    aExp -= 0x7F;
    aSig |= 0x00800000;
    zSign = aExp < 0;
    zSig = aExp << 23;

    for (i = 1 << 22; i > 0; i >>= 1) {
        aSig = ( (uint64_t)aSig * aSig ) >> 23;
        if ( aSig & 0x01000000 ) {
            aSig >>= 1;
            zSig |= i;
        }
    }

    if ( zSign )
        zSig = -zSig;

    return normalizeRoundAndPackFloat32(zSign, 0x85, zSig, status);
}

/*----------------------------------------------------------------------------
| Returns the remainder of the double-precision floating-point value `a'
| with respect to the corresponding value `b'.  The operation is performed
| according to the IEC/IEEE Standard for Binary Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/

float64 float64_rem(float64 a, float64 b, float_status *status)
{
    bool aSign, zSign;
    int aExp, bExp, expDiff;
    uint64_t aSig, bSig;
    uint64_t q, alternateASig;
    int64_t sigMean;

    a = float64_squash_input_denormal(a, status);
    b = float64_squash_input_denormal(b, status);
    aSig = extractFloat64Frac( a );
    aExp = extractFloat64Exp( a );
    aSign = extractFloat64Sign( a );
    bSig = extractFloat64Frac( b );
    bExp = extractFloat64Exp( b );
    if ( aExp == 0x7FF ) {
        if ( aSig || ( ( bExp == 0x7FF ) && bSig ) ) {
            return propagateFloat64NaN(a, b, status);
        }
        float_raise(float_flag_invalid, status);
        return float64_default_nan(status);
    }
    if ( bExp == 0x7FF ) {
        if (bSig) {
            return propagateFloat64NaN(a, b, status);
        }
        return a;
    }
    if ( bExp == 0 ) {
        if ( bSig == 0 ) {
            float_raise(float_flag_invalid, status);
            return float64_default_nan(status);
        }
        normalizeFloat64Subnormal( bSig, &bExp, &bSig );
    }
    if ( aExp == 0 ) {
        if ( aSig == 0 ) return a;
        normalizeFloat64Subnormal( aSig, &aExp, &aSig );
    }
    expDiff = aExp - bExp;
    aSig = (aSig | UINT64_C(0x0010000000000000)) << 11;
    bSig = (bSig | UINT64_C(0x0010000000000000)) << 11;
    if ( expDiff < 0 ) {
        if ( expDiff < -1 ) return a;
        aSig >>= 1;
    }
    q = ( bSig <= aSig );
    if ( q ) aSig -= bSig;
    expDiff -= 64;
    while ( 0 < expDiff ) {
        q = estimateDiv128To64( aSig, 0, bSig );
        q = ( 2 < q ) ? q - 2 : 0;
        aSig = - ( ( bSig>>2 ) * q );
        expDiff -= 62;
    }
    expDiff += 64;
    if ( 0 < expDiff ) {
        q = estimateDiv128To64( aSig, 0, bSig );
        q = ( 2 < q ) ? q - 2 : 0;
        q >>= 64 - expDiff;
        bSig >>= 2;
        aSig = ( ( aSig>>1 )<<( expDiff - 1 ) ) - bSig * q;
    }
    else {
        aSig >>= 2;
        bSig >>= 2;
    }
    do {
        alternateASig = aSig;
        ++q;
        aSig -= bSig;
    } while ( 0 <= (int64_t) aSig );
    sigMean = aSig + alternateASig;
    if ( ( sigMean < 0 ) || ( ( sigMean == 0 ) && ( q & 1 ) ) ) {
        aSig = alternateASig;
    }
    zSign = ( (int64_t) aSig < 0 );
    if ( zSign ) aSig = - aSig;
    return normalizeRoundAndPackFloat64(aSign ^ zSign, bExp, aSig, status);

}

/*----------------------------------------------------------------------------
| Returns the binary log of the double-precision floating-point value `a'.
| The operation is performed according to the IEC/IEEE Standard for Binary
| Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/
float64 float64_log2(float64 a, float_status *status)
{
    bool aSign, zSign;
    int aExp;
    uint64_t aSig, aSig0, aSig1, zSig, i;
    a = float64_squash_input_denormal(a, status);

    aSig = extractFloat64Frac( a );
    aExp = extractFloat64Exp( a );
    aSign = extractFloat64Sign( a );

    if ( aExp == 0 ) {
        if ( aSig == 0 ) return packFloat64( 1, 0x7FF, 0 );
        normalizeFloat64Subnormal( aSig, &aExp, &aSig );
    }
    if ( aSign ) {
        float_raise(float_flag_invalid, status);
        return float64_default_nan(status);
    }
    if ( aExp == 0x7FF ) {
        if (aSig) {
            return propagateFloat64NaN(a, float64_zero, status);
        }
        return a;
    }

    aExp -= 0x3FF;
    aSig |= UINT64_C(0x0010000000000000);
    zSign = aExp < 0;
    zSig = (uint64_t)aExp << 52;
    for (i = 1LL << 51; i > 0; i >>= 1) {
        mul64To128( aSig, aSig, &aSig0, &aSig1 );
        aSig = ( aSig0 << 12 ) | ( aSig1 >> 52 );
        if ( aSig & UINT64_C(0x0020000000000000) ) {
            aSig >>= 1;
            zSig |= i;
        }
    }

    if ( zSign )
        zSig = -zSig;
    return normalizeRoundAndPackFloat64(zSign, 0x408, zSig, status);
}

/*----------------------------------------------------------------------------
| Returns the result of converting the extended double-precision floating-
| point value `a' to the 32-bit two's complement integer format.  The
| conversion is performed according to the IEC/IEEE Standard for Binary
| Floating-Point Arithmetic---which means in particular that the conversion
| is rounded according to the current rounding mode.  If `a' is a NaN, the
| largest positive integer is returned.  Otherwise, if the conversion
| overflows, the largest integer with the same sign as `a' is returned.
*----------------------------------------------------------------------------*/

int32_t floatx80_to_int32(floatx80 a, float_status *status)
{
    bool aSign;
    int32_t aExp, shiftCount;
    uint64_t aSig;

    if (floatx80_invalid_encoding(a)) {
        float_raise(float_flag_invalid, status);
        return 1 << 31;
    }
    aSig = extractFloatx80Frac( a );
    aExp = extractFloatx80Exp( a );
    aSign = extractFloatx80Sign( a );
    if ( ( aExp == 0x7FFF ) && (uint64_t) ( aSig<<1 ) ) aSign = 0;
    shiftCount = 0x4037 - aExp;
    if ( shiftCount <= 0 ) shiftCount = 1;
    shift64RightJamming( aSig, shiftCount, &aSig );
    return roundAndPackInt32(aSign, aSig, status);

}

/*----------------------------------------------------------------------------
| Returns the result of converting the extended double-precision floating-
| point value `a' to the 32-bit two's complement integer format.  The
| conversion is performed according to the IEC/IEEE Standard for Binary
| Floating-Point Arithmetic, except that the conversion is always rounded
| toward zero.  If `a' is a NaN, the largest positive integer is returned.
| Otherwise, if the conversion overflows, the largest integer with the same
| sign as `a' is returned.
*----------------------------------------------------------------------------*/

int32_t floatx80_to_int32_round_to_zero(floatx80 a, float_status *status)
{
    bool aSign;
    int32_t aExp, shiftCount;
    uint64_t aSig, savedASig;
    int32_t z;

    if (floatx80_invalid_encoding(a)) {
        float_raise(float_flag_invalid, status);
        return 1 << 31;
    }
    aSig = extractFloatx80Frac( a );
    aExp = extractFloatx80Exp( a );
    aSign = extractFloatx80Sign( a );
    if ( 0x401E < aExp ) {
        if ( ( aExp == 0x7FFF ) && (uint64_t) ( aSig<<1 ) ) aSign = 0;
        goto invalid;
    }
    else if ( aExp < 0x3FFF ) {
        if (aExp || aSig) {
            float_raise(float_flag_inexact, status);
        }
        return 0;
    }
    shiftCount = 0x403E - aExp;
    savedASig = aSig;
    aSig >>= shiftCount;
    z = aSig;
    if ( aSign ) z = - z;
    if ( ( z < 0 ) ^ aSign ) {
 invalid:
        float_raise(float_flag_invalid, status);
        return aSign ? (int32_t) 0x80000000 : 0x7FFFFFFF;
    }
    if ( ( aSig<<shiftCount ) != savedASig ) {
        float_raise(float_flag_inexact, status);
    }
    return z;

}

/*----------------------------------------------------------------------------
| Returns the result of converting the extended double-precision floating-
| point value `a' to the 64-bit two's complement integer format.  The
| conversion is performed according to the IEC/IEEE Standard for Binary
| Floating-Point Arithmetic---which means in particular that the conversion
| is rounded according to the current rounding mode.  If `a' is a NaN,
| the largest positive integer is returned.  Otherwise, if the conversion
| overflows, the largest integer with the same sign as `a' is returned.
*----------------------------------------------------------------------------*/

int64_t floatx80_to_int64(floatx80 a, float_status *status)
{
    bool aSign;
    int32_t aExp, shiftCount;
    uint64_t aSig, aSigExtra;

    if (floatx80_invalid_encoding(a)) {
        float_raise(float_flag_invalid, status);
        return 1ULL << 63;
    }
    aSig = extractFloatx80Frac( a );
    aExp = extractFloatx80Exp( a );
    aSign = extractFloatx80Sign( a );
    shiftCount = 0x403E - aExp;
    if ( shiftCount <= 0 ) {
        if ( shiftCount ) {
            float_raise(float_flag_invalid, status);
            if (!aSign || floatx80_is_any_nan(a)) {
                return INT64_MAX;
            }
            return INT64_MIN;
        }
        aSigExtra = 0;
    }
    else {
        shift64ExtraRightJamming( aSig, 0, shiftCount, &aSig, &aSigExtra );
    }
    return roundAndPackInt64(aSign, aSig, aSigExtra, status);

}

/*----------------------------------------------------------------------------
| Returns the result of converting the extended double-precision floating-
| point value `a' to the 64-bit two's complement integer format.  The
| conversion is performed according to the IEC/IEEE Standard for Binary
| Floating-Point Arithmetic, except that the conversion is always rounded
| toward zero.  If `a' is a NaN, the largest positive integer is returned.
| Otherwise, if the conversion overflows, the largest integer with the same
| sign as `a' is returned.
*----------------------------------------------------------------------------*/

int64_t floatx80_to_int64_round_to_zero(floatx80 a, float_status *status)
{
    bool aSign;
    int32_t aExp, shiftCount;
    uint64_t aSig;
    int64_t z;

    if (floatx80_invalid_encoding(a)) {
        float_raise(float_flag_invalid, status);
        return 1ULL << 63;
    }
    aSig = extractFloatx80Frac( a );
    aExp = extractFloatx80Exp( a );
    aSign = extractFloatx80Sign( a );
    shiftCount = aExp - 0x403E;
    if ( 0 <= shiftCount ) {
        aSig &= UINT64_C(0x7FFFFFFFFFFFFFFF);
        if ( ( a.high != 0xC03E ) || aSig ) {
            float_raise(float_flag_invalid, status);
            if ( ! aSign || ( ( aExp == 0x7FFF ) && aSig ) ) {
                return INT64_MAX;
            }
        }
        return INT64_MIN;
    }
    else if ( aExp < 0x3FFF ) {
        if (aExp | aSig) {
            float_raise(float_flag_inexact, status);
        }
        return 0;
    }
    z = aSig>>( - shiftCount );
    if ( (uint64_t) ( aSig<<( shiftCount & 63 ) ) ) {
        float_raise(float_flag_inexact, status);
    }
    if ( aSign ) z = - z;
    return z;

}

/*----------------------------------------------------------------------------
| Returns the result of converting the extended double-precision floating-
| point value `a' to the single-precision floating-point format.  The
| conversion is performed according to the IEC/IEEE Standard for Binary
| Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/

float32 floatx80_to_float32(floatx80 a, float_status *status)
{
    bool aSign;
    int32_t aExp;
    uint64_t aSig;

    if (floatx80_invalid_encoding(a)) {
        float_raise(float_flag_invalid, status);
        return float32_default_nan(status);
    }
    aSig = extractFloatx80Frac( a );
    aExp = extractFloatx80Exp( a );
    aSign = extractFloatx80Sign( a );
    if ( aExp == 0x7FFF ) {
        if ( (uint64_t) ( aSig<<1 ) ) {
            float32 res = commonNaNToFloat32(floatx80ToCommonNaN(a, status),
                                             status);
            return float32_silence_nan(res, status);
        }
        return packFloat32( aSign, 0xFF, 0 );
    }
    shift64RightJamming( aSig, 33, &aSig );
    if ( aExp || aSig ) aExp -= 0x3F81;
    return roundAndPackFloat32(aSign, aExp, aSig, status);

}

/*----------------------------------------------------------------------------
| Returns the result of converting the extended double-precision floating-
| point value `a' to the double-precision floating-point format.  The
| conversion is performed according to the IEC/IEEE Standard for Binary
| Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/

float64 floatx80_to_float64(floatx80 a, float_status *status)
{
    bool aSign;
    int32_t aExp;
    uint64_t aSig, zSig;

    if (floatx80_invalid_encoding(a)) {
        float_raise(float_flag_invalid, status);
        return float64_default_nan(status);
    }
    aSig = extractFloatx80Frac( a );
    aExp = extractFloatx80Exp( a );
    aSign = extractFloatx80Sign( a );
    if ( aExp == 0x7FFF ) {
        if ( (uint64_t) ( aSig<<1 ) ) {
            float64 res = commonNaNToFloat64(floatx80ToCommonNaN(a, status),
                                             status);
            return float64_silence_nan(res, status);
        }
        return packFloat64( aSign, 0x7FF, 0 );
    }
    shift64RightJamming( aSig, 1, &zSig );
    if ( aExp || aSig ) aExp -= 0x3C01;
    return roundAndPackFloat64(aSign, aExp, zSig, status);

}

/*----------------------------------------------------------------------------
| Returns the result of converting the extended double-precision floating-
| point value `a' to the quadruple-precision floating-point format.  The
| conversion is performed according to the IEC/IEEE Standard for Binary
| Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/

float128 floatx80_to_float128(floatx80 a, float_status *status)
{
    bool aSign;
    int aExp;
    uint64_t aSig, zSig0, zSig1;

    if (floatx80_invalid_encoding(a)) {
        float_raise(float_flag_invalid, status);
        return float128_default_nan(status);
    }
    aSig = extractFloatx80Frac( a );
    aExp = extractFloatx80Exp( a );
    aSign = extractFloatx80Sign( a );
    if ( ( aExp == 0x7FFF ) && (uint64_t) ( aSig<<1 ) ) {
        float128 res = commonNaNToFloat128(floatx80ToCommonNaN(a, status),
                                           status);
        return float128_silence_nan(res, status);
    }
    shift128Right( aSig<<1, 0, 16, &zSig0, &zSig1 );
    return packFloat128( aSign, aExp, zSig0, zSig1 );

}

/*----------------------------------------------------------------------------
| Rounds the extended double-precision floating-point value `a'
| to the precision provided by floatx80_rounding_precision and returns the
| result as an extended double-precision floating-point value.
| The operation is performed according to the IEC/IEEE Standard for Binary
| Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/

floatx80 floatx80_round(floatx80 a, float_status *status)
{
    return roundAndPackFloatx80(status->floatx80_rounding_precision,
                                extractFloatx80Sign(a),
                                extractFloatx80Exp(a),
                                extractFloatx80Frac(a), 0, status);
}

/*----------------------------------------------------------------------------
| Rounds the extended double-precision floating-point value `a' to an integer,
| and returns the result as an extended quadruple-precision floating-point
| value.  The operation is performed according to the IEC/IEEE Standard for
| Binary Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/

floatx80 floatx80_round_to_int(floatx80 a, float_status *status)
{
    bool aSign;
    int32_t aExp;
    uint64_t lastBitMask, roundBitsMask;
    floatx80 z;

    if (floatx80_invalid_encoding(a)) {
        float_raise(float_flag_invalid, status);
        return floatx80_default_nan(status);
    }
    aExp = extractFloatx80Exp( a );
    if ( 0x403E <= aExp ) {
        if ( ( aExp == 0x7FFF ) && (uint64_t) ( extractFloatx80Frac( a )<<1 ) ) {
            return propagateFloatx80NaN(a, a, status);
        }
        return a;
    }
    if ( aExp < 0x3FFF ) {
        if (    ( aExp == 0 )
             && ( (uint64_t) ( extractFloatx80Frac( a ) ) == 0 ) ) {
            return a;
        }
        float_raise(float_flag_inexact, status);
        aSign = extractFloatx80Sign( a );
        switch (status->float_rounding_mode) {
         case float_round_nearest_even:
            if ( ( aExp == 0x3FFE ) && (uint64_t) ( extractFloatx80Frac( a )<<1 )
               ) {
                return
                    packFloatx80( aSign, 0x3FFF, UINT64_C(0x8000000000000000));
            }
            break;
        case float_round_ties_away:
            if (aExp == 0x3FFE) {
                return packFloatx80(aSign, 0x3FFF, UINT64_C(0x8000000000000000));
            }
            break;
         case float_round_down:
            return
                  aSign ?
                      packFloatx80( 1, 0x3FFF, UINT64_C(0x8000000000000000))
                : packFloatx80( 0, 0, 0 );
         case float_round_up:
            return
                  aSign ? packFloatx80( 1, 0, 0 )
                : packFloatx80( 0, 0x3FFF, UINT64_C(0x8000000000000000));

        case float_round_to_zero:
            break;
        default:
            g_assert_not_reached();
        }
        return packFloatx80( aSign, 0, 0 );
    }
    lastBitMask = 1;
    lastBitMask <<= 0x403E - aExp;
    roundBitsMask = lastBitMask - 1;
    z = a;
    switch (status->float_rounding_mode) {
    case float_round_nearest_even:
        z.low += lastBitMask>>1;
        if ((z.low & roundBitsMask) == 0) {
            z.low &= ~lastBitMask;
        }
        break;
    case float_round_ties_away:
        z.low += lastBitMask >> 1;
        break;
    case float_round_to_zero:
        break;
    case float_round_up:
        if (!extractFloatx80Sign(z)) {
            z.low += roundBitsMask;
        }
        break;
    case float_round_down:
        if (extractFloatx80Sign(z)) {
            z.low += roundBitsMask;
        }
        break;
    default:
        abort();
    }
    z.low &= ~ roundBitsMask;
    if ( z.low == 0 ) {
        ++z.high;
        z.low = UINT64_C(0x8000000000000000);
    }
    if (z.low != a.low) {
        float_raise(float_flag_inexact, status);
    }
    return z;

}

/*----------------------------------------------------------------------------
| Returns the result of adding the absolute values of the extended double-
| precision floating-point values `a' and `b'.  If `zSign' is 1, the sum is
| negated before being returned.  `zSign' is ignored if the result is a NaN.
| The addition is performed according to the IEC/IEEE Standard for Binary
| Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/

static floatx80 addFloatx80Sigs(floatx80 a, floatx80 b, bool zSign,
                                float_status *status)
{
    int32_t aExp, bExp, zExp;
    uint64_t aSig, bSig, zSig0, zSig1;
    int32_t expDiff;

    aSig = extractFloatx80Frac( a );
    aExp = extractFloatx80Exp( a );
    bSig = extractFloatx80Frac( b );
    bExp = extractFloatx80Exp( b );
    expDiff = aExp - bExp;
    if ( 0 < expDiff ) {
        if ( aExp == 0x7FFF ) {
            if ((uint64_t)(aSig << 1)) {
                return propagateFloatx80NaN(a, b, status);
            }
            return a;
        }
        if ( bExp == 0 ) --expDiff;
        shift64ExtraRightJamming( bSig, 0, expDiff, &bSig, &zSig1 );
        zExp = aExp;
    }
    else if ( expDiff < 0 ) {
        if ( bExp == 0x7FFF ) {
            if ((uint64_t)(bSig << 1)) {
                return propagateFloatx80NaN(a, b, status);
            }
            return packFloatx80(zSign,
                                floatx80_infinity_high,
                                floatx80_infinity_low);
        }
        if ( aExp == 0 ) ++expDiff;
        shift64ExtraRightJamming( aSig, 0, - expDiff, &aSig, &zSig1 );
        zExp = bExp;
    }
    else {
        if ( aExp == 0x7FFF ) {
            if ( (uint64_t) ( ( aSig | bSig )<<1 ) ) {
                return propagateFloatx80NaN(a, b, status);
            }
            return a;
        }
        zSig1 = 0;
        zSig0 = aSig + bSig;
        if ( aExp == 0 ) {
            if ((aSig | bSig) & UINT64_C(0x8000000000000000) && zSig0 < aSig) {
                /* At least one of the values is a pseudo-denormal,
                 * and there is a carry out of the result.  */
                zExp = 1;
                goto shiftRight1;
            }
            if (zSig0 == 0) {
                return packFloatx80(zSign, 0, 0);
            }
            normalizeFloatx80Subnormal( zSig0, &zExp, &zSig0 );
            goto roundAndPack;
        }
        zExp = aExp;
        goto shiftRight1;
    }
    zSig0 = aSig + bSig;
    if ( (int64_t) zSig0 < 0 ) goto roundAndPack;
 shiftRight1:
    shift64ExtraRightJamming( zSig0, zSig1, 1, &zSig0, &zSig1 );
    zSig0 |= UINT64_C(0x8000000000000000);
    ++zExp;
 roundAndPack:
    return roundAndPackFloatx80(status->floatx80_rounding_precision,
                                zSign, zExp, zSig0, zSig1, status);
}

/*----------------------------------------------------------------------------
| Returns the result of subtracting the absolute values of the extended
| double-precision floating-point values `a' and `b'.  If `zSign' is 1, the
| difference is negated before being returned.  `zSign' is ignored if the
| result is a NaN.  The subtraction is performed according to the IEC/IEEE
| Standard for Binary Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/

static floatx80 subFloatx80Sigs(floatx80 a, floatx80 b, bool zSign,
                                float_status *status)
{
    int32_t aExp, bExp, zExp;
    uint64_t aSig, bSig, zSig0, zSig1;
    int32_t expDiff;

    aSig = extractFloatx80Frac( a );
    aExp = extractFloatx80Exp( a );
    bSig = extractFloatx80Frac( b );
    bExp = extractFloatx80Exp( b );
    expDiff = aExp - bExp;
    if ( 0 < expDiff ) goto aExpBigger;
    if ( expDiff < 0 ) goto bExpBigger;
    if ( aExp == 0x7FFF ) {
        if ( (uint64_t) ( ( aSig | bSig )<<1 ) ) {
            return propagateFloatx80NaN(a, b, status);
        }
        float_raise(float_flag_invalid, status);
        return floatx80_default_nan(status);
    }
    if ( aExp == 0 ) {
        aExp = 1;
        bExp = 1;
    }
    zSig1 = 0;
    if ( bSig < aSig ) goto aBigger;
    if ( aSig < bSig ) goto bBigger;
    return packFloatx80(status->float_rounding_mode == float_round_down, 0, 0);
 bExpBigger:
    if ( bExp == 0x7FFF ) {
        if ((uint64_t)(bSig << 1)) {
            return propagateFloatx80NaN(a, b, status);
        }
        return packFloatx80(zSign ^ 1, floatx80_infinity_high,
                            floatx80_infinity_low);
    }
    if ( aExp == 0 ) ++expDiff;
    shift128RightJamming( aSig, 0, - expDiff, &aSig, &zSig1 );
 bBigger:
    sub128( bSig, 0, aSig, zSig1, &zSig0, &zSig1 );
    zExp = bExp;
    zSign ^= 1;
    goto normalizeRoundAndPack;
 aExpBigger:
    if ( aExp == 0x7FFF ) {
        if ((uint64_t)(aSig << 1)) {
            return propagateFloatx80NaN(a, b, status);
        }
        return a;
    }
    if ( bExp == 0 ) --expDiff;
    shift128RightJamming( bSig, 0, expDiff, &bSig, &zSig1 );
 aBigger:
    sub128( aSig, 0, bSig, zSig1, &zSig0, &zSig1 );
    zExp = aExp;
 normalizeRoundAndPack:
    return normalizeRoundAndPackFloatx80(status->floatx80_rounding_precision,
                                         zSign, zExp, zSig0, zSig1, status);
}

/*----------------------------------------------------------------------------
| Returns the result of adding the extended double-precision floating-point
| values `a' and `b'.  The operation is performed according to the IEC/IEEE
| Standard for Binary Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/

floatx80 floatx80_add(floatx80 a, floatx80 b, float_status *status)
{
    bool aSign, bSign;

    if (floatx80_invalid_encoding(a) || floatx80_invalid_encoding(b)) {
        float_raise(float_flag_invalid, status);
        return floatx80_default_nan(status);
    }
    aSign = extractFloatx80Sign( a );
    bSign = extractFloatx80Sign( b );
    if ( aSign == bSign ) {
        return addFloatx80Sigs(a, b, aSign, status);
    }
    else {
        return subFloatx80Sigs(a, b, aSign, status);
    }

}

/*----------------------------------------------------------------------------
| Returns the result of subtracting the extended double-precision floating-
| point values `a' and `b'.  The operation is performed according to the
| IEC/IEEE Standard for Binary Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/

floatx80 floatx80_sub(floatx80 a, floatx80 b, float_status *status)
{
    bool aSign, bSign;

    if (floatx80_invalid_encoding(a) || floatx80_invalid_encoding(b)) {
        float_raise(float_flag_invalid, status);
        return floatx80_default_nan(status);
    }
    aSign = extractFloatx80Sign( a );
    bSign = extractFloatx80Sign( b );
    if ( aSign == bSign ) {
        return subFloatx80Sigs(a, b, aSign, status);
    }
    else {
        return addFloatx80Sigs(a, b, aSign, status);
    }

}

/*----------------------------------------------------------------------------
| Returns the result of multiplying the extended double-precision floating-
| point values `a' and `b'.  The operation is performed according to the
| IEC/IEEE Standard for Binary Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/

floatx80 floatx80_mul(floatx80 a, floatx80 b, float_status *status)
{
    bool aSign, bSign, zSign;
    int32_t aExp, bExp, zExp;
    uint64_t aSig, bSig, zSig0, zSig1;

    if (floatx80_invalid_encoding(a) || floatx80_invalid_encoding(b)) {
        float_raise(float_flag_invalid, status);
        return floatx80_default_nan(status);
    }
    aSig = extractFloatx80Frac( a );
    aExp = extractFloatx80Exp( a );
    aSign = extractFloatx80Sign( a );
    bSig = extractFloatx80Frac( b );
    bExp = extractFloatx80Exp( b );
    bSign = extractFloatx80Sign( b );
    zSign = aSign ^ bSign;
    if ( aExp == 0x7FFF ) {
        if (    (uint64_t) ( aSig<<1 )
             || ( ( bExp == 0x7FFF ) && (uint64_t) ( bSig<<1 ) ) ) {
            return propagateFloatx80NaN(a, b, status);
        }
        if ( ( bExp | bSig ) == 0 ) goto invalid;
        return packFloatx80(zSign, floatx80_infinity_high,
                                   floatx80_infinity_low);
    }
    if ( bExp == 0x7FFF ) {
        if ((uint64_t)(bSig << 1)) {
            return propagateFloatx80NaN(a, b, status);
        }
        if ( ( aExp | aSig ) == 0 ) {
 invalid:
            float_raise(float_flag_invalid, status);
            return floatx80_default_nan(status);
        }
        return packFloatx80(zSign, floatx80_infinity_high,
                                   floatx80_infinity_low);
    }
    if ( aExp == 0 ) {
        if ( aSig == 0 ) return packFloatx80( zSign, 0, 0 );
        normalizeFloatx80Subnormal( aSig, &aExp, &aSig );
    }
    if ( bExp == 0 ) {
        if ( bSig == 0 ) return packFloatx80( zSign, 0, 0 );
        normalizeFloatx80Subnormal( bSig, &bExp, &bSig );
    }
    zExp = aExp + bExp - 0x3FFE;
    mul64To128( aSig, bSig, &zSig0, &zSig1 );
    if ( 0 < (int64_t) zSig0 ) {
        shortShift128Left( zSig0, zSig1, 1, &zSig0, &zSig1 );
        --zExp;
    }
    return roundAndPackFloatx80(status->floatx80_rounding_precision,
                                zSign, zExp, zSig0, zSig1, status);
}

/*----------------------------------------------------------------------------
| Returns the result of dividing the extended double-precision floating-point
| value `a' by the corresponding value `b'.  The operation is performed
| according to the IEC/IEEE Standard for Binary Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/

floatx80 floatx80_div(floatx80 a, floatx80 b, float_status *status)
{
    bool aSign, bSign, zSign;
    int32_t aExp, bExp, zExp;
    uint64_t aSig, bSig, zSig0, zSig1;
    uint64_t rem0, rem1, rem2, term0, term1, term2;

    if (floatx80_invalid_encoding(a) || floatx80_invalid_encoding(b)) {
        float_raise(float_flag_invalid, status);
        return floatx80_default_nan(status);
    }
    aSig = extractFloatx80Frac( a );
    aExp = extractFloatx80Exp( a );
    aSign = extractFloatx80Sign( a );
    bSig = extractFloatx80Frac( b );
    bExp = extractFloatx80Exp( b );
    bSign = extractFloatx80Sign( b );
    zSign = aSign ^ bSign;
    if ( aExp == 0x7FFF ) {
        if ((uint64_t)(aSig << 1)) {
            return propagateFloatx80NaN(a, b, status);
        }
        if ( bExp == 0x7FFF ) {
            if ((uint64_t)(bSig << 1)) {
                return propagateFloatx80NaN(a, b, status);
            }
            goto invalid;
        }
        return packFloatx80(zSign, floatx80_infinity_high,
                                   floatx80_infinity_low);
    }
    if ( bExp == 0x7FFF ) {
        if ((uint64_t)(bSig << 1)) {
            return propagateFloatx80NaN(a, b, status);
        }
        return packFloatx80( zSign, 0, 0 );
    }
    if ( bExp == 0 ) {
        if ( bSig == 0 ) {
            if ( ( aExp | aSig ) == 0 ) {
 invalid:
                float_raise(float_flag_invalid, status);
                return floatx80_default_nan(status);
            }
            float_raise(float_flag_divbyzero, status);
            return packFloatx80(zSign, floatx80_infinity_high,
                                       floatx80_infinity_low);
        }
        normalizeFloatx80Subnormal( bSig, &bExp, &bSig );
    }
    if ( aExp == 0 ) {
        if ( aSig == 0 ) return packFloatx80( zSign, 0, 0 );
        normalizeFloatx80Subnormal( aSig, &aExp, &aSig );
    }
    zExp = aExp - bExp + 0x3FFE;
    rem1 = 0;
    if ( bSig <= aSig ) {
        shift128Right( aSig, 0, 1, &aSig, &rem1 );
        ++zExp;
    }
    zSig0 = estimateDiv128To64( aSig, rem1, bSig );
    mul64To128( bSig, zSig0, &term0, &term1 );
    sub128( aSig, rem1, term0, term1, &rem0, &rem1 );
    while ( (int64_t) rem0 < 0 ) {
        --zSig0;
        add128( rem0, rem1, 0, bSig, &rem0, &rem1 );
    }
    zSig1 = estimateDiv128To64( rem1, 0, bSig );
    if ( (uint64_t) ( zSig1<<1 ) <= 8 ) {
        mul64To128( bSig, zSig1, &term1, &term2 );
        sub128( rem1, 0, term1, term2, &rem1, &rem2 );
        while ( (int64_t) rem1 < 0 ) {
            --zSig1;
            add128( rem1, rem2, 0, bSig, &rem1, &rem2 );
        }
        zSig1 |= ( ( rem1 | rem2 ) != 0 );
    }
    return roundAndPackFloatx80(status->floatx80_rounding_precision,
                                zSign, zExp, zSig0, zSig1, status);
}

/*----------------------------------------------------------------------------
| Returns the remainder of the extended double-precision floating-point value
| `a' with respect to the corresponding value `b'.  The operation is performed
| according to the IEC/IEEE Standard for Binary Floating-Point Arithmetic,
| if 'mod' is false; if 'mod' is true, return the remainder based on truncating
| the quotient toward zero instead.  '*quotient' is set to the low 64 bits of
| the absolute value of the integer quotient.
*----------------------------------------------------------------------------*/

floatx80 floatx80_modrem(floatx80 a, floatx80 b, bool mod, uint64_t *quotient,
                         float_status *status)
{
    bool aSign, zSign;
    int32_t aExp, bExp, expDiff, aExpOrig;
    uint64_t aSig0, aSig1, bSig;
    uint64_t q, term0, term1, alternateASig0, alternateASig1;

    *quotient = 0;
    if (floatx80_invalid_encoding(a) || floatx80_invalid_encoding(b)) {
        float_raise(float_flag_invalid, status);
        return floatx80_default_nan(status);
    }
    aSig0 = extractFloatx80Frac( a );
    aExpOrig = aExp = extractFloatx80Exp( a );
    aSign = extractFloatx80Sign( a );
    bSig = extractFloatx80Frac( b );
    bExp = extractFloatx80Exp( b );
    if ( aExp == 0x7FFF ) {
        if (    (uint64_t) ( aSig0<<1 )
             || ( ( bExp == 0x7FFF ) && (uint64_t) ( bSig<<1 ) ) ) {
            return propagateFloatx80NaN(a, b, status);
        }
        goto invalid;
    }
    if ( bExp == 0x7FFF ) {
        if ((uint64_t)(bSig << 1)) {
            return propagateFloatx80NaN(a, b, status);
        }
        if (aExp == 0 && aSig0 >> 63) {
            /*
             * Pseudo-denormal argument must be returned in normalized
             * form.
             */
            return packFloatx80(aSign, 1, aSig0);
        }
        return a;
    }
    if ( bExp == 0 ) {
        if ( bSig == 0 ) {
 invalid:
            float_raise(float_flag_invalid, status);
            return floatx80_default_nan(status);
        }
        normalizeFloatx80Subnormal( bSig, &bExp, &bSig );
    }
    if ( aExp == 0 ) {
        if ( aSig0 == 0 ) return a;
        normalizeFloatx80Subnormal( aSig0, &aExp, &aSig0 );
    }
    zSign = aSign;
    expDiff = aExp - bExp;
    aSig1 = 0;
    if ( expDiff < 0 ) {
        if ( mod || expDiff < -1 ) {
            if (aExp == 1 && aExpOrig == 0) {
                /*
                 * Pseudo-denormal argument must be returned in
                 * normalized form.
                 */
                return packFloatx80(aSign, aExp, aSig0);
            }
            return a;
        }
        shift128Right( aSig0, 0, 1, &aSig0, &aSig1 );
        expDiff = 0;
    }
    *quotient = q = ( bSig <= aSig0 );
    if ( q ) aSig0 -= bSig;
    expDiff -= 64;
    while ( 0 < expDiff ) {
        q = estimateDiv128To64( aSig0, aSig1, bSig );
        q = ( 2 < q ) ? q - 2 : 0;
        mul64To128( bSig, q, &term0, &term1 );
        sub128( aSig0, aSig1, term0, term1, &aSig0, &aSig1 );
        shortShift128Left( aSig0, aSig1, 62, &aSig0, &aSig1 );
        expDiff -= 62;
        *quotient <<= 62;
        *quotient += q;
    }
    expDiff += 64;
    if ( 0 < expDiff ) {
        q = estimateDiv128To64( aSig0, aSig1, bSig );
        q = ( 2 < q ) ? q - 2 : 0;
        q >>= 64 - expDiff;
        mul64To128( bSig, q<<( 64 - expDiff ), &term0, &term1 );
        sub128( aSig0, aSig1, term0, term1, &aSig0, &aSig1 );
        shortShift128Left( 0, bSig, 64 - expDiff, &term0, &term1 );
        while ( le128( term0, term1, aSig0, aSig1 ) ) {
            ++q;
            sub128( aSig0, aSig1, term0, term1, &aSig0, &aSig1 );
        }
        if (expDiff < 64) {
            *quotient <<= expDiff;
        } else {
            *quotient = 0;
        }
        *quotient += q;
    }
    else {
        term1 = 0;
        term0 = bSig;
    }
    if (!mod) {
        sub128( term0, term1, aSig0, aSig1, &alternateASig0, &alternateASig1 );
        if (    lt128( alternateASig0, alternateASig1, aSig0, aSig1 )
                || (    eq128( alternateASig0, alternateASig1, aSig0, aSig1 )
                        && ( q & 1 ) )
            ) {
            aSig0 = alternateASig0;
            aSig1 = alternateASig1;
            zSign = ! zSign;
            ++*quotient;
        }
    }
    return
        normalizeRoundAndPackFloatx80(
            floatx80_precision_x, zSign, bExp + expDiff, aSig0, aSig1, status);

}

/*----------------------------------------------------------------------------
| Returns the remainder of the extended double-precision floating-point value
| `a' with respect to the corresponding value `b'.  The operation is performed
| according to the IEC/IEEE Standard for Binary Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/

floatx80 floatx80_rem(floatx80 a, floatx80 b, float_status *status)
{
    uint64_t quotient;
    return floatx80_modrem(a, b, false, &quotient, status);
}

/*----------------------------------------------------------------------------
| Returns the remainder of the extended double-precision floating-point value
| `a' with respect to the corresponding value `b', with the quotient truncated
| toward zero.
*----------------------------------------------------------------------------*/

floatx80 floatx80_mod(floatx80 a, floatx80 b, float_status *status)
{
    uint64_t quotient;
    return floatx80_modrem(a, b, true, &quotient, status);
}

/*----------------------------------------------------------------------------
| Returns the square root of the extended double-precision floating-point
| value `a'.  The operation is performed according to the IEC/IEEE Standard
| for Binary Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/

floatx80 floatx80_sqrt(floatx80 a, float_status *status)
{
    bool aSign;
    int32_t aExp, zExp;
    uint64_t aSig0, aSig1, zSig0, zSig1, doubleZSig0;
    uint64_t rem0, rem1, rem2, rem3, term0, term1, term2, term3;

    if (floatx80_invalid_encoding(a)) {
        float_raise(float_flag_invalid, status);
        return floatx80_default_nan(status);
    }
    aSig0 = extractFloatx80Frac( a );
    aExp = extractFloatx80Exp( a );
    aSign = extractFloatx80Sign( a );
    if ( aExp == 0x7FFF ) {
        if ((uint64_t)(aSig0 << 1)) {
            return propagateFloatx80NaN(a, a, status);
        }
        if ( ! aSign ) return a;
        goto invalid;
    }
    if ( aSign ) {
        if ( ( aExp | aSig0 ) == 0 ) return a;
 invalid:
        float_raise(float_flag_invalid, status);
        return floatx80_default_nan(status);
    }
    if ( aExp == 0 ) {
        if ( aSig0 == 0 ) return packFloatx80( 0, 0, 0 );
        normalizeFloatx80Subnormal( aSig0, &aExp, &aSig0 );
    }
    zExp = ( ( aExp - 0x3FFF )>>1 ) + 0x3FFF;
    zSig0 = estimateSqrt32( aExp, aSig0>>32 );
    shift128Right( aSig0, 0, 2 + ( aExp & 1 ), &aSig0, &aSig1 );
    zSig0 = estimateDiv128To64( aSig0, aSig1, zSig0<<32 ) + ( zSig0<<30 );
    doubleZSig0 = zSig0<<1;
    mul64To128( zSig0, zSig0, &term0, &term1 );
    sub128( aSig0, aSig1, term0, term1, &rem0, &rem1 );
    while ( (int64_t) rem0 < 0 ) {
        --zSig0;
        doubleZSig0 -= 2;
        add128( rem0, rem1, zSig0>>63, doubleZSig0 | 1, &rem0, &rem1 );
    }
    zSig1 = estimateDiv128To64( rem1, 0, doubleZSig0 );
    if ( ( zSig1 & UINT64_C(0x3FFFFFFFFFFFFFFF) ) <= 5 ) {
        if ( zSig1 == 0 ) zSig1 = 1;
        mul64To128( doubleZSig0, zSig1, &term1, &term2 );
        sub128( rem1, 0, term1, term2, &rem1, &rem2 );
        mul64To128( zSig1, zSig1, &term2, &term3 );
        sub192( rem1, rem2, 0, 0, term2, term3, &rem1, &rem2, &rem3 );
        while ( (int64_t) rem1 < 0 ) {
            --zSig1;
            shortShift128Left( 0, zSig1, 1, &term2, &term3 );
            term3 |= 1;
            term2 |= doubleZSig0;
            add192( rem1, rem2, rem3, 0, term2, term3, &rem1, &rem2, &rem3 );
        }
        zSig1 |= ( ( rem1 | rem2 | rem3 ) != 0 );
    }
    shortShift128Left( 0, zSig1, 1, &zSig0, &zSig1 );
    zSig0 |= doubleZSig0;
    return roundAndPackFloatx80(status->floatx80_rounding_precision,
                                0, zExp, zSig0, zSig1, status);
}

/*----------------------------------------------------------------------------
| Returns the result of converting the quadruple-precision floating-point
| value `a' to the extended double-precision floating-point format.  The
| conversion is performed according to the IEC/IEEE Standard for Binary
| Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/

floatx80 float128_to_floatx80(float128 a, float_status *status)
{
    bool aSign;
    int32_t aExp;
    uint64_t aSig0, aSig1;

    aSig1 = extractFloat128Frac1( a );
    aSig0 = extractFloat128Frac0( a );
    aExp = extractFloat128Exp( a );
    aSign = extractFloat128Sign( a );
    if ( aExp == 0x7FFF ) {
        if ( aSig0 | aSig1 ) {
            floatx80 res = commonNaNToFloatx80(float128ToCommonNaN(a, status),
                                               status);
            return floatx80_silence_nan(res, status);
        }
        return packFloatx80(aSign, floatx80_infinity_high,
                                   floatx80_infinity_low);
    }
    if ( aExp == 0 ) {
        if ( ( aSig0 | aSig1 ) == 0 ) return packFloatx80( aSign, 0, 0 );
        normalizeFloat128Subnormal( aSig0, aSig1, &aExp, &aSig0, &aSig1 );
    }
    else {
        aSig0 |= UINT64_C(0x0001000000000000);
    }
    shortShift128Left( aSig0, aSig1, 15, &aSig0, &aSig1 );
    return roundAndPackFloatx80(floatx80_precision_x, aSign, aExp, aSig0, aSig1, status);

}

/*----------------------------------------------------------------------------
| Returns the remainder of the quadruple-precision floating-point value `a'
| with respect to the corresponding value `b'.  The operation is performed
| according to the IEC/IEEE Standard for Binary Floating-Point Arithmetic.
*----------------------------------------------------------------------------*/

float128 float128_rem(float128 a, float128 b, float_status *status)
{
    bool aSign, zSign;
    int32_t aExp, bExp, expDiff;
    uint64_t aSig0, aSig1, bSig0, bSig1, q, term0, term1, term2;
    uint64_t allZero, alternateASig0, alternateASig1, sigMean1;
    int64_t sigMean0;

    aSig1 = extractFloat128Frac1( a );
    aSig0 = extractFloat128Frac0( a );
    aExp = extractFloat128Exp( a );
    aSign = extractFloat128Sign( a );
    bSig1 = extractFloat128Frac1( b );
    bSig0 = extractFloat128Frac0( b );
    bExp = extractFloat128Exp( b );
    if ( aExp == 0x7FFF ) {
        if (    ( aSig0 | aSig1 )
             || ( ( bExp == 0x7FFF ) && ( bSig0 | bSig1 ) ) ) {
            return propagateFloat128NaN(a, b, status);
        }
        goto invalid;
    }
    if ( bExp == 0x7FFF ) {
        if (bSig0 | bSig1) {
            return propagateFloat128NaN(a, b, status);
        }
        return a;
    }
    if ( bExp == 0 ) {
        if ( ( bSig0 | bSig1 ) == 0 ) {
 invalid:
            float_raise(float_flag_invalid, status);
            return float128_default_nan(status);
        }
        normalizeFloat128Subnormal( bSig0, bSig1, &bExp, &bSig0, &bSig1 );
    }
    if ( aExp == 0 ) {
        if ( ( aSig0 | aSig1 ) == 0 ) return a;
        normalizeFloat128Subnormal( aSig0, aSig1, &aExp, &aSig0, &aSig1 );
    }
    expDiff = aExp - bExp;
    if ( expDiff < -1 ) return a;
    shortShift128Left(
        aSig0 | UINT64_C(0x0001000000000000),
        aSig1,
        15 - ( expDiff < 0 ),
        &aSig0,
        &aSig1
    );
    shortShift128Left(
        bSig0 | UINT64_C(0x0001000000000000), bSig1, 15, &bSig0, &bSig1 );
    q = le128( bSig0, bSig1, aSig0, aSig1 );
    if ( q ) sub128( aSig0, aSig1, bSig0, bSig1, &aSig0, &aSig1 );
    expDiff -= 64;
    while ( 0 < expDiff ) {
        q = estimateDiv128To64( aSig0, aSig1, bSig0 );
        q = ( 4 < q ) ? q - 4 : 0;
        mul128By64To192( bSig0, bSig1, q, &term0, &term1, &term2 );
        shortShift192Left( term0, term1, term2, 61, &term1, &term2, &allZero );
        shortShift128Left( aSig0, aSig1, 61, &aSig0, &allZero );
        sub128( aSig0, 0, term1, term2, &aSig0, &aSig1 );
        expDiff -= 61;
    }
    if ( -64 < expDiff ) {
        q = estimateDiv128To64( aSig0, aSig1, bSig0 );
        q = ( 4 < q ) ? q - 4 : 0;
        q >>= - expDiff;
        shift128Right( bSig0, bSig1, 12, &bSig0, &bSig1 );
        expDiff += 52;
        if ( expDiff < 0 ) {
            shift128Right( aSig0, aSig1, - expDiff, &aSig0, &aSig1 );
        }
        else {
            shortShift128Left( aSig0, aSig1, expDiff, &aSig0, &aSig1 );
        }
        mul128By64To192( bSig0, bSig1, q, &term0, &term1, &term2 );
        sub128( aSig0, aSig1, term1, term2, &aSig0, &aSig1 );
    }
    else {
        shift128Right( aSig0, aSig1, 12, &aSig0, &aSig1 );
        shift128Right( bSig0, bSig1, 12, &bSig0, &bSig1 );
    }
    do {
        alternateASig0 = aSig0;
        alternateASig1 = aSig1;
        ++q;
        sub128( aSig0, aSig1, bSig0, bSig1, &aSig0, &aSig1 );
    } while ( 0 <= (int64_t) aSig0 );
    add128(
        aSig0, aSig1, alternateASig0, alternateASig1, (uint64_t *)&sigMean0, &sigMean1 );
    if (    ( sigMean0 < 0 )
         || ( ( ( sigMean0 | sigMean1 ) == 0 ) && ( q & 1 ) ) ) {
        aSig0 = alternateASig0;
        aSig1 = alternateASig1;
    }
    zSign = ( (int64_t) aSig0 < 0 );
    if ( zSign ) sub128( 0, 0, aSig0, aSig1, &aSig0, &aSig1 );
    return normalizeRoundAndPackFloat128(aSign ^ zSign, bExp - 4, aSig0, aSig1,
                                         status);
}

static inline FloatRelation
floatx80_compare_internal(floatx80 a, floatx80 b, bool is_quiet,
                          float_status *status)
{
    bool aSign, bSign;

    if (floatx80_invalid_encoding(a) || floatx80_invalid_encoding(b)) {
        float_raise(float_flag_invalid, status);
        return float_relation_unordered;
    }
    if (( ( extractFloatx80Exp( a ) == 0x7fff ) &&
          ( extractFloatx80Frac( a )<<1 ) ) ||
        ( ( extractFloatx80Exp( b ) == 0x7fff ) &&
          ( extractFloatx80Frac( b )<<1 ) )) {
        if (!is_quiet ||
            floatx80_is_signaling_nan(a, status) ||
            floatx80_is_signaling_nan(b, status)) {
            float_raise(float_flag_invalid, status);
        }
        return float_relation_unordered;
    }
    aSign = extractFloatx80Sign( a );
    bSign = extractFloatx80Sign( b );
    if ( aSign != bSign ) {

        if ( ( ( (uint16_t) ( ( a.high | b.high ) << 1 ) ) == 0) &&
             ( ( a.low | b.low ) == 0 ) ) {
            /* zero case */
            return float_relation_equal;
        } else {
            return 1 - (2 * aSign);
        }
    } else {
        /* Normalize pseudo-denormals before comparison.  */
        if ((a.high & 0x7fff) == 0 && a.low & UINT64_C(0x8000000000000000)) {
            ++a.high;
        }
        if ((b.high & 0x7fff) == 0 && b.low & UINT64_C(0x8000000000000000)) {
            ++b.high;
        }
        if (a.low == b.low && a.high == b.high) {
            return float_relation_equal;
        } else {
            return 1 - 2 * (aSign ^ ( lt128( a.high, a.low, b.high, b.low ) ));
        }
    }
}

FloatRelation floatx80_compare(floatx80 a, floatx80 b, float_status *status)
{
    return floatx80_compare_internal(a, b, 0, status);
}

FloatRelation floatx80_compare_quiet(floatx80 a, floatx80 b,
                                     float_status *status)
{
    return floatx80_compare_internal(a, b, 1, status);
}

floatx80 floatx80_scalbn(floatx80 a, int n, float_status *status)
{
    bool aSign;
    int32_t aExp;
    uint64_t aSig;

    if (floatx80_invalid_encoding(a)) {
        float_raise(float_flag_invalid, status);
        return floatx80_default_nan(status);
    }
    aSig = extractFloatx80Frac( a );
    aExp = extractFloatx80Exp( a );
    aSign = extractFloatx80Sign( a );

    if ( aExp == 0x7FFF ) {
        if ( aSig<<1 ) {
            return propagateFloatx80NaN(a, a, status);
        }
        return a;
    }

    if (aExp == 0) {
        if (aSig == 0) {
            return a;
        }
        aExp++;
    }

    if (n > 0x10000) {
        n = 0x10000;
    } else if (n < -0x10000) {
        n = -0x10000;
    }

    aExp += n;
    return normalizeRoundAndPackFloatx80(status->floatx80_rounding_precision,
                                         aSign, aExp, aSig, 0, status);
}

static void __attribute__((constructor)) softfloat_init(void)
{
    union_float64 ua, ub, uc, ur;

    if (QEMU_NO_HARDFLOAT) {
        return;
    }
    /*
     * Test that the host's FMA is not obviously broken. For example,
     * glibc < 2.23 can perform an incorrect FMA on certain hosts; see
     *   https://sourceware.org/bugzilla/show_bug.cgi?id=13304
     */
    ua.s = 0x0020000000000001ULL;
    ub.s = 0x3ca0000000000000ULL;
    uc.s = 0x0020000000000000ULL;
    ur.h = fma(ua.h, ub.h, uc.h);
    if (ur.s != 0x0020000000000001ULL) {
        force_soft_fma = true;
    }
}

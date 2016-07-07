/* Copyright 2016 Brian Smith.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHORS DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

#include "gfp_internal.h"

#include <string.h>

#include "../bn/internal.h"
#include "../internal.h"

#include "gfp_limbs.inl"

 /* XXX: Here we assume that the conversion from |GFp_Carry| to |GFp_Limb|
  * is constant-time, but we haven't verified that assumption. TODO: Fix it so
  * we don't need to make that assumption. */


typedef GFp_Limb Elem[P384_LIMBS];
typedef GFp_Limb ScalarMont[P384_LIMBS];
typedef GFp_Limb Scalar[P384_LIMBS];


/* Prototypes to avoid -Wmissing-prototypes warnings. */
void GFp_p384_elem_add(Elem r, const Elem a, const Elem b);
void GFp_p384_elem_div_by_2(Elem r, const Elem a);
void GFp_p384_elem_mul_mont(Elem r, const Elem a, const Elem b);
void GFp_p384_elem_sub(Elem r, const Elem a, const Elem b);
void GFp_p384_scalar_inv_to_mont(ScalarMont r, const Scalar a);
void GFp_p384_scalar_mul_mont(ScalarMont r, const ScalarMont a,
                              const ScalarMont b);


static const BN_ULONG Q[P384_LIMBS] = {
  TOBN(0x00000000, 0xffffffff),
  TOBN(0xffffffff, 0x00000000),
  TOBN(0xffffffff, 0xfffffffe),
  TOBN(0xffffffff, 0xffffffff),
  TOBN(0xffffffff, 0xffffffff),
  TOBN(0xffffffff, 0xffffffff),
};

static const BN_ULONG N[P384_LIMBS] = {
  TOBN(0xecec196a, 0xccc52973),
  TOBN(0x581a0db2, 0x48b0a77a),
  TOBN(0xc7634d81, 0xf4372ddf),
  TOBN(0xffffffff, 0xffffffff),
  TOBN(0xffffffff, 0xffffffff),
  TOBN(0xffffffff, 0xffffffff),
};

OPENSSL_COMPILE_ASSERT(sizeof(size_t) == sizeof(GFp_Limb),
                       size_t_and_gfp_limb_are_different_sizes);

OPENSSL_COMPILE_ASSERT(sizeof(size_t) == sizeof(BN_ULONG),
                       size_t_and_bn_ulong_are_different_sizes);


/* XXX: MSVC for x86 warns when it fails to inline these functions it should
 * probably inline. */
#if defined(_MSC_VER)  && defined(OPENSSL_X86)
#define INLINE_IF_POSSIBLE __forceinline
#else
#define INLINE_IF_POSSIBLE inline
#endif


static INLINE_IF_POSSIBLE void copy_conditional(Elem r, const Elem a,
                                                const GFp_Limb condition) {
  for (size_t i = 0; i < P384_LIMBS; ++i) {
    r[i] = constant_time_select_size_t(condition, a[i], r[i]);
  }
}


static void elem_add(Elem r, const Elem a, const Elem b) {
  GFp_Limb carry =
      constant_time_is_nonzero_size_t(gfp_limbs_add(r, a, b, P384_LIMBS));
  Elem adjusted;
  GFp_Limb no_borrow =
      constant_time_is_zero_size_t(gfp_limbs_sub(adjusted, r, Q, P384_LIMBS));
  copy_conditional(r, adjusted,
                   constant_time_select_size_t(carry, carry, no_borrow));
}

static void elem_sub(Elem r, const Elem a, const Elem b) {
  GFp_Limb borrow =
    constant_time_is_nonzero_size_t(gfp_limbs_sub(r, a, b, P384_LIMBS));
  Elem adjusted;
  (void)gfp_limbs_add(adjusted, r, Q, P384_LIMBS);
  copy_conditional(r, adjusted, borrow);
}

static void elem_div_by_2(Elem r, const Elem a) {
  /* Consider the case where `a` is even. Then we can shift `a` right one bit
   * and the result will still be valid because we didn't lose any bits and so
   * `(a >> 1) * 2 == a (mod q)`, which is the invariant we must satisfy.
   *
   * The remainder of this comment is considering the case where `a` is odd.
   *
   * Since `a` is odd, it isn't the case that `(a >> 1) * 2 == a (mod q)`
   * because the lowest bit is lost during the shift. For example, consider:
   *
   * ```python
   * q = 2**384 - 2**128 - 2**96 + 2**32 - 1
   * a = 2**383
   * two_a = a * 2 % q
   * assert two_a == 0x100000000ffffffffffffffff00000001
   * ```
   *
   * Notice there how `(2 * a) % q` wrapped around to a smaller odd value. When
   * we divide `two_a` by two (mod q), we need to get the value `2**383`, which
   * we obviously can't get with just a right shift.
   *
   * `q` is odd, and `a` is odd, so `a + q` is even. We could calculate
   * `(a + q) >> 1` and then reduce it mod `q`. However, we then we would have
   * to keep track of an extra most significant bit. We can avoid that by
   * instead calculating `(a >> 1) + ((q + 1) >> 1)`. The `1` in `q + 1` is the
   * least significant bit of `a`. `q + 1` is even, which means it can be
   * shifted without losing any bits. Since `q` is odd, `q - 1` is even, so the
   * largest odd field element is `q - 2`. Thus we know that `a <= q - 2`. We
   * know `(q + 1) >> 1` is `(q + 1) / 2` since (`q + 1`) is even. The value of
   * `a >> 1` is `(a - 1)/2` since the shift will drop the least significant
   * bit of `a`, which is 1. Thus:
   *
   * sum  =  ((q + 1) >> 1) + (a >> 1)
   * sum  =  (q + 1)/2 + (a >> 1)       (substituting (q + 1)/2)
   *     <=  (q + 1)/2 + (q - 2 - 1)/2  (substituting a <= q - 2)
   *     <=  (q + 1)/2 + (q - 3)/2      (simplifying)
   *     <=  (q + 1 + q - 3)/2          (factoring out the common divisor)
   *     <=  (2q - 2)/2                 (simplifying)
   *     <=  q - 1                      (simplifying)
   *
   * Thus, no reduction of the sum mod `q` is necessary. */

  GFp_Limb is_odd = constant_time_is_nonzero_size_t(a[0] & 1);

  /* r = a >> 1. */
  GFp_Limb carry = a[P384_LIMBS - 1] & 1;
  r[P384_LIMBS - 1] = a[P384_LIMBS - 1] >> 1;
  for (size_t i = 1; i < P384_LIMBS; ++i) {
    GFp_Limb new_carry = a[P384_LIMBS - i - 1];
    r[P384_LIMBS - i - 1] =
        (a[P384_LIMBS - i - 1] >> 1) | (carry << (GFp_LIMB_BITS - 1));
    carry = new_carry;
  }

  static const Elem Q_PLUS_1_SHR_1 = {
    TOBN(0x00000000, 0x80000000), TOBN(0x7fffffff, 0x80000000),
    TOBN(0xffffffff, 0xffffffff), TOBN(0xffffffff, 0xffffffff),
    TOBN(0xffffffff, 0xffffffff), TOBN(0x7fffffff, 0xffffffff),
  };

  Elem adjusted;
  BN_ULONG carry2 = gfp_limbs_add(adjusted, r, Q_PLUS_1_SHR_1, P384_LIMBS);
#if defined(NDEBUG)
  (void)carry2;
#endif
  assert(carry2 == 0);

  copy_conditional(r, adjusted, is_odd);
}

static inline void elem_mul_mont(Elem r, const Elem a, const Elem b) {
  static const BN_ULONG Q_N0[] = {
    BN_MONT_CTX_N0(0x1, 0x1)
  };
  /* XXX: Not (clearly) constant-time; inefficient. TODO: Add a dedicated
   * squaring routine. */
  bn_mul_mont(r, a, b, Q, Q_N0, P384_LIMBS);
}

static inline void elem_mul_by_2(Elem r, const Elem a) {
  elem_add(r, a, a);
}

static INLINE_IF_POSSIBLE void elem_mul_by_3(Elem r, const Elem a) {
  /* XXX: inefficient. TODO: Replace with an integrated shift + add. */
  Elem doubled;
  elem_add(doubled, a, a);
  elem_add(r, doubled, a);
}


void GFp_p384_elem_add(Elem r, const Elem a, const Elem b) {
  elem_add(r, a, b);
}

void GFp_p384_elem_sub(Elem r, const Elem a, const Elem b) {
  elem_sub(r, a, b);
}

void GFp_p384_elem_div_by_2(Elem r, const Elem a) {
  elem_div_by_2(r, a);
}

void GFp_p384_elem_mul_mont(Elem r, const Elem a, const Elem b) {
  elem_mul_mont(r, a, b);
}


void GFp_p384_scalar_mul_mont(ScalarMont r, const ScalarMont a,
                              const ScalarMont b) {
  static const BN_ULONG N_N0[] = {
    BN_MONT_CTX_N0(0x6ed46089, 0xe88fdc45)
  };
  /* XXX: Inefficient. TODO: Add dedicated multiplication routine. */
  bn_mul_mont(r, a, b, N, N_N0, P384_LIMBS);
}

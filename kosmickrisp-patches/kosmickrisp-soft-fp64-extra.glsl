/* SPDX-License-Identifier: MIT */
/* Binary64 operations over integer significands. Rounding happens once, after
 * the exact quotient remainder or fused product/addend has been retained. */
const uint64_t kk_f64_sign = 0x8000000000000000UL;
const uint64_t kk_f64_inf = 0x7ff0000000000000UL;
const uint64_t kk_f64_nan = 0x7ff8000000000000UL;
const uint64_t kk_f64_fraction = 0x000fffffffffffffUL;
const uint64_t kk_f64_hidden = 0x0010000000000000UL;

void kk_f64_normalize(uint64_t a, out uint64_t sig, out int exponent)
{
   sig = a & kk_f64_fraction;
   int e = int((a >> 52) & 2047UL);
   exponent = e - 1023;
   if (e == 0) {
      exponent = -1022;
      while (sig < kk_f64_hidden) { sig <<= 1; --exponent; }
   } else {
      sig |= kk_f64_hidden;
   }
}

uint64_t kk_f64_div(uint64_t a, uint64_t b)
{
   uint64_t sign = (a ^ b) & kk_f64_sign;
   uint64_t aa = a & ~kk_f64_sign, bb = b & ~kk_f64_sign;
   if (aa > kk_f64_inf || bb > kk_f64_inf) return kk_f64_nan;
   if (aa == kk_f64_inf) return bb == kk_f64_inf ? kk_f64_nan : sign | kk_f64_inf;
   if (bb == kk_f64_inf) return sign;
   if (bb == 0UL) return aa == 0UL ? kk_f64_nan : sign | kk_f64_inf;
   if (aa == 0UL) return sign;
   uint64_t sa, sb; int ea, eb;
   kk_f64_normalize(aa, sa, ea); kk_f64_normalize(bb, sb, eb);
   int exponent = ea - eb;
   if (sa < sb) { sa <<= 1; --exponent; }
   uint64_t q = kk_f64_hidden, rem = sa - sb;
   for (int i = 51; i >= 0; --i) {
      rem <<= 1;
      if (rem >= sb) { rem -= sb; q |= 1UL << uint(i); }
   }
   if (exponent < -1022) {
      int shift = -1022 - exponent;
      if (shift > 53) return sign;
      uint64_t retained = q >> uint(shift);
      uint64_t lost = q & ((1UL << uint(shift)) - 1UL);
      uint64_t midpoint = 1UL << uint(shift - 1);
      if (lost > midpoint || (lost == midpoint && (rem != 0UL || (retained & 1UL) != 0UL))) ++retained;
      return sign | retained;
   }
   if (rem + rem > sb || (rem + rem == sb && (q & 1UL) != 0UL)) ++q;
   if (q == (kk_f64_hidden << 1)) { q >>= 1; ++exponent; }
   if (exponent > 1023) return sign | kk_f64_inf;
   return sign | (uint64_t(exponent + 1023) << 52) | (q & kk_f64_fraction);
}

uint64_t kk_f64_rcp(uint64_t a) { return kk_f64_div(0x3ff0000000000000UL, a); }

/* Little-endian 128-bit limbs are sufficient for the complete 106-bit product,
 * an aligned addend and rounding bits. A discarded tail is kept separately. */
uvec4 kk_f64_shl128(uvec4 a, int count)
{
   if (count >= 128) return uvec4(0);
   uint words = uint(count) >> 5, bits = uint(count) & 31u;
   uvec4 r = uvec4(0);
   for (uint i = 0u; i < 4u; ++i) {
      if (i >= words) {
         r[i] = a[i - words] << bits;
         if (bits != 0u && i > words) r[i] |= a[i - words - 1u] >> ((32u - bits) & 31u);
      }
   }
   return r;
}

uvec4 kk_f64_shr128(uvec4 a, int count)
{
   if (count >= 128) return uvec4(0);
   uint words = uint(count) >> 5, bits = uint(count) & 31u;
   uvec4 r = uvec4(0);
   for (uint i = 0u; i < 4u; ++i) {
      if (i + words < 4u) {
         r[i] = a[i + words] >> bits;
         if (bits != 0u && i + words + 1u < 4u) r[i] |= a[i + words + 1u] << ((32u - bits) & 31u);
      }
   }
   return r;
}

bool kk_f64_low128(uvec4 a, int count)
{
   if (count <= 0) return false;
   if (count >= 128) return any(notEqual(a, uvec4(0)));
   uint words = uint(count) >> 5, bits = uint(count) & 31u;
   for (uint i = 0u; i < 4u; ++i) {
      if (i < words && a[i] != 0u) return true;
      if (i == words && bits != 0u && (a[i] & ((1u << bits) - 1u)) != 0u) return true;
   }
   return false;
}

uvec4 kk_f64_add128(uvec4 a, uvec4 b)
{
   uvec4 r; uint64_t carry = 0UL;
   for (uint i = 0u; i < 4u; ++i) {
      uint64_t sum = uint64_t(a[i]) + uint64_t(b[i]) + carry;
      r[i] = uint(sum); carry = sum >> 32;
   }
   return r;
}

uvec4 kk_f64_sub128(uvec4 a, uvec4 b)
{
   uvec4 r; uint64_t borrow = 0UL;
   for (uint i = 0u; i < 4u; ++i) {
      uint64_t sub = uint64_t(b[i]) + borrow;
      r[i] = uint(uint64_t(a[i]) - sub);
      borrow = uint64_t(uint64_t(a[i]) < sub);
   }
   return r;
}

int kk_f64_compare128(uvec4 a, uvec4 b)
{
   for (int i = 3; i >= 0; --i) {
      if (a[i] > b[i]) return 1;
      if (a[i] < b[i]) return -1;
   }
   return 0;
}

uint64_t kk_f64_round128(uvec4 a, int shift, bool tail)
{
   if (shift <= 0) {
      uvec4 v = kk_f64_shl128(a, -shift);
      return packUint2x32(v.xy);
   }
   if (shift > 128) return 0UL;
   uvec4 v = kk_f64_shr128(a, shift);
   uint64_t q = packUint2x32(v.xy);
   int bit = shift - 1;
   bool midpoint = (a[uint(bit) >> 5] & (1u << (uint(bit) & 31u))) != 0u;
   if (midpoint && (tail || kk_f64_low128(a, bit) || (q & 1UL) != 0UL)) ++q;
   return q;
}

uint64_t kk_f64_pack128(uvec4 a, int exponent, bool tail, uint64_t sign)
{
   int top = -1;
   for (int i = 3; i >= 0; --i) if (a[i] != 0u) { top = i * 32 + findMSB(a[i]); break; }
   if (top < 0) return sign;
   int e = exponent + top - 126;
   if (e < -1022) return sign | kk_f64_round128(a, -exponent - 948, tail);
   uint64_t q = kk_f64_round128(a, top - 52, tail);
   if (q >= (kk_f64_hidden << 1)) { q >>= 1; ++e; }
   if (e > 1023) return sign | kk_f64_inf;
   return sign | (uint64_t(e + 1023) << 52) | (q & kk_f64_fraction);
}

uint64_t kk_f64_fma(uint64_t a, uint64_t b, uint64_t c)
{
   uint64_t aa = a & ~kk_f64_sign, bb = b & ~kk_f64_sign, cc = c & ~kk_f64_sign;
   uint64_t psign = (a ^ b) & kk_f64_sign, csign = c & kk_f64_sign;
   if (aa > kk_f64_inf || bb > kk_f64_inf || cc > kk_f64_inf) return kk_f64_nan;
   if ((aa == kk_f64_inf && bb == 0UL) || (bb == kk_f64_inf && aa == 0UL)) return kk_f64_nan;
   if (aa == kk_f64_inf || bb == kk_f64_inf) return cc == kk_f64_inf && psign != csign ? kk_f64_nan : psign | kk_f64_inf;
   if (cc == kk_f64_inf) return c;
   if (aa == 0UL || bb == 0UL) return cc == 0UL ? psign & csign : c;
   uint64_t sa, sb; int ea, eb;
   kk_f64_normalize(aa, sa, ea); kk_f64_normalize(bb, sb, eb);
   uint z0, z1, z2, z3;
   __mul64To128(uint(sa >> 32), uint(sa), uint(sb >> 32), uint(sb), z0, z1, z2, z3);
   uvec4 product = uvec4(z3, z2, z1, z0);
   int ptop = (product.w & 512u) != 0u ? 105 : 104;
   int pe = ea + eb + ptop - 104;
   product = kk_f64_shl128(product, 126 - ptop);
   if (cc == 0UL) return kk_f64_pack128(product, pe, false, psign);
   uint64_t sc; int ce; kk_f64_normalize(cc, sc, ce);
   uvec4 addend = kk_f64_shl128(uvec4(unpackUint2x32(sc), 0u, 0u), 74);
   int e = max(pe, ce);
   bool plost = kk_f64_low128(product, e - pe), clost = kk_f64_low128(addend, e - ce);
   product = kk_f64_shr128(product, e - pe); addend = kk_f64_shr128(addend, e - ce);
   if (psign == csign) return kk_f64_pack128(kk_f64_add128(product, addend), e, plost || clost, psign);
   int order = kk_f64_compare128(product, addend);
   if (order == 0) return 0UL;
   uvec4 difference; bool tail; uint64_t sign;
   if (order > 0) {
      difference = kk_f64_sub128(product, addend); sign = psign; tail = plost || clost;
      if (clost) difference = kk_f64_sub128(difference, uvec4(1u, 0u, 0u, 0u));
   } else {
      difference = kk_f64_sub128(addend, product); sign = csign; tail = plost || clost;
      if (plost) difference = kk_f64_sub128(difference, uvec4(1u, 0u, 0u, 0u));
   }
   return kk_f64_pack128(difference, e, tail, sign);
}

uint64_t kk_f64_ldexp(uint64_t a, int scale)
{
   uint64_t aa = a & ~kk_f64_sign, sign = a & kk_f64_sign;
   if (aa == 0UL || aa >= kk_f64_inf) return a;
   if (scale > 4096) return sign | kk_f64_inf;
   if (scale < -4096) return sign;
   uint64_t sig; int e; kk_f64_normalize(aa, sig, e); e += scale;
   if (e > 1023) return sign | kk_f64_inf;
   if (e >= -1022) return sign | (uint64_t(e + 1023) << 52) | (sig & kk_f64_fraction);
   int shift = -1022 - e;
   if (shift > 53) return sign;
   uint64_t q = sig >> uint(shift), lost = sig & ((1UL << uint(shift)) - 1UL), midpoint = 1UL << uint(shift - 1);
   if (lost > midpoint || (lost == midpoint && (q & 1UL) != 0UL)) ++q;
   return sign | q;
}

uint64_t kk_f64_to_integer(uint64_t a)
{
   int e = int((a >> 52) & 2047UL) - 1023;
   if (e < 0 || e > 63) return 0UL; /* Out-of-range conversions are undefined. */
   uint64_t sig = (a & kk_f64_fraction) | kk_f64_hidden;
   uint64_t magnitude = e < 52 ? sig >> uint(52 - e) : sig << uint(e - 52);
   return (a & kk_f64_sign) != 0UL ? 0UL - magnitude : magnitude;
}

uint64_t kk_f64_rsq(uint64_t a) { return kk_f64_rcp(__fsqrt64(a)); }

uint64_t kk_f64_round_even(uint64_t a)
{
   uint64_t sign = a & kk_f64_sign, aa = a & ~kk_f64_sign;
   int e = int(aa >> 52) - 1023;
   if (e >= 52) return a;
   if (e < 0) return aa <= 0x3fe0000000000000UL ? sign : sign | 0x3ff0000000000000UL;
   uint shift = uint(52 - e);
   uint64_t mask = (1UL << shift) - 1UL, lost = aa & mask;
   uint64_t retained = aa & ~mask, midpoint = 1UL << (shift - 1u);
   if (lost > midpoint || (lost == midpoint && ((retained >> shift) & 1UL) != 0UL)) retained += 1UL << shift;
   return sign | retained;
}

uint kk_f64_to_half_mode(uint64_t a, bool rtz)
{
   uint sign = uint(a >> 48) & 0x8000u;
   uint64_t aa = a & ~kk_f64_sign;
   if (aa >= kk_f64_inf) return sign | (aa > kk_f64_inf ? 0x7e00u : 0x7c00u);
   if (aa == 0UL) return sign;
   uint64_t sig; int e; kk_f64_normalize(aa, sig, e);
   if (e > 15) return sign | (rtz ? 0x7bffu : 0x7c00u);
   int shift = e < -14 ? 28 - e : 42;
   if (shift > 53) return sign;
   uint64_t q = sig >> uint(shift), lost = sig & ((1UL << uint(shift)) - 1UL), midpoint = 1UL << uint(shift - 1);
   if (!rtz && (lost > midpoint || (lost == midpoint && (q & 1UL) != 0UL))) ++q;
   if (e < -14) return sign | uint(q);
   if (q == 2048UL) { q >>= 1; ++e; }
   if (e > 15) return sign | (rtz ? 0x7bffu : 0x7c00u);
   return sign | (uint(e + 15) << 10) | (uint(q) & 1023u);
}

uint kk_f64_to_half(uint64_t a) { return kk_f64_to_half_mode(a, false); }
uint kk_f64_to_half_rtz(uint64_t a) { return kk_f64_to_half_mode(a, true); }

uint64_t kk_half_to_f64(uint a)
{
   uint64_t sign = uint64_t(a & 0x8000u) << 48;
   uint sig = a & 1023u; int e = int((a >> 10) & 31u);
   if (e == 31) return sign | (sig == 0u ? kk_f64_inf : kk_f64_nan);
   if (e == 0) {
      if (sig == 0u) return sign;
      e = -14;
      while (sig < 1024u) { sig <<= 1; --e; }
   } else e -= 15;
   return sign | (uint64_t(e + 1023) << 52) | (uint64_t(sig & 1023u) << 42);
}

uint64_t kk_f64_frexp_sig(uint64_t a)
{
   uint64_t aa = a & ~kk_f64_sign;
   if (aa == 0UL || aa >= kk_f64_inf) return a;
   uint64_t sig; int e; kk_f64_normalize(aa, sig, e);
   return (a & kk_f64_sign) | 0x3fe0000000000000UL | (sig & kk_f64_fraction);
}

int kk_f64_frexp_exp(uint64_t a)
{
   uint64_t aa = a & ~kk_f64_sign;
   if (aa == 0UL || aa >= kk_f64_inf) return 0;
   uint64_t sig; int e; kk_f64_normalize(aa, sig, e);
   return e + 1;
}

uint64_t kk_f64_rem(uint64_t a, uint64_t b)
{
   uint64_t aa = a & ~kk_f64_sign, bb = b & ~kk_f64_sign, sign = a & kk_f64_sign;
   if (aa >= kk_f64_inf || bb > kk_f64_inf || bb == 0UL) return kk_f64_nan;
   if (aa == 0UL || bb == kk_f64_inf || aa < bb) return a;
   uint64_t sa, sb; int ea, eb; kk_f64_normalize(aa, sa, ea); kk_f64_normalize(bb, sb, eb);
   for (int i = ea - eb; i > 0; --i) { if (sa >= sb) sa -= sb; sa <<= 1; }
   if (sa >= sb) sa -= sb;
   return kk_f64_pack128(uvec4(unpackUint2x32(sa), 0u, 0u), eb + 74, false, sign);
}

uint64_t kk_f64_mod(uint64_t a, uint64_t b)
{
   uint64_t r = kk_f64_rem(a, b);
   if ((r & ~kk_f64_sign) == 0UL) return b & kk_f64_sign;
   return (a ^ b) >= kk_f64_sign ? __fadd64(r, b) : r;
}

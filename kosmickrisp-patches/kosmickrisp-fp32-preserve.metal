// SPDX-License-Identifier: MIT
// Gradual-underflow arithmetic for explicit SPIR-V DenormPreserve 32.
// Only the affected shader receives these helpers. Integer exponent adjustment
// never submits a subnormal operand to an Apple floating-point instruction.
// Each exact path rounds once, with guard, round and sticky information.
#pragma METAL fp math_mode(safe)
struct kk_dp_number {
   uint sign;
   uint mant;
   int exp;
};
inline uint kk_dp_abs(float a) { return as_type<uint>(a) & 0x7fffffffu; }
inline float kk_dp_bits(uint a) { return as_type<float>(a); }
inline bool kk_dp_sub(float a)
{
   uint m = kk_dp_abs(a);
   return m != 0u && m < 0x800000u;
}
inline kk_dp_number kk_dp_decode(float a)
{
   uint bits = as_type<uint>(a), mag = bits & 0x7fffffffu, e = mag >> 23,
        m = mag & 0x7fffffu;
   if (e)
      return {bits & 0x80000000u, m | 0x800000u, int(e) - 150};
   if (!m)
      return {bits & 0x80000000u, 0u, 0};
   int shift = int(clz(m)) - 8;
   return {bits & 0x80000000u, m << shift, -149 - shift};
}
inline ulong kk_dp_jam(ulong a, int shift)
{
   if (shift <= 0)
      return a << (-shift);
   if (shift >= 64)
      return ulong(a != 0);
   return (a >> shift) | ulong((a << (64 - shift)) != 0);
}
inline float kk_dp_pack(uint sign, ulong mant, int exponent)
{
   if (!mant)
      return kk_dp_bits(sign);
   int top = 63 - int(clz(mant)), e = exponent + top;
   int shift = max(top - 23, -149 - exponent);
   ulong rounded;
   if (shift > 0) {
      if (shift > 64)
         rounded = 0;
      else {
         ulong upper = shift == 64 ? 0 : mant >> shift;
         ulong midpoint = 1ul << (shift - 1),
               remainder = mant & (midpoint | (midpoint - 1));
         rounded = upper + ulong(remainder > midpoint ||
                                 (remainder == midpoint && (upper & 1ul)));
      }
   } else
      rounded = mant << (-shift);
   if (e < -126)
      return kk_dp_bits(sign | uint(rounded));
   if (rounded >= 0x1000000ul) {
      rounded >>= 1;
      ++e;
   }
   if (e > 127)
      return kk_dp_bits(sign | 0x7f800000u);
   return kk_dp_bits(sign | (uint(e + 127) << 23) |
                     (uint(rounded) & 0x7fffffu));
}
inline float kk_dp_combine(uint as, ulong am, int ae, uint bs, ulong bm, int be)
{
   if (!am && !bm)
      return kk_dp_bits(as & bs);
   if (!am)
      return kk_dp_pack(bs, bm, be);
   if (!bm)
      return kk_dp_pack(as, am, ae);
   int e = max(ae + 63 - int(clz(am)), be + 63 - int(clz(bm))) - 61;
   ulong a = kk_dp_jam(am, e - ae), b = kk_dp_jam(bm, e - be);
   if (as == bs)
      return kk_dp_pack(as, a + b, e);
   if (a == b)
      return 0.0f;
   return a > b ? kk_dp_pack(as, a - b, e) : kk_dp_pack(bs, b - a, e);
}
inline float kk_dp_add(float a, float b)
{
   float r = a + b;
   if (!kk_dp_sub(a) && !kk_dp_sub(b) && kk_dp_abs(r) >= 0x800000u)
      return r;
   if (kk_dp_abs(a) >= 0x7f800000u || kk_dp_abs(b) >= 0x7f800000u)
      return r;
   kk_dp_number x = kk_dp_decode(a), y = kk_dp_decode(b);
   return kk_dp_combine(x.sign, x.mant, x.exp, y.sign, y.mant, y.exp);
}
inline float kk_dp_subtract(float a, float b)
{
   return kk_dp_add(a, kk_dp_bits(as_type<uint>(b) ^ 0x80000000u));
}
// TwoProduct on normalized operands: h+l is the exact 48-bit product.
// Both terms and their integer-scaled conversions are normal FP32 values.
inline ulong kk_dp_product(uint a, uint b)
{
   float x = kk_dp_bits(0x3f800000u | (a & 0x7fffffu)),
         y = kk_dp_bits(0x3f800000u | (b & 0x7fffffu));
   float h = x * y, l = precise::fma(x, y, -h);
   return ulong(long(h * 0x1p46f) + long(l * 0x1p46f));
}
inline float kk_dp_nan(float a)
{
   return kk_dp_bits(as_type<uint>(a) | 0x00400000u);
}
inline float kk_dp_mul(float a, float b)
{
   uint am = kk_dp_abs(a), bm = kk_dp_abs(b),
        sign = (as_type<uint>(a) ^ as_type<uint>(b)) & 0x80000000u;
   if (am > 0x7f800000u)
      return kk_dp_nan(a);
   if (bm > 0x7f800000u)
      return kk_dp_nan(b);
   if (am == 0x7f800000u || bm == 0x7f800000u)
      return kk_dp_bits((am == 0u || bm == 0u) ? 0x7fc00000u
                                               : sign | 0x7f800000u);
   if (!am || !bm)
      return kk_dp_bits(sign);
   float r = a * b;
   if (am >= 0x800000u && bm >= 0x800000u && kk_dp_abs(r) >= 0x800000u)
      return r;
   kk_dp_number x = kk_dp_decode(a), y = kk_dp_decode(b);
   return kk_dp_pack(sign, kk_dp_product(x.mant, y.mant), x.exp + y.exp);
}
inline float kk_dp_fma(float a, float b, float c)
{
   uint am = kk_dp_abs(a), bm = kk_dp_abs(b), cm = kk_dp_abs(c),
        sign = (as_type<uint>(a) ^ as_type<uint>(b)) & 0x80000000u;
   if (am > 0x7f800000u)
      return kk_dp_nan(a);
   if (bm > 0x7f800000u)
      return kk_dp_nan(b);
   if (cm > 0x7f800000u)
      return kk_dp_nan(c);
   if (am == 0x7f800000u || bm == 0x7f800000u) {
      if (!am || !bm ||
          (cm == 0x7f800000u && ((as_type<uint>(c) ^ sign) & 0x80000000u)))
         return kk_dp_bits(0x7fc00000u);
      return kk_dp_bits(sign | 0x7f800000u);
   }
   if (cm == 0x7f800000u)
      return c;
   float r = precise::fma(a, b, c);
   if (!kk_dp_sub(a) && !kk_dp_sub(b) && !kk_dp_sub(c) &&
       kk_dp_abs(r) >= 0x800000u)
      return r;
   kk_dp_number x = kk_dp_decode(a), y = kk_dp_decode(b), z = kk_dp_decode(c);
   ulong product = x.mant && y.mant ? kk_dp_product(x.mant, y.mant) : 0ul;
   return kk_dp_combine(sign, product, x.exp + y.exp, z.sign, z.mant, z.exp);
}
inline float kk_dp_div(float a, float b)
{
   uint am = kk_dp_abs(a), bm = kk_dp_abs(b),
        sign = (as_type<uint>(a) ^ as_type<uint>(b)) & 0x80000000u;
   if (am > 0x7f800000u)
      return kk_dp_nan(a);
   if (bm > 0x7f800000u)
      return kk_dp_nan(b);
   if ((!am && !bm) || (am == 0x7f800000u && bm == 0x7f800000u))
      return kk_dp_bits(0x7fc00000u);
   if (am == 0x7f800000u || !bm)
      return kk_dp_bits(sign | 0x7f800000u);
   if (!am || bm == 0x7f800000u)
      return kk_dp_bits(sign);
   float r = a / b;
   if (am >= 0x800000u && bm >= 0x800000u && kk_dp_abs(r) >= 0x800000u)
      return r;
   kk_dp_number x = kk_dp_decode(a), y = kk_dp_decode(b);
   ulong numerator = ulong(x.mant) << 39;
   ulong q = numerator / y.mant, rem = numerator % y.mant;
   return kk_dp_pack(sign, q | ulong(rem != 0), x.exp - y.exp - 39);
}
inline float kk_dp_sqrt(float a)
{
   if (!kk_dp_sub(a))
      return precise::sqrt(a);
   if (as_type<uint>(a) & 0x80000000u)
      return kk_dp_bits(0x7fc00000u);
   kk_dp_number x = kk_dp_decode(a);
   if (x.exp & 1) {
      x.mant <<= 1;
      --x.exp;
   }
   ulong n = ulong(x.mant) << 38, rem = n, result = 0, bit = 1ul << 62;
   while (bit > rem)
      bit >>= 2;
   while (bit) {
      if (rem >= result + bit) {
         rem -= result + bit;
         result = (result >> 1) + bit;
      } else
         result >>= 1;
      bit >>= 2;
   }
   return kk_dp_pack(0, result | ulong(rem != 0), x.exp / 2 - 19);
}
inline float kk_dp_ldexp(float a, int n)
{
   uint mag = kk_dp_abs(a);
   if (!mag || mag >= 0x7f800000u)
      return a;
   float r = precise::ldexp(a, n);
   if (!kk_dp_sub(a) && kk_dp_abs(r) >= 0x800000u)
      return r;
   kk_dp_number x = kk_dp_decode(a);
   return kk_dp_pack(x.sign, x.mant, x.exp + clamp(n, -512, 512));
}
inline float kk_dp_lrp(float a, float b, float c)
{
   return kk_dp_add(a, kk_dp_mul(kk_dp_subtract(b, a), c));
}
// Integer ordering distinguishes subnormals from zero without a floating
// compare.
inline uint kk_dp_key(float a)
{
   uint x = as_type<uint>(a);
   if (!(x & 0x7fffffffu))
      x = 0;
   return x & 0x80000000u ? ~x : x ^ 0x80000000u;
}
inline bool kk_dp_eq(float a, float b)
{
   return kk_dp_abs(a) <= 0x7f800000u && kk_dp_abs(b) <= 0x7f800000u &&
          kk_dp_key(a) == kk_dp_key(b);
}
inline bool kk_dp_ne(float a, float b) { return !kk_dp_eq(a, b); }
inline bool kk_dp_lt(float a, float b)
{
   return kk_dp_abs(a) <= 0x7f800000u && kk_dp_abs(b) <= 0x7f800000u &&
          kk_dp_key(a) < kk_dp_key(b);
}
inline bool kk_dp_ge(float a, float b)
{
   return kk_dp_abs(a) <= 0x7f800000u && kk_dp_abs(b) <= 0x7f800000u &&
          kk_dp_key(a) >= kk_dp_key(b);
}
inline float kk_dp_min(float a, float b)
{
   if (kk_dp_abs(a) > 0x7f800000u)
      return b;
   if (kk_dp_abs(b) > 0x7f800000u)
      return a;
   if (kk_dp_eq(a, b))
      return kk_dp_bits(as_type<uint>(a) | as_type<uint>(b));
   return kk_dp_lt(a, b) ? a : b;
}
inline float kk_dp_max(float a, float b)
{
   if (kk_dp_abs(a) > 0x7f800000u)
      return b;
   if (kk_dp_abs(b) > 0x7f800000u)
      return a;
   if (kk_dp_eq(a, b))
      return kk_dp_bits(as_type<uint>(a) & as_type<uint>(b));
   return kk_dp_lt(a, b) ? b : a;
}
inline float kk_dp_sat(float a) { return kk_dp_min(kk_dp_max(a, 0.0f), 1.0f); }
inline float kk_dp_floor(float a)
{
   if (kk_dp_sub(a))
      return as_type<uint>(a) & 0x80000000u ? -1.0f : 0.0f;
   return precise::floor(a);
}
inline float kk_dp_ceil(float a)
{
   if (kk_dp_sub(a))
      return as_type<uint>(a) & 0x80000000u ? kk_dp_bits(0x80000000u) : 1.0f;
   return precise::ceil(a);
}
inline float kk_dp_sign(float a)
{
   if (!kk_dp_abs(a))
      return a;
   if (kk_dp_abs(a) > 0x7f800000u)
      return a;
   return kk_dp_bits((as_type<uint>(a) & 0x80000000u) | 0x3f800000u);
}
inline float kk_dp_rsqrt(float a)
{
   if (!kk_dp_sub(a))
      return precise::rsqrt(a);
   return kk_dp_div(1.0f, kk_dp_sqrt(a));
}
inline float kk_dp_sin(float a) { return kk_dp_sub(a) ? a : precise::sin(a); }
inline float kk_dp_log2(float a)
{
   if (!kk_dp_sub(a))
      return precise::log2(a);
   if (as_type<uint>(a) & 0x80000000u)
      return kk_dp_bits(0x7fc00000u);
   kk_dp_number x = kk_dp_decode(a);
   return precise::log2(kk_dp_bits(0x3f800000u | (x.mant & 0x7fffffu))) +
          float(x.exp + 23);
}
inline float kk_dp_exp2(float a)
{
   if (a >= -126.0f)
      return precise::exp2(a);
   if (a < -150.0f)
      return 0.0f;
   return kk_dp_ldexp(precise::exp2(a + 126.0f), -126);
}
inline float kk_dp_pow(float a, float b)
{
   if (kk_dp_sub(a))
      return kk_dp_exp2(kk_dp_mul(b, kk_dp_log2(a)));
   float r = precise::pow(a, b);
   if (kk_dp_abs(r) || !kk_dp_abs(a) || a < 0.0f)
      return r;
   return kk_dp_exp2(kk_dp_mul(b, kk_dp_log2(a)));
}
inline float kk_dp_fract(float a)
{
   if (kk_dp_sub(a))
      return as_type<uint>(a) & 0x80000000u ? 0x1.fffffep-1f : a;
   return fract(a);
}
inline float kk_dp_fmod(float a, float b)
{
   uint am = kk_dp_abs(a), bm = kk_dp_abs(b);
   if (am > 0x7f800000u)
      return kk_dp_nan(a);
   if (bm > 0x7f800000u)
      return kk_dp_nan(b);
   if (!bm || am == 0x7f800000u)
      return kk_dp_bits(0x7fc00000u);
   if (am < bm || !am)
      return a;
   float r = precise::fmod(a, b);
   if (!kk_dp_sub(a) && !kk_dp_sub(b) && kk_dp_abs(r) >= 0x800000u)
      return r;
   kk_dp_number x = kk_dp_decode(a), y = kk_dp_decode(b);
   int diff = x.exp - y.exp;
   uint rem = x.mant % y.mant;
   while (diff-- > 0)
      rem = (rem << 1) % y.mant;
   return kk_dp_pack(x.sign, rem, y.exp);
}

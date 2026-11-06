/* Copyright 2025 Valve Corporation
 * SPDX-License-Identifier: MIT
 * Bit conversions adapted from Mesa src/util/float8.c at
 * 4f071f891d63396c799b267172c19ba6b0301682.
 */
inline float kk_fp8_decode(uchar value, bool e5m2)
{
    uint mantissa_bits = e5m2 ? 2u : 3u;
    uint exponent_mask = e5m2 ? 31u : 15u;
    uint bias = e5m2 ? 15u : 7u;
    uint m = uint(value) & ((1u << mantissa_bits) - 1u);
    uint e = (uint(value) >> mantissa_bits) & exponent_mask;
    uint result = (uint(value) & 128u) << 24;
    if (e == exponent_mask && (e5m2 ? m != 0u : m == 7u))
        return as_type<float>(0xffc00000u);
    if (e5m2 && e == 31u)
        result |= 0x7f800000u;
    else if (e == 0u && m != 0u) {
        uint shift = mantissa_bits + 1u - (32u - clz(m));
        result |= (127u - (bias - 1u) - shift) << 23;
        result |= ((m << shift) & ((1u << mantissa_bits) - 1u)) << (23u - mantissa_bits);
    } else if (e != 0u) {
        result |= (e + 127u - bias) << 23;
        result |= m << (23u - mantissa_bits);
    }
    return as_type<float>(result);
}

inline uchar kk_fp8_encode(float value, bool e5m2, bool saturate, bool finite_nan)
{
    uint bits = as_type<uint>(value);
    uint magnitude = bits & 0x7fffffffu;
    uint sign = bits >> 24 & 128u;
    uint nan_value = e5m2 ? 0xfeu : 0xffu;
    uint maximum = e5m2 ? 0x7bu : 0x7eu;
    if (magnitude > 0x7f800000u || (finite_nan && magnitude == 0x7f800000u))
        return uchar(nan_value);
    if (saturate && magnitude > (e5m2 ? 0x47600000u : 0x43e00000u))
        return uchar(sign | maximum);
    if (e5m2 ? magnitude >= 0x47700000u : magnitude > 0x43e80000u)
        return uchar(e5m2 ? sign | 0x7cu : nan_value);
    int exponent = int(magnitude >> 23) - 127 + (e5m2 ? 15 : 7);
    if (exponent < (e5m2 ? -2 : -3))
        return uchar(sign);
    uint mantissa = magnitude & 0x7fffffu;
    bool round_up = false;
    if (exponent <= 0) {
        uint offset = uint(1 - exponent);
        round_up = (mantissa & ((1u << offset) - 1u)) != 0u;
        mantissa = (mantissa | 0x800000u) >> offset;
    }
    round_up |= (mantissa & (e5m2 ? 0x2fffffu : 0x17ffffu)) != 0u;
    if ((mantissa & (e5m2 ? 0x100000u : 0x080000u)) != 0u && round_up) {
        mantissa += e5m2 ? 0x200000u : 0x100000u;
        if ((mantissa & 0x800000u) != 0u) {
            mantissa = 0u;
            exponent++;
        }
    }
    uint result = sign;
    if (exponent > 0)
        result |= uint(exponent) << (e5m2 ? 2u : 3u);
    result |= mantissa >> (e5m2 ? 21u : 20u);
    return uchar(result);
}

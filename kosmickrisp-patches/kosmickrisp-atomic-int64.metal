/* SPDX-License-Identifier: MIT
 * Full-width operations are serialized by address. Each successful SIMD lane
 * releases its lock before retrying lanes reconverge at the loop condition.
 */
static inline ulong
kk_atomic64_value(ulong old, uint op, ulong value, ulong compare)
{
    switch (op) {
    case 0: return old + value;
    case 1: return as_type<ulong>(min(as_type<long>(old), as_type<long>(value)));
    case 2: return min(old, value);
    case 3: return as_type<ulong>(max(as_type<long>(old), as_type<long>(value)));
    case 4: return max(old, value);
    case 5: return old & value;
    case 6: return old | value;
    case 7: return old ^ value;
    case 9: return old;
    case 10: return old == compare ? value : old;
    default: return value;
    }
}
static inline device atomic_uint *
kk_atomic64_lock(ulong address, device atomic_uint *table)
{
    ulong key = address >> 3;
    uint hash = uint(key) ^ uint(key >> 32);
    return table + ((hash ^ (hash >> 16)) & 65535u);
}
static inline ulong
kk_atomic64(device ulong *address, device atomic_uint *table,
            uint op, ulong value, ulong compare, bool unused)
{
    device atomic_uint *lock = kk_atomic64_lock(ulong(address), table);
    device atomic_uint *words = reinterpret_cast<device atomic_uint *>(address);
    uint2 result = uint2(0);
    bool done = false;
    while (!done) {
        uint expected = 0;
        if (atomic_compare_exchange_weak_explicit(lock, &expected, 1u,
                memory_order_relaxed, memory_order_relaxed)) {
            atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst,
                                thread_scope_device);
            if (unused && (op == 2u || op == 4u)) {
                device atomic_ulong *p = reinterpret_cast<device atomic_ulong *>(address);
                if (op == 2u) atomic_min_explicit(p, value, memory_order_relaxed);
                else atomic_max_explicit(p, value, memory_order_relaxed);
            } else {
                result = uint2(atomic_load_explicit(words, memory_order_relaxed),
                               atomic_load_explicit(words + 1, memory_order_relaxed));
                if (op != 9u) {
                    ulong next = kk_atomic64_value(as_type<ulong>(result), op, value, compare);
                    atomic_store_explicit(words, uint(next), memory_order_relaxed);
                    atomic_store_explicit(words + 1, uint(next >> 32), memory_order_relaxed);
                }
            }
            atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst,
                                thread_scope_device);
            atomic_store_explicit(lock, 0u, memory_order_relaxed);
            done = true;
        }
    }
    return as_type<ulong>(result);
}
static inline ulong
kk_atomic64(threadgroup ulong *address, device atomic_uint *table,
            uint op, ulong value, ulong compare, bool unused)
{
    device atomic_uint *lock = kk_atomic64_lock(ulong(address), table);
    threadgroup atomic_uint *words = reinterpret_cast<threadgroup atomic_uint *>(address);
    uint2 result = uint2(0);
    bool done = false;
    while (!done) {
        uint expected = 0;
        if (atomic_compare_exchange_weak_explicit(lock, &expected, 1u,
                memory_order_relaxed, memory_order_relaxed)) {
            atomic_thread_fence(mem_flags::mem_threadgroup, memory_order_seq_cst,
                                thread_scope_threadgroup);
            result = uint2(atomic_load_explicit(words, memory_order_relaxed),
                               atomic_load_explicit(words + 1, memory_order_relaxed));
            if (op != 9u) {
                ulong next = kk_atomic64_value(as_type<ulong>(result), op, value, compare);
                atomic_store_explicit(words, uint(next), memory_order_relaxed);
                atomic_store_explicit(words + 1, uint(next >> 32), memory_order_relaxed);
            }
            atomic_thread_fence(mem_flags::mem_threadgroup, memory_order_seq_cst,
                                thread_scope_threadgroup);
            atomic_store_explicit(lock, 0u, memory_order_relaxed);
            done = true;
        }
    }
    return as_type<ulong>(result);
}

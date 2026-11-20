// andropkg: like bionic's <stdatomic.h>, map the C11 atomics API onto <atomic> in C++ (Apple's
// libc++ rejects mixing <stdatomic.h> and <atomic> before C++23; AOSP headers such as
// cutils/trace.h include <stdatomic.h> from C++).
#pragma once
#if defined(__cplusplus) && __cplusplus < 202302L
#include <atomic>
#define _Atomic(t) std::atomic<t>
using std::atomic_bool;
using std::atomic_char;
using std::atomic_int;
using std::atomic_uint;
using std::atomic_long;
using std::atomic_ulong;
using std::atomic_llong;
using std::atomic_ullong;
using std::atomic_int_least32_t;
using std::atomic_uint_least32_t;
using std::atomic_int_least64_t;
using std::atomic_uint_least64_t;
using std::atomic_intptr_t;
using std::atomic_uintptr_t;
using std::atomic_size_t;
using std::atomic_flag;
using std::memory_order;
using std::memory_order_relaxed;
using std::memory_order_consume;
using std::memory_order_acquire;
using std::memory_order_release;
using std::memory_order_acq_rel;
using std::memory_order_seq_cst;
using std::atomic_init;
using std::atomic_is_lock_free;
using std::atomic_load;
using std::atomic_load_explicit;
using std::atomic_store;
using std::atomic_store_explicit;
using std::atomic_exchange;
using std::atomic_exchange_explicit;
using std::atomic_compare_exchange_strong;
using std::atomic_compare_exchange_strong_explicit;
using std::atomic_compare_exchange_weak;
using std::atomic_compare_exchange_weak_explicit;
using std::atomic_fetch_add;
using std::atomic_fetch_add_explicit;
using std::atomic_fetch_sub;
using std::atomic_fetch_sub_explicit;
using std::atomic_fetch_or;
using std::atomic_fetch_or_explicit;
using std::atomic_fetch_and;
using std::atomic_fetch_and_explicit;
using std::atomic_thread_fence;
using std::atomic_signal_fence;
using std::atomic_flag_test_and_set;
using std::atomic_flag_test_and_set_explicit;
using std::atomic_flag_clear;
using std::atomic_flag_clear_explicit;
#else
#include_next <stdatomic.h>
#endif

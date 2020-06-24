/*****************************************************************************

Copyright (c) 2012, 2020, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is also distributed with certain software (including but not
limited to OpenSSL) that is licensed under separate terms, as designated in a
particular file or component or in included license documentation. The authors
of MySQL hereby grant you an additional permission to link the program and
your derivative works with the separately licensed software that they have
included with MySQL.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file include/ut0counter.h

 Counter utility class

 Created 2012/04/12 by Sunny Bains
 *******************************************************/

#ifndef ut0counter_h
#define ut0counter_h

#include <my_rdtsc.h>

#include "univ.i"

#include "os0thread.h"
#include "ut0dbg.h"

#include <array>
#include <atomic>
#include <functional>

/** CPU cache line size */
#ifdef __powerpc__
#define INNOBASE_CACHE_LINE_SIZE 128
#else
#define INNOBASE_CACHE_LINE_SIZE 64
#endif /* __powerpc__ */

/** Default number of slots to use in ib_counter_t */
#define IB_N_SLOTS 64

/** Get the offset into the counter array. */
template <typename Type, int N>
struct generic_indexer_t {
  /** Default constructor/destructor should be OK. */

  /** @return offset within m_counter */
  static size_t offset(size_t index) UNIV_NOTHROW {
    return (((index % N) + 1) * (INNOBASE_CACHE_LINE_SIZE / sizeof(Type)));
  }
};

/** Use the result of my_timer_cycles(), which mainly uses RDTSC for cycles,
to index into the counter array. See the comments for my_timer_cycles() */
template <typename Type = ulint, int N = 1>
struct counter_indexer_t : public generic_indexer_t<Type, N> {
  /** Default constructor/destructor should be OK. */

  enum { fast = 1 };

  /** @return result from RDTSC or similar functions. */
  static size_t get_rnd_index() UNIV_NOTHROW {
    size_t c = static_cast<size_t>(my_timer_cycles());

    if (c != 0) {
      return (c);
    } else {
      /* We may go here if my_timer_cycles() returns 0,
      so we have to have the plan B for the counter. */
#if !defined(_WIN32)
      return (size_t(os_thread_get_curr_id()));
#else
      LARGE_INTEGER cnt;
      QueryPerformanceCounter(&cnt);

      return (static_cast<size_t>(cnt.QuadPart));
#endif /* !_WIN32 */
    }
  }
};

/** For counters where N=1 */
template <typename Type = ulint, int N = 1>
struct single_indexer_t {
  /** Default constructor/destructor should are OK. */

  enum { fast = 0 };

  /** @return offset within m_counter */
  static size_t offset(size_t index) UNIV_NOTHROW {
    ut_ad(N == 1);
    return ((INNOBASE_CACHE_LINE_SIZE / sizeof(Type)));
  }

  /** @return 1 */
  static size_t get_rnd_index() UNIV_NOTHROW {
    ut_ad(N == 1);
    return (1);
  }
};

/** core affinity indexer */
template <typename Type = uint64_t, size_t N = IB_N_SLOTS>
struct core_affinity_indexer_t {
  /** Default constructor/destructor should be OK. */
  enum { fast = 1 };

  static size_t offset() UNIV_NOTHROW {
#ifdef HAVE_SCHED_GETCPU
    return (sched_getcpu());
#else
    size_t c = static_cast<size_t>(my_timer_cycles());

    if (c != 0) {
      return (c);
    } else {
#if !defined(_WIN32)
      return (size_t(os_thread_get_curr_id()));
#else
      LARGE_INTEGER cnt;
      QueryPerformanceCounter(&cnt);

      return (static_cast<size_t>(cnt.QuadPart));
#endif /* !_WIN32 */
    }
#endif /* HAVE_SCHED_GETCPU */
  }
};

#define default_indexer_t counter_indexer_t

/** Class for using fuzzy counters. The counter is not protected by any
mutex and the results are not guaranteed to be 100% accurate but close
enough. Creates an array of counters and separates each element by the
INNOBASE_CACHE_LINE_SIZE bytes */
template <typename Type, int N = IB_N_SLOTS,
          template <typename, int> class Indexer = default_indexer_t>
class ib_counter_t {
 public:
  ib_counter_t() { memset(m_counter, 0x0, sizeof(m_counter)); }

  ~ib_counter_t() { ut_ad(validate()); }

  static bool is_fast() { return (Indexer<Type, N>::fast); }

  bool validate() UNIV_NOTHROW {
#ifdef UNIV_DEBUG
    size_t n = (INNOBASE_CACHE_LINE_SIZE / sizeof(Type));

    /* Check that we aren't writing outside our defined bounds. */
    for (size_t i = 0; i < UT_ARR_SIZE(m_counter); i += n) {
      for (size_t j = 1; j < n - 1; ++j) {
        ut_ad(m_counter[i + j] == 0);
      }
    }
#endif /* UNIV_DEBUG */
    return (true);
  }

  /** If you can't use a good index id. Increment by 1. */
  void inc() UNIV_NOTHROW { add(1); }

  /** If you can't use a good index id.
  @param n is the amount to increment */
  void add(Type n) UNIV_NOTHROW {
    size_t i = m_policy.offset(m_policy.get_rnd_index());

    ut_ad(i < UT_ARR_SIZE(m_counter));

    m_counter[i] += n;
  }

  /** Use this if you can use a unique identifier, saves a
  call to get_rnd_index().
  @param	index	index into a slot
  @param	n	amount to increment */
  void add(size_t index, Type n) UNIV_NOTHROW {
    size_t i = m_policy.offset(index);

    ut_ad(i < UT_ARR_SIZE(m_counter));

    m_counter[i] += n;
  }

  /** If you can't use a good index id. Decrement by 1. */
  void dec() UNIV_NOTHROW { sub(1); }

  /** If you can't use a good index id.
  @param n the amount to decrement */
  void sub(Type n) UNIV_NOTHROW {
    size_t i = m_policy.offset(m_policy.get_rnd_index());

    ut_ad(i < UT_ARR_SIZE(m_counter));

    m_counter[i] -= n;
  }

  /** Use this if you can use a unique identifier, saves a
  call to get_rnd_index().
  @param	index	index into a slot
  @param	n	amount to decrement */
  void sub(size_t index, Type n) UNIV_NOTHROW {
    size_t i = m_policy.offset(index);

    ut_ad(i < UT_ARR_SIZE(m_counter));

    m_counter[i] -= n;
  }

  /* @return total value - not 100% accurate, since it is not atomic. */
  operator Type() const UNIV_NOTHROW {
    Type total = 0;

    for (size_t i = 0; i < N; ++i) {
      total += m_counter[m_policy.offset(i)];
    }

    return (total);
  }

  Type operator[](size_t index) const UNIV_NOTHROW {
    size_t i = m_policy.offset(index);

    ut_ad(i < UT_ARR_SIZE(m_counter));

    return (m_counter[i]);
  }

 private:
  /** Indexer into the array */
  Indexer<Type, N> m_policy;

  /** Slot 0 is unused. */
  Type m_counter[(N + 1) * (INNOBASE_CACHE_LINE_SIZE / sizeof(Type))];
};

/** Sharded atomic counter. */
namespace Counter {

using Type = uint64_t;

using N = std::atomic<Type>;

static_assert(INNOBASE_CACHE_LINE_SIZE >= sizeof(N),
              "Atomic counter size > INNOBASE_CACHE_LINE_SIZE");

using Pad = byte[INNOBASE_CACHE_LINE_SIZE - sizeof(N)];

/** Counter shard. */
struct Shard {
  /** Separate on cache line. */
  Pad m_pad;

  /** Sharded counter. */
  N m_n{};
};

template <size_t COUNT = 128>
using Shards = std::array<Shard, COUNT>;

using Function = std::function<void(const Type)>;

constexpr auto Memory_order = std::memory_order_relaxed;

/** Increment the counter for a shard by n.
@param[in,out]  shards          Sharded counter to increment.
@param[in] id                   Shard key.
@param[in] n                    Number to add.
@return previous value. */
template <size_t COUNT>
inline Type add(Shards<COUNT> &shards, size_t id, size_t n) {
  return (shards[id % shards.size()].m_n.fetch_add(n, Memory_order));
}

/** Decrement the counter for a shard by n.
@param[in,out]  shards          Sharded counter to increment.
@param[in] id                   Shard key.
@param[in] n                    Number to add.
@return previous value. */
template <size_t COUNT>
inline Type sub(Shards<COUNT> &shards, size_t id, size_t n) {
  return (shards[id % shards.size()].m_n.fetch_sub(n, Memory_order));
}

/** Increment the counter of a shard by 1.
@param[in,out]  shards          Sharded counter to increment.
@param[in] id                   Shard key.
@return previous value. */
template <size_t COUNT>
inline Type inc(Shards<COUNT> &shards, size_t id) {
  return (add(shards, id, 1));
}

/** Decrement the counter of a shard by 1.
@param[in,out]  shards          Sharded counter to decrement.
@param[in] id                   Shard key.
@return previous value. */
template <size_t COUNT>
inline Type dec(Shards<COUNT> &shards, size_t id) {
  return (sub(shards, id, 1));
}

/** Get the counter value for a shard.
@param[in,out]  shards          Sharded counter to increment.
@param[in] id                   Shard key.
@return current value. */
template <size_t COUNT>
inline Type get(const Shards<COUNT> &shards, size_t id) noexcept {
  return (shards[id % shards.size()].m_n.load(Memory_order));
}

/** Iterate over the shards.
@param[in] shards               Shards to iterate over
@param[in] f                    Callback function
*/
template <size_t COUNT>
inline void for_each(const Shards<COUNT> &shards, Function &&f) noexcept {
  for (const auto &shard : shards) {
    f(shard.m_n);
  }
}

/** Get the total value of all shards.
@param[in] shards               Shards to sum.
@return total value. */
template <size_t COUNT>
inline Type total(const Shards<COUNT> &shards) noexcept {
  Type n = 0;

  for_each(shards, [&](const Type count) { n += count; });

  return (n);
}

/** Clear the counter - reset to 0.
@param[in,out] shards          Shards to clear. */
template <size_t COUNT>
inline void clear(Shards<COUNT> &shards) noexcept {
  for (auto &shard : shards) {
    shard.m_n.store(0, Memory_order);
  }
}

/** Copy the counters, overwrite destination.
@param[in,out] dst              Destination shard
@param[in]     src              Source shard. */
template <size_t COUNT>
inline void copy(Shards<COUNT> &dst, const Shards<COUNT> &src) noexcept {
  size_t i{0};
  for_each(src, [&](const Type count) {
    dst[i++].m_n.store(count, std::memory_order_relaxed);
  });
}

/** Accumulate the counters, add source to destination.
@param[in,out] dst              Destination shard
@param[in]     src              Source shard. */
template <size_t COUNT>
inline void add(Shards<COUNT> &dst, const Shards<COUNT> &src) noexcept {
  size_t i{0};
  for_each(src, [&](const Type count) {
    dst[i++].m_n.fetch_add(count, std::memory_order_relaxed);
  });
}
}  // namespace Counter

/* Atomic Counter with N slots */

template <typename Type = uint64_t>
struct atomic_slot_padded_t {
  char m_pad[INNOBASE_CACHE_LINE_SIZE - sizeof(std::atomic<Type>)];
  std::atomic<Type> m_value{};
};

template <typename Type = uint64_t>
struct atomic_slot_aligned_t {
  alignas(INNOBASE_CACHE_LINE_SIZE) std::atomic<Type> m_value{};
};

template <typename Type = uint64_t>
struct atomic_slot_t {
  std::atomic<Type> m_value{};
};

template <typename Type, int N = IB_N_SLOTS,
          template <typename> class Slot = atomic_slot_aligned_t,
          template <typename, size_t> class Indexer = core_affinity_indexer_t>
class ib_atomic_counter_t {
 public:
  ib_atomic_counter_t() : m_slots() {}

  void inc() UNIV_NOTHROW { inc(1); }
  void dec() UNIV_NOTHROW { dec(1); }
  void add(size_t id, Type incr) UNIV_NOTHROW { inc(incr); }
  void sub(size_t id, Type decr) UNIV_NOTHROW { dec(decr); }

  void inc(Type incr) UNIV_NOTHROW {
    size_t id = m_policy.offset();

    m_slots[id % m_slots.size()].m_value.fetch_add(incr,
                                                   std::memory_order_relaxed);
  }

  void dec(Type decr) UNIV_NOTHROW {
    size_t id = m_policy.offset();
    m_slots[id % m_slots.size()].m_value.fetch_sub(decr,
                                                   std::memory_order_relaxed);
  }

  Type total() const UNIV_NOTHROW { return get(); }

  Type get() const UNIV_NOTHROW {
    Type total = 0;
    for (size_t i = 0; i < m_slots.size(); ++i) {
      total += m_slots[i].m_value.load(std::memory_order_relaxed);
    }
    return total;
  }

  void set(uint64_t val) UNIV_NOTHROW {
    reset();
    size_t id = m_policy.offset() % m_slots.size();
    m_slots[id].m_value.store(val, std::memory_order_relaxed);
  }

  void clear() UNIV_NOTHROW { reset(); }

  void reset() UNIV_NOTHROW {
    for (size_t i = 0; i < m_slots.size(); ++i) {
      m_slots[i].m_value.store(0);
    }
  }

  void copy(const ib_atomic_counter_t &src) {
    for (size_t i = 0; i < m_slots.size(); ++i) {
      m_slots[i].m_value.store(
          src.m_slots[i].m_value.load(std::memory_order_relaxed),
          std::memory_order_relaxed);
    }
  }

  void add(const ib_atomic_counter_t &src) {
    for (size_t i = 0; i < m_slots.size(); ++i) {
      m_slots[i].m_value.fetch_add(
          src.m_slots[i].m_value.load(std::memory_order_relaxed),
          std::memory_order_relaxed);
    }
  }

  operator Type() const UNIV_NOTHROW { return get(); }

 private:
  /** Indexer into the array */
  Indexer<Type, N> m_policy;

  std::array<Slot<Type>, N> m_slots;
};

#endif /* ut0counter_h */

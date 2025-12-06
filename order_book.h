#pragma once
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include "itch.h"
#include "align.h"
#include <type_traits>
#include <cassert>

/* This is an optimized order book implementation.
 * Conceptually an order book is two sets of levels, with each
 * level representing all the queued orders at a price.
 *
 * This implementation provides O(1) lookup of the best bid and offer
 * as well as the aggregated quantity for any price. It provides
 * very fast throughput (61.5ns / tick at last benchmark), although
 * note that there are some poor performance worst-case ticks.
 *
 * The ITCH feed sends some metadata about each order with each add_order
 * message, such as the price, quantity and symbol (really a number
 * referring to the symbol). The user is expected after that to keep
 * track of the metadata; for instance the delete message only has
 * the order id and the user is expected to know which symbol
 * that refers to as well as the price and size.
 *
 * This implementation uses several tricks. The first is that, while
 * a hashmap seems like a reasonable data structure to keep track of
 * the metadata for each order, in practice the ITCH feed generates
 * the order ids close together, not going past a max id of several
 * hundred million. We thus store the metadata for each order in an
 * array, so looking up the order metadata is a single dereference.
 * The order id generation has a side benefit that new orders are
 * likely to be near recently added orders, so the pages are likely
 * to be in the TLB and memory cache.
 *
 * Each order knows which book and price level it belongs to. So
 * to find all the data about an order requires two to three
 * dereferences. Also the quantities in each price level are held
 * in a pointed to data structure so that a reduce operation does
 * not need to search the levels but can modify the quantity at the
 * level directly from the pointer stored in the order metadata.
 *
 * Another trick is that each price level is represented as a price
 * and quantity, and the levels are stored in a sorted array instead
 * of a tree. Averagely speaking most activity is near the top of
 * the book, so the implementation only needs to go 1-5 levels deep
 * into the book. Thus using an array is fast since it improves
 * locality and uses less memory. Note though that the worst case
 * performance could be bad - if somebody adds and deletes orders
 * far away from the inside of the book it could result in longer
 * processing for those messages.
 *
 * Lastly, since the orders and levels are stored in their own global
 * pools, they are likely to be local and there is very little pressure
 * on the allocator. In fact the only allocations are bulk allocations
 * from stl container resizing.
 */

#define CROSS_CHECK 0

using sprice_t = int32_t;
bool constexpr is_bid(sprice_t const x) { return int32_t(x) >= 0; }
// Helper to extract an integral underlying type for ptr_t while avoiding
// hard errors when ptr_t is not an enum. If ptr_t is an enum, use
// std::underlying_type<ptr_t>::type. Otherwise use ptr_t directly.
template<typename P, bool IsEnum = std::is_enum<P>::value>
struct ptr_underlying { using type = P; };
template<typename P>
struct ptr_underlying<P, true> { using type = typename std::underlying_type<P>::type; };

/* A custom, pooling allocator. It uses a non-shrinking vector as its pool,
 * and a vector (LIFO stack) as its free list.
 * If there are no free locations, increment m_size, representing growing
 * the pool. If there is a free location, pop its address off the free list
 * and return that. To free an object, push its address onto the free list.
 * Note that pointers are not guaranteed to be stable across invocations
 * of `allocate`. To have a stable way of referencing an object use
 * `get` on its address.
 *
 * Performance: Since the pool and the free list are both represented
 * as arrays, an allocate is an increment if the free list is empty,
 * or a decrement and dereference (to the tail of the pool which is
 * likely in cache), if the free list is not empty.
 * A deallocate is a decrement and a write to memory.
 *
 * The implementation uses custom pointer types to save space, and
 * to preserve addresses if the underlying container is resized.
 * For instance we define `enum class order_id_t : uint32_t {}`
 * instead of (order *)
 */
template <class T, typename ptr_t, size_t SIZE_HINT>
class pool
{
 public:
  using __ptr = ptr_t;
  using size_t__ = typename ptr_underlying<ptr_t>::type;
  std::vector<T> m_allocated;
  std::vector<ptr_t> m_free;
  pool() { m_allocated.reserve(SIZE_HINT); }
  pool(size_t reserve_size) { m_allocated.reserve(reserve_size); }
  T *get(ptr_t idx) { return &m_allocated[size_t__(idx)]; }
  T &operator[](ptr_t idx) { return m_allocated[size_t__(idx)]; }
#define ALLOC_INVARIANT \
  (m_free_size >= 0) /* aka can't free more than has been allocated */
  __ptr alloc(void)
  {
    if (m_free.empty()) {
      auto ret = __ptr(m_allocated.size());
      m_allocated.push_back(T());
      return ret;
    } else {
      auto ret = __ptr(m_free.back());
      m_free.pop_back();
      return ret;
    }
  }
  void free(__ptr idx) { m_free.push_back(idx); }
#undef ALLOC_INVARIANT
};
class level
{
 public:
  sprice_t m_price;
  qty_t m_qty;
  level(sprice_t __price, qty_t __qty) : m_price(__price), m_qty(__qty) {}
  level() {}
};

using book_id_t = uint16_t;
using level_id_t = uint32_t;
using order_id_t = uint32_t;

/* A datatype representing an order. Since this order book only wants
 * to know the size at each level it just remembers its quantity. If
 * one wanted to maintain knowledge about the queue it would probably
 * be fast to maintain a doubly linked list.
 * It also remember which book it belongs to and its price level. This
 * is so that given just an oid we can look up the book structure
 * as well as just the price level if we want to modify the quantity
 * at the level without searching for it in the book.
 */
typedef struct order_level {
  book_id_t book_idx;
  level_id_t level_idx;
  qty_t m_qty;
#if CROSS_CHECK
  order_id_t oid;
#endif
  void initialize(order_id_t __oid, book_id_t __book_idx, sprice_t __price, qty_t __qty) {
#if CROSS_CHECK
    oid = __oid;
#endif
    book_idx = __book_idx;
    m_qty = __qty;
  }
} order_level_t;

typedef struct order_price {
  book_id_t book_idx;
  qty_t m_qty;
  sprice_t m_price;
#if CROSS_CHECK
  order_id_t oid;
#endif
  void initialize(order_id_t __oid, book_id_t __book_idx, sprice_t __price, qty_t __qty) {
#if CROSS_CHECK
    oid = __oid;
#endif
    book_idx = __book_idx;
    m_price = __price;
    m_qty = __qty;
  }
} order_price_t;

class price_level_indirect
{
 public:
  price_level_indirect() {}
  price_level_indirect(sprice_t __price, level_id_t __ptr)
      : m_price(__price), m_ptr(__ptr)
  {
  }
  sprice_t m_price;
  level_id_t m_ptr;
};

template <class T>
class oidmap
{
 public:
  std::vector<T> m_data;
  void reserve(order_id_t const oid)
  {
    size_t const idx = size_t(oid);
    if (idx >= m_data.size()) {
      m_data.resize(idx + 1);
    }
  }
  T &operator[](order_id_t const oid)
  {
    size_t const idx = size_t(oid);
    return m_data[idx];
  }
  T *get(order_id_t const oid)
  {
    size_t const idx = size_t(oid);
    return &m_data[idx];
  }
};

struct order_id_hash {
  size_t operator()(order_id_t const id) const { return size_t(id); }
};

enum class LAYOUT { ARRAY_OF_STRUCTS, STRUCT_OF_ARRAYS };
enum class TRACE { DISABLED, ENABLED };

template<typename Derived, typename order_t, LAYOUT layout, TARGET_ISA isa, TRACE trace = TRACE::DISABLED>
class order_book
{
 public:
  static constexpr size_t MAX_BOOKS = 1 << 14;
  static constexpr size_t NUM_LEVELS = 1 << 20;
  static inline Derived s_books[MAX_BOOKS];  // can we allocate this on the stack?
  static inline oidmap<order_t> oid_map;

  static void add_order(order_id_t const oid, book_id_t const book_idx,
                        sprice_t const price, qty_t const qty)
  {
    if constexpr ( trace == TRACE::ENABLED ) {
      printf("ADD %u, %u, %d, %u\n", oid, book_idx, price, qty);
    }
    oid_map.reserve(oid);
    order_t *order = oid_map.get(oid);
    order->initialize( oid, book_idx, price, qty );
    static_cast<Derived *>(&s_books[size_t(order->book_idx)])->ADD_ORDER(order, price, qty);
  }
  static void delete_order(order_id_t const oid)
  {
    if constexpr ( trace == TRACE::ENABLED ) {
      printf("DELETE %u\n", oid);
    }
    order_t *order = oid_map.get(oid);
    static_cast<Derived *>(&s_books[size_t(order->book_idx)])->DELETE_ORDER(order);
  }
  static void cancel_order(order_id_t const oid, qty_t const qty)
  {
    if constexpr ( trace == TRACE::ENABLED ) {
      printf("REDUCE %u, %u\n", oid, qty);
    }
    order_t *order = oid_map.get(oid);
    static_cast<Derived *>(&s_books[size_t(order->book_idx)])->REDUCE_ORDER(order, qty);
  }
  static void execute_order(order_id_t const oid, qty_t const qty)
  {
    if constexpr ( trace == TRACE::ENABLED ) {
      printf("EXECUTE %lu %u\n", uint64_t(oid), qty);
    }
    order_t *order = oid_map.get(oid);
    auto book = static_cast<Derived *>(&s_books[size_t(order->book_idx)]);

    if (qty == order->m_qty) {
      book->DELETE_ORDER(order);
    } else {
      book->REDUCE_ORDER(order, qty);
    }
  }
  static void replace_order(order_id_t const old_oid, order_id_t const new_oid,
                              qty_t const new_qty, sprice_t new_price)
  {
    if constexpr ( trace == TRACE::ENABLED ) {
      printf("REPLACE %lu %lu %d %u\n", uint64_t(old_oid), uint64_t(new_oid), int32_t(new_price), uint32_t(new_qty));
    }
    order_t *order = oid_map.get(old_oid);
    auto book = static_cast<Derived *>(&s_books[size_t(order->book_idx)]);
    bool const bid = book->check_order_bid( order );
    book->DELETE_ORDER(order);
    book->add_order(new_oid, order->book_idx, (bid) ? new_price : -new_price, new_qty);
  }
};

#include "order_book_scalar.h"
#include "order_book_soa.h"
#include "order_book_soa_price.h"
#include "order_book_soa_avx2.h"

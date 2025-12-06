#include <algorithm>

template<TRACE trace = TRACE::DISABLED>
class order_book_soa_price : public order_book<order_book_soa_price<trace>, order_price_t, LAYOUT::ARRAY_OF_STRUCTS, TARGET_ISA::GENERIC_C, trace>
{
public:
  using sorted_prices_t = std::vector<sprice_t>;
  using sorted_qtys_t = std::vector<qty_t>;
  sorted_prices_t m_bid_prices;
  sorted_prices_t m_ask_prices;
  sorted_qtys_t m_bid_qtys;
  sorted_qtys_t m_ask_qtys;
  bool check_order_bid( const order_price_t *order ) const {
    return is_bid( order->m_price );
  }

#if CROSS_CHECK
  void crosscheck( size_t book_idx, bool is_bid ) {
    const auto& book = order_book_scalar<TRACE::DISABLED>::s_books[book_idx];
    auto ref_side = is_bid ? book.m_bids : book.m_asks;
    auto our_prices = is_bid ? m_bid_prices : m_ask_prices;
    auto our_qtys = is_bid ? m_bid_qtys : m_ask_qtys;

    assert( ref_side.size() == our_prices.size() );
    assert( ref_side.size() == our_qtys.size() );
    for ( size_t i = 0; i < our_prices.size(); i++ ) {
      assert( ref_side[i].m_price == our_prices[i] );
      assert( order_book_scalar<TRACE::DISABLED>::s_levels[ref_side[i].m_ptr].m_qty == our_qtys[i] );
    }
  }
#endif
  void ADD_ORDER(order_price_t *order, sprice_t const price, qty_t const qty)
  {
    sorted_prices_t& sorted_prices = is_bid(price) ? m_bid_prices : m_ask_prices;
    sorted_qtys_t& sorted_qtys = is_bid(price) ? m_bid_qtys : m_ask_qtys;
    // search descending for the price

    auto insertion_point = sorted_prices.end();
    bool found = false;
    while (insertion_point-- != sorted_prices.begin()) {
      auto curprice = *insertion_point;
      if ( curprice == price) {
        auto idx = insertion_point-sorted_prices.begin();
        sorted_qtys[idx] += qty;
        found = true;
        break;
      } else if ( price > curprice ) {
        // insertion pt will be -1 if price < all prices
        break;
      }
    }
    if (!found) {
      assert( order->m_price == price );
      assert( order->m_qty == qty );
      ++insertion_point;
      auto idx = insertion_point - sorted_prices.begin();
      sorted_prices.insert(insertion_point, price);
      sorted_qtys.insert(sorted_qtys.begin()+idx, qty );
    }
#if CROSS_CHECK
    order_book_scalar<TRACE::DISABLED>::add_order( order->oid, order->book_idx, price, qty );
    crosscheck( order->book_idx, is_bid(price) );
#endif
  }
  // shared between cancel(aka partial cancel aka reduce) and execute
  void REDUCE_ORDER(order_price_t *order, qty_t const qty)
  {
    sorted_prices_t& sorted_prices = is_bid(order->m_price) ? m_bid_prices : m_ask_prices;
    sorted_qtys_t& sorted_qtys = is_bid(order->m_price) ? m_bid_qtys : m_ask_qtys;
    auto it = std::find( sorted_prices.begin(), sorted_prices.end(), order->m_price );
    assert( it != sorted_prices.end() );
    auto idx = it - sorted_prices.begin();
    sorted_qtys[idx] -= qty;
#if CROSS_CHECK
    order_book_scalar<TRACE::DISABLED>::cancel_order( order->oid, qty );
    crosscheck( order->book_idx, is_bid( order->m_price ) );
#endif
    // this got done by cancel_order in the CROSS_CHECK case
    order->m_qty -= qty;
  }
  // shared between delete and execute
  void DELETE_ORDER(order_price_t *order)
  {
    sorted_prices_t& sorted_prices = is_bid(order->m_price) ? m_bid_prices : m_ask_prices;
    sorted_qtys_t& sorted_qtys = is_bid(order->m_price) ? m_bid_qtys : m_ask_qtys;
    auto it = std::find( sorted_prices.begin(), sorted_prices.end(), order->m_price );
    assert( sorted_prices.end() != it );
    auto idx = it - sorted_prices.begin();
    sorted_qtys[idx] -= order->m_qty;
    if (qty_t(0) == sorted_qtys[idx] ) {
      sorted_prices.erase( it );
      sorted_qtys.erase( sorted_qtys.begin() + idx );
    }
#if CROSS_CHECK
    order_book_scalar<TRACE::DISABLED>::delete_order( order->oid );
    crosscheck( order->book_idx, is_bid( order->m_price ) );
#endif
  }
};


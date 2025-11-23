class order_book_soa : public order_book<order_book_soa, LAYOUT::ARRAY_OF_STRUCTS, TARGET_ISA::GENERIC_C>
{
public:
  using sorted_prices_t = std::vector<sprice_t>;
  using sorted_levels_t = std::vector<level_id_t>;
  sorted_prices_t m_bid_prices;
  sorted_prices_t m_ask_prices;
  sorted_levels_t m_bid_levels;
  sorted_levels_t m_ask_levels;
#if CROSS_CHECK
  order_book_scalar reference_;

  void crosscheck( bool is_bid ) {
    auto ref_side = is_bid ? reference_.m_bids : reference_.m_asks;
    auto our_prices = is_bid ? m_bid_prices : m_ask_prices;
    auto our_levels = is_bid ? m_bid_levels : m_ask_levels;
    assert( ref_side.size() == our_prices.size() );
    assert( ref_side.size() == our_levels.size() );
    for ( size_t i = 0; i < our_prices.size(); i++ ) {
      assert( ref_side[i].m_price == our_prices[i] );
      assert( ref_side[i].m_ptr == our_levels[i] );
    }
  }
#endif
  void ADD_ORDER(order_t *order, sprice_t const price, qty_t const qty)
  {
    sorted_prices_t& sorted_prices = is_bid(price) ? m_bid_prices : m_ask_prices;
    sorted_levels_t& sorted_levels = is_bid(price) ? m_bid_levels : m_ask_levels;
    // search descending for the price
    auto insertion_point = sorted_prices.end();
    bool found = false;
    while (insertion_point-- != sorted_prices.begin()) {
      auto curprice = *insertion_point;
      if ( curprice == price) {
        auto idx = insertion_point-sorted_prices.begin();
        order->level_idx = sorted_levels[idx];
        found = true;
        break;
      } else if ( price > curprice ) {
        // insertion pt will be -1 if price < all prices
        break;
      }
    }
    if (!found) {
      order->level_idx = s_levels.alloc();
      s_levels[order->level_idx].m_qty = qty_t(0);
      s_levels[order->level_idx].m_price = price;
      ++insertion_point;
      auto idx = insertion_point - sorted_prices.begin();
      sorted_prices.insert(insertion_point, price);
      sorted_levels.insert(sorted_levels.begin()+idx, order->level_idx );
    }
    s_levels[order->level_idx].m_qty += qty;
#if CROSS_CHECK
    reference_.ADD_ORDER( order, price, qty );
    crosscheck( is_bid(price) );
#endif
  }
  // shared between cancel(aka partial cancel aka reduce) and execute
  void REDUCE_ORDER(order_t *order, qty_t const qty)
  {
    // subtract the reduced quantity from both the level and the order
    s_levels[order->level_idx].m_qty -= qty;
#if CROSS_CHECK
    reference_.REDUCE_ORDER( order, qty );
    crosscheck( is_bid( s_levels[order->level_idx].m_price ) );
#else
    // this got done by reference_.REDUCE_ORDER in the CROSS_CHECK case
    order->m_qty -= qty;
#endif
  }
  // shared between delete and execute
  void DELETE_ORDER(order_t *order)
  {
    assert(s_levels[order->level_idx].m_qty >= order->m_qty);
#if CROSS_CHECK
    bool bid = is_bid( s_levels[order->level_idx].m_price );
    reference_.DELETE_ORDER( order );
#endif
    s_levels[order->level_idx].m_qty -= order->m_qty;
    if (qty_t(0) == s_levels[order->level_idx].m_qty) {
      sprice_t price = s_levels[order->level_idx].m_price;
      sorted_prices_t& sorted_prices = is_bid(price) ? m_bid_prices : m_ask_prices;
      sorted_levels_t& sorted_levels = is_bid(price) ? m_bid_levels : m_ask_levels;
      auto it = sorted_prices.end();
      while (it-- != sorted_prices.begin()) {
        if ( *it == price) {
          auto idx = it - sorted_prices.begin();
          sorted_prices.erase( it );
          sorted_levels.erase( sorted_levels.begin() + idx );
          break;
        }
      }
      s_levels.free(order->level_idx);
    }
#if CROSS_CHECK
    crosscheck( bid );
#endif
  }
};


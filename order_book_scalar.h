template<TRACE trace = TRACE::DISABLED>
class order_book_scalar : public order_book<order_book_scalar<trace>, order_level_t, trace>
{
public:
  using base = order_book<order_book_scalar<trace>, order_level_t, trace>;
  using sorted_levels_t = std::vector<price_level_indirect>;
  sorted_levels_t m_bids;
  sorted_levels_t m_asks;
  using level_vector = pool<level, level_id_t, base::NUM_LEVELS>;
  static inline level_vector s_levels;
  bool check_order_bid ( const order_level_t *order ) const {
    return s_levels[ order->level_idx ].m_price > 0;
  }
  void ADD_ORDER(order_level_t *order, sprice_t const price, qty_t const qty)
  {
    sorted_levels_t *sorted_levels = is_bid(price) ? &m_bids : &m_asks;
    // search descending for the price
    auto insertion_point = sorted_levels->end();
    bool found = false;
    while (insertion_point-- != sorted_levels->begin()) {
      price_level_indirect &curprice = *insertion_point;
      if (curprice.m_price == price) {
        order->level_idx = curprice.m_ptr;
        found = true;
        break;
      } else if (price > curprice.m_price) {
        // insertion pt will be -1 if price < all prices
        break;
      }
    }
    if (!found) {
      order->level_idx = s_levels.alloc();
      s_levels[order->level_idx].m_qty = qty_t(0);
      s_levels[order->level_idx].m_price = price;
      price_level_indirect const px(price, order->level_idx);
      ++insertion_point;
      sorted_levels->insert(insertion_point, px);
    }
    s_levels[order->level_idx].m_qty += qty;
  }
  // shared between cancel(aka partial cancel aka reduce) and execute
  void REDUCE_ORDER(order_level_t *order, qty_t const qty)
  {
    // subtract the reduced quantity from both the level and the order
    s_levels[order->level_idx].m_qty -= qty;
    order->m_qty -= qty;
  }
  // shared between delete and execute
  void DELETE_ORDER(order_level_t *order)
  {
    assert(s_levels[order->level_idx].m_qty >= order->m_qty);
    s_levels[order->level_idx].m_qty -= order->m_qty;
    if (qty_t(0) == s_levels[order->level_idx].m_qty) {
      sprice_t price = s_levels[order->level_idx].m_price;
      sorted_levels_t *sorted_levels = is_bid(price) ? &m_bids : &m_asks;
      auto it = sorted_levels->end();
      while (it-- != sorted_levels->begin()) {
        if (it->m_price == price) {
          sorted_levels->erase(it);
          break;
        }
      }
      s_levels.free(order->level_idx);
    }
  }
};


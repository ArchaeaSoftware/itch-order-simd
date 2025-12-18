#include <algorithm>

#include <x86intrin.h>

/*
 * AVX2 helper functions
 */

//
// shift full 256b register left by 4 bytes
//
template<int N>
inline __m256i
_mm256_sll_4b_si256( __m256i m )
{
    return _mm256_alignr_epi8(m, _mm256_permute2x128_si256(m, m, _MM_SHUFFLE(0,
 0, 3, 0)), 16-N );
}

//
// shift full 256b register right by 4 bytes
//
template<int N>
inline __m256i
_mm256_srl_4b_si256( __m256i m )
{
    return _mm256_alignr_epi8( _mm256_permute2x128_si256(m, m, _MM_SHUFFLE(2, 0
, 0, 1)), m, N );
}

template<typename T, typename vector>
__attribute__((__always_inline__))
bool
Search_avx2( int *p, const vector& v, const T& q, __m256i& v_values, __m256i& v_cmpeq, __m256i& v_cmpgt, int start8 )
{
  int cmpgt;

  __m256i *p_v = (__m256i *) v.data();
  __m256i v_q = _mm256_set1_epi32( int(q) );
  if ( start8 < v.getN8() ) {
    v_values = _mm256_load_si256( p_v+start8 );
    v_cmpeq = _mm256_cmpeq_epi32( v_values, v_q );
    v_cmpgt = _mm256_cmpgt_epi32( v_values, v_q );
    int cmpeq = _mm256_movemask_ps( _mm256_castsi256_ps( v_cmpeq ) );
    if ( cmpeq ) {
        *p = start8;
        return true;
    }
  }
  do {
    v_values = _mm256_load_si256( p_v );
    v_cmpeq = _mm256_cmpeq_epi32( v_values, v_q );
    v_cmpgt = _mm256_cmpgt_epi32( v_values, v_q );
    int cmpeq = _mm256_movemask_ps( _mm256_castsi256_ps( v_cmpeq ) );
    if ( cmpeq ) {
        *p = (p_v-((__m256i *) v.data()));
        return true;
    }
    cmpgt = _mm256_movemask_ps( _mm256_castsi256_ps( v_cmpgt ) );
    p_v += 1;
  } while ( 0==cmpgt );
  *p = (p_v-((__m256i *) v.data()))-1;
  return false;
}

template<TRACE trace = TRACE::DISABLED>
class order_book_soa_avx2 : public order_book<order_book_soa_avx2<trace>, order_price_t, trace>
{
public:
  static constexpr int32_t price_sentinel = int32_t(1<<30);

  using sorted_prices_t = AlignedVector<sprice_t, Alignment::AVX2>;
  using sorted_qtys_t = AlignedVector<qty_t, Alignment::AVX2>;

  order_book_soa_avx2():
    lasti8(0),
    m_bid_prices(sprice_t(price_sentinel)),
    m_ask_prices(sprice_t(price_sentinel)),
    m_bid_qtys(qty_t(0)),
    m_ask_qtys(qty_t(0))
  { }

  sorted_prices_t m_bid_prices;
  sorted_prices_t m_ask_prices;
  sorted_qtys_t m_bid_qtys;
  sorted_qtys_t m_ask_qtys;
  int lasti8;
  bool check_order_bid( const order_price_t *order ) const {
    return is_bid( order->m_price );
  }

#if CROSS_CHECK
  void crosscheck( order_id_t oid, size_t book_idx, bool is_bid ) {
    const auto& book = order_book_scalar<TRACE::DISABLED>::s_books[book_idx];
    auto ref_side = is_bid ? book.m_bids : book.m_asks;
    const auto& our_prices = is_bid ? m_bid_prices : m_ask_prices;
    const auto& our_qtys = is_bid ? m_bid_qtys : m_ask_qtys;
    auto compare = [&]() -> bool {
      for ( size_t i = 0; i < ref_side.size(); i++ ) {
        if( ref_side[i].m_price != our_prices[i] ) {
          return false;
        }
        if( order_book_scalar<TRACE::DISABLED>::s_levels[ref_side[i].m_ptr].m_qty != our_qtys[i] ) {
          return false;
        }
      }
      return true;
    };
    if ( !compare() ) {
      printf("CROSSCHECK FAILED on order %u side %s\n", uint32_t(oid), is_bid ? "BID" : "ASK" );
      printf( "Reference: ");
      for ( size_t i = 0; i < ref_side.size(); i++ ) {
        printf( "(%d, %d) ", ref_side[i].m_price, order_book_scalar<TRACE::DISABLED>::s_levels[ref_side[i].m_ptr].m_qty );
      }
      printf( "\nOur book: ");
      for ( size_t i = 0; our_prices[i] != price_sentinel; i++ ) {
        printf( "(%d, %d) ", our_prices[i], our_qtys[i] );
      }
      printf( "\n" );
      exit(1);
    }
  }
#endif
  void ADD_ORDER(order_price_t *order, sprice_t const price, qty_t const qty)
  {
    sorted_prices_t& sorted_prices = is_bid(price) ? m_bid_prices : m_ask_prices;
    sorted_qtys_t& sorted_qtys = is_bid(price) ? m_bid_qtys : m_ask_qtys;

    int i8;
    __m256i v_prices, v_cmpeq, v_cmpgt;

    bool soa_found = Search_avx2( &i8, sorted_prices, price, v_prices, v_cmpeq, v_cmpgt, lasti8 );
    lasti8 = i8;
    if ( soa_found ) {
        __m256i v_qtys = _mm256_load_si256( (__m256i *) sorted_qtys.data() + i8 );
                v_qtys = _mm256_add_epi32( v_qtys, _mm256_and_si256( v_cmpeq, _mm256_set1_epi32( int32_t(qty) ) ) );
        _mm256_store_si256( (__m256i *) sorted_qtys.data() + i8, v_qtys );
    }
    else {
        __m256i v_price = _mm256_set1_epi32( int32_t( price ) );

        v_prices = _mm256_load_si256( (__m256i *) sorted_prices.data() + i8 );
        v_cmpgt = _mm256_cmpgt_epi32( v_prices, v_price );
        __m256i v_qtys = _mm256_load_si256( (__m256i *) sorted_qtys.data() + i8 );

        __m256i v_msdw_price = _mm256_permutevar8x32_epi32( v_prices, _mm256_set1_epi32( 7 ) );
        __m256i v_msdw_qty = _mm256_permutevar8x32_epi32( v_qtys, _mm256_set1_epi32( 7 ) );

        __m256i v_insertion_mask = _mm256_xor_si256( v_cmpgt, _mm256_sll_4b_si256<4>( v_cmpgt ) );

        __m256i v_output_price = _mm256_blendv_epi8( v_prices, _mm256_sll_4b_si256<4>( v_prices ), v_cmpgt );
                v_output_price = _mm256_blendv_epi8( v_output_price, v_price, v_insertion_mask );
        __m256i v_output_qty = _mm256_blendv_epi8( v_qtys, _mm256_sll_4b_si256<4>( v_qtys ), v_cmpgt );
                v_output_qty = _mm256_blendv_epi8( v_output_qty, _mm256_set1_epi32( int32_t(qty) ), v_insertion_mask );

        __m256i *p_p = ((__m256i *) sorted_prices.data() + i8 );
        __m256i *p_q = ((__m256i *) sorted_qtys.data() + i8 );

        bool sentinels = false;
        do {
            _mm256_store_si256( p_p /*(__m256i *) sorted_prices.data() + i8*/, v_output_price );
            _mm256_store_si256( p_q /*(__m256i *) sorted_qtys.data() + i8*/, v_output_qty );

            i8 += 1;
            sentinels = 0 != _mm256_movemask_ps( _mm256_castsi256_ps( _mm256_cmpeq_epi32( v_output_price, _mm256_set1_epi32( price_sentinel ) ) ) );
            if ( sentinels ) break;
            p_p += 1;
            p_q += 1;

            __m256i v_next_price = _mm256_load_si256( p_p /*(__m256i *) sorted_prices.data() + i8*/ );
            __m256i v_next_qty = _mm256_load_si256( p_q /*(__m256i *) sorted_qtys.data() + i8*/ );

            v_output_price = _mm256_sll_4b_si256<4>( v_next_price );
            v_output_qty = _mm256_sll_4b_si256<4>( v_next_qty );
            v_output_price = _mm256_blend_epi32( v_output_price, v_msdw_price, 1 );
            v_output_qty = _mm256_blend_epi32( v_output_qty, v_msdw_qty, 1 );

            v_msdw_price = _mm256_srl_4b_si256<28>( v_next_price );
            v_msdw_qty = _mm256_srl_4b_si256<28>( v_next_qty );
        } while ( ! sentinels );

        // Update maxi8 after insertion
        sorted_prices.setN8(i8);
        sorted_qtys.setN8(i8);
    }

#if CROSS_CHECK
    order_book_scalar<TRACE::DISABLED>::add_order( order->oid, order->book_idx, price, qty );
    crosscheck( order->oid, order->book_idx, is_bid(price) );
#endif
  }

  // shared between cancel(aka partial cancel aka reduce) and execute
  void REDUCE_ORDER(order_price_t *order, qty_t const qty)
  {
#if CROSS_CHECK
    order_book_scalar<TRACE::DISABLED>::cancel_order( order->oid, qty );
#endif
    sorted_prices_t& sorted_prices = is_bid(order->m_price) ? m_bid_prices : m_ask_prices;
    sorted_qtys_t& sorted_qtys = is_bid(order->m_price) ? m_bid_qtys : m_ask_qtys;

    __m256i v_prices;
    __m256i v_cmpeq;
    int cmpeq;
    __m256i *p = (__m256i *) sorted_prices.data();
    do {
      v_prices = *p++;
      v_cmpeq = _mm256_cmpeq_epi32( v_prices, _mm256_set1_epi32( order->m_price ) );
      cmpeq = _mm256_movemask_ps( _mm256_castsi256_ps( v_cmpeq ) );
    } while ( ! cmpeq );
    size_t i8 = (p - (__m256i *) sorted_prices.data()) - 1;
    __m256i v_qtys = _mm256_load_si256( (__m256i *) sorted_qtys.data() + i8 );
    __m256i v_masked_order = _mm256_and_si256( v_cmpeq, _mm256_set1_epi32( int32_t(qty) ) );
            v_qtys = _mm256_sub_epi32( v_qtys, v_masked_order );
      _mm256_store_si256( (__m256i *) sorted_qtys.data() + i8, v_qtys );

#if CROSS_CHECK
    //order_book_scalar::cancel_order( order->oid, qty );
    crosscheck( order->oid, order->book_idx, is_bid( order->m_price ) );
#endif
    order->m_qty -= qty;
  }
  // shared between delete and execute
  void DELETE_ORDER(order_price_t *order)
  {
    sorted_prices_t& sorted_prices = is_bid(order->m_price) ? m_bid_prices : m_ask_prices;
    sorted_qtys_t& sorted_qtys = is_bid(order->m_price) ? m_bid_qtys : m_ask_qtys;

    __m256i v_prices;
    __m256i v_cmpeq;
    __m256i v_cmpgt;
    int cmpeq;
    __m256i *p = (__m256i *) sorted_prices.data();
    do {
      v_prices = *p++;
      v_cmpeq = _mm256_cmpeq_epi32( v_prices, _mm256_set1_epi32( order->m_price ) );
      v_cmpgt = _mm256_cmpgt_epi32( v_prices, _mm256_set1_epi32( order->m_price ) );
      cmpeq = _mm256_movemask_ps( _mm256_castsi256_ps( v_cmpeq ) );
    } while ( ! cmpeq );
    size_t i8 = (p - (__m256i *) sorted_prices.data()) - 1;
    __m256i v_qtys = _mm256_load_si256( (__m256i *) sorted_qtys.data() + i8 );
    __m256i v_masked_order = _mm256_and_si256( v_cmpeq, _mm256_set1_epi32( int32_t(order->m_qty) ) );
            v_qtys = _mm256_sub_epi32( v_qtys, _mm256_and_si256( v_cmpeq, v_masked_order ) );
    __m256i v_qty0 = _mm256_cmpeq_epi32( _mm256_setzero_si256(), _mm256_and_si256( v_qtys, v_cmpeq ) );
      _mm256_store_si256( (__m256i *) sorted_qtys.data() + i8, v_qtys );
    if ( 0xff == _mm256_movemask_ps( _mm256_castsi256_ps( v_qty0 ) ) ) {
      // need to shift the price and qty arrays left starting at i8
      __m256i *p_p = (__m256i *) sorted_prices.data() + i8;
      __m256i *p_q = (__m256i *) sorted_qtys.data() + i8;
      __m256i v_cmpge = _mm256_or_si256( v_cmpeq, v_cmpgt );
      __m256i v_output_price = _mm256_blendv_epi8( v_prices, _mm256_srl_4b_si256<4>( v_prices ), v_cmpge );
      __m256i v_output_qty = _mm256_blendv_epi8( v_qtys, _mm256_srl_4b_si256<4>( v_qtys ), v_cmpge );
      __m256i v_next_price = _mm256_load_si256( p_p+1 /*(__m256i *) sorted_prices.data() + i8 + 1*/ );
      __m256i v_next_qty = _mm256_load_si256( p_q+1 /*(__m256i *) sorted_qtys.data() + i8 + 1*/ );
      do {
        __m256i v_lsdw_price = _mm256_broadcastd_epi32( _mm256_castsi256_si128( v_next_price ) );
        __m256i v_lsdw_qty = _mm256_broadcastd_epi32( _mm256_castsi256_si128( v_next_qty ) );
        v_output_price = _mm256_blend_epi32( v_output_price, v_lsdw_price, 0x80 );
        v_output_qty = _mm256_blend_epi32( v_output_qty, v_lsdw_qty, 0x80 );
        _mm256_store_si256( p_p /*(__m256i *) sorted_prices.data() + i8*/, v_output_price );
        _mm256_store_si256( p_q /*(__m256i *) sorted_qtys.data() + i8*/, v_output_qty );
        i8 += 1;
        p_p += 1;
        p_q += 1;

        v_output_price = _mm256_srl_4b_si256<4>( v_next_price );
        v_output_qty = _mm256_srl_4b_si256<4>( v_next_qty );
        v_next_price = _mm256_load_si256( p_p+1 /*(__m256i *) sorted_prices.data() + i8 + 1*/ );
        v_next_qty = _mm256_load_si256( p_q+1 /*(__m256i *) sorted_qtys.data() + i8 + 1*/ );
      } while ( i8 < sorted_prices.getN8() );
    }
#if CROSS_CHECK
    order_book_scalar<TRACE::DISABLED>::delete_order( order->oid );
    crosscheck( order->oid, order->book_idx, is_bid( order->m_price ) );
#endif
  }
};


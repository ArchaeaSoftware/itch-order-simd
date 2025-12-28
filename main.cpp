#include <chrono>
#include <cstdio>
#include <limits>
#include <memory>
#include <fcntl.h>
#include <iostream>
#include "bufferedreader.h"
#include "itch.h"
#include "order_book.h"

std::vector<symbol_t> symbol_from_locate;

template <itch_t __code>
class PROCESS
{
 public:
  static itch_message<__code> read_from(buf_t *__buf)
  {
    uint16_t const msglen = be16toh(*(uint16_t *)__buf->get(0));
    __buf->advance(2);
    assert(msglen == netlen<__code>);

    __buf->ensure(netlen<__code>);
    itch_message<__code> ret = itch_message<__code>::parse(__buf->get(0));
    __buf->advance(netlen<__code>);
    return ret;
  }
};

static sprice_t mksigned(price_t price, BUY_SELL buy)
{
  assert(price < std::numeric_limits<int32_t>::max());
  return buy == BUY_SELL::BUY ? price : -price;
}

#define DO_CASE(__itch_t)               \
  case (__itch_t): {                    \
    PROCESS<__itch_t>::read_from(&buf); \
    break;                              \
  }

template<typename T>
double
timeBacktest( const std::string filename )
{
  int fd = open( filename.c_str(), O_RDONLY );

  if ( fd < 0 ) {
    fprintf( stderr, "Could not open file %s\n", filename.c_str() );
    return 0.0;
  }

  buf_t buf(fd);
  std::chrono::steady_clock::time_point start;
  size_t npkts = 0;
  // order_book::oid_map.max_load_factor(0.5);
  T::oid_map.reserve(order_id_t(184118975 * 2));  // the first number
                                                  // is the empirically
                                                  // largest oid seen.
                                                  // multiply by 2 for
                                                  // good measure
  printf("%lu\n", sizeof(T) * T::MAX_BOOKS);
  while (is_ok(buf.ensure(3))) {
    if (npkts) ++npkts;
    itch_t const msgtype = itch_t(*buf.get(2));
    switch (msgtype) {
      DO_CASE(itch_t::SYSEVENT);
      DO_CASE(itch_t::STOCK_DIRECTORY);
      DO_CASE(itch_t::TRADING_ACTION);
      DO_CASE(itch_t::REG_SHO_RESTRICT);
      DO_CASE(itch_t::MPID_POSITION);
      DO_CASE(itch_t::MWCB_DECLINE);
      DO_CASE(itch_t::MWCB_STATUS);
      DO_CASE(itch_t::IPO_QUOTE_UPDATE);
      DO_CASE(itch_t::TRADE);
      DO_CASE(itch_t::CROSS_TRADE);
      DO_CASE(itch_t::BROKEN_TRADE);
      DO_CASE(itch_t::NET_ORDER_IMBALANCE);
      DO_CASE(itch_t::RETAIL_PRICE_IMPROVEMENT);
      DO_CASE(itch_t::PROCESS_LULD_AUCTION_COLLAR_MESSAGE);

      case (itch_t::ADD_ORDER): {
        auto const pkt = PROCESS<itch_t::ADD_ORDER>::read_from(&buf);

        if (!npkts) {
          start = std::chrono::steady_clock::now();
          ++npkts;
        }

        assert(uint64_t(pkt.oid) <
               uint64_t(std::numeric_limits<int32_t>::max()));
        T::add_order(order_id_t(pkt.oid), book_id_t(pkt.stock_locate),
                              mksigned(pkt.price, pkt.buy), pkt.qty);
        break;
      }
      case (itch_t::ADD_ORDER_MPID): {
        auto const pkt = PROCESS<itch_t::ADD_ORDER_MPID>::read_from(&buf);
        T::add_order(
            order_id_t(pkt.add_msg.oid), book_id_t(pkt.add_msg.stock_locate),
            mksigned(pkt.add_msg.price, pkt.add_msg.buy), pkt.add_msg.qty);
        break;
      }
      case (itch_t::EXECUTE_ORDER): {
        auto const pkt = PROCESS<itch_t::EXECUTE_ORDER>::read_from(&buf);
        T::execute_order(order_id_t(pkt.oid), pkt.qty);
        break;
      }
      case (itch_t::EXECUTE_ORDER_WITH_PRICE): {
        auto const pkt =
            PROCESS<itch_t::EXECUTE_ORDER_WITH_PRICE>::read_from(&buf);
        T::execute_order(order_id_t(pkt.exec.oid), pkt.exec.qty);
        break;
      }
      case (itch_t::REDUCE_ORDER): {
        auto const pkt = PROCESS<itch_t::REDUCE_ORDER>::read_from(&buf);
        T::cancel_order(order_id_t(pkt.oid), pkt.qty);
        break;
      }
      case (itch_t::DELETE_ORDER): {
        auto const pkt = PROCESS<itch_t::DELETE_ORDER>::read_from(&buf);
        T::delete_order(order_id_t(pkt.oid));
        break;
      }
      case (itch_t::REPLACE_ORDER): {
        auto const pkt = PROCESS<itch_t::REPLACE_ORDER>::read_from(&buf);
        T::replace_order(order_id_t(pkt.oid),
                                  order_id_t(pkt.new_order_id), pkt.new_qty,
                                  mksigned(pkt.new_price, BUY_SELL::BUY));
        // actually it will get re-signed inside. code smell
        break;
      }
      default: {
        printf("Uh oh bad code %d\n", char(msgtype));
        assert(false);
        break;
      }
    }
  }

  std::vector<std::string> symbol_lookup(symbol_from_locate.size());
  for ( size_t i = 0; i < symbol_from_locate.size(); i++ ) {
    char *s = string_from_locate( i );
    if ( '\0'==s[0] ) continue;
    for ( size_t j = 0; j < 8; j++ ) {
        if ( ' '==s[j] ) {
            s[j] = '\0';
            break;
        }
    }
    symbol_lookup[i] = s;
  }

  std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
  size_t nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

  printf("%lu packets in %lu nanos , %.2f nanos per packet \n", npkts, nanos,
         nanos / (double)npkts);
  return nanos / (double)npkts;
}

int main(int argc, char *argv[])
{
  std::string filename;
  bool enable_trace = false;
  std::string isa = "scalar";  // default to scalar implementation

  auto print_usage = [argv]() -> void {
      fprintf(stderr, "Usage: %s [options]\n", argv[0]);
      fprintf(stderr, "Options:\n");
      fprintf(stderr, "  --file <path>, -f <path>    Input ITCH file\n");
      fprintf(stderr, "  --isa <implementation>      Order book implementation\n");
      fprintf(stderr, "                              (scalar, soa, soa_price, avx2)\n");
      fprintf(stderr, "                              Default: scalar\n");
      fprintf(stderr, "  --trace                     Enable trace mode\n");
      fprintf(stderr, "  --help, -h                  Show this help message\n");
  };

  // Parse command line arguments
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--trace") {
      enable_trace = true;
    } else if (arg == "--file" || arg == "-f") {
      if (i + 1 < argc) {
        filename = argv[++i];
      } else {
        fprintf(stderr, "Error: --file requires an argument\n");
        return 1;
      }
    } else if (arg == "--isa") {
      if (i + 1 < argc) {
        isa = argv[++i];
      } else {
        fprintf(stderr, "Error: --isa requires an argument\n");
        return 1;
      }
    } else if (arg == "--help" || arg == "-h") {
      print_usage();
    } else if (arg[0] == '-') {
      fprintf(stderr, "Unknown option: %s\n", arg.c_str());
      print_usage();
      return 1;
    } else {
      // Positional argument - treat as filename for backwards compatibility
      filename = arg;
    }
  }

  if (filename.empty()) {
    fprintf(stderr, "Error: No input file specified\n");
    print_usage();
    return 1;
  }

  // Run with appropriate ISA and trace setting
  TRACE trace_mode = enable_trace ? TRACE::ENABLED : TRACE::DISABLED;

  if (isa == "scalar") {
    if (trace_mode == TRACE::ENABLED) {
      timeBacktest<order_book_scalar<TRACE::ENABLED>>( filename );
    } else {
      timeBacktest<order_book_scalar<TRACE::DISABLED>>( filename );
    }
  } else if (isa == "soa") {
    if (trace_mode == TRACE::ENABLED) {
      timeBacktest<order_book_soa<TRACE::ENABLED>>( filename );
    } else {
      timeBacktest<order_book_soa<TRACE::DISABLED>>( filename );
    }
  } else if (isa == "soa_price") {
    if (trace_mode == TRACE::ENABLED) {
      timeBacktest<order_book_soa_price<TRACE::ENABLED>>( filename );
    } else {
      timeBacktest<order_book_soa_price<TRACE::DISABLED>>( filename );
    }
  } else if (isa == "avx2") {
    if (trace_mode == TRACE::ENABLED) {
      timeBacktest<order_book_soa_avx2<TRACE::ENABLED>>( filename );
    } else {
      timeBacktest<order_book_soa_avx2<TRACE::DISABLED>>( filename );
    }
  } else {
    fprintf(stderr, "Error: Unknown ISA '%s'\n", isa.c_str());
    fprintf(stderr, "Valid options: scalar, soa, soa_price, avx2\n");
    return 1;
  }

  return 0;
}

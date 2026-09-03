#include "../src/process/livepeer_order.h"

#include <cassert>

int main() {
  Mist::LivepeerInsertOrder order(2);
  assert(order.current() == 0);
  assert(order.isCurrent(0));
  assert(!order.isCurrent(1));

  // A fast response or rejection for slot 1 must not skip unfinished slot 0.
  assert(!order.complete(1));
  assert(order.current() == 0);

  assert(order.complete(0));
  assert(order.current() == 1);
  assert(order.complete(1));
  assert(order.current() == 0);
  return 0;
}

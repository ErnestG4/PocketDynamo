// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host test: g++ -std=c++17 -O2 -I src/pd src/pd/ui/arp_test.cpp -o t && ./t
#include "arp.h"

#include <cassert>
#include <cstdio>

using namespace pd::arp;

namespace {
void check(bool c, const char* m) { if (!c) { fprintf(stderr, "FAIL: %s\n", m); assert(false); } }
}  // namespace

int main() {
  const float pool[4] = {67.0f, 60.0f, 72.0f, 64.0f};   // pressed G C C' E
  Arp a;
  auto run = [&](Pattern p, int steps, int* got) {
    a.restart();
    for (int i = 0; i < steps; ++i) got[i] = a.next(p, pool, 4, 12345u * (i + 1), 0.0f);
  };
  int g[8];
  run(Pattern::Up, 5, g);
  check(g[0] == 1 && g[1] == 3 && g[2] == 0 && g[3] == 2 && g[4] == 1, "up: C E G C' then wraps");
  run(Pattern::Down, 4, g);
  check(g[0] == 2 && g[1] == 0 && g[2] == 3 && g[3] == 1, "down: C' G E C");
  run(Pattern::UpDown, 7, g);
  const int ud[7] = {1, 3, 0, 2, 0, 3, 1};
  for (int i = 0; i < 7; ++i) check(g[i] == ud[i], "up-down bounces without repeating the ends");
  run(Pattern::Order, 5, g);
  check(g[0] == 0 && g[1] == 1 && g[2] == 2 && g[3] == 3 && g[4] == 0, "order: as pressed");
  a.restart();
  int prev = -1;
  for (int i = 0; i < 200; ++i) {
    const int k = a.next(Pattern::Random, pool, 4, 2654435761u * (i + 7), 0.0f);
    check(k >= 0 && k < 4 && k != prev, "random stays in the pool and never repeats");
    prev = k;
  }
  check(a.next(Pattern::Motif, pool, 4, 0, 0.0f) == 1 && a.next(Pattern::Motif, pool, 4, 0, 0.99f) == 2,
        "motif: the gap quantile picks low to high");
  check(a.next(Pattern::Up, pool, 0, 0, 0.0f) == -1, "an empty pool plays nothing");
  // The pool shrinking under a running arp stays in range.
  a.restart();
  for (int i = 0; i < 3; ++i) a.next(Pattern::UpDown, pool, 4, 0, 0.0f);
  check(a.next(Pattern::UpDown, pool, 2, 0, 0.0f) < 2, "a shrinking pool stays in range");
  printf("arp ok\n");
  return 0;
}

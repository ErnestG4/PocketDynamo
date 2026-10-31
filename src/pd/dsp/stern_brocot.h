// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Stern-Brocot tree paths (Stern 1858, Brocot 1861): a frequency ratio named
// by its L/R descent from 1/1. Every positive rational appears exactly once,
// always in lowest terms; short paths are the simple, consonant ratios (R =
// 2/1, RL = 3/2, RLL = 4/3), deeper ones finer and more inharmonic, and the
// prefixes of one path are its successive mediants — continued-fraction
// convergents of wherever the path is heading. Ported from Brocot (Combust);
// the string path became a
// bit word, so walk/prefix/parent are integer ops. Control rate only.
#pragma once
#include <cstdint>

namespace pd {

struct SbPath {
  static constexpr int kMaxDepth = 32;

  uint32_t bits = 0;   // step i at bit i: 1 = R (right, > current), 0 = L
  uint8_t  len  = 0;

  constexpr SbPath() = default;
  // From a literal like "RLLLR"; anything but 'R'/'L' is skipped, and steps
  // past kMaxDepth are dropped.
  constexpr SbPath(const char* steps) {
    for (; steps && *steps && len < kMaxDepth; ++steps) {
      if (*steps == 'R') { bits |= 1u << len; ++len; }
      else if (*steps == 'L') ++len;
    }
  }

  constexpr int  depth() const { return len; }
  constexpr bool right(int i) const { return ((bits >> i) & 1u) != 0; }

  constexpr SbPath extended(bool rightStep) const {
    SbPath p = *this;
    if (p.len < kMaxDepth) {
      if (rightStep) p.bits |= 1u << p.len;
      ++p.len;
    }
    return p;
  }
  constexpr SbPath prefix(int n) const {
    SbPath p;
    p.len = (uint8_t)(n < 0 ? 0 : n > len ? len : n);
    p.bits = p.len >= 32 ? bits : bits & ((1u << p.len) - 1u);
    return p;
  }
  constexpr SbPath parent() const { return prefix(len - 1); }
  constexpr bool startsWith(const SbPath& o) const {
    return o.len <= len && prefix(o.len) == o;
  }
  constexpr SbPath commonPrefix(const SbPath& o) const {
    int i = 0;
    while (i < len && i < o.len && right(i) == o.right(i)) ++i;
    return prefix(i);
  }
  constexpr bool operator==(const SbPath& o) const { return len == o.len && bits == o.bits; }
  constexpr bool operator!=(const SbPath& o) const { return !(*this == o); }

  // Mediant walk from 1/1 between the bounds 0/1 and 1/0. Brocot's
  // convention: R moves the lower bound up to the current node, L the upper
  // bound down, then the node becomes the mediant of the bounds. 32 steps
  // alternating (the fastest growth, Fibonacci) stay below 2^23.
  constexpr void walk(uint32_t* p, uint32_t* q) const {
    uint32_t lp = 0, lq = 1, rp = 1, rq = 0, cp = 1, cq = 1;
    for (int i = 0; i < len; ++i) {
      if (right(i)) { lp = cp; lq = cq; }
      else          { rp = cp; rq = cq; }
      cp = lp + rp;
      cq = lq + rq;
    }
    *p = cp;
    *q = cq;
  }
  float ratio() const {
    uint32_t p = 1, q = 1;
    walk(&p, &q);
    return (float)p / (float)q;
  }

  // Path to p/q: the subtractive Euclidean algorithm is the descent itself
  // (p > q steps R and subtracts, else L), and a non-reduced p/q lands on the
  // same node as its lowest terms, so no gcd is needed. False — leaving *out
  // untouched — for a zero term or a descent deeper than maxDepth.
  static constexpr bool fromRatio(uint32_t p, uint32_t q, SbPath* out,
                                  int maxDepth = kMaxDepth) {
    if (p == 0 || q == 0) return false;
    SbPath path;
    while (p != q) {
      if (path.len >= maxDepth || path.len >= kMaxDepth) return false;
      if (p > q) { path = path.extended(true);  p -= q; }
      else       { path = path.extended(false); q -= p; }
    }
    *out = path;
    return true;
  }
};

}  // namespace pd

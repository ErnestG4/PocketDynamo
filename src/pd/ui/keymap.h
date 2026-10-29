// What each key on the 4x14 grid does. Two tracker-style piano octaves:
//   lower  z s x d c v g b h n j m , l . ; /     (C .. E, s d g h j l ; = sharps)
//   upper  q 2 w 3 e r 5 t 6 y 7 u i 9 o 0 p [ = ]  (C .. G)
// Everything else is a control key. Pure table — host-testable.
#pragma once
#include <cstdint>

namespace pd {
namespace keymap {

// Semitone offset from the lower C, or -1 for a non-note key.
constexpr int8_t kNote[4][14] = {
  // `   1   2   3   4   5   6   7   8   9   0   -   =  BS
  {-1, -1, 13, 15, -1, 18, 20, 22, -1, 25, 27, -1, 30, -1},
  // TAB q   w   e   r   t   y   u   i   o   p   [   ]   \  .
  {-1, 12, 14, 16, 17, 19, 21, 23, 24, 26, 28, 29, 31, -1},
  // FN SHIFT a  s   d   f   g   h   j   k   l   ;   '  ENTER
  {-1, -1, -1,  1,  3, -1,  6,  8, 10, -1, 13, 15, -1, -1},
  // CTRL OPT ALT z  x   c   v   b   n   m   ,   .   /  SPACE
  {-1, -1, -1,  0,  2,  4,  5,  7,  9, 11, 12, 14, 16, -1},
};

// Printable label for each key, for the display and logs.
constexpr char kLabel[4][14] = {
  {'`', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '<'},
  {'>', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\\'},
  {'F', '^', 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '#'},
  {'C', 'O', 'A', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', '_'},
};

constexpr bool isFn(int r, int c)    { return r == 2 && c == 0; }
constexpr bool isShift(int r, int c) { return r == 2 && c == 1; }
constexpr bool isTab(int r, int c)   { return r == 1 && c == 0; }

}  // namespace keymap
}  // namespace pd

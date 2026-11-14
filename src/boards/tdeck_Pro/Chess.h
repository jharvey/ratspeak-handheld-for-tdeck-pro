#pragma once
// Chess rules + a small engine for the Library's chess game (no UI here; StandaloneApp
// draws it). Squares: a1 = 0, b1 = 1 ... h8 = 63. Pieces: >0 white, <0 black.
// Copy-make: make() changes the position in place; callers keep a copy to go back.
#include <stddef.h>
#include <stdint.h>

namespace chess {

enum Piece : int8_t { EMPTY = 0, PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6 };
enum MoveFlag : uint8_t { F_CAPTURE = 1, F_EP = 2, F_CASTLE = 4, F_DOUBLE = 8 };

struct Move {
    uint8_t from = 0, to = 0;
    int8_t promo = 0;            // piece type (KNIGHT..QUEEN) or 0
    uint8_t flags = 0;
    bool operator==(const Move& o) const { return from == o.from && to == o.to && promo == o.promo; }
    bool valid() const { return from != to; }
};

struct Pos {
    int8_t sq[64];
    int8_t side;                 // +1 white to move, -1 black
    uint8_t castle;              // 1 = white O-O, 2 = white O-O-O, 4 = black O-O, 8 = black O-O-O
    int8_t ep;                   // en-passant target square, or -1
    uint8_t half;                // half-moves since a capture or pawn move (50-move rule)
    uint16_t full;               // move number
    uint64_t key;                // position hash (repetition)
};

enum Result : uint8_t { ONGOING, CHECKMATE, STALEMATE, DRAW_50, DRAW_MATERIAL, DRAW_REPEAT };

constexpr int kMaxMoves = 256;

void startPos(Pos& p);
bool attacked(const Pos& p, int sq, int bySide);
bool inCheck(const Pos& p);                         // side to move
int legalMoves(const Pos& p, Move* out);           // out: kMaxMoves
void make(Pos& p, const Move& m);
Result result(const Pos& p, const uint64_t* hist, int histN);   // hist: keys of earlier positions
// SAN-like name of m in position p: "e4", "Nf3", "exd5", "O-O", "e8=Q", "+" / "#" added.
void moveName(const Pos& p, const Move& m, char* out, size_t cap);

// FEN without the clocks' validation; true if it parsed.
void toFen(const Pos& p, char* out, size_t cap);
bool fromFen(Pos& p, const char* fen);

struct ThinkInfo { int score = 0; int depth = 0; uint32_t nodes = 0; uint32_t ms = 0; };
// Best move for the side to move. level 1-4 (1 = beginner, 4 = strongest), maxMs caps
// the time. *stop (optional) aborts early. Returns an invalid move if there is none.
Move think(const Pos& p, int level, uint32_t maxMs, const uint64_t* hist, int histN,
           ThinkInfo* info = nullptr, volatile bool* stop = nullptr);

inline int file(int s) { return s & 7; }
inline int rank(int s) { return s >> 3; }

}  // namespace chess

#include "Chess.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace chess {
namespace {

const int8_t kKnight[8][2] = {{1, 2}, {2, 1}, {2, -1}, {1, -2}, {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
const int8_t kKing[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
const int8_t kDiag[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
const int8_t kOrth[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
const int kValue[7] = {0, 100, 320, 330, 500, 900, 0};
constexpr int kInf = 32000, kMate = 30000, kMaxPly = 48, kMaxQ = 8;

inline int at(int f, int r) { return ((f | r) & ~7) ? -1 : r * 8 + f; }
inline int absi(int v) { return v < 0 ? -v : v; }

// ---- Position hash (fixed seed: the same position always hashes the same) --------------
// In PSRAM when there is some (6.8 KB the radio and messages need more in internal RAM).
struct ZTable { uint64_t piece[13][64], castle[16], ep[8], side; };
ZTable* g_z = nullptr;
bool g_zReady = false;
uint64_t next64(uint64_t& s) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
void zinit() {
    if (g_zReady) return;
    g_z = (ZTable*)heap_caps_malloc(sizeof(ZTable), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_z) g_z = (ZTable*)malloc(sizeof(ZTable));
    if (!g_z) abort();
    uint64_t s = 0x9E3779B97F4A7C15ull;
    for (auto& row : g_z->piece) for (auto& v : row) v = next64(s);
    for (auto& v : g_z->castle) v = next64(s);
    for (auto& v : g_z->ep) v = next64(s);
    g_z->side = next64(s);
    g_zReady = true;
}
uint64_t hashOf(const Pos& p) {
    zinit();
    uint64_t k = 0;
    for (int s = 0; s < 64; s++) if (p.sq[s]) k ^= g_z->piece[p.sq[s] + 6][s];
    k ^= g_z->castle[p.castle & 15];
    if (p.ep >= 0) k ^= g_z->ep[file(p.ep)];
    if (p.side < 0) k ^= g_z->side;
    return k;
}

int kingSq(const Pos& p, int side) {
    for (int s = 0; s < 64; s++) if (p.sq[s] == side * KING) return s;
    return -1;
}

// Pseudo-legal moves (own king may be left in check); castling checks the squares crossed.
int gen(const Pos& p, Move* out, bool capsOnly, int capN = kMaxMoves) {
    int n = 0;
    const int s = p.side;
    auto add = [&](int from, int to, uint8_t flags, int8_t promo = 0) {
        if (n >= capN) return;
        Move& m = out[n++];
        m.from = (uint8_t)from; m.to = (uint8_t)to; m.flags = flags; m.promo = promo;
    };
    auto addPawn = [&](int from, int to, uint8_t flags) {
        if (rank(to) == (s > 0 ? 7 : 0)) {
            add(from, to, flags, QUEEN);
            add(from, to, flags, KNIGHT);
            add(from, to, flags, ROOK);
            add(from, to, flags, BISHOP);
        } else {
            add(from, to, flags);
        }
    };
    for (int sq = 0; sq < 64; sq++) {
        const int pc = p.sq[sq];
        if (pc * s <= 0) continue;
        const int f = file(sq), r = rank(sq);
        switch (absi(pc)) {
        case PAWN: {
            const int r1 = r + s;
            const int to = at(f, r1);
            if (to >= 0 && !p.sq[to] && (!capsOnly || rank(to) == (s > 0 ? 7 : 0))) {
                addPawn(sq, to, 0);
                if (!capsOnly && r == (s > 0 ? 1 : 6)) {
                    const int to2 = at(f, r + 2 * s);
                    if (!p.sq[to2]) add(sq, to2, F_DOUBLE);
                }
            }
            for (int df = -1; df <= 1; df += 2) {
                const int t = at(f + df, r1);
                if (t < 0) continue;
                if (p.sq[t] * s < 0) addPawn(sq, t, F_CAPTURE);
                else if (t == p.ep) add(sq, t, F_CAPTURE | F_EP);
            }
            break;
        }
        case KNIGHT:
        case KING: {
            const int8_t(*d)[2] = absi(pc) == KNIGHT ? kKnight : kKing;
            for (int i = 0; i < 8; i++) {
                const int t = at(f + d[i][0], r + d[i][1]);
                if (t < 0) continue;
                const int v = p.sq[t];
                if (v * s > 0 || (!v && capsOnly)) continue;
                add(sq, t, v ? F_CAPTURE : 0);
            }
            if (absi(pc) == KING && !capsOnly) {
                const int home = s > 0 ? 4 : 60, kBit = s > 0 ? 1 : 4, qBit = s > 0 ? 2 : 8;
                if (sq == home && !attacked(p, home, -s)) {
                    if ((p.castle & kBit) && !p.sq[home + 1] && !p.sq[home + 2] && p.sq[home + 3] == s * ROOK &&
                        !attacked(p, home + 1, -s) && !attacked(p, home + 2, -s))
                        add(home, home + 2, F_CASTLE);
                    if ((p.castle & qBit) && !p.sq[home - 1] && !p.sq[home - 2] && !p.sq[home - 3] &&
                        p.sq[home - 4] == s * ROOK && !attacked(p, home - 1, -s) && !attacked(p, home - 2, -s))
                        add(home, home - 2, F_CASTLE);
                }
            }
            break;
        }
        default: {
            const int t0 = absi(pc);
            for (int k = 0; k < 2; k++) {
                if (k == 0 && t0 == ROOK) continue;      // rook: straight only
                if (k == 1 && t0 == BISHOP) continue;    // bishop: diagonal only
                const int8_t(*d)[2] = k == 0 ? kDiag : kOrth;
                for (int i = 0; i < 4; i++) {
                    for (int st = 1; st < 8; st++) {
                        const int t = at(f + d[i][0] * st, r + d[i][1] * st);
                        if (t < 0) break;
                        const int v = p.sq[t];
                        if (v * s > 0) break;
                        if (v) { add(sq, t, F_CAPTURE); break; }
                        if (!capsOnly) add(sq, t, 0);
                    }
                }
            }
            break;
        }
        }
    }
    return n;
}

// After make(q, m) by `mover`: false if the mover's king is attacked (illegal move).
bool leavesKingSafe(const Pos& q, int mover) {
    const int k = kingSq(q, mover);
    return k >= 0 && !attacked(q, k, -mover);
}

// ---- Evaluation (white's view, centipawns) ------------------------------------------
int evalWhite(const Pos& p) {
    int score = 0, nonPawn[2] = {0, 0}, bishops[2] = {0, 0};
    for (int s = 0; s < 64; s++) {
        const int pc = p.sq[s];
        if (!pc) continue;
        const int t = absi(pc);
        if (t != PAWN && t != KING) nonPawn[pc > 0 ? 0 : 1] += kValue[t];
        if (t == BISHOP) bishops[pc > 0 ? 0 : 1]++;
    }
    const bool endgame = nonPawn[0] <= 1300 && nonPawn[1] <= 1300;
    for (int s = 0; s < 64; s++) {
        const int pc = p.sq[s];
        if (!pc) continue;
        const int t = absi(pc), side = pc > 0 ? 1 : -1;
        const int f = file(s), rr = side > 0 ? rank(s) : 7 - rank(s);   // rank from own side
        const int dist = (absi(2 * f - 7) + absi(2 * rank(s) - 7)) / 2;  // 0 centre .. 7 corner
        int v = kValue[t];
        switch (t) {
        case PAWN:
            v += (rr - 1) * 6;
            if ((f == 3 || f == 4) && (rr == 3 || rr == 4)) v += 12;
            if (rr == 6) v += 30;
            break;
        case KNIGHT: v += 15 - 5 * dist; break;
        case BISHOP: v += 10 - 3 * dist; break;
        case ROOK: if (rr == 6) v += 15; break;
        case QUEEN: v += 4 - dist; break;
        case KING:
            if (endgame) v += 20 - 6 * dist;
            else v += rr == 0 ? ((f <= 2 || f >= 6) ? 20 : 0) : -20 * rr;
            break;
        }
        score += side * v;
    }
    if (bishops[0] >= 2) score += 30;
    if (bishops[1] >= 2) score -= 30;
    return score;
}

// ---- Search ------------------------------------------------------------------------------
struct Ctx {
    uint32_t nodes = 0, deadline = 0;
    volatile bool* stop = nullptr;
    bool abort = false;
    const uint64_t* hist = nullptr;
    int histN = 0;
    uint64_t path[kMaxPly + kMaxQ + 2];
    Move killer[kMaxPly + 2];
};

bool timeUp(Ctx& c) {
    if ((++c.nodes & 1023) == 0 && ((int32_t)(millis() - c.deadline) >= 0 || (c.stop && *c.stop))) c.abort = true;
    return c.abort;
}

// Earlier occurrence since the last capture / pawn move: in this search line or in the game.
bool repeated(const Pos& p, const Ctx& c, int ply) {
    int back = 2;
    for (int i = ply - 2; i >= 0 && back <= p.half; i -= 2, back += 2)
        if (c.path[i] == p.key) return true;
    for (int i = c.histN - (back - ply); i >= 0 && back <= p.half; i -= 2, back += 2)
        if (c.hist[i] == p.key) return true;
    return false;
}

void order(const Pos& p, Move* mv, int16_t* sc, int n, const Move* killer) {
    for (int i = 0; i < n; i++) {
        int v = 0;
        if (mv[i].flags & F_CAPTURE) {
            const int victim = (mv[i].flags & F_EP) ? PAWN : absi(p.sq[mv[i].to]);
            v = 10000 + 10 * kValue[victim] / 2 - kValue[absi(p.sq[mv[i].from])] / 10;
        }
        if (mv[i].promo) v += 8000 + kValue[mv[i].promo] / 2;
        if (killer && mv[i] == *killer) v += 4000;
        sc[i] = (int16_t)v;
    }
}

// Next best by score (selection, swaps it to position i).
void pickNext(Move* mv, int16_t* sc, int n, int i) {
    int b = i;
    for (int j = i + 1; j < n; j++) if (sc[j] > sc[b]) b = j;
    if (b != i) {
        const Move tm = mv[i]; mv[i] = mv[b]; mv[b] = tm;
        const int16_t ts = sc[i]; sc[i] = sc[b]; sc[b] = ts;
    }
}

int quiesce(const Pos& p, int a, int b, int qply, Ctx& c) {
    if (timeUp(c)) return 0;
    const int stand = p.side * evalWhite(p);
    if (stand >= b) return b;
    if (stand > a) a = stand;
    if (qply >= kMaxQ) return a;
    Move mv[64];
    int16_t sc[64];
    const int n = gen(p, mv, true, 64);
    order(p, mv, sc, n, nullptr);
    for (int i = 0; i < n; i++) {
        pickNext(mv, sc, n, i);
        Pos q = p;
        make(q, mv[i]);
        if (!leavesKingSafe(q, p.side)) continue;
        const int v = -quiesce(q, -b, -a, qply + 1, c);
        if (c.abort) return 0;
        if (v >= b) return b;
        if (v > a) a = v;
    }
    return a;
}

int search(const Pos& p, int depth, int a, int b, int ply, Ctx& c) {
    c.path[ply] = p.key;
    if (ply > 0 && (p.half >= 100 || repeated(p, c, ply))) return 0;
    const bool check = attacked(p, kingSq(p, p.side), -p.side);
    if (check && ply < kMaxPly) depth++;                          // look further when in check
    if (depth <= 0 || ply >= kMaxPly) return quiesce(p, a, b, 0, c);
    if (timeUp(c)) return 0;
    Move mv[kMaxMoves];
    int16_t sc[kMaxMoves];
    const int n = gen(p, mv, false);
    order(p, mv, sc, n, &c.killer[ply]);
    int legal = 0, best = -kInf;
    for (int i = 0; i < n; i++) {
        pickNext(mv, sc, n, i);
        Pos q = p;
        make(q, mv[i]);
        if (!leavesKingSafe(q, p.side)) continue;
        legal++;
        const int v = -search(q, depth - 1, -b, -a, ply + 1, c);
        if (c.abort) return 0;
        if (v > best) best = v;
        if (v > a) a = v;
        if (a >= b) {
            if (!(mv[i].flags & F_CAPTURE)) c.killer[ply] = mv[i];
            break;
        }
    }
    if (!legal) return check ? -kMate + ply : 0;
    return best;
}

const char kPieceChar[7] = {' ', 'P', 'N', 'B', 'R', 'Q', 'K'};

}  // namespace

// ---- Rules ------------------------------------------------------------------------------
void startPos(Pos& p) {
    fromFen(p, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

bool attacked(const Pos& p, int sq, int by) {
    if (sq < 0) return false;
    const int f = file(sq), r = rank(sq);
    for (int df = -1; df <= 1; df += 2) {               // a pawn of `by` one rank behind
        const int s = at(f + df, r - by);
        if (s >= 0 && p.sq[s] == by * PAWN) return true;
    }
    for (int i = 0; i < 8; i++) {
        int s = at(f + kKnight[i][0], r + kKnight[i][1]);
        if (s >= 0 && p.sq[s] == by * KNIGHT) return true;
        s = at(f + kKing[i][0], r + kKing[i][1]);
        if (s >= 0 && p.sq[s] == by * KING) return true;
    }
    for (int i = 0; i < 4; i++) {
        for (int st = 1; st < 8; st++) {
            const int s = at(f + kDiag[i][0] * st, r + kDiag[i][1] * st);
            if (s < 0) break;
            const int v = p.sq[s];
            if (!v) continue;
            if (v == by * BISHOP || v == by * QUEEN) return true;
            break;
        }
        for (int st = 1; st < 8; st++) {
            const int s = at(f + kOrth[i][0] * st, r + kOrth[i][1] * st);
            if (s < 0) break;
            const int v = p.sq[s];
            if (!v) continue;
            if (v == by * ROOK || v == by * QUEEN) return true;
            break;
        }
    }
    return false;
}

bool inCheck(const Pos& p) { return attacked(p, kingSq(p, p.side), -p.side); }

int legalMoves(const Pos& p, Move* out) {
    Move all[kMaxMoves];
    const int n = gen(p, all, false);
    int k = 0;
    for (int i = 0; i < n; i++) {
        Pos q = p;
        make(q, all[i]);
        if (leavesKingSafe(q, p.side)) out[k++] = all[i];
    }
    return k;
}

void make(Pos& p, const Move& m) {
    const int s = p.side;
    const int8_t pc = p.sq[m.from];
    const int8_t cap = p.sq[m.to];
    p.half = (absi(pc) == PAWN || cap) ? 0 : (uint8_t)(p.half < 255 ? p.half + 1 : 255);
    p.sq[m.to] = pc;
    p.sq[m.from] = 0;
    if (m.flags & F_EP) p.sq[m.to - 8 * s] = 0;
    if (m.promo) p.sq[m.to] = (int8_t)(s * m.promo);
    if (m.flags & F_CASTLE) {
        const int rFrom = m.to > m.from ? m.to + 1 : m.to - 2, rTo = m.to > m.from ? m.to - 1 : m.to + 1;
        p.sq[rTo] = p.sq[rFrom];
        p.sq[rFrom] = 0;
    }
    p.ep = (m.flags & F_DOUBLE) ? (int8_t)(m.from + 8 * s) : (int8_t)-1;
    auto clear = [&](int sq) {
        if (sq == 4) p.castle &= ~3;
        if (sq == 60) p.castle &= ~12;
        if (sq == 7) p.castle &= ~1;
        if (sq == 0) p.castle &= ~2;
        if (sq == 63) p.castle &= ~4;
        if (sq == 56) p.castle &= ~8;
    };
    clear(m.from);
    clear(m.to);
    if (s < 0) p.full++;
    p.side = (int8_t)-s;
    p.key = hashOf(p);
}

Result result(const Pos& p, const uint64_t* hist, int histN) {
    Move mv[kMaxMoves];
    if (!legalMoves(p, mv)) return inCheck(p) ? CHECKMATE : STALEMATE;
    if (p.half >= 100) return DRAW_50;
    int minor = 0, other = 0;
    for (int s = 0; s < 64; s++) {
        const int t = absi(p.sq[s]);
        if (t == KNIGHT || t == BISHOP) minor++;
        else if (t && t != KING) other++;
    }
    if (!other && minor <= 1) return DRAW_MATERIAL;
    int seen = 0;
    for (int i = histN - 1; i >= 0 && histN - i <= p.half; i--) if (hist[i] == p.key) seen++;
    if (seen >= 2) return DRAW_REPEAT;
    return ONGOING;
}

void moveName(const Pos& p, const Move& m, char* out, size_t cap) {
    char b[16];
    int k = 0;
    const int t = absi(p.sq[m.from]);
    if (m.flags & F_CASTLE) {
        k = snprintf(b, sizeof(b), "%s", m.to > m.from ? "O-O" : "O-O-O");
    } else {
        if (t != PAWN) {
            b[k++] = kPieceChar[t];
            Move mv[kMaxMoves];                         // another piece of the same kind can go there too?
            const int n = legalMoves(p, mv);
            bool other = false, sameFile = false, sameRank = false;
            for (int i = 0; i < n; i++) {
                if (mv[i].to != m.to || mv[i].from == m.from || absi(p.sq[mv[i].from]) != t) continue;
                other = true;
                if (file(mv[i].from) == file(m.from)) sameFile = true;
                if (rank(mv[i].from) == rank(m.from)) sameRank = true;
            }
            if (other) {
                if (!sameFile) b[k++] = (char)('a' + file(m.from));
                else if (!sameRank) b[k++] = (char)('1' + rank(m.from));
                else { b[k++] = (char)('a' + file(m.from)); b[k++] = (char)('1' + rank(m.from)); }
            }
        } else if (m.flags & F_CAPTURE) {
            b[k++] = (char)('a' + file(m.from));
        }
        if (m.flags & F_CAPTURE) b[k++] = 'x';
        b[k++] = (char)('a' + file(m.to));
        b[k++] = (char)('1' + rank(m.to));
        if (m.promo) { b[k++] = '='; b[k++] = kPieceChar[m.promo]; }
        b[k] = 0;
    }
    Pos q = p;
    make(q, m);
    if (inCheck(q)) {
        Move mv[kMaxMoves];
        b[k++] = legalMoves(q, mv) ? '+' : '#';
        b[k] = 0;
    }
    snprintf(out, cap, "%s", b);
}

void toFen(const Pos& p, char* out, size_t cap) {
    char b[100];
    int k = 0;
    for (int r = 7; r >= 0; r--) {
        int empty = 0;
        for (int f = 0; f < 8; f++) {
            const int v = p.sq[r * 8 + f];
            if (!v) { empty++; continue; }
            if (empty) { b[k++] = (char)('0' + empty); empty = 0; }
            const char c = kPieceChar[absi(v)];
            b[k++] = v > 0 ? c : (char)(c + 32);
        }
        if (empty) b[k++] = (char)('0' + empty);
        if (r) b[k++] = '/';
    }
    b[k++] = ' ';
    b[k++] = p.side > 0 ? 'w' : 'b';
    b[k++] = ' ';
    if (!p.castle) b[k++] = '-';
    if (p.castle & 1) b[k++] = 'K';
    if (p.castle & 2) b[k++] = 'Q';
    if (p.castle & 4) b[k++] = 'k';
    if (p.castle & 8) b[k++] = 'q';
    b[k++] = ' ';
    if (p.ep < 0) b[k++] = '-';
    else { b[k++] = (char)('a' + file(p.ep)); b[k++] = (char)('1' + rank(p.ep)); }
    b[k] = 0;
    snprintf(out, cap, "%s %u %u", b, (unsigned)p.half, (unsigned)p.full);
}

bool fromFen(Pos& p, const char* fen) {
    Pos q;
    memset(&q, 0, sizeof(q));
    q.ep = -1;
    int r = 7, f = 0;
    const char* c = fen;
    for (; *c && *c != ' '; c++) {
        if (*c == '/') { r--; f = 0; continue; }
        if (*c >= '1' && *c <= '8') { f += *c - '0'; continue; }
        const char* hit = strchr("PNBRQKpnbrqk", *c);
        if (!hit || r < 0 || f > 7) return false;
        const int idx = (int)(hit - "PNBRQKpnbrqk");
        q.sq[r * 8 + f] = (int8_t)(idx < 6 ? idx + 1 : -(idx - 5));
        f++;
    }
    if (*c != ' ') return false;
    c++;
    q.side = *c == 'b' ? -1 : 1;
    while (*c && *c != ' ') c++;
    while (*c == ' ') c++;
    for (; *c && *c != ' '; c++) {
        if (*c == 'K') q.castle |= 1;
        if (*c == 'Q') q.castle |= 2;
        if (*c == 'k') q.castle |= 4;
        if (*c == 'q') q.castle |= 8;
    }
    while (*c == ' ') c++;
    if (*c >= 'a' && *c <= 'h' && c[1] >= '1' && c[1] <= '8') { q.ep = (int8_t)((c[1] - '1') * 8 + (c[0] - 'a')); c += 2; }
    else if (*c == '-') c++;
    unsigned half = 0, full = 1;
    sscanf(c, "%u %u", &half, &full);
    q.half = (uint8_t)(half > 255 ? 255 : half);
    q.full = (uint16_t)(full ? full : 1);
    if (kingSq(q, 1) < 0 || kingSq(q, -1) < 0) return false;
    q.key = hashOf(q);
    p = q;
    return true;
}

// ---- Engine --------------------------------------------------------------------------------
Move think(const Pos& p, int level, uint32_t maxMs, const uint64_t* hist, int histN, ThinkInfo* info,
           volatile bool* stop) {
    static const int kDepth[5] = {1, 1, 2, 3, 7};
    static const int kNoise[5] = {150, 150, 50, 12, 0};
    if (level < 1) level = 1;
    if (level > 4) level = 4;
    const uint32_t t0 = millis();
    Ctx* c = new Ctx();
    if (!c) return Move();
    c->deadline = t0 + maxMs;
    c->stop = stop;
    c->hist = hist;
    c->histN = histN;
    c->path[0] = p.key;

    Move root[kMaxMoves];
    int score[kMaxMoves];
    const int n = legalMoves(p, root);
    Move best;
    if (!n) { delete c; return best; }
    best = root[0];
    int bestScore = 0, doneDepth = 0;
    for (int i = 0; i < n; i++) score[i] = 0;
    if (n > 1) {
        for (int d = 1; d <= kDepth[level]; d++) {
            int tmp[kMaxMoves];
            int alpha = -kInf;
            bool complete = true;
            // Best-first from the last depth (stable selection by score).
            for (int i = 0; i < n; i++) {
                int b = i;
                for (int j = i + 1; j < n; j++) if (score[j] > score[b]) b = j;
                if (b != i) { const Move tm = root[i]; root[i] = root[b]; root[b] = tm; const int ts = score[i]; score[i] = score[b]; score[b] = ts; }
            }
            for (int i = 0; i < n; i++) {
                Pos q = p;
                make(q, root[i]);
                // Weaker levels need every move's real score (they pick near the best).
                const int lo = kNoise[level] ? -kInf : alpha;
                const int v = -search(q, d - 1, -kInf, lo == -kInf ? kInf : -lo, 1, *c);
                if (c->abort) { complete = false; break; }
                tmp[i] = v;
                if (v > alpha) alpha = v;
            }
            if (!complete) {
                if (d == 1) for (int i = 0; i < n; i++) score[i] = 0;
                break;
            }
            for (int i = 0; i < n; i++) score[i] = tmp[i];
            doneDepth = d;
            if (alpha > kMate - 100) break;                 // mate found
        }
        // Pick: best score, plus a random margin on the easier levels.
        int bi = 0, bv = -kInf * 2;
        for (int i = 0; i < n; i++) {
            const int noise = kNoise[level] ? (int)(esp_random() % (uint32_t)(kNoise[level] + 1)) : 0;
            const int v = score[i] + noise;
            if (v > bv) { bv = v; bi = i; }
        }
        best = root[bi];
        bestScore = score[bi];
    }
    if (info) {
        info->score = bestScore;
        info->depth = doneDepth;
        info->nodes = c->nodes;
        info->ms = millis() - t0;
    }
    delete c;
    return best;
}

}  // namespace chess

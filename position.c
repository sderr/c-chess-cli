/*
 * c-chess-cli, a command line interface for UCI chess engines. Copyright 2020 lucasart.
 *
 * c-chess-cli is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * c-chess-cli is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If
 * not, see <http://www.gnu.org/licenses/>.
*/
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include "position.h"
#include "util.h"

static const char *PieceLabel[NB_COLOR] = {"NBRQKP.", "nbrqkp."};

static uint64_t ZobristKey[NB_COLOR][NB_PIECE][NB_SQUARE];
static uint64_t ZobristCastling[NB_SQUARE], ZobristEnPassant[NB_SQUARE + 1], ZobristTurn;

static __attribute__((constructor)) void zobrist_init()
{
    uint64_t state = 0;

    for (int color = WHITE; color <= BLACK; color++)
        for (int piece = KNIGHT; piece < NB_PIECE; piece++)
            for (int square = A1; square <= H8; square++)
                ZobristKey[color][piece][square] = prng(&state);

    for (int square = A1; square <= H8; square++) {
        ZobristCastling[square] = prng(&state);
        ZobristEnPassant[square] = prng(&state);
    }

    ZobristEnPassant[NB_SQUARE] = prng(&state);
    ZobristTurn = prng(&state);

    assert(hash(ZobristKey, sizeof ZobristKey, 0) == 0x29f67831c4e3babd);
    assert(hash(ZobristCastling, sizeof ZobristCastling, 0) == 0xe83e371a6f469321);
    assert(hash(ZobristEnPassant, sizeof ZobristEnPassant, 0) == 0xe6832f7aa0522be2);
    assert(ZobristTurn == 0xba969207a2d24a3e);
}

static uint64_t zobrist_castling(bitboard_t castleRooks)
{
    bitboard_t k = 0;

    while (castleRooks)
        k ^= ZobristCastling[bb_pop_lsb(&castleRooks)];

    return k;
}

static str_t square_to_string(int square)
{
    BOUNDS(square, NB_SQUARE + 1);
    str_t str = str_new();

    if (square == NB_SQUARE)
        str_putc(&str, '-');
    else {
        str_putc(&str, file_of(square) + 'a');
        str_putc(&str, rank_of(square) + '1');
    }

    return str;
}

static int string_to_square(const char *str)
{
    return *str != '-'
        ? square_from(str[1] - '1', str[0] - 'a')
        : NB_SQUARE;
}

// Sets the position in its empty state (no pieces, white to play, rule50=0, etc.)
static void clear(Position *pos)
{
    memset(pos, 0, sizeof *pos);
}

// Remove 'piece' of 'color' on 'square'. Such a piece must be there first.
static void clear_square(Position *pos, int color, int piece, int square)
{
    BOUNDS(color, NB_COLOR);
    BOUNDS(piece, NB_PIECE);
    BOUNDS(square, NB_SQUARE);

    bb_clear(&pos->byColor[color], square);
    bb_clear(&pos->byPiece[piece], square);
    pos->key ^= ZobristKey[color][piece][square];
}

// Put 'piece' of 'color' on 'square'. Square must be empty first.
static void set_square(Position *pos, int color, int piece, int square)
{
    BOUNDS(color, NB_COLOR);
    BOUNDS(piece, NB_PIECE);
    BOUNDS(square, NB_SQUARE);

    bb_set(&pos->byColor[color], square);
    bb_set(&pos->byPiece[piece], square);
    pos->key ^= ZobristKey[color][piece][square];
}

static void finish(Position *pos)
{
    const int us = pos->turn, them = opposite(us);
    const int king = pos_king_square(pos, us);

    // ** Calculate pos->pins **

    pos->pins = 0;
    bitboard_t pinners = (pos_pieces_cpp(pos, them, ROOK, QUEEN) & bb_rook_attacks(king, 0))
        | (pos_pieces_cpp(pos, them, BISHOP, QUEEN) & bb_bishop_attacks(king, 0));

    while (pinners) {
        const int pinner = bb_pop_lsb(&pinners);
        bitboard_t skewered = Segment[king][pinner] & pos_pieces(pos);
        bb_clear(&skewered, king);
        bb_clear(&skewered, pinner);

        if (!bb_several(skewered) && (skewered & pos->byColor[us]))
            pos->pins |= skewered;
    }

    // ** Calculate pos->attacked **

    // King and Knight attacks
    pos->attacked = KingAttacks[pos_king_square(pos, them)];
    bitboard_t knights = pos_pieces_cp(pos, them, KNIGHT);

    while (knights)
        pos->attacked |= KnightAttacks[bb_pop_lsb(&knights)];

    // Pawn captures
    bitboard_t pawns = pos_pieces_cp(pos, them, PAWN);
    pos->attacked |= bb_shift(pawns & ~File[FILE_A], push_inc(them) + LEFT);
    pos->attacked |= bb_shift(pawns & ~File[FILE_H], push_inc(them) + RIGHT);

    // Sliders (using modified occupancy to see through a checked king)
    bitboard_t occ = pos_pieces(pos) ^ pos_pieces_cp(pos, opposite(them), KING);
    bitboard_t rookMovers = pos_pieces_cpp(pos, them, ROOK, QUEEN);

    while (rookMovers)
        pos->attacked |= bb_rook_attacks(bb_pop_lsb(&rookMovers), occ);

    bitboard_t bishopMovers = pos_pieces_cpp(pos, them, BISHOP, QUEEN);

    while (bishopMovers)
        pos->attacked |= bb_bishop_attacks(bb_pop_lsb(&bishopMovers), occ);

    // ** Calculate pos->checkers **

    if (bb_test(pos->attacked, king)) {
        pos->checkers = (pos_pieces_cp(pos, them, PAWN) & PawnAttacks[us][king])
            | (pos_pieces_cp(pos, them, KNIGHT) & KnightAttacks[king])
            | (pos_pieces_cpp(pos, them, ROOK, QUEEN) & bb_rook_attacks(king, pos_pieces(pos)))
            | (pos_pieces_cpp(pos, them, BISHOP, QUEEN) & bb_bishop_attacks(king, pos_pieces(pos)));

        // We can't be checked by the opponent king
        assert(!(pos_pieces_cp(pos, them, KING) & KingAttacks[king]));

        // Since our king is attacked, we must have at least one checker. Also more than 2 checkers
        // is impossible (even in Chess960).
        assert(pos->checkers && bb_count(pos->checkers) <= 2);
    } else
        pos->checkers = 0;

#ifndef NDEBUG
    // Verify that byColor[] and byPiece[] do not collide, and are consistent
    bitboard_t all = 0;

    for (int piece = KNIGHT; piece <= PAWN; piece++) {
        assert(!(pos->byPiece[piece] & all));
        all |= pos->byPiece[piece];
    }

    assert(!(pos->byColor[WHITE] & pos->byColor[BLACK]));
    assert(all == (pos->byColor[WHITE] | pos->byColor[BLACK]));

    // Verify piece counts
    for (int color = WHITE; color <= BLACK; color++) {
        assert(bb_count(pos_pieces_cpp(pos, color, KNIGHT, PAWN)) <= 10);
        assert(bb_count(pos_pieces_cpp(pos, color, BISHOP, PAWN)) <= 10);
        assert(bb_count(pos_pieces_cpp(pos, color, ROOK, PAWN)) <= 10);
        assert(bb_count(pos_pieces_cpp(pos, color, QUEEN, PAWN)) <= 9);
        assert(bb_count(pos_pieces_cp(pos, color, PAWN)) <= 8);
        assert(bb_count(pos_pieces_cp(pos, color, KING)) == 1);
        assert(bb_count(pos->byColor[color]) <= 16);
    }

    // Verify pawn ranks
    assert(!(pos->byPiece[PAWN] & (Rank[RANK_1] | Rank[RANK_8])));

    // Verify castle rooks
    if (pos->castleRooks) {
        assert(!(pos->castleRooks & ~((Rank[RANK_1] & pos_pieces_cp(pos, WHITE, ROOK))
            | (Rank[RANK_8] & pos_pieces_cp(pos, BLACK, ROOK)))));

        for (int color = WHITE; color <= BLACK; color++) {
            const bitboard_t b = pos->castleRooks & pos->byColor[color];

            if (bb_count(b) == 2)
                assert(Segment[bb_lsb(b)][bb_msb(b)] & pos_pieces_cp(pos, color, KING));
            else if (bb_count(b) == 1)
                assert(!(pos_pieces_cp(pos, color, KING) & (File[FILE_A] | File[FILE_H])));
            else
                assert(!b);
        }
    }

    // Verify ep square
    if (pos->epSquare != NB_SQUARE) {
        const int rank = rank_of(pos->epSquare);
        const int color = rank == RANK_3 ? WHITE : BLACK;

        assert(color != pos->turn);
        assert(!bb_test(pos_pieces(pos), pos->epSquare));
        assert(rank == RANK_3 || rank == RANK_6);
        assert(bb_test(pos_pieces_cp(pos, color, PAWN), pos->epSquare + push_inc(color)));
        assert(!bb_test(pos_pieces(pos), pos->epSquare - push_inc(color)));
    }

    // Verify key
    bitboard_t key = 0;

    for (int color = WHITE; color <= BLACK; color++)
        for (int piece = KNIGHT; piece <= PAWN; piece++) {
            bitboard_t b = pos_pieces_cp(pos, color, piece);

            while (b)
                key ^= ZobristKey[color][piece][bb_pop_lsb(&b)];
        }

    key ^= ZobristEnPassant[pos->epSquare] ^ (pos->turn == BLACK ? ZobristTurn : 0)
        ^ zobrist_castling(pos->castleRooks);
    assert(pos->key == key);

    // Verify turn and rule50
    assert(pos->turn == WHITE || pos->turn == BLACK);
    assert(pos->rule50 <= 100);
#endif
}

// Set position from FEN string
void pos_set(Position *pos, const char *fen)
{
    clear(pos);
    str_t token = str_new();

    // Piece placement
    fen = str_tok(fen, &token, " ");
    assert(token.len >= 10);
    int file = FILE_A, rank = RANK_8;

    for (const char *c = token.buf; *c; c++) {
        if ('1' <= *c && *c <= '8') {
            file += *c -'0';
            assert(file <= NB_FILE);
        } else if (*c == '/') {
            rank--;
            file = FILE_A;
        } else {
            assert(strchr("nbrqkpNBRQKP", *c));
            const bool color = islower(*c);
            set_square(pos, color, strchr(PieceLabel[color], *c) - PieceLabel[color],
                square_from(rank, file++));
        }
    }

    // Turn of play
    fen = str_tok(fen, &token, " ");
    assert(token.len == 1);

    if (token.buf[0] == 'w')
        pos->turn = WHITE;
    else {
        assert(token.buf[0] == 'b');
        pos->turn = BLACK;
        pos->key ^= ZobristTurn;
    }

    // Castling rights: optional, default '-'
    if ((fen = str_tok(fen, &token, " "))) {
        assert(token.len <= 4);

        for (const char *c = token.buf; *c; c++) {
            rank = isupper(*c) ? RANK_1 : RANK_8;
            char C = toupper(*c);

            if (C == 'K')
                bb_set(&pos->castleRooks, bb_msb(Rank[rank] & pos->byPiece[ROOK]));
            else if (C == 'Q')
                bb_set(&pos->castleRooks, bb_lsb(Rank[rank] & pos->byPiece[ROOK]));
            else if ('A' <= C && C <= 'H')
                bb_set(&pos->castleRooks, square_from(rank, C - 'A'));
            else
                assert(C == '-' && !pos->castleRooks && c[1] == '\0');
        }
    }

    pos->key ^= zobrist_castling(pos->castleRooks);

    // En passant square: optional, default '-'
    if (!(fen = str_tok(fen, &token, " ")))
        str_cpy(&token, "-");

    assert(token.len <= 2);
    pos->epSquare = string_to_square(token.buf);
    pos->key ^= ZobristEnPassant[pos->epSquare];

    // 50 move counter (in plies, starts at 0): optional, default 0
    pos->rule50 = (fen = str_tok(fen, &token, " ")) ? atoi(token.buf) : 0;

    // Full move counter (in moves, starts at 1): optional, default 1
    pos->fullMove = str_tok(fen, &token, " ") ? atoi(token.buf) : 1;

    str_free(&token);
    finish(pos);
}

// Get FEN string of position
str_t pos_get(const Position *pos)
{
    str_t fen = str_new();

    // Piece placement
    for (int rank = RANK_8; rank >= RANK_1; rank--) {
        int cnt = 0;

        for (int file = FILE_A; file <= FILE_H; file++) {
            const int square = square_from(rank, file);

            if (bb_test(pos_pieces(pos), square)) {
                if (cnt)
                    str_putc(&fen, cnt + '0');

                str_putc(&fen, PieceLabel[pos_color_on(pos, square)][pos_piece_on(pos, square)]);
                cnt = 0;
            } else
                cnt++;
        }

        if (cnt)
            str_putc(&fen, cnt + '0');

        str_putc(&fen, rank == RANK_1 ? ' ' : '/');
    }

    // Turn of play
    str_cat(&fen, pos->turn == WHITE ? "w " : "b ");

    // Castling rights
    if (!pos->castleRooks)
        str_putc(&fen, '-');
    else {
        for (int color = WHITE; color <= BLACK; color++) {
            const bitboard_t b = pos->castleRooks & pos->byColor[color];

            if (b) {
                const int king = pos_king_square(pos, color);

                // Right side castling
                if (b & Ray[king][king + RIGHT])
                    str_putc(&fen, PieceLabel[color][KING]);

                // Left side castling
                if (b & Ray[king][king + LEFT])
                    str_putc(&fen, PieceLabel[color][QUEEN]);
            }
        }
    }

    // En passant and 50 move
    str_t ep = square_to_string(pos->epSquare);
    str_catf(&fen, " %s %d %d", ep.buf, pos->rule50, pos->fullMove);
    str_free(&ep);

    return fen;
}

// Play a move on a position copy (original 'before' is untouched): pos = before + play(m)
void pos_move(Position *pos, const Position *before, move_t m)
{
    *pos = *before;

    pos->rule50++;
    pos->epSquare = NB_SQUARE;

    const int us = pos->turn, them = opposite(us);
    const int from = move_from(m), to = move_to(m), prom = move_prom(m);
    const int piece = pos_piece_on(pos, from);
    const int capture = pos_piece_on(pos, to);

    // Capture piece on to square (if any)
    if (capture != NB_PIECE) {
        assert(capture != KING);
        pos->rule50 = 0;

        // Use pos_color_on() instead of them, because we could be playing a KxR castling here
        clear_square(pos, pos_color_on(pos, to), capture, to);

        // Capturing a rook alters corresponding castling right
        pos->castleRooks &= ~(1ULL << to);
    }

    if (piece <= QUEEN) {
        // Move piece
        clear_square(pos, us, piece, from);
        set_square(pos, us, piece, to);

        // Lose specific castling right (if not already lost)
        pos->castleRooks &= ~(1ULL << from);
    } else {
        // Move piece
        clear_square(pos, us, piece, from);
        set_square(pos, us, piece, to);

        if (piece == PAWN) {
            // reset rule50, and set epSquare
            const int push = push_inc(us);
            pos->rule50 = 0;

            // Set ep square upon double push, only if catpturably by enemy pawns
            if (to == from + 2 * push
                    && (PawnAttacks[us][from + push] & pos_pieces_cp(pos, them, PAWN)))
                pos->epSquare = from + push;

            // handle ep-capture and promotion
            if (to == before->epSquare)
                clear_square(pos, them, piece, to - push);
            else if (rank_of(to) == RANK_8 || rank_of(to) == RANK_1) {
                clear_square(pos, us, piece, to);
                set_square(pos, us, prom, to);
            }
        } else if (piece == KING) {
            // Lose all castling rights
            pos->castleRooks &= ~Rank[us * RANK_8];

            // Castling
            if (bb_test(before->byColor[us], to)) {
                // Capturing our own piece can only be a castling move, encoded KxR
                assert(pos_piece_on(before, to) == ROOK);
                const int rank = rank_of(from);

                clear_square(pos, us, KING, to);
                set_square(pos, us, KING, square_from(rank, to > from ? FILE_G : FILE_C));
                set_square(pos, us, ROOK, square_from(rank, to > from ? FILE_F : FILE_D));
            }
        }
    }

    pos->turn = them;
    pos->key ^= ZobristTurn;
    pos->key ^= ZobristEnPassant[before->epSquare] ^ ZobristEnPassant[pos->epSquare];
    pos->key ^= zobrist_castling(before->castleRooks ^ pos->castleRooks);
    pos->fullMove += pos->turn == WHITE;
    pos->lastMove = m;

    finish(pos);
}

// All pieces
bitboard_t pos_pieces(const Position *pos)
{
    assert(!(pos->byColor[WHITE] & pos->byColor[BLACK]));
    return pos->byColor[WHITE] | pos->byColor[BLACK];
}

// Pieces of color 'color' and type 'piece'
bitboard_t pos_pieces_cp(const Position *pos, int color, int piece)
{
    BOUNDS(color, NB_COLOR);
    BOUNDS(piece, NB_PIECE);
    return pos->byColor[color] & pos->byPiece[piece];
}

// Pieces of color 'color' and type 'p1' or 'p2'
bitboard_t pos_pieces_cpp(const Position *pos, int color, int p1, int p2)
{
    BOUNDS(color, NB_COLOR);
    BOUNDS(p1, NB_PIECE);
    BOUNDS(p2, NB_PIECE);
    return pos->byColor[color] & (pos->byPiece[p1] | pos->byPiece[p2]);
}

// Detect insufficient material configuration (draw by chess rules only)
bool pos_insufficient_material(const Position *pos)
{
    return bb_count(pos_pieces(pos)) <= 3 && !pos->byPiece[PAWN] && !pos->byPiece[ROOK]
        && !pos->byPiece[QUEEN];
}

// Square occupied by the king of color 'color'
int pos_king_square(const Position *pos, int color)
{
    assert(bb_count(pos_pieces_cp(pos, color, KING)) == 1);
    return bb_lsb(pos_pieces_cp(pos, color, KING));
}

// Color of piece on square 'square'. Square is assumed to be occupied.
int pos_color_on(const Position *pos, int square)
{
    assert(bb_test(pos_pieces(pos), square));
    return bb_test(pos->byColor[WHITE], square) ? WHITE : BLACK;
}

// Piece on square 'square'. NB_PIECE if empty.
int pos_piece_on(const Position *pos, int square)
{
    BOUNDS(square, NB_SQUARE);

    int piece;

    for (piece = KNIGHT; piece <= PAWN; piece++)
        if (bb_test(pos->byPiece[piece], square))
            break;

    return piece;
}

bool pos_move_is_capture(const Position *pos, move_t m)
// Detect normal captures only (not en passant)
{
    return bb_test(pos->byColor[opposite(pos->turn)], move_to(m));
}

bool pos_move_is_castling(const Position *pos, move_t m)
{
    return bb_test(pos->byColor[pos->turn], move_to(m));
}

str_t pos_move_to_lan(const Position *pos, move_t m, bool chess960)
{
    str_t lan = str_new();
    const int from = move_from(m), prom = move_prom(m);
    int to = move_to(m);

    if (!(from | to | prom)) {
        str_cpy(&lan, "0000");
        return lan;
    }

    if (!chess960 && pos_move_is_castling(pos, m))
        to = to > from ? from + 2 : from - 2;  // e1h1 -> e1g1, e1a1 -> e1c1

    str_t fromStr = square_to_string(from);
    str_t toStr = square_to_string(to);
    str_cat(&lan, fromStr.buf, toStr.buf);
    str_free(&fromStr, &toStr);

    if (prom < NB_PIECE)
        str_putc(&lan, PieceLabel[BLACK][prom]);

    return lan;
}

move_t pos_string_to_move(const Position *pos, const char *str, bool chess960)
{
    const int prom = str[4] ? (int)(strchr(PieceLabel[BLACK], str[4]) - PieceLabel[BLACK]) : NB_PIECE;
    const int from = square_from(str[1] - '1', str[0] - 'a');
    int to = square_from(str[3] - '1', str[2] - 'a');

    if (!chess960 && pos_piece_on(pos, from) == KING) {
        if (to == from + 2)  // e1g1 -> e1h1
            to++;
        else if (to == from - 2)  // e1c1 -> e1a1
            to -= 2;
    }

    return move_build(from, to, prom);
}

str_t pos_move_to_san(const Position *pos, move_t m)
// Converts a move to Standard Algebraic Notation. Note that the '+' (check) or '#' (checkmate)
// suffixes are not generated here.
{
    str_t san = str_new();

    const int us = pos->turn;
    const int from = move_from(m), to = move_to(m), prom = move_prom(m);
    const int piece = pos_piece_on(pos, from);

    if (piece == PAWN) {
        str_putc(&san, file_of(from) + 'a');

        if (pos_move_is_capture(pos, m) || to == pos->epSquare)
            str_putc(&san, 'x', file_of(to) + 'a');

        str_putc(&san, rank_of(to) + '1');

        if (prom < NB_PIECE)
            str_putc(&san, '=', PieceLabel[WHITE][prom]);
    } else {
        str_putc(&san, PieceLabel[WHITE][piece]);

        // ** SAN disambiguation **

        // 1. Build a list of 'contesters', which are all our pieces of the same type that can also
        // reach the 'to' square.
        const bitboard_t pins = pos->pins;
        bitboard_t contesters = pos_pieces_cp(pos, us, piece);
        bb_clear(&contesters, from);

         if (piece == KING)
             // 1.0. Trivial case. Skip the swhole SAN disambiguation process.
             contesters = 0;
         if (piece == KNIGHT)
            // 1.1. Knights. Restrict to those within a knight jump of of 'to' that are not pinned.
            contesters &= KnightAttacks[to] & ~pins;
        else {
            // 1.2. Sliders

            // 1.2.1. Restrict to those that can pseudo-legally reach the 'to' square.
            bitboard_t occ = pos_pieces(pos);

            if (piece == BISHOP)
                contesters &= bb_bishop_attacks(to, occ);
            else if (piece == ROOK)
                contesters &= bb_rook_attacks(to, occ);
            else if (piece == QUEEN)
                contesters &= bb_bishop_attacks(to, occ) | bb_rook_attacks(to, occ);

            // 1.2.2. Remove pinned sliders, which, by sliding to the 'to' square, would escape
            // their pin-ray.
            bitboard_t pinnedContesters = contesters & pins;

            while (pinnedContesters) {
                const int pinnedContester = bb_pop_lsb(&pinnedContesters);

                if (!bb_test(Ray[pos_king_square(pos, us)][pinnedContester], to))
                    bb_clear(&contesters, pinnedContester);
            }
        }

        // 2. Use the contesters to disambiguate
        if (contesters) {
            // 2.1. Contested rank. Use file to disambiguate
            if (Rank[rank_of(from)] & contesters)
                str_putc(&san, file_of(from) + 'a');

            // 2.2. Contested file. Use rank to disambiguate
            if (File[file_of(from)] & contesters)
                str_putc(&san, rank_of(from) + '1');

            // Note that 2.1 and 2.2 are not mutually exclusive
        }

        if (pos_move_is_capture(pos, m))
            str_putc(&san, 'x');

        str_t toStr = square_to_string(to);
        str_cat(&san, toStr.buf);
        str_free(&toStr);
    }

    return san;
}

// Prints the position in ASCII 'art' (for debugging)
void pos_print(const Position *pos)
{
    for (int rank = RANK_8; rank >= RANK_1; rank--) {
        char line[] = ". . . . . . . .";

        for (int file = FILE_A; file <= FILE_H; file++) {
            const int square = square_from(rank, file);
            line[2 * file] = bb_test(pos_pieces(pos), square)
                ? PieceLabel[pos_color_on(pos, square)][pos_piece_on(pos, square)]
                : square == pos->epSquare ? '*' : '.';
        }

        puts(line);
    }

    str_t fen = pos_get(pos);
    puts(fen.buf);
    str_free(&fen);

    str_t lan = pos_move_to_lan(pos, pos->lastMove, true);
    printf("Last move: %s\n", lan.buf);
    str_free(&lan);
}

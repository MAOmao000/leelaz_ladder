/*
    This file is part of Leela Zero.
    Copyright (C) 2017-2019 Gian-Carlo Pascutto and contributors

    Leela Zero is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Leela Zero is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Leela Zero.  If not, see <http://www.gnu.org/licenses/>.

    Additional permission under GNU GPL version 3 section 7

    If you modify this Program, or any covered work, by linking or
    combining it with NVIDIA Corporation's libraries from the
    NVIDIA CUDA Toolkit and/or the NVIDIA CUDA Deep Neural
    Network library and/or the NVIDIA TensorRT inference library
    (or a modified version of those libraries), containing parts covered
    by the terms of the respective license agreement, the licensors of
    this Program grant you additional permission to convey the resulting
    work.
*/

#include <algorithm>
#include <array>
#include <cassert>
#include <iterator>
#include <memory>
#include <string>

#include "GameState.h"

#include "FastBoard.h"
#include "FastState.h"
#include "FullBoard.h"
#include "KoState.h"
#include "Network.h"
#include "UCTSearch.h"

void GameState::init_game(const int size, const float komi) {
    KoState::init_game(size, komi);

    m_game_history.clear();
    m_game_history.emplace_back(std::make_shared<KoState>(*this));

    m_timecontrol.reset_clocks();

    m_resigned = FastBoard::EMPTY;
}

void GameState::reset_game() {
    KoState::reset_game();

    m_game_history.clear();
    m_game_history.emplace_back(std::make_shared<KoState>(*this));

    m_timecontrol.reset_clocks();

    m_resigned = FastBoard::EMPTY;
}

bool GameState::forward_move() {
    if (m_game_history.size() > m_movenum + 1) {
        m_movenum++;
        *(static_cast<KoState*>(this)) = *m_game_history[m_movenum];
        return true;
    } else {
        return false;
    }
}

bool GameState::undo_move() {
    if (m_movenum > 0) {
        m_movenum--;

        // this is not so nice, but it should work
        *(static_cast<KoState*>(this)) = *m_game_history[m_movenum];

        // This also restores hashes as they're part of state
        return true;
    } else {
        return false;
    }
}

void GameState::rewind() {
    *(static_cast<KoState*>(this)) = *m_game_history[0];
    m_movenum = 0;
}

void GameState::play_move(const int vertex) {
    play_move(get_to_move(), vertex);
}

void GameState::play_move(const int color, const int vertex) {
    if (vertex == FastBoard::RESIGN) {
        m_resigned = color;
    } else {
        KoState::play_move(color, vertex);
    }

    // cut off any leftover moves from navigating
    m_game_history.resize(m_movenum);
    m_game_history.emplace_back(std::make_shared<KoState>(*this));
}

bool GameState::play_textmove(std::string color, const std::string& vertex) {
    int who;
    transform(cbegin(color), cend(color), begin(color), tolower);
    if (color == "w" || color == "white") {
        who = FullBoard::WHITE;
    } else if (color == "b" || color == "black") {
        who = FullBoard::BLACK;
    } else {
        return false;
    }

    const auto move = board.text_to_move(vertex);
    if (move == FastBoard::NO_VERTEX
        || (move != FastBoard::PASS && move != FastBoard::RESIGN
            && board.get_state(move) != FastBoard::EMPTY)) {
        return false;
    }

    set_to_move(who);
    play_move(move);

    return true;
}

void GameState::stop_clock(const int color) {
    m_timecontrol.stop(color);
}

void GameState::start_clock(const int color) {
    m_timecontrol.start(color);
}

void GameState::display_state() {
    FastState::display_state();

    m_timecontrol.display_times();
}

int GameState::who_resigned() const {
    return m_resigned;
}

bool GameState::has_resigned() const {
    return m_resigned != FastBoard::EMPTY;
}

const TimeControl& GameState::get_timecontrol() const {
    return m_timecontrol;
}

void GameState::set_timecontrol(const TimeControl& timecontrol) {
    m_timecontrol = timecontrol;
}

void GameState::set_timecontrol(const int maintime, const int byotime,
                                const int byostones, const int byoperiods) {
    TimeControl timecontrol(maintime, byotime, byostones, byoperiods);

    m_timecontrol = timecontrol;
}

void GameState::adjust_time(const int color, const int time, const int stones) {
    m_timecontrol.adjust_time(color, time, stones);
}

void GameState::anchor_game_history() {
    // handicap moves don't count in game history
    m_movenum = 0;
    m_game_history.clear();
    m_game_history.emplace_back(std::make_shared<KoState>(*this));
}

bool GameState::set_fixed_handicap(const int handicap) {
    if (!valid_handicap(handicap)) {
        return false;
    }

    int board_size = board.get_boardsize();
    int high = board_size >= 13 ? 3 : 2;
    int mid = board_size / 2;

    int low = board_size - 1 - high;
    if (handicap >= 2) {
        play_move(FastBoard::BLACK, board.get_vertex(low, low));
        play_move(FastBoard::BLACK, board.get_vertex(high, high));
    }

    if (handicap >= 3) {
        play_move(FastBoard::BLACK, board.get_vertex(high, low));
    }

    if (handicap >= 4) {
        play_move(FastBoard::BLACK, board.get_vertex(low, high));
    }

    if (handicap >= 5 && handicap % 2 == 1) {
        play_move(FastBoard::BLACK, board.get_vertex(mid, mid));
    }

    if (handicap >= 6) {
        play_move(FastBoard::BLACK, board.get_vertex(low, mid));
        play_move(FastBoard::BLACK, board.get_vertex(high, mid));
    }

    if (handicap >= 8) {
        play_move(FastBoard::BLACK, board.get_vertex(mid, low));
        play_move(FastBoard::BLACK, board.get_vertex(mid, high));
    }

    board.set_to_move(FastBoard::WHITE);

    anchor_game_history();

    set_handicap(handicap);

    return true;
}

int GameState::set_fixed_handicap_2(const int handicap) {
    int board_size = board.get_boardsize();
    int low = board_size >= 13 ? 3 : 2;
    int mid = board_size / 2;
    int high = board_size - 1 - low;

    int interval = (high - mid) / 2;
    int placed = 0;

    while (interval >= 3) {
        for (int i = low; i <= high; i += interval) {
            for (int j = low; j <= high; j += interval) {
                if (placed >= handicap) return placed;
                if (board.get_state(i - 1, j - 1) != FastBoard::EMPTY) continue;
                if (board.get_state(i - 1, j) != FastBoard::EMPTY) continue;
                if (board.get_state(i - 1, j + 1) != FastBoard::EMPTY) continue;
                if (board.get_state(i, j - 1) != FastBoard::EMPTY) continue;
                if (board.get_state(i, j) != FastBoard::EMPTY) continue;
                if (board.get_state(i, j + 1) != FastBoard::EMPTY) continue;
                if (board.get_state(i + 1, j - 1) != FastBoard::EMPTY) continue;
                if (board.get_state(i + 1, j) != FastBoard::EMPTY) continue;
                if (board.get_state(i + 1, j + 1) != FastBoard::EMPTY) continue;
                play_move(FastBoard::BLACK, board.get_vertex(i, j));
                placed++;
            }
        }
        interval = interval / 2;
    }

    return placed;
}

bool GameState::valid_handicap(const int handicap) {
    int board_size = board.get_boardsize();

    if (handicap < 2 || handicap > 9) {
        return false;
    }
    if (board_size % 2 == 0 && handicap > 4) {
        return false;
    }
    if (board_size == 7 && handicap > 4) {
        return false;
    }
    if (board_size < 7 && handicap > 0) {
        return false;
    }

    return true;
}

void GameState::place_free_handicap(int stones, Network& network) {
    int limit = board.get_boardsize() * board.get_boardsize();
    if (stones > limit / 2) {
        stones = limit / 2;
    }

    int orgstones = stones;

    int fixplace = std::min(9, stones);

    set_fixed_handicap(fixplace);
    stones -= fixplace;

    stones -= set_fixed_handicap_2(stones);

    for (int i = 0; i < stones; i++) {
        auto search = std::make_unique<UCTSearch>(*this, network);
        auto move = search->think(FastBoard::BLACK, UCTSearch::NOPASS);
        play_move(FastBoard::BLACK, move);
    }

    if (orgstones) {
        board.set_to_move(FastBoard::WHITE);
    } else {
        board.set_to_move(FastBoard::BLACK);
    }

    anchor_game_history();

    set_handicap(orgstones);
}

const FullBoard& GameState::get_past_board(const int moves_ago) const {
    assert(moves_ago >= 0 && (unsigned)moves_ago <= m_movenum);
    assert(m_movenum + 1 <= m_game_history.size());
    return m_game_history[m_movenum - moves_ago]->board;
}

const std::vector<std::shared_ptr<const KoState>>&
GameState::get_game_history() const {
    return m_game_history;
}

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

#include "config.h"
#include "UCTSearch.h"

#include <boost/format.hpp>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <type_traits>
#include <algorithm>

#include "FastBoard.h"
#include "FastState.h"
#include "FullBoard.h"
#include "GTP.h"
#include "GameState.h"
#include "TimeControl.h"
#include "Timing.h"
#include "Training.h"
#include "Utils.h"
#ifdef USE_OPENCL
#include "OpenCLScheduler.h"
#endif

using namespace Utils;

constexpr int UCTSearch::UNLIMITED_PLAYOUTS;

class OutputAnalysisData {
public:
    OutputAnalysisData(const std::string& move, int visits,
                       float winrate, float policy_prior, std::string pv,
                       float lcb, bool lcb_ratio_exceeded)
    : m_move(move), m_visits(visits), m_winrate(winrate),
      m_policy_prior(policy_prior), m_pv(pv), m_lcb(lcb),
      m_lcb_ratio_exceeded(lcb_ratio_exceeded) {};

    std::string get_info_string(int order) const {
        auto tmp = "info move " + m_move
                 + " visits " + std::to_string(m_visits)
                 + " winrate "
                 + std::to_string(static_cast<int>(m_winrate * 10000))
                 + " prior "
                 + std::to_string(static_cast<int>(m_policy_prior * 10000.0f))
                 + " lcb "
                 + std::to_string(static_cast<int>(std::max(0.0f, m_lcb) * 10000));
        if (order >= 0) {
            tmp += " order " + std::to_string(order);
        }
        tmp += " pv " + m_pv;
        return tmp;
    }

    friend bool operator<(const OutputAnalysisData& a,
                          const OutputAnalysisData& b) {
        if (a.m_lcb_ratio_exceeded && b.m_lcb_ratio_exceeded) {
            if (a.m_lcb != b.m_lcb) {
                return a.m_lcb < b.m_lcb;
            }
        }
        if (a.m_visits == b.m_visits) {
            return a.m_winrate < b.m_winrate;
        }
        return a.m_visits < b.m_visits;
    }

private:
    std::string m_move;
    int m_visits;
    float m_winrate;
    float m_policy_prior;
    std::string m_pv;
    float m_lcb;
    bool m_lcb_ratio_exceeded;
};


UCTSearch::UCTSearch(GameState& g, Network& network)
    : m_rootstate(g), m_network(network) {
    myprintf("Program Version %s%s\n", PROGRAM_VERSION, ".1");
    set_playout_limit(cfg_max_playouts);
    set_visit_limit(cfg_max_visits);

    m_root = std::make_unique<UCTNode>(FastBoard::PASS, 0.0f);
}

bool UCTSearch::advance_to_new_rootstate() {
    if (!m_root || !m_last_rootstate) {
        // No current state
        return false;
    }

    if (m_rootstate.get_komi() != m_last_rootstate->get_komi()) {
        return false;
    }

    auto depth =
        int(m_rootstate.get_movenum() - m_last_rootstate->get_movenum());

    if (depth < 0) {
        return false;
    }


    auto test = std::make_unique<GameState>(m_rootstate);
    for (auto i = 0; i < depth; i++) {
        test->undo_move();
    }

    if (m_last_rootstate->board.get_hash() != test->board.get_hash()) {
        // m_rootstate and m_last_rootstate don't match
        return false;
    }

    // Make sure that the nodes we destroyed the previous move are
    // in fact destroyed.
    while (!m_delete_futures.empty()) {
        m_delete_futures.front().wait_all();
        m_delete_futures.pop_front();
    }

    // Try to replay moves advancing m_root
    for (auto i = 0; i < depth; i++) {
        ThreadGroup tg(thread_pool);

        test->forward_move();
        const auto move = test->get_last_move();

        auto oldroot = std::move(m_root);
        m_root = oldroot->find_child(move);

        // Lazy tree destruction.  Instead of calling the destructor of the
        // old root node on the main thread, send the old root to a separate
        // thread and destroy it from the child thread.  This will save a
        // bit of time when dealing with large trees.
        auto p = oldroot.release();
        tg.add_task([p]() { delete p; });
        m_delete_futures.push_back(std::move(tg));

        if (!m_root) {
            // Tree hasn't been expanded this far
            return false;
        }
        m_last_rootstate->play_move(move);
    }

    assert(m_rootstate.get_movenum() == m_last_rootstate->get_movenum());

    if (m_last_rootstate->board.get_hash() != test->board.get_hash()) {
        // Can happen if user plays multiple moves in a row by same player
        return false;
    }

    return true;
}

void UCTSearch::update_root() {
    // Definition of m_playouts is playouts per search call.
    // So reset this count now.
    m_playouts = 0;

#ifndef NDEBUG
    auto start_nodes = m_root->count_nodes_and_clear_expand_state();
#endif

    if (!advance_to_new_rootstate() || !m_root) {
        m_root = std::make_unique<UCTNode>(FastBoard::PASS, 0.0f);
    }
    // Clear last_rootstate to prevent accidental use.
    m_last_rootstate.reset(nullptr);

    // Check how big our search tree (reused or new) is.
    m_nodes = m_root->count_nodes_and_clear_expand_state();

#ifndef NDEBUG
    if (m_nodes > 0) {
        myprintf("update_root, %d -> %d nodes (%.1f%% reused)\n",
            start_nodes, m_nodes.load(), 100.0 * m_nodes.load() / start_nodes);
    }
#endif
}

float UCTSearch::get_min_psa_ratio() const {
    const auto mem_full = UCTNodePointer::get_tree_size() / static_cast<float>(cfg_max_tree_size);
    // If we are halfway through our memory budget, start trimming
    // moves with very low policy priors.
    if (mem_full > 0.5f) {
        // Memory is almost exhausted, trim more aggressively.
        if (mem_full > 0.95f) {
            // if completely full just stop expansion by returning an impossible number
            if (mem_full >= 1.0f) {
                return 2.0f;
            }
            return 0.01f;
        }
        return 0.001f;
    }
    return 0.0f;
}

SearchResult UCTSearch::play_simulation(GameState & currstate,
                                        UCTNode* const node, int mycolor) {
//                                        UCTNode* const node) {
    const auto color = currstate.get_to_move();
    auto result = SearchResult{};

    node->virtual_loss();

    if (node->expandable()) {
        if (currstate.get_passes() >= 2) {
            auto score = currstate.final_score();
            result = SearchResult::from_score(score);
        } else {
            float eval;
            const auto had_children = node->has_children();
/*
            const auto success =
                node->create_children(m_network, m_nodes, currstate, eval,
                                      get_min_psa_ratio());
            if (!had_children && success) {
                result = SearchResult::from_eval(eval);
            }
*/
            if (cfg_ladder_check == 2) {
                const auto success =
                    node->create_children(m_network, m_nodes, currstate, eval,
                                          FastBoard::INVAL, get_min_psa_ratio());
                if (!had_children && success) {
                    result = SearchResult::from_eval(eval);
                }
            } else {
                const auto success =
                    node->create_children(m_network, m_nodes, currstate, eval,
                                          mycolor, get_min_psa_ratio());
                if (!had_children && success) {
                    result = SearchResult::from_eval(eval);
                }
            }
        }
    }

    if (node->has_children() && !result.valid()) {
        auto next = node->uct_select_child(color, node == m_root.get());
        auto move = next->get_move();

        currstate.play_move(move);
        if (move != FastBoard::PASS && currstate.superko()) {
            next->invalidate();
        } else {
//            result = play_simulation(currstate, next);
            result = play_simulation(currstate, next, mycolor);
        }
    }

    if (result.valid()) {
        node->update(result.eval());
    }
    node->virtual_loss_undo();

    return result;
}

void UCTSearch::dump_stats(FastState & state, UCTNode & parent) {
    if (cfg_quiet || !parent.has_children()) {
        return;
    }

    const int color = state.get_to_move();

    auto max_visits = 0;
    for (const auto& node : parent.get_children()) {
        max_visits = std::max(max_visits, node->get_visits());
    }

    // sort children, put best move on top
    if (cfg_vis_rate < 0) {
        parent.sort_children(color, cfg_lcb_min_visit_ratio * max_visits);
    } else {
        parent.sort_children(color);
    }

    if (parent.get_first_child()->first_visit()) {
        return;
    }

    int movecount = 0;
    for (const auto& node : parent.get_children()) {
        // Always display at least two moves. In the case there is
        // only one move searched the user could get an idea why.
        if (++movecount > 2 && !node->get_visits()) break;

        auto move = state.move_to_text(node->get_move());
//        auto tmpstate = FastState{state};
//        tmpstate.play_move(node->get_move());
//        auto pv = move + " " + get_pv(tmpstate, *node);
        auto pv = move + " " + get_pv(color, *node);

        myprintf("%4s -> %7d (V: %5.2f%%) (LCB: %5.2f%%) (N: %5.2f%%) PV: %s\n",
            move.c_str(),
            node->get_visits(),
            node->get_visits() ? node->get_raw_eval(color)*100.0f : 0.0f,
            std::max(0.0f, node->get_eval_lcb(color) * 100.0f),
            node->get_policy() * 100.0f,
            pv.c_str());
    }
/* skip log
    tree_stats(parent);
*/
}

void UCTSearch::output_analysis(FastState & state, UCTNode & parent) {
    // We need to make a copy of the data before sorting
    auto sortable_data = std::vector<OutputAnalysisData>();

    if (!parent.has_children()) {
        return;
    }

    const auto color = state.get_to_move();

    auto max_visits = 0;
    for (const auto& node : parent.get_children()) {
        max_visits = std::max(max_visits, node->get_visits());
    }

    for (const auto& node : parent.get_children()) {
        // Send only variations with visits, unless more moves were
        // requested explicitly.
        if (!node->get_visits()
            && sortable_data.size() >= cfg_analyze_tags.post_move_count()) {
            continue;
        }
        auto move = state.move_to_text(node->get_move());
//        auto tmpstate = FastState{state};
//        tmpstate.play_move(node->get_move());
//        auto rest_of_pv = get_pv(tmpstate, *node);
        auto rest_of_pv = get_pv(color, *node);
        auto pv = move + (rest_of_pv.empty() ? "" : " " + rest_of_pv);
        auto move_eval = node->get_visits() ? node->get_raw_eval(color) : 0.0f;
        auto policy = node->get_policy();
        auto lcb = node->get_eval_lcb(color);
        auto visits = node->get_visits();
        // Need at least 2 visits for valid LCB.
        auto lcb_ratio_exceeded = visits > 2 &&
            visits > max_visits * cfg_lcb_min_visit_ratio;
        // Store data in array
        sortable_data.emplace_back(move, visits,
                                   move_eval, policy, pv, lcb, lcb_ratio_exceeded);
    }
    // Sort array to decide order
    std::stable_sort(rbegin(sortable_data), rend(sortable_data));

    auto i = 0;
    // Output analysis data in gtp stream
    for (const auto& node : sortable_data) {
        if (i > 0) {
            gtp_printf_raw(" ");
        }
        gtp_printf_raw(node.get_info_string(i).c_str());
        i++;
    }
    gtp_printf_raw("\n");
}

void UCTSearch::tree_stats(const UCTNode& node) {
    size_t nodes = 0;
    size_t non_leaf_nodes = 0;
    size_t depth_sum = 0;
    size_t max_depth = 0;
    size_t children_count = 0;

    std::function<void(const UCTNode& node, size_t)> traverse =
          [&](const UCTNode& node, size_t depth) {
        nodes += 1;
        non_leaf_nodes += node.get_visits() > 1;
        depth_sum += depth;
        if (depth > max_depth) max_depth = depth;

        for (const auto& child : node.get_children()) {
            if (child.get_visits() > 0) {
                children_count += 1;
                traverse(*(child.get()), depth+1);
            } else {
                nodes += 1;
                depth_sum += depth+1;
                if (depth >= max_depth) max_depth = depth+1;
            }
        }
    };

    traverse(node, 0);

    if (nodes > 0) {
        myprintf("%.1f average depth, %d max depth\n",
                 (1.0f*depth_sum) / nodes, max_depth);
        myprintf("%d non leaf nodes, %.2f average children\n",
                 non_leaf_nodes, (1.0f*children_count) / non_leaf_nodes);
    }
}

bool UCTSearch::should_resign(passflag_t passflag, float besteval) {
    if (passflag & UCTSearch::NORESIGN) {
        // resign not allowed
        return false;
    }

    if (cfg_resignpct == 0) {
        // resign not allowed
        return false;
    }

    const size_t num_intersections = m_rootstate.board.get_boardsize()
                                   * m_rootstate.board.get_boardsize();
    const auto move_threshold = num_intersections / 4;
    const auto movenum = m_rootstate.get_movenum();
    if (movenum <= move_threshold) {
        // too early in game to resign
        return false;
    }

    const auto color = m_rootstate.board.get_to_move();

    const auto is_default_cfg_resign = cfg_resignpct < 0;
    const auto resign_threshold =
        0.01f * (is_default_cfg_resign ? 10 : cfg_resignpct);
    if (besteval > resign_threshold) {
        // eval > cfg_resign
        return false;
    }

    if ((m_rootstate.get_handicap() > 0)
            && (color == FastBoard::WHITE)
            && is_default_cfg_resign) {
        const auto handicap_resign_threshold =
            resign_threshold / (1 + m_rootstate.get_handicap());

        // Blend the thresholds for the first ~215 moves.
        auto blend_ratio = std::min(1.0f, movenum / (0.6f * num_intersections));
        auto blended_resign_threshold = blend_ratio * resign_threshold
            + (1 - blend_ratio) * handicap_resign_threshold;
        if (besteval > blended_resign_threshold) {
            // Allow lower eval for white in handicap games
            // where opp may fumble.
            return false;
        }
    }

    if (!m_rootstate.is_move_legal(color, FastBoard::RESIGN)) {
        return false;
    }

    return true;
}

int UCTSearch::get_best_move(passflag_t passflag) {
    int color = m_rootstate.board.get_to_move();

    auto max_visits = 0;
    for (const auto& node : m_root->get_children()) {
        max_visits = std::max(max_visits, node->get_visits());
    }

    // Make sure best is first
    if (cfg_vis_rate < 0) {
        m_root->sort_children(color, cfg_lcb_min_visit_ratio * max_visits);
    } else {
        m_root->sort_children(color);
    }

    // Check whether to randomize the best move proportional
    // to the playout counts, early game only.
    auto movenum = int(m_rootstate.get_movenum());
    if (movenum < cfg_random_cnt) {
        m_root->randomize_first_proportionally();
    }

    auto first_child = m_root->get_first_child();
    assert(first_child != nullptr);

    auto bestmove = first_child->get_move();
    auto besteval = first_child->first_visit() ? 0.5f : first_child->get_raw_eval(color);

    // do we want to fiddle with the best move because of the rule set?
    if (passflag & UCTSearch::NOPASS) {
        // were we going to pass?
        if (bestmove == FastBoard::PASS) {
            UCTNode * nopass = m_root->get_nopass_child(m_rootstate);

            if (nopass != nullptr) {
                myprintf("Preferring not to pass.\n");
                bestmove = nopass->get_move();
                if (nopass->first_visit()) {
                    besteval = 1.0f;
                } else {
                    besteval = nopass->get_raw_eval(color);
                }
            } else {
                myprintf("Pass is the only acceptable move.\n");
            }
        }
    } else if (!cfg_dumbpass) {
        const auto relative_score =
            (color == FastBoard::BLACK ? 1 : -1) * m_rootstate.final_score();
        if (bestmove == FastBoard::PASS) {
            // Either by forcing or coincidence passing is
            // on top...check whether passing loses instantly
            // do full count including dead stones.
            // In a reinforcement learning setup, it is possible for the
            // network to learn that, after passing in the tree, the two last
            // positions are identical, and this means the position is only won
            // if there are no dead stones in our own territory (because we use
            // Trump-Taylor scoring there). So strictly speaking, the next
            // heuristic isn't required for a pure RL network, and we have
            // a commandline option to disable the behavior during learning.
            // On the other hand, with a supervised learning setup, we fully
            // expect that the engine will pass out anything that looks like
            // a finished game even with dead stones on the board (because the
            // training games were using scoring with dead stone removal).
            // So in order to play games with a SL network, we need this
            // heuristic so the engine can "clean up" the board. It will still
            // only clean up the bare necessity to win. For full dead stone
            // removal, kgs-genmove_cleanup and the NOPASS mode must be used.

            // Do we lose by passing?
            if (relative_score < 0.0f) {
                myprintf("Passing loses :-(\n");
                // Find a valid non-pass move.
                UCTNode * nopass = m_root->get_nopass_child(m_rootstate);
                if (nopass != nullptr) {
                    myprintf("Avoiding pass because it loses.\n");
                    bestmove = nopass->get_move();
                    if (nopass->first_visit()) {
                        besteval = 1.0f;
                    } else {
                        besteval = nopass->get_raw_eval(color);
                    }
                } else {
                    myprintf("No alternative to passing.\n");
                }
            } else if (relative_score > 0.0f) {
                myprintf("Passing wins :-)\n");
            } else {
                myprintf("Passing draws :-|\n");
                // Find a valid non-pass move.
                const auto nopass = m_root->get_nopass_child(m_rootstate);
                if (nopass != nullptr && !nopass->first_visit()) {
                    const auto nopass_eval = nopass->get_raw_eval(color);
                    if (nopass_eval > 0.5f) {
                        myprintf("Avoiding pass because there could be a winning alternative.\n");
                        bestmove = nopass->get_move();
                        besteval = nopass_eval;
                    }
                }
                if (bestmove == FastBoard::PASS) {
                    myprintf("No seemingly better alternative to passing.\n");
                }
            }
        } else if (m_rootstate.get_last_move() == FastBoard::PASS) {
            // Opponents last move was passing.
            // We didn't consider passing. Should we have and
            // end the game immediately?

            if (!m_rootstate.is_move_legal(color, FastBoard::PASS)) {
                myprintf("Passing is forbidden, I'll play on.\n");
            // do we lose by passing?
            } else if (relative_score < 0.0f) {
                myprintf("Passing loses, I'll play on.\n");
            } else if (relative_score > 0.0f) {
                myprintf("Passing wins, I'll pass out.\n");
                bestmove = FastBoard::PASS;
            } else {
                myprintf("Passing draws, make it depend on evaluation.\n");
                if (besteval < 0.5f) {
                    bestmove = FastBoard::PASS;
                }
            }
        }
    }

    // if we aren't passing, should we consider resigning?
    if (bestmove != FastBoard::PASS) {
        if (should_resign(passflag, besteval)) {
            myprintf("Eval (%.2f%%) looks bad. Resigning.\n",
                     100.0f * besteval);
            bestmove = FastBoard::RESIGN;
        }
    }

    return bestmove;
}

/*
std::string UCTSearch::get_pv(FastState & state, UCTNode& parent) {
    if (!parent.has_children()) {
        return std::string();
    }

    if (parent.expandable()) {
        // Not fully expanded. This means someone could expand
        // the node while we want to traverse the children.
        // Avoid the race conditions and don't go through the rabbit hole
        // of trying to print things from this node.
        return std::string();
    }

    auto& best_child = parent.get_best_root_child(state.get_to_move());
    if (best_child.first_visit()) {
        return std::string();
    }
    auto best_move = best_child.get_move();
    auto res = state.move_to_text(best_move);

    state.play_move(best_move);

    auto next = get_pv(state, best_child);
    if (!next.empty()) {
        res.append(" ").append(next);
    }
    return res;
}
*/

std::string UCTSearch::get_pv(int color, UCTNode& parent) {
    UCTNode* temp = &parent;
    std::string res = std::string();
    while (true) {
        if (!temp->has_children() || temp->expandable()) {
            return res;
        }
        auto& best_child = temp->get_best_root_child(color);
        if (best_child.first_visit()) {
            return res;
        }
        auto best_move = best_child.get_move();
        if (res.size() > 0) {
            res.append(" ").append(m_rootstate.move_to_text(best_move));
        } else {
            res.append(m_rootstate.move_to_text(best_move));
        }
        temp = &best_child;
    }
}

std::string UCTSearch::get_analysis(int playouts) {
    FastState tempstate = m_rootstate;
    int color = tempstate.board.get_to_move();

//    auto pvstring = get_pv(tempstate, *m_root);
    auto pvstring = get_pv(color, *m_root);
    float winrate = 100.0f * m_root->get_raw_eval(color);
    return str(boost::format("Playouts: %d, Win: %5.2f%%, PV: %s")
        % playouts % winrate % pvstring.c_str());
}

bool UCTSearch::is_running() const {
    return m_run && UCTNodePointer::get_tree_size() < cfg_max_tree_size;
}

int UCTSearch::est_playouts_left(int elapsed_centis, int time_for_move) const {
    auto playouts = m_playouts.load();
    const auto playouts_left =
        std::max(0, std::min(m_maxplayouts - playouts,
                             m_maxvisits - m_root->get_visits()));

    // Wait for at least 1 second and 100 playouts
    // so we get a reliable playout_rate.
    if (elapsed_centis < 100 || playouts < 100) {
        return playouts_left;
    }
    const auto playout_rate = 1.0f * playouts / elapsed_centis;
    const auto time_left = std::max(0, time_for_move - elapsed_centis);
    return std::min(playouts_left,
                    static_cast<int>(std::ceil(playout_rate * time_left)));
}

size_t UCTSearch::prune_noncontenders(int color, int elapsed_centis, int time_for_move, bool prune) {
    auto lcb_max = 0.0f;
    auto Nfirst = 0;
    // There are no cases where the root's children vector gets modified
    // during a multithreaded search, so it is safe to walk it here without
    // taking the (root) node lock.
    for (const auto& node : m_root->get_children()) {
        if (node->valid()) {
            const auto visits = node->get_visits();
            if (visits > 0) {
                lcb_max = std::max(lcb_max, node->get_eval_lcb(color));
            }
            Nfirst = std::max(Nfirst, visits);
        }
    }
    const auto min_required_visits =
        Nfirst - est_playouts_left(elapsed_centis, time_for_move);
    auto pruned_nodes = size_t{0};
    for (const auto& node : m_root->get_children()) {
        if (node->valid()) {
            const auto visits = node->get_visits();
            const auto has_enough_visits =
                visits >= min_required_visits;
            // Avoid pruning moves that could have the best lower confidence
            // bound.
            const auto high_winrate = visits > 0 ?
                node->get_raw_eval(color) >= lcb_max : false;
            const auto prune_this_node = !(has_enough_visits || high_winrate);

            if (prune) {
                node->set_active(!prune_this_node);
            }
            if (prune_this_node) {
                ++pruned_nodes;
            }
        }
    }

    assert(pruned_nodes < m_root->get_children().size());
    return pruned_nodes;
}

bool UCTSearch::have_alternate_moves(int elapsed_centis, int time_for_move) {
    if (cfg_timemanage == TimeManagement::OFF) {
        return true;
    }
    auto my_color = m_rootstate.get_to_move();
    // For self play use. Disables pruning of non-contenders to not bias the training data.
    auto prune = cfg_timemanage != TimeManagement::NO_PRUNING;
    auto pruned = prune_noncontenders(my_color, elapsed_centis, time_for_move, prune);
    if (pruned < m_root->get_children().size() - 1) {
        return true;
    }
    // If we cannot save up time anyway, use all of it. This
    // behavior can be overruled by setting "fast" time management,
    // which will cause Leela to quickly respond to obvious/forced moves.
    // That comes at the cost of some playing strength as she now cannot
    // think ahead about her next moves in the remaining time.
    auto tc = m_rootstate.get_timecontrol();
    if (!tc.can_accumulate_time(my_color)
        || m_maxplayouts < UCTSearch::UNLIMITED_PLAYOUTS) {
        if (cfg_timemanage != TimeManagement::FAST) {
            return true;
        }
    }
    // In a timed search we will essentially always exit because
    // the remaining time is too short to let another move win, so
    // avoid spamming this message every move. We'll print it if we
    // save at least half a second.
    if (time_for_move - elapsed_centis > 50) {
        myprintf("%.1fs left, stopping early.\n",
                    (time_for_move - elapsed_centis) / 100.0f);
    }
    return false;
}

bool UCTSearch::stop_thinking(int elapsed_centis, int time_for_move) const {
    return m_playouts >= m_maxplayouts
           || m_root->get_visits() >= m_maxvisits
           || elapsed_centis >= time_for_move;
}

void UCTWorker::operator()() {
    int color = m_rootstate.board.get_to_move();
    do {
        auto currstate = std::make_unique<GameState>(m_rootstate);
//        auto result = m_search->play_simulation(*currstate, m_root);
        auto result = m_search->play_simulation(*currstate, m_root, color);
        if (result.valid()) {
            m_search->increment_playouts();
        }
    } while (m_search->is_running());
}

void UCTSearch::increment_playouts() {
    m_playouts++;
}

int UCTSearch::think(int color, passflag_t passflag) {
    // Start counting time for us
    m_rootstate.start_clock(color);

    // set up timing info
    Time start;

    update_root();
    // set side to move
    m_rootstate.board.set_to_move(color);

    auto time_for_move =
        m_rootstate.get_timecontrol().max_time_for_move(
            m_rootstate.board.get_boardsize(),
            color, m_rootstate.get_movenum());

/* skip log
    myprintf("Thinking at most %.1f seconds...\n", time_for_move/100.0f);
*/

    // create a sorted list of legal moves (make sure we
    // play something legal and decent even in time trouble)
    m_root->prepare_root_node(m_network, color, m_nodes, m_rootstate);

    m_run = true;
    int cpus = cfg_num_threads;
    ThreadGroup tg(thread_pool);
    for (int i = 1; i < cpus; i++) {
        tg.add_task(UCTWorker(m_rootstate, this, m_root.get()));
    }

    auto keeprunning = true;
/* skip log
    auto last_update = 0;
*/
    auto last_output = 0;
/*
    do {
        auto currstate = std::make_unique<GameState>(m_rootstate);

        auto result = play_simulation(*currstate, m_root.get());
        if (result.valid()) {
            increment_playouts();
        }

        Time elapsed;
        int elapsed_centis = Time::timediff_centis(start, elapsed);

        if (cfg_analyze_tags.interval_centis() &&
            elapsed_centis - last_output > cfg_analyze_tags.interval_centis()) {
            last_output = elapsed_centis;
            output_analysis(m_rootstate, *m_root);
        }

        // output some stats every few seconds
        // check if we should still search
        if (!cfg_quiet && elapsed_centis - last_update > 250) {
            last_update = elapsed_centis;
            myprintf("%s\n", get_analysis(m_playouts.load()).c_str());
        }
        keeprunning  = is_running();
        keeprunning &= !stop_thinking(elapsed_centis, time_for_move);
        keeprunning &= have_alternate_moves(elapsed_centis, time_for_move);
    } while (keeprunning);
    // Make sure to post at least once.
    if (cfg_analyze_tags.interval_centis() && last_output == 0) {
        output_analysis(m_rootstate, *m_root);
    }

    // Stop the search.
    m_run = false;
    tg.wait_all();

    // Reactivate all pruned root children.
    for (const auto& node : m_root->get_children()) {
        node->set_active(true);
    }

    m_rootstate.stop_clock(color);
    if (!m_root->has_children()) {
        return FastBoard::PASS;
    }

    // Display search info.
    myprintf("\n");
    dump_stats(m_rootstate, *m_root);
    Training::record(m_network, m_rootstate, *m_root);

    Time elapsed;
    int elapsed_centis = Time::timediff_centis(start, elapsed);
    myprintf("%d visits, %d nodes, %d playouts, %.0f n/s\n\n",
             m_root->get_visits(),
             m_nodes.load(),
             m_playouts.load(),
             (m_playouts * 100.0) / (elapsed_centis+1));

#ifdef USE_OPENCL
#ifndef NDEBUG
    myprintf("batch stats: %d %d\n",
        batch_stats.single_evals.load(),
        batch_stats.batch_evals.load()
    );
#endif
#endif

    int bestmove = get_best_move(passflag);

    // Save the explanation.
    m_think_output =
        str(boost::format("move %d, %c => %s\n%s")
        % m_rootstate.get_movenum()
        % (color == FastBoard::BLACK ? 'B' : 'W')
        % m_rootstate.move_to_text(bestmove).c_str()
        % get_analysis(m_root->get_visits()).c_str());

    // Copy the root state. Use to check for tree re-use in future calls.
    m_last_rootstate = std::make_unique<GameState>(m_rootstate);
    return bestmove;
*/
    if (cfg_playouts_rate <= 0) {
        /* single playout */
        do {
            auto currstate = std::make_unique<GameState>(m_rootstate);
            auto result = play_simulation(*currstate, m_root.get(), color);
            if (result.valid()) {
                increment_playouts();
            }
            Time elapsed;
            int elapsed_centis = Time::timediff_centis(start, elapsed);
            if (cfg_analyze_tags.interval_centis() &&
                elapsed_centis - last_output > cfg_analyze_tags.interval_centis()) {
                last_output = elapsed_centis;
                output_analysis(m_rootstate, *m_root);
            }
            keeprunning  = is_running();
            keeprunning &= !stop_thinking(elapsed_centis, time_for_move);
            keeprunning &= have_alternate_moves(elapsed_centis, time_for_move);
        } while (keeprunning);
        // stop the search
        m_run = false;
        tg.wait_all();
        /* additional playout */
        if (cfg_additional_playouts > 0 &&
            m_root->get_children().size() > 1 &&
            m_root->get_children()[1]->get_visits() > 0 &&
            (m_rootstate.m_komove != FastBoard::NO_VERTEX ||
             m_root->get_children()[0]->get_raw_eval(color) < m_last_win - cfg_win_divergence)) {
            int playouts = m_playouts;
            m_run = true;
            ThreadGroup tgn(thread_pool);
            for (int i = 1; i < cpus; i++) {
                tgn.add_task(UCTWorker(m_rootstate, this, m_root.get()));
            }
            keeprunning = true;
            do {
                auto currstate = std::make_unique<GameState>(m_rootstate);
                auto result = play_simulation(*currstate, m_root.get(), color);
                if (result.valid()) {
                    increment_playouts();
                }
                Time elapsed;
                int elapsed_centis = Time::timediff_centis(start, elapsed);
                if (cfg_analyze_tags.interval_centis() &&
                    elapsed_centis - last_output > cfg_analyze_tags.interval_centis()) {
                    last_output = elapsed_centis;
                    output_analysis(m_rootstate, *m_root);
                }
                keeprunning  = is_running();
                keeprunning &= (m_playouts < playouts + cfg_additional_playouts);
            } while (keeprunning);
            m_run = false;
            tgn.wait_all();
        }
        // reactivate all pruned root children
        for (const auto& node : m_root->get_children()) {
            node->set_active(true);
        }
        m_rootstate.stop_clock(color);
        if (!m_root->has_children()) {
            return FastBoard::PASS;
        }
        // display search info
        myprintf("\n");
        dump_stats(m_rootstate, *m_root);
        Training::record(m_network, m_rootstate, *m_root);
        Time elapsed1;
        int elapsed_centis = Time::timediff_centis(start, elapsed1);
        if (elapsed_centis+1 > 0) {
            myprintf("%d visits, %d nodes, %d playouts, %.2f n/s\n",
                     m_root->get_visits(),
                     static_cast<int>(m_nodes),
                     static_cast<int>(m_playouts),
                     (m_playouts * 100.0) / (elapsed_centis+1));
        }
        int bestmove = get_best_move(passflag);
        int savemove = bestmove;
        int visrate = 0;
        int min_vis_rate = 0;
        bool first = true;
        float bestwin = 0.0f;
        for (const auto& node : m_root->get_children()) {
            if (!node->get_visits()) {
                break;
            }
            if (first) {
                first = false;
                visrate = node->get_visits();
                bestwin = node->get_raw_eval(color);

                if (cfg_vis_rate == 0) {
                    if (m_last_win - bestwin >= 0.08f) {
                        min_vis_rate = visrate * 25 / 100;
                    } else if (m_last_win - bestwin >= 0.04f) {
                        min_vis_rate = visrate * 30 / 100;
                    } else if (m_last_win - bestwin >= 0.02f) {
                        min_vis_rate = visrate * 35 / 100;
                    } else if (m_last_win - bestwin >= 0.01f) {
                        min_vis_rate = visrate * 40 / 100;
                    } else if (m_last_win - bestwin > 0.0f) {
                        min_vis_rate = visrate * 45 / 100;
                    } else {
                        min_vis_rate = visrate * 50 / 100;
                    }
                } else if (cfg_vis_rate < 0){
                    break;
                } else {
                    if (node->get_policy() * 100 > cfg_select_policy) {
                        break;
                    }
                    min_vis_rate = m_root->get_visits() * cfg_vis_rate / 100;
                }
                if (min_vis_rate < 3) {
                    min_vis_rate = 3;
                }
            }
            if (node->get_visits() < min_vis_rate) {
                continue;
            }
            if (node->get_raw_eval(color) > bestwin) {
                bestwin = node->get_raw_eval(color);
                bestmove = node->get_move();
            }
        }
        if (bestmove != savemove) {
            if (should_resign(passflag, bestwin)) {
                myprintf("Eval (%.2f%%) looks bad. Resigning.\n", 100.0f * bestwin);
                bestmove = FastBoard::RESIGN;
            }
        }
        myprintf("Best move: %s(%d), Win: %.2f%%\n",
                 (m_rootstate.move_to_text(bestmove)).c_str(),
                 m_rootstate.get_movenum()+1,
                 bestwin*100.0f);
        // Save the explanation.
        m_think_output =
            str(boost::format("move %d, %c => %s\n%s")
            % m_rootstate.get_movenum()
            % (color == FastBoard::BLACK ? 'B' : 'W')
            % m_rootstate.move_to_text(bestmove).c_str()
            % get_analysis(m_root->get_visits()).c_str());

        // Copy the root state. Use to check for tree re-use in future calls.
        m_last_rootstate = std::make_unique<GameState>(m_rootstate);
        m_last_win = bestwin;
        return bestmove;
    }
    /* 1'st playout */
    do {
        auto currstate = std::make_unique<GameState>(m_rootstate);
        auto result = play_simulation(*currstate, m_root.get(), color);
        if (result.valid()) {
            increment_playouts();
        }

        Time elapsed;
        int elapsed_centis = Time::timediff_centis(start, elapsed);

        if (cfg_analyze_tags.interval_centis() &&
            elapsed_centis - last_output > cfg_analyze_tags.interval_centis()) {
            last_output = elapsed_centis;
            output_analysis(m_rootstate, *m_root);
        }

        // output some stats every few seconds
        // check if we should still search
/* skip log
        if (elapsed_centis - last_update > 250) {
            last_update = elapsed_centis;
            dump_analysis(static_cast<int>(m_playouts));
        }
*/
        keeprunning  = is_running();
        keeprunning &= !(m_playouts >= static_cast<int>(m_maxplayouts * (cfg_playouts_rate / 100.0f))
                      || m_root->get_visits() >= static_cast<int>(m_maxvisits * (cfg_playouts_rate / 100.0f))
                      || elapsed_centis >= static_cast<int>(time_for_move * (cfg_playouts_rate / 100.0f)));
    } while (keeprunning);

    // stop the search
    m_run = false;
    tg.wait_all();

    /* 2'nd playout */
    std::vector<int> vis1(FastBoard::NUM_VERTICES+1, 0);
    std::vector<float> win1(FastBoard::NUM_VERTICES+1, 0.0f);
    for (const auto& node : m_root->get_children()) {
        if (!node->get_visits()) {
            break;
        }
        vis1[node->get_move()+1] = node->get_visits();
        win1[node->get_move()+1] = node->get_raw_eval(color);
    }
    m_run = true;
    ThreadGroup tgn(thread_pool);
    for (int i = 1; i < cpus; i++) {
        tgn.add_task(UCTWorker(m_rootstate, this, m_root.get()));
    }
    keeprunning = true;
    Time elapsed;
/* skip log
    last_update = Time::timediff_centis(start, elapsed);
*/
    last_output = Time::timediff_centis(start, elapsed);
    do {
        auto currstate = std::make_unique<GameState>(m_rootstate);
        auto result = play_simulation(*currstate, m_root.get(), color);
        if (result.valid()) {
            increment_playouts();
        }
        Time elapsed;
        int elapsed_centis = Time::timediff_centis(start, elapsed);
        if (cfg_analyze_tags.interval_centis() &&
            elapsed_centis - last_output > cfg_analyze_tags.interval_centis()) {
            last_output = elapsed_centis;
            output_analysis(m_rootstate, *m_root);
        }
/* skip log
        if (elapsed_centis - last_update > 250) {
            last_update = elapsed_centis;
            dump_analysis(static_cast<int>(m_playouts));
        }
*/
        keeprunning  = is_running();
        keeprunning &= !stop_thinking(elapsed_centis, time_for_move);
    } while (keeprunning);
    m_run = false;
    tgn.wait_all();

   // reactivate all pruned root children
   for (const auto& node : m_root->get_children()) {
       node->set_active(true);
   }

   m_rootstate.stop_clock(color);
   if (!m_root->has_children()) {
       return FastBoard::PASS;
   }

    // display search info
/* skip log
    myprintf("\n");
    dump_stats(m_rootstate, *m_root);
*/
    Training::record(m_network, m_rootstate, *m_root);

/* skip log*/
    Time elapsed2;
    int elapsed_centis = Time::timediff_centis(start, elapsed2);
    if (elapsed_centis+1 > 0) {
        myprintf("%d visits, %d nodes, %d playouts, %.2f n/s\n",
                 m_root->get_visits(),
                 static_cast<int>(m_nodes),
                 static_cast<int>(m_playouts),
                 (m_playouts * 100.0) / (elapsed_centis+1));
    }
/**/
    int savevis1 = 0;
    int savevis2 = 0;
    int bestmove = -1;
    std::vector<int> vis2(FastBoard::NUM_VERTICES+1, 0);
    std::vector<float> win2(FastBoard::NUM_VERTICES+1, 0.0f);
    auto max_visits = 0;
    for (const auto& node : m_root->get_children()) {
        max_visits = std::max(max_visits, node->get_visits());
    }
    if (cfg_vis_rate < 0) {
        m_root->sort_children(color,  cfg_lcb_min_visit_ratio * max_visits);
    } else {
        m_root->sort_children(color);
    }
    int bestvis = 0;
    float bestwin = 0.0f;
    float savewin = 0.0f;
    for (const auto& node : m_root->get_children()) {
        if (!node->get_visits()) {
            break;
        }
        vis2[node->get_move()+1] = node->get_visits() - vis1[node->get_move()+1];
        win2[node->get_move()+1] = node->get_raw_eval(color);
        if (vis1[node->get_move()+1] <= 0) {
            win1[node->get_move()+1] = win2[node->get_move()+1];
        }
        myprintf("%4s: %d/%d, V: %.2f%%(%.2f%%), N: %.2f%%\n",
                 (m_rootstate.move_to_text(node->get_move()).c_str()),
                 vis2[node->get_move()+1],
                 node->get_visits(),
                 win2[node->get_move()+1]*100.0f,
                 win2[node->get_move()+1]*100.0f - win1[node->get_move()+1]*100.0f,
                 node->get_policy() * 100.0f);
        if (bestvis <= 0) {
            if (vis2[node->get_move()+1] <= 0) {
                continue;
            }
            bestmove = node->get_move();
            bestvis = vis2[node->get_move()+1];
            savevis1 = node->get_visits();
            savevis2 = bestvis;
            bestwin = node->get_raw_eval(color);
            savewin = 2 * node->get_raw_eval(color) - win1[node->get_move()+1];
        } else if (vis2[node->get_move()+1] < 3) {
            continue;
        } else if (vis2[node->get_move()+1] > bestvis &&
                   node->get_policy() * 100 >= cfg_select_policy) {
            bestmove = node->get_move();
            bestwin = node->get_raw_eval(color);
            savewin = 2 * node->get_raw_eval(color) - win1[node->get_move()+1];
            bestvis = vis2[node->get_move()+1];
        } else if (node->get_visits() >= savevis1 * cfg_select_visits / 100 &&
                   vis2[node->get_move()+1] >= savevis2 * cfg_select_visits / 100 &&
                   2 * node->get_raw_eval(color) - win1[node->get_move()+1] > savewin) {
            bestmove = node->get_move();
            bestwin = node->get_raw_eval(color);
            savewin = 2 * node->get_raw_eval(color) - win1[node->get_move()+1];
        }
    }
    if (should_resign(passflag, bestwin)) {
        myprintf("Eval (%.2f%%) looks bad. Resigning.\n", 100.0f * bestwin);
        bestmove = FastBoard::RESIGN;
    }
    // Save the explanation.
    m_think_output =
        str(boost::format("move %d, %c => %s\n%s")
        % m_rootstate.get_movenum()
        % (color == FastBoard::BLACK ? 'B' : 'W')
        % m_rootstate.move_to_text(bestmove).c_str()
        % get_analysis(m_root->get_visits()).c_str());

    // Copy the root state. Use to check for tree re-use in future calls.
    m_last_rootstate = std::make_unique<GameState>(m_rootstate);
    m_last_win = bestwin;
/* Add debug print */
    myprintf("Best move: %s(%d), Win: %.2f%%\n",
             (m_rootstate.move_to_text(bestmove)).c_str(),
             m_rootstate.get_movenum()+1,
             bestwin*100.0f);

    return bestmove;
}

// Brief output from last think() call.
std::string UCTSearch::explain_last_think() const {
    return m_think_output;
}

//void UCTSearch::ponder() {
void UCTSearch::ponder(int color) {
    auto disable_reuse = cfg_analyze_tags.has_move_restrictions();
    if (disable_reuse) {
        m_last_rootstate.reset(nullptr);
    }

    update_root();

    m_root->prepare_root_node(m_network, m_rootstate.board.get_to_move(),
                              m_nodes, m_rootstate);

    m_run = true;
    ThreadGroup tg(thread_pool);
    for (auto i = size_t{1}; i < cfg_num_threads; i++) {
        tg.add_task(UCTWorker(m_rootstate, this, m_root.get()));
    }
    Time start;
    auto keeprunning = true;
    auto last_output = 0;
    do {
        auto currstate = std::make_unique<GameState>(m_rootstate);
//        auto result = play_simulation(*currstate, m_root.get());
        auto result = play_simulation(*currstate, m_root.get(), color);
        if (result.valid()) {
            increment_playouts();
        }
        if (cfg_analyze_tags.interval_centis()) {
            Time elapsed;
            int elapsed_centis = Time::timediff_centis(start, elapsed);
            if (elapsed_centis - last_output > cfg_analyze_tags.interval_centis()) {
                last_output = elapsed_centis;
                output_analysis(m_rootstate, *m_root);
            }
        }
        keeprunning  = is_running();
        keeprunning &= !stop_thinking(0, 1);
    } while (!Utils::input_pending() && keeprunning);

    // Make sure to post at least once.
    if (cfg_analyze_tags.interval_centis() && last_output == 0) {
        output_analysis(m_rootstate, *m_root);
    }

    // Stop the search.
    m_run = false;
    tg.wait_all();

    // Display search info.
/* skip log
    myprintf("\n");
    dump_stats(m_rootstate, *m_root);

    myprintf("\n%d visits, %d nodes\n\n", m_root->get_visits(), m_nodes.load());
*/

    // Copy the root state. Use to check for tree re-use in future calls.
    if (!disable_reuse) {
        m_last_rootstate = std::make_unique<GameState>(m_rootstate);
    }
}

void UCTSearch::set_playout_limit(int playouts) {
    static_assert(std::is_convertible<decltype(playouts),
                                      decltype(m_maxplayouts)>::value,
                  "Inconsistent types for playout amount.");
    m_maxplayouts = std::min(playouts, UNLIMITED_PLAYOUTS);
}

void UCTSearch::set_visit_limit(int visits) {
    static_assert(std::is_convertible<decltype(visits),
                                      decltype(m_maxvisits)>::value,
                  "Inconsistent types for visits amount.");
    // Limit to type max / 2 to prevent overflow when multithreading.
    m_maxvisits = std::min(visits, UNLIMITED_PLAYOUTS);
}


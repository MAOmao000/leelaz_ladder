#include <iostream>
#include <memory>

#include "Ladder.h"
#include "SearchBoard.h"
#include "src/GTP.h"

using namespace std;

#define ALIVE  1
#define DEAD   0
#define LADDER 1

//
static bool IsLadderCaptured(int &depth, search_game_info_t *game, const int ren_xy, const int turn_color);

////////////////////////////////
//                            //
////////////////////////////////
void LadderExtension(game_info_t *game, int color, char *ladder_pos)
{
    const string_t *string = game->string;
    std::unique_ptr<search_game_info_t> search_game = nullptr;
    char checked[BOARD_MAX] = {};

    if (game->ko_move == (game->moves - 1)) return;

    for (int i = 0; i < MAX_STRING; i++) {
        if (!string[i].flag ||
            string[i].color != color) {
            continue;
        }
        int ladder = string[i].lib[0];

        char flag = DEAD;

        if (!checked[ladder] && string[i].libs == 1) {
            if (!search_game)
                search_game.reset(new search_game_info_t(game));
            search_game_info_t *ladder_game = search_game.get();
            int neighbor = string[i].neighbor[0];
            while (neighbor != NEIGHBOR_END && flag == DEAD) {
                if (string[neighbor].libs == 1) {
                    if (IsLegal(game, string[neighbor].lib[0], color)) {
                        PutStoneForSearch(ladder_game, string[neighbor].lib[0], color);
                        int depth = 0;
                        if (IsLadderCaptured(depth, ladder_game, string[i].origin, FLIP_COLOR(color)) ||
                            depth < cfg_ladder_defense) {
                            flag = ALIVE;
                            Undo(ladder_game);
                            break;
                        }
                        Undo(ladder_game);
                    }
                }
                neighbor = string[i].neighbor[neighbor];
            }
            if (flag == DEAD) {
                if (IsLegal(game, ladder, color)) {
                    PutStoneForSearch(ladder_game, ladder, color);
                    int depth = 0;
                    if (IsLadderCaptured(depth, ladder_game, ladder, FLIP_COLOR(color)) == DEAD) {
                        if (depth >= cfg_ladder_defense) {
                            ladder_pos[ladder] = LADDER;
                        }
                    }
                    Undo(ladder_game);
                }
            }
            checked[ladder] = 1;
        }
    }
    if (cfg_ladder_attack == 0) {
        return;
    }
    int neighbor4[4];
    for (int i = RAY_BOARD_SIZE * OB_SIZE + OB_SIZE;
             i < BOARD_MAX - (RAY_BOARD_SIZE * OB_SIZE + OB_SIZE); i++) {
        if (ladder_pos[i] == LADDER || !IsLegal(game, i, color)) {
            continue;
        }
        if (!search_game)
            search_game.reset(new search_game_info_t(game));
        search_game_info_t *ladder_game = search_game.get();
        const char *board = ladder_game->board;
        const string_t *string = ladder_game->string;
        PutStoneForSearch(ladder_game, i, color);
        if (ladder_game->ko_move == (ladder_game->moves - 1)) {
            Undo(ladder_game);
            continue;
        }
        GetNeighbor4(neighbor4, i);
        char flag = -1;
        int depth = 0;
        int max_depth = 0;
        for (int j = 0; j < 4; j++) {
            const int str = ladder_game->string_id[neighbor4[j]];
            if (board[neighbor4[j]] == FLIP_COLOR(color)
                && string[str].libs == 1
                && IsLegalForSearch(ladder_game, string[str].lib[0], FLIP_COLOR(color))) {
                int neighbor = string[str].neighbor[0];
                if (neighbor != NEIGHBOR_END && string[str].neighbor[neighbor] == NEIGHBOR_END) {
                    neighbor = NEIGHBOR_END;
                }
                while (neighbor != NEIGHBOR_END) {
                    if (string[neighbor].libs == 1) {
                        if (IsLegalForSearch(ladder_game, string[neighbor].lib[0], FLIP_COLOR(color))) {
                            PutStoneForSearch(ladder_game, string[neighbor].lib[0], FLIP_COLOR(color));
                            depth = 0;
                            if (IsLadderCaptured(depth, ladder_game, string[str].origin, color) ||
                                depth < cfg_ladder_defense) {
//                                flag = ALIVE;
                                if (depth > max_depth) max_depth = depth;
                            } else /*if (flag == -1)*/ {
                                flag = DEAD;
                            }
                            Undo(ladder_game);
                        }
                    }
                    neighbor = string[str].neighbor[neighbor];
                }
                PutStoneForSearch(ladder_game, string[str].lib[0], FLIP_COLOR(color));
                depth = 0;
                if (IsLadderCaptured(depth, ladder_game, string[str].origin, color) ||
                    depth < cfg_ladder_defense) {
//                    flag = ALIVE;
                    if (depth > max_depth) max_depth = depth;
                } else /*if (flag == -1)*/ {
                    flag = DEAD;
                }
                Undo(ladder_game);
            }
        }
        if (flag != DEAD && max_depth >= cfg_ladder_attack) ladder_pos[i] = LADDER;
//        if (cfg_ladder_check == 4 && flag == DEAD && depth >= cfg_ladder_defense) ladder_pos[i] = LADDER+1;
        Undo(ladder_game);
    }
}
////////////////////
//                //
////////////////////
static bool IsLadderCaptured(int &depth, search_game_info_t *game, const int ren_xy, const int turn_color)
{
    const char *board = game->board;
    const string_t *string = game->string;
    const int str = game->string_id[ren_xy];
    int escape_color, capture_color;
    int escape_xy, capture_xy;
    int neighbor, base_depth, max_depth;
    bool result;

    if (game->ko_move == (game->moves - 1) || depth >= cfg_ladder_depth) {
        return ALIVE;
    }

    if (board[ren_xy] == S_EMPTY) {
        return DEAD;
    } else if (string[str].libs >= 3) {
        return ALIVE;
    }

    escape_color = board[ren_xy];
    capture_color = FLIP_COLOR(escape_color);
    base_depth = depth;
    max_depth = depth;

    if (turn_color == escape_color) {
        neighbor = string[str].neighbor[0];
        while (neighbor != NEIGHBOR_END) {
            if (string[neighbor].libs == 1) {
                if (IsLegalForSearch(game, string[neighbor].lib[0], escape_color)) {
                    PutStoneForSearch(game, string[neighbor].lib[0], escape_color);
                    depth = base_depth;
                    result = IsLadderCaptured(++depth, game, ren_xy, FLIP_COLOR(turn_color));
                    Undo(game);
                    if (result == ALIVE) {
                        if (depth < max_depth) depth = max_depth;
                        return ALIVE;
                    }
                }
            }
            neighbor = string[str].neighbor[neighbor];
        }
        escape_xy = string[str].lib[0];
        while (escape_xy != LIBERTY_END) {
            if (IsLegalForSearch(game, escape_xy, escape_color)) {
                PutStoneForSearch(game, escape_xy, escape_color);
                depth = base_depth;
                result = IsLadderCaptured(++depth, game, ren_xy, FLIP_COLOR(turn_color));
                Undo(game);
                if (result == ALIVE) {
                    if (depth < max_depth) depth = max_depth;
                    return ALIVE;
                }
            }
            escape_xy = string[str].lib[escape_xy];
        }
        return DEAD;
    } else {
        if (string[str].libs == 1) {
            return DEAD;
        }
        capture_xy = string[str].lib[0];
        while (capture_xy != LIBERTY_END) {
            if (IsLegalForSearch(game, capture_xy, capture_color)) {
                PutStoneForSearch(game, capture_xy, capture_color);
                depth = base_depth;
                result = IsLadderCaptured(++depth, game, ren_xy, FLIP_COLOR(turn_color));
                Undo(game);
                if (result == DEAD) {
                    return DEAD;
                }
                if (depth < max_depth) depth = max_depth;
                else max_depth = depth;
            }
            capture_xy = string[str].lib[capture_xy];
        }
    }
    return ALIVE;
}

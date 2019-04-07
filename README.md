# leelaz_ladder

LeelaZero source code incorporating Ray's ladder check function

## Description

We dealt with the ladder problem, which is a weakness of the AI Go engine, using Ray's ladder check function.
It also incorporates efficient selection algorithm of the best move for low performance PC.

## Changes to LeelaZero

1) A move that can be judged as a ladder within a certain move number range in playout does not create a UCT node.
   However, the phase where kou is occurring is excluded.
2) If it is atari's movement in playout, the movement that can not take the opponent's stone within the fixed movement number range does not create a UCT node.
   However, the phase where kou is occurring is excluded.

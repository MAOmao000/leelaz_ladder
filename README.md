# leelaz_ladder

LeelaZero source code incorporating Ray's ladder check function

## Description

We coped with the ladder problem, which is a weakness of the AI Go engine, using Ray's ladder check function.

## Changes to LeelaZero

1) A move that can be judged as ladder in playout does not create UCT node.
   However, the phase where kou is occurring is excluded.
   In addition, it excludes the case where the stone is captured outside the range of the fixed number of moves.
2) UCT node are not created for movements that can not capture stones such as ladder during playout processing.
   However, the phase where kou is occurring is excluded.
   In addition, it excludes the case where the stone is captured outside the range of the fixed number of moves.

caSim -- headless play-testing simulator for Chessamphetamine (CA)
===================================================================

caSim plays CA over and over with a scripted player, without the graphical
engine, and reports how far players get.  It is meant for judging the
*average* effect of balance changes (piece rarities, shop costs, enemy
growth, etc.) that are impossible to see through a handful of human runs.


Building
--------

    ./buildCaSim.sh

This compiles three translation units with the game's strict C89 flags:

    caSim.c                        the simulator and the scripted player
    caSimMingin.c                  mingin.h's null platform implementation
    Chessamphetamine/maxigin_imp.c the stock maxigin implementation

caSim.c includes every CA module header with its *_IMPLEMENTATION define,
just like game.c does, so it can call the real game logic (getLevel,
getChessMove, shop costs, rarity rolls, unlocks, ...) and also reach each
module's static state directly (formation grid, side board slots, shop
slots, rng states, unlock flags).

The only non-CA header used is stdio.h (stdout output, and reading the
tiny .ini settings files).


Running
-------

    ./caSim

Settings files, read from the current directory:

    logOn.ini         1 = verbose log of every decision, formation, shop
                      layout, and board state after every move.
                      Anything else = only batch progress and reports.
    numPerBatch.ini   number of runs per batch
    maxLevel.ini      (optional, default 100) a run that reaches this
                      level is stopped and counted as reaching it

Four batches are always run, each starting every run from scratch:

    No unlocks
    First unlock only      (extra side board slot)
    First two unlocks      (+ extra shop slot)
    All three unlocks      (+ permanent sale slot)

Unlocks are granted with unlocksBeatLevel() before each run and cleared
with unlocksHardReset() after it (persist-data writes are no-ops under the
null mingin, so nothing touches the real unlocks.ini).  If the player
checkmates at/after level 8/16/32 during a run, they gain that unlock for
the rest of the run, exactly like the real game.

Speed: roughly 1.5-3 seconds per run (the chess AI dominates), so
100 runs per batch takes 10-20 minutes.


What one run does (mirrors game.c step by step)
-----------------------------------------------

    new game:  costFullReset(drawCost), moneyForce(5), heartsReset(),
               playerDeckSetupFresh(), formationBackToStart(), shopReset()
               + reseed chess/levels/deck/roll/shop rngs, reset rarity
               roll pools (so runs are independent)

    per level:
      1. player arranges formation spots (blind)
      2. getLevel(WHITE) draws deck pieces into the spots
      3. sideBoardRedraw() fills the side board
         getLevel(BLACK) reveals the enemy
      4. player swaps side board pieces in, pays redraws, arranges pieces
         board pieces -> deck as "played", side board -> deck as unplayed
      5. AI-vs-AI match via getChessMove(); captures pay moneyAddCapture;
         overrun after 50 no-capture moves (20 if no forced mate is
         possible), score decides, tie = player win; trapped side loses
      6. checkmate win: heartsGain(), unlocksBeatLevel(level)
         any loss: heartsLose();  dead -> run ends at this level
      7. playerDeckReshuffle(), shopReroll(), allowance +3, shopping
      8. drawCost reset/level-increment, playerDeckReshuffle(), level++


The scripted player
-------------------

Knobs are static variables near the top of caSim.c.

  Formation:  king on the back rank center (7,4); spots are filled in a
              fixed priority order (pawn shield in front of the king, then
              outward along the back two rows, then the front row).

  Drafting:   any side board piece better than the weakest board piece is
              swapped in.  Redraws are paid while the redraw cost is at
              most playerMaxRedrawCost and the deck still holds pieces
              better than the weakest one on the board.
              Pieces are placed pawn-like forward / big pieces back, then
              playerPlacementTrials random arrangements are scored with the
              chess AI's own look-ahead (getGreedyDepthMove, white's point
              of view) and the best one is kept -- this is what makes it
              set up good opening trades or a quick mate.

  Shop:       buys a formation spot whenever affordable (saves up when
              within playerSaveForSpotMargin of the price), then buys the
              highest-value affordable pieces whose value is at least the
              deck's current average value (so it doesn't dilute the deck),
              keeps playerMoneyReserve for next level's redraws, and rerolls
              only when money is plentiful.

Piece values are the chess AI's own material values (chess.h pieceValue).

Knob tuning (120 no-unlock runs per setting, average highest level):
    money reserve for redraws   0 -> 9.5    1 -> 8.6    2 -> 7.6
    max redraw cost paid        1 -> 9.3    2 -> 7.6    4 -> 8.0
    placement trials            0 -> 7.7   12 -> 7.6   24 -> 8.2  (40-run
                                                        sample for 0)
    buy pieces below deck avg   yes -> 7.3  no -> 7.6
    save up for spots           no -> 7.2   yes -> 7.6
  Run-to-run noise is about +/- 1 level at this sample size, but the
  clear lessons were: spend everything in the shop (spots and good pieces
  beat redraws), and only pay the 1-coin redraw.


Reports
-------

Per batch: histogram of the highest level reached (the level the player
died on), with cumulative "reached at least" percentages, average and
median, % reaching 8/16/32/64, unlocks earned in-run, a breakdown of match
results (checkmate / overrun / trapped, either side), a per-level table
of match outcomes (shows where difficulty spikes), and average formation
spots, deck size and money spent at death.  Each batch's report prints as
soon as that batch finishes; a summary table across the four batches
comes last.

All four batches use the same sequence of per-run seeds, so the batches
face the same enemy formations / deck shuffles / shop rolls until their
decisions diverge -- differences between batches are therefore much less
noisy than the run-to-run spread suggests.


Notes for future work
---------------------

  - Game balance parameters live in the CA source (shop.h prices,
    rarity.h weights, levels.h enemy growth, money.h capture values,
    cost setups in shop.h/game.c).  The drawCost setup and starting money
    are duplicated in caSim.c (simDrawCost, simStartingMoney,
    simShopAllowance) because they live inside game.c; keep them in sync.
  - Never call the module *Init functions more than once (they register
    memory and try to load assets).  Use the reset/clear functions.
  - The dummy mingin returns a constant entropy seed, so all rngs must be
    reseeded per run (simSeedEverything).
  - buildCaSim.sh passes -Wno-maybe-uninitialized because at -O3 gcc
    reports a false positive inside chess.h's getGreedyDepthMove.

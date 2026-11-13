/*
 * Path Master
 *
 * Every user-visible string lives here, as UTF-8, so that the game can
 * be translated by adding another table.  Number placeholders are %s
 * (numbers are formatted to strings before insertion) or %d.
 */

#include "game.h"


static const char *english[ NUM_TXT ] = {
    /* TXT_TITLE */         "PATH MASTER",
    /* TXT_CLICK_START */   "Click to walk the path",
    /* TXT_BEST */          "Best run: wave %d",
    /* TXT_HOWTO */
    "The wizard walks and fights alone.  You master the wand.\n"
    "\n"
    "\xE2\x80\xA2 Drag gems from the satchel into the wand's web.\n"
    "\xE2\x80\xA2 Gems that touch along a thread form synergies.\n"
    "\xE2\x80\xA2 Drop a gem on its twin to fuse a stronger gem.\n"
    "\xE2\x80\xA2 Web shards open new sockets.\n"
    "\xE2\x80\xA2 Melt spare gems in the crucible for experience.",
    /* TXT_WAND */          "WAND",
    /* TXT_SATCHEL */       "SATCHEL",
    /* TXT_CRUCIBLE */      "CRUCIBLE",
    /* TXT_CRUCIBLE_HINT */ "Drop here: +%d XP",
    /* TXT_LEVEL_UP */      "LEVEL UP!",
    /* TXT_CHOOSE */        "Choose a boon",
    /* TXT_SATCHEL_FULL */  "Satchel is full!  Make room first.",
    /* TXT_DEAD_TITLE */    "THE PATH ENDS",
    /* TXT_DEAD_STATS */    "Fell on wave %d  \xE2\x80\xA2  Level %d  \xE2\x80\xA2  %d foes slain",
    /* TXT_NEW_BEST */      "A new record!",
    /* TXT_WALK_AGAIN */    "Walk Again",
    /* TXT_STAGE_FMT */     "Stage %d: %s",
    /* TXT_BOSS_FMT */      "%s approaches!",
    /* TXT_LV_FMT */        "Lv %d",
    /* TXT_POWER_FMT */     "Power %s",
    /* TXT_FUSE_HINT */     "Drop onto another Lv %d %s to fuse.",
    /* TXT_MAX_LEVEL */     "Maximum level.",
    /* TXT_SYNERGIES */     "SYNERGIES",
    /* TXT_NO_SYNERGY */
    "None yet.  Socket two different gems side by side and see what "
    "happens.  Hover over a gem to see its partners.",
    /* TXT_EMPTY_SOCKET */  "An empty socket.  Drag a gem here.",
    /* TXT_LOCKED_SOCKET */ "A sealed socket.  Drag a Web Shard here to "
                            "open it.",
    /* TXT_LOCKED_FAR */    "A sealed socket.  Open a neighboring socket "
                            "first.",
    /* TXT_SHARD_NAME */    "Web Shard",
    /* TXT_SHARD_DESC */    "Opens one sealed socket of the wand's web.  "
                            "Drag it onto a sealed socket that touches an "
                            "open one.",
    /* TXT_SHARD_SHORT */   "Opens one sealed socket of the wand's web.",
    /* TXT_HEART_NAME */    "Heart of Oak",
    /* TXT_HEART_DESC */    "+20 maximum life and heals 40% of your life.",
    /* TXT_STAT_DAMAGE */   "Bolt damage",
    /* TXT_STAT_RATE */     "Casts / sec",
    /* TXT_STAT_CRIT */     "Critical",
    /* TXT_STAT_BURN */     "Burn / sec",
    /* TXT_STAT_CHILL */    "Chill",
    /* TXT_STAT_POISON */   "Poison / hit",
    /* TXT_STAT_CHAIN */    "Arcs",
    /* TXT_STAT_BOLTS */    "Bolts / cast",
    /* TXT_STAT_STEAL */    "Life steal",
    /* TXT_STAT_REGEN */    "Regen / sec",
    /* TXT_STAT_BARRIER */  "Barrier",
    /* TXT_SATCHEL_FULL_XP */ "Full! +%d XP",
    /* TXT_PAUSED */        "PAUSED",
    /* TXT_CANT_PLACE */    "Only gems fit in the web.",
    /* TXT_NEED_SHARD */    "That socket is sealed.",
    /* TXT_RESONANCE */     "Resonance: +25%% power for each touching %s.",
    /* TXT_BIOME0 */        "Meadow",
    /* TXT_BIOME1 */        "Emberwood",
    /* TXT_BIOME2 */        "Frostfell",
    /* TXT_BIOME3 */        "Ashlands",
    /* TXT_BOSS0 */         "King Slime",
    /* TXT_BOSS1 */         "The Night Wing",
    /* TXT_BOSS2 */         "Broodmother",
    /* TXT_BOSS3 */         "The Bone Lord",
    /* TXT_BOSS4 */         "Arch Imp",
    /* TXT_BOSS5 */         "Elder Golem",

    /* gem names */
    "Ruby", "Sapphire", "Topaz", "Emerald", "Amethyst", "Diamond",
    "Onyx", "Quartz", "Garnet", "Opal", "Citrine", "Moonstone",

    /* gem descriptions */
    "Ignites foes for %s fire damage per second.  +%s bolt damage.",
    "Chills foes, slowing them by %s%%.  +%s bolt damage.",
    "Bolts arc onward to %s more foes for %s%% damage.",
    "Each hit adds %s poison damage per second, stacking 6 times.",
    "%s%% chance for bolts to strike critically for double damage.",
    "Splits the bolt:  +%s extra bolts per cast at 60%% damage.",
    "Heals you for %s%% of the bolt damage you deal.",
    "+%s%% casting speed.",
    "+%s maximum life.  Regenerates %s life per second.",
    "Empowers every touching gem by +%s power.  Does nothing alone.",
    "+%s%% bolt damage.",
    "Every 6 seconds, conjures a barrier that absorbs %s damage.",

    /* synergy names */
    "Thermal Shock", "Plasma Burst", "Wildfire", "Shatter",
    "Superconductor", "Overclock", "Leeching Venom", "Executioner",
    "Kaleidoscope", "Barrage", "Bulwark", "Radiance", "Blood Pact",
    "Frost Armor", "Zenith",

    /* synergy descriptions */
    "+%s%% damage to burning or chilled foes.",
    "Burning foes explode when slain, for %s%% of their life.",
    "Burn and poison deal +%s%% damage.",
    "Critical hits freeze foes solid for %s seconds.",
    "Bolts arc to %s more foes.",
    "+%s%% casting speed.",
    "Heals you for %s%% of burn and poison damage.",
    "Instantly slays foes below %s%% life.",
    "Critical hits deal +%s%% more damage.",
    "Every 4th cast fires %s extra bolts.",
    "Barriers are %s%% stronger, and all hits hurt you less.",
    "Extra bolts deal full damage.  +%s%% bolt damage.",
    "+%s%% bolt damage per 100 maximum life.",
    "Foes that strike you are chilled and take %s%% of your bolt damage.",
    "+%s%% bolt damage and casting speed."
    };


static const char **currentLanguage = english;


const char *T( int inID ) {
    if( inID < 0 || inID >= NUM_TXT || currentLanguage[ inID ] == 0 ) {
        return "?";
        }
    return currentLanguage[ inID ];
    }

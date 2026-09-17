// d2fmh_versions.h — per-patch addresses and structure offsets for the D2F in-game module.
//
// HOW THESE WERE OBTAINED. Every function address here was located by disassembling that patch's own
// D2Client.dll / D2Common.dll / D2Net.dll (the stock files under privtools\Cactus-master) and matching
// the body of the function, not by copying a table. Where a published table exists — BH /
// slashdiablo-maphack (1.13c, 1.13d), D2BS 1.1 (1.12a), jieaido/d2hackmap (1.11b) — the addresses found
// that way agreed with it exactly, which is what gives confidence in the two patches no published table
// covers, 1.10 and 1.11.
//
// The 1.10 STRUCTURE offsets are different: they were read out of the running game (the dump= option in
// d2fmh.ini prints the room structures), because no published table covers 1.10 and the three tables
// that do cover later patches disagree with each other about the preset-unit layout — Blizzard shuffled
// these structures at 1.10, 1.11, 1.12 and 1.13. So each patch gets its own set and none is assumed.
//
// WHAT IS VERIFIED. 1.10 has been run. The other five are assembled from the sources above and are
// sanity-checked at run time (see SaneTargets in d2fmh.c): if a patch's offsets were wrong the module
// disables travel for that patch and says so in the log rather than misbehaving in game.
#pragma once
#include <windows.h>

typedef struct D2Version {
    const char* name;
    DWORD       d2clientTimeStamp;   // IMAGE_FILE_HEADER.TimeDateStamp of that patch's D2Client.dll

    // ---- D2Client.dll (RVAs)
    DWORD       revealAutomapRoom;   // void (Room1*, DWORD drawAll, AutomapLayer*)
    BOOL        revealIsFastcall;    // 1.09d and 1.10; every later patch is __stdcall
    DWORD       newAutomapCell;      // AutomapCell* __fastcall (void)
    DWORD       addAutomapCell;      // void __fastcall (AutomapCell*, AutomapCell** list)
    DWORD       automapLayer;        // AutomapLayer* : the layer the automap is currently drawing
    DWORD       playerUnit;          // UnitAny*

    // ---- D2Common.dll / D2Net.dll (export ordinals)
    DWORD       ordAddRoomData;      // void __stdcall (Act*, int levelNo, int x, int y, Room1*)
    DWORD       ordRemoveRoomData;   // same signature
    DWORD       ordSendPacket;
    // 1.10 takes (unused, buffer, length); every later patch takes (length, unused, buffer). Checked in
    // the disassembly of each - getting this backwards would send the game server nonsense.
    BOOL        sendLenFirst;

    // ---- Room2
    DWORD       room2_pLevel, room2_posX, room2_posY, room2_sizeX, room2_sizeY;
    DWORD       room2_pNext, room2_pRoom1;
    DWORD       room2_pRoomsNear, room2_numRoomsNear;   // how exits to other levels are found
    DWORD       room2_pPreset;

    // ---- Room1 / Level
    DWORD       room1_pRoom2;
    DWORD       room1_pUnitFirst;    // head of the units standing in this room (monsters, players, items)
    DWORD       level_levelNo, level_pRoom2First;

    // ---- PresetUnit (the fixed contents of a room: monsters, objects, warps)
    DWORD       preset_type, preset_txt, preset_x, preset_y, preset_next;

    // ---- UnitAny / Path
    //
    // These were the same from 1.10 to 1.13d and were constants here until 1.08 and 1.09 were added,
    // which do not share them. Read out of those patches' own code: the unit-table lookup compares the
    // id at +0x08, the player check reads the mode at +0x0C and the path at +0x38 (D2Common's
    // room-for-a-unit is literally "mov eax,[unit+38h]; mov eax,[eax+1Ch]"), the item-data pointer is
    // asserted non-null at +0x70 right after the type==4 check, and the stat list is at +0x6C. The act
    // pointer at +0x18 is the one value here that is empirical rather than read from code: rooms load on
    // demand through AddRoomData with it, which they could not with a wrong pointer.
    DWORD       unit_pAct, unit_pPath, unit_dwUnitId, unit_pItemData;
    DWORD       path_xPos, path_yPos, path_pRoom1, path_pUnit;
    // dwMode is the one that bit: it was a fixed 0x10 in the code, which on 1.08 and 1.09 is the act
    // number instead, so every monster in Act I read as mode 0 - "dead" - and none was drawn.
    DWORD       unit_dwMode;
    // The 1.09-era stat list is not the flat array of eight-byte entries that 1.10 onwards keep. Its
    // shape was read out of D2Common's own stat SETTER (ordinal 10517 on 1.08 and 1.09d, which the
    // client calls as (unit, 6, 0) when the character dies): stats+8 is the working list, whose entry
    // blocks start at +0x4C - a count, then up to fifteen (id, value) pairs, then the next block at
    // +0x7C. Zero here means the 1.10-style list is found by the search in d2fmh.c instead.
    //
    // A warning for whoever comes next: that setter LOOKS like a getter - same three arguments, same
    // null-check on the list - and calling it to read life zeroed the character's client-side life
    // and mana four times a second. Nothing in this module calls a D2Common routine to read a stat.
    DWORD       unit_pStats;
    // The game's unit table, a hash of 128 buckets per unit type. unitMonsters is the RVA of the
    // monster row (type 1), and a bucket is a linked list chained by unitChain. Reading it is exact
    // where the old sweep was a guess: the sweep saw only the bucket heads that sat in the array and
    // missed every monster hanging off a chain, and on 1.08 it latched onto the wrong region entirely
    // and drew the town's rogues instead of the monsters around the character. Zero falls back to the
    // sweep. Found from the client's own id lookup: `mov esi,[idx*4+base]; cmp [esi+id],..; mov esi,[esi+chain]`.
    //
    // ORDER MATTERS: these three must sit here in the same order the rows list them (mode, stats,
    // monsters, chain). Putting unitMonsters before unit_pStats once made the census log "row +6C" -
    // it was reading the stats value - and broke monsters, vitals and potions all at once.
    DWORD       unitMonsters;
    DWORD       unitChain;

    // ---- On-screen overlay. Zero for any of these disables the overlay on that patch.
    //
    // The game draws the automap by calling one "draw this cell list" routine three times, once for
    // floors, walls and objects. drawHookCall is the RVA of the THIRD of those call instructions and
    // drawHookTarget the routine it calls: replacing that call's target with our own stub gives a slot
    // that runs exactly once per drawn frame, after the map itself, which is where an overlay belongs.
    DWORD       drawHookCall, drawHookTarget;
    // A second slot, on a call the in-game draw makes unconditionally just after it has (or has not)
    // drawn the automap. Screen-space things like the list of places go here so they show whether or
    // not the map is open.
    DWORD       drawHookCall2, drawHookTarget2;
    // The game converts an automap cell's pixel position to a screen position as
    // cellPixel * 10 / divisor - offset, reading these two globals. Our lines and labels use the same
    // arithmetic so they sit exactly where the map does at any zoom or scroll position.
    DWORD       automapDivisor;      // int
    DWORD       automapOffset;       // POINT (x then y)
    DWORD       ordDrawLine;         // D2gfx:  void __stdcall (x1, y1, x2, y2, colour, unused)
    DWORD       ordDrawText;         // D2Win:  void __fastcall (wchar_t*, x, y, colour, unused)

    // The Act to load rooms into, when it is easier to read a client global than to walk the player.
    // 1.00's client keeps the current Act in a global rather than at a fixed offset on the player unit,
    // and that global is simpler to trust than hunting the pAct offset in an early debug build. Zero
    // means "read player->pAct", which every later patch does. Left off a row, C zero-fills it.
    DWORD       actGlobal;
} D2Version;

// Unit and path fields read through the patch in hand. Written as names rather than g_ver->... at every
// use so the code reads the same as it did when these were fixed numbers; g_ver is set before anything
// touches a unit, and nothing here runs on an unrecognised patch.
#define UNIT_pAct      (g_ver->unit_pAct)
#define UNIT_pPath     (g_ver->unit_pPath)
#define UNIT_dwUnitId  (g_ver->unit_dwUnitId)
#define UNIT_pItemData (g_ver->unit_pItemData)
#define PATH_xPos      (g_ver->path_xPos)     // WORD, world subtile
#define PATH_yPos      (g_ver->path_yPos)     // WORD
#define PATH_pRoom1    (g_ver->path_pRoom1)
#define PATH_pUnit     (g_ver->path_pUnit)    // a path points back at its own unit: how a unit is recognised
#define LAYER_pObjects 0x10      // AutomapLayer.pObjects - the cell list our markers go into

// PresetUnit.type values.
#define PRESET_MONSTER 1
#define PRESET_OBJECT  2
#define PRESET_WARP    5

static const D2Version g_versions[] = {
  // 1.00, the original Classic release - REVEAL ONLY. It is a debug build (int-3 padding) with a
  // Classic-era unit layout (pPath 0x34), and it cannot join the LoD realm, so only the level reveal is
  // wired up. The room, level and preset structures are identical to LoD, which is what makes reveal
  // work; monsters, potions and the overlay are left off (their fields zero). Base 0x10000000. The Act
  // comes from the client global 0x12EDFC rather than player->pAct (see actGlobal).
  { "1.00", 0x392ECD34,
    /* reveal */ 0x30DF0, TRUE, /* newCell */ 0x2F160, /* addCell */ 0x308A0,
    /* layer */ 0x17D480, /* player */ 0x1817BC,
    /* add/remove/send */ 10063, 10064, 10005, FALSE,
    /* Room2 */ 0x00, 0x04, 0x08, 0x0C, 0x10, 0xE8, 0xE4, 0xC0, 0xC4, 0xCC,
    /* Room1/Level */ 0x38, 0x3C, 0x04, 0x30,
    /* Preset */ 0x00, 0x04, 0x0C, 0x10, 0x1C,
    /* Unit/Path */ 0x00, 0x34, 0x00, 0x00, 0x02, 0x06, 0x1C, 0x00, /* mode */ 0x00, /* stats */ 0x00,
    /* monsters */ 0x00, 0x00,
    /* Overlay: off */ 0, 0, 0, 0, 0, 0, 10057, 10117,
    /* actGlobal */ 0x12EDFC },

  // 1.07, the first LoD patch. Base 6FAD0000 (same as 1.08); unit layout is the 1.08/1.09-era shift
  // (pPath 0x38, id 0x08, data 0x70), confirmed by D2Common's room-for-unit reading [unit+38h]/[+1Ch].
  { "1.07", 0x3B007197,
    /* reveal */ 0x271E0, TRUE, /* newCell */ 0x25A70, /* addCell */ 0x26DB0,
    /* layer */ 0x1242A0, /* player */ 0x12F2A0,
    /* add/remove/send */ 10063, 10064, 10005, FALSE,
    /* Room2 */ 0x00, 0x04, 0x08, 0x0C, 0x10, 0xE8, 0xE4, 0xC0, 0xC4, 0xCC,
    /* Room1/Level */ 0x38, 0x3C, 0x04, 0x30,
    /* Preset */ 0x00, 0x04, 0x0C, 0x10, 0x1C,
    /* Unit/Path */ 0x18, 0x38, 0x08, 0x70, 0x02, 0x06, 0x1C, 0x30, /* mode */ 0x0C, /* stats */ 0x6C,
    /* monsters */ 0x12DCA0, 0x108,
    /* Overlay */ 0x27ED5, 0x282C0, 0x8BC40, 0x934E0, 0xE1D10, 0x1242D4, 10057, 10117 },

  // 1.08, found the same way as 1.09d and agreeing with it in every structure. Its D2Client sits at a
  // different preferred base (6FAD0000, not 6FAA0000), so the addresses look unrelated until they are
  // turned back into RVAs. Everything else lines up: the same reveal body, the same cell helpers, the
  // same 10063/10064/10005 ordinals, the same three automap draw calls 0x235 into the draw, the same
  // 0x0A/0x14 divisor, the screen offset again 0x34 past the layer, and the same drawing ordinals.
  { "1.08", 0x3B2EB437,
    /* reveal */ 0x26B40, TRUE, /* newCell */ 0x253D0, /* addCell */ 0x26710,
    /* layer */ 0x119E10, /* player */ 0x1245E0,
    /* add/remove/send */ 10063, 10064, 10005, FALSE,
    /* Room2 */ 0x00, 0x04, 0x08, 0x0C, 0x10, 0xE8, 0xE4, 0xC0, 0xC4, 0xCC,
    /* Room1/Level */ 0x38, 0x3C, 0x04, 0x30,
    /* Preset */ 0x00, 0x04, 0x0C, 0x10, 0x1C,
    /* Unit/Path */ 0x18, 0x38, 0x08, 0x70, 0x02, 0x06, 0x1C, 0x30, /* mode */ 0x0C, /* stats */ 0x6C, /* monsters */ 0x122FE0, 0x108,
    /* Overlay */ 0x27835, 0x27C20, 0x86DB0, 0x8E250, 0xD7D10, 0x119E44, 10057, 10117 },

  // 1.09 (also 1.09b - they share this exact D2Client, timestamp 3B7C5076). Same era and structures as
  // 1.09d, addresses found the same way in this build. Base 6FAA0000.
  { "1.09", 0x3B7C5076,
    /* reveal */ 0x26E80, TRUE, /* newCell */ 0x25710, /* addCell */ 0x26A50,
    /* layer */ 0x11CDA0, /* player */ 0x127578,
    /* add/remove/send */ 10063, 10064, 10005, FALSE,
    /* Room2 */ 0x00, 0x04, 0x08, 0x0C, 0x10, 0xE8, 0xE4, 0xC0, 0xC4, 0xCC,
    /* Room1/Level */ 0x38, 0x3C, 0x04, 0x30,
    /* Preset */ 0x00, 0x04, 0x0C, 0x10, 0x1C,
    /* Unit/Path */ 0x18, 0x38, 0x08, 0x70, 0x02, 0x06, 0x1C, 0x30, /* mode */ 0x0C, /* stats */ 0x6C,
    /* monsters */ 0x125F78, 0x108,
    /* Overlay */ 0x27B75, 0x27F60, 0x87420, 0x8E8A0, 0xD8D10, 0x11CDD4, 10057, 10117 },

  // 1.09d. Every address here was read out of this patch's own code rather than adapted from 1.10:
  //   reveal      the one function containing "test al,8 / test eax,20000h / add esi,30h", fastcall
  //               with the layer as a stack argument, exactly as 1.10's
  //   cells       "and edx,1FFh / sar eax,9 / and ebx,800001FFh" and the cell sorter that compares
  //               word [x+8] then word [x+6]
  //   layer       the global the layer switch writes after clearing the four cell lists at +8..+14
  //   player      the global read by the function that then checks mode 1 or 5 and the path pointer;
  //               it is referenced thirteen times in 1.09d and thirteen times in 1.10, the same places
  //   room data   D2Common ordinals 10063/10064 again: the same five-argument pair that asserts on a
  //               null act and reads Room1+0x38, byte for byte the same body as 1.10's
  // The room, level and preset structures turned out to be UNCHANGED from 1.10 — the D2Common routines
  // that walk them are identical instruction for instruction, down to +0xC0/+0xC4 for the near-room
  // array, +0xE8 for the next room and +0xCC/+0x1C for the preset list. What did change is UnitAny.
  { "1.09d", 0x3C0700B0,
    /* reveal */ 0x26E70, TRUE, /* newCell */ 0x25700, /* addCell */ 0x26A40,
    /* layer */ 0x11BC00, /* player */ 0x1263F8,
    /* add/remove/send */ 10063, 10064, 10005, FALSE,
    /* Room2 */ 0x00, 0x04, 0x08, 0x0C, 0x10, 0xE8, 0xE4, 0xC0, 0xC4, 0xCC,
    /* Room1/Level */ 0x38, 0x3C, 0x04, 0x30,
    /* Preset */ 0x00, 0x04, 0x0C, 0x10, 0x1C,
    /* Unit/Path */ 0x18, 0x38, 0x08, 0x70, 0x02, 0x06, 0x1C, 0x30, /* mode */ 0x0C, /* stats */ 0x6C, /* monsters */ 0x124DF8, 0x108,
    // The overlay too: 1.09d draws its automap with the same three calls to one cell-list routine, the
    // third of them 0x235 bytes into the draw exactly as in 1.10, writes the same 0x0A/0x14 divisor and
    // the same pair of offset words, and exports DrawLine and DrawText under the same two ordinals.
    /* Overlay */ 0x27B65, 0x27F50, 0x867A0, 0x8DC20, 0xD7C58, 0x11BC34, 10057, 10117 },

  { "1.10", 0x3F7CB8BE,
    /* reveal */ 0x2D180, TRUE, /* newCell */ 0x2BA40, /* addCell */ 0x2CD50,
    /* layer */ 0x1119A4, /* player */ 0x11C200,
    /* add/remove/send */ 10063, 10064, 10005, FALSE,
    /* Room2 */ 0x00, 0x04, 0x08, 0x0C, 0x10, 0xE8, 0xE4, 0xC0, 0xC4, 0xCC,
    /* Room1/Level */ 0x38, 0x3C, 0x04, 0x30,
    /* Preset */ 0x00, 0x04, 0x0C, 0x10, 0x1C,
    /* Unit/Path */ 0x1C, 0x2C, 0x0C, 0x14, 0x02, 0x06, 0x1C, 0x30, /* mode */ 0x10, /* stats */ 0, /* monsters */ 0x11AC00, 0xE4,
    /* Overlay */ 0x2DEE5, 0x2E230, 0x81D6E, 0x89370, 0xD7BC0, 0x1119D8, 10057, 10117 },

  { "1.11", 0x42E6C43F,
    0x5A880, FALSE, 0x57AE0, 0x59640,
    0x11C248, 0x11C4F0,
    10432, 10716, 10035, TRUE,
    0x1C, 0x20, 0x24, 0x28, 0x2C, 0x38, 0xE8, 0x30, 0x10, 0x34,
    0x38, 0x3C, 0x14, 0x204,
    0x1C, 0x0C, 0x18, 0x08, 0x14,
    /* Unit/Path */ 0x1C, 0x2C, 0x0C, 0x14, 0x02, 0x06, 0x1C, 0x30, /* mode */ 0x10, /* stats */ 0, /* monsters */ 0x10B690, 0xE4,
    /* Overlay: not yet located for this patch */ 0, 0, 0, 0, 0, 0, 0, 0 },

  { "1.11b", 0x43028CA5,
    0x52A20, FALSE, 0x4FB10, 0x515F0,
    0x11C154, 0x11C1E0,
    10787, 10672, 10020, TRUE,
    0x1C, 0x20, 0x24, 0x28, 0x2C, 0x38, 0xE8, 0x30, 0x10, 0x34,
    0x38, 0x3C, 0x14, 0x204,
    0x1C, 0x0C, 0x18, 0x08, 0x14,
    /* Unit/Path */ 0x1C, 0x2C, 0x0C, 0x14, 0x02, 0x06, 0x1C, 0x30, /* mode */ 0x10, /* stats */ 0, /* monsters */ 0x10B870, 0xE4,
    /* Overlay: not yet located for this patch */ 0, 0, 0, 0, 0, 0, 0, 0 },

  { "1.12a", 0x483CB8DF,
    0x404C0, FALSE, 0x3D5B0, 0x3F090,
    0x11C2B4, 0x11C3D0,
    10184, 11009, 10036, TRUE,
    0x00, 0x2C, 0x30, 0x34, 0x38, 0xD4, 0xD8, 0x10, 0x08, 0xC4,
    0x70, 0x40, 0x94, 0x21C,
    0x1C, 0x00, 0x0C, 0x14, 0x18,
    /* Unit/Path */ 0x1C, 0x2C, 0x0C, 0x14, 0x02, 0x06, 0x1C, 0x30, /* mode */ 0x10, /* stats */ 0, /* monsters */ 0x11AD60, 0xE4,
    /* Overlay: not yet located for this patch */ 0, 0, 0, 0, 0, 0, 0, 0 },

  { "1.13c", 0x4B95CA3E,
    0x62580, FALSE, 0x5F6B0, 0x61320,
    0x11C1C4, 0x11BBFC,
    10401, 11099, 10024, TRUE,
    0x58, 0x34, 0x38, 0x3C, 0x40, 0x24, 0x30, 0x08, 0x2C, 0x5C,
    0x10, 0x74, 0x1D0, 0x010,
    0x14, 0x04, 0x08, 0x18, 0x0C,
    /* Unit/Path */ 0x1C, 0x2C, 0x0C, 0x14, 0x02, 0x06, 0x1C, 0x30, /* mode */ 0x10, /* stats */ 0, /* monsters */ 0x10AA08, 0xE4,
    /* Overlay: not yet located for this patch */ 0, 0, 0, 0, 0, 0, 0, 0 },

  { "1.13d", 0x4E9DE60A,
    0x73160, FALSE, 0x703C0, 0x71EA0,
    0x11CF28, 0x11D050,
    10890, 10208, 10015, TRUE,
    0x58, 0x34, 0x38, 0x3C, 0x40, 0x24, 0x30, 0x08, 0x2C, 0x5C,
    0x10, 0x74, 0x1D0, 0x010,
    0x14, 0x04, 0x08, 0x18, 0x0C,
    /* Unit/Path */ 0x1C, 0x2C, 0x0C, 0x14, 0x02, 0x06, 0x1C, 0x30, /* mode */ 0x10, /* stats */ 0, /* monsters */ 0x104BB8, 0xE4,
    /* Overlay: not yet located for this patch */ 0, 0, 0, 0, 0, 0, 0, 0 },
};
#define NUM_VERSIONS (sizeof(g_versions) / sizeof(g_versions[0]))

// ---------------------------------------------------------------- content tables
// Object numbers are the same across 1.10-1.13d, so these need no per-patch variant.

// Every waypoint in the game, by object number.
static const int kWaypointObjects[] = {
    119, 145, 156, 157, 237, 238, 288, 323, 324, 398, 402, 429, 494, 496, 511, 539
};

// A short list of places worth travelling to that are not exits or waypoints.
static const struct { int txt; const char* name; } kSpecialObjects[] = {
    { 152, "Horadric orifice" },
    { 267, "stash" },
    { 371, "Countess chest" },
    { 376, "Hell Forge" },
    { 460, "Anya" },
    { 580, "uber chest" },
};
#define NUM_SPECIALS (sizeof(kSpecialObjects) / sizeof(kSpecialObjects[0]))

// Level names by level number, for the overlay labels. The game can look these up itself, but that is
// another address per patch for a table that has not changed since 1.10, so it is simply data here.
static const char* const kLevelNames[] = {
 /*  0 */ "",
 /*  1 */ "Rogue Encampment", "Blood Moor", "Cold Plains", "Stony Field", "Dark Wood",
 /*  6 */ "Black Marsh", "Tamoe Highland", "Den of Evil", "Cave Level 1", "Underground Passage 1",
 /* 11 */ "Hole Level 1", "Pit Level 1", "Cave Level 2", "Underground Passage 2", "Hole Level 2",
 /* 16 */ "Pit Level 2", "Burial Grounds", "Crypt", "Mausoleum", "Forgotten Tower",
 /* 21 */ "Tower Cellar 1", "Tower Cellar 2", "Tower Cellar 3", "Tower Cellar 4", "Tower Cellar 5",
 /* 26 */ "Monastery Gate", "Outer Cloister", "Barracks", "Jail Level 1", "Jail Level 2",
 /* 31 */ "Jail Level 3", "Inner Cloister", "Cathedral", "Catacombs 1", "Catacombs 2",
 /* 36 */ "Catacombs 3", "Catacombs 4", "Tristram", "The Secret Cow Level",
 /* 40 */ "Lut Gholein", "Rocky Waste", "Dry Hills", "Far Oasis", "Lost City",
 /* 45 */ "Valley of Snakes", "Canyon of the Magi", "Sewers Level 1", "Sewers Level 2", "Sewers Level 3",
 /* 50 */ "Harem Level 1", "Harem Level 2", "Palace Cellar 1", "Palace Cellar 2", "Palace Cellar 3",
 /* 55 */ "Stony Tomb 1", "Halls of the Dead 1", "Halls of the Dead 2", "Claw Viper Temple 1",
 /* 59 */ "Stony Tomb 2", "Halls of the Dead 3", "Claw Viper Temple 2", "Maggot Lair 1",
 /* 63 */ "Maggot Lair 2", "Maggot Lair 3", "Ancient Tunnels", "Tal Rasha's Tomb 1",
 /* 67 */ "Tal Rasha's Tomb 2", "Tal Rasha's Tomb 3", "Tal Rasha's Tomb 4", "Tal Rasha's Tomb 5",
 /* 71 */ "Tal Rasha's Tomb 6", "Tal Rasha's Tomb 7", "Tal Rasha's Chamber", "Arcane Sanctuary",
 /* 75 */ "Kurast Docks", "Spider Forest", "Great Marsh", "Flayer Jungle", "Lower Kurast",
 /* 80 */ "Kurast Bazaar", "Upper Kurast", "Kurast Causeway", "Travincal", "Arachnid Lair",
 /* 85 */ "Spider Cavern", "Swampy Pit 1", "Swampy Pit 2", "Flayer Dungeon 1", "Flayer Dungeon 2",
 /* 90 */ "Swampy Pit 3", "Flayer Dungeon 3", "Kurast Sewers 1", "Kurast Sewers 2", "Ruined Temple",
 /* 95 */ "Disused Fane", "Forgotten Reliquary", "Forgotten Temple", "Ruined Fane", "Disused Reliquary",
 /*100 */ "Durance of Hate 1", "Durance of Hate 2", "Durance of Hate 3", "The Pandemonium Fortress",
 /*104 */ "Outer Steppes", "Plains of Despair", "City of the Damned", "River of Flame",
 /*108 */ "The Chaos Sanctuary", "Harrogath", "Bloody Foothills", "Frigid Highlands", "Arreat Plateau",
 /*113 */ "Crystalline Passage", "Frozen River", "Glacial Trail", "Drifter Cavern", "Frozen Tundra",
 /*118 */ "The Ancients' Way", "Icy Cellar", "Arreat Summit", "Nihlathak's Temple", "Halls of Anguish",
 /*123 */ "Halls of Pain", "Halls of Vaught", "Abaddon", "Pit of Acheron", "Infernal Pit",
 /*128 */ "Worldstone Keep 1", "Worldstone Keep 2", "Worldstone Keep 3", "Throne of Destruction",
 /*132 */ "The Worldstone Chamber", "Matron's Den", "Forgotten Sands", "Furnace of Pain", "Uber Tristram",
};
#define NUM_LEVEL_NAMES (sizeof(kLevelNames) / sizeof(kLevelNames[0]))

// Automap cell numbers used for our markers. These index the game's own automap cell file, which comes
// from the MPQs and is the same in every patch.
#define CELL_EXIT     301
#define CELL_WAYPOINT 300
#define CELL_SPECIAL  318

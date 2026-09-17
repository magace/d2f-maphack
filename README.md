# D2F Utilities

A launcher for the D2F realm's previous-patch ladders: starts one or more Diablo II clients
(1.07, 1.08, 1.09, 1.09d, 1.10, 1.11, 1.11b, 1.12a, 1.13c, 1.13d) side by side and gives them a maphack. Kolbot/D2BS only
run on 1.13c/1.13d, so this is what players on the older patches use.

One file to run: `D2FUtil\publish\D2FUtil.exe`. It bundles the .NET runtime and the in-game DLL, so nothing
is installed; settings, the extracted DLL and its log land in `%LOCALAPPDATA%\D2FUtil\`.

## Patches (the Platforms layout)

If a `Platforms` folder sits next to the exe, with a subfolder of DLLs for each patch (the Cactus
layout), and a base `Diablo II` folder with the shared archives, the launcher finds them and shows a
**Patches** checklist. Tick the patches you want, set a client count for each, and one Launch starts
them all side by side, each with the maphack.

Each ticked patch runs from its own folder under `run`, next to Platforms, assembled the first time
it is launched: the platform's small DLL-and-exe set is copied in, and the base folder's big archives
are brought in as hard links, which cost no extra disk because they point at the same bytes. So 1.08,
1.10 and 1.13d can run at once without copying a gigabyte per patch or disturbing the base folder.
Patches with no maphack table still launch (multi-instance only), and the single-folder picker stays
below for a one-off full install.

## Using it

The settings are grouped into sections — Game, Maphack, Travel, Potions, Log — listed down the left,
so each page fits on screen and Launch stays put at the bottom with the last log line beside it.

The wheel never changes a setting by accident: over a drop-down it scrolls the page and only moves
through the list while the list is open, and over a number it scrolls the page until you click into
the field. Rebinding a key by scrolling past it is the kind of change nobody notices until a hotkey
stops working.

The window is dark, in the game's own gold-on-black. WinForms has no dark mode, so the check boxes,
the number fields and the drop-down arrows are drawn by `Theme.cs` rather than by the system, which
also means the display's scaling is honoured by hand through `Theme.Px` — sizes come out right at
100% and at 150%. The caption bar and the scroll bars are darkened through the desktop window
manager, and simply stay light on a Windows too old to offer it.

1. Browse to the Diablo II folder you want to play from (the one holding `Game.exe`). Add as many
   folders as you have patches; the patch is read from each folder's `D2Client.dll` and shown next
   to it. Any folder with a `Game.exe` launches; the maphack works on the ten patches above.
2. Pick launch options (windowed is on by default so several clients fit on screen) and how many
   clients to start.
3. Launch. Every client started here can run alongside other copies of the game.

In game: **F5** toggles the maphack, **F6** re-reveals the level you are in (a beep confirms the
toggle). The maphack reveals the whole level you are standing in as soon as you enter it, into the
game's own automap — open the map as usual.

## Travel

The module works out where the level's exits, waypoint and a few notable places are, marks them on
the automap, and takes the character to whichever one you pick:

| Default key | Goes to |
|---|---|
| Page Down | the next area (the lowest-numbered level this one leads on to, which is the main path) |
| Page Up | the previous area (the level you came from) |
| Home | the waypoint |
| End | cycles through every target in the level, one per press |

Press the same key again to stop. All of these are configurable in the launcher, and the module
consumes the keys it uses, so binding one to a function key will not also fire the game's skill slot.

**Move by** chooses how it travels: *Run* and *Walk* let the game server path around obstacles, so
they cope with walls. *Teleport* sends a right-click skill cast at each hop, so it only teleports if
Teleport is the character's right-click skill — otherwise it casts whatever is bound there.

Exits are found from the game's room graph rather than from warp tiles, so both kinds of level border
work: the seamless walk from town into the Blood Moor, and stairs such as the Den of Evil.

Teleport lands exactly where it is aimed and fails silently if the destination is not standable, which
is what used to make it stick. It now reads the game's collision grid and picks the longest hop along
the straightest direction that is actually standable, so it glides rather than bouncing off walls.
Where there is no grid to read, because the room is not loaded, the cell is not ruled out and a
did-it-move check retries at a widening spread of angles instead.

The grid's location inside a room differs per patch, so rather than carrying another number per patch
the module finds it: a collision map is self-describing, and the one thing in a room whose subtile
size is exactly five times its room size with a readable grid of collision flags behind it. It then
checks the grid against the one cell known to be standable, the one the character is on, and stops
trusting it if they disagree.

## Overlay

With the automap open, the module draws a line from the character to every target, labelled with the
place and the key that goes there, and marks nearby monsters. It draws through the game's own line
and text routines from inside the game's automap draw, so it pans and zooms with the map.

Above the control bar it shows life and mana as the module reads them, what you have left to drink,
and the places you can travel to with the key for each, in the style of the older maphacks:

    HP 526/526 (100%)   MP 299/297 (100%)
    Potions: 12 HP  8 MP  2 RV
    Home: Kurast Causeway
    Ins: Waypoint

The counts cover the belt and the inventory together, and count only potions whose kind has been
identified; any still unidentified are shown as such rather than counted as nothing. The life and mana
line turns gold when one of them is under its auto potion threshold, the potion line turns gold when
there is nothing left to heal with, and no life and mana line at all is the quickest way to see that
the module cannot read the character. That list is drawn from a second hook, on a call the game makes every frame whether
or not it drew the map, so it is there with the map closed too. `list_x` and `list_bottom` in the ini
move it; the launcher has a switch to turn it off.

A level border usually runs along several rooms, so a destination can be entered from more than one
place. The marker and the travel target are the nearest of them to the character, not an arbitrary one
along the border.

Line colours are palette indices, so the shade depends on the game's palette and picking one is trial
and error. They live in `%LOCALAPPDATA%\D2FUtil\settings.json` as `ColourExit`, `ColourWaypoint`,
`ColourPlace`, `ColourMonster` and `ColourActive` rather than in the launcher window. The target you
are currently travelling to is drawn in the active colour with a gold label.

The overlay works on 1.07, 1.08, 1.09, 1.09d and 1.10. The other patches need their own draw hook and drawing ordinals
located the same way; until then they get the maphack, travel and automap markers but no lines.

Scripting: `D2FUtil.exe --launch "C:\Games\Diablo II" -w -ns` starts one client headlessly (result in
`%LOCALAPPDATA%\D2FUtil\launch.log`).

If a Game.exe has been given a Windows compatibility mode (right-click → Properties → Compatibility,
common on old installs), Windows insists it be started elevated and the launcher will offer to
restart itself as administrator. Clearing the compatibility mode avoids the prompt; the game does not
need it on Windows 10/11 in windowed mode.

## Auto potion

Drinks for you when life or mana drops below the percentages set in the launcher, and holds
rejuvenations back for below a third percentage so they are not spent topping up.

It picks a potion by **what it is**, not by which belt key it sits under. A belt is not organised into
a healing column and a mana column, so it reads the character's items and remembers each potion by its
item type, learned from what drinking one actually did: life up is healing, mana up is mana, both up is
a rejuvenation. That is true on every patch and in every language, so no item numbers are hardcoded.

The first potion of a kind you ever drink is the one that teaches it, and the answer is written to
`%LOCALAPPDATA%\D2FUtil\d2fmh-learn.ini`, per patch, so it is known in every game after that. Until a
kind is known it presses a belt key to find out, which is safe because a belt key can only ever produce
a belt potion.

Potions in the inventory are used too, but only of a kind already identified from the belt, and only
once the game has shown how it uses one: the module watches the client's own packets and keeps the one
that carries the item, so what it sends is the game's format rather than a guess. Right-click a potion
in your inventory once to teach it that; the log says when it has.

Where the inventory hangs off the character, what links the items together, where a position is kept
and which byte marks an item as being in the belt are all worked out from the data, the same way the
rooms and the stat list are. The belt marker is settled from a fact that costs nothing: whatever
vanished after a belt key press was in the belt, at the bottom of the column pressed.

## Status

Verified 2026-09-11 on 1.10 in single player: town and Blood Moor revealed on entry (35 and 83
rooms), targets found correctly (town gave the waypoint and the stash, the Blood Moor gave Cold
Plains, the Den of Evil and town), markers drawn on the automap, and travel to the waypoint arrived
on the spot in about two seconds. Several injected clients ran side by side with other copies of the
game.

Teleport mode has not been exercised — the test character has no Teleport — but it uses the same
packet as running with a different command byte.

1.09d was added from its own binaries and has not been played yet. It is the odd one out in a useful
way: its rooms, levels and preset units turned out to be laid out exactly as 1.10's — the D2Common
routines that walk them are identical instruction for instruction — while its UnitAny is not, because
1.09 has no spare word at +0x08 and keeps the path pointer at +0x38 rather than +0x2C. Those unit
fields are now per-patch rather than shared constants. The automap functions, the two draw hooks, the
zoom divisor and the screen offset all sit in the same shape of code as 1.10's and were matched one by
one; the drawing calls are the same two ordinals.

The other five patches have had every address checked against their own DLLs, and where a published
table exists (BH for 1.13c/1.13d, D2BS for 1.12a, d2hackmap for 1.11b) the addresses found
independently agreed with it exactly. They have still not been played with the module loaded. A wrong
offset shows up as `exception ... on the heartbeat` in `%LOCALAPPDATA%\D2FUtil\d2fmh.log`, after
which the module disables itself for that client rather than crashing it; preset-based targets are
dropped separately if the preset list does not decode, leaving exits working.

To report a problem, send both files from `%LOCALAPPDATA%\D2FUtil\`: `d2fmh.log`, written by the
module inside the game, and `launch.log`, written by the launcher. For a deeper look set `"Dump": true`
in `settings.json` first, which makes the module print the game's room structures and outgoing packets.

The module logs what it is running on — the game's path, the Windows build, this module's path — and,
for the first minute, a line every ten seconds saying whether the window is up, whether the heartbeat
is running and whether the game's DLLs have loaded. A log that stops after two lines usually means the
client itself quit; `client NNN closing` says so outright.

**Incomplete installs.** A folder with `Game.exe` but missing DLLs or data archives will not start, or
starts and dies at once with nothing on screen — the same thing a half-copied install looks like from
the outside. The launcher checks the folder for the files the game needs (the `D2*.dll` set, `Fog.dll`,
`Storm.dll` and the core `.mpq` archives) and, if any required one is absent, names them and blocks
Launch until they are restored. Music, speech and movie archives are optional and only noted, not
required. A complete retail or realm copy passes silently.

**Modded folders.** Median XL, Project Diablo 2, PlugY, D2SE and the like look like ordinary installs
from the outside, and a client started from one usually closes itself a second later with nothing on
screen, which looks exactly like a fault here. The launcher names the mod when it recognises one — by a
file only that mod leaves behind, such as `D2Sigma.dll`, `MXL.mpq` or a `FogOriginal.dll` left beside a
replaced `Fog.dll` — and says to use an unmodded copy. Those clients cannot join the realm either.

The heartbeat normally comes from a message hook on the game's thread. One machine refused to install
it at all (`error 87`), which leaves a client doing nothing, so there is a second way in: after ten
seconds of failures the module puts itself in front of the game window's own message handling instead,
which needs nothing unusual from Windows and does the same work.

## How it works

`native\d2fmh.dll` is injected into the suspended client before it runs. It hooks `FindWindow` and
`CreateMutex` so the game's single-instance check never sees another copy, then waits for the game
DLLs, recognizes the patch from `D2Client.dll`'s PE timestamp, and installs a message hook on the
game thread. Every 250 ms on that thread it checks whether the player's level or the current
automap layer changed; if so it walks every room of the level and calls the game's own
`RevealAutomapRoom`, loading rooms the game has not got in memory with D2Common's `AddRoomData`
for the duration of the call (the same method BH / slashdiablo-maphack use on 1.13). The game
draws the cells; nothing is rendered by the DLL.

Everything patch-specific is one table, `native\d2fmh_versions.h`: three D2Client addresses, two
D2Common ordinals and eight struct offsets per patch. The 1.13c/1.13d, 1.12a and 1.11b rows come
from published open-source tables (BH, D2BS 1.1, d2hackmap) and were checked against the stock
DLLs; the 1.11 and 1.10 rows had no published source and were located by matching the same
function bodies into those DLLs with `dumpbin`. 1.10 is the odd one: its `RevealAutomapRoom` is
`__fastcall` and its DRLG structures are laid out differently from every later patch.

## Building

Needs the VS 2022 Build Tools (C++ x86) and the .NET 8 SDK.

    .\build.ps1          # d2fmh.dll then D2FUtil\publish\D2FUtil.exe (win-x86, self-contained)
    .\build.ps1 -DllOnly

The launcher is deliberately **32-bit**: injecting a 32-bit DLL with `CreateRemoteThread` is only
reliable from a 32-bit process, which is why D2F (x64) needed a separate `d2launch.exe` for this.

## Adding a patch

Add a row to `d2fmh_versions.h` (and the timestamp to `GameInstall.Known` for the UI). Find the
addresses by disassembling that patch's `D2Client.dll` / `D2Common.dll` and matching the bodies of
`RevealAutomapRoom` (`test eax,20000h` … `add ebx,30h`), `AddRoomData` / `RemoveRoomData` (the
5-argument stdcall pair that null-checks the Act and reads `Room1->pRoom2`), and the player
getter (`mov eax,[PlayerUnit]; … cmp edx,11h; sete cl`).

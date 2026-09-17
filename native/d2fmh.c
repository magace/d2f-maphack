// d2fmh.dll (32-bit) — the D2F Utilities in-game module. Injected into a freshly started, still
// suspended Diablo II client by D2FUtil.exe. It does three things:
//
//   1. MULTI-INSTANCE. Diablo II refuses to start a second copy. The check is FindWindow("Diablo II")
//      on the newer patches and (belt and braces) a named mutex on some builds. Both are hooked here
//      so every injected client believes it is the only one. Same trick D2Bot/D2BS/d2multi use.
//
//   2. MAPHACK (reveal the level you are in). Walks every room of the current level and hands it to
//      the game's own RevealAutomapRoom, loading rooms the game has not got in memory with D2Common's
//      AddRoomData for the call and unloading them again, the way BH / slashdiablo-maphack do it.
//      Nothing is drawn by us; the game draws its own automap cells.
//
//   3. TRAVEL. Finds the level's exits, waypoint and a few notable places, marks them on the automap,
//      and walks or teleports the character to whichever one you pick with a hotkey.
//
// EVERYTHING RUNS ON THE GAME'S OWN THREAD. A WH_GETMESSAGE hook on the game thread gives a heartbeat
// there; calling the game's functions from a foreign thread is how maphacks corrupt memory, so we
// never do. Work per beat is bounded and wrapped in SEH: a bad offset on a patch nobody has played
// yet disables the feature and logs, rather than taking the game down.
//
// Everything version-specific lives in d2fmh_versions.h; the patch is recognised from D2Client.dll's
// PE timestamp, so an unknown patch simply logs "unsupported" and only the multi-instance part runs.
//
// Config: d2fmh.ini in %LOCALAPPDATA%\D2FUtil (written by the launcher). Log: d2fmh.log beside it.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include "d2fmh_versions.h"

#pragma comment(lib, "user32.lib")

// ---------------------------------------------------------------- state
static HMODULE g_self;
static char g_dir[MAX_PATH];
static FILE* g_log;
static BOOL g_logEnabled = TRUE;
static volatile LONG g_maphackOn = 1;
static BOOL g_dump;                 // diagnostic structure / packet dump
static BOOL g_travelEnabled = TRUE;
static BOOL g_markTargets = TRUE;
static int  g_moveMode;             // 0 run, 1 walk, 2 teleport
static BOOL g_overlayOn;            // lines and labels drawn over the automap (set from the ini)
static BOOL g_autoPotion;           // drink from the belt when life or mana runs low
static int  g_lifePct = 50, g_manaPct = 30, g_rejuvPct = 25;

// Defaults avoid the keys Diablo II already uses. F5 and F6 are skill slots in game, so those two are
// consumed by us rather than passed on (see OnKey); the travel keys default to navigation keys the
// game does not bind at all: PageDown goes deeper, PageUp goes back, Home is the waypoint, End cycles.
static UINT g_keyToggle = VK_F5, g_keyReveal = VK_F6;
static UINT g_keyNext = VK_NEXT, g_keyPrev = VK_PRIOR, g_keyWaypoint = VK_HOME, g_keyCycle = VK_END;

static const D2Version* g_ver;
static BYTE* g_d2client;
static HMODULE g_d2common, g_d2net;

typedef void (__stdcall *AddRoomData_t)(void* act, int levelNo, int x, int y, void* room1);
typedef void (__stdcall *RevealStd_t)(void* room1, DWORD drawAll, void* layer);
typedef void (__fastcall *RevealFast_t)(void* room1, DWORD drawAll, void* layer);
typedef void* (__fastcall *NewCell_t)(void);
typedef void (__fastcall *AddCell_t)(void* cell, void** list);
typedef void (__stdcall *SendNew_t)(size_t len, DWORD arg, BYTE* buf);   // 1.11+
typedef void (__stdcall *SendOld_t)(DWORD arg, BYTE* buf, size_t len);   // 1.10
typedef void (__stdcall *DrawLine_t)(int x1, int y1, int x2, int y2, DWORD colour, DWORD unused);
typedef void (__fastcall *DrawText_t)(const wchar_t* s, int x, int y, DWORD colour, DWORD unused);

static DrawLine_t g_drawLine;
static DrawText_t g_drawText;
static void* g_origCellDraw;
static void* g_origFrame;
static BOOL g_overlayFailed, g_overlayHooked, g_showMonsters = TRUE, g_showList = TRUE;
static int g_panelX = 12, g_panelBottom = 92;   // where the place list sits above the control bar
#define PANEL_LINE 14
static int g_playerX, g_playerY, g_travelTarget = -1;
static int g_monstersDrawn, g_unitsSeen;
static void* g_curLevel;
static DWORD g_lastTravelStep;

// Collision map state (see the collision section for what these mean).
static DWORD* g_unitTable;          // the game's own monster table, located at run time
static int g_unitTableLen;
static DWORD g_lastSweep;           // throttles the search for it
static int g_collOff = -1;          // discovered offset of the grid pointer inside Room1
static BOOL g_collUsable = TRUE;    // cleared if the grid disagrees with where the character is standing
static BOOL g_useCollision = TRUE;
static DWORD g_collMask = 0x0001;   // bits that mean "cannot stand here"
static void* g_lastColl;            // one-entry cache; hops are near the character
static DWORD g_colExit = 0x84, g_colWaypoint = 0x97, g_colSpecial = 0x0C,
             g_colMonster = 0x5B, g_colActive = 0x9B;
static int g_overlayDX, g_overlayDY;    // pixel nudge for the overlay, from the ini

static AddRoomData_t g_addRoomData, g_removeRoomData;
static void* g_reveal;
static NewCell_t g_newCell;
static AddCell_t g_addCell;
static void* g_sendPacket;

static DWORD g_lastLevel, g_lastLayer;          // what was last revealed
static DWORD g_seenLevel, g_seenLayer;          // what the previous beat observed (settle check)
static DWORD g_markedLevel, g_markedLayer;      // what the target markers were placed for
static DWORD g_prevLevel;                       // the level we came from - "previous area"
static volatile LONG g_forceReveal;
static int g_faults;
static HHOOK g_hook;
static HWND g_hwnd;
static DWORD g_mainTid;
static DWORD g_lastBeat;
#define TIMER_ID 0x0D2F
#define BEAT_MS 250

// ---------------------------------------------------------------- log
static void Log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d [%lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

// ---------------------------------------------------------------- ini
static void ReadIni(void) {
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\d2fmh.ini", g_dir);
    g_maphackOn     = GetPrivateProfileIntA("d2fmh", "maphack", 1, p) ? 1 : 0;
    g_keyToggle     = GetPrivateProfileIntA("d2fmh", "key_toggle", VK_F5, p);
    g_keyReveal     = GetPrivateProfileIntA("d2fmh", "key_reveal", VK_F6, p);
    g_keyNext       = GetPrivateProfileIntA("d2fmh", "key_next", VK_NEXT, p);
    g_keyPrev       = GetPrivateProfileIntA("d2fmh", "key_prev", VK_PRIOR, p);
    g_keyWaypoint   = GetPrivateProfileIntA("d2fmh", "key_waypoint", VK_HOME, p);
    g_keyCycle      = GetPrivateProfileIntA("d2fmh", "key_cycle", VK_END, p);
    g_logEnabled    = GetPrivateProfileIntA("d2fmh", "log", 1, p) != 0;
    g_dump          = GetPrivateProfileIntA("d2fmh", "dump", 0, p) != 0;
    g_travelEnabled = GetPrivateProfileIntA("d2fmh", "travel", 1, p) != 0;
    g_markTargets   = GetPrivateProfileIntA("d2fmh", "mark_targets", 1, p) != 0;
    g_overlayOn     = GetPrivateProfileIntA("d2fmh", "overlay", 1, p) != 0;
    g_showMonsters  = GetPrivateProfileIntA("d2fmh", "show_monsters", 1, p) != 0;
    g_colExit       = GetPrivateProfileIntA("d2fmh", "colour_exit", 0x84, p);
    g_colWaypoint   = GetPrivateProfileIntA("d2fmh", "colour_waypoint", 0x97, p);
    g_colSpecial    = GetPrivateProfileIntA("d2fmh", "colour_place", 0x0C, p);
    g_colMonster    = GetPrivateProfileIntA("d2fmh", "colour_monster", 0x5B, p);
    g_colActive     = GetPrivateProfileIntA("d2fmh", "colour_active", 0x9B, p);
    g_useCollision  = GetPrivateProfileIntA("d2fmh", "use_collision", 1, p) != 0;
    g_collMask      = GetPrivateProfileIntA("d2fmh", "collision_mask", 0x0001, p);
    g_overlayDX     = GetPrivateProfileIntA("d2fmh", "overlay_nudge_x", 0, p);
    g_overlayDY     = GetPrivateProfileIntA("d2fmh", "overlay_nudge_y", 0, p);
    g_showList      = GetPrivateProfileIntA("d2fmh", "show_list", 1, p) != 0;
    g_panelX        = GetPrivateProfileIntA("d2fmh", "list_x", 12, p);
    g_panelBottom   = GetPrivateProfileIntA("d2fmh", "list_bottom", 92, p);
    g_autoPotion    = GetPrivateProfileIntA("d2fmh", "auto_potion", 0, p) != 0;
    g_lifePct       = GetPrivateProfileIntA("d2fmh", "potion_life_pct", 50, p);
    g_manaPct       = GetPrivateProfileIntA("d2fmh", "potion_mana_pct", 30, p);
    g_rejuvPct      = GetPrivateProfileIntA("d2fmh", "potion_rejuv_pct", 25, p);
    g_moveMode      = GetPrivateProfileIntA("d2fmh", "move_mode", 0, p);
}

// ---------------------------------------------------------------- inline hooks
// 5-byte JMP patch on the target's entry; the original is restored around each pass-through call.
typedef struct Hook { FARPROC fn; void* dest; BYTE orig[5]; BOOL on; } Hook;

static void PatchJmp(Hook* h) {
    DWORD old; VirtualProtect((void*)h->fn, 5, PAGE_EXECUTE_READWRITE, &old);
    BYTE p[5]; p[0] = 0xE9; *(DWORD*)(p + 1) = (DWORD)((BYTE*)h->dest - (BYTE*)h->fn - 5);
    memcpy((void*)h->fn, p, 5);
    VirtualProtect((void*)h->fn, 5, old, &old);
}
static void Unpatch(Hook* h) {
    DWORD old; VirtualProtect((void*)h->fn, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)h->fn, h->orig, 5);
    VirtualProtect((void*)h->fn, 5, old, &old);
}
static BOOL InstallAt(Hook* h, FARPROC fn, void* dest) {
    if (!fn) return FALSE;
    h->fn = fn; h->dest = dest;
    memcpy(h->orig, (void*)fn, 5);
    PatchJmp(h); h->on = TRUE;
    return TRUE;
}
static BOOL Install(Hook* h, const char* mod, const char* name, void* dest) {
    HMODULE m = GetModuleHandleA(mod); if (!m) m = LoadLibraryA(mod);
    return InstallAt(h, m ? GetProcAddress(m, name) : NULL, dest);
}

static Hook hFindA, hFindW, hFindExA, hFindExW, hMutexA, hMutexW, hOpenMutexA, hOpenMutexW, hSend;

typedef HWND (WINAPI *FindWindowA_t)(LPCSTR, LPCSTR);
typedef HWND (WINAPI *FindWindowW_t)(LPCWSTR, LPCWSTR);
typedef HWND (WINAPI *FindWindowExA_t)(HWND, HWND, LPCSTR, LPCSTR);
typedef HWND (WINAPI *FindWindowExW_t)(HWND, HWND, LPCWSTR, LPCWSTR);
typedef HANDLE (WINAPI *CreateMutexA_t)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
typedef HANDLE (WINAPI *CreateMutexW_t)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
typedef HANDLE (WINAPI *OpenMutexA_t)(DWORD, BOOL, LPCSTR);
typedef HANDLE (WINAPI *OpenMutexW_t)(DWORD, BOOL, LPCWSTR);

static HWND WINAPI MyFindWindowA(LPCSTR cls, LPCSTR win) {
    if ((cls && _stricmp(cls, "Diablo II") == 0) || (!cls && win && _stricmp(win, "Diablo II") == 0)) return NULL;
    Unpatch(&hFindA); HWND r = ((FindWindowA_t)hFindA.fn)(cls, win); PatchJmp(&hFindA);
    return r;
}
static HWND WINAPI MyFindWindowW(LPCWSTR cls, LPCWSTR win) {
    if ((cls && _wcsicmp(cls, L"Diablo II") == 0) || (!cls && win && _wcsicmp(win, L"Diablo II") == 0)) return NULL;
    Unpatch(&hFindW); HWND r = ((FindWindowW_t)hFindW.fn)(cls, win); PatchJmp(&hFindW);
    return r;
}
// The same two questions asked the other way round. A copy of the game that finds one of these tells
// itself another copy is running and quits - on some builds without a word on screen, which looks
// exactly like a client that died for no reason.
static HWND WINAPI MyFindWindowExA(HWND parent, HWND after, LPCSTR cls, LPCSTR win) {
    if ((cls && _stricmp(cls, "Diablo II") == 0) || (!cls && win && _stricmp(win, "Diablo II") == 0)) return NULL;
    Unpatch(&hFindExA); HWND r = ((FindWindowExA_t)hFindExA.fn)(parent, after, cls, win); PatchJmp(&hFindExA);
    return r;
}
static HWND WINAPI MyFindWindowExW(HWND parent, HWND after, LPCWSTR cls, LPCWSTR win) {
    if ((cls && _wcsicmp(cls, L"Diablo II") == 0) || (!cls && win && _wcsicmp(win, L"Diablo II") == 0)) return NULL;
    Unpatch(&hFindExW); HWND r = ((FindWindowExW_t)hFindExW.fn)(parent, after, cls, win); PatchJmp(&hFindExW);
    return r;
}
static HANDLE WINAPI MyOpenMutexA(DWORD access, BOOL inherit, LPCSTR name) {
    char buf[300]; LPCSTR use = name;
    if (name && strstr(name, "DIABLO II")) { snprintf(buf, sizeof buf, "D2F_%lu_%s", GetCurrentProcessId(), name); use = buf; }
    Unpatch(&hOpenMutexA); HANDLE r = ((OpenMutexA_t)hOpenMutexA.fn)(access, inherit, use); PatchJmp(&hOpenMutexA);
    return r;
}
static HANDLE WINAPI MyOpenMutexW(DWORD access, BOOL inherit, LPCWSTR name) {
    wchar_t buf[300]; LPCWSTR use = name;
    if (name && wcsstr(name, L"DIABLO II")) { _snwprintf(buf, 300, L"D2F_%lu_%s", GetCurrentProcessId(), name); use = buf; }
    Unpatch(&hOpenMutexW); HANDLE r = ((OpenMutexW_t)hOpenMutexW.fn)(access, inherit, use); PatchJmp(&hOpenMutexW);
    return r;
}

static HANDLE WINAPI MyCreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCSTR name) {
    char buf[300]; LPCSTR use = name;
    if (name && strstr(name, "DIABLO II")) { snprintf(buf, sizeof buf, "D2F_%lu_%s", GetCurrentProcessId(), name); use = buf; }
    Unpatch(&hMutexA); HANDLE r = ((CreateMutexA_t)hMutexA.fn)(sa, own, use); PatchJmp(&hMutexA);
    return r;
}
static HANDLE WINAPI MyCreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCWSTR name) {
    wchar_t buf[300]; LPCWSTR use = name;
    if (name && wcsstr(name, L"DIABLO II")) { _snwprintf(buf, 300, L"D2F_%lu_%s", GetCurrentProcessId(), name); use = buf; }
    Unpatch(&hMutexW); HANDLE r = ((CreateMutexW_t)hMutexW.fn)(sa, own, use); PatchJmp(&hMutexW);
    return r;
}

// Diagnostic only (dump=1): log what the game itself sends, which is how the exact byte layout of the
// movement commands was established instead of guessing it from second-hand packet tables.
static int g_packetsLogged;

// Auto potion learns the game's own "use this item" packet by watching what the client sends when a
// potion is drunk, rather than guessing a layout out of a packet table. The last few outgoing packets
// are kept here for that; our own sends are skipped so it can never learn from itself.
#define PKT_RING 16
typedef struct { BYTE buf[32]; int len; DWORD when; } PktRec;
static PktRec g_ring[PKT_RING];
static int    g_ringN;
static BOOL   g_ourSend;

static void RememberPacket(const BYTE* b, size_t len) {
    if (g_ourSend || len < 5 || len > 32) return;
    PktRec* r = &g_ring[g_ringN++ & (PKT_RING - 1)];
    memcpy(r->buf, b, len);
    r->len = (int)len;
    r->when = GetTickCount();
}
static void LogPacket(const char* why, const BYTE* buf, size_t len) {
    char line[160]; int n = sprintf(line, "%s len=%u:", why, (unsigned)len);
    for (size_t i = 0; i < len && i < 24; i++) n += sprintf(line + n, " %02X", buf[i]);
    Log("%s", line);
}
static void __stdcall MySendNew(size_t len, DWORD arg, BYTE* buf) {
    if (buf && len) RememberPacket(buf, len);
    if (g_dump && g_packetsLogged < 60 && buf && len) { g_packetsLogged++; LogPacket("send", buf, len); }
    Unpatch(&hSend); ((SendNew_t)hSend.fn)(len, arg, buf); PatchJmp(&hSend);
}
static void __stdcall MySendOld(DWORD arg, BYTE* buf, size_t len) {
    if (buf && len) RememberPacket(buf, len);
    if (g_dump && g_packetsLogged < 60 && buf && len) { g_packetsLogged++; LogPacket("send", buf, len); }
    Unpatch(&hSend); ((SendOld_t)hSend.fn)(arg, buf, len); PatchJmp(&hSend);
}

// ---------------------------------------------------------------- memory helpers
#define RD(base, off) (*(DWORD*)((BYTE*)(base) + (off)))
#define RW(base, off) (*(WORD*)((BYTE*)(base) + (off)))
#define RP(base, off) (*(void**)((BYTE*)(base) + (off)))

// True when n bytes at p are committed and readable. Used before following any pointer derived from a
// patch's offset table, so a wrong offset logs instead of crashing the game.
static BOOL Readable(const void* p, SIZE_T n) {
    MEMORY_BASIC_INFORMATION mbi;
    if ((UINT_PTR)p < 0x10000) return FALSE;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return FALSE;
    if (mbi.State != MEM_COMMIT) return FALSE;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return FALSE;
    return (const BYTE*)p + n <= (const BYTE*)mbi.BaseAddress + mbi.RegionSize;
}

// ---------------------------------------------------------------- version
static DWORD PeTimeStamp(HMODULE m) {
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)m;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)m + dos->e_lfanew);
    return nt->FileHeader.TimeDateStamp;
}

// The game DLLs are not there from the start: on 1.10/1.11 Game.exe only loads D2Client.dll and
// D2Common.dll when you actually enter a game, and they can be unloaded again between games. So this
// runs on every heartbeat and re-resolves whenever the modules appear, move, or vanish.
static HMODULE g_seenClient, g_seenCommon;
static DWORD g_unknownStamp;
static void InstallDrawHook(const D2Version* v);     // defined with the overlay, below

static BOOL EnsureResolved(void) {
    HMODULE c = GetModuleHandleA("D2Client.dll");
    HMODULE m = GetModuleHandleA("D2Common.dll");
    if (!c || !m) {
        if (g_seenClient) {
            Log("game DLLs unloaded (between games)");
            g_seenClient = g_seenCommon = NULL; g_ver = NULL; g_d2client = NULL;
            g_lastLevel = g_seenLevel = g_markedLevel = g_prevLevel = 0;
            g_collOff = -1; g_collUsable = TRUE; g_lastColl = NULL;
            g_unitTable = NULL; g_unitTableLen = 0; g_lastSweep = 0;   // a new game means a new search
            if (hSend.on) { Unpatch(&hSend); hSend.on = FALSE; }
        }
        return FALSE;
    }
    if (c == g_seenClient && m == g_seenCommon) return g_ver != NULL;
    g_seenClient = c; g_seenCommon = m; g_ver = NULL; g_d2client = NULL;
    g_lastLevel = g_seenLevel = g_markedLevel = 0;

    DWORD stamp = PeTimeStamp(c);
    const D2Version* v = NULL;
    for (size_t i = 0; i < NUM_VERSIONS; i++)
        if (g_versions[i].d2clientTimeStamp == stamp) v = &g_versions[i];
    if (!v) {
        if (stamp != g_unknownStamp) { g_unknownStamp = stamp; Log("D2Client.dll timestamp %08X is not a patch this build knows - maphack off, multi-instance only", stamp); }
        return FALSE;
    }
    AddRoomData_t add = (AddRoomData_t)GetProcAddress(m, (LPCSTR)(UINT_PTR)v->ordAddRoomData);
    AddRoomData_t rem = (AddRoomData_t)GetProcAddress(m, (LPCSTR)(UINT_PTR)v->ordRemoveRoomData);
    if (!add || !rem) { Log("patch %s: D2Common ordinals %u/%u not found - maphack off", v->name, v->ordAddRoomData, v->ordRemoveRoomData); return FALSE; }

    g_d2client = (BYTE*)c; g_d2common = m; g_addRoomData = add; g_removeRoomData = rem;
    g_reveal  = g_d2client + v->revealAutomapRoom;
    g_newCell = (NewCell_t)(g_d2client + v->newAutomapCell);
    g_addCell = (AddCell_t)(g_d2client + v->addAutomapCell);

    g_d2net = GetModuleHandleA("D2Net.dll");
    g_sendPacket = g_d2net ? (void*)GetProcAddress(g_d2net, (LPCSTR)(UINT_PTR)v->ordSendPacket) : NULL;
    if (!g_sendPacket) Log("patch %s: D2Net ordinal %u not found - travel disabled", v->name, v->ordSendPacket);
    else if ((g_dump || g_autoPotion) && !hSend.on)
        InstallAt(&hSend, (FARPROC)g_sendPacket, v->sendLenFirst ? (void*)MySendNew : (void*)MySendOld);

    // Overlay: the game's own line and text routines, plus the once-per-frame slot to call them from.
    HMODULE gfx = GetModuleHandleA("D2gfx.dll"), win = GetModuleHandleA("D2Win.dll");
    g_drawLine = (v->ordDrawLine && gfx) ? (DrawLine_t)GetProcAddress(gfx, (LPCSTR)(UINT_PTR)v->ordDrawLine) : NULL;
    g_drawText = (v->ordDrawText && win) ? (DrawText_t)GetProcAddress(win, (LPCSTR)(UINT_PTR)v->ordDrawText) : NULL;
    g_overlayFailed = FALSE;
    g_ver = v;
    InstallDrawHook(v);

    Log("patch %s: D2Client @%p D2Common @%p D2Net @%p  reveal=%p send=%p overlay=%s",
        v->name, c, m, g_d2net, g_reveal, g_sendPacket,
        !v->drawHookCall ? "not supported on this patch" : g_overlayHooked ? "on" : "unavailable");
    return TRUE;
}

// ---------------------------------------------------------------- targets
typedef enum { TK_EXIT, TK_WAYPOINT, TK_SPECIAL } TargetKind;
typedef struct {
    TargetKind kind;
    int levelNo;        // for an exit: the level it leads to
    int txt;            // for a waypoint / special: the object number
    int x, y;           // world subtile coordinates
} Target;

#define MAX_TARGETS 48
static Target g_targets[MAX_TARGETS];
static int g_numTargets;
static int g_cycle;                 // index used by the "cycle" key
static BOOL g_presetsSane = TRUE;   // cleared when a patch's preset offsets look wrong

static int g_buildPx, g_buildPy;      // where the character was when the targets were worked out

static int DistFrom(int x, int y, int px, int py) {
    int dx = x - px, dy = y - py;
    return dx * dx + dy * dy;
}

static void AddTarget(TargetKind kind, int levelNo, int txt, int x, int y) {
    for (int i = 0; i < g_numTargets; i++) {
        Target* t = &g_targets[i];
        // One target per destination level, and one per distinct place otherwise.
        if (t->kind != kind) continue;
        if (kind == TK_EXIT ? t->levelNo != levelNo : t->txt != txt) continue;
        // A level border runs along several rooms, so the same destination is reached from more than
        // one place. Keep the nearest: picking whichever room came first in the list put the marker at
        // an arbitrary point along the border, sometimes most of a level away from the obvious crossing.
        if (DistFrom(x, y, g_buildPx, g_buildPy) < DistFrom(t->x, t->y, g_buildPx, g_buildPy)) {
            t->x = x; t->y = y;
        }
        return;
    }
    if (g_numTargets >= MAX_TARGETS) return;
    Target* t = &g_targets[g_numTargets++];
    t->kind = kind; t->levelNo = levelNo; t->txt = txt; t->x = x; t->y = y;
}

static int IsWaypoint(int txt) {
    for (size_t i = 0; i < sizeof kWaypointObjects / sizeof kWaypointObjects[0]; i++)
        if (kWaypointObjects[i] == txt) return 1;
    return 0;
}
static const char* SpecialName(int txt) {
    for (size_t i = 0; i < NUM_SPECIALS; i++) if (kSpecialObjects[i].txt == txt) return kSpecialObjects[i].name;
    return NULL;
}

// Walk one room's preset list, collecting waypoints and notable objects, and reporting whether the
// list looked like preset units at all. A patch whose preset offsets are wrong produces nonsense
// types and is switched off rather than trusted.
static void CollectPresets(void* room2, int roomX, int roomY, int* sane, int* total) {
    const D2Version* v = g_ver;
    int n = 0;
    for (void* p = RP(room2, v->room2_pPreset); Readable(p, 0x24) && n < 300; p = RP(p, v->preset_next), n++) {
        DWORD type = RD(p, v->preset_type), txt = RD(p, v->preset_txt);
        DWORD px = RD(p, v->preset_x), py = RD(p, v->preset_y);
        (*total)++;
        if ((type == PRESET_MONSTER || type == PRESET_OBJECT || type == PRESET_WARP) && txt < 1000 && px < 256 && py < 256)
            (*sane)++;
        else
            continue;
        if (type != PRESET_OBJECT) continue;
        int wx = roomX * 5 + (int)px, wy = roomY * 5 + (int)py;
        if (IsWaypoint((int)txt)) AddTarget(TK_WAYPOINT, 0, (int)txt, wx, wy);
        else if (SpecialName((int)txt)) AddTarget(TK_SPECIAL, 0, (int)txt, wx, wy);
    }
}

// Build the target list for the level the player is standing in.
//
// EXITS COME FROM THE ROOM GRAPH, NOT FROM ROOM TILES. Every room lists the rooms adjacent to it, and
// where a level border runs some of those belong to a different level - for a seamless border (town to
// Blood Moor) and for a warp (the Den of Evil stairs) alike. Taking the exits from that list means the
// feature needs two offsets rather than a whole room-tile structure, and it was verified in game.
//
// For a seamless border the neighbouring room is right next door, so we aim at it and the character
// walks across. For a warp the neighbour is somewhere else entirely on the map, so we instead aim at
// our own room, which is where the stairs are.
static void BuildTargets(void* level, DWORD levelNo, int px, int py) {
    const D2Version* v = g_ver;
    g_numTargets = 0; g_cycle = 0;
    g_buildPx = px; g_buildPy = py;
    int sane = 0, total = 0;

    int rooms = 0;
    for (void* r = RP(level, v->level_pRoom2First); Readable(r, 0x100) && rooms < 400; r = RP(r, v->room2_pNext), rooms++) {
        int rx = (int)RD(r, v->room2_posX), ry = (int)RD(r, v->room2_posY);
        int sx = (int)RD(r, v->room2_sizeX), sy = (int)RD(r, v->room2_sizeY);
        if (sx <= 0 || sx > 64 || sy <= 0 || sy > 64) continue;

        DWORD cnt = RD(r, v->room2_numRoomsNear);
        void** nbrs = (void**)RP(r, v->room2_pRoomsNear);
        if (cnt <= 64 && Readable(nbrs, cnt * 4)) {
            for (DWORD i = 0; i < cnt; i++) {
                void* nb = nbrs[i];
                if (!Readable(nb, 0x100)) continue;
                void* nl = RP(nb, v->room2_pLevel);
                if (!Readable(nl, 0x210)) continue;
                DWORD nlevel = RD(nl, v->level_levelNo);
                if (nlevel == levelNo || nlevel < 1 || nlevel > 200) continue;

                int nx = (int)RD(nb, v->room2_posX), ny = (int)RD(nb, v->room2_posY);
                int nsx = (int)RD(nb, v->room2_sizeX), nsy = (int)RD(nb, v->room2_sizeY);
                // Where the two rooms touch is the actual crossing. The overlap of their extents is a
                // thin strip along the shared edge; its midpoint is the doorway. Aiming at the
                // neighbour's CENTRE instead put the marker half a room past the border, which in Act 3's
                // large jungle rooms is far from where you cross - the lines "did not match up". A gap of
                // up to two tiles still counts as touching, for rooms that abut without exactly meeting.
                int ox0 = rx > nx ? rx : nx, ox1 = (rx + sx < nx + nsx ? rx + sx : nx + nsx);
                int oy0 = ry > ny ? ry : ny, oy1 = (ry + sy < ny + nsy ? ry + sy : ny + nsy);
                int touches = (ox1 >= ox0 - 2) && (oy1 >= oy0 - 2);
                int tx, ty;
                if (touches) { tx = (ox0 + ox1) * 5 / 2; ty = (oy0 + oy1) * 5 / 2; }   // the shared edge
                else         { tx = rx * 5 + sx * 5 / 2; ty = ry * 5 + sy * 5 / 2; }   // a warp: aim at our room
                AddTarget(TK_EXIT, (int)nlevel, 0, tx, ty);
            }
        }
        if (g_presetsSane) CollectPresets(r, rx, ry, &sane, &total);
    }

    if (total > 8 && sane * 5 < total * 4) {       // fewer than 80% of entries looked like preset units
        g_presetsSane = FALSE;
        Log("patch %s: preset units do not decode (%d of %d sane) - waypoint and place targets disabled",
            g_ver->name, sane, total);
    }
    Log("level %lu: %d rooms, %d targets%s", levelNo, rooms, g_numTargets, g_presetsSane ? "" : " (exits only)");
    for (int i = 0; i < g_numTargets; i++) {
        Target* t = &g_targets[i];
        const char* nm = t->kind == TK_EXIT ? "exit" : t->kind == TK_WAYPOINT ? "waypoint" : SpecialName(t->txt);
        Log("  target[%d] %s%s%d at %d,%d (player %d,%d)", i, nm ? nm : "place",
            t->kind == TK_EXIT ? " to level " : " #", t->kind == TK_EXIT ? t->levelNo : t->txt, t->x, t->y, px, py);
    }
}

// ---------------------------------------------------------------- automap markers
// Adds a cell to the game's own automap layer, the same call the game uses for the icons it draws
// itself, so a marker looks native and is drawn, panned and zoomed by the game.
static void MarkCell(void* layer, int cellNo, int x, int y) {
    void* cell = g_newCell();
    if (!Readable(cell, 0x14)) return;
    *(WORD*)((BYTE*)cell + 0x04) = (WORD)cellNo;
    *(WORD*)((BYTE*)cell + 0x06) = (WORD)(((x - y) * 16) / 10 + 1);
    *(WORD*)((BYTE*)cell + 0x08) = (WORD)(((y + x) * 8) / 10 - 3);
    g_addCell(cell, (void**)((BYTE*)layer + LAYER_pObjects));
}

static void MarkTargets(void* layer) {
    for (int i = 0; i < g_numTargets; i++) {
        Target* t = &g_targets[i];
        int cell = t->kind == TK_EXIT ? CELL_EXIT : t->kind == TK_WAYPOINT ? CELL_WAYPOINT : CELL_SPECIAL;
        MarkCell(layer, cell, t->x, t->y);
    }
    Log("marked %d target(s) on the automap", g_numTargets);
}

// ---------------------------------------------------------------- life and mana
// Reading these needs the character's stat list. Rather than carry the offsets per patch - the source
// of most of the trouble so far - the shape of the data is used to find them: a stat list holds an
// array of eight-byte entries, and among them are the well-known numbers for life and maximum life.
// Nothing else reachable from a unit looks like that, and the values it yields are checked for sanity
// before being believed.
#define STAT_LIFE      6
#define STAT_MAX_LIFE  7
#define STAT_MANA      8
#define STAT_MAX_MANA  9
#define STAT_ENTRY     8        // bytes per entry: sub-index, index, value
#define STAT_SCAN      96       // entries to look at

static int g_statsOff = -1, g_statArrOff = -1;

static BOOL PlausiblePtr(const void* p) {
    return (UINT_PTR)p >= 0x10000 && (UINT_PTR)p < 0x7FF00000 && !((UINT_PTR)p & 3);
}

// Life and mana are stored shifted up by eight bits, so a raw value of 2560 means 10.
static BOOL ScanStats(BYTE* arr, int* life, int* maxLife, int* mana, int* maxMana) {
    int got = 0;
    for (int i = 0; i < STAT_SCAN; i++) {
        WORD id = *(WORD*)(arr + i * STAT_ENTRY + 2);
        DWORD val = *(DWORD*)(arr + i * STAT_ENTRY + 4);
        if (id > 600) break;
        int v = (int)(val >> 8);
        if (id == STAT_LIFE)     { *life = v;    got |= 1; }
        if (id == STAT_MAX_LIFE) { *maxLife = v; got |= 2; }
        if (id == STAT_MANA)     { *mana = v;    got |= 4; }
        if (id == STAT_MAX_MANA) { *maxMana = v; got |= 8; }
    }
    return (got & 3) == 3;          // life and maximum life are the ones that matter
}

static BOOL ReadVitalsAt(void* unit, int* life, int* maxLife, int* mana, int* maxMana) {
    __try {
        {
            BYTE* sl = *(BYTE**)((BYTE*)unit + g_statsOff);
            if (!PlausiblePtr(sl)) return FALSE;
            BYTE* arr = *(BYTE**)(sl + g_statArrOff);
            if (!PlausiblePtr(arr)) return FALSE;
            return ScanStats(arr, life, maxLife, mana, maxMana) && *maxLife > 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_statsOff = -1; return FALSE; }
    return FALSE;
}

// The search for it, kept apart from the reading so that ONE unreadable probe cannot end the whole
// search. It used to share the reader's handler: a single field on the character that pointed at a
// block ending on a page boundary threw, the search was abandoned before it ever reached the stat
// list, and auto potion then sat silent for the rest of the game with nothing in the log to say why.
static BOOL TryScan(BYTE* arr, int* life, int* maxLife, int* mana, int* maxMana) {
    if (!Readable(arr, STAT_ENTRY * 8)) return FALSE;
    __try {
        if (!ScanStats(arr, life, maxLife, mana, maxMana)) return FALSE;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
    return *maxLife >= 1 && *maxLife <= 20000 && *life <= *maxLife + 1;
}

static BOOL FindVitals(void* unit, int* life, int* maxLife, int* mana, int* maxMana) {
    for (int f = 0x30; f <= 0x90; f += 4) {
        BYTE* sl;
        if (!Readable((BYTE*)unit + f, 4)) continue;
        sl = *(BYTE**)((BYTE*)unit + f);
        if (!PlausiblePtr(sl)) continue;
        for (int g = 0; g <= 0x40; g += 4) {
            BYTE* arr;
            if (!Readable(sl + g, 4)) continue;
            arr = *(BYTE**)(sl + g);
            if (!PlausiblePtr(arr)) continue;
            if (!TryScan(arr, life, maxLife, mana, maxMana)) continue;
            g_statsOff = f; g_statArrOff = g;
            Log("vitals: stat list at Unit+%02X, entries at +%02X (life %d/%d, mana %d/%d)",
                f, g, *life, *maxLife, *mana, *maxMana);
            return TRUE;
        }
    }
    return FALSE;
}

// The last good reading, kept for the panel to show. Seeing the numbers on screen is also how you can
// tell at a glance whether auto potion can read the character at all.
static int  g_vitLife, g_vitMax, g_vitMana, g_vitManaMax;
static BOOL g_saidNoVitals;
static DWORD g_lastVitalSearch;

// The 1.08 / 1.09 stat list, read directly and read only. Its shape came from D2Common's stat setter:
// the list at unit+0x6C carries a working copy at +8, whose entries live in blocks starting at +0x4C -
// a count, then up to fifteen (id, value) pairs, then a pointer to the next block at +0x7C. Values are
// stored shifted up eight bits, as on every patch. If the walk does not turn up life and maximum life
// the first few pairs it did see are logged once, which is enough to correct the shape from a log.
#define OLD_STAT_BLOCK   0x4C
#define OLD_STAT_NEXT    0x7C
#define OLD_STAT_PER     15

static int g_bestMaxLife, g_bestMaxMana;   // the largest sensible maximum seen, to fill a frame that misses it

static BOOL ReadVitalsOld(void* unit, int* life, int* maxLife, int* mana, int* maxMana) {
    static BOOL said;
    int got = 0;
    DWORD seenId[6], seenVal[6]; int seen = 0;
    __try {
        BYTE* stats = *(BYTE**)((BYTE*)unit + g_ver->unit_pStats);
        if (!PlausiblePtr(stats)) return FALSE;
        BYTE* block = stats + 8 + OLD_STAT_BLOCK;
        for (int guard = 0; guard < 32 && PlausiblePtr(block); guard++) {
            DWORD n = *(DWORD*)block;
            if (n > OLD_STAT_PER) break;
            for (DWORD i = 0; i < n; i++) {
                DWORD id = *(DWORD*)(block + 4 + i * 8), val = *(DWORD*)(block + 8 + i * 8);
                if (seen < 6) { seenId[seen] = id; seenVal[seen] = val; seen++; }
                int v = (int)(val >> 8);
                // FIRST occurrence wins. The stat list is layered: the primary block holds the real
                // current value, and a later item/skill layer can carry the same id with 0 - taking the
                // last match read current life as 0 and the orb showed empty on a living character.
                if (id == STAT_LIFE     && !(got & 1)) { *life = v;    got |= 1; }
                if (id == STAT_MAX_LIFE && !(got & 2)) { *maxLife = v; got |= 2; }
                if (id == STAT_MANA     && !(got & 4)) { *mana = v;    got |= 4; }
                if (id == STAT_MAX_MANA && !(got & 8)) { *maxMana = v; got |= 8; }
            }
            block = *(BYTE**)(block + OLD_STAT_NEXT);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }

    // Current life is the value that must be read; the maximum is nice to have. A maximum that reads as
    // the base without item bonuses (so lower than current) or is missing this frame is filled from the
    // largest sensible maximum seen so far, so the panel and the drink threshold stay steady.
    if ((got & 2) && *maxLife >= *life && *maxLife <= 20000 && *maxLife > g_bestMaxLife) g_bestMaxLife = *maxLife;
    if ((got & 8) && *maxMana >= *mana && *maxMana <= 40000 && *maxMana > g_bestMaxMana) g_bestMaxMana = *maxMana;
    if (*maxLife < *life && g_bestMaxLife >= *life) *maxLife = g_bestMaxLife;
    if (*maxMana < *mana && g_bestMaxMana >= *mana) *maxMana = g_bestMaxMana;

    BOOL ok = (got & 1) && *life >= 0 && *life <= 20000 && *maxLife >= 1;
    if (!said) {
        said = TRUE;
        if (ok) Log("vitals: read from the 1.09-style stat list (life %d/%d, mana %d/%d)", *life, *maxLife, *mana, *maxMana);
        else {
            Log("vitals: the 1.09-style stat list did not yield life - first pairs seen:");
            for (int i = 0; i < seen; i++) Log("  stat %lu = %lu (>>8: %lu)", seenId[i], seenVal[i], seenVal[i] >> 8);
        }
    }
    return ok;
}

static BOOL ReadVitals(void* unit, int* life, int* maxLife, int* mana, int* maxMana) {
    *life = *maxLife = *mana = *maxMana = 0;
    BOOL ok;
    if (g_ver->unit_pStats) {
        ok = ReadVitalsOld(unit, life, maxLife, mana, maxMana);
    } else if (g_statsOff >= 0) {
        ok = ReadVitalsAt(unit, life, maxLife, mana, maxMana);
        if (!ok) g_statsOff = g_statArrOff = -1;      // the offsets stopped working: look again
    } else {
        DWORD now = GetTickCount();
        if (now - g_lastVitalSearch < 2000) return FALSE;   // the search is thorough, not cheap
        g_lastVitalSearch = now;
        ok = FindVitals(unit, life, maxLife, mana, maxMana);
        if (!ok && !g_saidNoVitals) {
            g_saidNoVitals = TRUE;
            Log("vitals: life and mana cannot be read on this character - auto potion is asleep");
        }
    }
    // Current can never exceed the true maximum, so if the maximum reads lower than the current value
    // it is the base stat without the +life/+mana an item adds, and the current value is the better
    // estimate of the real maximum. Clamping here keeps the panel from showing 163% and keeps the
    // drink-below percentage honest, on 1.08 where only the base maximum is in easy reach.
    if (ok) {
        if (*maxLife < *life) *maxLife = *life;
        if (*maxMana < *mana) *maxMana = *mana;
        g_vitLife = *life; g_vitMax = *maxLife; g_vitMana = *mana; g_vitManaMax = *maxMana;
    }
    return ok;
}

// ---------------------------------------------------------------- items
// Auto potion needs to know WHAT a potion is, not which key it sits under. Belts are not organised
// into a healing column and a mana column - potions land wherever there is room - so a column is not
// a kind, and learning columns is how it ended up drinking mana when it wanted health.
//
// So the character's items are read instead. Every potion is remembered by its item type, learned
// from what drinking one actually did, and that knowledge is written to disk: the first potion of a
// kind you ever drink teaches it, every one after that is chosen correctly, and inventory potions of
// a kind already met from the belt can be used too.
//
// Nothing here is a per-patch number. Where the inventory hangs off the character, what links the
// items together, where a position is kept and which byte marks an item as being in the belt are all
// worked out from the data itself, the same way the rooms and the stat list were.
#define UNIT_TYPE_ITEM 4
#define MAX_ITEMS      96
#define ITEMDATA_SCAN  0x80      // bytes of an item's data kept, to search for the belt marker in

typedef struct {
    void* unit;
    void* data;
    DWORD id, txt;
    int   x, y;                  // position within whatever holds it; -1 if not readable
    BOOL  belt;
    BYTE  d[ITEMDATA_SCAN];
} Item;

static Item g_items[MAX_ITEMS], g_snap[MAX_ITEMS];
static int  g_nItems, g_nSnap;
static int  g_invOff = -1, g_firstOff = -1, g_nextOff = -1;
static BOOL g_nextInData;
static int  g_posOff = -1;
static int  g_locOff = -1;       // byte in the item's data that says where it is kept
static BYTE g_locBelt;           // the value that byte has for a belt item
static BOOL g_beltRowInY;        // belt position: a slot number in x, or column in x and row in y
static BYTE g_locCand[ITEMDATA_SCAN];   // bytes that could still be the one marking the belt
static BOOL g_locInit;
static DWORD g_lastItemLog;

static BOOL CopyBytes(const void* src, BYTE* dst, int n) {
    __try { memcpy(dst, src, n); return TRUE; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
}

static BOOL IsItemUnit(void* u) {
    if (!PlausiblePtr(u)) return FALSE;
    __try {
        if (RD(u, 0x00) != UNIT_TYPE_ITEM) return FALSE;    // unit type: item
        if (RD(u, 0x04) > 1000) return FALSE;               // item type number
        if (!PlausiblePtr(RP(u, UNIT_pItemData))) return FALSE;
        return RD(u, UNIT_dwUnitId) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
}

// Follow a candidate "next item" link from the first item and count how many real items it strings
// together. The right offset is the one that reaches the most, the same test that found the room
// chain; a wrong one stops at the first thing that is not an item.
static int ChainLen(void* first, int off, BOOL inData) {
    void* seen[MAX_ITEMS]; int n = 0; void* u = first;
    while (u && n < MAX_ITEMS) {
        if (!IsItemUnit(u)) break;
        for (int i = 0; i < n; i++) if (seen[i] == u) return n;      // a loop: stop counting
        seen[n++] = u;
        void* nx = NULL;
        __try {
            void* base = inData ? RP(u, UNIT_pItemData) : u;
            if (PlausiblePtr(base)) nx = RP(base, off);
        } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (nx && !PlausiblePtr(nx)) break;
        u = nx;
    }
    return n;
}

static BOOL FindItemChain(void* player) {
    if (g_nextOff >= 0) return TRUE;
    int best = 0, bf = -1, bg = -1, bn = -1; BOOL bd = FALSE;
    __try {
        for (int f = 0x50; f <= 0x88; f += 4) {
            void* inv = RP(player, f);
            if (!PlausiblePtr(inv)) continue;
            for (int g = 0; g <= 0x20; g += 4) {
                void* first = RP(inv, g);
                if (!IsItemUnit(first)) continue;
                if (g_invOff < 0) { g_invOff = f; g_firstOff = g; }
                for (int d = 0; d < 2; d++) {
                    int lo = d ? 0x00 : 0x18, hi = d ? 0x7C : 0xF0;
                    for (int n = lo; n <= hi; n += 4) {
                        int len = ChainLen(first, n, (BOOL)d);
                        if (len > best) { best = len; bf = f; bg = g; bn = n; bd = (BOOL)d; }
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
    if (best < 2 || bf < 0) return g_invOff >= 0;        // one item alone cannot show the link
    g_invOff = bf; g_firstOff = bg; g_nextOff = bn; g_nextInData = bd;
    Log("items: inventory at Unit+%02X, first item at +%02X, next at %s+%02X (%d items)",
        bf, bg, bd ? "data" : "unit", bn, best);
    return TRUE;
}

static BOOL ItemPos(void* unit, int off, int* x, int* y) {
    __try {
        void* path = RP(unit, UNIT_pPath);
        if (!PlausiblePtr(path)) return FALSE;
        DWORD px = RD(path, off), py = RD(path, off + 4);
        if (px > 20 || py > 20) return FALSE;
        *x = (int)px; *y = (int)py;
        return TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
}

// Which pair of words in an item's path is its position: the pair that gives every item small numbers
// and tells the most of them apart. A pair that is really something else reads as nonsense or reads
// the same for everything.
static void ReadPositions(void) {
    if (g_posOff < 0 && g_nItems >= 3) {
        int best = 1, bo = -1;
        for (int o = 0; o <= 0x18; o += 4) {
            int xs[MAX_ITEMS], ys[MAX_ITEMS], distinct = 0; BOOL ok = TRUE;
            for (int i = 0; i < g_nItems; i++)
                if (!ItemPos(g_items[i].unit, o, &xs[i], &ys[i])) { ok = FALSE; break; }
            if (!ok) continue;
            for (int i = 0; i < g_nItems; i++) {
                BOOL dup = FALSE;
                for (int j = 0; j < i; j++) if (xs[j] == xs[i] && ys[j] == ys[i]) dup = TRUE;
                if (!dup) distinct++;
            }
            if (distinct > best) { best = distinct; bo = o; }
        }
        if (bo >= 0) { g_posOff = bo; Log("items: position at path+%02X", bo); }
    }
    for (int i = 0; i < g_nItems; i++) {
        g_items[i].x = g_items[i].y = -1;
        if (g_posOff >= 0) ItemPos(g_items[i].unit, g_posOff, &g_items[i].x, &g_items[i].y);
    }
}

static int BeltCol(const Item* it) { return g_beltRowInY ? it->x : (it->x & 3); }
static int BeltRow(const Item* it) { return g_beltRowInY ? it->y : (it->x >> 2); }

// Flag the items that are in the belt, once the marker byte is known, and settle how a belt slot is
// written down. Belts hold at most sixteen things and no two of them share a slot, so a marker that
// starts producing something else has been misread and is thrown away.
static void MarkBelt(void) {
    if (g_locOff < 0) return;
    int n = 0; BOOL wideX = FALSE, deepY = FALSE, bad = FALSE;
    for (int i = 0; i < g_nItems; i++) {
        g_items[i].belt = (g_items[i].d[g_locOff] == g_locBelt);
        if (!g_items[i].belt) continue;
        n++;
        if (g_items[i].x < 0 || g_items[i].x > 15 || g_items[i].y > 3) bad = TRUE;
        if (g_items[i].x > 3) wideX = TRUE;
        if (g_items[i].y > 0) deepY = TRUE;
    }
    if (bad || n > 16) {
        Log("items: the belt marker stopped making sense - looking for it again");
        g_locOff = -1; g_locInit = FALSE;
        for (int i = 0; i < g_nItems; i++) g_items[i].belt = FALSE;
        return;
    }
    if (deepY && !wideX) g_beltRowInY = TRUE;
    if (wideX) g_beltRowInY = FALSE;
}

static void EnumItems(void* player) {
    g_nItems = 0;
    if (!FindItemChain(player)) return;
    __try {
        void* inv = RP(player, g_invOff);
        if (!PlausiblePtr(inv)) return;
        void* u = RP(inv, g_firstOff);
        while (u && g_nItems < MAX_ITEMS && IsItemUnit(u)) {
            Item* it = &g_items[g_nItems];
            it->unit = u;
            it->data = RP(u, UNIT_pItemData);
            it->id   = RD(u, UNIT_dwUnitId);
            it->txt  = RD(u, 0x04);
            it->x = it->y = -1; it->belt = FALSE;
            memset(it->d, 0, ITEMDATA_SCAN);
            CopyBytes(it->data, it->d, ITEMDATA_SCAN);
            g_nItems++;
            if (g_nextOff < 0) break;
            void* base = g_nextInData ? it->data : u;
            void* nx = PlausiblePtr(base) ? RP(base, g_nextOff) : NULL;
            if (nx == u) break;
            u = nx;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_nItems = 0; return; }
    ReadPositions();
    MarkBelt();
}

// ---------------------------------------------------------------- auto potion
//
// Kinds are learned by watching, not guessed from item numbers that cannot be checked without running
// the game: a drink that raises life is healing, one that raises mana is mana, and the one potion that
// raises both is a rejuvenation. That holds on every patch and in every language.
//
// An unidentified item is only ever tried through a BELT KEY, which the game can only ever answer with
// a belt potion. Nothing in the inventory is touched until its kind has been established, so it can
// never fire off a scroll or some other item by mistake.
#define KIND_UNKNOWN 0
#define KIND_LIFE    1
#define KIND_MANA    2
#define KIND_REJUV   3
#define KIND_NOHEAL  4           // drunk while hurt and did not heal: never reach for it for life again
#define POTION_SETTLE 1000       // how long a drink is given to show up in the bars
#define MAX_KINDS    64

static DWORD g_lastQuaff;
static int   g_pendKind, g_pendCol = -1, g_prevLife, g_prevMana;
static int   g_tryCol;
// A last-resort memory of what came out of each belt column, for when the character carries too few
// items for the belt to be picked out of the data at all. It is allowed to go stale, so a belt that
// has been rearranged is explored again rather than written off for the rest of the game.
static int   g_colKind[4];
static DWORD g_colWhen[4];
#define COL_STALE 60000
static BOOL  g_learnLoaded, g_saidNoInv;
static struct { DWORD txt; int kind; } g_kind[MAX_KINDS];
static int   g_nKind;

// The packet the game itself sends to drink a potion, captured from the game rather than copied out
// of a packet table: the format that matters is the one this client actually uses.
typedef struct { BYTE buf[32]; int len, idOff; BOOL known; } UsePkt;
static UsePkt g_useBelt, g_useInv;

static const char* KindName(int k) {
    return k == KIND_LIFE ? "healing" : k == KIND_MANA ? "mana" : k == KIND_REJUV ? "rejuvenation"
         : k == KIND_NOHEAL ? "not healing" : "unknown";
}

static void LearnFile(char* p, size_t n) { snprintf(p, n, "%s\\d2fmh-learn.ini", g_dir); }
static void LearnSection(char* s, size_t n, const char* what) {
    snprintf(s, n, "%s_%s", what, g_ver ? g_ver->name : "unknown");
}

static int KindOf(DWORD txt) {
    for (int i = 0; i < g_nKind; i++) if (g_kind[i].txt == txt) return g_kind[i].kind;
    return KIND_UNKNOWN;
}

static void SetKind(DWORD txt, int kind) {
    int at = -1;
    for (int i = 0; i < g_nKind; i++) if (g_kind[i].txt == txt) at = i;
    if (at < 0) { if (g_nKind >= MAX_KINDS) return; at = g_nKind++; g_kind[at].txt = txt; }
    if (g_kind[at].kind == kind) return;
    g_kind[at].kind = kind;
    Log("auto potion: item type %lu is a %s potion", txt, KindName(kind));
    char p[MAX_PATH], sec[64], key[16], val[16];
    LearnFile(p, sizeof p); LearnSection(sec, sizeof sec, "potions");
    sprintf(key, "%lu", txt); sprintf(val, "%d", kind);
    WritePrivateProfileStringA(sec, key, val, p);
}

static void SavePkt(const UsePkt* u, const char* name) {
    char p[MAX_PATH], sec[64], key[32], val[80], off[16];
    LearnFile(p, sizeof p); LearnSection(sec, sizeof sec, "use");
    int n = 0;
    for (int i = 0; i < u->len; i++) n += sprintf(val + n, "%02X", u->buf[i]);
    snprintf(key, sizeof key, "%s_bytes", name); WritePrivateProfileStringA(sec, key, val, p);
    snprintf(key, sizeof key, "%s_idoff", name); sprintf(off, "%d", u->idOff);
    WritePrivateProfileStringA(sec, key, off, p);
}

static void LoadPkt(UsePkt* u, const char* name, const char* p, const char* sec) {
    char key[32], val[80];
    snprintf(key, sizeof key, "%s_bytes", name);
    GetPrivateProfileStringA(sec, key, "", val, sizeof val, p);
    int len = (int)strlen(val) / 2;
    if (len < 5 || len > 32) return;
    for (int i = 0; i < len; i++) {
        char h[3]; h[0] = val[i * 2]; h[1] = val[i * 2 + 1]; h[2] = 0;
        u->buf[i] = (BYTE)strtoul(h, NULL, 16);
    }
    snprintf(key, sizeof key, "%s_idoff", name);
    int off = GetPrivateProfileIntA(sec, key, 0, p);
    if (off < 1 || off + 4 > len) return;
    u->len = len; u->idOff = off; u->known = TRUE;
    Log("auto potion: remembered how to drink from the %s (packet %02X, %d bytes)", name, u->buf[0], len);
}

static void LoadLearned(void) {
    char p[MAX_PATH], sec[64], buf[4096];
    LearnFile(p, sizeof p);
    LearnSection(sec, sizeof sec, "potions");
    g_nKind = 0;
    DWORD n = GetPrivateProfileSectionA(sec, buf, sizeof buf, p);
    if (n) for (char* s = buf; *s && g_nKind < MAX_KINDS; s += strlen(s) + 1) {
        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        g_kind[g_nKind].txt = strtoul(s, NULL, 10);
        g_kind[g_nKind].kind = atoi(eq + 1);
        g_nKind++;
    }
    if (g_nKind) Log("auto potion: %d potion types remembered from earlier games", g_nKind);
    LearnSection(sec, sizeof sec, "use");
    LoadPkt(&g_useBelt, "belt", p, sec);
    LoadPkt(&g_useInv, "inv", p, sec);
}

static void PressBelt(int col) {
    if (!g_hwnd || col < 0 || col > 3) return;
    UINT vk = (UINT)('1' + col);
    LPARAM down = 1 | ((col + 2) << 16);        // scan codes for 1-4 are 2-5
    PostMessageA(g_hwnd, WM_KEYDOWN, vk, down);
    PostMessageA(g_hwnd, WM_KEYUP, vk, down | (1 << 30) | (1u << 31));
}

static BOOL UseItemPkt(UsePkt* u, DWORD id) {
    if (!u->known || !g_sendPacket) return FALSE;
    BYTE b[32];
    memcpy(b, u->buf, (size_t)u->len);
    *(DWORD*)(b + u->idOff) = id;
    g_ourSend = TRUE;
    if (g_ver->sendLenFirst) ((SendNew_t)g_sendPacket)((size_t)u->len, 1, b);
    else                     ((SendOld_t)g_sendPacket)(0, b, (size_t)u->len);
    g_ourSend = FALSE;
    return TRUE;
}

// The item just used was named in one of the packets the game sent a moment ago; find the one that
// carries its number and keep it as the template. Replaying that exact packet with another item's
// number is how every other potion is drunk from then on.
static void LearnUse(UsePkt* u, DWORD id, const char* what) {
    if (u->known) return;
    DWORD now = GetTickCount();
    for (int i = 0; i < PKT_RING; i++) {
        PktRec* r = &g_ring[i];
        if (!r->len || now - r->when > 4000) continue;
        for (int o = 1; o + 4 <= r->len; o++) {
            if (*(DWORD*)(r->buf + o) != id) continue;
            memcpy(u->buf, r->buf, (size_t)r->len);
            u->len = r->len; u->idOff = o; u->known = TRUE;
            Log("auto potion: learned to drink from the %s (packet %02X, %d bytes, item at +%d)",
                what, r->buf[0], r->len, o);
            SavePkt(u, what);
            return;
        }
    }
}

// Which byte of an item's data says it is in the belt. The ground truth arrives free: a belt key can
// only consume a belt potion, so whatever vanished after a press was in the belt, at the bottom of the
// column pressed. Every byte that would contradict that - or would put two things in one slot, or more
// than sixteen things in a belt, or would sweep in every item the character owns - is struck out, and
// when the survivors all describe the same set of items it is settled.
static void GroupMask(int b, BYTE v, DWORD* mask) {
    mask[0] = mask[1] = mask[2] = 0;
    for (int i = 0; i < g_nItems && i < 96; i++)
        if (g_items[i].d[b] == v) mask[i >> 5] |= 1u << (i & 31);
}

static void LearnBeltMarker(const Item* gone, int col) {
    if (g_locOff >= 0 || g_posOff < 0 || col < 0 || gone->x < 0) return;
    if ((gone->x & 3) != col) return;                 // not the item the press should have taken
    if (!g_locInit) { memset(g_locCand, 1, sizeof g_locCand); g_locInit = TRUE; }
    int survivors = 0, first = -1;
    for (int b = 0; b < ITEMDATA_SCAN; b++) {
        if (!g_locCand[b]) continue;
        BYTE v = gone->d[b];
        int in = 0, out = 0; BOOL ok = TRUE;
        for (int i = 0; i < g_nItems && ok; i++) {
            if (g_items[i].d[b] != v) { out++; continue; }
            in++;
            if (g_items[i].x < 0 || g_items[i].x > 15 || g_items[i].y > 3) { ok = FALSE; break; }
            for (int j = 0; j < i; j++)
                if (g_items[j].d[b] == v && g_items[j].x == g_items[i].x && g_items[j].y == g_items[i].y) {
                    ok = FALSE; break;
                }
        }
        if (!ok || in > 16 || out == 0) { g_locCand[b] = 0; continue; }
        survivors++;
        if (first < 0) first = b;
    }
    if (!survivors) { g_locInit = FALSE; return; }     // contradictory: start the search over
    DWORD m0[3], m[3];
    GroupMask(first, gone->d[first], m0);
    for (int b = first + 1; b < ITEMDATA_SCAN; b++) {
        if (!g_locCand[b]) continue;
        GroupMask(b, gone->d[b], m);
        if (m[0] != m0[0] || m[1] != m0[1] || m[2] != m0[2]) return;   // still ambiguous
    }
    g_locOff = first; g_locBelt = gone->d[first];
    Log("items: belt items are the ones marked by data+%02X = %02X", g_locOff, g_locBelt);
    MarkBelt();
}

// Something the character owned is gone. If a bar went up at the same moment it was a drink - ours or
// the player's own - and that names the potion, teaches the packet, and, when we pressed a belt key,
// points out which items live in the belt.
static void JudgeChange(int life, int mana) {
    int gone = -1;
    for (int i = 0; i < g_nSnap; i++) {
        BOOL still = FALSE;
        for (int j = 0; j < g_nItems; j++) if (g_items[j].id == g_snap[i].id) { still = TRUE; break; }
        if (!still) {
            if (gone >= 0) { gone = -2; break; }        // several at once: cannot tell which did what
            gone = i;
        }
    }
    int pend = g_pendKind, col = g_pendCol;
    g_pendKind = 0; g_pendCol = -1;
    if (gone < 0) {
        // No item was seen to vanish - either nothing was drunk, or item enumeration is not working on
        // this patch (as on 1.08). Either way the BARS still tell the story: a belt press that raised
        // life is a healing column, one that raised mana is a mana column, one that raised both is
        // rejuvenations. This is how the column is learned when the item list is empty, so auto potion
        // reaches for the right column instead of cycling through all four.
        if (pend && col >= 0 && col < 4) {
            BOOL lifeUp = life > g_prevLife + 1, manaUp = mana > g_prevMana + 1;
            int k = lifeUp && manaUp ? KIND_REJUV : lifeUp ? KIND_LIFE : manaUp ? KIND_MANA : KIND_UNKNOWN;
            g_colKind[col] = k; g_colWhen[col] = GetTickCount();
            if (k != KIND_UNKNOWN) Log("auto potion: belt %d holds %s (learned from the bars)", col + 1, KindName(k));
            else                   Log("auto potion: nothing came out of belt %d", col + 1);
        }
        return;
    }
    Item* it = &g_snap[gone];
    BOOL lifeUp = life > g_prevLife + 1, manaUp = mana > g_prevMana + 1;
    int kind = lifeUp && manaUp ? KIND_REJUV : lifeUp ? KIND_LIFE : manaUp ? KIND_MANA : KIND_UNKNOWN;
    if (kind == KIND_UNKNOWN) {
        // Nothing moved. If we had asked for healing, this is not a healing potion, whatever else it
        // may be; if we had not, the item was dropped or sold and it says nothing about anything.
        if (pend == KIND_LIFE && KindOf(it->txt) == KIND_UNKNOWN) SetKind(it->txt, KIND_NOHEAL);
        return;
    }
    SetKind(it->txt, kind);
    if (pend && col >= 0 && col < 4) { g_colKind[col] = kind; g_colWhen[col] = GetTickCount(); }
    if (pend && col >= 0) {
        LearnBeltMarker(it, col);
        LearnUse(&g_useBelt, it->id, "belt");
    } else if (!pend) {
        LearnUse(it->belt ? &g_useBelt : &g_useInv, it->id, it->belt ? "belt" : "inv");
    }
}

// A belt key drinks the lowest potion in its column, so that is the only one a press can aim at.
static BOOL LowestInColumn(const Item* it) {
    for (int i = 0; i < g_nItems; i++) {
        const Item* o = &g_items[i];
        if (o == it || !o->belt || o->x < 0) continue;
        if (BeltCol(o) == BeltCol(it) && BeltRow(o) < BeltRow(it)) return FALSE;
    }
    return TRUE;
}

// The best thing to drink for what is needed. Healing first, a rejuvenation when things are dire or
// when there is nothing else, and an unidentified belt potion only as a way of finding out what it is.
static int PickPotion(int want, BOOL desperate) {
    int best = -1, bestScore = 0;
    for (int i = 0; i < g_nItems; i++) {
        Item* it = &g_items[i];
        int k = KindOf(it->txt), score = 0;
        if (want == KIND_LIFE) {
            if (k == KIND_LIFE)       score = 100;
            else if (k == KIND_REJUV) score = desperate ? 110 : 40;
            else if (k == KIND_UNKNOWN && it->belt) score = 10;
        } else {
            if (k == KIND_MANA)       score = 100;
            else if (k == KIND_REJUV) score = 30;
            else if (k == KIND_UNKNOWN && it->belt) score = 10;
            else if (k == KIND_NOHEAL && it->belt)  score = 20;   // did not heal, so try it for mana
        }
        if (!score) continue;
        if (it->belt) {
            // Without the game's own packet a key press is all there is, and a press takes the bottom
            // of the column, so only the bottom row can be aimed at.
            if (!g_useBelt.known && (g_posOff < 0 || it->x < 0 || !LowestInColumn(it))) continue;
            score += 5;                                  // the belt before the inventory
        } else {
            if (k == KIND_UNKNOWN || k == KIND_NOHEAL) continue;   // never use an unidentified item
            if (!g_useInv.known) {
                if (!g_saidNoInv) {
                    g_saidNoInv = TRUE;
                    Log("auto potion: there are potions in the inventory but the game has not yet shown "
                        "how it uses one - right-click a potion in your inventory once to teach it");
                }
                continue;
            }
        }
        if (score > bestScore || (score == bestScore && best >= 0 && it->txt < g_items[best].txt)) {
            bestScore = score; best = i;
        }
    }
    return best;
}

static void Drink(int want, BOOL desperate, int cur, int max, int pct, DWORD now) {
    const char* what = want == KIND_LIFE ? "life" : "mana";
    int i = PickPotion(want, desperate);
    if (i >= 0) {
        Item* it = &g_items[i];
        int k = KindOf(it->txt);
        if (it->belt && !g_useBelt.known) {
            g_pendCol = BeltCol(it);
            PressBelt(g_pendCol);
        } else {
            g_pendCol = -1;
            if (!UseItemPkt(it->belt ? &g_useBelt : &g_useInv, it->id)) return;
        }
        g_pendKind = want; g_lastQuaff = now;
        Log("auto potion: %s %d/%d (%d%%) - %s potion (type %lu) from the %s",
            what, cur, max, pct, KindName(k), it->txt, it->belt ? "belt" : "inventory");
        return;
    }
    // Nothing identified to reach for: press a belt column and find out what is in it. A belt key can
    // only ever drink a belt potion, so the worst this can cost is one potion of the wrong kind, once
    // per kind ever - after that it is remembered, in this game and in every game after it.
    int col = -1;
    for (int n = 0; n < 4 && col < 0; n++) {
        int c = (g_tryCol + n) & 3;
        int k = (now - g_colWhen[c] > COL_STALE) ? KIND_UNKNOWN : g_colKind[c];
        if (k == KIND_UNKNOWN || k == KIND_REJUV || k == want) col = c;
    }
    if (col < 0) {
        Log("auto potion: %s is low but every belt column last gave the wrong kind", what);
        for (int c = 0; c < 4; c++) g_colWhen[c] = 0;    // look again rather than give up for good
        return;
    }
    g_pendCol = col;
    g_tryCol = (col + 1) & 3;
    PressBelt(g_pendCol);
    g_pendKind = want; g_lastQuaff = now;
    Log("auto potion: %s %d/%d (%d%%) - nothing known to drink yet, trying belt %d",
        what, cur, max, pct, g_pendCol + 1);
}

// Counted once a beat for the panel, so you can see what is left without opening the belt. Only
// potions of a kind already identified can be counted as that kind; the ones still unidentified are
// counted apart rather than quietly left out.
static int g_nLifePots, g_nManaPots, g_nRejuvPots, g_nUnknownPots;

static void CountPotions(void) {
    g_nLifePots = g_nManaPots = g_nRejuvPots = g_nUnknownPots = 0;
    for (int i = 0; i < g_nItems; i++) {
        switch (KindOf(g_items[i].txt)) {
            case KIND_LIFE:  g_nLifePots++;  break;
            case KIND_MANA:  g_nManaPots++;  break;
            case KIND_REJUV: g_nRejuvPots++; break;
            default: if (g_items[i].belt) g_nUnknownPots++; break;   // a belt item is always a potion
        }
    }
}

// Everything the potion code needs from the character's items, gathered once a beat. It runs whether
// or not auto potion is switched on, because the counts on the panel are worth having either way.
// When the item chain cannot be found on a patch, this dumps the player unit and, for each field that
// points somewhere, whether that target leads to an item - which is exactly what the inventory pointer
// does. Read off a log, it gives the inventory offset the same way the Room1 dump gave the collision
// offset. Logged once per game.
static void DumpPlayerForItems(void* player) {
    static BOOL dumped;
    if (dumped) return;
    dumped = TRUE;
    __try {
        Log("items: chain not found - player @%p dump (looking for the inventory pointer):", player);
        for (int f = 0x00; f <= 0xA0; f += 4) {
            void* p = RP(player, f);
            if (!PlausiblePtr(p) || !Readable(p, 0x40)) continue;
            // Does anything in the first 0x40 bytes of this struct point at an item unit? If so, this
            // field is the inventory and that sub-offset is where the first item hangs. The inventory
            // struct also holds the item count near its start, another giveaway.
            int itemAt = -1;
            for (int g = 0; g <= 0x3C; g += 4) if (IsItemUnit(RP(p, g))) { itemAt = g; break; }
            DWORD* h = (DWORD*)p;
            Log("  player+%02X -> %p : %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX%s",
                f, p, h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7],
                itemAt >= 0 ? " <== holds an item" : "");
            if (itemAt >= 0) Log("      first item at +%02X", itemAt);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

static void RefreshItems(void* player) {
    if (!g_learnLoaded && g_ver) { g_learnLoaded = TRUE; LoadLearned(); }
    EnumItems(player);
    if (g_nItems == 0) DumpPlayerForItems(player);
    CountPotions();
}

static void AutoPotion(void* player) {
    if (!g_autoPotion) return;

    int life, maxLife, mana, maxMana;
    if (!ReadVitals(player, &life, &maxLife, &mana, &maxMana)) return;
    if (maxLife <= 0 || life <= 0) { g_pendKind = 0; g_pendCol = -1; return; }

    DWORD now = GetTickCount();

    // A drink is given a moment to show in the bars before it is judged, and until then the picture of
    // what the character was carrying beforehand is kept, because that is what the change is read from.
    if (now - g_lastQuaff < POTION_SETTLE) return;
    if (g_nSnap) JudgeChange(life, mana);
    memcpy(g_snap, g_items, sizeof(Item) * (size_t)g_nItems);
    g_nSnap = g_nItems;
    g_prevLife = life; g_prevMana = mana;

    if (g_dump && now - g_lastItemLog > 30000) {
        g_lastItemLog = now;
        Log("items: %d carried, belt marker %s, positions %s", g_nItems,
            g_locOff >= 0 ? "found" : "not found yet", g_posOff >= 0 ? "found" : "not found yet");
        for (int i = 0; i < g_nItems && i < 24; i++)
            Log("  item %lu type %lu at %d,%d %s %s", g_items[i].id, g_items[i].txt,
                g_items[i].x, g_items[i].y, g_items[i].belt ? "belt" : "-",
                KindName(KindOf(g_items[i].txt)));
    }

    int lifePct = life * 100 / maxLife;
    if (lifePct <= g_lifePct) {
        Drink(KIND_LIFE, lifePct <= g_rejuvPct, life, maxLife, lifePct, now);
        return;
    }
    if (g_manaPct > 0 && maxMana > 0) {
        int manaPct = mana * 100 / maxMana;
        if (manaPct <= g_manaPct) Drink(KIND_MANA, FALSE, mana, maxMana, manaPct, now);
    }
}

// ---------------------------------------------------------------- collision map
// Each loaded room carries a grid of one 16-bit value per subtile saying what blocks there. Teleport
// lands exactly where it is aimed and fails silently if that cell is not standable, so checking the
// grid first is the difference between gliding across a level and bouncing off the first wall.
//
// The pointer to the grid sits at a different offset in Room1 on every patch, so rather than carrying
// yet another number per patch it is FOUND: a collision map is self-describing, because its header
// gives the grid size and the two pointers that bracket the grid must be exactly width*height*2 apart.
// Nothing else in a room looks like that, and being wrong is caught anyway by the check below that the
// character's own cell reads as standable.
// The collision map's fields sit at different offsets per patch. Two layouts are known: 1.10 and later
// keep the grid inline after an eight-dword header (posGame, sizeGame, posRoom, sizeRoom, then a
// pointer to the inline grid); 1.08 and 1.09 prepend a pMapStart / pMapEnd pair, so everything after
// shifts by eight. Rather than carry the offsets per patch, the layout is detected the first time a
// map is found and remembered, the same way the map's offset inside Room1 is. These globals hold the
// detected offsets; posY and sizeY always follow posX and sizeX by four.
#define COLL_POS_X   (g_cPos)
#define COLL_POS_Y   (g_cPos + 4)
#define COLL_SIZE_X  (g_cSize)
#define COLL_SIZE_Y  (g_cSize + 4)
#define COLL_MAP_START (g_cGrid)

static int g_cPos = 0x00, g_cSize = 0x08, g_cGrid = 0x20;   // default to the 1.10 layout

// The known collision-map shapes, tried in order: posX, sizeX, roomX (the room-tile size, one fifth of
// sizeGame), and the offset of the pointer that leads to the grid.
static const struct { int pos, size, room, grid; } kCollLayouts[] = {
    { 0x00, 0x08, 0x18, 0x20 },   // 1.10 – 1.13d
    { 0x08, 0x10, 0x20, 0x00 },   // 1.08, 1.09: pMapStart/pMapEnd prepended
};

// A Room1 and a collision map begin with similar numbers, so size alone cannot tell them apart. What
// can: the subtile size is exactly five times the room size, the grid is readable at its full extent,
// and its first entries look like collision flags (small bit masks) not pointers. Each known layout is
// tried; the one that fits is remembered in the globals above.
static BOOL ValidColl(void* c) {
    if (!Readable(c, 0x2C)) return FALSE;
    for (int i = 0; i < (int)(sizeof kCollLayouts / sizeof kCollLayouts[0]); i++) {
        int po = kCollLayouts[i].pos, so = kCollLayouts[i].size, ro = kCollLayouts[i].room, go = kCollLayouts[i].grid;
        DWORD sx = RD(c, so), sy = RD(c, so + 4), rx = RD(c, ro), ry = RD(c, ro + 4);
        if (sx < 1 || sx > 1024 || sy < 1 || sy > 1024) continue;
        if (rx == 0 || ry == 0 || sx != rx * 5 || sy != ry * 5) continue;
        WORD* g = (WORD*)RP(c, go);
        if (!Readable(g, sx * sy * 2)) continue;
        BOOL flags = TRUE;
        for (int k = 0; k < 16; k++) if (g[k] >= 0x1000) { flags = FALSE; break; }
        if (!flags) continue;
        g_cPos = po; g_cSize = so; g_cGrid = go;
        return TRUE;
    }
    return FALSE;
}

static void* RoomColl(void* room1) {
    if (!Readable(room1, 0x100)) return NULL;
    // A cached offset is used only while it still validates. It used to be latched forever on the first
    // hit, which on 1.08 was a false positive read out of the next structure (Room1+0xFC): once cached,
    // every later room failed there and the level looked collision-less. Now a miss re-searches.
    if (g_collOff >= 0) {
        void* c = RP(room1, g_collOff);
        if (ValidColl(c)) return c;
        g_collOff = -1;
    }
    void* found = NULL; int foundOff = -1;
    static BOOL logged;
    for (int off = 0; off < 0x100; off += 4) {
        void* c = RP(room1, off);
        if (!ValidColl(c)) continue;
        if (found == NULL) { found = c; foundOff = off; }
        if (!logged) Log("collision map candidate at Room1+%02X", off);   // more than one means ambiguity
    }
    logged = TRUE;
    if (found) {
        g_collOff = foundOff;
        static BOOL saidLayout;
        if (!saidLayout) { saidLayout = TRUE; Log("collision map at Room1+%02X, layout pos+%02X size+%02X grid+%02X",
                                                  foundOff, g_cPos, g_cSize, g_cGrid); }
        return found;
    }
    return NULL;
}

// 1 standable, 0 blocked, -1 no data (the room is not loaded, so do not rule the cell out).
static int CellWalkable(int wx, int wy) {
    const D2Version* v = g_ver;
    if (!g_useCollision || !g_collUsable || !g_curLevel) return -1;

    void* c = g_lastColl;
    for (int pass = 0; pass < 2; pass++) {
        if (c && Readable(c, 0x28)) {
            int px = (int)RD(c, COLL_POS_X), py = (int)RD(c, COLL_POS_Y);
            int sx = (int)RD(c, COLL_SIZE_X), sy = (int)RD(c, COLL_SIZE_Y);
            if (wx >= px && wy >= py && wx < px + sx && wy < py + sy) {
                WORD* grid = (WORD*)RP(c, COLL_MAP_START);
                int idx = (wy - py) * sx + (wx - px);
                if (!Readable(grid + idx, 2)) return -1;
                g_lastColl = c;
                return (grid[idx] & g_collMask) ? 0 : 1;
            }
        }
        if (pass) break;
        // Not in the cached room: look through the rooms the game currently has loaded.
        c = NULL;
        int rooms = 0;
        for (void* r2 = RP(g_curLevel, v->level_pRoom2First);
             Readable(r2, 0x100) && rooms < 400; r2 = RP(r2, v->room2_pNext), rooms++) {
            void* r1 = RP(r2, v->room2_pRoom1);
            if (!Readable(r1, 0x100)) continue;
            void* cand = RoomColl(r1);
            if (!cand) continue;
            int px = (int)RD(cand, COLL_POS_X), py = (int)RD(cand, COLL_POS_Y);
            int sx = (int)RD(cand, COLL_SIZE_X), sy = (int)RD(cand, COLL_SIZE_Y);
            if (wx >= px && wy >= py && wx < px + sx && wy < py + sy) { c = cand; break; }
        }
        if (!c) return -1;
    }
    return -1;
}

// The character is by definition standing somewhere standable. If the grid says otherwise the offset or
// the mask is wrong for this patch, so stop trusting it rather than refusing every hop.
static void CheckCollisionAgainstPlayer(int px, int py) {
    if (!g_useCollision || !g_collUsable) return;
    if (CellWalkable(px, py) == 0) {
        g_collUsable = FALSE;
        Log("collision map disagrees with where the character is standing - falling back to angle retries");
    }
}

// ---------------------------------------------------------------- path planning
// Hopping greedily towards the target cannot get out of a concave obstacle, and a level like the Great
// Marsh is almost nothing but concave obstacles - the log showed 87% of one room blocked. So the route
// is planned first, by a breadth-first search over a grid stamped from the collision maps of the rooms
// the game has loaded, and the teleport then follows that route. Cells with no collision data are
// treated as open, so a route can still be laid across ground the game has not loaded yet.
#define PATH_DIM        256                  // grid is at most this many nodes on a side
#define PATH_NODES      (PATH_DIM * PATH_DIM)
#define PATH_MAX_POINTS 400
#define PATH_MARGIN     48                   // subtiles of slack around the straight line
#define PATH_HORIZON    160                  // plan this far ahead at a time, then plan again

static BYTE* g_grid;        // 0 open, 1 blocked
static BYTE* g_came;        // 0 unvisited, else direction taken to get here (+1), 0xFF for the start
static int*  g_queue;
static int g_gw, g_gh, g_gstep, g_gx0, g_gy0;
static int g_pathX[PATH_MAX_POINTS], g_pathY[PATH_MAX_POINTS];
static int g_pathLen, g_pathIdx;

static const int kDirX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
static const int kDirY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

static BOOL PathAlloc(void) {
    if (g_grid) return TRUE;
    g_grid = (BYTE*)malloc(PATH_NODES);
    g_came = (BYTE*)malloc(PATH_NODES);
    g_queue = (int*)malloc(PATH_NODES * sizeof(int));
    if (g_grid && g_came && g_queue) return TRUE;
    free(g_grid); free(g_came); free(g_queue);
    g_grid = g_came = NULL; g_queue = NULL;
    Log("path planning disabled: out of memory");
    return FALSE;
}

// Mark the nodes that the loaded rooms say are blocked.
//
// A node covers step*step subtiles. Judging it by one representative subtile threw away almost all the
// open ground in a place like the Great Marsh, where the walkable strips are narrower than the node
// spacing - the search ended up boxed in after a single step. A node is open if ANY of its subtiles is
// standable, and the grid remembers WHICH one so the hop aims at ground that is actually free.
//   0            = no data, treat as open at the node's own corner
//   1            = blocked
//   2 + dy*step+dx = open, at that offset inside the node
#define NODE_BLOCKED 1
#define NODE_FREE(off) ((BYTE)(2 + (off)))

static void NodePoint(int i, int j, int* x, int* y) {
    BYTE v = g_grid[j * g_gw + i];
    int off = v >= 2 ? v - 2 : 0;
    *x = g_gx0 + i * g_gstep + off % g_gstep;
    *y = g_gy0 + j * g_gstep + off / g_gstep;
}

static void StampRooms(void) {
    const D2Version* v = g_ver;
    memset(g_grid, 0, (size_t)g_gw * g_gh);
    int rooms = 0;
    for (void* r2 = RP(g_curLevel, v->level_pRoom2First);
         Readable(r2, 0x100) && rooms < 400; r2 = RP(r2, v->room2_pNext), rooms++) {
        void* r1 = RP(r2, v->room2_pRoom1);
        if (!Readable(r1, 0x100)) continue;
        void* c = RoomColl(r1);
        if (!c) continue;
        int rpx = (int)RD(c, COLL_POS_X), rpy = (int)RD(c, COLL_POS_Y);
        int rsx = (int)RD(c, COLL_SIZE_X), rsy = (int)RD(c, COLL_SIZE_Y);
        WORD* g = (WORD*)RP(c, COLL_MAP_START);
        if (!Readable(g, (SIZE_T)rsx * rsy * 2)) continue;

        int i0 = (rpx - g_gx0 + g_gstep - 1) / g_gstep, i1 = (rpx + rsx - 1 - g_gx0) / g_gstep;
        int j0 = (rpy - g_gy0 + g_gstep - 1) / g_gstep, j1 = (rpy + rsy - 1 - g_gy0) / g_gstep;
        if (i0 < 0) i0 = 0; if (j0 < 0) j0 = 0;
        if (i1 >= g_gw) i1 = g_gw - 1; if (j1 >= g_gh) j1 = g_gh - 1;
        for (int j = j0; j <= j1; j++) {
            for (int i = i0; i <= i1; i++) {
                int bx = g_gx0 + i * g_gstep, by = g_gy0 + j * g_gstep;
                int found = -1, examined = 0;
                for (int dy = 0; dy < g_gstep && found < 0; dy++) {
                    int sy = by + dy;
                    if (sy < rpy || sy >= rpy + rsy) continue;
                    for (int dx = 0; dx < g_gstep; dx++) {
                        int sx = bx + dx;
                        if (sx < rpx || sx >= rpx + rsx) continue;
                        examined++;
                        if (!(g[(sy - rpy) * rsx + (sx - rpx)] & g_collMask)) { found = dy * g_gstep + dx; break; }
                    }
                }
                if (found >= 0) g_grid[j * g_gw + i] = NODE_FREE(found);
                else if (examined) g_grid[j * g_gw + i] = NODE_BLOCKED;
            }
        }
    }
}

// Plan a route from the character to the target. Returns FALSE only if nothing could be laid out at
// all, in which case the caller falls back to aiming straight at the target.
static BOOL PlanPath(int px, int py, int tx, int ty) {
    g_pathLen = g_pathIdx = 0;
    if (!g_useCollision || !g_collUsable || !g_curLevel || !PathAlloc()) return FALSE;

    // Plan only as far ahead as the next stretch, not all the way to a target that may be most of a
    // level away. Covering the whole distance forced the grid coarse - four subtiles a node in the log
    // that showed this failing - and a coarse grid cannot see the gaps between obstacles. A short
    // horizon keeps the grid fine, and the route is simply laid again as the character advances.
    int dx = tx - px, dy = ty - py;
    double straight = sqrt((double)dx * dx + (double)dy * dy);
    int gxT = tx, gyT = ty;
    if (straight > PATH_HORIZON) {
        gxT = px + (int)(dx / straight * PATH_HORIZON);
        gyT = py + (int)(dy / straight * PATH_HORIZON);
    }

    int minX = px < gxT ? px : gxT, maxX = px > gxT ? px : gxT;
    int minY = py < gyT ? py : gyT, maxY = py > gyT ? py : gyT;
    minX -= PATH_MARGIN; minY -= PATH_MARGIN; maxX += PATH_MARGIN; maxY += PATH_MARGIN;
    if (minX < 0) minX = 0; if (minY < 0) minY = 0;

    g_gstep = 2;
    while ((maxX - minX) / g_gstep + 1 > PATH_DIM || (maxY - minY) / g_gstep + 1 > PATH_DIM) g_gstep += 2;
    g_gx0 = minX; g_gy0 = minY;
    g_gw = (maxX - minX) / g_gstep + 1;
    g_gh = (maxY - minY) / g_gstep + 1;

    StampRooms();

    int si = (px - g_gx0) / g_gstep, sj = (py - g_gy0) / g_gstep;
    int gi = (gxT - g_gx0) / g_gstep, gj = (gyT - g_gy0) / g_gstep;
    if (si < 0 || sj < 0 || si >= g_gw || sj >= g_gh) return FALSE;
    if (gi < 0) gi = 0; if (gj < 0) gj = 0;
    if (gi >= g_gw) gi = g_gw - 1; if (gj >= g_gh) gj = g_gh - 1;

    memset(g_came, 0, (size_t)g_gw * g_gh);
    int start = sj * g_gw + si, goal = gj * g_gw + gi;
    int head = 0, tail = 0;
    g_came[start] = 0xFF;
    g_queue[tail++] = start;

    int best = start, bestD = abs(si - gi) + abs(sj - gj);
    while (head < tail) {
        int cur = g_queue[head++];
        if (cur == goal) { best = cur; break; }
        int ci = cur % g_gw, cj = cur / g_gw;
        int d = abs(ci - gi) + abs(cj - gj);
        if (d < bestD) { bestD = d; best = cur; }
        for (int k = 0; k < 8; k++) {
            int ni = ci + kDirX[k], nj = cj + kDirY[k];
            if (ni < 0 || nj < 0 || ni >= g_gw || nj >= g_gh) continue;
            int nidx = nj * g_gw + ni;
            if (g_came[nidx] || g_grid[nidx] == NODE_BLOCKED) continue;
            g_came[nidx] = (BYTE)(k + 1);
            g_queue[tail++] = nidx;
        }
    }

    // Walk the route back from the closest node reached, then reverse it into travel order.
    static int backX[PATH_NODES / 8], backY[PATH_NODES / 8];
    int n = 0, cur = best;
    while (cur != start && n < (int)(sizeof backX / sizeof backX[0])) {
        int ci = cur % g_gw, cj = cur / g_gw;
        NodePoint(ci, cj, &backX[n], &backY[n]);
        n++;
        BYTE dir = g_came[cur];
        if (!dir || dir == 0xFF) break;
        cur = (cj - kDirY[dir - 1]) * g_gw + (ci - kDirX[dir - 1]);
    }
    if (n == 0) return FALSE;

    int stride = n / PATH_MAX_POINTS + 1;
    g_pathLen = 0;
    for (int i = n - 1; i >= 0 && g_pathLen < PATH_MAX_POINTS; i -= stride) {
        g_pathX[g_pathLen] = backX[i];
        g_pathY[g_pathLen] = backY[i];
        g_pathLen++;
    }
    // Finish on the point aimed at rather than the nearest grid node to it.
    if (g_pathLen < PATH_MAX_POINTS) { g_pathX[g_pathLen] = gxT; g_pathY[g_pathLen] = gyT; g_pathLen++; }
    g_pathIdx = 0;
    // Routes are laid repeatedly as the character advances, so only say something when one comes out
    // suspiciously short - which is the symptom of the search being boxed in.
    static DWORD lastLog;
    DWORD now = GetTickCount();
    if (g_pathLen <= 2 || now - lastLog > 5000) {
        lastLog = now;
        Log("planned a route of %d point(s) over %d subtiles, grid %dx%d at %d per node%s",
            g_pathLen, (int)straight, g_gw, g_gh, g_gstep, best == goal ? "" : " (stopped short of the aim point)");
    }
    return TRUE;
}

// ---------------------------------------------------------------- travel
static BOOL g_traveling;
static int g_tx, g_ty;
static DWORD g_travelStart, g_lastSend;
static int g_lastPx, g_lastPy, g_noProgress;
static char g_travelWhat[64];
static int g_teleTry, g_teleFromX, g_teleFromY, g_replans;

// Teleport lands you exactly where you asked, so a hop into a wall simply does nothing and repeating it
// gets nowhere - which is what made travel give up. The game's own collision map would say in advance
// which cells are free; short of reading it, trying the same hop at a widening spread of angles walks
// around almost anything, and shortening the hop after a full sweep gets through narrow gaps.
static const int kTeleAngles[] = { 0, 20, -20, 40, -40, 65, -65, 90, -90, 120, -120 };
#define NUM_TELE_ANGLES (sizeof(kTeleAngles) / sizeof(kTeleAngles[0]))
#define TELE_HOP 42          // subtiles per hop - about a screen. Teleport has no skill range limit in
                             // classic D2, so long hops are fine; the collision check and the did-it-move
                             // retry keep a hop that would land in a wall from sticking. Short hops (20)
                             // were the main reason travel felt slow - half a screen at a time.
#define RUN_HOP  25          // how far ahead a walk or run may be aimed; roughly a screen

// The movement commands, in the exact form the game itself emits: one command byte followed by two
// 16-bit world coordinates, five bytes in all. Taken from the client's own outgoing packets rather
// than from a packet table - logging a real click produced "03 BA 16 75 12" while the character stood
// at 5813,4723, i.e. run to (0x16BA, 0x1275) = (5818, 4725). Several published tables claim 32-bit
// coordinates, which would have been wrong.
#define CMD_WALK 0x01
#define CMD_RUN  0x03
#define CMD_TELE 0x0C      // right skill at a location; only teleports if Teleport is the right skill

static void SendMove(BYTE cmd, int x, int y) {
    if (!g_sendPacket || x < 0 || y < 0 || x > 0xFFFF || y > 0xFFFF) return;
    BYTE pkt[5];
    pkt[0] = cmd;
    *(WORD*)(pkt + 1) = (WORD)x;
    *(WORD*)(pkt + 3) = (WORD)y;
    if (g_ver->sendLenFirst) ((SendNew_t)g_sendPacket)(sizeof pkt, 1, pkt);
    else                     ((SendOld_t)g_sendPacket)(0, pkt, sizeof pkt);
}

static void StopTravel(const char* why) {
    if (!g_traveling) return;
    g_traveling = FALSE;
    g_travelTarget = -1;
    Log("travel stopped (%s)", why);
}

static void StartTravel(int x, int y, const char* what) {
    if (!g_sendPacket) { Log("cannot travel: the packet sender was not found for this patch"); return; }
    g_traveling = TRUE;
    g_tx = x; g_ty = y;
    g_travelStart = GetTickCount(); g_lastSend = 0;
    g_lastPx = g_lastPy = -1; g_noProgress = 0;
    g_teleTry = 0; g_teleFromX = g_teleFromY = -9999;
    g_pathLen = g_pathIdx = 0; g_replans = 0;
    snprintf(g_travelWhat, sizeof g_travelWhat, "%s", what);
    Log("travelling to %s at %d,%d (%s%s)", what, x, y,
        g_moveMode == 2 ? "teleport" : g_moveMode == 1 ? "walk" : "run",
        g_moveMode == 2 ? (g_useCollision && g_collUsable ? ", collision aware" : ", no collision data") : "");
}

// Choose where the next teleport should land: the longest hop along the straightest direction that the
// collision map says is standable. Where there is no collision data (a room the game has not loaded)
// the cell is not ruled out, so a long trip still makes progress and the did-it-move retry covers it.
static BOOL PickHop(int px, int py, int dx, int dy, int dist, int tryIdx, int* outX, int* outY) {
    double len = sqrt((double)dx * dx + (double)dy * dy);
    if (len < 1.0) return FALSE;
    double ux = dx / len, uy = dy / len;

    if (dist <= TELE_HOP && CellWalkable(px + dx, py + dy) != 0) {
        *outX = px + dx; *outY = py + dy;
        return TRUE;
    }
    for (int k = 0; k < (int)NUM_TELE_ANGLES; k++) {
        // Start from a different direction each retry, so a direction that keeps failing is dropped.
        int ai = (tryIdx + k) % NUM_TELE_ANGLES;
        double a = kTeleAngles[ai] * 3.14159265358979 / 180.0;
        double rx = ux * cos(a) - uy * sin(a), ry = ux * sin(a) + uy * cos(a);
        for (int d = TELE_HOP; d >= 6; d -= 3) {
            int cx = px + (int)(rx * d), cy = py + (int)(ry * d);
            if (CellWalkable(cx, cy) != 0) { *outX = cx; *outY = cy; return TRUE; }
        }
    }
    return FALSE;
}

// Is the straight line between two points clear of walls? Sampled every few subtiles against the
// collision map; a cell with no data (an unloaded room) is not treated as a wall, so a hop across
// unknown ground still goes. This is what keeps a long teleport hop from cutting the corner through an
// obstacle: the route bends around the wall, and a hop is only taken as far as the line stays clear.
static BOOL LineClear(int x0, int y0, int x1, int y1) {
    int dx = x1 - x0, dy = y1 - y0;
    int steps = (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) / 3;
    if (steps < 1) steps = 1;
    for (int s = 1; s <= steps; s++) {
        int x = x0 + dx * s / steps, y = y0 + dy * s / steps;
        if (CellWalkable(x, y) == 0) return FALSE;      // 0 blocked, -1 unknown (allowed), 1 clear
    }
    return TRUE;
}

// The next place to teleport to along the planned route: the farthest route point still within one hop
// AND reachable in a straight line, so the character follows the path round obstacles instead of
// teleporting through them. Falls back to the farthest-within-range point if none has a clear line
// (e.g. the whole neighbourhood is unmapped), which the did-it-move retry then sorts out.
static BOOL NextHopOnPath(int px, int py, int maxHop, int* hx, int* hy) {
    if (g_pathLen <= 0) return FALSE;
    int best = -1, within = -1;
    for (int i = g_pathLen - 1; i >= g_pathIdx; i--) {
        int dx = g_pathX[i] - px, dy = g_pathY[i] - py;
        int d = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
        if (d > maxHop) continue;
        if (within < 0) within = i;                              // farthest simply within range
        if (LineClear(px, py, g_pathX[i], g_pathY[i])) { best = i; break; }   // farthest with a clear line
    }
    if (best < 0) best = within;                // nothing had a clear line: take the nearest-blocked one
    if (best < 0) return FALSE;                 // drifted off the route; the caller replans
    int dx = g_pathX[best] - px, dy = g_pathY[best] - py;
    int d = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    if (d < 3) {                                // standing on it already
        if (best + 1 >= g_pathLen) return FALSE;
        best++;
    }
    g_pathIdx = best;
    *hx = g_pathX[best]; *hy = g_pathY[best];
    return TRUE;
}

static void TravelStep(int px, int py) {
    if (!g_traveling) return;
    DWORD now = GetTickCount();

    int dx = g_tx - px, dy = g_ty - py;
    int dist = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    if (dist <= 6) { StopTravel("arrived"); return; }
    if (now - g_travelStart > 120000) { StopTravel("timed out"); return; }

    if (g_moveMode == 2) {
        // Cast again as soon as the character has arrived from the last hop, and otherwise give the
        // cast a fair chance to resolve before deciding it failed. Waiting a fixed interval regardless
        // is what made teleporting feel slow.
        BOOL moved = abs(px - g_teleFromX) > 2 || abs(py - g_teleFromY) > 2;
        DWORD wait = moved ? 120 : 320;
        if (now - g_lastSend < wait) return;
        g_lastSend = now;

        if (moved) g_teleTry = 0; else g_teleTry++;
        g_teleFromX = px; g_teleFromY = py;

        CheckCollisionAgainstPlayer(px, py);
        if (g_pathLen == 0) PlanPath(px, py, g_tx, g_ty);

        // Repeatedly failing to move means the route is wrong about this spot; lay a new one from
        // where we actually are, and only give up once that has stopped helping.
        if (g_teleTry >= 8) {
            g_teleTry = 0;
            if (++g_replans > 5 || !PlanPath(px, py, g_tx, g_ty)) { StopTravel("teleport blocked"); return; }
        }

        int hx, hy;
        if (!NextHopOnPath(px, py, TELE_HOP, &hx, &hy)) {
            // Off the route, or no route could be planned. Fall back to feeling for a way forward.
            if (g_pathLen > 0 && PlanPath(px, py, g_tx, g_ty) && NextHopOnPath(px, py, TELE_HOP, &hx, &hy)) {
                // replanned successfully
            } else if (!PickHop(px, py, dx, dy, dist, g_teleTry, &hx, &hy)) {
                double len = sqrt((double)dx * dx + (double)dy * dy);
                if (len < 1.0) return;
                int hop = dist < TELE_HOP ? dist : TELE_HOP;
                hx = px + (int)(dx / len * hop);
                hy = py + (int)(dy / len * hop);
            }
        }
        SendMove(CMD_TELE, hx, hy);
        return;
    }

    // Walking and running are pathed by the game server, but only within reach: the game never asks to
    // move further than a click on screen, and a destination hundreds of subtiles away is simply
    // ignored. That is why travelling to a waypoint in the same room worked while an area exit did
    // nothing. So the route is followed in steps here too, just longer ones than a teleport takes.
    if (g_lastPx >= 0 && abs(px - g_lastPx) <= 1 && abs(py - g_lastPy) <= 1) {
        if (++g_noProgress >= 24) { StopTravel("stuck"); return; }   // ~6s without moving
    } else g_noProgress = 0;
    g_lastPx = px; g_lastPy = py;

    if (now - g_lastSend < 400) return;
    g_lastSend = now;

    int hx = g_tx, hy = g_ty;
    if (dist > RUN_HOP) {
        if (g_pathLen == 0) PlanPath(px, py, g_tx, g_ty);
        if (!NextHopOnPath(px, py, RUN_HOP, &hx, &hy)) {
            double len = sqrt((double)dx * dx + (double)dy * dy);
            if (len < 1.0) return;
            hx = px + (int)(dx / len * RUN_HOP);
            hy = py + (int)(dy / len * RUN_HOP);
        }
    }
    SendMove(g_moveMode == 1 ? CMD_WALK : CMD_RUN, hx, hy);
}

// Which target a given action would pick. "Next" prefers the lowest destination level above this one,
// which is the main path through every act; "previous" prefers the level we actually came from.
// Returns an index into g_targets, or -1.
static int PickTarget(TargetKind kind, DWORD levelNo, int forward) {
    int best = -1;
    for (int i = 0; i < g_numTargets; i++) {
        Target* t = &g_targets[i];
        if (t->kind != kind) continue;
        if (kind != TK_EXIT) return i;
        if (forward) {
            if ((DWORD)t->levelNo <= levelNo) continue;
            if (best < 0 || t->levelNo < g_targets[best].levelNo) best = i;
        } else {
            if ((DWORD)t->levelNo == g_prevLevel) return i;
            if ((DWORD)t->levelNo >= levelNo) continue;
            if (best < 0 || t->levelNo > g_targets[best].levelNo) best = i;
        }
    }
    return best;
}

static void TravelToKind(TargetKind kind, DWORD levelNo, const char* label, int forward) {
    if (!g_travelEnabled) return;
    if (g_traveling) { StopTravel("cancelled"); return; }
    int idx = PickTarget(kind, levelNo, forward);
    Target* best = idx >= 0 ? &g_targets[idx] : NULL;
    if (!best) { Log("no %s target in this level", label); return; }
    char what[64];
    if (kind == TK_EXIT) snprintf(what, sizeof what, "%s (level %d)", label, best->levelNo);
    else snprintf(what, sizeof what, "%s", label);
    g_travelTarget = idx;
    StartTravel(best->x, best->y, what);
}

static void TravelCycle(void) {
    if (!g_travelEnabled) return;
    if (g_traveling) { StopTravel("cancelled"); return; }
    if (g_numTargets == 0) { Log("no targets in this level"); return; }
    if (g_cycle >= g_numTargets) g_cycle = 0;
    g_travelTarget = g_cycle;
    Target* t = &g_targets[g_cycle++];
    char what[64];
    if (t->kind == TK_EXIT) snprintf(what, sizeof what, "exit to level %d", t->levelNo);
    else { const char* nm = t->kind == TK_WAYPOINT ? "waypoint" : SpecialName(t->txt); snprintf(what, sizeof what, "%s", nm ? nm : "place"); }
    StartTravel(t->x, t->y, what);
}

// ---------------------------------------------------------------- on-screen overlay
// Draws, over the game's own automap, a line from the character to every target with a label naming
// it and the key that goes there. It runs inside the game's draw, from a detour on one of the calls
// the automap draw makes (see drawHookCall), and uses the game's own line and text routines, so it
// scrolls and zooms with the map and looks like part of it.

// Line colours are palette indices, so the exact shade depends on the game's palette; they are read
// from the ini so they can be changed without a rebuild. Text colours are the game's own small set.
#define TEXT_WHITE 0
#define TEXT_GOLD  4

// Fields of UnitAny that did not move between 1.10 and 1.13d.
#define UNIT_dwType     0x00
#define UNIT_dwMode     (g_ver->unit_dwMode)
#define UNIT_pRoomNext  0xE4
#define UNIT_pListNext  0xE8
#define UNIT_TYPE_MONSTER 1

static const char* KeyName(UINT vk) {
    static char buf[12];
    switch (vk) {
        case VK_PRIOR:  return "PgUp";
        case VK_NEXT:   return "PgDn";
        case VK_HOME:   return "Home";
        case VK_END:    return "End";
        case VK_INSERT: return "Ins";
        case VK_DELETE: return "Del";
        default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12)             { sprintf(buf, "F%u", vk - VK_F1 + 1); return buf; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)    { sprintf(buf, "Num%u", vk - VK_NUMPAD0); return buf; }
    sprintf(buf, "0x%02X", vk);
    return buf;
}

static const char* TargetName(const Target* t) {
    if (t->kind == TK_EXIT)
        return (t->levelNo > 0 && (size_t)t->levelNo < NUM_LEVEL_NAMES) ? kLevelNames[t->levelNo] : "exit";
    if (t->kind == TK_WAYPOINT) return "Waypoint";
    const char* s = SpecialName(t->txt);
    return s ? s : "place";
}

// Which key takes you to this target, so the label can say so.
static UINT KeyForTarget(int i) {
    if (g_targets[i].kind == TK_WAYPOINT) return g_keyWaypoint;
    if (g_targets[i].kind == TK_EXIT) {
        if (PickTarget(TK_EXIT, g_seenLevel, 1) == i) return g_keyNext;
        if (PickTarget(TK_EXIT, g_seenLevel, 0) == i) return g_keyPrev;
    }
    return g_keyCycle;
}

// World subtile position to a screen position on the automap.
//
// Deriving this from how the game places automap CELLS put everything about seven pixels left and a
// few low, because a cell is drawn from its own anchor rather than centred on the point it is given.
// These are the constants that put a screen position where the eye expects it; the two nudges are in
// the ini so the last pixel or two can be adjusted without a rebuild.
static void ToScreen(int wx, int wy, int div, int ox, int oy, int* sx, int* sy) {
    int x = wx * 32, y = wy * 32;
    *sx = ((x - y) / 2) / div - ox + 8 + g_overlayDX;
    *sy = ((x + y) / 4) / div - oy - 8 + g_overlayDY;
}

// Where a room keeps its units, and how one unit links to the next in that room, differ per patch - the
// offset a later patch's table gives is a seed field on 1.10 - so both are FOUND rather than assumed.
// A unit is recognisable: its type is one of a handful of small numbers and it carries pointers to an
// act and to a path that both have to be readable.
static int g_unitFirstOff = -1, g_unitNextOff = -1;

static BOOL LooksLikeUnit(void* u) {
    // Only the head of the structure is required to be readable. Demanding 0x110 bytes assumed the
    // 1.13-era UnitAny size; 1.10's structures are smaller (its Room1 is half the size), so that test
    // could reject every real unit and leave nothing to draw.
    if (!Readable(u, 0x40)) return FALSE;
    if (RD(u, UNIT_dwType) > 5) return FALSE;
    if (RD(u, 0x04) > 2000) return FALSE;                  // txt file number
    if (!Readable(RP(u, UNIT_pAct), 0x40)) return FALSE;
    void* path = RP(u, UNIT_pPath);
    if (!Readable(path, 0x40)) return FALSE;
    // The clincher: a unit's path points back at the unit. Without this the test is loose enough that
    // ordinary memory passes it, which is how the search kept settling on a different field each time.
    return RP(path, PATH_pUnit) == u;
}

// A room keeps a separate list per kind of unit, so the field holding OBJECTS is not the one holding
// monsters. Rather than guess which is which, look for the field whose list actually contains a
// monster; the first room with one settles it for the rest of the session.
// A Room1 is only 0x80 bytes, so searching further walks into the NEXT room's fields and settles on
// whatever that one happens to hold - which is exactly why the list appeared to be at a different
// offset each run. The list holds every unit in the room, not only monsters, so the head is any unit.
#define ROOM1_SIZE 0x80

static void DiscoverUnitList(void* room1) {
    if (g_unitFirstOff >= 0 && g_unitNextOff >= 0) return;
    for (int off = 0; off < ROOM1_SIZE; off += 4) {
        void* head = RP(room1, off);
        if (!LooksLikeUnit(head)) continue;

        // Which field chains one unit to the next also moved between patches, and picking the first
        // plausible one found a link that stopped after a single unit. Try every candidate and keep
        // whichever actually walks the longest run of units - a wrong guess chains to nothing.
        int bestN = 0, bestOff = -1;
        for (int n = 0xC0; n < 0x110; n += 4) {
            int count = 1;
            void* u = RP(head, n);
            while (LooksLikeUnit(u) && count < 64) { count++; u = RP(u, n); }
            if (count > bestN) { bestN = count; bestOff = n; }
        }
        if (bestN < 2) continue;        // a single unit teaches nothing; try a room with more
        g_unitFirstOff = off;
        g_unitNextOff = bestOff;
        Log("unit list: Room1+%02X, chained at Unit+%02X (%d in the first chain)", off, g_unitNextOff, bestN);
        return;
    }
}

// THE UNIT TABLE. On 1.10 the client does not keep monsters in a room's unit list - a census of a
// whole level found one list holding nothing but the player - so they have to come from the table the
// game itself keeps, five rows of 128 buckets, one row per kind of unit, chained by pListNext.
//
// Its address is FOUND rather than carried per patch, and the character is the key: the player unit is
// in row 0 at the bucket its id hashes to, so scanning the client's own image for a pointer to the
// player gives the slot, and the slot gives the base. A candidate is only accepted if the monster row
// then really does hold monsters.
#define UNIT_TABLE_ROWS    5
#define UNIT_TABLE_BUCKETS 128

// Is this a live monster? Checked with raw reads guarded individually, because the sweep below looks at
// hundreds of thousands of values and asking the operating system about each one would take far too
// long. The clincher is the same as elsewhere: a unit's path points back at the unit.
static BOOL TryIsMonster(void* p) {
    __try {
        if ((UINT_PTR)p < 0x10000 || ((UINT_PTR)p & 3)) return FALSE;
        if (*(DWORD*)p != UNIT_TYPE_MONSTER) return FALSE;
        if (*(DWORD*)((BYTE*)p + 4) > 2000) return FALSE;                 // txt file number
        void* path = *(void**)((BYTE*)p + UNIT_pPath);
        if ((UINT_PTR)path < 0x10000 || ((UINT_PTR)path & 3)) return FALSE;
        return *(void**)((BYTE*)path + PATH_pUnit) == p;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
}

static BOOL SlotOk(DWORD* s) {     // a table slot is either empty or holds a monster
    __try { DWORD v = *s; return v == 0 || TryIsMonster((void*)v); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
}

// Find the row of the table that holds monsters, by looking for a long run of slots that are all
// either empty or pointing at a monster. Nothing else in the client's data looks like that. Retried
// every few seconds until it succeeds, because a sweep run in town before any monster has loaded has
// nothing to recognise.

static void FindUnitTable(void* player) {
    (void)player;
    if (g_unitTable || !g_d2client) return;
    DWORD now = GetTickCount();
    if (g_lastSweep && now - g_lastSweep < 5000) return;
    g_lastSweep = now;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)g_d2client;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_d2client + dos->e_lfanew);
    DWORD size = nt->OptionalHeader.SizeOfImage;

    DWORD* bestLo = NULL; int bestRun = 0, bestCount = 0;
    DWORD* skipTo = NULL;

    for (DWORD off = 0; off + 4 <= size; off += 4) {
        DWORD* p = (DWORD*)(g_d2client + off);
        if (p < skipTo) continue;                  // already covered by the run just measured
        DWORD v = *p;
        if (v < 0x10000 || (v & 3) || !TryIsMonster((void*)v)) continue;

        // Grow outwards while the neighbouring slots still look like table entries.
        DWORD* lo = p; DWORD* hi = p;
        while (lo > (DWORD*)g_d2client && (DWORD)(p - lo) < UNIT_TABLE_BUCKETS && SlotOk(lo - 1)) lo--;
        while ((BYTE*)(hi + 1) < g_d2client + size && (DWORD)(hi - p) < UNIT_TABLE_BUCKETS && SlotOk(hi + 1)) hi++;
        skipTo = hi + 1;

        // A stretch of zeros with one stray pointer in it also passes "empty or a monster", and that is
        // what the first attempt latched onto. A real row has SEVERAL monsters in it.
        int count = 0;
        for (DWORD* s = lo; s <= hi; s++) if (*s) count++;
        int run = (int)(hi - lo) + 1;
        if (run < 16 || count < 3) continue;
        if (count > bestCount) { bestCount = count; bestRun = run; bestLo = lo; }
    }

    if (!bestLo) { Log("monster table not found yet (no monsters loaded?) - will try again"); return; }

    // The run only covers the buckets that happened to be occupied when the sweep ran. Units hash into
    // the table by id, so the rest of it matters as soon as anything else spawns - taking just the run
    // showed the town's people but not the monster standing next to the character. Widen it well past
    // the run; every slot is validated before use, so the extra ground costs nothing but a few reads.
    DWORD* base = bestLo - 256;
    if (base < (DWORD*)g_d2client) base = (DWORD*)g_d2client;
    int len = bestRun + 512 + (int)(bestLo - base);
    if ((BYTE*)(base + len) > g_d2client + size) len = (int)((DWORD*)(g_d2client + size) - base);

    g_unitTable = base;
    g_unitTableLen = len;
    Log("monster table found at D2Client+%05X: run of %d slot(s), %d in use, scanning %d",
        (DWORD)bestLo - (DWORD)g_d2client, bestRun, bestCount, len);

    // Say who is actually in it. "Only the town's people show" and "the table is the wrong one" look
    // identical from the outside; the monster numbers and positions tell them apart at a glance.
    int shown = 0;
    for (int b = 0; b < len && shown < 10; b++) {
        void* u = (void*)base[b];
        if (!TryIsMonster(u)) continue;
        void* path = RP(u, UNIT_pPath);
        Log("  slot %d: txt=%lu mode=%lu at %d,%d", b, RD(u, 4), RD(u, UNIT_dwMode),
            RW(path, PATH_xPos), RW(path, PATH_yPos));
        shown++;
    }
}

// Every monster in a room the game currently has loaded, as a small cross.
//
// A room holds more than one unit list and which one a monster turns up in varies, so latching onto a
// single field kept finding objects instead. This instead treats EVERY field of a loaded room that
// looks like a unit as a list head and follows both of the links a unit can be chained by, taking
// monsters wherever they appear and discarding repeats. Only loaded rooms have units at all, so the
// number of rooms searched is small - this is the monsters around the character, not the whole level.
#define MAX_SEEN 300

#define UNIT_TABLE_BUCKETS 128
#define CHAIN_GUARD 400          // a bad chain offset must not loop forever

static void DrawOneMonster(void* u, int div, int ox, int oy) {
    DWORD mode = RD(u, UNIT_dwMode);
    if (mode == 0 || mode == 12) return;                    // dead or dying
    void* path = RP(u, UNIT_pPath);
    int sx, sy;
    ToScreen(RW(path, PATH_xPos), RW(path, PATH_yPos), div, ox, oy, &sx, &sy);
    g_drawLine(sx - 3, sy - 3, sx + 3, sy + 3, g_colMonster, 0);
    g_drawLine(sx - 3, sy + 3, sx + 3, sy - 3, g_colMonster, 0);
}

static void DrawMonsters(int div, int ox, int oy) {
    const D2Version* v = g_ver;
    if (!g_showMonsters) return;

    // Best: the game's own monster table, read exactly. It is a hash of 128 buckets, each a chain, so
    // walking the buckets AND their chains reaches every monster the client knows - not just the ones
    // whose pointer happens to sit in the array, which is all the old sweep ever saw.
    if (v->unitMonsters) {
        int drawn = 0;
        void* seen[MAX_SEEN]; int nseen = 0;
        __try {
            DWORD* row = (DWORD*)(g_d2client + v->unitMonsters);
            for (int bkt = 0; bkt < UNIT_TABLE_BUCKETS && drawn < MAX_SEEN; bkt++) {
                void* u = (void*)row[bkt];
                for (int g = 0; g < CHAIN_GUARD && u && drawn < MAX_SEEN; g++) {
                    if (!TryIsMonster(u)) break;
                    BOOL dup = FALSE;
                    for (int i = 0; i < nseen; i++) if (seen[i] == u) { dup = TRUE; break; }
                    if (dup) break;                          // a loop, or a shared tail
                    if (nseen < MAX_SEEN) seen[nseen++] = u;
                    DrawOneMonster(u, div, ox, oy);
                    drawn++;
                    void* nx = RP(u, v->unitChain);
                    if (nx == u) break;
                    u = nx;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* a wrong table for an untested patch: draw nothing */ }
        static DWORD censusLevel;
        if (censusLevel != g_seenLevel) {
            censusLevel = g_seenLevel;
            Log("monsters: %d drawn from the game's monster table (row +%X)", drawn, v->unitMonsters);
        }
        g_monstersDrawn = drawn;
        return;
    }

    // Fallback for a patch with no table located: the runtime sweep.
    if (g_unitTable) {
        int drawn = 0;
        for (int b = 0; b < g_unitTableLen && drawn < MAX_SEEN; b++) {
            void* u = (void*)g_unitTable[b];
            if (!TryIsMonster(u)) continue;
            DrawOneMonster(u, div, ox, oy);
            drawn++;
        }
        static DWORD censusLevel;
        if (censusLevel != g_seenLevel) {
            censusLevel = g_seenLevel;
            int used = 0;
            for (int b = 0; b < g_unitTableLen; b++) if (g_unitTable[b]) used++;
            Log("monsters: %d drawn from the swept table (%d of %d slots non-empty)", drawn, used, g_unitTableLen);
        }
        g_monstersDrawn = drawn;
        return;
    }

    if (!g_curLevel) return;
    static const int links[2] = { UNIT_pRoomNext, UNIT_pListNext };
    void* seen[MAX_SEEN];
    int drawn = 0, rooms = 0;
    int lists = 0, byType[8] = { 0 };
    void* firstUnit = NULL;

    for (void* r2 = RP(g_curLevel, v->level_pRoom2First);
         Readable(r2, 0x100) && rooms < 400 && drawn < MAX_SEEN;
         r2 = RP(r2, v->room2_pNext), rooms++) {
        void* r1 = RP(r2, v->room2_pRoom1);
        if (!Readable(r1, ROOM1_SIZE)) continue;

        for (int off = 0; off < ROOM1_SIZE && drawn < MAX_SEEN; off += 4) {
            void* head = RP(r1, off);
            if (!LooksLikeUnit(head)) continue;
            lists++;
            if (!firstUnit) firstUnit = head;
            for (int l = 0; l < 2 && drawn < MAX_SEEN; l++) {
                int guard = 0;
                void* u = head;
                for (; LooksLikeUnit(u) && guard < 100 && drawn < MAX_SEEN; guard++) {
                    DWORD ty = RD(u, UNIT_dwType);
                    if (ty < 8) byType[ty]++;
                    void* nxt = Readable((BYTE*)u + links[l], 4) ? RP(u, links[l]) : NULL;
                    if (ty != UNIT_TYPE_MONSTER) { u = nxt; continue; }
                    DWORD mode = RD(u, UNIT_dwMode);
                    if (mode == 0 || mode == 12) { u = nxt; continue; }     // dying or dead
                    int dup = 0;
                    for (int i = 0; i < drawn; i++) if (seen[i] == u) { dup = 1; break; }
                    if (dup) { u = nxt; continue; }
                    seen[drawn] = u;

                    void* path = RP(u, UNIT_pPath);
                    int sx, sy;
                    ToScreen(RW(path, PATH_xPos), RW(path, PATH_yPos), div, ox, oy, &sx, &sy);
                    g_drawLine(sx - 3, sy - 3, sx + 3, sy + 3, g_colMonster, 0);
                    g_drawLine(sx - 3, sy + 3, sx + 3, sy - 3, g_colMonster, 0);
                    drawn++;
                }
            }
        }
    }
    // One census per level, so "no monsters on the map" can be told apart from "found them and drew
    // them somewhere unexpected" without anyone having to turn on a diagnostic first. When nothing was
    // drawn it also prints the head of the first unit it did find, which says what the structure
    // really looks like on this patch.
    static DWORD censusLevel;
    if (censusLevel != g_seenLevel) {
        censusLevel = g_seenLevel;
        Log("monsters: %d room(s), %d unit list(s), types player=%d monster=%d object=%d missile=%d item=%d tile=%d, drew %d",
            rooms, lists, byType[0], byType[1], byType[2], byType[3], byType[4], byType[5], drawn);
        if (!drawn && firstUnit && Readable(firstUnit, 0x40)) {
            char line[400]; int n = sprintf(line, "monsters: first unit @%p:", firstUnit);
            for (int i = 0; i < 0x40; i += 4) n += sprintf(line + n, " %08X", RD(firstUnit, i));
            Log("%s", line);
        }
    }
    g_monstersDrawn = drawn;
}

static void OverlayDrawInner(void) {
    const D2Version* v = g_ver;
    if (!v || !g_overlayOn || g_overlayFailed) return;
    if (!g_drawLine || !g_drawText || !g_playerX) return;

    int div = *(int*)(g_d2client + v->automapDivisor);
    if (div <= 0 || div > 4096) return;
    int ox = *(int*)(g_d2client + v->automapOffset);
    int oy = *(int*)(g_d2client + v->automapOffset + 4);

    DrawMonsters(div, ox, oy);

    int px, py;
    ToScreen(g_playerX, g_playerY, div, ox, oy, &px, &py);

    for (int i = 0; i < g_numTargets; i++) {
        Target* t = &g_targets[i];
        int sx, sy;
        ToScreen(t->x, t->y, div, ox, oy, &sx, &sy);
        BOOL active = g_traveling && i == g_travelTarget;
        DWORD colour = active ? g_colActive
                     : t->kind == TK_EXIT ? g_colExit
                     : t->kind == TK_WAYPOINT ? g_colWaypoint : g_colSpecial;
        g_drawLine(px, py, sx, sy, colour, 0);

        wchar_t label[96];
        _snwprintf(label, 96, L"%hs [%hs]", TargetName(t), KeyName(KeyForTarget(i)));
        label[95] = 0;
        g_drawText(label, sx + 6, sy, active ? TEXT_GOLD : TEXT_WHITE, 0);
    }
}

static void OverlayDraw(void) {
    __try { OverlayDrawInner(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_overlayFailed = TRUE;
        Log("overlay disabled after exception %08X", GetExceptionCode());
    }
}

// The list of places you can travel to, with the key that goes to each, drawn just above the control
// bar. This one is in screen space and hangs off the second hook, the call the game makes every frame
// whether or not it drew the automap, so the list is there even with the map closed.
static void PanelDrawInner(void) {
    if (!g_ver || !g_showList || g_overlayFailed) return;
    if (!g_drawText || !g_playerX) return;
    BOOL vitals = g_vitMax > 0;
    if (!g_numTargets && !vitals) return;

    int height = 600;
    RECT rc;
    if (g_hwnd && GetClientRect(g_hwnd, &rc) && rc.bottom > 200) height = rc.bottom;

    BOOL pots = g_nItems > 0;
    int top = height - g_panelBottom - (g_numTargets + (vitals ? 1 : 0) + (pots ? 1 : 0)) * PANEL_LINE;

    // Life and mana as the module reads them, which is also the plainest way to see whether auto
    // potion is watching at all: no line means it cannot read the character.
    if (vitals) {
        int lifePct = g_vitLife * 100 / g_vitMax;
        int manaPct = g_vitManaMax > 0 ? g_vitMana * 100 / g_vitManaMax : 0;
        wchar_t line[96];
        if (g_vitManaMax > 0)
            _snwprintf(line, 96, L"HP %d/%d (%d%%)   MP %d/%d (%d%%)%hs",
                       g_vitLife, g_vitMax, lifePct, g_vitMana, g_vitManaMax, manaPct,
                       g_autoPotion ? "" : "   potions off");
        else
            _snwprintf(line, 96, L"HP %d/%d (%d%%)", g_vitLife, g_vitMax, lifePct);
        line[95] = 0;
        BOOL low = g_autoPotion && (lifePct <= g_lifePct || (g_manaPct > 0 && g_vitManaMax > 0 && manaPct <= g_manaPct));
        g_drawText(line, g_panelX, top, low ? TEXT_GOLD : TEXT_WHITE, 0);
        top += PANEL_LINE;
    }

    // What is left to drink, belt and inventory together. A kind that has not been identified yet is
    // shown as such rather than counted as nothing, so an empty-looking count is never a lie.
    if (pots) {
        wchar_t line[96];
        if (g_nUnknownPots > 0)
            _snwprintf(line, 96, L"Potions: %d HP  %d MP  %d RV   (%d not identified yet)",
                       g_nLifePots, g_nManaPots, g_nRejuvPots, g_nUnknownPots);
        else
            _snwprintf(line, 96, L"Potions: %d HP  %d MP  %d RV",
                       g_nLifePots, g_nManaPots, g_nRejuvPots);
        line[95] = 0;
        BOOL out = g_autoPotion && g_nLifePots == 0 && g_nRejuvPots == 0 && g_nUnknownPots == 0;
        g_drawText(line, g_panelX, top, out ? TEXT_GOLD : TEXT_WHITE, 0);
        top += PANEL_LINE;
    }

    for (int i = 0; i < g_numTargets; i++) {
        wchar_t line[96];
        _snwprintf(line, 96, L"%hs: %hs", KeyName(KeyForTarget(i)), TargetName(&g_targets[i]));
        line[95] = 0;
        BOOL active = g_traveling && i == g_travelTarget;
        g_drawText(line, g_panelX, top + i * PANEL_LINE, active ? TEXT_GOLD : TEXT_WHITE, 0);
    }
}

static void PanelDraw(void) {
    __try { PanelDrawInner(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_showList = FALSE;
        Log("place list disabled after exception %08X", GetExceptionCode());
    }
}

// Replaces one `call` inside the automap draw with a call to this, which performs the original call
// and then draws our overlay. Detouring a call rather than patching a function entry means no
// instructions have to be relocated.
static __declspec(naked) void CellDrawDetour(void) {
    __asm {
        call dword ptr [g_origCellDraw]
        pushad
        pushfd
        call OverlayDraw
        popfd
        popad
        ret
    }
}

static __declspec(naked) void FrameDetour(void) {
    __asm {
        call dword ptr [g_origFrame]
        pushad
        pushfd
        call PanelDraw
        popfd
        popad
        ret
    }
}

// Both hooks are installed the same way: rewrite the target of one `call` so it comes to us, we run
// the original, then we draw. Each verifies the call really is the one expected before touching it.
static BOOL PatchCall(DWORD site, DWORD expected, void* detour, void** original, const char* what) {
    BYTE* p = (BYTE*)site;
    if (!Readable(p, 5) || p[0] != 0xE8) { Log("%s: no call instruction at the hook site", what); return FALSE; }
    DWORD target = (DWORD)(p + 5) + *(DWORD*)(p + 1);
    if (target == (DWORD)detour) return TRUE;                       // already patched
    if (target != expected) {
        Log("%s: the call goes to %08X, not the expected %08X", what, target, expected);
        return FALSE;
    }
    *original = (void*)target;
    DWORD rel = (DWORD)((BYTE*)detour - (p + 5)), old;
    VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old);
    *(DWORD*)(p + 1) = rel;
    VirtualProtect(p + 1, 4, old, &old);
    Log("%s: hook installed at %p", what, p);
    return TRUE;
}

static void InstallDrawHook(const D2Version* v) {
    g_overlayHooked = FALSE;
    if (!v->drawHookCall || !g_drawLine || !g_drawText) return;
    BYTE* site = g_d2client + v->drawHookCall;
    if (!Readable(site, 5) || site[0] != 0xE8) { Log("overlay: no call instruction at the hook site"); return; }

    DWORD target = (DWORD)(site + 5) + *(DWORD*)(site + 1);
    if (target == (DWORD)CellDrawDetour) { g_overlayHooked = TRUE; return; }     // already patched
    if (target != (DWORD)(g_d2client + v->drawHookTarget)) {
        Log("overlay: the call at the hook site goes to %08X, not the expected %08X - overlay off",
            target, (DWORD)(g_d2client + v->drawHookTarget));
        return;
    }
    g_origCellDraw = (void*)target;
    DWORD rel = (DWORD)((BYTE*)CellDrawDetour - (site + 5)), old;
    VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old);
    *(DWORD*)(site + 1) = rel;
    VirtualProtect(site + 1, 4, old, &old);
    g_overlayHooked = TRUE;
    Log("overlay: draw hook installed at %p", site);

    if (v->drawHookCall2)
        PatchCall((DWORD)(g_d2client + v->drawHookCall2), (DWORD)(g_d2client + v->drawHookTarget2),
                  FrameDetour, &g_origFrame, "place list");
}

// ---------------------------------------------------------------- structure dump (offset discovery)
static DWORD g_dumpedLevel;

static void DumpFields(const char* what, void* p, int len) {
    for (int off = 0; off < len; off += 4) {
        if (!Readable((BYTE*)p + off, 4)) break;
        DWORD v = RD(p, off);
        char note[200] = "";
        if (Readable((void*)v, 0x20)) {
            int n = sprintf(note, "  ->");
            for (int i = 0; i < 0x20; i += 4) n += sprintf(note + n, " %08X", RD((void*)v, i));
        }
        Log("  %s+%03X = %08X%s", what, off, v, note);
    }
}

static void DumpRoom2(void* r2, void* level, DWORD levelNo) {
    Log("---- dump: level %lu  Room2 @%p  Level @%p", levelNo, r2, level);
    DumpFields("Room2", r2, 0x100);
    int idx = 0;
    for (void* r = RP(level, g_ver->level_pRoom2First); Readable(r, 0x100) && idx < 4; r = RP(r, g_ver->room2_pNext)) {
        void* head = RP(r, g_ver->room2_pPreset);
        if (!Readable(head, 0x24)) continue;
        idx++;
        Log("  -- presets of room @%p:", r);
        int node = 0;
        for (void* p = head; Readable(p, 0x24) && node < 8; node++) {
            char line[200]; int n = sprintf(line, "    node%02d", node);
            for (int i = 0; i < 0x24; i += 4) n += sprintf(line + n, " %08X", RD(p, i));
            Log("%s", line);
            p = RP(p, g_ver->preset_next);
        }
    }
}

// ---------------------------------------------------------------- reveal
static void CallReveal(void* room1, void* layer) {
    if (g_ver->revealIsFastcall) ((RevealFast_t)g_reveal)(room1, 1, layer);
    else                         ((RevealStd_t)g_reveal)(room1, 1, layer);
}

static void RevealLevel(void* level, void* act, void* layer, DWORD levelNo) {
    const D2Version* v = g_ver;
    int rooms = 0, loaded = 0, guard = 0;
    for (void* r2 = RP(level, v->level_pRoom2First); r2 && guard < 8192; r2 = RP(r2, v->room2_pNext), guard++) {
        int x = (int)RD(r2, v->room2_posX), y = (int)RD(r2, v->room2_posY);
        BOOL added = FALSE;
        if (!RP(r2, v->room2_pRoom1)) { g_addRoomData(act, (int)levelNo, x, y, NULL); added = TRUE; }
        void* r1 = RP(r2, v->room2_pRoom1);
        if (!r1) continue;
        CallReveal(r1, layer);
        rooms++;
        if (added) { g_removeRoomData(act, (int)levelNo, x, y, r1); loaded++; }
    }
    Log("revealed level %lu: %d rooms (%d loaded on demand)", levelNo, rooms, loaded);
}

// Runs on the game thread, once per beat.
static void Tick(void) {
    const D2Version* v = g_ver;
    void* player = RP(g_d2client, v->playerUnit);
    if (!player) {                                   // not in a game
        if (g_lastLevel || g_traveling) StopTravel("left the game");
        g_lastLevel = g_seenLevel = g_markedLevel = 0;
        g_numTargets = 0; g_playerX = g_playerY = 0; g_curLevel = NULL;   // no stale overlay
        return;
    }
    void* path = RP(player, UNIT_pPath);        if (!Readable(path, 0x20)) return;
    void* room1 = RP(path, PATH_pRoom1);        if (!Readable(room1, 0x80)) return;
    void* room2 = RP(room1, v->room1_pRoom2);   if (!Readable(room2, 0x100)) return;
    void* level = RP(room2, v->room2_pLevel);   if (!Readable(level, 0x210)) return;
    DWORD levelNo = RD(level, v->level_levelNo);
    void* act = g_ver->actGlobal ? *(void**)(g_d2client + g_ver->actGlobal) : RP(player, UNIT_pAct);
    if (!Readable(act, 0x50)) return;
    int px = RW(path, PATH_xPos), py = RW(path, PATH_yPos);
    g_playerX = px; g_playerY = py;             // the overlay draws from here, on the game's draw thread
    g_curLevel = level;
    if (g_showMonsters && !g_ver->unitMonsters) FindUnitTable(player);   // sweep only where the table is unknown
    int hp, hpMax, mp, mpMax;
    ReadVitals(player, &hp, &hpMax, &mp, &mpMax);    // also feeds the HP/MP line on the panel
    RefreshItems(player);                            // and the potion counts beside it
    AutoPotion(player);

    void* layer = RP(g_d2client, v->automapLayer);
    BOOL layerOk = Readable(layer, 0x1C);

    BOOL force = InterlockedExchange(&g_forceReveal, 0) != 0;
    // Only act once level AND layer have held still for a full beat: right after a level change the
    // game may not have switched automap layers yet, and revealing into the old layer would paint the
    // new level's rooms onto the previous level's map.
    BOOL settled = (levelNo == g_seenLevel && (DWORD)layer == g_seenLayer);
    if (levelNo != g_seenLevel && g_seenLevel) {
        g_prevLevel = g_seenLevel;
        g_lastColl = NULL;              // the cached collision room belongs to the level we just left
        StopTravel("changed level");
    }
    g_seenLevel = levelNo; g_seenLayer = (DWORD)layer;

    if (settled && (force || levelNo != g_lastLevel || (DWORD)layer != g_lastLayer)) {
        if (g_maphackOn && layerOk) RevealLevel(level, act, layer, levelNo);
        if (force || levelNo != g_lastLevel) {
            BuildTargets(level, levelNo, px, py);
            // Check the collision grid against the one cell we know is standable: the one the character
            // is on. Done on entering a level so the log says whether teleport can trust it.
            int here = CellWalkable(px, py);
            if (here < 0) {
                Log("collision map: not found in this level");
                // Once, dump the character's Room1 so the real collision offset can be read off a log
                // from a patch where the finder comes up empty. Each line is a field and, if it looks
                // like a pointer, the eight dwords it points at - a collision map header stands out.
                static BOOL dumped;
                void* r1 = RP(RP(player, UNIT_pPath), PATH_pRoom1);
                if (!dumped && Readable(r1, 0x80)) {
                    dumped = TRUE;
                    Log("room1 @%p dump:", r1);
                    for (int o = 0; o < 0x80; o += 4) {
                        void* p = RP(r1, o);
                        if (Readable(p, 0x20)) {
                            DWORD* h = (DWORD*)p;
                            Log("  +%02X -> %p : %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX",
                                o, p, h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
                        } else {
                            Log("  +%02X = %08lX", o, (DWORD)(UINT_PTR)p);
                        }
                    }
                }
            }
            else if (here == 0) Log("collision map: says the character is inside a wall - not trusted");
            else {
                // A mask that never matches would silently let teleport walk into walls again, so say
                // how much of the room it marks blocked: neither none nor all of it is what we want.
                int blocked = 0, total = 0;
                void* c = g_lastColl;
                if (Readable(c, 0x28)) {
                    DWORD sx = RD(c, COLL_SIZE_X), sy = RD(c, COLL_SIZE_Y);
                    WORD* g = (WORD*)RP(c, COLL_MAP_START);
                    if (Readable(g, sx * sy * 2)) {
                        total = (int)(sx * sy);
                        for (int i = 0; i < total; i++) if (g[i] & g_collMask) blocked++;
                    }
                }
                Log("collision map: usable (%d%% of the character's room is blocked)",
                    total ? blocked * 100 / total : -1);
            }
            CheckCollisionAgainstPlayer(px, py);
        }
        g_lastLevel = levelNo; g_lastLayer = (DWORD)layer;
        if (g_dump && g_dumpedLevel != levelNo) {
            g_dumpedLevel = levelNo;
            DumpFields("Room1", room1, 0x100);
            // A collision map repeats the room's own position and size, so any field pointing at a
            // struct starting with the same two numbers is the candidate; show it in full.
            for (int off = 0; off < 0x100; off += 4) {
                void* c = RP(room1, off);
                if (!Readable(c, 0x30)) continue;
                if (RD(c, 0) != RD(room1, 0) || RD(c, 4) != RD(room1, 4)) continue;
                Log("  -- collision candidate at Room1+%02X:", off);
                DumpFields("Coll", c, 0x30);
            }
            DumpRoom2(room2, level, levelNo);
        }
    }

    if (g_markTargets && layerOk && g_numTargets &&
        (g_markedLevel != levelNo || g_markedLayer != (DWORD)layer)) {
        g_markedLevel = levelNo; g_markedLayer = (DWORD)layer;
        MarkTargets(layer);
    }

}

// Travel is stepped far more often than the heartbeat, because a teleport that only gets a chance to
// fire every quarter second crawls. This reads nothing but the character's position, so it is cheap.
static void TravelTick(void) {
    __try {
        if (!g_traveling || !g_ver || !g_d2client) return;
        void* player = RP(g_d2client, g_ver->playerUnit);
        if (!player) { StopTravel("left the game"); return; }
        void* path = RP(player, UNIT_pPath);
        if (!Readable(path, 0x20)) return;
        TravelStep(RW(path, PATH_xPos), RW(path, PATH_yPos));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_traveling = FALSE; g_travelTarget = -1;
        Log("travel stopped after exception %08X", GetExceptionCode());
    }
}

static void SafeTick(void) {
    __try { Tick(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults++;
        Log("exception %08X on the heartbeat (fault %d)", GetExceptionCode(), g_faults);
        g_lastLevel = 0; g_traveling = FALSE;
        if (g_faults >= 3) { InterlockedExchange(&g_maphackOn, 0); g_travelEnabled = FALSE; Log("features disabled after repeated faults"); }
    }
}

// ---------------------------------------------------------------- game-thread heartbeat
// Is this one of our hotkeys? Keys we act on are swallowed rather than passed to the game, so binding
// one to, say, F5 does not also switch the character's skill.
static BOOL IsOurKey(UINT vk) {
    if (vk == g_keyToggle || vk == g_keyReveal) return TRUE;
    if (!g_travelEnabled) return FALSE;
    return vk == g_keyNext || vk == g_keyPrev || vk == g_keyWaypoint || vk == g_keyCycle;
}

static void OnKey(UINT vk) {
    if (vk == g_keyToggle) {
        LONG on = !g_maphackOn; InterlockedExchange(&g_maphackOn, on);
        g_lastLevel = 0; if (on) InterlockedExchange(&g_forceReveal, 1);
        MessageBeep(on ? MB_OK : MB_ICONHAND);
        Log("maphack %s (key)", on ? "ON" : "OFF");
    } else if (vk == g_keyReveal) {
        InterlockedExchange(&g_forceReveal, 1);
    } else if (!g_ver || !g_travelEnabled) {
        return;
    } else if (vk == g_keyNext) {
        TravelToKind(TK_EXIT, g_seenLevel, "next area", 1);
    } else if (vk == g_keyPrev) {
        TravelToKind(TK_EXIT, g_seenLevel, "previous area", 0);
    } else if (vk == g_keyWaypoint) {
        TravelToKind(TK_WAYPOINT, g_seenLevel, "waypoint", 1);
    } else if (vk == g_keyCycle) {
        TravelCycle();
    }
}

// One beat of work, whichever way we got onto the game's thread.
static void Beat(void) {
    static BOOL first = TRUE;
    if (first) { first = FALSE; Log("heartbeat running on the game thread"); }
    DWORD now = GetTickCount();
    if (now - g_lastBeat >= BEAT_MS) {
        g_lastBeat = now;
        if (EnsureResolved()) SafeTick();
    }
    if (g_traveling && now - g_lastTravelStep >= 50) { g_lastTravelStep = now; TravelTick(); }
}

static LRESULT CALLBACK GetMsgHook(int code, WPARAM wParam, LPARAM lParam) {
    // wParam is the PeekMessage flags; the game passes PM_REMOVE|PM_NOYIELD, so test the bit, not
    // equality. Messages merely peeked (PM_NOREMOVE) are skipped or a key press would count twice.
    if (code >= 0 && (wParam & PM_REMOVE)) {
        MSG* m = (MSG*)lParam;
        if ((m->message == WM_KEYDOWN || m->message == WM_KEYUP) && IsOurKey((UINT)m->wParam)) {
            if (m->message == WM_KEYDOWN && !(m->lParam & (1 << 30))) OnKey((UINT)m->wParam);
            m->message = WM_NULL;        // keep it away from the game's own key handling
        }
        Beat();
    }
    return CallNextHookEx(g_hook, code, wParam, lParam);
}

// Which game, which Windows, which copy of this module. Printed at load because when a client does
// nothing at all the answer is usually one of the three, and a log from someone else's machine is all
// there is to go on. RtlGetVersion is asked for the build number because GetVersionEx reports whatever
// the application manifest claims to support rather than the truth.
typedef struct { ULONG size, major, minor, build, platform; WCHAR csd[128]; } OsVersion;
typedef LONG (WINAPI *RtlGetVersion_t)(OsVersion*);

static void LogEnvironment(void) {
    char exe[MAX_PATH] = "?", self[MAX_PATH] = "?";
    GetModuleFileNameA(NULL, exe, sizeof exe);
    GetModuleFileNameA(g_self, self, sizeof self);

    OsVersion v; memset(&v, 0, sizeof v); v.size = sizeof v;
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    RtlGetVersion_t get = nt ? (RtlGetVersion_t)GetProcAddress(nt, "RtlGetVersion") : NULL;
    if (get) get(&v);

    BOOL wow = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow);
    Log("  game: %s", exe);
    Log("  module: %s", self);
    Log("  windows %lu.%lu build %lu, %s", v.major, v.minor, v.build, wow ? "64-bit" : "32-bit");
}

// ---------------------------------------------------------------- window fallback
//
// If the message hook cannot be installed the client is dead weight: nothing else runs on the game's
// thread. That happens - a machine has turned up refusing SetWindowsHookEx outright (error 87) - so
// there is a second way in that asks Windows for nothing unusual: put ourselves in front of the game
// window's own message handling. It sees the same messages on the same thread, including the timer
// that keeps the beat when nobody is typing, and hands everything but our own keys straight on.
static HWND    g_subWnd;
static WNDPROC g_subOld;

static LRESULT CALLBACK OurWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    WNDPROC old = g_subOld;
    if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && IsOurKey((UINT)wp)) {
        if (msg == WM_KEYDOWN && !(lp & (1 << 30))) OnKey((UINT)wp);
        Beat();
        return 0;                        // ours: the game never sees it
    }
    Beat();
    return old ? CallWindowProcA(old, h, msg, wp, lp) : DefWindowProcA(h, msg, wp, lp);
}

static void TakeOverWindow(HWND h) {
    if (g_subWnd == h || !h) return;
    SetLastError(0);
    WNDPROC old = (WNDPROC)(LONG_PTR)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)OurWndProc);
    if (!old) { Log("could not attach to the game window either (error %lu)", GetLastError()); return; }
    g_subOld = old; g_subWnd = h;
    Log("heartbeat attached to the game window instead of a message hook");
}

static BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM lp) {
    DWORD pid; GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    char cls[64]; GetClassNameA(hwnd, cls, sizeof cls);
    if (strcmp(cls, "Diablo II") == 0) { *(HWND*)lp = hwnd; return FALSE; }
    return TRUE;
}

// The game runs on the process's first thread; find it as the thread with the earliest creation time.
static DWORD MainThreadId(void) {
    DWORD best = 0; ULONGLONG bestTime = ~0ULL;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te; te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        FILETIME c, e, k, u;
        if (GetThreadTimes(h, &c, &e, &k, &u)) {
            ULONGLONG t = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
            if (t < bestTime) { bestTime = t; best = te.th32ThreadID; }
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return best;
}

static DWORD WINAPI Worker(LPVOID p) {
    (void)p;
    // The game's own thread exists from the start (we were injected while it was suspended), so the
    // heartbeat can be armed right away; the game DLLs are picked up by EnsureResolved as they load.
    // Hooking the game thread can fail if we get there before the thread is ready, so keep trying for
    // a few seconds rather than giving up - a client that loses the hook does nothing at all.
    for (int attempt = 1; attempt <= 40 && !g_hook; attempt++) {
        // Hook the thread that actually pumps the game's messages once its window exists, rather than
        // whichever thread started first; before that, the first thread is the only candidate there is.
        HWND w = NULL; EnumWindows(FindGameWindow, (LPARAM)&w);
        if (w) g_hwnd = w;
        g_mainTid = w ? GetWindowThreadProcessId(w, NULL) : MainThreadId();
        // The module handle is this DLL. Passing NULL is allowed by the documentation for a thread in
        // this same process, and worked everywhere it was tried, but at least one machine refused it
        // with error 87 - a hook procedure living in a library is better off naming the library.
        if (g_mainTid) g_hook = SetWindowsHookExA(WH_GETMESSAGE, GetMsgHook, g_self, g_mainTid);
        if (g_hook) { Log("message hook on thread %lu: ok (attempt %d)", g_mainTid, attempt); break; }
        if (attempt == 1 || attempt == 10 || attempt == 40)
            Log("message hook on thread %lu failed (error %lu), attempt %d, window %s",
                g_mainTid, GetLastError(), attempt, w ? "up" : "not up yet");
        Sleep(250);
    }
    if (!g_hook)
        Log("no message hook after ten seconds - attaching to the game window instead");

    // Said once if the game never shows its window. A log that simply stops after the first line means
    // the client itself went away, which is a different problem from anything this module does.
    DWORD waiting = GetTickCount();
    BOOL saidNoWindow = FALSE;
    int beats = 0;

    // Keep a timer on the game window so the heartbeat fires even when no input arrives; re-arm it
    // if the window is recreated (entering a game does that).
    for (;;) {
        if (!IsWindow(g_hwnd) || (!g_hook && g_subWnd != g_hwnd)) {
            HWND h = NULL; EnumWindows(FindGameWindow, (LPARAM)&h);
            if (h) {
                g_hwnd = h;
                SetTimer(h, TIMER_ID, BEAT_MS, NULL);
                if (!g_hook) { g_subWnd = NULL; TakeOverWindow(h); }   // the window is made again in game
                Log("game window %p, heartbeat armed", h);
            } else if (!saidNoWindow && GetTickCount() - waiting > 20000) {
                saidNoWindow = TRUE;
                Log("twenty seconds and the game still has no window - it may not be starting at all");
            }
        }
        // A line every ten seconds for the first minute, so the log shows how far the client got even
        // when nothing else happens. Without it a client that dies early leaves two lines and no story.
        if (beats < 6 && GetTickCount() - waiting > (DWORD)(10000 * (beats + 1))) {
            beats++;
            Log("waiting: window %s, heartbeat %s, game dlls %s",
                IsWindow(g_hwnd) ? "up" : "none",
                g_hook ? "hooked" : g_subWnd ? "on the window" : "NOT RUNNING",
                g_ver ? "loaded" : "not loaded yet");
        }
        Sleep(1000);
    }
}

// ---------------------------------------------------------------- entry
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        // Settings and log live in %LOCALAPPDATA%\D2FUtil, the folder the launcher owns - NOT next to
        // this DLL, which during development is injected straight out of the build tree.
        const char* local = getenv("LOCALAPPDATA");
        if (local && *local) snprintf(g_dir, sizeof g_dir, "%s\\D2FUtil", local);
        else {
            GetModuleFileNameA(h, g_dir, sizeof g_dir);
            char* s = strrchr(g_dir, '\\'); if (s) *s = 0;
        }
        ReadIni();
        if (g_logEnabled) {
            char lp[MAX_PATH]; snprintf(lp, sizeof lp, "%s\\d2fmh.log", g_dir);
            g_log = fopen(lp, "a");
        }
        Log("d2fmh loaded into pid %lu (maphack=%ld travel=%d move=%d potion=%d life<%d%% mana<%d%%)",
             GetCurrentProcessId(), g_maphackOn, g_travelEnabled, g_moveMode, g_autoPotion, g_lifePct, g_manaPct);
        LogEnvironment();
        Install(&hFindA, "user32.dll", "FindWindowA", MyFindWindowA);
        Install(&hFindW, "user32.dll", "FindWindowW", MyFindWindowW);
        Install(&hFindExA, "user32.dll", "FindWindowExA", MyFindWindowExA);
        Install(&hFindExW, "user32.dll", "FindWindowExW", MyFindWindowExW);
        Install(&hMutexA, "kernel32.dll", "CreateMutexA", MyCreateMutexA);
        Install(&hMutexW, "kernel32.dll", "CreateMutexW", MyCreateMutexW);
        Install(&hOpenMutexA, "kernel32.dll", "OpenMutexA", MyOpenMutexA);
        Install(&hOpenMutexW, "kernel32.dll", "OpenMutexW", MyOpenMutexW);
        HANDLE t = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    // Says so when the client goes away, so that a log which stops after two lines can be told apart
    // from one whose game quit thirty seconds in.
    else if (reason == DLL_PROCESS_DETACH && g_log) {
        // lpReserved says which kind of detach this is, and the difference matters: the game quitting
        // on its own is one problem, something unloading this module out from under a running game is
        // a completely different one.
        Log(r ? "client %lu is exiting" : "client %lu unloaded this module while still running",
            GetCurrentProcessId());
        fclose(g_log);
        g_log = NULL;
    }
    return TRUE;
}

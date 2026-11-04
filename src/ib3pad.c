/*
 * IB3Pad: XInput controller support for the Infinity Blade III PC port.
 *
 * Drop-in proxy for xinput1_3.dll. IB3.exe imports XInputGetState (ordinal 2)
 * and XInputSetState (ordinal 3) from xinput1_3.dll, so placing this DLL next
 * to IB3.exe makes Windows load it instead of the system copy. Every XInput
 * export is forwarded to the real system DLL.
 *
 * A background thread reads the controller and translates it into the
 * keyboard and mouse input the port already understands (launcher keybinds
 * and mouse swipes). No game or config file is modified.
 *
 * Settings: ib3pad.ini next to the DLL. Diagnostics: ib3pad.log.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IB3PAD_VERSION "0.6.0-beta7.1"
#define ERR_NOT_CONNECTED 1167u

/* ---------- XInput types (declared locally to avoid clashing with xinput.h) ---------- */
typedef struct { WORD wButtons; BYTE bLeftTrigger; BYTE bRightTrigger;
                 SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XPad;
typedef struct { DWORD dwPacketNumber; XPad Gamepad; } XState;
typedef struct { WORD wLeftMotorSpeed; WORD wRightMotorSpeed; } XVib;

typedef DWORD (WINAPI *PFN_GetState)(DWORD, XState *);
typedef DWORD (WINAPI *PFN_SetState)(DWORD, XVib *);
typedef DWORD (WINAPI *PFN_GetCaps)(DWORD, DWORD, void *);
typedef void  (WINAPI *PFN_Enable)(BOOL);
typedef DWORD (WINAPI *PFN_GetDSound)(DWORD, GUID *, GUID *);
typedef DWORD (WINAPI *PFN_GetBattery)(DWORD, BYTE, void *);
typedef DWORD (WINAPI *PFN_GetKeystroke)(DWORD, DWORD, void *);
typedef DWORD (WINAPI *PFN_WaitGuide)(DWORD, DWORD, void *);
typedef DWORD (WINAPI *PFN_OneArg)(DWORD);

static struct {
    HMODULE mod;
    char path[MAX_PATH];
    PFN_GetState GetState, GetStateEx;
    PFN_SetState SetState;
    PFN_GetCaps GetCaps;
    PFN_Enable Enable;
    PFN_GetDSound GetDSound;
    PFN_GetBattery GetBattery;
    PFN_GetKeystroke GetKeystroke;
    PFN_WaitGuide WaitGuide;
    PFN_OneArg CancelGuide, PowerOff;
} R;

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static HINSTANCE g_self;
static char g_dir[MAX_PATH];
static volatile LONG g_pass = 0; /* PassThroughToGame */

#define GPA(T, name) ((T)(void *)GetProcAddress(R.mod, name))

static BOOL CALLBACK load_real_cb(PINIT_ONCE once, PVOID arg, PVOID *ctx)
{
    static const char *names[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    char sys[MAX_PATH];
    UINT n = GetSystemDirectoryA(sys, MAX_PATH);
    (void)once; (void)arg; (void)ctx;
    if (n == 0 || n >= MAX_PATH)
        return TRUE;
    for (int i = 0; i < 3 && !R.mod; i++) {
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\%s", sys, names[i]);
        HMODULE h = LoadLibraryA(p);
        if (h == g_self) { FreeLibrary(h); continue; }
        if (h) { R.mod = h; lstrcpynA(R.path, p, MAX_PATH); }
    }
    if (!R.mod)
        return TRUE;
    R.GetState     = GPA(PFN_GetState, "XInputGetState");
    R.SetState     = GPA(PFN_SetState, "XInputSetState");
    R.GetCaps      = GPA(PFN_GetCaps, "XInputGetCapabilities");
    R.Enable       = GPA(PFN_Enable, "XInputEnable");
    R.GetDSound    = GPA(PFN_GetDSound, "XInputGetDSoundAudioDeviceGuids");
    R.GetBattery   = GPA(PFN_GetBattery, "XInputGetBatteryInformation");
    R.GetKeystroke = GPA(PFN_GetKeystroke, "XInputGetKeystroke");
    R.GetStateEx   = GPA(PFN_GetState, (LPCSTR)100);
    R.WaitGuide    = GPA(PFN_WaitGuide, (LPCSTR)101);
    R.CancelGuide  = GPA(PFN_OneArg, (LPCSTR)102);
    R.PowerOff     = GPA(PFN_OneArg, (LPCSTR)103);
    return TRUE;
}

static void ensure_real(void) { InitOnceExecuteOnce(&g_once, load_real_cb, NULL, NULL); }

static char g_ini[MAX_PATH];
static char g_userini[MAX_PATH], g_state[MAX_PATH];
static int g_cam_applied;
static INIT_ONCE g_pass_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK read_pass_cb(PINIT_ONCE once, PVOID arg, PVOID *ctx)
{
    (void)once; (void)arg; (void)ctx;
    g_pass = GetPrivateProfileIntA("General", "PassThroughToGame", 0, g_ini) ? 1 : 0;
    return TRUE;
}
/* Read on first use (never inside DllMain) so the game's earliest calls see the real setting. */
static int pass_enabled(void)
{
    InitOnceExecuteOnce(&g_pass_once, read_pass_cb, NULL, NULL);
    return (int)g_pass;
}

/* ---------- exports seen by the game ---------- */
/* Diagnostic for the 0.6 work: which thread the game polls the controller from, and how often. */
static volatile LONG g_poll_calls;
static volatile DWORD g_poll_tid;
DWORD WINAPI XInputGetState(DWORD i, XState *s)
{
    InterlockedIncrement(&g_poll_calls);
    g_poll_tid = GetCurrentThreadId();
    if (!pass_enabled()) return ERR_NOT_CONNECTED;
    ensure_real();
    return R.GetState ? R.GetState(i, s) : ERR_NOT_CONNECTED;
}
DWORD WINAPI XInputGetStateEx(DWORD i, XState *s)
{
    if (!pass_enabled()) return ERR_NOT_CONNECTED;
    ensure_real();
    if (R.GetStateEx) return R.GetStateEx(i, s);
    return R.GetState ? R.GetState(i, s) : ERR_NOT_CONNECTED;
}
DWORD WINAPI XInputSetState(DWORD i, XVib *v)
{
    if (!pass_enabled()) return ERR_NOT_CONNECTED;
    ensure_real();
    return R.SetState ? R.SetState(i, v) : ERR_NOT_CONNECTED;
}
DWORD WINAPI XInputGetCapabilities(DWORD i, DWORD f, void *c)
{
    if (!pass_enabled()) return ERR_NOT_CONNECTED;
    ensure_real();
    return R.GetCaps ? R.GetCaps(i, f, c) : ERR_NOT_CONNECTED;
}
void WINAPI XInputEnable(BOOL e)
{
    /* When hidden from the game, never let it disable XInput for our reader. */
    if (!pass_enabled()) return;
    ensure_real();
    if (R.Enable) R.Enable(e);
}
DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD i, GUID *a, GUID *b)
{
    if (!pass_enabled()) return ERR_NOT_CONNECTED;
    ensure_real();
    return R.GetDSound ? R.GetDSound(i, a, b) : ERR_NOT_CONNECTED;
}
DWORD WINAPI XInputGetBatteryInformation(DWORD i, BYTE t, void *b)
{
    if (!pass_enabled()) return ERR_NOT_CONNECTED;
    ensure_real();
    return R.GetBattery ? R.GetBattery(i, t, b) : ERR_NOT_CONNECTED;
}
DWORD WINAPI XInputGetKeystroke(DWORD i, DWORD r, void *k)
{
    if (!pass_enabled()) return ERR_NOT_CONNECTED;
    ensure_real();
    return R.GetKeystroke ? R.GetKeystroke(i, r, k) : ERR_NOT_CONNECTED;
}
DWORD WINAPI XInputWaitForGuideButton(DWORD i, DWORD f, void *l)
{
    if (!pass_enabled()) return ERR_NOT_CONNECTED;
    ensure_real();
    return R.WaitGuide ? R.WaitGuide(i, f, l) : ERR_NOT_CONNECTED;
}
DWORD WINAPI XInputCancelGuideButtonWait(DWORD i)
{
    if (!pass_enabled()) return ERR_NOT_CONNECTED;
    ensure_real();
    return R.CancelGuide ? R.CancelGuide(i) : ERR_NOT_CONNECTED;
}
DWORD WINAPI XInputPowerOffController(DWORD i)
{
    if (!pass_enabled()) return ERR_NOT_CONNECTED;
    ensure_real();
    return R.PowerOff ? R.PowerOff(i) : ERR_NOT_CONNECTED;
}

/* ---------- logging ---------- */
static FILE *g_log;
static void logf_(const char *fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME t;
    va_list ap;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* ---------- buttons, actions, config ---------- */
enum { B_A, B_B, B_X, B_Y, B_LB, B_RB, B_LT, B_RT, B_START, B_BACK, B_L3, B_R3,
       B_DUP, B_DDOWN, B_DLEFT, B_DRIGHT, NBTN };
static const char *BTN_NAMES[NBTN] = { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Start", "Back",
                                       "L3", "R3", "DPadUp", "DPadDown", "DPadLeft", "DPadRight" };
static const DWORD BTN_MASK[NBTN] = { 0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0x10000, 0x20000,
                                      0x0010, 0x0020, 0x0040, 0x0080, 0x0001, 0x0002, 0x0004, 0x0008 };
#define DPAD_MASK 0x000Fu

enum { A_NONE, A_KEY, A_LCLICK, A_RCLICK, A_MCLICK, A_WUP, A_WDOWN, A_WLEFT, A_WRIGHT };
typedef struct { int type; WORD vk; } Act1;
/* One button can send up to 3 inputs, written "F+Enter" in the ini. */
typedef struct { int type; int n; Act1 p[3]; char name[48]; } Act;

static const char *SLASH_DIRS[8] = { "Right", "UpRight", "Up", "UpLeft", "Left", "DownLeft", "Down", "DownRight" };

/* Every controller action is one of the game's own commands, bound for the session to a key the
 * game does not use, so it works whatever keyboard bindings the player has set in the launcher.
 * Commands are copied from the port's DefaultInput.ini. The first twelve are the slash directions,
 * in 30 degree steps counter-clockwise from "swipe right". `name` is what ib3pad.ini uses; `def`
 * is the port's default key, used only if the session bindings could not be written. */
static const struct { const char *key; WORD vk; const char *cmd; const char *name; WORD def; } SLASH12[] = {
    { "NumPadZero",   VK_NUMPAD0,    "SlashLeft",          "SwipeRight",     0 },
    { "NumPadOne",    VK_NUMPAD1,    "SlashLeftLow",       "SwipeRightUp",   0 },
    { "NumPadTwo",    VK_NUMPAD2,    "SlashDownLeft",      "SwipeUpRight",   0 },
    { "NumPadThree",  VK_NUMPAD3,    "SlashDown",          "SwipeUp",        0 },
    { "NumPadFour",   VK_NUMPAD4,    "SlashDownRight",     "SwipeUpLeft",    0 },
    { "NumPadFive",   VK_NUMPAD5,    "SlashRightLow",      "SwipeLeftUp",    0 },
    { "NumPadSix",    VK_NUMPAD6,    "SlashRight",         "SwipeLeft",      0 },
    { "NumPadSeven",  VK_NUMPAD7,    "SlashRightHigh",     "SwipeLeftDown",  0 },
    { "NumPadEight",  VK_NUMPAD8,    "SlashCenterRight",   "SwipeDownLeft",  0 },
    { "NumPadNine",   VK_NUMPAD9,    "SlashCenter",        "SwipeDown",      0 },
    { "Multiply",     VK_MULTIPLY,   "SlashCenterLeft",    "SwipeDownRight", 0 },
    { "Divide",       VK_DIVIDE,     "SlashLeftHigh",      "SwipeRightDown", 0 },
    { "Subtract",     VK_SUBTRACT,   "AcceptPopupKeybind", "PopupOK",        VK_RETURN },
    /* combat: F-keys and numpad keys the game leaves free */
    { "F5",           VK_F5,         "ButtonDodgeLeft | OnRelease ReleaseButtonDodgeLeft",   "DodgeLeft",  'A' },
    { "F6",           VK_F6,         "ButtonBlockCenter | OnRelease ReleaseButtonBlockCenter", "Block",    'S' },
    { "F7",           VK_F7,         "ButtonDodgeRight | OnRelease ReleaseButtonDodgeRight", "DodgeRight", 'D' },
    { "F8",           VK_F8,         "ActivateFinalStrikeMode", "Super",       'Q' },
    { "ScrollLock",   VK_SCROLL,     "ActivateMagicMode",       "Magic",       'E' }, /* not F11: the game toggles fullscreen on F11 */
    { "F1",           VK_F1,         "DoAutoStab",              "AutoStab",    'F' },
    { "F4",           VK_F4,         "SwordClashMash",          "ClashMash",   VK_LMENU },
    { "Add",          VK_ADD,        "DoMagic0",                "Magic1",      '1' },
    { "Decimal",      VK_DECIMAL,    "DoMagic1",                "Magic2",      '2' },
    { "Pause",        VK_PAUSE,      "DoMagic2",                "Magic3",      '3' },
    /* menus and the rest: punctuation keys the game leaves free */
    { "LeftBracket",  VK_OEM_4,      "ScrollUp",                "ScrollUp",    0 },
    { "RightBracket", VK_OEM_6,      "ScrollDown",              "ScrollDown",  0 },
    { "Semicolon",    VK_OEM_1,      "ScrollLeft",              "ScrollLeft",  0 },
    { "Quote",        VK_OEM_7,      "ScrollRight",             "ScrollRight", 0 },
    { "Comma",        VK_OEM_COMMA,  "PauseGameKeybind",        "Pause",       VK_SPACE },
    { "Period",       VK_OEM_PERIOD, "ExitKeybind",             "Exit",        VK_ESCAPE },
    { "Slash",        VK_OEM_2,      "MatineeFastForward | OnRelease MatineeResume", "FastForward", VK_LSHIFT },
    { "Equals",       VK_OEM_PLUS,   "ToggleBossPopup",         "BossInfo",    VK_TAB },
};
#define NBIND ((int)(sizeof SLASH12 / sizeof SLASH12[0]))
enum { BK_POPUPOK = 12, BK_SCROLLUP = 23, BK_SCROLLDOWN, BK_SCROLLLEFT, BK_SCROLLRIGHT, BK_PAUSE, BK_EXIT };

static struct {
    int pad_index, start_fight, auto_mode, mode_btn, mode_rumble, log, trig_thresh, poll_ms, debug_input, confine;
    Act fight[NBTN], layer[NBTN], menu[NBTN];
    int layer_btn, layer_tap;
    int slash_keys, snap, dpad_slash, steps, step_ms, restore_cursor, temp_binds, roll_deg;
    float flick_on, flick_off, len_pct, ox_pct, oy_pct;
    Act slash_key[8];
    Act chord_l3r3;
    float cur_speed, cur_dead, cur_curve;
    int cursor_in_fight, lstick_look, nav_swipe, nav_steps, nav_stepms, read_state, back_btn, recenter_btn;
    int focus_on, focus_outline, focus_auto, focus_stick, focus_delay, focus_rep, dump_screens; COLORREF focus_color;
    float lstick_speed;
    int scroll_lstick, scroll_dpad, scroll_ms;
} C;

static const struct { const char *n; WORD vk; } KEYS[] = {
    { "Space", VK_SPACE }, { "SpaceBar", VK_SPACE }, { "Enter", VK_RETURN }, { "Escape", VK_ESCAPE },
    { "Esc", VK_ESCAPE }, { "Tab", VK_TAB }, { "Backspace", VK_BACK }, { "LeftShift", VK_LSHIFT },
    { "RightShift", VK_RSHIFT }, { "LeftAlt", VK_LMENU }, { "RightAlt", VK_RMENU },
    { "LeftControl", VK_LCONTROL }, { "LeftCtrl", VK_LCONTROL }, { "RightControl", VK_RCONTROL },
    { "RightCtrl", VK_RCONTROL }, { "Up", VK_UP }, { "Down", VK_DOWN }, { "Left", VK_LEFT },
    { "Right", VK_RIGHT }, { "Insert", VK_INSERT }, { "Delete", VK_DELETE }, { "Home", VK_HOME },
    { "End", VK_END }, { "PageUp", VK_PRIOR }, { "PageDown", VK_NEXT }, { "Tilde", VK_OEM_3 },
    { "Zero", '0' }, { "One", '1' }, { "Two", '2' }, { "Three", '3' }, { "Four", '4' },
    { "Five", '5' }, { "Six", '6' }, { "Seven", '7' }, { "Eight", '8' }, { "Nine", '9' },
    { "NumPadZero", VK_NUMPAD0 }, { "NumPadOne", VK_NUMPAD1 }, { "NumPadTwo", VK_NUMPAD2 },
    { "NumPadThree", VK_NUMPAD3 }, { "NumPadFour", VK_NUMPAD4 }, { "NumPadFive", VK_NUMPAD5 },
    { "NumPadSix", VK_NUMPAD6 }, { "NumPadSeven", VK_NUMPAD7 }, { "NumPadEight", VK_NUMPAD8 },
    { "NumPadNine", VK_NUMPAD9 }, { "Multiply", VK_MULTIPLY }, { "Divide", VK_DIVIDE },
    { "Add", VK_ADD }, { "Subtract", VK_SUBTRACT }, { "Decimal", VK_DECIMAL },
};

static WORD key_vk(const char *s)
{
    size_t n = strlen(s);
    if (n == 1) {
        char c = s[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (WORD)c;
    }
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
        if (lstrcmpiA(s, KEYS[i].n) == 0) return KEYS[i].vk;
    if ((s[0] == 'F' || s[0] == 'f') && n >= 2 && n <= 3) {
        int f = atoi(s + 1);
        if (f >= 1 && f <= 24) return (WORD)(VK_F1 + f - 1);
    }
    if (n == 7 && _strnicmp(s, "NumPad", 6) == 0 && s[6] >= '0' && s[6] <= '9')
        return (WORD)(VK_NUMPAD0 + (s[6] - '0'));
    return 0;
}

static void trim(char *s)
{
    char *p = s, *e;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
}

static void ini_str(const char *sec, const char *key, const char *def, char *out, DWORD n)
{
    GetPrivateProfileStringA(sec, key, def, out, n, g_ini);
    trim(out);
}
static int ini_int(const char *sec, const char *key, int def)
{
    char b[64], d[32];
    snprintf(d, sizeof d, "%d", def);
    ini_str(sec, key, d, b, sizeof b);
    return atoi(b);
}
static float ini_float(const char *sec, const char *key, float def)
{
    char b[64], d[32];
    snprintf(d, sizeof d, "%g", def);
    ini_str(sec, key, d, b, sizeof b);
    return (float)atof(b);
}
static Act1 parse_one(const char *s, const char *where)
{
    Act1 a = { A_NONE, 0 };
    if (!*s || lstrcmpiA(s, "None") == 0) return a;
    if (lstrcmpiA(s, "LeftClick") == 0) { a.type = A_LCLICK; return a; }
    if (lstrcmpiA(s, "RightClick") == 0) { a.type = A_RCLICK; return a; }
    if (lstrcmpiA(s, "MiddleClick") == 0) { a.type = A_MCLICK; return a; }
    if (lstrcmpiA(s, "WheelUp") == 0) { a.type = A_WUP; return a; }
    if (lstrcmpiA(s, "WheelDown") == 0) { a.type = A_WDOWN; return a; }
    if (lstrcmpiA(s, "WheelLeft") == 0) { a.type = A_WLEFT; return a; }
    if (lstrcmpiA(s, "WheelRight") == 0) { a.type = A_WRIGHT; return a; }
    for (int i = 0; i < NBIND; i++)
        if (lstrcmpiA(s, SLASH12[i].name) == 0 || lstrcmpiA(s, SLASH12[i].cmd) == 0) { a.type = A_KEY; a.vk = SLASH12[i].vk; return a; }
    a.vk = key_vk(s);
    if (a.vk) a.type = A_KEY;
    else logf_("WARNING: unknown key name '%s' for %s, ignored", s, where);
    return a;
}
static Act parse_act(const char *s, const char *where)
{
    Act a;
    char buf[64], *tok, *ctx = NULL;
    ZeroMemory(&a, sizeof a);
    lstrcpynA(a.name, s, sizeof a.name);
    lstrcpynA(buf, s, sizeof buf);
    for (tok = strtok_s(buf, "+", &ctx); tok && a.n < 3; tok = strtok_s(NULL, "+", &ctx)) {
        Act1 one;
        trim(tok);
        one = parse_one(tok, where);
        if (one.type != A_NONE) a.p[a.n++] = one;
    }
    a.type = a.n ? a.p[0].type : A_NONE;
    if (!a.n) lstrcpynA(a.name, "None", sizeof a.name);
    return a;
}
static Act ini_act(const char *sec, const char *key, const char *def)
{
    char b[64], where[96];
    ini_str(sec, key, def, b, sizeof b);
    snprintf(where, sizeof where, "[%s] %s", sec, key);
    return parse_act(b, where);
}
static int btn_index(const char *s)
{
    for (int i = 0; i < NBTN; i++)
        if (lstrcmpiA(s, BTN_NAMES[i]) == 0) return i;
    return -1;
}

static void load_config(void)
{
    /* A B X Y LB RB LT RT Start Back L3 R3 DPad(U D L R) */
    static const char *fight_def[NBTN] = { "Block+LeftClick", "DodgeRight", "DodgeLeft", "AutoStab+PopupOK", "LeftClick",
                                           "ClashMash", "Super", "Magic", "Pause", "FastForward", "BossInfo", "LeftClick",
                                           "ScrollUp+SwipeUp", "ScrollDown+SwipeDown", "ScrollLeft+SwipeLeft",
                                           "ScrollRight+SwipeRight" };
    static const char *menu_def[NBTN]  = { "LeftClick", "Pause", "None", "PopupOK", "None", "None", "None",
                                           "None", "Pause", "None", "None", "None", "None", "None",
                                           "None", "None" };
    char b[64];

    C.pad_index   = ini_int("General", "PadIndex", -1);
    pass_enabled();
    ini_str("General", "StartMode", "Fight", b, sizeof b);
    C.start_fight = lstrcmpiA(b, "Fight") == 0;
    ini_str("General", "ModeToggleButton", "None", b, sizeof b);
    C.mode_btn    = btn_index(b);
    C.auto_mode   = ini_int("General", "AutoMode", 0);
    C.mode_rumble = ini_int("General", "ModeRumble", 1);
    C.trig_thresh = ini_int("General", "TriggerThreshold", 64);
    C.poll_ms     = ini_int("General", "PollMs", 2);
    C.debug_input = ini_int("General", "DebugInput", 0);
    C.confine     = ini_int("General", "ConfineCursor", 1);
    C.chord_l3r3  = ini_act("General", "L3R3Together", "Exit");
    if (C.poll_ms < 1) C.poll_ms = 1;
    if (C.poll_ms > 16) C.poll_ms = 16;

    for (int i = 0; i < NBTN; i++) {
        C.fight[i] = ini_act("Fight", BTN_NAMES[i], fight_def[i]);
        C.menu[i]  = ini_act("Menu", BTN_NAMES[i], menu_def[i]);
        C.layer[i] = ini_act("FightMagicLayer", BTN_NAMES[i],
                             i == B_X ? "Magic1" : i == B_Y ? "Magic2" : i == B_B ? "Magic3" : "None");
    }
    ini_str("Fight", "MagicLayerButton", "RT", b, sizeof b);
    C.layer_btn = btn_index(b);
    C.layer_tap = ini_int("Fight", "MagicLayerTapOnRelease", 1);

    ini_str("Slash", "Mode", "Keys", b, sizeof b);
    C.slash_keys     = lstrcmpiA(b, "Mouse") != 0;
    C.roll_deg       = ini_int("Slash", "RollAngle", 75);
    C.flick_on       = ini_float("Slash", "FlickThreshold", 0.65f);
    C.flick_off      = ini_float("Slash", "ResetThreshold", 0.35f);
    C.snap           = ini_int("Slash", "Snap", 0);
    C.dpad_slash     = ini_int("Slash", "DPadSlashes", 0);
    C.len_pct        = ini_float("Slash", "SwipeLength", 25.0f);
    C.ox_pct         = ini_float("Slash", "SwipeOriginX", 50.0f);
    C.oy_pct         = ini_float("Slash", "SwipeOriginY", 50.0f);
    C.steps          = ini_int("Slash", "SwipeSteps", 4);
    C.step_ms        = ini_int("Slash", "SwipeStepMs", 8);
    C.restore_cursor = ini_int("Slash", "RestoreCursor", 1);
    if (C.steps < 1) C.steps = 1;
    if (C.steps > 64) C.steps = 64;
    if (C.step_ms < 1) C.step_ms = 1;
    for (int i = 0; i < 8; i++)
        C.slash_key[i] = ini_act("Slash", SLASH_DIRS[i], "None");

    C.cur_speed       = ini_float("Cursor", "Speed", 1600.0f);
    C.cur_dead        = ini_float("Cursor", "Deadzone", 0.20f);
    C.cur_curve       = ini_float("Cursor", "Curve", 2.0f);
    C.cursor_in_fight = ini_int("Cursor", "InFight", 1);
    C.lstick_look     = ini_int("Cursor", "LeftStickLook", 0);
    C.nav_swipe       = ini_int("Navigation", "LeftStickSwipe", 1);
    C.nav_steps       = ini_int("Navigation", "SwipeSteps", 8);
    C.nav_stepms      = ini_int("Navigation", "SwipeStepMs", 10);
    C.read_state      = ini_int("Navigation", "ReadGameState", 1);
    ini_str("Menu", "BackButton", "B", b, sizeof b);
    C.back_btn        = btn_index(b);
    ini_str("Navigation", "RecenterCameraButton", "R3", b, sizeof b);
    C.recenter_btn    = btn_index(b);
    C.focus_on        = ini_int("Focus", "Enabled", 1);
    C.focus_outline   = ini_int("Focus", "Outline", 1);
    C.focus_auto      = ini_int("Focus", "AutoFocus", 1);
    C.focus_stick     = ini_int("Focus", "LeftStick", 1);
    C.focus_delay     = ini_int("Focus", "RepeatDelayMs", 300);
    C.focus_rep       = ini_int("Focus", "RepeatMs", 130);
    C.dump_screens    = ini_int("Focus", "LogScreens", 0);
    {
        char col[16];
        unsigned v;
        ini_str("Focus", "OutlineColor", "E6C35A", col, sizeof col);
        v = (unsigned)strtoul(col, NULL, 16);
        C.focus_color = RGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
    }
    C.lstick_speed    = ini_float("Cursor", "LeftStickSpeed", 1600.0f);
    if (C.cur_dead < 0.0f) C.cur_dead = 0.0f;
    if (C.cur_dead > 0.9f) C.cur_dead = 0.9f;

    C.scroll_lstick = ini_int("Scroll", "LeftStick", 1);
    C.scroll_dpad   = ini_int("Scroll", "DPad", 1);
    C.scroll_ms     = ini_int("Scroll", "RepeatMs", 90);
}

/* ---------- synthetic input ---------- */
static HWND g_hwnd;
static int g_focused, g_clipped;

/* The game's client area in screen coordinates, shrunk by 1px so the cursor stays inside. */
static BOOL game_rect(RECT *r)
{
    POINT tl = { 0, 0 }, br;
    RECT c;
    if (!g_hwnd || !GetClientRect(g_hwnd, &c) || c.right < 8 || c.bottom < 8) return FALSE;
    br.x = c.right; br.y = c.bottom;
    ClientToScreen(g_hwnd, &tl);
    ClientToScreen(g_hwnd, &br);
    r->left = tl.x + 1; r->top = tl.y + 1; r->right = br.x - 1; r->bottom = br.y - 1;
    return TRUE;
}
static int g_binds_applied; /* legacy: 0.6 no longer writes key bindings */
static int cmd_direct(int idx, BOOL down);
static int cmd_ready(void);
static void key_evt(WORD vk, BOOL down)
{
    INPUT in;
    UINT sc;
    /* A game action (the table above) is called straight in the game, never sent as a key. Only if
     * the direct route is not up yet (the first seconds after launch) does it fall back to the
     * port's default key for that action; actions without one (slashes, scrolling) do nothing. */
    for (int i = 0; i < NBIND; i++)
        if (SLASH12[i].vk == vk) {
            if (cmd_direct(i, down)) return;
            if (!SLASH12[i].def) return;
            vk = SLASH12[i].def;
            break;
        }
    sc = MapVirtualKeyA(vk, 4 /* MAPVK_VK_TO_VSC_EX */);
    ZeroMemory(&in, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)(sc & 0xFF);
    /* Sent by virtual key (with its scan code attached) so numpad keys stay numpad keys
     * whatever the NumLock state. */
    in.ki.dwFlags = ((sc & 0xFF00) ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof in);
}
static void mouse_flags(DWORD flags, DWORD data)
{
    INPUT in;
    ZeroMemory(&in, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    if (SendInput(1, &in, sizeof in) != 1 && C.debug_input)
        logf_("SendInput mouse REJECTED by Windows, error %lu", GetLastError());
}
static void mouse_to(LONG x, LONG y)
{
    int vl = GetSystemMetrics(SM_XVIRTUALSCREEN), vt = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT in;
    RECT gr;
    if (vw < 2 || vh < 2) return;
    if (C.confine && g_focused && game_rect(&gr)) {
        if (x < gr.left) x = gr.left;
        if (y < gr.top) y = gr.top;
        if (x > gr.right - 1) x = gr.right - 1;
        if (y > gr.bottom - 1) y = gr.bottom - 1;
    }
    if (x < vl) x = vl;
    if (y < vt) y = vt;
    if (x > vl + vw - 1) x = vl + vw - 1;
    if (y > vt + vh - 1) y = vt + vh - 1;
    ZeroMemory(&in, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)(((double)(x - vl) * 65535.0) / (double)(vw - 1) + 0.5);
    in.mi.dy = (LONG)(((double)(y - vt) * 65535.0) / (double)(vh - 1) + 0.5);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof in);
}
/* Menu scrolling: the game's own ScrollUp / ScrollDown / ScrollLeft / ScrollRight commands (what its
 * mouse wheel is bound to), or the real wheel if the session bindings are not in place. */
static void wheel(int delta, BOOL with_shift)
{
    if (cmd_ready()) {
        cmd_direct(with_shift ? (delta > 0 ? BK_SCROLLLEFT : BK_SCROLLRIGHT) : (delta > 0 ? BK_SCROLLUP : BK_SCROLLDOWN), TRUE);
        return;
    }
    if (with_shift) key_evt(VK_LSHIFT, TRUE);
    mouse_flags(MOUSEEVENTF_WHEEL, (DWORD)delta);
    if (with_shift) key_evt(VK_LSHIFT, FALSE);
}
static void wheel(int delta, BOOL with_shift);
static void debug_click(const char *what, BOOL down)
{
    POINT pt;
    HWND under;
    DWORD pid = 0;
    char cls[64] = "";
    GetCursorPos(&pt);
    under = WindowFromPoint(pt);
    if (under) { GetWindowThreadProcessId(under, &pid); GetClassNameA(under, cls, sizeof cls); }
    logf_("%s %s at %ld,%ld over window '%s' (%s), last SendInput error %lu", what, down ? "down" : "up",
          pt.x, pt.y, cls, pid == GetCurrentProcessId() ? "game" : "NOT the game", GetLastError());
}
static void act1(const Act1 *a, BOOL down)
{
    if (C.debug_input && a->type >= A_LCLICK && a->type <= A_MCLICK) SetLastError(0);
    switch (a->type) {
    case A_KEY:    key_evt(a->vk, down); break;
    case A_LCLICK: mouse_flags(down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP, 0); break;
    case A_RCLICK: mouse_flags(down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP, 0); break;
    case A_MCLICK: mouse_flags(down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP, 0); break;
    case A_WUP:    if (down) wheel(120, FALSE); break;
    case A_WDOWN:  if (down) wheel(-120, FALSE); break;
    case A_WLEFT:  if (down) wheel(120, TRUE); break;
    case A_WRIGHT: if (down) wheel(-120, TRUE); break;
    default: break;
    }
    if (C.debug_input && a->type >= A_LCLICK && a->type <= A_MCLICK) debug_click("click", down);
}
static void act(const Act *a, BOOL down)
{
    if (down) for (int i = 0; i < a->n; i++) act1(&a->p[i], TRUE);
    else      for (int i = a->n - 1; i >= 0; i--) act1(&a->p[i], FALSE);
}

/* ---------- runtime state ---------- */
static int g_menu = 1;
static Act g_held[NBTN];
static DWORD g_held_since[NBTN], g_held_rep[NBTN];
static int g_held_on[NBTN];
static int g_flicked;
static DWORD g_last_scroll;
static float g_acc_x, g_acc_y;

static struct { int active, stage, queued, steps, stepms, qsteps, qstepms; DWORD next; POINT o, restore; float dx, dy, len, qdx, qdy; } SW;
static struct { int on; DWORD until; Act a; } TAP;
static struct { int pulses_left, on; DWORD next; } RB;

static BOOL game_focused(void)
{
    HWND f = GetForegroundWindow();
    DWORD pid = 0;
    if (!f) return FALSE;
    GetWindowThreadProcessId(f, &pid);
    if (pid != GetCurrentProcessId()) return FALSE;
    g_hwnd = f;
    return TRUE;
}

static void swipe_cancel(void)
{
    if (SW.active && SW.stage >= 2 && SW.stage <= 2 + SW.steps)
        mouse_flags(MOUSEEVENTF_LEFTUP, 0);
    SW.active = 0;
    SW.queued = 0;
}

static void release_all(void)
{
    for (int i = 0; i < NBTN; i++)
        if (g_held_on[i]) { act(&g_held[i], FALSE); g_held_on[i] = 0; }
    if (TAP.on) { act(&TAP.a, FALSE); TAP.on = 0; }
    swipe_cancel();
}

static void swipe_start(float dx, float dy, int steps, int stepms)
{
    RECT rc;
    POINT o;
    if (SW.active) { SW.queued = 1; SW.qdx = dx; SW.qdy = dy; SW.qsteps = steps; SW.qstepms = stepms; return; }
    if (!g_hwnd || !GetClientRect(g_hwnd, &rc) || rc.bottom <= 0) return;
    o.x = (LONG)(rc.right * C.ox_pct / 100.0f);
    o.y = (LONG)(rc.bottom * C.oy_pct / 100.0f);
    ClientToScreen(g_hwnd, &o);
    GetCursorPos(&SW.restore);
    SW.o = o;
    SW.dx = dx;
    SW.dy = dy;
    SW.len = rc.bottom * C.len_pct / 100.0f;
    SW.steps = steps < 1 ? 1 : steps > 64 ? 64 : steps;
    SW.stepms = stepms < 1 ? 1 : stepms;
    SW.stage = 0;
    SW.active = 1;
    SW.next = timeGetTime();
}

static void swipe_tick(DWORD now)
{
    int s;
    if (!SW.active || (LONG)(now - SW.next) < 0) return;
    s = SW.stage;
    if (s == 0) {
        mouse_to(SW.o.x, SW.o.y);
    } else if (s == 1) {
        mouse_flags(MOUSEEVENTF_LEFTDOWN, 0);
    } else if (s <= 1 + SW.steps) {
        float f = (float)(s - 1) / (float)SW.steps;
        mouse_to(SW.o.x + (LONG)(SW.dx * SW.len * f), SW.o.y + (LONG)(SW.dy * SW.len * f));
    } else if (s == 2 + SW.steps) {
        mouse_flags(MOUSEEVENTF_LEFTUP, 0);
    } else {
        if (C.restore_cursor) mouse_to(SW.restore.x, SW.restore.y);
        SW.active = 0;
        if (SW.queued) { SW.queued = 0; swipe_start(SW.qdx, SW.qdy, SW.qsteps, SW.qstepms); }
        return;
    }
    SW.stage++;
    SW.next = now + (DWORD)SW.stepms;
}

static void tap(const Act *a)
{
    if (a->type == A_NONE) return;
    if (TAP.on) act(&TAP.a, FALSE);
    TAP.a = *a;
    act(a, TRUE);
    TAP.on = 1;
    TAP.until = timeGetTime() + 35;
}
static void tap_tick(DWORD now)
{
    if (TAP.on && (LONG)(now - TAP.until) >= 0) { act(&TAP.a, FALSE); TAP.on = 0; }
}

static void rumble_start(int n)
{
    if (!C.mode_rumble || !R.SetState) return;
    RB.pulses_left = n;
    RB.on = 0;
    RB.next = timeGetTime();
}
static void rumble_tick(DWORD now, DWORD pad)
{
    XVib v = { 0, 0 };
    if ((!RB.pulses_left && !RB.on) || !R.SetState || (LONG)(now - RB.next) < 0) return;
    if (!RB.on) {
        v.wLeftMotorSpeed = v.wRightMotorSpeed = 30000;
        R.SetState(pad, &v);
        RB.on = 1;
    } else {
        R.SetState(pad, &v);
        RB.on = 0;
        RB.pulses_left--;
    }
    RB.next = now + 90;
}

static void set_mode(int menu, const char *why)
{
    if (menu == g_menu) return;
    release_all();
    g_menu = menu;
    logf_("mode -> %s (%s)", menu ? "MENU" : "FIGHT", why);
    rumble_start(menu ? 1 : 2);
}

/* Slash in the stick's direction. sx/sy are stick axes in -1..1 with up positive. */
/* ---------- reading the game's own state (read-only) ----------
 * The game keeps its current screen in SwordHudBase.HudMode.Mode (EHudMode). We locate the
 * engine's name and object tables in memory, work out where that value lives, and poll it.
 * Only reads, never writes; if anything is not found, the left stick keeps the swipe. */
enum { HM_DISABLED, HM_PAUSED, HM_CINEMATIC, HM_BOSSBATTLE, HM_BOSSRECOVERY, HM_CASTMAGIC, HM_FINALSTRIKE,
       HM_DEATH, HM_COMPLETE, HM_BOSSCHOICE, HM_UBERCHOICE, HM_LOOTCHOICE, HM_PATHCHOICE, HM_CLASHMODE,
       HM_GESTURETEST };
static const char *HM_NAMES[] = { "Disabled", "GamePaused", "Cinematic", "BossBattle", "BossRecovery", "CastMagic",
                                  "FinalStrike", "Death", "Complete", "BossChoice", "UberBossChoice",
                                  "DeathlessLootChoice", "PathChoice", "ClashMode", "GestureTest" };

static struct {
    int ok, tried;
    UINT_PTR names; int names_n, name_off, wide;
    UINT_PTR objs; int objs_n;
    UINT_PTR names_hdr, objs_hdr; /* the engine's own array headers, re-read live (they grow on level loads) */
    int o_index, o_name, o_class, o_outer, o_poff;
    UINT_PTR hud_classes[2];
    int off_hudmode, off_mode;
    volatile UINT_PTR hud;
    UINT_PTR pi_class; int off_stack;
    volatile UINT_PTR pinput;
    /* MobileMenuScene / MobileMenuObject layout, for pressing the on-screen back arrow */
    int mm_ok, sc_objs, sc_left, sc_top, sc_w, sc_h, ob_tag, ob_left, ob_top, ob_w, ob_h, ob_hidden, ob_hidden_mask;
    int ob_hl, ob_hl_mask; /* bIsHighlighted: a SwordTab draws itself selected from it */
    int bool_moff;          /* where a BoolProperty keeps its bit mask */
    int ob_relto, ob_owner; /* MobileMenuObject.RelativeTo / OwnerScene: real position like GetRealPosition */
    int el_vppos, el_vpsize, el_vis, el_vis_mask; /* MobileMenuElement: where the game last drew it */
    int inv_slots, list_items, perk_lvl, perk_cnt, moli_objs, sc_opacity, btn_icon, inv_items, list_vert, list_vert_mask;
    /* exploring camera: SwordLookCamera.DesiredLookRotation / LookRotation, SwordPC.SavedLook */
    UINT_PTR lookcam_class, pc_class; int off_desired, off_look, off_savedlook;
    volatile UINT_PTR lookcam, pc;
} G;

static BOOL rd(UINT_PTR a, void *out, SIZE_T n)
{
    SIZE_T got = 0;
    if (a < 0x10000) return FALSE;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)a, out, n, &got) && got == n;
}
static UINT_PTR rdp(UINT_PTR a) { UINT_PTR v = 0; rd(a, &v, sizeof v); return v; }
static int rdi(UINT_PTR a) { int v = -1; rd(a, &v, sizeof v); return v; }

static BOOL name_at(UINT_PTR entry, int off, int wide, char *out, int n)
{
    char raw[128];
    int i;
    if (!rd(entry + off, raw, sizeof raw)) return FALSE;
    if (wide) {
        for (i = 0; i < n - 1 && i < 63 && raw[i * 2]; i++) out[i] = raw[i * 2];
        out[i] = 0;
    } else {
        raw[sizeof raw - 1] = 0;
        lstrcpynA(out, raw, n);
    }
    return TRUE;
}
static const char *uname(int idx)
{
    static char buf[4][96];
    static int k;
    char *o = buf[k++ & 3];
    UINT_PTR e;
    o[0] = 0;
    if (idx < 0 || idx >= G.names_n) return o;
    e = rdp(G.names + (UINT_PTR)idx * 8);
    if (e) name_at(e, G.name_off, G.wide, o, 96);
    return o;
}
static UINT_PTR uobj(int i) { return (i >= 0 && i < G.objs_n) ? rdp(G.objs + (UINT_PTR)i * 8) : 0; }
static const char *oname(UINT_PTR o) { return o ? uname(rdi(o + G.o_name)) : ""; }
static BOOL is_obj(UINT_PTR o) { int i; if (!o) return FALSE; i = rdi(o + G.o_index); return i >= 0 && i < G.objs_n && uobj(i) == o; }
static const char *oclass(UINT_PTR o) { return oname(rdp(o + G.o_class)); }

/* Walks IB3.exe's writable data for engine array headers {data pointer, count, capacity}. */
typedef BOOL (*cand_fn)(UINT_PTR p, int n);
static UINT_PTR g_scan_hdr;

/* The object and name tables grow (and move) when a level loads. Reading them live is what
 * lets the mod find each new fight's HUD; a stale copy made the left stick fall back to
 * mouse swipes after the first level change. */
static void refresh_tables(void)
{
    if (G.objs_hdr) {
        UINT_PTR p = rdp(G.objs_hdr);
        int n = rdi(G.objs_hdr + 8);
        if (p && !(p & 7) && n > 1000 && n < 32000000) { G.objs = p; G.objs_n = n; }
    }
    if (G.names_hdr) {
        UINT_PTR p = rdp(G.names_hdr);
        int n = rdi(G.names_hdr + 8);
        if (p && !(p & 7) && n > 1000 && n < 16000000) { G.names = p; G.names_n = n; }
    }
}
static BOOL scan_arrays(cand_fn fn, int min_n, int max_n)
{
    UINT_PTR base = (UINT_PTR)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        UINT_PTR a, end;
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE) || sec->Misc.VirtualSize < 32) continue;
        a = base + sec->VirtualAddress;
        end = a + sec->Misc.VirtualSize - 16;
        for (; a < end; a += 8) {
            UINT_PTR p = *(UINT_PTR *)a;
            int n = *(int *)(a + 8), m = *(int *)(a + 12);
            if (n < min_n || n > max_n || m < n || m > max_n * 4 || (p & 7) || p < 0x10000) continue;
            g_scan_hdr = a;
            if (fn(p, n)) return TRUE;
        }
    }
    return FALSE;
}

static BOOL names_cand(UINT_PTR p, int n)
{
    UINT_PTR e0 = rdp(p), e1 = rdp(p + 8);
    if (!e0 || !e1) return FALSE;
    for (int off = 0; off <= 0x20; off += 4)
        for (int w = 0; w < 2; w++) {
            char a[32], b2[32];
            if (!name_at(e0, off, w, a, sizeof a) || lstrcmpA(a, "None") != 0) continue;
            if (!name_at(e1, off, w, b2, sizeof b2) || lstrcmpA(b2, "ByteProperty") != 0) continue;
            G.names = p; G.names_n = n; G.name_off = off; G.wide = w; G.names_hdr = g_scan_hdr;
            return TRUE;
        }
    return FALSE;
}

static BOOL objs_cand(UINT_PTR p, int n)
{
    UINT_PTR sample[64];
    int sidx[64], ns = 0, found;
    G.objs = p; G.objs_n = n; G.objs_hdr = g_scan_hdr;
    for (int i = 0; i < 400 && ns < 64; i++) {
        UINT_PTR o = rdp(p + (UINT_PTR)i * 8);
        if (o) { sample[ns] = o; sidx[ns] = i; ns++; }
    }
    if (ns < 32) return FALSE;
    /* Index: the field equal to the object's slot in the table. */
    found = 0;
    for (int off = 8; off <= 0x80 && !found; off += 4) {
        int good = 1;
        for (int k = 0; k < ns && good; k++) good = rdi(sample[k] + off) == sidx[k];
        if (good) { G.o_index = off; found = 1; }
    }
    if (!found) return FALSE;
    /* Name: the field whose names include the core class names. */
    found = 0;
    for (int off = 8; off <= 0x90 && !found; off += 4) {
        int hits = 0, valid = 1;
        if (off == G.o_index) continue;
        for (int k = 0; k < ns && valid; k++) {
            int v = rdi(sample[k] + off);
            const char *nm2;
            if (v < 0 || v >= G.names_n) { valid = 0; break; }
            nm2 = uname(v);
            if (!nm2[0]) { valid = 0; break; }
            if (!lstrcmpA(nm2, "Core") || !lstrcmpA(nm2, "Object") || !lstrcmpA(nm2, "Class") || !lstrcmpA(nm2, "Package")) hits++;
        }
        if (valid && hits >= 2) { G.o_name = off; found = 1; }
    }
    if (!found) return FALSE;
    /* Class: a pointer that always lands on a table object, sometimes one named "Class". */
    found = 0;
    for (int off = 8; off <= 0x90 && !found; off += 8) {
        int all = 1, cls = 0;
        for (int k = 0; k < ns && all; k++) {
            UINT_PTR c = rdp(sample[k] + off);
            if (!is_obj(c)) { all = 0; break; }
            if (!lstrcmpA(oname(c), "Class")) cls++;
        }
        if (all && cls) { G.o_class = off; found = 1; }
    }
    if (!found) return FALSE;
    /* Outer: for the class Core.Object, the pointer to the package named "Core". */
    found = 0;
    for (int i = 0; i < 3000 && !found; i++) {
        UINT_PTR o = uobj(i);
        if (!o || lstrcmpA(oname(o), "Object") || lstrcmpA(oclass(o), "Class")) continue;
        for (int off = 8; off <= 0x90; off += 8) {
            UINT_PTR q;
            if (off == G.o_class) continue;
            q = rdp(o + off);
            if (is_obj(q) && !lstrcmpA(oname(q), "Core") && !lstrcmpA(oclass(q), "Package")) { G.o_outer = off; found = 1; break; }
        }
    }
    return found;
}

static BOOL is_prop_class(const char *c) { size_t n = strlen(c); return n > 8 && !lstrcmpA(c + n - 8, "Property"); }

/* UProperty::Offset: among a class's properties, the int field whose values are distinct
 * (bools may share one), aligned for their type and in a sane range. */
static int find_prop_offset_field(UINT_PTR cls)
{
    static UINT_PTR props[256];
    static char pc[256][24];
    static int vals[256];
    int np = 0, best = -1, best_score = 0;
    for (int i = 0; i < G.objs_n && np < 256; i++) {
        UINT_PTR o = uobj(i);
        if (!o || rdp(o + G.o_outer) != cls) continue;
        if (!is_prop_class(oclass(o))) continue;
        props[np] = o; lstrcpynA(pc[np], oclass(o), 24); np++;
    }
    if (np < 8) return -1;
    for (int off = 8; off <= 0x100; off += 4) {
        int ok = 1, distinct = 0;
        if (off == G.o_index || off == G.o_name) continue;
        for (int k = 0; k < np && ok; k++) {
            int v = rdi(props[k] + off);
            vals[k] = v;
            if (v < 0x40 || v > 0x20000) { ok = 0; break; }
            if ((!lstrcmpA(pc[k], "IntProperty") || !lstrcmpA(pc[k], "FloatProperty")) && (v & 3)) ok = 0;
            if ((!lstrcmpA(pc[k], "ObjectProperty") || !lstrcmpA(pc[k], "StrProperty") || !lstrcmpA(pc[k], "ArrayProperty")) && (v & 7)) ok = 0;
        }
        if (!ok) continue;
        for (int k = 0; k < np; k++) {
            int dup = 0;
            if (!lstrcmpA(pc[k], "BoolProperty")) continue;
            for (int j = 0; j < np; j++) if (j != k && vals[j] == vals[k] && lstrcmpA(pc[j], "BoolProperty")) dup = 1;
            if (!dup) distinct++;
        }
        if (distinct > best_score) { best_score = distinct; best = off; }
    }
    return best;
}

static UINT_PTR find_obj(const char *name, const char *cls, UINT_PTR outer)
{
    for (int i = 0; i < G.objs_n; i++) {
        UINT_PTR o = uobj(i);
        if (!o || lstrcmpA(oname(o), name)) continue;
        if (cls && lstrcmpA(oclass(o), cls)) continue;
        if (outer && rdp(o + G.o_outer) != outer) continue;
        return o;
    }
    return 0;
}

/* UProperty::Offset found exactly: HudModeStruct is declared as
 *   byte Mode; byte PreviousMode; float BeginTime; string OverrideConfig; float FadeInAlpha;
 * so its members sit at 0, 1, 4, 8 and 24 on x64. The field holding those five values in the
 * five property objects is the Offset field. Falls back to a looser match, logging what it saw. */
static int find_offset_field_exact(UINT_PTR st)
{
    static const char *nm[5] = { "Mode", "PreviousMode", "BeginTime", "OverrideConfig", "FadeInAlpha" };
    static const int want[5] = { 0, 1, 4, 8, 24 };
    UINT_PTR po[5];
    int loose = -1;
    for (int k = 0; k < 5; k++) {
        po[k] = 0;
        for (int i = 0; i < G.objs_n && !po[k]; i++) {
            UINT_PTR o = uobj(i);
            if (o && rdp(o + G.o_outer) == st && !lstrcmpA(oname(o), nm[k])) po[k] = o;
        }
        if (!po[k]) { logf_("state: HudModeStruct member %s not found", nm[k]); return -1; }
    }
    for (int off = 8; off <= 0x180; off += 4) {
        int v[5], exact = 1, ok = 1;
        for (int k = 0; k < 5; k++) { v[k] = rdi(po[k] + off); if (v[k] != want[k]) exact = 0; }
        if (exact) return off;
        /* loose: small, distinct, Mode lowest and next to PreviousMode */
        for (int k = 0; k < 5 && ok; k++) if (v[k] < 0 || v[k] > 64) ok = 0;
        if (ok && v[1] == v[0] + 1 && v[2] > v[1] && (v[2] & 3) == 0 && loose < 0) loose = off;
    }
    if (loose >= 0) logf_("state: using near-match property layout at +%x", loose);
    else {
        char line[512] = "";
        for (int off = 0x40; off <= 0x100; off += 4) {
            char t[48];
            snprintf(t, sizeof t, " %x:%d/%d", off, rdi(po[0] + off), rdi(po[2] + off));
            if (strlen(line) + strlen(t) < sizeof line) strcat(line, t);
        }
        logf_("state: property layout dump (Mode/BeginTime):%s", line);
    }
    return loose;
}

static void probe_init(void)
{
    UINT_PTR base_cls, mode_struct, p_hud, p_mode;
    G.tried++;
    if (!G.names && !scan_arrays(names_cand, 5000, 4000000)) { if (G.tried == 1) logf_("state: name table not found yet"); return; }
    if (!G.objs_n && !scan_arrays(objs_cand, 10000, 8000000)) { G.objs_n = 0; if (G.tried == 1) logf_("state: object table not found yet"); return; }
    base_cls = find_obj("SwordHudBase", "Class", 0);
    if (!base_cls) { if (G.tried <= 2) logf_("state: game classes not loaded yet"); return; }
    p_hud = find_obj("HudMode", "StructProperty", base_cls);
    mode_struct = find_obj("HudModeStruct", "ScriptStruct", 0);
    p_mode = mode_struct ? find_obj("Mode", "ByteProperty", mode_struct) : 0;
    G.o_poff = mode_struct ? find_offset_field_exact(mode_struct) : -1;
    if (G.o_poff < 0) G.o_poff = find_prop_offset_field(base_cls);
    if (G.o_poff < 0 || !p_hud || !p_mode) {
        logf_("state: layout not found (poff %d, HudMode %d, Mode %d)", G.o_poff, p_hud != 0, p_mode != 0);
        G.tried = 1000; /* give up; the left stick keeps the swipe */
        return;
    }
    G.off_hudmode = rdi(p_hud + G.o_poff);
    G.off_mode = rdi(p_mode + G.o_poff);
    if (G.off_mode < 0 || G.off_mode > 64) G.off_mode = 0;
    if (G.off_hudmode < 0x40 || G.off_hudmode > 0x20000) {
        logf_("state: HudMode offset %x looks wrong; not reading", G.off_hudmode);
        G.tried = 1000;
        return;
    }
    {
        UINT_PTR mpi = find_obj("MobilePlayerInput", "Class", 0);
        UINT_PTR ps = mpi ? find_obj("MobileMenuStack", "ArrayProperty", mpi) : 0;
        G.off_stack = ps ? rdi(ps + G.o_poff) : -1;
        G.pi_class = find_obj("SwordPlayerInput", "Class", 0);
        if (G.off_stack < 0x40 || G.off_stack > 0x20000 || !G.pi_class) {
            logf_("state: menu stack not found (B stays dodge right)");
            G.off_stack = -1;
        }
    }
    {
        UINT_PTR msc = find_obj("MobileMenuScene", "Class", 0), mob = find_obj("MobileMenuObject", "Class", 0);
        UINT_PTR pr[11];
        static const char *pn[11] = { "MenuObjects", "Left", "Top", "Width", "Height", "Tag", "Left", "Top", "Width", "Height", "bIsHidden" };
        static const char *pc[11] = { "ArrayProperty", "FloatProperty", "FloatProperty", "FloatProperty", "FloatProperty", "StrProperty",
                                      "FloatProperty", "FloatProperty", "FloatProperty", "FloatProperty", "BoolProperty" };
        int all = msc && mob;
        for (int k = 0; k < 11 && all; k++) {
            pr[k] = find_obj(pn[k], pc[k], k < 5 ? msc : mob);
            if (!pr[k]) { logf_("state: menu object field %s not found", pn[k]); all = 0; }
        }
        if (all) {
            int *dst[11] = { &G.sc_objs, &G.sc_left, &G.sc_top, &G.sc_w, &G.sc_h, &G.ob_tag, &G.ob_left, &G.ob_top, &G.ob_w, &G.ob_h, &G.ob_hidden };
            for (int k = 0; k < 11; k++) *dst[k] = rdi(pr[k] + G.o_poff);
            /* BoolProperty bit mask: the field where MobileMenuObject's bools hold distinct powers of two */
            {
                UINT_PTR bools[16]; int nb = 0;
                for (int i = 0; i < G.objs_n && nb < 16; i++) {
                    UINT_PTR o = uobj(i);
                    if (o && rdp(o + G.o_outer) == mob && !lstrcmpA(oclass(o), "BoolProperty")) bools[nb++] = o;
                }
                for (int off = G.o_poff + 4; off <= G.o_poff + 0x60 && nb >= 3 && !G.ob_hidden_mask; off += 4) {
                    int ok = 1;
                    for (int k = 0; k < nb && ok; k++) {
                        unsigned v = (unsigned)rdi(bools[k] + off);
                        if (!v || (v & (v - 1))) ok = 0;
                        for (int j = 0; j < k && ok; j++) if ((unsigned)rdi(bools[j] + off) == v && rdi(bools[j] + G.o_poff) == rdi(bools[k] + G.o_poff)) ok = 0;
                    }
                    if (ok) {
                        UINT_PTR hl = find_obj("bIsHighlighted", "BoolProperty", mob);
                        G.ob_hidden_mask = rdi(pr[10] + off);
                        if (hl) { G.ob_hl = rdi(hl + G.o_poff); G.ob_hl_mask = rdi(hl + off); }
                        G.bool_moff = off;
                    }
                }
            }
            G.mm_ok = G.sc_objs > 0 && G.ob_tag > 0 && G.ob_hidden_mask;
            {
                UINT_PTR el = find_obj("MobileMenuElement", "Class", 0), inv = find_obj("MobileMenuInventory", "Class", 0);
                UINT_PTR ml = find_obj("MobileMenuList", "Class", 0), pb = find_obj("SwordPerkBar", "Class", 0);
                UINT_PTR mo = find_obj("SwordMenuObjectsListItem", "Class", 0), q;
#define POFF(name, cls, outer) ((outer) && (q = find_obj(name, cls, outer)) ? rdi(q + G.o_poff) : -1)
                G.ob_relto = POFF("RelativeTo", "ObjectProperty", mob);
                G.ob_owner = POFF("OwnerScene", "ObjectProperty", mob);
                G.el_vppos = POFF("VpPos", "StructProperty", el);
                G.el_vpsize = POFF("VpSize", "StructProperty", el);
                G.el_vis = POFF("bIsVisible", "BoolProperty", el);
                G.el_vis_mask = (el && G.bool_moff && (q = find_obj("bIsVisible", "BoolProperty", el))) ? rdi(q + G.bool_moff) : 0;
                G.inv_slots = POFF("Slots", "ArrayProperty", inv);
                G.list_items = POFF("Items", "ArrayProperty", ml);
                G.perk_lvl = POFF("LevelBttn", "ObjectProperty", pb);
                G.perk_cnt = POFF("PerkItemCount", "IntProperty", pb);
                G.moli_objs = POFF("MenuObjects", "ArrayProperty", mo);
                G.inv_items = POFF("Items", "ArrayProperty", inv);
                G.list_vert = POFF("bIsVerticalList", "BoolProperty", ml);
                G.list_vert_mask = (ml && G.bool_moff && (q = find_obj("bIsVerticalList", "BoolProperty", ml))) ? rdi(q + G.bool_moff) : 0;
                G.sc_opacity = POFF("Opacity", "FloatProperty", msc);
                {
                    UINT_PTR sb = find_obj("SwordButton", "Class", 0);
                    G.btn_icon = POFF("CurrentIcon", "IntProperty", sb);
                }
#undef POFF
                logf_("state: screen layout relto+%x owner+%x vp+%x/%x vis+%x/%x slots+%x items+%x perk+%x/%x listobjs+%x",
                      G.ob_relto, G.ob_owner, G.el_vppos, G.el_vpsize, G.el_vis, G.el_vis_mask, G.inv_slots, G.list_items,
                      G.perk_lvl, G.perk_cnt, G.moli_objs);
            }
            logf_("state: menu objects %s (objs+%x tag+%x rect+%x/%x/%x/%x hidden+%x mask %x)", G.mm_ok ? "found" : "incomplete",
                  G.sc_objs, G.ob_tag, G.ob_left, G.ob_top, G.ob_w, G.ob_h, G.ob_hidden, G.ob_hidden_mask);
        }
    }
    G.hud_classes[0] = find_obj("SwordHudMain", "Class", 0);
    G.hud_classes[1] = find_obj("SwordHudBoss", "Class", 0);
    {
        UINT_PTR pd, pl, ps;
        G.lookcam_class = find_obj("SwordLookCamera", "Class", 0);
        G.pc_class = find_obj("SwordPC", "Class", 0);
        pd = G.lookcam_class ? find_obj("DesiredLookRotation", "StructProperty", G.lookcam_class) : 0;
        pl = G.lookcam_class ? find_obj("LookRotation", "StructProperty", G.lookcam_class) : 0;
        ps = G.pc_class ? find_obj("SavedLook", "StructProperty", G.pc_class) : 0;
        G.off_desired = pd ? rdi(pd + G.o_poff) : -1;
        G.off_look = pl ? rdi(pl + G.o_poff) : -1;
        G.off_savedlook = ps ? rdi(ps + G.o_poff) : -1;
        if (G.off_desired < 0x30 || G.off_desired > 0x20000 || G.off_look < 0x30 || G.off_look > 0x20000) G.off_desired = -1;
        if (G.off_savedlook < 0x30 || G.off_savedlook > 0x20000) G.off_savedlook = -1;
        logf_("state: camera recentre %s (desired+%x look+%x savedlook+%x)", G.off_desired > 0 ? "ready" : "not found",
              G.off_desired, G.off_look, G.off_savedlook);
    }
    G.ok = 1;
    logf_("state: reading the game screen (names %d, objects %d, layout %x/%x/%x/%x/%x, HudMode+%x Mode+%x)",
          G.names_n, G.objs_n, G.o_index, G.o_name, G.o_class, G.o_outer, G.o_poff, G.off_hudmode, G.off_mode);
}

static int track_ok(UINT_PTR cur, UINT_PTR cls)
{
    return cls && cur && is_obj(cur) && rdp(cur + G.o_class) == cls;
}
/* The live (non-default) instance of a class, keeping the current one while it is valid. */
static UINT_PTR track_instance(UINT_PTR cur, UINT_PTR cls)
{
    if (!cls) return 0;
    if (track_ok(cur, cls)) return cur;
    for (int i = G.objs_n - 1; i >= 0; i--) {
        UINT_PTR o = uobj(i);
        if (o && rdp(o + G.o_class) == cls && strncmp(oname(o), "Default__", 9) != 0) return o;
    }
    return 0;
}

/* ---------- direct game commands ----------
 * Each controller action calls the game's own command function (SwordPC.ButtonDodgeLeft,
 * SwordHudMain.ActivateMagicMode, SwordPC.ScrollUp...) through the engine's ProcessEvent, the same
 * call the console makes for a key binding, but with no key and no binding in between. The call has
 * to run on the game's thread, so the worker posts a message to the game window and a small window
 * procedure, which runs on that thread, makes the call. ProcessEvent is virtual slot 0x210/8 of the
 * engine's object vtable (found from the 569 call sites in IB3.exe that call it right after looking
 * a function up). */
#define PE_SLOT (0x210 / 8)
typedef void (*PFN_ProcessEvent)(UINT_PTR self, UINT_PTR fn, void *parms, void *result);
static struct { UINT_PTR press, release, need_cls; int hud; } CMD[64];
static volatile int g_cmd_ok;
static UINT g_cmd_msg;
static UINT_PTR g_img_lo, g_img_hi;
static volatile HWND g_cmd_hwnd;
static int g_act_del = -1, g_act_del_mask, g_pc_myhud = -1;
static volatile DWORD g_obj_change; /* when the HUD or player object last changed (level loads) */

static UINT_PTR cmd_find(const char *name, UINT_PTR const *cls, int ncls, int *which)
{
    for (int k = 0; k < ncls; k++) {
        UINT_PTR f = cls[k] ? find_obj(name, "Function", cls[k]) : 0;
        if (f) { *which = k; return f; }
    }
    return 0;
}
static void cmd_resolve(void)
{
    UINT_PTR cls[3];
    int ok = 0;
    cls[0] = find_obj("SwordPC", "Class", 0);
    cls[1] = find_obj("SwordHudMain", "Class", 0);
    cls[2] = find_obj("SwordHudBase", "Class", 0);
    {
        UINT_PTR base = (UINT_PTR)GetModuleHandleA(NULL);
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        g_img_lo = base; g_img_hi = base + nt->OptionalHeader.SizeOfImage;
    }
    for (int i = 0; i < NBIND && i < 64; i++) {
        char press[64], rel[64] = "";
        const char *c = SLASH12[i].cmd, *bar = strchr(c, '|'), *on;
        int w = 0;
        size_t n = bar ? (size_t)(bar - c) : strlen(c);
        if (n >= sizeof press) n = sizeof press - 1;
        memcpy(press, c, n); press[n] = 0; trim(press);
        if ((on = strstr(c, "OnRelease ")) != NULL) {
            const char *e = strchr(on + 10, '|');
            n = e ? (size_t)(e - (on + 10)) : strlen(on + 10);
            if (n >= sizeof rel) n = sizeof rel - 1;
            memcpy(rel, on + 10, n); rel[n] = 0; trim(rel);
        }
        CMD[i].press = cmd_find(press, cls, 3, &w);
        CMD[i].hud = w > 0;
        CMD[i].need_cls = w == 1 ? cls[1] : 0; /* SwordHudMain commands only on that HUD */
        CMD[i].release = rel[0] ? cmd_find(rel, cls, 3, &w) : 0;
        if (CMD[i].press && (!rel[0] || CMD[i].release)) ok++;
        else logf_("commands: %s not found in the game", press);
    }
    {
        UINT_PTR act = find_obj("Actor", "Class", 0), plc = find_obj("PlayerController", "Class", 0), q;
        if (act && (q = find_obj("bDeleteMe", "BoolProperty", act)) && G.bool_moff) {
            g_act_del = rdi(q + G.o_poff);
            g_act_del_mask = rdi(q + G.bool_moff);
        }
        if (plc && (q = find_obj("myHUD", "ObjectProperty", plc))) g_pc_myhud = rdi(q + G.o_poff);
        logf_("commands: safety checks %s", g_act_del > 0 && g_act_del_mask && g_pc_myhud > 0 ? "ready" : "partly available");
    }
    g_cmd_msg = RegisterWindowMessageA("IB3PadDirectGameCommand");
    g_cmd_ok = ok == NBIND && g_cmd_msg;
    logf_("commands: %d of %d game commands found; direct calls %s", ok, NBIND, g_cmd_ok ? "ready" : "NOT available");
}
/* Runs on the game's thread (inside its window procedure). */
/* The game thread can pump messages in the middle of a level load, while the old player and HUD
 * are being torn down. A command only runs when its object is alive in the live object table,
 * not destroyed, still the player's HUD, and the scene has been stable for a moment. Presses that
 * waited in the queue (the game was busy loading) are dropped. */
static BOOL live_obj(UINT_PTR o)
{
    UINT_PTR tab;
    int i, n;
    if (!o || !G.objs_hdr) return o && is_obj(o);
    tab = rdp(G.objs_hdr); n = rdi(G.objs_hdr + 8);
    i = rdi(o + G.o_index);
    return tab && i >= 0 && i < n && rdp(tab + (UINT_PTR)i * 8) == o;
}
static BOOL actor_alive(UINT_PTR a)
{
    if (!live_obj(a)) return FALSE;
    if (g_act_del > 0 && g_act_del_mask && (rdi(a + g_act_del) & g_act_del_mask)) return FALSE;
    return TRUE;
}
static BOOL cmd_safe(UINT_PTR obj, int down, DWORD posted)
{
    UINT_PTR pc = G.pc, hud;
    DWORD now = timeGetTime();
    if (!obj || !pc || !actor_alive(obj) || !actor_alive(pc)) return FALSE;
    if (g_pc_myhud > 0) {
        hud = rdp(pc + g_pc_myhud);
        if (!hud || !actor_alive(hud)) return FALSE;
        if (obj != pc && obj != hud) return FALSE;
    }
    if (down && (LONG)(now - g_obj_change) < 600) return FALSE;
    if (down && (LONG)((now & 0x7fffffff) - posted) > 300) return FALSE;
    return TRUE;
}
static void cmd_run(int idx, int down, DWORD posted)
{
    UINT_PTR fn, obj, vt, pe;
    unsigned char parms[512];
    static int warned;
    if (idx < 0 || idx >= NBIND) return;
    fn = down ? CMD[idx].press : CMD[idx].release;
    if (!fn) return;
    obj = CMD[idx].hud ? G.hud : G.pc;
    if (!cmd_safe(obj, down, posted)) {
        if (C.debug_input || !warned++) logf_("commands: %s%s skipped, game is changing scene", SLASH12[idx].name, down ? "" : " (release)");
        return;
    }
    if (CMD[idx].need_cls && rdp(obj + G.o_class) != CMD[idx].need_cls) return; /* e.g. magic on a non-main HUD */
    vt = rdp(obj);
    pe = vt ? rdp(vt + PE_SLOT * 8) : 0;
    if (pe < g_img_lo || pe >= g_img_hi) { if (!warned++) logf_("commands: engine call not where expected; skipped"); return; }
    ZeroMemory(parms, sizeof parms);
    ((PFN_ProcessEvent)pe)(obj, fn, parms, NULL);
    if (C.debug_input) logf_("command: %s%s", SLASH12[idx].name, down ? "" : " (release)");
}
/* Commands run on the game thread from a message hook on that thread's queue. The game window
 * itself is left untouched (no subclassing), exactly as in 0.6.0-beta6 and earlier. */
static LRESULT CALLBACK cmd_msghook(int code, WPARAM w, LPARAM l)
{
    if (code == HC_ACTION && w == PM_REMOVE && g_cmd_msg) {
        MSG *m = (MSG *)l;
        if (m && m->message == g_cmd_msg) {
            int idx = (int)m->wParam, down = (int)(m->lParam & 1);
            DWORD posted = (DWORD)((UINT_PTR)m->lParam >> 1);
            m->message = WM_NULL;
            cmd_run(idx, down, posted);
        }
    }
    return CallNextHookEx(NULL, code, w, l);
}
static void cmd_attach(HWND h)
{
    static DWORD tid;
    static HHOOK hook;
    char cls[64];
    DWORD t;
    if (!g_cmd_ok || !h || h == g_cmd_hwnd) return;
    if (!GetClassNameA(h, cls, sizeof cls) || strcmp(cls, "LaunchUnrealUWindowsClient")) return;
    t = GetWindowThreadProcessId(h, NULL);
    if (!t) return;
    if (t != tid || !hook) {
        HHOOK nh = SetWindowsHookExW(WH_GETMESSAGE, cmd_msghook, g_self, t);
        if (!nh) { logf_("commands: could not reach the game thread (%lu)", GetLastError()); return; }
        if (hook) UnhookWindowsHookEx(hook);
        hook = nh;
        tid = t;
        logf_("commands: hooked to the game thread; controller actions call the game directly");
    }
    g_cmd_hwnd = h;
}
static int cmd_ready(void)
{
    return g_cmd_ok && g_cmd_hwnd && G.pc;
}
static int cmd_direct(int idx, BOOL down)
{
    if (!cmd_ready() || idx < 0 || idx >= NBIND) return 0;
    if (!down && !CMD[idx].release) return 1; /* nothing to call on release */
    return PostMessageW(g_cmd_hwnd, g_cmd_msg, (WPARAM)idx,
                        (LPARAM)((((UINT_PTR)(timeGetTime() & 0x7fffffff)) << 1) | (down ? 1u : 0u))) ? 1 : 0;
}

/* Background thread: finds the layout once, then keeps track of the live HUD object
 * (it is recreated on level loads). The controller loop only reads one byte. */
static DWORD WINAPI probe_thread(LPVOID arg)
{
    (void)arg;
    Sleep(3000);
    while (!G.ok && G.tried < 90) {
        probe_init();
        if (!G.ok) Sleep(2000);
    }
    if (!G.ok) { logf_("state: game screen not readable; left stick keeps the swipe everywhere"); return 0; }
    cmd_resolve();
    for (;;) {
        UINT_PTR cur = G.hud, found = 0, c;
        refresh_tables();
        if (cur && is_obj(cur)) {
            c = rdp(cur + G.o_class);
            if (c && (c == G.hud_classes[0] || c == G.hud_classes[1])) found = cur;
        }
        if (!found)
            for (int i = G.objs_n - 1; i >= 0; i--) {
                UINT_PTR o = uobj(i);
                if (!o) continue;
                c = rdp(o + G.o_class);
                if (c && (c == G.hud_classes[0] || c == G.hud_classes[1]) && strncmp(oname(o), "Default__", 9) != 0) {
                    found = o;
                    break;
                }
            }
        if (found != cur) { logf_("state: HUD %s", found ? oname(found) : "gone"); g_obj_change = timeGetTime(); }
        G.hud = found;
        if (G.off_stack > 0) {
            UINT_PTR pcur = G.pinput, pf = 0;
            if (pcur && is_obj(pcur) && rdp(pcur + G.o_class) == G.pi_class) pf = pcur;
            if (!pf)
                for (int i = G.objs_n - 1; i >= 0; i--) {
                    UINT_PTR o = uobj(i);
                    if (o && rdp(o + G.o_class) == G.pi_class && strncmp(oname(o), "Default__", 9) != 0) { pf = o; break; }
                }
            G.pinput = pf;
        }
        {
            static DWORD next_cam;
            DWORD t = GetTickCount();
            int need = (G.off_desired > 0 && !track_ok(G.lookcam, G.lookcam_class)) || (G.pc_class && !track_ok(G.pc, G.pc_class));
            if (!need || (LONG)(t - next_cam) >= 0) {
                UINT_PTR lc = track_instance(G.lookcam, G.lookcam_class), pc = track_instance(G.pc, G.pc_class);
                if (lc != G.lookcam) logf_("state: exploring camera %s", lc ? "found" : "gone");
                if (pc != G.pc) g_obj_change = timeGetTime();
                G.lookcam = lc; G.pc = pc;
                next_cam = t + ((G.pc && G.lookcam) ? 2000 : 150); /* re-find quickly after a level load */
            }
        }
        {
            /* the safety checks must never block a live game: if what they read disagrees with
             * a player and HUD that have been steady for seconds, that check is switched off */
            static int bad_del, bad_hud;
            UINT_PTR pc = G.pc, hud = G.hud;
            if (pc && hud && live_obj(pc) && live_obj(hud) && (LONG)(timeGetTime() - g_obj_change) > 3000) {
                if (g_act_del > 0 && ((rdi(pc + g_act_del) & g_act_del_mask) || (rdi(hud + g_act_del) & g_act_del_mask))) {
                    if (++bad_del >= 10) { g_act_del = -1; logf_("commands: destroyed-object check disagrees with the game; off"); }
                } else bad_del = 0;
                if (g_pc_myhud > 0 && rdp(pc + g_pc_myhud) != hud) {
                    if (++bad_hud >= 10) { g_pc_myhud = -1; logf_("commands: player HUD check disagrees with the game; off"); }
                } else bad_hud = 0;
            }
        }
        Sleep(found ? 400 : 150); /* re-find a new HUD quickly after a level load */
    }
    return 0;
}

/* Current EHudMode of the active HUD, or -1 when unknown. */
static int game_hud_mode(void)
{
    unsigned char m;
    UINT_PTR h = G.hud;
    if (!G.ok || !h || !rd(h + G.off_hudmode + G.off_mode, &m, 1)) return -1;
    return m;
}

static const char *POPUPS[] = {
    "SwordBossInfoPopupScene", "SwordClashMobPopup", "SwordCookerBasePopup", "SwordCreditsScene", "SwordCsEmailPopup",
    "SwordGemCookerPopup", "SwordGenericPopup", "SwordGrabBagOkPopup", "SwordIapDiscountPopup", "SwordIapPopup",
    "SwordItemForgePopup", "SwordMcpAuthenticatePopup", "SwordOkPopup", "SwordPlayerInfoPopupScene", "SwordPopupScene",
    "SwordPopupWindow", "SwordPotionCauldronPopup", "SwordRateGamePopup", "SwordResolveCloudSaveConflictPopup",
    "SwordRewardsScene", "SwordTechnicalDataPopup", "SwordTutorialPopup", "SwordWaitPopup", "SwordYesNoPopup" };

/* Number of open menus (the game's MobileMenuStack; element 0 is the top one) and the top
 * screen's class name. Returns -1 when unknown. */
static int game_menu(char *top, int n)
{
    UINT_PTR pi = G.pinput, data, sc;
    int cnt;
    if (top) top[0] = 0;
    if (!G.ok || G.off_stack <= 0 || !pi) return -1;
    data = rdp(pi + G.off_stack);
    cnt = rdi(pi + G.off_stack + 8);
    if (cnt < 0 || cnt > 64) return -1;
    if (cnt > 0 && top && data && (sc = rdp(data)) != 0) lstrcpynA(top, oclass(sc), n);
    return cnt;
}
static float rdf(UINT_PTR a) { float v = 0; rd(a, &v, sizeof v); return v; }
static void rdfstr(UINT_PTR a, char *out, int n)
{
    UINT_PTR data = rdp(a);
    int len = rdi(a + 8), i;
    wchar_t w[64];
    out[0] = 0;
    if (!data || len <= 0 || len > 63 || !rd(data, w, (SIZE_T)len * 2)) return;
    for (i = 0; i < len && i < n - 1 && w[i]; i++) out[i] = (char)w[i];
    out[i] = 0;
}

/* Finds the visible back arrow on the top menu screen (tag containing "Back"); returns its
 * centre in game-window pixels. Logs the candidates so new screens can be checked. */
static BOOL find_back_arrow(float *cx, float *cy, char *tag_out, int tn)
{
    UINT_PTR pi = G.pinput, stack, scene, objs;
    int cnt, n, best = -1;
    float sl, st, sw, sh, bx = 0, by = 0;
    char found[64] = "";
    if (!G.mm_ok || !pi) return FALSE;
    stack = rdp(pi + G.off_stack);
    cnt = rdi(pi + G.off_stack + 8);
    if (cnt <= 0 || !stack || !(scene = rdp(stack))) return FALSE;
    objs = rdp(scene + G.sc_objs);
    n = rdi(scene + G.sc_objs + 8);
    if (!objs || n <= 0 || n > 512) return FALSE;
    sl = rdf(scene + G.sc_left); st = rdf(scene + G.sc_top); sw = rdf(scene + G.sc_w); sh = rdf(scene + G.sc_h);
    for (int i = 0; i < n; i++) {
        UINT_PTR o = rdp(objs + (UINT_PTR)i * 8);
        char tag[64];
        int score;
        float l, t, w, h;
        if (!o) continue;
        if ((unsigned)rdi(o + G.ob_hidden) & (unsigned)G.ob_hidden_mask) continue;
        rdfstr(o + G.ob_tag, tag, sizeof tag);
        if (!strstr(tag, "Back") || strstr(tag, "Background") || strstr(tag, "Glow")) continue;
        /* prefer a screen's own back button over the shared one */
        score = strstr(tag, "Global") ? 1 : 2;
        if (score <= best) continue;
        l = rdf(o + G.ob_left); t = rdf(o + G.ob_top); w = rdf(o + G.ob_w); h = rdf(o + G.ob_h);
        if (w <= 0 || h <= 0) continue;
        if (w <= 1.5f && h <= 1.5f && sw > 2 && sh > 2) { l *= sw; t *= sh; w *= sw; h *= sh; }
        bx = sl + l + w / 2; by = st + t + h / 2;
        best = score;
        lstrcpynA(found, tag, sizeof found);
    }
    if (best < 0) return FALSE;
    *cx = bx; *cy = by;
    lstrcpynA(tag_out, found, tn);
    return TRUE;
}

static void log_scene_buttons(void)
{
    UINT_PTR pi = G.pinput, stack, scene, objs;
    int n;
    char line[900] = "";
    if (!G.mm_ok || !pi || !(stack = rdp(pi + G.off_stack)) || !(scene = rdp(stack))) return;
    objs = rdp(scene + G.sc_objs);
    n = rdi(scene + G.sc_objs + 8);
    for (int i = 0; i < n && i < 128 && objs; i++) {
        UINT_PTR o = rdp(objs + (UINT_PTR)i * 8);
        char tag[64];
        if (!o || ((unsigned)rdi(o + G.ob_hidden) & (unsigned)G.ob_hidden_mask)) continue;
        rdfstr(o + G.ob_tag, tag, sizeof tag);
        if (tag[0] && strlen(line) + strlen(tag) + 2 < sizeof line) { strcat(line, " "); strcat(line, tag); }
    }
    logf_("no back arrow found; visible buttons on %s:%s", oclass(scene), line);
}

static int is_popup(const char *cls)
{
    for (size_t i = 0; i < sizeof POPUPS / sizeof POPUPS[0]; i++) if (!lstrcmpA(cls, POPUPS[i])) return 1;
    return 0;
}

/* ---------- named buttons on the top screen, exploring camera ---------- */

static BOOL wr(UINT_PTR a, const void *v, SIZE_T n)
{
    SIZE_T done = 0;
    if (a < 0x10000) return FALSE;
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)a, v, n, &done) && done == n;
}
/* Exploring: swing the look camera back to centre. The game's camera glides toward
 * DesiredLookRotation, so zeroing it (and the pending mouse look) recentres it smoothly. */
static void recenter_camera(void)
{
    UINT_PTR lc = G.lookcam, pc = G.pc;
    int zero2[2] = { 0, 0 };
    float zf[2] = { 0, 0 };
    if (G.off_desired <= 0 || !track_ok(lc, G.lookcam_class)) { logf_("recentre: exploring camera not found"); return; }
    if (C.debug_input) logf_("recentre: camera was pitch %d yaw %d", rdi(lc + G.off_look), rdi(lc + G.off_look + 4));
    wr(lc + G.off_desired, zero2, sizeof zero2);
    if (G.off_savedlook > 0 && track_ok(pc, G.pc_class)) wr(pc + G.off_savedlook, zf, sizeof zf);
}

/* ---------- 0.6 menus: per-screen controller profiles ----------
 * Each screen the game opens is identified by its class. Screens with a profile get controls
 * made for them (below); every other screen keeps the 0.5 behaviour untouched. Positions come
 * from the game's own data: a menu object's real position is worked out exactly like the game's
 * GetRealPosition (owner screen or anchor object, plus offset), and list rows and drag slots use
 * the rectangle the game last drew them at (VpPos / VpSize). */
enum { PF_NONE, PF_GENERIC, PF_HUB, PF_ITEMS, PF_SUPPLIES, PF_GEMS, PF_PERKS, PF_TABS, PF_GOALS, PF_CUSTOM, PF_FORGE, PF_PROGRESS, PF_COOKER, PF_PINFO };
static const char *PF_NAMES[] = { "none", "generic", "hub", "items", "supplies", "gem bag", "skills", "tabs", "goals", "customize",
                                  "blacksmith", "progress list", "cauldron / fusion", "potion pop-up" };
enum { FT_BUTTON, FT_TAB, FT_LIST, FT_GRID, FT_TOGGLE, FT_SLOT, FT_CELL, FT_ROW };
static const char *FT_NAMES[] = { "button", "tab", "list", "grid", "toggle", "slot", "cell", "row" };
typedef struct { UINT_PTR obj, scene; int sub, row, col; char tag[48]; char cls[48]; float l, t, r, b, cx, cy; int kind, hl; } FTarget;
#define MAXT 128
static FTarget g_ft[MAXT];
static int g_nft;
static UINT_PTR g_scr[8];                   /* open screens, top first, collapsed gear menu skipped */
static int g_nscr;
static UINT_PTR g_popout;                   /* the gear menu (SwordOptionsPopout), when open but collapsed */
static int g_prof;
static UINT_PTR g_fscene;                   /* screen the focus belongs to */
static UINT_PTR g_fobj;                     /* focused target: object (+ sub index) */
static int g_fsub;
static int g_fuser;
static DWORD g_fscene_since;
static volatile int g_menu_cnt;
static char g_menu_top[64];
static struct { char cls[48]; char tag[48]; int sub; } g_fmem[48];
static int g_fmem_n;
/* Gem Bag: a gem being carried (left button held since it was picked up) */
static int g_carry;
static float g_carry_x, g_carry_y;

static BOOL obj_hidden(UINT_PTR o) { return ((unsigned)rdi(o + G.ob_hidden) & (unsigned)G.ob_hidden_mask) != 0; }

/* Real rectangle of a menu object in game-window pixels (like MobileMenuObject.GetRealPosition). */
static BOOL obj_rect(UINT_PTR o, float *l, float *t, float *r, float *b)
{
    float x = 0, y = 0, w = rdf(o + G.ob_w), h = rdf(o + G.ob_h);
    UINT_PTR cur = o;
    if (obj_hidden(o) || !(w > 0 && h > 0) || w > 100000 || h > 100000) return FALSE;
    for (int d = 0; cur && d < 8; d++) {
        UINT_PTR rel = G.ob_relto > 0 ? rdp(cur + G.ob_relto) : 0;
        x += rdf(cur + G.ob_left); y += rdf(cur + G.ob_top);
        if (!rel) {
            UINT_PTR sc = G.ob_owner > 0 ? rdp(cur + G.ob_owner) : 0;
            if (sc) { x += rdf(sc + G.sc_left); y += rdf(sc + G.sc_top); }
            break;
        }
        cur = rel;
    }
    *l = x; *t = y; *r = x + w; *b = y + h;
    return TRUE;
}
/* Where the game last drew a list row or drag slot. */
static BOOL el_rect(UINT_PTR e, float *l, float *t, float *r, float *b)
{
    float x, y, w, h;
    if (!e || G.el_vppos <= 0 || G.el_vpsize <= 0) return FALSE;
    if (G.el_vis_mask && !((unsigned)rdi(e + G.el_vis) & (unsigned)G.el_vis_mask)) return FALSE;
    x = rdf(e + G.el_vppos); y = rdf(e + G.el_vppos + 4); w = rdf(e + G.el_vpsize); h = rdf(e + G.el_vpsize + 4);
    if (!(w > 1 && h > 1) || w > 100000 || h > 100000 || x < -50000 || y < -50000) return FALSE;
    *l = x; *t = y; *r = x + w; *b = y + h;
    return TRUE;
}

/* Visible menu object with this tag in a screen (substring match). */
static UINT_PTR scene_find(UINT_PTR scene, const char *tag)
{
    UINT_PTR objs;
    int n;
    if (!scene || !G.mm_ok) return 0;
    objs = rdp(scene + G.sc_objs);
    n = rdi(scene + G.sc_objs + 8);
    for (int i = 0; i < n && i < 512 && objs; i++) {
        UINT_PTR o = rdp(objs + (UINT_PTR)i * 8);
        char t[64];
        float l, tp, r, b;
        if (!o) continue;
        rdfstr(o + G.ob_tag, t, sizeof t);
        if (strstr(t, tag) && obj_rect(o, &l, &tp, &r, &b)) return o;
    }
    return 0;
}
static UINT_PTR any_find(const char *tag)
{
    for (int i = 0; i < g_nscr; i++) { UINT_PTR o = scene_find(g_scr[i], tag); if (o) return o; }
    return 0;
}

/* The open screens, top first. The gear menu stays in the stack even when folded away (only
 * its gear shows); folded, it is skipped so the screen under it is the one being used. */
static void read_screens(void)
{
    UINT_PTR pi = G.pinput, stack;
    int cnt;
    g_nscr = 0; g_popout = 0;
    if (!G.ok || G.off_stack <= 0 || !pi || (cnt = rdi(pi + G.off_stack + 8)) <= 0 || cnt > 64 || !(stack = rdp(pi + G.off_stack))) return;
    for (int i = 0; i < cnt && g_nscr < 8; i++) {
        UINT_PTR sc = rdp(stack + (UINT_PTR)i * 8);
        if (!sc) continue;
        if (!lstrcmpA(oclass(sc), "SwordOptionsPopout")) {
            /* open only when the game's own IsShowing() says so: opacity above zero AND the gear
             * button showing its "open" icon (the folded menu keeps some opacity) */
            UINT_PTR gear = scene_find(sc, "OptionsPopoutBttn");
            int shown;
            if (G.sc_opacity > 0 && G.btn_icon > 0 && gear) shown = rdf(sc + G.sc_opacity) > 0.0f && rdi(gear + G.btn_icon) == 1;
            else shown = scene_find(sc, "MainScreen") != 0;
            if (!shown) { g_popout = sc; continue; }
        }
        g_scr[g_nscr++] = sc;
    }
}

static int profile_of(const char *cls)
{
    if (!lstrcmpA(cls, "SwordCharacterScene")) return PF_HUB;
    if (!lstrcmpA(cls, "SwordInventoryScene")) return PF_ITEMS;
    if (!lstrcmpA(cls, "SwordSuppliesScene")) return PF_SUPPLIES;
    if (!lstrcmpA(cls, "SwordGemManagementScene")) return PF_GEMS;
    if (!lstrcmpA(cls, "SwordPerksScene")) return PF_PERKS;
    if (!lstrcmpA(cls, "SwordStatsScene")) return PF_TABS;
    if (!lstrcmpA(cls, "SwordAchievementsScene")) return PF_GOALS;
    if (!lstrcmpA(cls, "SwordCharacterViewerControlsScene")) return PF_CUSTOM;
    if (!lstrcmpA(cls, "SwordHideoutForgeSelectionScene")) return PF_FORGE;
    /* SwordPlayerInfoPopupScene (player info + potion button) is deliberately left alone: the game
     * shows it by itself while exploring, so it must never take the left stick or move the cursor. */
    if (!lstrcmpA(cls, "SwordHideoutForgeProgressScene") || !lstrcmpA(cls, "SwordHideoutGemProgressScene") ||
        !lstrcmpA(cls, "SwordHideoutPotionProgressScene")) return PF_PROGRESS;
    if (!lstrcmpA(cls, "SwordHideoutPotionSelectionScene") || !lstrcmpA(cls, "SwordHideoutGemSelectionScene")) return PF_COOKER;
    {
        /* question popups and the open gear menu; reward and info overlays keep the normal controls */
        static const char *gen[] = { "SwordYesNoPopup", "SwordOkPopup", "SwordMapPinPopup", "SwordRateGamePopup", "SwordGrabBagOkPopup",
                                     "SwordWaitPopup", "SwordMcpAuthenticatePopup", "SwordResolveCloudSaveConflictPopup",
                                     "SwordBattleResultsScene", "SwordBattleLossScene", "SwordOptionsPopout",
                                     "SwordHideoutForgeResultsScene", "SwordHideoutGemResultsScene", "SwordHideoutPotionResultsScene" };
        for (size_t i = 0; i < sizeof gen / sizeof *gen; i++) if (!lstrcmpA(cls, gen[i])) return PF_GENERIC;
    }
    return PF_NONE;
}

static int ft_classify(const char *cls, const char *tag)
{
    static const char *skip_tag[] = { "GlobalBack", "InfoBttn", "NoClick", "TouchProtect", "WholeScreen", "Background",
                                      "Overlay", "SnapShotQuit", "ViewAdjuster", "MainScreen" };
    static const char *skip_cls[] = { "XpBar", "BttnStats", "BttnItemAttrib", "BttnAbility", "Draw", "Image", "Anim",
                                      "PlayerInfo", "Text", "RankBar", "StatsBar", "PageSelector", "SoloLevels", "Proxy" };
    size_t n = strlen(cls);
    for (size_t i = 0; i < sizeof skip_tag / sizeof *skip_tag; i++) if (strstr(tag, skip_tag[i])) return -1;
    for (size_t i = 0; i < sizeof skip_cls / sizeof *skip_cls; i++) if (strstr(cls, skip_cls[i])) return -1;
    if (strstr(tag, "ToggleChar")) return FT_TOGGLE;
    if (strstr(cls, "Tab")) return FT_TAB;
    if ((n >= 4 && !strcmp(cls + n - 4, "List")) || strstr(cls, "ListMenu") || strstr(cls, "Wheel")) return FT_LIST;
    if (!strcmp(cls, "MobileMenuInventory")) return FT_GRID;
    if (strstr(cls, "Bttn") || strstr(cls, "Button") || !strcmp(cls, "SwordPerkBar")) return FT_BUTTON;
    return -1;
}

static FTarget *ft_new(UINT_PTR obj, UINT_PTR scene, int sub, int kind, const char *tag, float l, float t, float r, float b)
{
    FTarget *f;
    RECT cr;
    if (g_nft >= MAXT || !g_hwnd || !GetClientRect(g_hwnd, &cr)) return NULL;
    if ((l + r) / 2 < 0 || (t + b) / 2 < 0 || (l + r) / 2 > cr.right || (t + b) / 2 > cr.bottom) return NULL;
    f = &g_ft[g_nft++];
    ZeroMemory(f, sizeof *f);
    f->obj = obj; f->scene = scene; f->sub = sub; f->kind = kind;
    lstrcpynA(f->tag, tag ? tag : "", sizeof f->tag);
    f->l = l; f->t = t; f->r = r; f->b = b; f->cx = (l + r) / 2; f->cy = (t + b) / 2;
    return f;
}

/* Every interactive object of a screen (generic screens and the hub). */
static void add_scene_objects(UINT_PTR scene, const char *const *only, int nonly)
{
    UINT_PTR objs = rdp(scene + G.sc_objs);
    int n = rdi(scene + G.sc_objs + 8);
    for (int i = 0; i < n && i < 512 && objs; i++) {
        UINT_PTR o = rdp(objs + (UINT_PTR)i * 8);
        char tag[64], cls[64];
        float l, t, r, b;
        int kind, ok = !nonly;
        FTarget *f;
        if (!o || !obj_rect(o, &l, &t, &r, &b)) continue;
        rdfstr(o + G.ob_tag, tag, sizeof tag);
        lstrcpynA(cls, oclass(o), sizeof cls);
        for (int k = 0; k < nonly && !ok; k++) if (strstr(tag, only[k])) ok = 1;
        if (!ok || (kind = ft_classify(cls, tag)) < 0) continue;
        if ((f = ft_new(o, scene, 0, kind, tag, l, t, r, b)) != NULL) {
            lstrcpynA(f->cls, cls, sizeof f->cls);
            f->hl = G.ob_hl_mask && ((unsigned)rdi(o + G.ob_hl) & (unsigned)G.ob_hl_mask);
        }
    }
}

/* The visible rows of a menu list, at the rectangles the game last drew them. */
static void add_list_rows(UINT_PTR list, UINT_PTR scene)
{
    UINT_PTR arr;
    float ll, lt, lr, lb;
    int n;
    if (!list || !obj_rect(list, &ll, &lt, &lr, &lb) || G.list_items <= 0) return;
    arr = rdp(list + G.list_items);
    n = rdi(list + G.list_items + 8);
    for (int i = 0; arr && i < n && i < 512; i++) {
        UINT_PTR it = rdp(arr + (UINT_PTR)i * 8);
        float l, t, r, b;
        if (!el_rect(it, &l, &t, &r, &b)) continue;
        if ((t + b) / 2 < lt || (t + b) / 2 > lb || (l + r) / 2 < ll || (l + r) / 2 > lr) {
            /* not inside as screen pixels: try relative to the list */
            l += ll; r += ll; t += lt; b += lt;
            if ((t + b) / 2 < lt || (t + b) / 2 > lb || (l + r) / 2 < ll || (l + r) / 2 > lr) continue;
        }
        if (b - t < 20 || r - l < 20) continue; /* spacer rows */
        ft_new(it, scene, i, FT_ROW, "row", l, t, r, b);
    }
}

/* An int field of a screen, looked up by name on the screen's own class (cached per name). */
static int scene_int(UINT_PTR scene, const char *name, UINT_PTR *cls_cache, int *off_cache)
{
    UINT_PTR cls = rdp(scene + G.o_class), q;
    if (*cls_cache != cls) {
        *cls_cache = cls;
        *off_cache = (q = find_obj(name, "IntProperty", cls)) != 0 ? rdi(q + G.o_poff) : -1;
    }
    return *off_cache > 0 ? rdi(scene + *off_cache) : -1;
}
/* Cauldron / Fusion: where the game's own slot numbering splits the ingredient (or gem) cells
 * from the cauldron (or fusion) slots. */
static void cooker_split(UINT_PTR scene, int *nsrc, int *first_dst)
{
    static UINT_PTR c1, c2, c3, c4;
    static int o1, o2, o3, o4;
    if (!lstrcmpA(oclass(scene), "SwordHideoutPotionSelectionScene")) {
        *nsrc = scene_int(scene, "NumReagentSlots", &c1, &o1);
        *first_dst = scene_int(scene, "CauldronSlotIdx", &c2, &o2);
    } else {
        *nsrc = scene_int(scene, "NumUnequippedSlots", &c3, &o3);
        *first_dst = scene_int(scene, "FirstCookerSlotIdx", &c4, &o4);
    }
}
static UINT_PTR slot_item(UINT_PTR grid, int i)
{
    UINT_PTR arr = G.inv_items > 0 ? rdp(grid + G.inv_items) : 0;
    int n = arr ? rdi(grid + G.inv_items + 8) : 0;
    return (i >= 0 && i < n) ? rdp(arr + (UINT_PTR)i * 8) : 0;
}
/* The slots of a drag grid in game-window pixels (slot positions are kept relative to the grid). */
static void add_grid_slots(UINT_PTR grid, UINT_PTR scene, int kind_split)
{
    UINT_PTR arr;
    float gl, gt, gr, gb, l, t, r, b, ox, oy;
    int n, in_local = 0, in_abs = 0, nsrc = -1, fdst = -1;
    if (!grid || !obj_rect(grid, &gl, &gt, &gr, &gb) || G.inv_slots <= 0 || !(arr = rdp(grid + G.inv_slots)) ||
        (n = rdi(grid + G.inv_slots + 8)) <= 0 || n > 256) return;
    for (int i = 0; i < n; i++) {
        UINT_PTR e = rdp(arr + (UINT_PTR)i * 8);
        if (!el_rect(e, &l, &t, &r, &b)) continue;
        if ((l + r) / 2 + gl < gr && (t + b) / 2 + gt < gb) in_local++;
        if ((l + r) / 2 > gl && (l + r) / 2 < gr && (t + b) / 2 > gt && (t + b) / 2 < gb) in_abs++;
    }
    ox = in_abs > in_local ? 0 : gl; oy = in_abs > in_local ? 0 : gt;
    if (kind_split) cooker_split(scene, &nsrc, &fdst);
    for (int i = 0; i < n; i++) {
        UINT_PTR e = rdp(arr + (UINT_PTR)i * 8);
        FTarget *f;
        if (!el_rect(e, &l, &t, &r, &b)) continue;
        if ((f = ft_new(grid, scene, i, FT_SLOT, "slot", l + ox, t + oy, r + ox, b + oy)) == NULL) continue;
        /* row: 0 = ingredient / gem cell, 1 = cauldron / fusion slot; col: 1 when something is in it */
        if (kind_split && nsrc > 0 && fdst >= 0) f->row = i >= fdst ? 1 : (i < nsrc ? 0 : 2);
        else if (kind_split) {
            /* numbering not readable: cauldron / fusion slots sit right of the cell grid */
            RECT cr;
            f->row = (GetClientRect(g_hwnd, &cr) && f->cx > cr.right * 0.45f) ? 1 : 0;
        } else f->row = 0;
        f->col = slot_item(grid, i) != 0;
    }
}
/* A list item's sub-objects (e.g. a progress row's main bar and FINISH NOW), placed relative to the row. */
static void add_row_parts(UINT_PTR it, UINT_PTR scene, int sub, float rl, float rt, int row)
{
    UINT_PTR arr = G.moli_objs > 0 ? rdp(it + G.moli_objs) : 0;
    int n = arr ? rdi(it + G.moli_objs + 8) : 0;
    for (int i = 0; i < n && i < 64; i++) {
        UINT_PTR o = rdp(arr + (UINT_PTR)i * 8);
        char tag[48];
        float x, y, w, h;
        FTarget *f;
        int col;
        if (!o || obj_hidden(o)) continue;
        rdfstr(o + G.ob_tag, tag, sizeof tag);
        if (!strncmp(tag, "ClickAll", 8)) col = 0;
        else if (!strncmp(tag, "SpeedUpBttn", 11)) col = 1;
        else continue;
        x = rl + rdf(o + G.ob_left); y = rt + rdf(o + G.ob_top); w = rdf(o + G.ob_w); h = rdf(o + G.ob_h);
        if (!(w > 1 && h > 1)) continue;
        if ((f = ft_new(it, scene, sub * 4 + col, FT_CELL, col ? "FinishNow" : "Row", x, y, x + w, y + h)) != NULL) { f->row = row; f->col = col; }
    }
}

/* Builds the targets for the current screen's profile. */
static int ft_collect(void)
{
    UINT_PTR top = g_nscr ? g_scr[0] : 0;
    g_nft = 0;
    if (!G.mm_ok || !top) return 0;
    switch (g_prof) {
    case PF_GENERIC: case PF_PINFO:
        add_scene_objects(top, NULL, 0);
        break;
    case PF_HUB: {
        static const char *hub[] = { "Items", "Supplies", "Perks", "Customize", "Goals" };
        static const char *gear[] = { "OptionsPopoutBttn" };
        add_scene_objects(top, hub, 5);
        if (g_popout) add_scene_objects(g_popout, gear, 1); /* right from the list reaches the gear */
        break;
    }
    case PF_GEMS: {
        /* the gem grid and the item's sockets are the slots of one drag grid; $ sells */
        UINT_PTR grid = scene_find(top, "GemSockets"), sell = scene_find(top, "SellBttn"), arr;
        int n;
        float l, t, r, b;
        float gl, gt, gr, gb;
        if (grid && obj_rect(grid, &gl, &gt, &gr, &gb) && G.inv_slots > 0 && (arr = rdp(grid + G.inv_slots)) != 0 &&
            (n = rdi(grid + G.inv_slots + 8)) > 0 && n < 256) {
            /* slot positions are kept relative to the grid (the game subtracts the grid's corner from
             * a touch before looking up the slot); count which reading lands inside the grid */
            int in_local = 0, in_abs = 0;
            float ox, oy;
            for (int i = 0; i < n; i++) {
                UINT_PTR e = rdp(arr + (UINT_PTR)i * 8);
                if (!el_rect(e, &l, &t, &r, &b)) continue;
                if ((l + r) / 2 + gl > gl && (l + r) / 2 + gl < gr && (t + b) / 2 + gt > gt && (t + b) / 2 + gt < gb) in_local++;
                if ((l + r) / 2 > gl && (l + r) / 2 < gr && (t + b) / 2 > gt && (t + b) / 2 < gb) in_abs++;
            }
            ox = in_abs > in_local ? 0 : gl; oy = in_abs > in_local ? 0 : gt;
            for (int i = 0; i < n; i++) {
                UINT_PTR e = rdp(arr + (UINT_PTR)i * 8);
                if (el_rect(e, &l, &t, &r, &b)) ft_new(grid, top, i, FT_SLOT, "slot", l + ox, t + oy, r + ox, b + oy);
            }
        }
        if (sell && obj_rect(sell, &l, &t, &r, &b)) ft_new(sell, top, 0, FT_BUTTON, "SellBttn", l, t, r, b);
        {
            /* the item's rows under the grid hold its gem sockets: pick a gem out of one, or drop one in */
            UINT_PTR objs = rdp(top + G.sc_objs);
            int no = rdi(top + G.sc_objs + 8);
            for (int i = 0; i < no && i < 512 && objs; i++) {
                UINT_PTR o = rdp(objs + (UINT_PTR)i * 8);
                if (o && !lstrcmpA(oclass(o), "SwordInventoryBttnItemAttrib") && obj_rect(o, &l, &t, &r, &b))
                    ft_new(o, top, 0, FT_SLOT, "socket", l, t, r, b);
            }
        }
        break;
    }
    case PF_PERKS: {
        /* 4 stat rows: column 0 is the stat number (adds a point), then the row's skills */
        static const char *rows[] = { "HealthPerks", "DamagePerks", "ShieldPerks", "MagicPerks" };
        for (int ri = 0; ri < 4; ri++) {
            UINT_PTR bar = scene_find(top, rows[ri]), lvl;
            float l, t, r, b, ll, lt, lr, lb;
            int cnt;
            if (!bar || !obj_rect(bar, &l, &t, &r, &b)) continue;
            lvl = G.perk_lvl > 0 ? rdp(bar + G.perk_lvl) : 0;
            if (!lvl || !obj_rect(lvl, &ll, &lt, &lr, &lb) || lr <= l || lr >= r) { ll = l; lr = l + (r - l) * 0.235f; lt = t; lb = b; }
            {
                FTarget *f = ft_new(bar, top, 0, FT_CELL, rows[ri], ll, t, lr, b);
                if (f) { f->row = ri; f->col = 0; }
            }
            cnt = G.perk_cnt > 0 ? rdi(bar + G.perk_cnt) : 6;
            if (cnt < 1 || cnt > 12) cnt = 6;
            for (int c = 0; c < cnt; c++) {
                float w = (r - lr) / cnt;
                FTarget *f = ft_new(bar, top, c + 1, FT_CELL, rows[ri], lr + c * w, t, lr + (c + 1) * w, b);
                if (f) { f->row = ri; f->col = c + 1; }
            }
        }
        break;
    }
    case PF_GOALS:
        add_list_rows(scene_find(top, "AchievementsList"), top);
        break;
    case PF_PROGRESS: {
        /* each slot row: its main bar (column 0) and its FINISH NOW (column 1) */
        UINT_PTR list = scene_find(top, "ProgressList"), arr;
        float ll, lt, lr, lb;
        int n, row = 0;
        if (!list || !obj_rect(list, &ll, &lt, &lr, &lb) || G.list_items <= 0) break;
        arr = rdp(list + G.list_items);
        n = rdi(list + G.list_items + 8);
        for (int i = 0; arr && i < n && i < 128; i++) {
            UINT_PTR it = rdp(arr + (UINT_PTR)i * 8);
            float l, t, r, b;
            if (!el_rect(it, &l, &t, &r, &b) || b - t < 20) continue;
            if ((t + b) / 2 < lt || (t + b) / 2 > lb) { l += ll; r += ll; t += lt; b += lt; if ((t + b) / 2 < lt || (t + b) / 2 > lb) continue; }
            {
                int before = g_nft;
                add_row_parts(it, top, i, l, t, row);
                if (g_nft == before) { FTarget *f = ft_new(it, top, i * 4, FT_CELL, "Row", l, t, r, b); if (f) { f->row = row; f->col = 0; } }
            }
            row++;
        }
        break;
    }
    case PF_COOKER: {
        static const char *btns[] = { "BrewBttn", "CancelBrewBttn", "CookBttn", "SellBttn", "UnlockBttn" };
        add_grid_slots(scene_find(top, "Sockets"), top, 1);
        add_scene_objects(top, btns, 5);
        break;
    }
    case PF_CUSTOM: {
        /* the side bar's tabs and buttons, plus the rows of the open sub-menu list */
        static const char *side[] = { "Tab", "ActionBttn", "Buy3dBttn", "UploadButton" };
        add_scene_objects(top, side, 4);
        add_list_rows(scene_find(top, "ToolList"), top);
        break;
    }
    default:
        break;
    }
    return g_nft;
}

static void dump_one(UINT_PTR scene, const char *why)
{
    UINT_PTR objs;
    int n;
    if (!scene || !G.mm_ok) return;
    objs = rdp(scene + G.sc_objs);
    n = rdi(scene + G.sc_objs + 8);
    logf_("screen %s (%s): %d menu objects", oclass(scene), why, n);
    for (int i = 0; i < n && i < 160 && objs; i++) {
        UINT_PTR o = rdp(objs + (UINT_PTR)i * 8);
        char tag[64];
        const char *cls;
        float l, t, r, b;
        if (!o) continue;
        rdfstr(o + G.ob_tag, tag, sizeof tag);
        cls = oclass(o);
        if (obj_rect(o, &l, &t, &r, &b)) logf_("  %-28s %-30s %5.0f,%5.0f %5.0fx%-5.0f", tag, cls, l, t, r - l, b - t);
        else logf_("  %-28s %-30s hidden", tag, cls);
    }
}
static void dump_screen(const char *why)
{
    for (int i = 0; i < g_nscr && i < 2; i++) dump_one(g_scr[i], i ? "underneath" : why);
    if (g_prof >= PF_GEMS && ft_collect()) {
        for (int i = 0; i < g_nft; i++)
            logf_("  target %-14s %-5s sub %2d at %5.0f,%5.0f %5.0fx%-5.0f", g_ft[i].tag, FT_NAMES[g_ft[i].kind], g_ft[i].sub,
                  g_ft[i].l, g_ft[i].t, g_ft[i].r - g_ft[i].l, g_ft[i].b - g_ft[i].t);
    }
}

/* ---- the outline: a click-through window owned by the game window ---- */
static HWND g_ov;
static volatile LONG g_ov_x, g_ov_y, g_ov_w, g_ov_h, g_ov_show;
static LONG g_ov_last[5] = { -1, -1, -1, -1, -1 };
#define OV_KEY RGB(255, 0, 255)
static LRESULT CALLBACK ov_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r, cr;
        HBRUSH key = CreateSolidBrush(OV_KEY), dark = CreateSolidBrush(RGB(20, 16, 8)), gold = CreateSolidBrush(C.focus_color);
        int th = 3;
        if (g_hwnd && GetClientRect(g_hwnd, &cr) && cr.bottom > 0) th = cr.bottom / 400 + 2;
        GetClientRect(h, &r);
        FillRect(dc, &r, dark);
        InflateRect(&r, -1, -1);
        FillRect(dc, &r, gold);
        InflateRect(&r, -th, -th);
        FillRect(dc, &r, dark);
        InflateRect(&r, -1, -1);
        FillRect(dc, &r, key);
        DeleteObject(key); DeleteObject(dark); DeleteObject(gold);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_APP + 1) {
        if (g_ov_show) {
            SetWindowPos(h, NULL, g_ov_x, g_ov_y, g_ov_w, g_ov_h, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
            InvalidateRect(h, NULL, TRUE);
        } else ShowWindow(h, SW_HIDE);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}
static DWORD WINAPI ov_thread(LPVOID arg)
{
    WNDCLASSA wc;
    MSG msg;
    (void)arg;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = ov_proc;
    wc.hInstance = g_self;
    wc.lpszClassName = "IB3PadFocus";
    RegisterClassA(&wc);
    g_ov = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, "IB3PadFocus", "",
                           WS_POPUP, 0, 0, 8, 8, g_hwnd, NULL, g_self, NULL);
    if (!g_ov) { logf_("focus outline: window not created (%lu)", GetLastError()); return 0; }
    SetLayeredWindowAttributes(g_ov, OV_KEY, 0, LWA_COLORKEY);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    return 0;
}
static void ov_set(int show, float l, float t, float r, float b)
{
    static int started;
    POINT tl, br;
    LONG v[5];
    if (!C.focus_outline || !g_hwnd) return;
    if (!started) {
        HANDLE th;
        if (!show) return;
        th = CreateThread(NULL, 0, ov_thread, NULL, 0, NULL);
        if (th) CloseHandle(th);
        started = 1;
        Sleep(20);
    }
    if (!g_ov) return;
    tl.x = (LONG)l; tl.y = (LONG)t; br.x = (LONG)r; br.y = (LONG)b;
    ClientToScreen(g_hwnd, &tl);
    ClientToScreen(g_hwnd, &br);
    v[0] = show; v[1] = tl.x - 6; v[2] = tl.y - 6; v[3] = br.x - tl.x + 12; v[4] = br.y - tl.y + 12;
    if (!show) { if (g_ov_last[0] <= 0) return; v[1] = v[2] = v[3] = v[4] = 0; }
    if (!memcmp(v, g_ov_last, sizeof v)) return;
    memcpy(g_ov_last, v, sizeof v);
    g_ov_show = v[0]; g_ov_x = v[1]; g_ov_y = v[2]; g_ov_w = v[3]; g_ov_h = v[4];
    PostMessageA(g_ov, WM_APP + 1, 0, 0);
}

static void focus_clear(void)
{
    g_fobj = 0;
    ov_set(0, 0, 0, 0, 0);
}

static void fmem_store(const char *cls, const char *tag, int sub)
{
    int i;
    for (i = 0; i < g_fmem_n; i++) if (!lstrcmpA(g_fmem[i].cls, cls)) break;
    if (i == g_fmem_n) { if (g_fmem_n == 48) i = 47; else g_fmem_n++; }
    lstrcpynA(g_fmem[i].cls, cls, sizeof g_fmem[i].cls);
    lstrcpynA(g_fmem[i].tag, tag, sizeof g_fmem[i].tag);
    g_fmem[i].sub = sub;
}

/* Smooth cursor move (several small steps) so a held button reads as a drag. */
static void cursor_glide(float x, float y, int steps)
{
    POINT a, p;
    GetCursorPos(&a);
    p.x = (LONG)x; p.y = (LONG)y;
    ClientToScreen(g_hwnd, &p);
    for (int i = 1; i <= steps; i++) {
        mouse_to(a.x + (p.x - a.x) * i / steps, a.y + (p.y - a.y) * i / steps);
        if (i < steps) Sleep(8);
    }
}

/* Where the cursor goes for a target: its centre, or a goal row's COLLECT button. */
static void target_point(const FTarget *f, float *x, float *y)
{
    *x = f->cx; *y = f->cy;
    if (!strcmp(f->tag, "socket")) *x = f->l + (f->b - f->t); /* the gem icon at the row's left */
    if (g_prof == PF_GOALS && f->kind == FT_ROW) {
        UINT_PTR arr = G.moli_objs > 0 ? rdp(f->obj + G.moli_objs) : 0;
        int n = arr ? rdi(f->obj + G.moli_objs + 8) : 0;
        *x = f->l + (f->r - f->l) * 0.756f; *y = f->t + (f->b - f->t) * 0.62f;
        for (int i = 0; i < n && i < 64; i++) {
            UINT_PTR o = rdp(arr + (UINT_PTR)i * 8);
            char tag[48];
            float cx, cy;
            if (!o) continue;
            rdfstr(o + G.ob_tag, tag, sizeof tag);
            if (strcmp(tag, "CollectBttn")) continue;
            cx = f->l + rdf(o + G.ob_left) + rdf(o + G.ob_w) * 0.25f; cy = f->t + rdf(o + G.ob_top) + rdf(o + G.ob_h) / 2; /* the COLLECT label */
            if (cx > f->l && cx < f->r && cy > f->t && cy < f->b) { *x = cx; *y = cy; }
            break;
        }
    }
}

static void focus_set(int i, const char *why)
{
    FTarget *f = &g_ft[i];
    float px, py;
    g_fobj = f->obj;
    g_fsub = f->sub;
    if (g_fuser) fmem_store(g_menu_top, f->tag, f->sub);
    ov_set(1, f->l, f->t, f->r, f->b);
    target_point(f, &px, &py);
    cursor_glide(px, py, g_carry ? 6 : 1);
    if (C.debug_input) logf_("focus: %s %s %d at %.0f,%.0f on %s [%s]%s", f->tag, FT_NAMES[f->kind], f->sub, f->cx, f->cy, g_menu_top,
                             why, g_carry ? " (carrying)" : "");
}

static int ft_index(UINT_PTR o, int sub)
{
    for (int i = 0; i < g_nft; i++) if (g_ft[i].obj == o && g_ft[i].sub == sub) return i;
    return -1;
}

static int target_at_cursor(int nearest)
{
    POINT p;
    int best = -1;
    float bd = 1e18f;
    GetCursorPos(&p);
    ScreenToClient(g_hwnd, &p);
    for (int i = 0; i < g_nft; i++) {
        float ex, ey, d;
        if (p.x >= g_ft[i].l && p.x <= g_ft[i].r && p.y >= g_ft[i].t && p.y <= g_ft[i].b) return i;
        ex = g_ft[i].cx - p.x; ey = g_ft[i].cy - p.y; d = ex * ex + ey * ey;
        if (d < bd) { bd = d; best = i; }
    }
    return nearest ? best : -1;
}


static int focus_pick(int from, int dx, int dy)
{
    int best = -1;
    float bs = 1e9f, fx = g_ft[from].cx, fy = g_ft[from].cy;
    if (g_prof == PF_PERKS) {
        /* up/down: the stat number of the next row; left/right: along the row */
        int row = g_ft[from].row + dy, col = dy ? 0 : g_ft[from].col + dx;
        for (int i = 0; i < g_nft; i++) if (g_ft[i].row == row && g_ft[i].col == col) return i;
        return -1;
    }
    for (int pass = 0; pass < 2 && best < 0; pass++)
        for (int i = 0; i < g_nft; i++) {
            float vx, vy, along, side, score;
            if (i == from) continue;
            vx = g_ft[i].cx - fx; vy = g_ft[i].cy - fy;
            along = vx * dx + vy * dy;
            side = fabsf(vx * dy - vy * dx);
            if (along <= 4) continue;
            if (pass == 0 && side > along * 2.2f) continue;
            score = along + side * 2.0f;
            if (score < bs) { bs = score; best = i; }
        }
    return best;
}

static void click_client(float x, float y, int restore)
{
    POINT pt = { (LONG)x, (LONG)y }, old;
    GetCursorPos(&old);
    ClientToScreen(g_hwnd, &pt);
    mouse_to(pt.x, pt.y);
    mouse_flags(MOUSEEVENTF_LEFTDOWN, 0);
    Sleep(30);
    mouse_flags(MOUSEEVENTF_LEFTUP, 0);
    Sleep(10);
    if (restore) mouse_to(old.x, old.y);
}
/* Presses a named button wherever it is (live position); FALSE if it is not on screen. */
static BOOL press_tag(const char *tag, int restore)
{
    UINT_PTR o = any_find(tag);
    float l, t, r, b;
    if (!o || !obj_rect(o, &l, &t, &r, &b)) return FALSE;
    if (C.debug_input) logf_("press button %s at %.0f,%.0f on %s", tag, (l + r) / 2, (t + b) / 2, g_menu_top);
    click_client((l + r) / 2, (t + b) / 2, restore);
    return TRUE;
}
/* Puts the cursor (and outline) on a named button without pressing it. */
static void park_on(const char *tag)
{
    UINT_PTR o = any_find(tag);
    float l, t, r, b;
    POINT p;
    if (!o || !obj_rect(o, &l, &t, &r, &b)) { ov_set(0, 0, 0, 0, 0); return; }
    ov_set(1, l, t, r, b);
    p.x = (LONG)((l + r) / 2); p.y = (LONG)((t + b) / 2);
    ClientToScreen(g_hwnd, &p);
    mouse_to(p.x, p.y);
}
/* Supplies: in some categories (keys, ingredients, gold, grab bags) the game hides the BUY / USE
 * pair and shows one centred button instead (SwordSuppliesScene.UseCenteredButton). */
static const char *sup_action(void)
{
    UINT_PTR o;
    float l, t, r, b;
    if (g_prof == PF_SUPPLIES && !((o = any_find("ActionBttn")) && obj_rect(o, &l, &t, &r, &b)) &&
        (o = any_find("CenteredBttn")) && obj_rect(o, &l, &t, &r, &b)) return "CenteredBttn";
    return "ActionBttn";
}
/* LB / RB through a set of tabs; the highlighted tab is the current one. */
static BOOL cycle_tabs(const char *const *tags, int n, int step)
{
    int have = 0, cur = -1, idx[8];
    for (int i = 0; i < n && i < 8; i++) {
        UINT_PTR x = any_find(tags[i]);
        if (!x) continue;
        idx[have] = i;
        if (G.ob_hl_mask && ((unsigned)rdi(x + G.ob_hl) & (unsigned)G.ob_hl_mask)) cur = have;
        have++;
    }
    if (have < 2) return have == 1 ? press_tag(tags[idx[0]], 1) : FALSE;
    cur = cur < 0 ? 0 : (cur + step + have) % have;
    return press_tag(tags[idx[cur]], 1);
}

static void carry_drop(float x, float y, const char *why)
{
    cursor_glide(x, y, 6);
    Sleep(15);
    mouse_flags(MOUSEEVENTF_LEFTUP, 0);
    g_carry = 0;
    if (C.debug_input) logf_("gem bag: %s at %.0f,%.0f", why, x, y);
}

static void drag_list(UINT_PTR list, float amount)
{
    float l, t, r, b;
    POINT old;
    if (!list || !obj_rect(list, &l, &t, &r, &b)) return;
    GetCursorPos(&old);
    cursor_glide((l + r) / 2, (t + b) / 2, 1);
    mouse_flags(MOUSEEVENTF_LEFTDOWN, 0);
    Sleep(15);
    cursor_glide((l + r) / 2, (t + b) / 2 + amount, 8);
    Sleep(15);
    mouse_flags(MOUSEEVENTF_LEFTUP, 0);
    Sleep(10);
    mouse_to(old.x, old.y);
}

static BOOL list_vertical(UINT_PTR list)
{
    float l, t, r, b;
    if (G.list_vert > 0 && G.list_vert_mask) return ((unsigned)rdi(list + G.list_vert) & (unsigned)G.list_vert_mask) != 0;
    return obj_rect(list, &l, &t, &r, &b) && (b - t) >= (r - l);
}
/* Size of one item of a list (from where the game drew its items), for one-item drags. */
static float list_step(UINT_PTR list, int vertical)
{
    UINT_PTR arr = G.list_items > 0 ? rdp(list + G.list_items) : 0;
    int n = arr ? rdi(list + G.list_items + 8) : 0;
    float l, t, r, b;
    for (int i = 0; i < n && i < 256; i++)
        if (el_rect(rdp(arr + (UINT_PTR)i * 8), &l, &t, &r, &b) && (vertical ? b - t : r - l) > 20) return vertical ? b - t : r - l;
    if (obj_rect(list, &l, &t, &r, &b)) return (vertical ? b - t : r - l) / 3;
    return 150;
}
/* Drags a list by one item: step > 0 shows the next item, < 0 the previous one. */
static void list_drag_step(UINT_PTR list, int step)
{
    float l, t, r, b, cx, cy, d;
    int vert;
    POINT old;
    if (!list || !obj_rect(list, &l, &t, &r, &b)) return;
    vert = list_vertical(list);
    d = list_step(list, vert) * (step > 0 ? -1.0f : 1.0f);
    cx = (l + r) / 2; cy = (t + b) / 2;
    GetCursorPos(&old);
    cursor_glide(cx, cy, 1);
    mouse_flags(MOUSEEVENTF_LEFTDOWN, 0);
    Sleep(15);
    cursor_glide(vert ? cx : cx + d, vert ? cy + d : cy, 8);
    Sleep(15);
    mouse_flags(MOUSEEVENTF_LEFTUP, 0);
    Sleep(10);
    mouse_to(old.x, old.y);
    if (C.debug_input) logf_("list drag %s by %.0f (%s list)", step > 0 ? "next" : "previous", d, vert ? "vertical" : "sideways");
}
/* Press at one point, carry to another and let go (a whole drag in one go). */
static void drag_point(float x0, float y0, float x1, float y1)
{
    cursor_glide(x0, y0, 1);
    mouse_flags(MOUSEEVENTF_LEFTDOWN, 0);
    Sleep(25);
    cursor_glide(x0 + 6, y0 + 6, 2);
    cursor_glide(x1, y1, 10);
    Sleep(25);
    mouse_flags(MOUSEEVENTF_LEFTUP, 0);
    Sleep(15);
}

/* One D-pad / stick step (1 up, 2 down, 3 left, 4 right). Returns 1 if the screen took it. */
static int menu_dir(int dir)
{
    int dx = dir == 3 ? -1 : dir == 4 ? 1 : 0, dy = dir == 1 ? -1 : dir == 2 ? 1 : 0, cur, nx;
    switch (g_prof) {
    case PF_ITEMS: case PF_SUPPLIES:
        /* the item box: up/down scroll items, left/right change the item type (the game's own scrolling) */
        if (dy) wheel(dy < 0 ? 120 : -120, FALSE);
        else wheel(dx < 0 ? 120 : -120, TRUE);
        return 1;
    case PF_TABS:
        return 0;
    case PF_NONE:
        return 0;
    case PF_FORGE: {
        /* along the item list: step items; across it: the item-type arrows */
        UINT_PTR list = any_find("ItemList");
        int vert = list ? list_vertical(list) : 1;
        if (list && ((vert && dy) || (!vert && dx))) list_drag_step(list, vert ? dy : dx);
        else press_tag((vert ? dx : dy) < 0 ? "ItemFilterBttnLeft" : "ItemFilterBttnRight", 1);
        park_on("StartForgeBttn");
        return 1;
    }
    default:
        break;
    }
    if (!ft_collect()) return g_prof == PF_GOALS ? (wheel(dy < 0 ? 120 : -120, FALSE), 1) : 0;
    cur = g_fobj ? ft_index(g_fobj, g_fsub) : -1;
    g_fuser = 1;
    if (cur < 0) {
        /* no outline yet: start from the target under the cursor, or snap to the nearest one */
        if ((cur = target_at_cursor(0)) < 0) { focus_set(target_at_cursor(1), "nearest"); return 1; }
    }
    nx = focus_pick(cur, dx, dy);
    if (nx >= 0) { focus_set(nx, "move"); return 1; }
    if (g_prof == PF_GOALS && dy) {
        /* past the top or bottom row: scroll the list, focus stays on the edge row */
        wheel(dy < 0 ? 120 : -120, FALSE);
        focus_set(cur, "scroll");
        return 1;
    }
    if (g_prof == PF_PROGRESS && dy) {
        list_drag_step(any_find("ProgressList"), dy);
        Sleep(120);
        if (ft_collect()) { int k = target_at_cursor(1); if (k >= 0) focus_set(k, "scroll"); }
        return 1;
    }
    if (g_prof == PF_CUSTOM && dy && g_ft[cur].kind == FT_ROW) {
        /* this list only scrolls by dragging: drag it by one row */
        drag_list(any_find("ToolList"), (g_ft[cur].b - g_ft[cur].t) * (dy < 0 ? 1.0f : -1.0f));
        Sleep(120);
        if (ft_collect() && (nx = ft_index(g_fobj, g_fsub)) >= 0) { int k = focus_pick(nx, dx, dy); focus_set(k >= 0 ? k : nx, "scroll"); }
        return 1;
    }
    focus_set(cur, "edge");
    return 1;
}

/* A: a plain click at the cursor, except Items / Supplies (buy, equip, use) and the Gem Bag
 * (pick a gem up, drop it). Returns 1 when the screen used the press. */
static int cooker_move(int from)
{
    /* into the first empty cauldron / fusion slot, or back out to the first empty cell */
    int want_row = g_ft[from].row == 1 ? 0 : 1, to = -1;
    float x0 = g_ft[from].cx, y0 = g_ft[from].cy;
    if (!g_ft[from].col) { if (C.debug_input) logf_("cooker: slot %d is empty", g_ft[from].sub); return 0; }
    for (int i = 0; i < g_nft; i++)
        if (g_ft[i].kind == FT_SLOT && g_ft[i].row == want_row && !g_ft[i].col && (to < 0 || g_ft[i].sub < g_ft[to].sub)) to = i;
    if (to < 0) { if (C.debug_input) logf_("cooker: no empty %s slot", want_row ? "cauldron / fusion" : "cell"); return 0; }
    if (C.debug_input) logf_("cooker: slot %d -> slot %d", g_ft[from].sub, g_ft[to].sub);
    drag_point(x0, y0, g_ft[to].cx, g_ft[to].cy);
    cursor_glide(x0, y0, 1); /* back where you were, ready for the next one */
    return 1;
}

static int menu_a(void)
{
    if (g_prof == PF_FORGE) {
        if (!press_tag("StartForgeBttn", 0) && C.debug_input) logf_("A: no UPGRADE button showing");
        park_on("StartForgeBttn");
        return 1;
    }
    if (g_prof == PF_COOKER) {
        int i;
        if (!ft_collect() || (i = target_at_cursor(0)) < 0 || g_ft[i].kind != FT_SLOT) return 0; /* buttons: plain click */
        cooker_move(i);
        return 1;
    }
    if (g_prof == PF_ITEMS || g_prof == PF_SUPPLIES) {
        const char *act = sup_action();
        if (!press_tag(act, 0) && C.debug_input) logf_("A: no buy / equip / use button showing");
        park_on(sup_action());
        return 1;
    }
    if (g_prof == PF_GEMS) {
        POINT p;
        int i;
        if (g_carry) {
            GetCursorPos(&p);
            ScreenToClient(g_hwnd, &p);
            carry_drop((float)p.x, (float)p.y, "dropped");
            return 1;
        }
        if (!ft_collect() || (i = target_at_cursor(0)) < 0 || g_ft[i].kind != FT_SLOT) return 0; /* not on a slot: plain click */
        mouse_flags(MOUSEEVENTF_LEFTDOWN, 0);
        g_carry = 1;
        GetCursorPos(&p);
        ScreenToClient(g_hwnd, &p);
        g_carry_x = (float)p.x; g_carry_y = (float)p.y;
        Sleep(20);
        cursor_glide(g_carry_x + 6, g_carry_y + 6, 2); /* nudge so the game starts the drag */
        if (C.debug_input) logf_("gem bag: picked up from slot %d", g_ft[i].sub);
        return 1;
    }
    return 0;
}

/* Runs on the worker thread: follows the open screens, starts each profiled screen on its
 * main target, keeps the outline on the focus while things animate. */
static void focus_tick(DWORD now)
{
    static DWORD next, last_check;
    static UINT_PTR dumped;
    UINT_PTR sc;
    int cnt;
    if ((LONG)(now - next) < 0) return;
    next = now + 40;
    cnt = game_menu(NULL, 0);
    g_menu_cnt = cnt;
    if (!C.focus_on || cnt <= 0 || !g_focused) {
        if (g_carry) { mouse_flags(MOUSEEVENTF_LEFTUP, 0); g_carry = 0; }
        if (g_fobj || g_ov_last[0] > 0) focus_clear();
        if (cnt <= 0) { g_fscene = 0; g_prof = PF_NONE; g_menu_top[0] = 0; }
        return;
    }
    read_screens();
    sc = g_nscr ? g_scr[0] : 0;
    if (sc != g_fscene) {
        if (g_carry) { mouse_flags(MOUSEEVENTF_LEFTUP, 0); g_carry = 0; }
        g_fscene = sc;
        lstrcpynA(g_menu_top, sc ? oclass(sc) : "", sizeof g_menu_top);
        g_prof = sc ? profile_of(g_menu_top) : PF_NONE;
        g_fuser = 0;
        g_fscene_since = now;
        focus_clear();
        if (C.debug_input) logf_("controls: %s -> %s", g_menu_top[0] ? g_menu_top : "none", PF_NAMES[g_prof]);
        return;
    }
    if (g_prof == PF_NONE) return;
    if ((LONG)(now - g_fscene_since) >= 450) {
        if ((C.dump_screens || C.debug_input) && dumped != sc) { dump_screen("opened"); dumped = sc; }
        if (!g_fuser && C.focus_auto && (LONG)(now - last_check) >= 300 &&
            (g_prof == PF_ITEMS || g_prof == PF_SUPPLIES || g_prof == PF_FORGE || g_prof == PF_PINFO)) {
            last_check = now;
            park_on(g_prof == PF_FORGE ? "StartForgeBttn" : g_prof == PF_PINFO ? "PotionBttn" : sup_action());
        }
    }
    if (g_fobj && g_prof != PF_ITEMS && g_prof != PF_SUPPLIES && g_prof != PF_FORGE && (LONG)(now - last_check) >= 80) {
        int i;
        if (ft_collect() && (i = ft_index(g_fobj, g_fsub)) >= 0) ov_set(1, g_ft[i].l, g_ft[i].t, g_ft[i].r, g_ft[i].b);
        else if (!g_carry) focus_clear();
    }
}

/* All menu buttons for a profiled screen. Returns the buttons it used. */
static DWORD menu_buttons(DWORD newly, DWORD b, float lx, float ly, float rx, float ry, DWORD now)
{
    static int held_dir, sdir;
    static DWORD t0, tl;
    DWORD used = 0;
    int dir = 0;
    float sm = sqrtf(lx * lx + ly * ly);
    if (C.focus_stick) {
        if (sm < 0.35f) sdir = 0;
        else if (sm > 0.6f && !sdir) sdir = fabsf(ly) >= fabsf(lx) ? (ly > 0 ? 1 : 2) : (lx < 0 ? 3 : 4);
    } else sdir = 0;
    if (b & 0x1) dir = 1; else if (b & 0x2) dir = 2; else if (b & 0x4) dir = 3; else if (b & 0x8) dir = 4;
    if (!dir) dir = sdir;
    if (dir != held_dir) {
        held_dir = dir;
        t0 = tl = now;
        if (dir && menu_dir(dir)) used |= DPAD_MASK;
    } else if (dir) {
        used |= DPAD_MASK;
        if ((LONG)(now - t0) >= C.focus_delay && (LONG)(now - tl) >= C.focus_rep) { menu_dir(dir); tl = now; }
    }
    if ((newly & BTN_MASK[B_A]) && menu_a()) used |= BTN_MASK[B_A];
    if ((newly & BTN_MASK[B_X]) && g_prof == PF_COOKER) {
        /* Cauldron: the cauldron's own cancel (empties it); Fusion: take the last gem back out */
        if (!press_tag("CancelBrewBttn", 1) && ft_collect()) {
            int last = -1;
            POINT old;
            for (int i = 0; i < g_nft; i++) if (g_ft[i].kind == FT_SLOT && g_ft[i].row == 1 && g_ft[i].col && (last < 0 || g_ft[i].sub > g_ft[last].sub)) last = i;
            GetCursorPos(&old);
            if (last >= 0 && cooker_move(last)) mouse_to(old.x, old.y);
        }
        used |= BTN_MASK[B_X];
    }
    if ((newly & BTN_MASK[B_X]) && g_prof != PF_PERKS && g_prof != PF_ITEMS && g_prof != PF_SUPPLIES && g_prof != PF_TABS &&
        g_prof != PF_COOKER && g_prof != PF_FORGE) {
        int i;
        /* X: put the outline and cursor on the button nearest the cursor */
        if (ft_collect() && (i = target_at_cursor(1)) >= 0) { g_fuser = 1; focus_set(i, "X"); }
        used |= BTN_MASK[B_X];
    }
    if ((newly & BTN_MASK[B_B]) && g_carry) { carry_drop(g_carry_x, g_carry_y, "put back"); used |= BTN_MASK[B_B]; }
    if (newly & (BTN_MASK[B_LB] | BTN_MASK[B_RB])) {
        int step = (newly & BTN_MASK[B_RB]) ? 1 : -1;
        static const char *items_tabs[] = { "ShowAllTab", "ShowInventoryTab", "ShowStoreTab" };
        static const char *stat_tabs[] = { "PerksTab", "StatsTab" };
        BOOL done = FALSE;
        if (g_prof == PF_HUB || g_prof == PF_FORGE) done = press_tag("ToggleChar", 1);
        else if (g_prof == PF_ITEMS) done = cycle_tabs(items_tabs, 3, step);
        else if (g_prof == PF_PERKS || g_prof == PF_TABS) done = cycle_tabs(stat_tabs, 2, step);
        if (done || g_prof == PF_SUPPLIES || g_prof == PF_GEMS) used |= newly & (BTN_MASK[B_LB] | BTN_MASK[B_RB]);
    }
    if (newly & (BTN_MASK[B_LT] | BTN_MASK[B_RT])) {
        if (g_prof == PF_ITEMS || g_prof == PF_GEMS) {
            if (g_carry) carry_drop(g_carry_x, g_carry_y, "put back");
            press_tag((newly & BTN_MASK[B_RT]) ? "GemsTab" : "ItemsTab", 1);
            used |= newly & (BTN_MASK[B_LT] | BTN_MASK[B_RT]);
        }
    }
    if (newly & BTN_MASK[B_Y]) {
        BOOL done = FALSE;
        if (g_prof == PF_HUB && g_popout) {
            UINT_PTR o = scene_find(g_popout, "OptionsPopoutBttn");
            float l, t, r, bb;
            if (o && obj_rect(o, &l, &t, &r, &bb)) { click_client((l + r) / 2, (t + bb) / 2, 1); done = TRUE; }
        } else if (g_prof == PF_SUPPLIES) done = press_tag("StoreBttn", 0) || press_tag("CenteredBttn", 0);
        else if (g_prof == PF_COOKER) done = press_tag("BrewBttn", 1) || press_tag("CookBttn", 1); /* COOK / FUSE */
        else if (g_prof == PF_PERKS) done = press_tag("AcceptStatsUpgrade", 1);
        else if (g_prof == PF_GEMS) {
            /* sell: carry the focused (or held) gem to $ and let go; the game asks to confirm */
            UINT_PTR s = any_find("SellBttn");
            float l, t, r, bb;
            if (s && obj_rect(s, &l, &t, &r, &bb)) {
                if (!g_carry) menu_a();
                if (g_carry) carry_drop((l + r) / 2, (t + bb) / 2, "sold");
                else if (C.debug_input) logf_("gem bag: Y needs the cursor on a gem");
                done = TRUE;
            }
        }
        if (done || g_prof == PF_PERKS) used |= BTN_MASK[B_Y];
    }
    if ((newly & BTN_MASK[B_X]) && g_prof == PF_PERKS) { press_tag("RebalanceStatPoints", 1); used |= BTN_MASK[B_X]; }
    if (newly & BTN_MASK[B_START]) {
        if (g_prof == PF_PERKS && press_tag("AcceptStatsUpgrade", 1)) used |= BTN_MASK[B_START];
        else if (is_popup(g_menu_top)) {
            Act k;
            ZeroMemory(&k, sizeof k);
            k.type = A_KEY; k.n = 1; k.p[0].type = A_KEY; k.p[0].vk = VK_SUBTRACT;
            lstrcpynA(k.name, "popup OK / Yes", sizeof k.name);
            if (C.debug_input) logf_("press Start -> OK / Yes on %s", g_menu_top);
            tap(&k);
            used |= BTN_MASK[B_START];
        }
    }
    if (newly & BTN_MASK[B_BACK]) {
        if (any_find("InfoBttn") && press_tag("InfoBttn", 1)) used |= BTN_MASK[B_BACK];
    }
    /* the right stick takes the cursor away from the focus (not while carrying a gem) */
    if (!g_carry && (fabsf(rx) > C.cur_dead || fabsf(ry) > C.cur_dead)) { g_fuser = 1; if (g_fobj) focus_clear(); }
    return used;
}

static int is_fight_mode(int m)
{
    /* HM_DEATH is the finishing bonus after a boss falls: still slashing, so still a fight. */
    return m == HM_BOSSBATTLE || m == HM_BOSSRECOVERY || m == HM_CASTMAGIC || m == HM_FINALSTRIKE || m == HM_CLASHMODE ||
           m == HM_DEATH;
}

static int g_nav = 1; /* not in a fight: left stick uses the click-and-drag swipe (set from game state) */

static void slash(float sx, float sy)
{
    float m = sqrtf(sx * sx + sy * sy), a;
    if (m < 1e-4f) return;
    a = atan2f(sy, sx); /* math angle, up positive */
    if (g_nav && C.nav_swipe) {
        /* Exploring: the original click-and-drag swipe, which turns the camera fast. */
        if (C.debug_input) logf_("navigation swipe");
        swipe_start(cosf(a), -sinf(a), C.nav_steps, C.nav_stepms);
        return;
    }
    if (C.slash_keys && cmd_ready()) {
        int sector = ((int)floorf((a * 57.29578f + 15.0f) / 30.0f) % 12 + 12) % 12;
        Act k;
        ZeroMemory(&k, sizeof k);
        k.type = A_KEY; k.n = 1; k.p[0].type = A_KEY; k.p[0].vk = SLASH12[sector].vk;
        lstrcpynA(k.name, SLASH12[sector].cmd, sizeof k.name);
        if (C.debug_input) logf_("slash -> %s", SLASH12[sector].cmd);
        tap(&k);
        return;
    }
    if (C.snap > 0) {
        float step = 6.2831853f / (float)C.snap;
        a = floorf(a / step + 0.5f) * step;
    }
    swipe_start(cosf(a), -sinf(a), C.steps, C.step_ms); /* screen y grows downwards */
}

/* Stick deflection -> mouse speed (pixels per second, scaled to a 1080p screen). */
static void stick_vel(float x, float y, float speed, float *vx, float *vy)
{
    float m = sqrtf(x * x + y * y), n, sp, scale;
    if (m <= C.cur_dead) return;
    n = (m - C.cur_dead) / (1.0f - C.cur_dead);
    if (n > 1.0f) n = 1.0f;
    scale = (float)GetSystemMetrics(SM_CYSCREEN) / 1080.0f;
    if (scale < 0.5f) scale = 0.5f;
    sp = powf(n, C.cur_curve) * speed * scale;
    *vx += x / m * sp;
    *vy -= y / m * sp;
}

/* Relative mouse movement, like a real mouse: moves the cursor in menus and turns the camera
 * while exploring (the game reads mouse deltas for look); it also keeps working when the
 * cursor sits at the edge of the window. */
static void mouse_move_rel(float vx, float vy, float dt)
{
    int mx, my;
    if (vx == 0.0f && vy == 0.0f) { g_acc_x = g_acc_y = 0; return; }
    g_acc_x += vx * dt;
    g_acc_y += vy * dt;
    mx = (int)g_acc_x;
    my = (int)g_acc_y;
    if (!mx && !my) return;
    g_acc_x -= (float)mx;
    g_acc_y -= (float)my;
    {
        INPUT in;
        ZeroMemory(&in, sizeof in);
        in.type = INPUT_MOUSE;
        in.mi.dx = mx;
        in.mi.dy = my;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in, sizeof in);
    }
}

/* Trigger state with hysteresis: pressed above TriggerThreshold, released below half of it,
 * so a trigger resting near the threshold cannot flicker on and off. */
static int g_lt_on, g_rt_on;
static DWORD pad_buttons(const XPad *g)
{
    int lo = C.trig_thresh / 2;
    DWORD b = g->wButtons;
    if (g_lt_on ? g->bLeftTrigger > lo : g->bLeftTrigger > C.trig_thresh) g_lt_on = 1; else g_lt_on = 0;
    if (g_rt_on ? g->bRightTrigger > lo : g->bRightTrigger > C.trig_thresh) g_rt_on = 1; else g_rt_on = 0;
    if (g_lt_on) b |= 0x10000;
    if (g_rt_on) b |= 0x20000;
    return b;
}

static int g_layer_used, g_back_eaten;

static void handle_pad(const XState *st, DWORD b, DWORD prevb, DWORD now, float dt)
{
    const XPad *g = &st->Gamepad;
    DWORD newly, gone;
    float lx = g->sThumbLX / 32767.0f, ly = g->sThumbLY / 32767.0f;
    float rx = g->sThumbRX / 32767.0f, ry = g->sThumbRY / 32767.0f;
    int dpad_special, layer_held;

    newly = b & ~prevb;
    gone = ~b & prevb;

    if (C.debug_input && (newly & ~DPAD_MASK) && C.mode_btn >= 0 && (newly & BTN_MASK[C.mode_btn]))
        logf_("press %s -> mode toggle", BTN_NAMES[C.mode_btn]);
    if (C.mode_btn >= 0 && (newly & BTN_MASK[C.mode_btn]))
        set_mode(!g_menu, "toggle button");

    /* Both sticks clicked together: cancel their own actions and send the chord action. */
    {
        DWORD both = BTN_MASK[B_L3] | BTN_MASK[B_R3];
        if ((b & both) == both && (newly & both) && C.chord_l3r3.type != A_NONE) {
            for (int k = B_L3; k <= B_R3; k++)
                if (g_held_on[k]) { act(&g_held[k], FALSE); g_held_on[k] = 0; }
            if (C.debug_input) logf_("press L3+R3 -> %s", C.chord_l3r3.name);
            tap(&C.chord_l3r3);
            newly &= ~both;
        }
        if ((b & both) == both) newly &= ~both;
    }

    /* Back button: one step back in menus (the game's own back), its normal action otherwise. */
    /* Skills with points pending: B cancels them (leaving the screen would keep them). */
    if (C.back_btn >= 0 && (newly & BTN_MASK[C.back_btn]) && C.focus_on && g_menu_cnt > 0 && g_prof == PF_PERKS &&
        press_tag("CancelStatsUpgrade", 1)) {
        newly &= ~BTN_MASK[C.back_btn];
        g_back_eaten = 1;
    }
    if (C.back_btn >= 0 && (newly & BTN_MASK[C.back_btn]) && !g_carry) {
        char top[64];
        int cnt = game_menu(top, sizeof top);
        float bx, by;
        char btag[64];
        if (cnt > 0 && g_hwnd && find_back_arrow(&bx, &by, btag, sizeof btag)) {
            /* Press the screen's own back arrow, like tapping it, then put the cursor back. */
            POINT pt = { (LONG)bx, (LONG)by }, old;
            GetCursorPos(&old);
            ClientToScreen(g_hwnd, &pt);
            if (C.debug_input) logf_("press %s -> back arrow '%s' at %ld,%ld on %s", BTN_NAMES[C.back_btn], btag, pt.x, pt.y, top);
            mouse_to(pt.x, pt.y);
            mouse_flags(MOUSEEVENTF_LEFTDOWN, 0);
            Sleep(30);
            mouse_flags(MOUSEEVENTF_LEFTUP, 0);
            Sleep(10);
            mouse_to(old.x, old.y);
            newly &= ~BTN_MASK[C.back_btn];
            g_back_eaten = 1;
        } else if (cnt > 0) {
            Act k;
            if (C.debug_input) log_scene_buttons();
            ZeroMemory(&k, sizeof k);
            k.type = A_KEY; k.n = 1; k.p[0].type = A_KEY;
            k.p[0].vk = SLASH12[is_popup(top) ? BK_EXIT : BK_PAUSE].vk; /* ExitKeybind / PauseGameKeybind */
            lstrcpynA(k.name, is_popup(top) ? "popup close (ExitKeybind)" : "menu back (PauseGameKeybind)", sizeof k.name);
            if (C.debug_input) logf_("press %s -> %s on %s", BTN_NAMES[C.back_btn], k.name, top);
            tap(&k);
            newly &= ~BTN_MASK[C.back_btn];
            g_back_eaten = 1;
        }
    }
    if (g_back_eaten && C.back_btn >= 0 && !(b & BTN_MASK[C.back_btn])) g_back_eaten = 0;
    if (g_back_eaten) gone &= ~BTN_MASK[C.back_btn];

    /* Exploring: recentre the camera (no menu open, not in a fight). */
    if (C.recenter_btn >= 0 && (newly & BTN_MASK[C.recenter_btn]) && g_nav && game_menu(NULL, 0) == 0) {
        if (C.debug_input) logf_("press %s -> recentre camera", BTN_NAMES[C.recenter_btn]);
        recenter_camera();
        newly &= ~BTN_MASK[C.recenter_btn];
    }

    /* Menus (0.6): screens with a controller profile get their own controls; the buttons a
     * profile does not use keep their normal action, and other screens behave as in 0.5. */
    if (C.focus_on && g_menu_cnt > 0 && g_focused && g_prof != PF_NONE) {
        DWORD used = menu_buttons(newly, b, lx, ly, rx, ry, now);
        newly &= ~used;
        if (used & BTN_MASK[B_A]) gone &= ~BTN_MASK[B_A];
    }

    dpad_special = g_menu ? C.scroll_dpad : C.dpad_slash;
    layer_held = !g_menu && C.layer_btn >= 0 && (b & BTN_MASK[C.layer_btn]);

    for (int i = 0; i < NBTN; i++) {
        DWORD mask = BTN_MASK[i];
        if (i == C.mode_btn) continue;
        if (dpad_special && (mask & DPAD_MASK)) continue;
        if (!g_menu && i == C.layer_btn && C.layer_tap) {
            /* Held = magic layer for the face buttons; tapped on its own = its normal action. */
            if (newly & mask) g_layer_used = 0;
            if ((gone & mask) && !g_layer_used) {
                if (C.debug_input) logf_("tap %s -> %s", BTN_NAMES[i], C.fight[i].name);
                tap(&C.fight[i]);
            }
            continue;
        }
        if (newly & mask) {
            int use_layer = !g_menu && layer_held && i != C.layer_btn && C.layer[i].type;
            const Act *a = g_menu ? &C.menu[i] : use_layer ? &C.layer[i] : &C.fight[i];
            if (use_layer) g_layer_used = 1;
            if (C.debug_input) logf_("press %s -> %s", BTN_NAMES[i], a->name);
            if (a->type != A_NONE) {
                if (g_held_on[i]) act(&g_held[i], FALSE);
                act(a, TRUE);
                g_held[i] = *a;
                g_held_on[i] = 1;
                g_held_since[i] = g_held_rep[i] = now;
            }
        }
        if ((gone & mask) && g_held_on[i]) {
            act(&g_held[i], FALSE);
            g_held_on[i] = 0;
        }
        /* Holding a button that scrolls keeps scrolling after a short delay. */
        if (g_held_on[i] && (b & mask) && (LONG)(now - g_held_since[i]) >= 300
            && (LONG)(now - g_held_rep[i]) >= C.scroll_ms) {
            for (int k = 0; k < g_held[i].n; k++)
                if (g_held[i].p[k].type >= A_WUP) act1(&g_held[i].p[k], TRUE);
                else if (g_held[i].p[k].type == A_KEY &&
                         (g_held[i].p[k].vk == SLASH12[BK_SCROLLUP].vk || g_held[i].p[k].vk == SLASH12[BK_SCROLLDOWN].vk ||
                          g_held[i].p[k].vk == SLASH12[BK_SCROLLLEFT].vk || g_held[i].p[k].vk == SLASH12[BK_SCROLLRIGHT].vk)) {
                    key_evt(g_held[i].p[k].vk, FALSE);
                    key_evt(g_held[i].p[k].vk, TRUE);
                }
            g_held_rep[i] = now;
        }
    }

    {
        float vx = 0.0f, vy = 0.0f;
        if (g_menu || C.cursor_in_fight) stick_vel(rx, ry, C.cur_speed, &vx, &vy);
        /* Left stick also looks around (exploring); in fights the game ignores plain mouse moves. */
        if (!g_menu && C.lstick_look) stick_vel(lx, ly, C.lstick_speed, &vx, &vy);
        mouse_move_rel(vx, vy, dt);
    }

    if (g_menu) {
        if ((LONG)(now - g_last_scroll) >= C.scroll_ms) {
            int up = 0, dn = 0, lf = 0, rt = 0;
            if (C.scroll_lstick) { up = ly > 0.5f; dn = ly < -0.5f; lf = lx < -0.5f; rt = lx > 0.5f; }
            if (C.scroll_dpad) { up |= !!(b & 0x1); dn |= !!(b & 0x2); lf |= !!(b & 0x4); rt |= !!(b & 0x8); }
            if (up) wheel(120, FALSE);
            else if (dn) wheel(-120, FALSE);
            else if (lf) wheel(120, TRUE);
            else if (rt) wheel(-120, TRUE);
            if (up || dn || lf || rt) g_last_scroll = now;
        }
        g_flicked = 0;
    } else {
        float lm = sqrtf(lx * lx + ly * ly);
        if (C.focus_on && C.focus_stick && g_menu_cnt > 0 && g_prof != PF_NONE) { g_flicked = 0; return; } /* the stick drives the menu */
        static float last_ang;
        float ang = atan2f(ly, lx) * 57.29578f;
        float diff = fabsf(ang - last_ang);
        if (diff > 180.0f) diff = 360.0f - diff;
        if (lm >= C.flick_on && (!g_flicked || (C.roll_deg > 0 && diff >= (float)C.roll_deg))) {
            if (C.debug_input) logf_("flick slash x=%.2f y=%.2f%s", lx, ly, g_flicked ? " (rolled)" : "");
            slash(lx, ly);
            g_flicked = 1;
            last_ang = ang;
        } else if (g_flicked && lm < C.flick_off) {
            g_flicked = 0;
        }
        if (C.dpad_slash && (newly & DPAD_MASK)) {
            float dx = (float)(!!(b & 0x8)) - (float)(!!(b & 0x4));
            float dy = (float)(!!(b & 0x1)) - (float)(!!(b & 0x2));
            if (C.debug_input) logf_("dpad slash x=%.0f y=%.0f", dx, dy);
            slash(dx, dy);
        }
    }
}

/* The game calls ClipCursor itself (it imports it) and can drop the lock for a moment. We
 * route its calls through ours so the lock stays on the window while the game has focus. */
typedef BOOL (WINAPI *PFN_ClipCursor)(const RECT *);
static PFN_ClipCursor real_ClipCursor;
static BOOL WINAPI hook_ClipCursor(const RECT *r)
{
    RECT gr;
    if (C.confine && g_focused && game_rect(&gr)) return real_ClipCursor(&gr);
    return real_ClipCursor(r);
}
static void hook_game_clipcursor(void)
{
    UINT_PTR base = (UINT_PTR)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *d;
    if (!dir.VirtualAddress) return;
    for (d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        IMAGE_THUNK_DATA *names, *iat;
        if (lstrcmpiA((const char *)(base + d->Name), "USER32.dll")) continue;
        names = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        iat = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            IMAGE_IMPORT_BY_NAME *ibn;
            DWORD old;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (lstrcmpA((const char *)ibn->Name, "ClipCursor")) continue;
            if (VirtualProtect(&iat->u1.Function, sizeof(UINT_PTR), PAGE_READWRITE, &old)) {
                real_ClipCursor = (PFN_ClipCursor)(UINT_PTR)iat->u1.Function;
                iat->u1.Function = (UINT_PTR)hook_ClipCursor;
                VirtualProtect(&iat->u1.Function, sizeof(UINT_PTR), old, &old);
                logf_("cursor lock: game's ClipCursor calls now keep the cursor in the window");
            }
            return;
        }
    }
}

static void clip_update(void)
{
    RECT r, cur;
    if (!C.confine || !g_focused || !game_rect(&r)) return;
    if (!GetClipCursor(&cur) || memcmp(&cur, &r, sizeof r) != 0) {
        ClipCursor(&r);
        if (!g_clipped) logf_("cursor confined to game window");
        g_clipped = 1;
    }
}
static void clip_release(void)
{
    if (!g_clipped) return;
    ClipCursor(NULL);
    g_clipped = 0;
    logf_("cursor released");
}

static void spawn_cleanup_watcher(void);

static DWORD WINAPI worker(LPVOID arg)
{
    char logpath[MAX_PATH], exe[MAX_PATH];
    XState st;
    DWORD prevb = 0, last, last_scan = 0;
    int pad = -1, was_focused = 0, cursor_shown = -1;
    DWORD last_clip = 0, prevb_cur = 0;
    (void)arg;

    if (GetPrivateProfileIntA("General", "Log", 1, g_ini)) {
        snprintf(logpath, sizeof logpath, "%s\\ib3pad.log", g_dir);
        g_log = fopen(logpath, "w");
    }
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    logf_("IB3Pad %s loaded into %s", IB3PAD_VERSION, exe);
    logf_("config: %s (%s)", g_ini, GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES ? "missing, using defaults" : "found");
    load_config();
    g_menu = !C.start_fight;
    ensure_real();
    if (!R.mod || !R.GetState) {
        logf_("ERROR: could not load the system XInput DLL; controller support disabled");
        return 0;
    }
    logf_("system XInput: %s", R.path);
    spawn_cleanup_watcher();
    hook_game_clipcursor();
    logf_("game commands: called directly in the game (no keys, no key bindings); slashes: %s", C.slash_keys ? "direct" : "mouse swipes");
    logf_("camera inversion: %s (%s)", g_cam_applied ? "applied for this session" : "off",
          g_userini[0] ? g_userini : "user config not found");
    logf_("pass-through to game: %s, start mode: %s, slashes: %s, mode toggle: %s, auto mode: %d, debug input: %s",
          g_pass ? "on" : "off", g_menu ? "MENU" : "FIGHT", C.slash_keys ? "keys" : "mouse swipes",
          C.mode_btn >= 0 ? BTN_NAMES[C.mode_btn] : "none", C.auto_mode, C.debug_input ? "ON" : "off");

    timeBeginPeriod(1);
    last = timeGetTime();
    for (;;) {
        DWORD now;
        float dt;
        int focused;

        Sleep((DWORD)C.poll_ms);
        now = timeGetTime();
        dt = (float)(now - last) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;
        last = now;

        swipe_tick(now);
        tap_tick(now);
        if (pad >= 0) rumble_tick(now, (DWORD)pad);

        focused = game_focused();
        if (focused) cmd_attach(g_hwnd);
        g_focused = focused;
        if (focused != was_focused) {
            logf_(focused ? "game window focused" : "game window lost focus");
            was_focused = focused;
            if (!focused) clip_release();
        }
        if (focused && C.confine) {
            RECT gr;
            POINT cp;
            clip_update();
            if (game_rect(&gr) && GetCursorPos(&cp) &&
                (cp.x < gr.left || cp.y < gr.top || cp.x >= gr.right || cp.y >= gr.bottom)) {
                if (cp.x < gr.left) cp.x = gr.left;
                if (cp.y < gr.top) cp.y = gr.top;
                if (cp.x >= gr.right) cp.x = gr.right - 1;
                if (cp.y >= gr.bottom) cp.y = gr.bottom - 1;
                SetCursorPos(cp.x, cp.y);
            }
            (void)last_clip;
        }

        if (pad < 0) {
            if ((LONG)(now - last_scan) < 1000) continue;
            last_scan = now;
            for (int i = 0; i < 4; i++) {
                if (C.pad_index >= 0 && i != C.pad_index) continue;
                ZeroMemory(&st, sizeof st);
                if (R.GetState((DWORD)i, &st) == ERROR_SUCCESS) { pad = i; break; }
            }
            if (pad >= 0) { logf_("controller %d connected", pad); prevb = 0; }
            continue;
        }

        ZeroMemory(&st, sizeof st);
        if (R.GetState((DWORD)pad, &st) != ERROR_SUCCESS) {
            logf_("controller %d disconnected", pad);
            release_all();
            pad = -1;
            prevb = 0;
            continue;
        }

        if (!focused) {
            release_all();
            prevb = 0xFFFFFFFFu; /* ignore buttons still held when focus returns */
            continue;
        }
        {
            DWORD cur = pad_buttons(&st.Gamepad);
            if (prevb == 0xFFFFFFFFu) prevb = cur;
            prevb_cur = cur;
        }

        {
            CURSORINFO ci;
            ZeroMemory(&ci, sizeof ci);
            ci.cbSize = sizeof ci;
            if (GetCursorInfo(&ci)) {
                int shown = (ci.flags & CURSOR_SHOWING) ? 1 : 0;
                if (shown != cursor_shown) {
                    logf_("game cursor %s", shown ? "shown" : "hidden");
                    cursor_shown = shown;
                    if (C.auto_mode) set_mode(shown, "auto mode");
                }
            }
        }

        /* One-off diagnostic: is the game's controller poll on the game (window) thread? */
        {
            static DWORD t0;
            static int done;
            if (!done && g_hwnd) {
                if (!t0) { t0 = now; InterlockedExchange(&g_poll_calls, 0); }
                else if ((LONG)(now - t0) >= 10000) {
                    DWORD wt = GetWindowThreadProcessId(g_hwnd, NULL);
                    logf_("diag: game polls the controller %ld times in 10 s from thread %lu; game window thread %lu (%s)",
                          (long)g_poll_calls, (unsigned long)g_poll_tid, (unsigned long)wt,
                          g_poll_tid && g_poll_tid == wt ? "same thread" : "different thread");
                    done = 1;
                }
            }
        }

        /* Which screen is the game on? Fights get instant key slashes, everything else the swipe. */
        if (C.read_state) {
            static int last_mode = -2, started;
            int m;
            if (!started) {
                HANDLE th = CreateThread(NULL, 0, probe_thread, NULL, 0, NULL);
                if (th) CloseHandle(th);
                started = 1;
            }
            m = game_hud_mode();
            if (m != last_mode) {
                logf_("game screen: %s -> left stick %s", m < 0 ? "unknown" : (m <= HM_GESTURETEST ? HM_NAMES[m] : "?"),
                      (m >= 0 && !is_fight_mode(m)) ? "swipe" : "instant slashes");
                last_mode = m;
            }
            /* The swipe only when the game positively says it is not a fight; an unknown
             * screen (loading, HUD not found yet) gets the direct key slashes. */
            g_nav = (m >= 0 && !is_fight_mode(m));
            {
                static DWORD next_menu;
                static char last_top[64] = "?";
                if ((LONG)(now - next_menu) >= 0) {
                    char top[64];
                    int cnt = game_menu(top, sizeof top);
                    next_menu = now + 150;
                    if (cnt >= 0 && lstrcmpA(top, last_top)) {
                        logf_("menu: %s%s (%d open)", cnt ? top : "none", cnt && is_popup(top) ? " [popup]" : "", cnt);
                        lstrcpynA(last_top, top, sizeof last_top);
                    }
                }
            }
        } else {
            g_nav = 0;
        }
        if (C.read_state) focus_tick(now);
        handle_pad(&st, prevb_cur, prevb, now, dt);
        prevb = prevb_cur;
    }
    return 0;
}

/* ---------- camera inversion (applied at launch, restored at exit) ----------
 * Mouse look in this game goes through the engine's PlayerInput, whose bInvertMouse (vertical)
 * and bInvertTurn (horizontal) switches live in the user's SwordInput.ini. The engine reads that
 * file after this DLL loads, so we set the switches here, keep the original values in
 * ib3pad.state, and put them back when the game exits. A crash is recovered on the next launch. */
static const char *CAM_SEC = "Engine.PlayerInput";

static BOOL find_user_ini(void)
{
    char raw[MAX_PATH] = "", docs[MAX_PATH] = "";
    HKEY k;
    DWORD n = sizeof raw, type = 0;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
                      0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, "Personal", NULL, &type, (BYTE *)raw, &n) != ERROR_SUCCESS) raw[0] = 0;
        RegCloseKey(k);
    }
    if (raw[0]) ExpandEnvironmentStringsA(raw, docs, MAX_PATH);
    else ExpandEnvironmentStringsA("%USERPROFILE%\\Documents", docs, MAX_PATH);
    snprintf(g_userini, sizeof g_userini, "%s\\My Games\\Infinity Blade III\\SwordGame\\Config\\SwordInput.ini", docs);
    return GetFileAttributesA(g_userini) != INVALID_FILE_ATTRIBUTES;
}

static int is_true(const char *v) { return lstrcmpiA(v, "True") == 0 || lstrcmpiA(v, "1") == 0; }

static char *read_file(const char *path, DWORD *len);
static BOOL write_file(const char *path, const char *data, DWORD len);

/* Exact line edits so restoring gives back the very same bytes. A NULL or "<absent>" value
 * removes the key line; otherwise the line is replaced, or added after the section header. */
static void cam_write(const char *key, const char *val)
{
    DWORD len = 0, klen = (DWORD)strlen(key);
    char *buf = read_file(g_userini, &len), *out, *p, *line = NULL, *sec;
    char nl[96];
    int remove = !val || lstrcmpA(val, "<absent>") == 0;
    if (!buf) return;
    for (p = buf; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        if (_strnicmp(p, key, klen) == 0 && p[klen] == '=') { line = p; break; }
    snprintf(nl, sizeof nl, "%s=%s\r\n", key, remove ? "" : val);
    out = (char *)HeapAlloc(GetProcessHeap(), 0, len + sizeof nl);
    if (out) {
        DWORD o = 0;
        if (line) {
            char *end = strchr(line, '\n');
            DWORD before = (DWORD)(line - buf), after_at = end ? (DWORD)(end + 1 - buf) : len;
            memcpy(out, buf, before); o = before;
            if (!remove) { memcpy(out + o, nl, strlen(nl)); o += (DWORD)strlen(nl); }
            memcpy(out + o, buf + after_at, len - after_at); o += len - after_at;
        } else if (!remove && (sec = strstr(buf, "[Engine.PlayerInput]")) != NULL) {
            char *ins = strchr(sec, '\n');
            DWORD at = ins ? (DWORD)(ins + 1 - buf) : len;
            memcpy(out, buf, at); o = at;
            memcpy(out + o, nl, strlen(nl)); o += (DWORD)strlen(nl);
            memcpy(out + o, buf + at, len - at); o += len - at;
        } else {
            HeapFree(GetProcessHeap(), 0, out);
            HeapFree(GetProcessHeap(), 0, buf);
            return;
        }
        write_file(g_userini, out, o);
        HeapFree(GetProcessHeap(), 0, out);
    }
    HeapFree(GetProcessHeap(), 0, buf);
}

static void cam_apply(void)
{
    char om[16], ot[16];
    int ix = GetPrivateProfileIntA("Camera", "InvertX", 1, g_ini);
    int iy = GetPrivateProfileIntA("Camera", "InvertY", 1, g_ini);
    if (!find_user_ini()) return;
    if (GetPrivateProfileIntA("Camera", "Applied", 0, g_state)) {
        GetPrivateProfileStringA("Camera", "OrigInvertMouse", "<absent>", om, sizeof om, g_state);
        GetPrivateProfileStringA("Camera", "OrigInvertTurn", "<absent>", ot, sizeof ot, g_state);
    } else {
        GetPrivateProfileStringA(CAM_SEC, "bInvertMouse", "<absent>", om, sizeof om, g_userini);
        GetPrivateProfileStringA(CAM_SEC, "bInvertTurn", "<absent>", ot, sizeof ot, g_userini);
    }
    if (!ix && !iy) {
        /* Inversion switched off: make sure the user's own values are in place. */
        if (GetPrivateProfileIntA("Camera", "Applied", 0, g_state)) {
            cam_write("bInvertMouse", om);
            cam_write("bInvertTurn", ot);
            WritePrivateProfileStringA("Camera", "Applied", "0", g_state);
        }
        return;
    }
    WritePrivateProfileStringA("Camera", "OrigInvertMouse", om, g_state);
    WritePrivateProfileStringA("Camera", "OrigInvertTurn", ot, g_state);
    WritePrivateProfileStringA("Camera", "Applied", "1", g_state);
    cam_write("bInvertMouse", (is_true(om) != !!iy) ? "True" : "False");
    cam_write("bInvertTurn", (is_true(ot) != !!ix) ? "True" : "False");
    g_cam_applied = 1;
}

static void cam_restore(void)
{
    char om[16], ot[16];
    if (!g_cam_applied && !GetPrivateProfileIntA("Camera", "Applied", 0, g_state)) return;
    GetPrivateProfileStringA("Camera", "OrigInvertMouse", "<absent>", om, sizeof om, g_state);
    GetPrivateProfileStringA("Camera", "OrigInvertTurn", "<absent>", ot, sizeof ot, g_state);
    cam_write("bInvertMouse", om);
    cam_write("bInvertTurn", ot);
    WritePrivateProfileStringA("Camera", "Applied", "0", g_state);
    g_cam_applied = 0;
}

/* ---------- temporary slash bindings (added at launch, removed at exit) ---------- */
static char *read_file(const char *path, DWORD *len)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD n, got = 0;
    char *buf;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    n = GetFileSize(f, NULL);
    buf = (char *)HeapAlloc(GetProcessHeap(), 0, n + 1);
    if (buf && (!ReadFile(f, buf, n, &got, NULL) || got != n)) { HeapFree(GetProcessHeap(), 0, buf); buf = NULL; }
    CloseHandle(f);
    if (buf) { buf[n] = 0; *len = n; }
    return buf;
}
static BOOL write_file(const char *path, const char *data, DWORD len)
{
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    BOOL ok;
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(f, data, len, &put, NULL) && put == len;
    CloseHandle(f);
    return ok;
}
static void bind_line(int i, char *out, size_t n)
{
    snprintf(out, n, "Bindings=(Name=\"%s\",Command=\"%s | \")\r\n", SLASH12[i].key, SLASH12[i].cmd);
}
/* Removes every line we add (they are unique: note the trailing space in the command). */
static DWORD strip_binds(char *buf, DWORD len)
{
    char line[128];
    for (int i = 0; i < NBIND; i++) {
        size_t ll;
        char *hit;
        bind_line(i, line, sizeof line);
        ll = strlen(line);
        while ((hit = strstr(buf, line)) != NULL) {
            memmove(hit, hit + ll, len - (DWORD)(hit - buf) - ll + 1);
            len -= (DWORD)ll;
        }
    }
    return len;
}
/* 0.6 calls the game's commands directly, so it adds no key bindings. This only removes session
 * bindings an older IB3Pad version may have left behind (e.g. after a crash). */
static void binds_apply(void)
{
    DWORD len = 0, n2;
    char *buf;
    if (!g_userini[0]) return;
    buf = read_file(g_userini, &len);
    if (!buf) return;
    n2 = strip_binds(buf, len);
    if (n2 != len && write_file(g_userini, buf, n2)) WritePrivateProfileStringA("Bindings", "Applied", "0", g_state);
    HeapFree(GetProcessHeap(), 0, buf);
}
static void binds_remove(void)
{
    DWORD len = 0, n2;
    char *buf;
    if (!g_userini[0]) return;
    if (!g_binds_applied && !GetPrivateProfileIntA("Bindings", "Applied", 0, g_state)) return;
    buf = read_file(g_userini, &len);
    if (!buf) return;
    n2 = strip_binds(buf, len);
    if (n2 == len || write_file(g_userini, buf, n2))
        WritePrivateProfileStringA("Bindings", "Applied", "0", g_state);
    HeapFree(GetProcessHeap(), 0, buf);
    g_binds_applied = 0;
}

static BOOL host_is_game(void)
{
    char exe[MAX_PATH];
    const char *base;
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    base = strrchr(exe, '\\');
    base = base ? base + 1 : exe;
    return lstrcmpiA(base, "IB3.exe") == 0;
}

/* The game ends its process abruptly on quit, so DLL unload is not a reliable moment to undo
 * our session changes. A tiny watcher (rundll32 running the export below) waits for the game
 * process to end, then restores SwordInput.ini, whatever way the game exited. */
__declspec(dllexport) void CALLBACK IB3PadCleanup(HWND hwnd, HINSTANCE hi, LPSTR cmd, int show)
{
    DWORD pid = (DWORD)strtoul(cmd ? cmd : "0", NULL, 10);
    HANDLE proc = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;
    (void)hwnd; (void)hi; (void)show;
    if (proc) { WaitForSingleObject(proc, INFINITE); CloseHandle(proc); }
    Sleep(500);
    if (!g_userini[0]) find_user_ini();
    binds_remove();
    cam_restore();
}

static void spawn_cleanup_watcher(void)
{
    char dll[MAX_PATH], sysdir[MAX_PATH], cmd[3 * MAX_PATH];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    if (!g_cam_applied && !g_binds_applied) return;
    GetModuleFileNameA(g_self, dll, MAX_PATH);
    GetSystemDirectoryA(sysdir, MAX_PATH);
    snprintf(cmd, sizeof cmd, "\"%s\\rundll32.exe\" \"%s\",IB3PadCleanup %lu", sysdir, dll, GetCurrentProcessId());
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        logf_("cleanup watcher started (restores your settings when the game closes)");
    } else {
        logf_("WARNING: cleanup watcher could not start (error %lu)", GetLastError());
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_DETACH) {
        if (g_clipped) ClipCursor(NULL);
        binds_remove();
        cam_restore();
    }
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE t;
        char *slash_pos;
        g_self = h;
        DisableThreadLibraryCalls(h);
        GetModuleFileNameA(h, g_dir, MAX_PATH);
        slash_pos = strrchr(g_dir, '\\');
        if (slash_pos) *slash_pos = 0;
        snprintf(g_ini, sizeof g_ini, "%s\\ib3pad.ini", g_dir);
        snprintf(g_state, sizeof g_state, "%s\\ib3pad.state", g_dir);
        if (!host_is_game()) return TRUE; /* e.g. the cleanup watcher: paths are set, nothing else */
        cam_apply();
        if (!g_userini[0]) find_user_ini();
        binds_apply();
        t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}

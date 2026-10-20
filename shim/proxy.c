/* configit.dll shim for Kid Pix Deluxe 3 on Windows 11.

   KPD3.exe statically imports configit.dll from its own folder, so a
   replacement DLL here is loaded before any game code runs. It forwards all
   ten real exports to configit_real.dll, and patches the running EXE (its
   import table and a few call sites; KPD3.exe on disk is not touched) to work
   around the problems below. Each fix is explained where it is implemented.

   Fixes:
     one CPU            - the process is pinned to one CPU in DllMain, before
                          KPD3's startup worker thread can race the main one.
     screen metrics     - GetSystemMetrics / GetDeviceCaps report the 800x600
                          screen the engine expects to have switched to.
     waveOutGetDevCapsA - startup gate (sub_4B3570) that must succeed before
                          the window is ever shown. On this machine the call
                          fails or aborts in-process, and the gate then posts
                          WM_QUIT -> silent exit, no window. Answered locally.
     midiOutGetDevCapsA - winmm's MIDI enumeration crashes in-process
                          (winmmbase.dll+0x5AB3). Never call into it.
     ChangeDisplaySettingsA(Ex) - the engine resizes the real desktop to
                          800x600. cnc-ddraw does NOT intercept this, and the
                          engine discards the return value, so real mode
                          changes are swallowed here and probes say BADMODE.
     display mode       - the DirectDraw mode is kept at exactly 800x600.
     DirectDraw primary - KPD3 presents every frame with a GDI StretchBlt onto
                          its window DC and never creates a primary surface.
                          cnc-ddraw only scales blits it can redirect into a
                          primary; without one it draws them scaled straight
                          onto the window, where KPD3's own 800x600 clip cuts
                          them off. So create the primary through KPD3's own
                          exported helper as soon as DirectDraw is up.
     manual name        - Help > Manual opens KPD3Manual.pdf (see rename_manual:
                          this is what keeps Windows' admin-prompting Win98
                          layer from being applied).
     Win98 layer        - ToAscii, DeviceIoControl and window ghosting behave
                          the way that layer made them behave.
     menu bar           - KPD3's hidden menu bar is placed on the scaled picture
                          and pinned open.
     QuickTime          - a stand-in for the few QuickTime calls that saving and
                          opening pictures need (qtstub.inc.c).

   Logging is off unless ddraw.ini asks for it ([configit] log=1 or 2, see
   below); it then goes to configit.log next to KPD3.exe. */
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stddef.h>
#include <wincodec.h>
#include <shellapi.h>
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

/* ---- the log --------------------------------------------------------- */
/* configit.log, next to KPD3.exe, is written only when ddraw.ini asks:
       [configit]
       log=0   nothing (the default)
       log=1   startup lines and errors: what was patched, the display mode,
               the DirectDraw primary, the menu bar, crashes, R6025
       log=2   also traces: the engine's own debug output, file opens,
               windows, long waits, the first QuickTime calls
   The file starts afresh on each launch and stops growing at LOG_MAX. It is
   written with WriteFile on one append handle, not the CRT, so the crash
   logger can still use it when the game's heap is damaged. */
#define LOG_MAX (4L << 20)

static int    g_log;
static HANDLE g_logFile = INVALID_HANDLE_VALUE;
static LONG   g_logBytes;
static DWORD  g_t0;
static char   g_ddrawIni[MAX_PATH];     /* cnc-ddraw's ini, which also holds [configit] */

static void log_raw(const char *s, int n)
{
    DWORD w;
    if (g_logFile == INVALID_HANDLE_VALUE || n <= 0) return;
    if (InterlockedExchangeAdd(&g_logBytes, n) + n > LOG_MAX) return;
    WriteFile(g_logFile, s, (DWORD)n, &w, NULL);
}

static void log_line(int level, const char *api, int ok, const char *msg)
{
    char b[1200];
    int n;
    if (g_log < level) return;
    n = _snprintf(b, sizeof(b) - 2, "+%6lu  %-24s %-6s %s",
                  GetTickCount() - g_t0, api, ok ? "OK" : "FAIL", msg ? msg : "(null)");
    if (n < 0 || n > (int)sizeof(b) - 2) n = sizeof(b) - 2;
    b[n++] = '\r';
    b[n++] = '\n';
    log_raw(b, n);
}

static void log_va(int level, const char *api, int ok, const char *fmt, va_list va)
{
    char m[1024];
    if (g_log < level) return;
    _vsnprintf(m, sizeof(m), fmt, va);
    m[sizeof(m) - 1] = 0;
    log_line(level, api, ok, m);
}

/* log=1: startup lines and errors */
static void notef(const char *api, int ok, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    log_va(1, api, ok, fmt, va);
    va_end(va);
}

/* log=2: traces */
static void tracef(const char *api, int ok, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    log_va(2, api, ok, fmt, va);
    va_end(va);
}

/* DllMain: find ddraw.ini (cnc-ddraw's CNC_DDRAW_CONFIG_FILE, else the one
   next to this DLL, which is also where cnc-ddraw looks) and read log=. */
static void log_open(HMODULE self)
{
    char path[MAX_PATH], exe[MAX_PATH], *q;
    DWORD n;
    HANDLE h;
    SYSTEMTIME st;
    if (!GetModuleFileNameA(self, path, MAX_PATH) || !(q = strrchr(path, 92))) return;
    n = GetEnvironmentVariableA("CNC_DDRAW_CONFIG_FILE", g_ddrawIni, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        lstrcpynA(g_ddrawIni, path, (int)(q - path) + 2);
        strcat(g_ddrawIni, "ddraw.ini");
    }
    g_log = GetPrivateProfileIntA("configit", "log", 0, g_ddrawIni);
    if (g_log <= 0) { g_log = 0; return; }
    strcpy(q + 1, "configit.log");
    h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { g_log = 0; return; }
    CloseHandle(h);
    g_logFile = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_logFile == INVALID_HANDLE_VALUE) { g_log = 0; return; }
    GetLocalTime(&st);
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    notef("proxy", 1, "configit shim loaded %04u-%02u-%02u %02u:%02u:%02u into %s; log=%d (%s)",
          st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, exe, g_log, g_ddrawIni);
}

/* ------------------------------------------------------------------ */
typedef HANDLE  (WINAPI *PFN_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef void    (WINAPI *PFN_OutputDebugStringA)(LPCSTR);
typedef HMODULE (WINAPI *PFN_LoadLibraryA)(LPCSTR);
typedef DWORD   (WINAPI *PFN_GetVersion)(void);
typedef BOOL    (WINAPI *PFN_GetVersionExA)(OSVERSIONINFOA *);
typedef UINT    (WINAPI *PFN_midiOutGetDevCapsA)(UINT_PTR, LPMIDIOUTCAPSA, UINT);
typedef MMRESULT(WINAPI *PFN_waveOutGetDevCapsA)(UINT_PTR, LPWAVEOUTCAPSA, UINT);
typedef LONG    (WINAPI *PFN_ChangeDisplaySettingsA)(DEVMODEA *, DWORD);
typedef LONG    (WINAPI *PFN_ChangeDisplaySettingsExA)(LPCSTR, DEVMODEA *, HWND, DWORD, LPVOID);
typedef void    (WINAPI *PFN_PostQuitMessage)(int);
typedef void    (WINAPI *PFN_ExitProcess)(UINT);
typedef int     (WINAPI *PFN_MessageBoxA)(HWND, LPCSTR, LPCSTR, UINT);
typedef BOOL    (WINAPI *PFN_ShowWindow)(HWND, int);
typedef HWND    (WINAPI *PFN_CreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
typedef DWORD   (WINAPI *PFN_WaitForSingleObject)(HANDLE, DWORD);
typedef int     (WINAPI *PFN_IntersectClipRect)(HDC, int, int, int, int);
typedef int     (WINAPI *PFN_ToAscii)(UINT, UINT, CONST BYTE *, LPWORD, UINT);
typedef BOOL    (WINAPI *PFN_DeviceIoControl)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL    (WINAPI *PFN_CreateProcessA)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                             LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
typedef int     (WINAPI *PFN_GetSystemMetrics)(int);
typedef int     (WINAPI *PFN_GetDeviceCaps)(HDC, int);

static PFN_CreateFileA              o_CreateFileA;
static PFN_OutputDebugStringA       o_OutputDebugStringA;
static PFN_LoadLibraryA             o_LoadLibraryA;
static PFN_GetVersion               o_GetVersion;
static PFN_GetVersionExA            o_GetVersionExA;
static PFN_midiOutGetDevCapsA       o_midiOutGetDevCapsA;
static PFN_waveOutGetDevCapsA       o_waveOutGetDevCapsA;
static PFN_ChangeDisplaySettingsA   o_ChangeDisplaySettingsA;
static PFN_ChangeDisplaySettingsExA o_ChangeDisplaySettingsExA;
static PFN_PostQuitMessage          o_PostQuitMessage;
static PFN_ExitProcess              o_ExitProcess;
static PFN_MessageBoxA              o_MessageBoxA;
static PFN_ShowWindow               o_ShowWindow;
static PFN_CreateWindowExA          o_CreateWindowExA;
static PFN_WaitForSingleObject      o_WaitForSingleObject;
static PFN_IntersectClipRect        o_IntersectClipRect;
static PFN_ToAscii                  o_ToAscii;
static PFN_DeviceIoControl          o_DeviceIoControl;
static PFN_CreateProcessA           o_CreateProcessA;
static PFN_GetSystemMetrics         o_GetSystemMetrics;

/* ---- the screen the engine expects ---------------------------------- */
/* The engine was built to switch the display to 800x600 and then cover the
   whole screen with its window. It sizes that window from the screen metrics
   and centres its 800x600 canvas in it. On a modern desktop that puts the
   canvas in the middle of a huge window, clipped. So report the screen it
   would have had after its own mode switch. These calls happen before
   DirectDraw loads, which is why cnc-ddraw cannot fake them itself. */
#define FAKE_SCREEN_W 800
#define FAKE_SCREEN_H 600

static PFN_GetDeviceCaps o_GetDeviceCaps;
static BOOL g_smSeen[100];

static int WINAPI my_GetSystemMetrics(int i)
{
    int v = o_GetSystemMetrics(i), f = v;
    switch (i) {
    case SM_CXSCREEN: case SM_CXFULLSCREEN: case SM_CXVIRTUALSCREEN: f = FAKE_SCREEN_W; break;
    case SM_CYSCREEN: case SM_CYFULLSCREEN: case SM_CYVIRTUALSCREEN: f = FAKE_SCREEN_H; break;
    case SM_XVIRTUALSCREEN: case SM_YVIRTUALSCREEN: f = 0; break;
    }
    if (i >= 0 && i < 100 && !g_smSeen[i]) {      /* log each index once */
        g_smSeen[i] = TRUE;
        if (f != v) tracef("GetSystemMetrics", 1, "index %d: real %d -> reporting %d (caller %p)", i, v, f, _ReturnAddress());
        else        tracef("GetSystemMetrics", 1, "index %d -> %d (caller %p)", i, v, _ReturnAddress());
    }
    return f;
}

static int g_dcLogged;
static int WINAPI my_GetDeviceCaps(HDC dc, int idx)
{
    int v = o_GetDeviceCaps(dc, idx);
    /* only the display - the game also prints, and printer DCs must stay real */
    if ((idx == HORZRES || idx == VERTRES) && o_GetDeviceCaps(dc, TECHNOLOGY) == DT_RASDISPLAY) {
        int f = (idx == HORZRES) ? FAKE_SCREEN_W : FAKE_SCREEN_H;
        if (g_dcLogged++ < 4)
            tracef("GetDeviceCaps", 1, "%s: real %d -> reporting %d (caller %p)",
                  idx == HORZRES ? "HORZRES" : "VERTRES", v, f, _ReturnAddress());
        return f;
    }
    return v;
}

/* ---- DirectDraw primary -------------------------------------------- */
/* KPD3 draws each frame with StretchBlt onto the DC of its window, and clips
   that DC to (0,0,800,600) itself. cnc-ddraw hooks StretchBlt: with a primary
   surface it redirects the blit into that 800x600 surface and its renderer
   scales the whole surface to the window; with no primary it can only draw
   the blit scaled onto the window DC, where KPD3's clip cuts it off. KPD3
   never creates a primary on this path, so create it here through KPD3's own
   exported helper (ddrawEXPORTS[1] = sub_4FE376 -> sub_4FE968, CreateSurface
   PRIMARY, AddRef'd). That stores the surface in KPD3's own global, so its
   other DirectDraw code reuses the same primary. The reference is kept.

   Triggers: IntersectClipRect runs every time KPD3 takes its window DC to
   present, and CreateFileA is a backup. cnc-ddraw does not hook either, so
   they keep working after DDRAW.DLL loads (it re-patches the slots it hooks,
   by name, which is why nothing here touches StretchBlt or GetDC). */
#define KPD3_DD_STARTED  (*(volatile WORD  *)0x00598258)   /* sub_4FE6BC entered */
#define KPD3_DD_OBJECT   (*(volatile DWORD *)0x005980D0)   /* IDirectDraw2, set once ready */
#define KPD3_DD_PRIMARY  (*(void * volatile *)0x005980BC)
#define KPD3_DD_ERROR    (*(volatile WORD  *)0x00597F40)

static DWORD g_mainTid;
static void  menu_tick(void);
static int   g_primTried;
static void *g_primary;

static void ensure_primary(const char *why)
{
    FARPROC *t;
    if (g_primTried || GetCurrentThreadId() != g_mainTid || !GetModuleHandleA("ddraw.dll")) return;
    if (!KPD3_DD_STARTED || !KPD3_DD_OBJECT) return;
    g_primTried = 1;
    t = (FARPROC *)GetProcAddress(GetModuleHandleA(NULL), "ddrawEXPORTS");
    g_primary = t ? ((void *(WINAPI *)(void))t[1])() : NULL;
    notef("primary", g_primary != NULL, "created via %s -> %p (KPD3 global %p, err %u)",
          why, g_primary, KPD3_DD_PRIMARY, KPD3_DD_ERROR);
}

/* ---- display mode ---------------------------------------------------- */
/* KPD3 picks a mode in sub_4FA863 (smallest listed mode >= its request) and,
   for a DirectDraw mode, sets it with sub_4FEDC0. Both calls in sub_4FA3CD
   are redirected through here at run time; KPD3.exe on disk is unchanged. */
typedef short (__cdecl *PFN_pick)(unsigned short *, char *);
typedef short (__cdecl *PFN_setmode)(int, int, int, int);

static short __cdecl pick_mode(unsigned short *req, char *out)
{
    short r;
    notef("mode", 1, "request: %ux%u colors=%lu pal=%u bpp=%u refresh=%lu",
          req[0], req[1], *(DWORD *)(req + 2), req[4], req[5], *(DWORD *)(req + 6));
    r = ((PFN_pick)0x004FA863)(req, out);
    notef("mode", r > 0, "picked (%d): '%.32s' %lux%lu %lubpp %luHz",
          r, out, *(DWORD *)(out + 0x6C), *(DWORD *)(out + 0x70), *(DWORD *)(out + 0x68), *(DWORD *)(out + 0x78));
    /* KPD3 takes the smallest offered mode that is at least as big as its
       800x600 request. cnc-ddraw's list can leave out 800x600 (it did in
       testing on a 2560x1600 screen), and KPD3 then asks for 1024x768:
       cnc-ddraw centres the unscaled 800x600 canvas inside a 1024x768
       "screen", and a primary made at 800x600 no longer matches the mode, so
       its renderer shows black. cnc-ddraw accepts any mode, so keep the
       DirectDraw mode at exactly the size requested. */
    if (r > 0 && !_stricmp(out, "DDRAW") && req[0] != 0xFFFF && req[1] != 0xFFFF &&
        (*(DWORD *)(out + 0x6C) != req[0] || *(DWORD *)(out + 0x70) != req[1])) {
        *(DWORD *)(out + 0x6C) = req[0];
        *(DWORD *)(out + 0x70) = req[1];
        notef("mode", 1, "using the requested %ux%u instead", req[0], req[1]);
    }
    return r;
}

static short __cdecl log_setmode(int w, int h, int bpp, int hz)
{
    short r = ((PFN_setmode)0x004FEDC0)(w, h, bpp, hz);
    notef("mode", r == 0, "DirectDraw SetDisplayMode(%d, %d, %d, %d) -> %d", w, h, bpp, hz, r);
    return r;
}

static void redirect_call(DWORD at, void *to, DWORD expect)
{
    BYTE *p = (BYTE *)at; DWORD old, cur;
    if (p[0] != 0xE8) { notef("proxy", 0, "no CALL at %08lX", at); return; }
    cur = at + 5 + *(DWORD *)(p + 1);
    if (cur != expect) { notef("proxy", 0, "CALL at %08lX goes to %08lX, not %08lX", at, cur, expect); return; }
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    *(DWORD *)(p + 1) = (DWORD)to - (at + 5);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
}

/* ---- the manual's file name ----------------------------------------- */
/* Windows' compatibility database recognises Kid Pix Deluxe 3 by
   KdpxManual.pdf next to KPD3.exe and then applies its Windows 98 layer,
   whose AdditiveRunAsHighest flag makes every launch ask for administrator
   rights. The manual is renamed to KPD3Manual.pdf (same length) so that entry
   no longer matches; Help > Manual is pointed at the new name here. If the
   original name is put back, nothing is patched and Windows' layer returns. */
#define KPD3_MANUAL_NAME ((char *)0x00581710)   /* used only by sub_436280 */

static void rename_manual(void)
{
    char path[MAX_PATH], *q;
    DWORD old;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    q = strrchr(path, 92);
    if (!q) return;
    strcpy(q + 1, "KPD3Manual.pdf");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return;
    if (memcmp(KPD3_MANUAL_NAME, "KdpxManual.pdf", 15)) { notef("proxy", 0, "manual name not where expected"); return; }
    VirtualProtect(KPD3_MANUAL_NAME, 15, PAGE_READWRITE, &old);
    memcpy(KPD3_MANUAL_NAME, "KPD3Manual.pdf", 15);
    VirtualProtect(KPD3_MANUAL_NAME, 15, old, &old);
    notef("proxy", 1, "Help > Manual points at KPD3Manual.pdf");
}

/* ---- crash records ---------------------------------------------------- */
/* Without Windows' Win98 compatibility layer, and before the fixes here took
   its place, the game died with "R6025 pure virtual function call". __purecall
   (0x52BFE0) is redirected here to log the object and the KPD3 call chain
   before the CRT's own handling (the R6025 message and exit) continues
   exactly as before. Logged at log=1 and up. */
static void __cdecl log_purecall(DWORD ret, DWORD *obj, DWORD *esp)
{
    char line[700];
    int n, j;
    DWORD vt = 0, f[6] = {0};
    if (!g_log) return;
    __try { vt = obj ? obj[0] : 0; for (j = 0; j < 6 && obj; j++) f[j] = obj[j]; } __except (1) { }
    notef("R6025", 0, "pure virtual call from %08lX, this=%p vtable=%08lX obj=[%08lX %08lX %08lX %08lX %08lX %08lX]",
          ret, obj, vt, f[0], f[1], f[2], f[3], f[4], f[5]);
    n = _snprintf(line, sizeof(line), "KPD3 stack:");
    __try {
        for (j = 0; j < 512 && n < (int)sizeof(line) - 12; j++)
            if (esp[j] >= 0x00401000 && esp[j] < 0x00552000)
                n += _snprintf(line + n, sizeof(line) - n, " %08lX", esp[j]);
    } __except (1) { }
    line[sizeof(line) - 1] = 0;
    log_line(1, "R6025", 0, line);
}

__declspec(naked) static void my_purecall(void)
{
    __asm {
        mov  eax, esp
        push eax                 /* esp at entry */
        push ecx                 /* this (thiscall) */
        push dword ptr [eax]     /* return address = the virtual call site */
        call log_purecall
        add  esp, 12
        push 19h                 /* what __purecall itself does */
        mov  eax, 0x005303E0     /* __amsg_exit */
        call eax
        add  esp, 4
        ret
    }
}

/* Access violations and other fatal exceptions (first chance, the first four),
   with the faulting module and the KPD3 call chain, so a crash can be located.
   It only logs and always passes the exception on; DllMain registers it only
   when logging is on. */
static LONG g_nAV;
static void mod_of(DWORD a, char *out, int cb)
{
    HMODULE m = NULL;
    out[0] = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)a, &m) && m) {
        char path[MAX_PATH], *q;
        GetModuleFileNameA(m, path, MAX_PATH);
        q = strrchr(path, 92);
        _snprintf(out, cb, "%s+%lX", q ? q + 1 : path, a - (DWORD)m);
        out[cb - 1] = 0;
    } else _snprintf(out, cb, "?");
}

static LONG WINAPI av_logger(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    char where[160], line[700];
    int n, j;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && er->ExceptionCode != 0xC0000409 &&
        er->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION && er->ExceptionCode != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_nAV) > 4) return EXCEPTION_CONTINUE_SEARCH;
    mod_of((DWORD)er->ExceptionAddress, where, sizeof(where));
    notef("CRASH", 0, "code %08lX at %p (%s) %s %08lX | eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX tid=%lu",
          er->ExceptionCode, er->ExceptionAddress, where,
          er->NumberParameters > 1 ? (er->ExceptionInformation[0] ? "writing" : "reading") : "",
          er->NumberParameters > 1 ? (DWORD)er->ExceptionInformation[1] : 0,
          c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, GetCurrentThreadId());
    n = _snprintf(line, sizeof(line), "KPD3 stack:");
    __try {
        DWORD *sp = (DWORD *)c->Esp;
        DWORD *top = (DWORD *)((NT_TIB *)NtCurrentTeb())->StackBase;
        for (j = 0; j < 1024 && sp + j < top && n < (int)sizeof(line) - 12; j++)
            if (sp[j] >= 0x00401000 && sp[j] < 0x00552000)
                n += _snprintf(line + n, sizeof(line) - n, " %08lX", sp[j]);
    } __except (1) { }
    line[sizeof(line) - 1] = 0;
    log_line(1, "CRASH", 0, line);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void trap_purecall(void)
{
    BYTE *p = (BYTE *)0x0052BFE0; DWORD old;
    if (p[0] != 0x55 || p[1] != 0x8B || p[2] != 0xEC || p[3] != 0x6A || p[4] != 0x19) {
        notef("proxy", 0, "__purecall not where expected"); return;
    }
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9; *(DWORD *)(p + 1) = (DWORD)my_purecall - (0x0052BFE0 + 5);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
}

/* KPD3 calls this every time it takes its window DC to present a frame. */
static int WINAPI my_IntersectClipRect(HDC dc, int l, int t, int r, int b)
{
    ensure_primary("IntersectClipRect");
    menu_tick();
    return o_IntersectClipRect(dc, l, t, r, b);
}

/* ---- the startup audio gate -------------------------------------- */
/* sub_4B3570 loops on waveOutGetDevCapsA(WAVE_MAPPER) and posts WM_QUIT if
   the user gives up; ShowWindow is only reached after it returns. In this
   process the real call does not just fail - it can abort() the whole
   process from inside winmm (WER: ucrtbase.dll 0xC0000409, fast-fail code 7),
   and a fast-fail cannot be caught. So the real function is never called:
   the game only uses the answer as a yes/no gate, and its sound actually
   plays through DirectSound, which initialises fine. */
static int          g_waveLogged;
static WAVEOUTCAPSA g_waveCaps;

static void synth_wave_caps(WAVEOUTCAPSA *c)
{
    memset(c, 0, sizeof(*c));
    c->wMid = 1;                  /* MM_MICROSOFT */
    c->wPid = 2;                  /* MM_WAVE_MAPPER */
    c->vDriverVersion = 0x0500;
    lstrcpynA(c->szPname, "Microsoft Sound Mapper", sizeof(c->szPname));
    c->dwFormats = 0x000FFFFF;    /* every WAVE_FORMAT_xMxx flag */
    c->wChannels = 2;
    c->dwSupport = 0x0000002E;    /* what the real mapper reports in a clean process */
}

static MMRESULT WINAPI my_waveOutGetDevCapsA(UINT_PTR id, LPWAVEOUTCAPSA caps, UINT cb)
{
    if (!g_waveLogged) {
        g_waveLogged = 1;
        synth_wave_caps(&g_waveCaps);
        notef("waveOutGetDevCapsA", 1, "id=%d -> synthesized mapper caps, winmm not called (caller %p)",
              (int)id, _ReturnAddress());
    }
    if (caps && cb) {
        memset(caps, 0, cb);
        memcpy(caps, &g_waveCaps, cb < sizeof(g_waveCaps) ? cb : sizeof(g_waveCaps));
    }
    return MMSYSERR_NOERROR;
}

/* ---- MIDI: never call into winmm's MIDI enumeration --------------- */
/* Its only caller in the startup path just logs the device name. KPD3.exe
   also has its wrapper (sub_4E04E2) stubbed; this covers any other caller. */
static UINT WINAPI my_midiOutGetDevCapsA(UINT_PTR id, LPMIDIOUTCAPSA caps, UINT cb)
{
    tracef("midiOutGetDevCapsA", 1, "id=%d -> empty caps, winmm not called (caller %p)",
          (int)id, _ReturnAddress());
    if (caps && cb) memset(caps, 0, cb);
    return MMSYSERR_NOERROR;
}

/* ---- display mode ------------------------------------------------- */
/* Never forwarded - not even CDS_TEST probes. Windows watches for apps asking
   for 8/16-bit modes and silently re-registers its DWM8And16BitMitigation
   compatibility shim for them, which loads the shim engine (AcLayers) into
   every later launch. The engine ignores the return value anyway. */
static int g_cdsTests;

static LONG WINAPI my_ChangeDisplaySettingsA(DEVMODEA *dm, DWORD flags)
{
    /* Answer every probe "bad mode". The engine merges Windows' modes with
       DirectDraw's own list (tagged "ddraw") and prefers a Windows mode if one
       passes CDS_TEST - which then takes the ChangeDisplaySettings path and
       draws in raw window coordinates. With no Windows modes it picks the
       DirectDraw mode instead, i.e. its original fullscreen-DirectDraw design,
       which cnc-ddraw virtualises and scales. (Windows 11 has no 16-bit modes
       anyway, so "bad mode" is the truthful answer for what it asks for.) */
    if (flags & CDS_TEST) {
        if (g_cdsTests++ == 0)
            notef("ChangeDisplaySettingsA", 1, "CDS_TEST probes answered BADMODE locally (caller %p)", _ReturnAddress());
        return DISP_CHANGE_BADMODE;
    }
    if (dm && (dm->dmFields & (DM_PELSWIDTH | DM_PELSHEIGHT)))
        notef("ChangeDisplaySettingsA", 1, "BLOCKED real change to %lux%lu %lubpp (caller %p)",
              dm->dmPelsWidth, dm->dmPelsHeight, dm->dmBitsPerPel, _ReturnAddress());
    else
        notef("ChangeDisplaySettingsA", 1, "BLOCKED restore-to-registry (caller %p)", _ReturnAddress());
    return DISP_CHANGE_SUCCESSFUL;
}

static LONG WINAPI my_ChangeDisplaySettingsExA(LPCSTR dev, DEVMODEA *dm, HWND wnd,
                                               DWORD flags, LPVOID p)
{
    notef("ChangeDisplaySettingsExA", 1, "BLOCKED flags=%08lX (caller %p)", flags, _ReturnAddress());
    return DISP_CHANGE_SUCCESSFUL;
}

/* ---- pass-through hooks that only log ------------------------------- */
/* Kept from the investigation as a ready-made trace (log=2) of how the game
   starts and quits. ExitProcess and MessageBoxA (the game's error dialogs)
   are logged at log=1. */
static void WINAPI my_PostQuitMessage(int code)
{
    tracef("PostQuitMessage", 1, "code=%d  caller=%p", code, _ReturnAddress());
    o_PostQuitMessage(code);
}

static void WINAPI my_ExitProcess(UINT code)
{
    notef("ExitProcess", 1, "code=%u  caller=%p", code, _ReturnAddress());
    o_ExitProcess(code);
}

static int WINAPI my_MessageBoxA(HWND h, LPCSTR text, LPCSTR cap, UINT type)
{
    int r;
    notef("MessageBoxA", 1, "caption='%s' text='%.300s' (caller %p)",
          cap ? cap : "", text ? text : "", _ReturnAddress());
    r = o_MessageBoxA(h, text, cap, type);
    notef("MessageBoxA", 1, "-> user answered %d", r);
    return r;
}

static int g_nShow, g_nCreate, g_nWait;

static BOOL WINAPI my_ShowWindow(HWND h, int cmd)
{
    if (g_nShow++ < 40) {
        char cls[96] = "";
        GetClassNameA(h, cls, sizeof(cls));
        tracef("ShowWindow", 1, "hwnd=%p class='%s' cmd=%d (caller %p)", h, cls, cmd, _ReturnAddress());
    }
    return o_ShowWindow(h, cmd);
}

static HWND WINAPI my_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style,
                                      int x, int y, int w, int hgt, HWND parent,
                                      HMENU menu, HINSTANCE inst, LPVOID param)
{
    HWND r = o_CreateWindowExA(ex, cls, name, style, x, y, w, hgt, parent, menu, inst, param);
    if (g_nCreate++ < 40) {
        char c[96];
        if (cls && HIWORD((ULONG_PTR)cls)) lstrcpynA(c, cls, sizeof(c));
        else _snprintf(c, sizeof(c), "#atom%u", (unsigned)LOWORD((ULONG_PTR)cls));
        c[sizeof(c) - 1] = 0;
        tracef("CreateWindowExA", r != NULL, "class='%s' title='%s' %dx%d style=%08lX -> %p",
              c, name ? name : "", w, hgt, style, r);
    }
    return r;
}

/* Any wait of half a second or more, with who asked. */
static DWORD WINAPI my_WaitForSingleObject(HANDLE h, DWORD ms)
{
    DWORD t = GetTickCount();
    DWORD r = o_WaitForSingleObject(h, ms);
    DWORD dt = GetTickCount() - t;
    if (dt >= 500 && g_nWait++ < 40)
        tracef("WaitForSingleObject", r == WAIT_OBJECT_0, "waited %lums (timeout %ld) -> %lu (caller %p)",
              dt, (long)ms, r, _ReturnAddress());
    return r;
}

/* ---- what Windows' Win98 layer used to cover ------------------------- */
/* The layer is no longer applied (the manual is renamed, see rename_manual),
   and of its ~70 fixes the game needs these two besides one-CPU affinity. */

/* EmulateUSER: on Win9x ToAscii cleared the character it did not produce.
   NT leaves the buffer alone when it returns 0, and KPD3's key translator
   (sub_50C98B) keeps that buffer in an uninitialised stack slot, so arrows,
   Delete, Home/End and the F-keys could insert a junk character in the text
   tool instead of editing. */
static int WINAPI my_ToAscii(UINT vk, UINT sc, CONST BYTE *ks, LPWORD ch, UINT fl)
{
    if (ch) *ch = 0;
    return o_ToAscii(vk, sc, ks, ch, fl);
}

/* Startup drive scan (sub_4F5D1C): for removable drives it opens the volume
   and loops over the result of a disk query without checking that either
   worked. A failed DeviceIoControl leaves "bytes returned" untouched, so the
   loop would walk stack garbage. Report 0 bytes on failure. */
static BOOL WINAPI my_DeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD cbIn, LPVOID out, DWORD cbOut,
                                      LPDWORD ret, LPOVERLAPPED ov)
{
    BOOL r = o_DeviceIoControl(h, code, in, cbIn, out, cbOut, ret, ov);
    if (!r && ret && !ov) *ret = 0;
    return r;
}

/* Help > View User Guide (sub_436280). With Acrobat's old AcroExch.Document
   key registered, the game starts Acrobat on the manual itself. Without it
   (any modern PC) it runs "<game folder>\kprun.exe Acro" - unquoted, and
   kprun.exe, Broderbund's helper that offered to install Acrobat 4, is not
   shipped - so the menu item did nothing. Open the manual in whatever PDF
   viewer Windows has instead. The game ignores the result either way. */
static BOOL WINAPI my_CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
                                     BOOL inherit, DWORD flags, LPVOID env, LPCSTR dir,
                                     LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi)
{
    if (!app && cmd) {
        const char *k = strstr(cmd, "kprun.exe"), *e = cmd + strlen(cmd);
        while (e > cmd && e[-1] == ' ') e--;
        if (k && e - cmd >= 4 && !_strnicmp(e - 4, "Acro", 4)) {
            char path[MAX_PATH], *q;
            HINSTANCE r = 0;
            GetModuleFileNameA(NULL, path, MAX_PATH);
            if ((q = strrchr(path, 92)) != NULL) {
                strcpy(q + 1, "KPD3Manual.pdf");
                if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) strcpy(q + 1, "KdpxManual.pdf");
                r = ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
            }
            notef("manual", (INT_PTR)r > 32, "Help > View User Guide: opened %s -> %d", path, (int)(INT_PTR)r);
            SetLastError(ERROR_FILE_NOT_FOUND);
            return FALSE;
        }
    }
    return o_CreateProcessA(app, cmd, pa, ta, inherit, flags, env, dir, si, pi);
}

/* Fonts. At startup the game registers every .TTF/.FON in its own folder
   (sub_4FB280, AddFontResourceA) and then announces it with
   SendMessageA(HWND_BROADCAST, WM_FONTCHANGE) (0x4FB036), and does the same
   when it removes them at exit (0x4FB503). A broadcast SendMessage waits for
   every top-level window on the desktop to answer, and on an ordinary PC some
   never do (a hung program, or a suspended Store app such as Settings), so
   the game froze before showing its window. Post the notice instead: the
   fonts are already usable, and other programs learn of them just the same. */
#define KPD3_FONTCAST_START  0x004FB036
#define KPD3_FONTCAST_EXIT   0x004FB503
#define KPD3_IAT_SendMessageA 0x005526E0

static LRESULT WINAPI my_FontChangeNotify(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    PostMessageA(h, msg, w, l);
    return 0;
}

/* ---- version lie --------------------------------------------------- */
/* Report Windows XP SP3. Kept as a precaution for any OS-version checks; it
   is not proven necessary for anything specific. */
static DWORD WINAPI my_GetVersion(void) { return 0x0A280105; }   /* 5.1 build 2600 */

static BOOL WINAPI my_GetVersionExA(OSVERSIONINFOA *vi)
{
    BOOL r = o_GetVersionExA(vi);
    if (r && vi) {
        vi->dwMajorVersion = 5;
        vi->dwMinorVersion = 1;
        vi->dwBuildNumber  = 2600;
        vi->dwPlatformId   = VER_PLATFORM_WIN32_NT;
        lstrcpynA(vi->szCSDVersion, "Service Pack 3", sizeof(vi->szCSDVersion));
    }
    return r;
}

static HMODULE WINAPI my_LoadLibraryA(LPCSTR n)
{
    HMODULE h = o_LoadLibraryA(n);
    log_line(2, "LoadLibraryA", h != NULL, n);
    return h;
}

/* The engine traces everything through OutputDebugStringA (log=2). */
static void WINAPI my_OutputDebugStringA(LPCSTR s)
{
    if (g_log >= 2 && s) {
        char m[1024];
        int n;
        lstrcpynA(m, s, sizeof(m));
        n = (int)strlen(m);
        while (n && (m[n - 1] == '\n' || m[n - 1] == '\r')) m[--n] = 0;
        log_line(2, "TRACE", 1, m);
    }
    if (o_OutputDebugStringA) o_OutputDebugStringA(s);
}

/* Also a trigger for ensure_primary, as a backup to IntersectClipRect. */
static HANDLE WINAPI my_CreateFileA(LPCSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa,
                                    DWORD c, DWORD fl, HANDLE t)
{
    HANDLE h;
    ensure_primary("CreateFileA");
    h = o_CreateFileA(n, a, s, sa, c, fl, t);
    log_line(2, "CreateFileA", h != INVALID_HANDLE_VALUE, n);
    return h;
}

/* ---- the menu bar --------------------------------------------------- */
/* KPD3's menu bar lives on its own owned popup, class "menuWndProc"
   (created in sub_508381, HMENU attached by sub_5092C3 = SetMenu +
   EnableWindow). The main window has no menu. By design the bar is hidden
   (Read Me: "move your cursor to the top of the screen... click"). It opens
   on a click at game y == 0 (sub_508DE2 @0x5091AF) or on Alt/F10
   (@0x509260), and WM_EXITMENULOOP (sub_508B6D @0x508C3C) hides it again
   unless the engine's pinned flag obj+0x80 is set. KPD3 never sets that flag
   (its setter, vfunc 0xF8 = sub_5092F4, has no callers). The top 17 game
   rows it overlays are black. Under cnc-ddraw:
     - Alt/F10: fake_WndProc returns 0 for SC_KEYMENU because the main
       window has no menu (wndproc.c:626), so the game never sees it.
     - click: GetWindowRect(popup) @0x509202 goes through cnc's fake, which
       returns the rect relative to the main client. SC_MOUSEMENU then gets
       a point near the desktop corner, so the menu loop ends at once and the
       popup hides again.
     - placement: WM_USER moves the popup to the client origin at
       800 x SM_CYMENU. It is never fitted to the scaled picture.
   Fix: subclass the popup (with the real SetWindowLongA; cnc-ddraw only
   intercepts the main window) so it always sits on the top edge of
   cnc-ddraw's viewport, as wide as the viewport and as tall as its own bar.
   Pin it with the engine's own vfunc 0xF8 ([configit] menubar=pinned, the
   default). Give the click path true screen coordinates, and pass plain
   Alt/F10 past cnc-ddraw to the game.
   [configit] menubar=auto restores the original hidden bar; off disables
   all of this. The work starts on KPD3's first presents after the
   DirectDraw primary exists (menu_tick from my_IntersectClipRect). By then
   cnc-ddraw has subclassed the main window and sized it. */
#define KPD3_W                   800
#define KPD3_H                   600
#define KPD3_IMAGE_LO            0x00401000
#define KPD3_IMAGE_HI            0x00552000
#define KPD3_PIN_FN              0x005092F4   /* vfunc 0xF8: obj+0x80 = on; shows (SW_SHOWNA) / hides */
#define KPD3_PINNED(obj)         (*(volatile WORD *)((BYTE *)(obj) + 0x80))
#define KPD3_IAT_GetWindowLongA  0x0055251C
#define KPD3_IAT_GetWindowRect   0x005526F0
#define KPD3_CLICK_GETRECT       0x00509202   /* CALL [GetWindowRect] before SC_MOUSEMENU */
#ifndef WM_DPICHANGED
#define WM_DPICHANGED            0x02E0
#endif
#define DPI_CTX_PMV2             ((HANDLE)(LONG_PTR)-4)   /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */

enum { MENU_OFF, MENU_AUTO, MENU_PINNED };

typedef BOOL    (WINAPI *PFN_GetRect)(HWND, LPRECT);
typedef BOOL    (WINAPI *PFN_ClientToScreen)(HWND, LPPOINT);
typedef BOOL    (WINAPI *PFN_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
typedef LONG    (WINAPI *PFN_SetWindowLongA)(HWND, int, LONG);
typedef LONG    (WINAPI *PFN_GetWindowLongA)(HWND, int);
typedef LRESULT (WINAPI *PFN_DefWindowProcA)(HWND, UINT, WPARAM, LPARAM);
typedef HANDLE  (WINAPI *PFN_SetThreadDpiCtx)(HANDLE);
typedef UINT    (WINAPI *PFN_GetDpiForWindow)(HWND);
typedef HANDLE  (WINAPI *PFN_GetWindowDpiCtx)(HWND);
typedef int     (WINAPI *PFN_AwarenessOfCtx)(HANDLE);
typedef void    (__fastcall *PFN_pin)(void *self, void *edx_unused, int on);   /* = thiscall, retn 4 */

/* Real user32 entry points, taken in DllMain before DDRAW.DLL exists. With
   hook=4, cnc-ddraw also re-patches configit.dll's own imports for every
   name on its list. */
static PFN_GetRect         r_GetWindowRect, r_GetClientRect;
static PFN_ClientToScreen  r_ClientToScreen;
static PFN_SetWindowPos    r_SetWindowPos;
static PFN_SetWindowLongA  r_SetWindowLongA;
static PFN_GetWindowLongA  r_GetWindowLongA;
static PFN_DefWindowProcA  r_DefWindowProcA;
static PFN_ShowWindow      r_ShowWindow;
static PFN_SetThreadDpiCtx p_SetThreadDpiAwarenessContext;
static PFN_GetDpiForWindow p_GetDpiForWindow;
static PFN_GetWindowDpiCtx p_GetWindowDpiAwarenessContext;
static PFN_AwarenessOfCtx  p_GetAwarenessFromDpiAwarenessContext;

static int     g_menuInit, g_menuMode = MENU_OFF;
static HWND    g_menuWnd, g_mainWnd;
static WNDPROC g_menuPrev, g_mainPrev, g_gameProc;
static UINT    g_menuSyncMsg;
static int     g_placing, g_nPlace, g_nKey, g_nBack, g_badGame;
static int     g_boxing, g_maintas, g_arX = KPD3_W, g_arY = KPD3_H;

/* Like redirect_call, but for a 6-byte CALL [IAT slot]: it becomes CALL rel32 + NOP. */
static int redirect_icall(DWORD at, void *to, DWORD slot)
{
    BYTE *p = (BYTE *)at; DWORD old;
    if (p[0] != 0xFF || p[1] != 0x15 || *(DWORD *)(p + 2) != slot) {
        notef("proxy", 0, "no CALL [%08lX] at %08lX", slot, at);
        return 0;
    }
    VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE8;
    *(DWORD *)(p + 1) = (DWORD)to - (at + 5);
    p[5] = 0x90;
    VirtualProtect(p, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    return 1;
}

static void menu_resolve(void)          /* DllMain */
{
    HMODULE u = GetModuleHandleA("user32.dll");
    if (!u) return;
    r_GetWindowRect   = (PFN_GetRect)GetProcAddress(u, "GetWindowRect");
    r_GetClientRect   = (PFN_GetRect)GetProcAddress(u, "GetClientRect");
    r_ClientToScreen  = (PFN_ClientToScreen)GetProcAddress(u, "ClientToScreen");
    r_SetWindowPos    = (PFN_SetWindowPos)GetProcAddress(u, "SetWindowPos");
    r_SetWindowLongA  = (PFN_SetWindowLongA)GetProcAddress(u, "SetWindowLongA");
    r_GetWindowLongA  = (PFN_GetWindowLongA)GetProcAddress(u, "GetWindowLongA");
    r_DefWindowProcA  = (PFN_DefWindowProcA)GetProcAddress(u, "DefWindowProcA");
    r_ShowWindow      = (PFN_ShowWindow)GetProcAddress(u, "ShowWindow");
    p_SetThreadDpiAwarenessContext        = (PFN_SetThreadDpiCtx)GetProcAddress(u, "SetThreadDpiAwarenessContext");
    p_GetDpiForWindow                     = (PFN_GetDpiForWindow)GetProcAddress(u, "GetDpiForWindow");
    p_GetWindowDpiAwarenessContext        = (PFN_GetWindowDpiCtx)GetProcAddress(u, "GetWindowDpiAwarenessContext");
    p_GetAwarenessFromDpiAwarenessContext = (PFN_AwarenessOfCtx)GetProcAddress(u, "GetAwarenessFromDpiAwarenessContext");
}

/* cnc-ddraw's own keys: [KPD3] wins over [ddraw], as in config.c cfg_get_string. */
static void cnc_ini(const char *key, const char *def, char *out, DWORD cb)
{
    if (!GetPrivateProfileStringA("KPD3", key, "", out, cb, g_ddrawIni) || !out[0])
        GetPrivateProfileStringA("ddraw", key, def, out, cb, g_ddrawIni);
}

static int cnc_bool(const char *key)
{
    char v[16];
    cnc_ini(key, "false", v, sizeof(v));
    return !_stricmp(v, "yes") || !_stricmp(v, "true") || !_stricmp(v, "1");
}

static void menu_config(void)           /* first menu_tick */
{
    char v[32], *e;
    GetPrivateProfileStringA("configit", "menubar", "pinned", v, sizeof(v), g_ddrawIni);
    g_menuMode = !_stricmp(v, "off") ? MENU_OFF : !_stricmp(v, "auto") ? MENU_AUTO : MENU_PINNED;
    if (g_menuMode == MENU_OFF) { notef("menubar", 1, "menubar=off: KPD3's menu bar left alone"); return; }
    if (!r_GetWindowRect || !r_GetClientRect || !r_ClientToScreen || !r_SetWindowPos ||
        !r_SetWindowLongA || !r_GetWindowLongA || !r_DefWindowProcA || !r_ShowWindow) {
        notef("menubar", 0, "user32 entry points missing - menu bar fix disabled");
        g_menuMode = MENU_OFF;
        return;
    }
    g_menuSyncMsg = RegisterWindowMessageA("configit.menubar.sync");
    g_boxing  = cnc_bool("boxing");
    g_maintas = cnc_bool("maintas");
    cnc_ini("aspect_ratio", "", v, sizeof(v));
    if (v[0]) {                          /* config.c: aspect_ratio implies maintas */
        unsigned long cx = strtoul(v, &e, 0), cy = *e ? strtoul(e + 1, &e, 0) : 0;
        if (cx && cy) { g_arX = (int)cx; g_arY = (int)cy; }
        g_maintas = 1;
    }
    notef("menubar", 1, "menubar=%s; cnc-ddraw scaling boxing=%d maintas=%d aspect %d:%d (%s)",
          g_menuMode == MENU_AUTO ? "auto" : "pinned", g_boxing, g_maintas, g_arX, g_arY, g_ddrawIni);
}

/* cnc-ddraw's viewport inside a W x H client (dd.c dd_SetDisplayMode, gdi renderer). */
static void cnc_viewport(int W, int H, int *vx, int *vy, int *vw, int *vh)
{
    int i;
    *vw = W; *vh = H;
    if (g_boxing) {
        for (i = 19; i >= 1; i--)
            if (KPD3_W * i <= W && KPD3_H * i <= H) { *vw = KPD3_W * i; *vh = KPD3_H * i; break; }
    } else if (g_maintas) {
        double dst = (double)g_arY / g_arX, src = (double)H / W;
        *vh = (int)floor(dst * W + 0.5);
        if (src < dst) { *vw = (int)floor((double)W / *vh * H + 0.5); *vh = H; }
        if (*vw > W) *vw = W;
        if (*vh > H) *vh = H;
    }
    *vx = W / 2 - *vw / 2;
    *vy = H / 2 - *vh / 2;
}

/* Height of the popup's own menu bar (one row, or more if the titles wrap). */
static int menu_bar_height(void)
{
    MENUBARINFO mbi;
    RECT wr;
    memset(&mbi, 0, sizeof(mbi));
    mbi.cbSize = sizeof(mbi);
    if (GetMenu(g_menuWnd) && GetMenuBarInfo(g_menuWnd, OBJID_MENU, 0, &mbi) && mbi.rcBar.bottom > mbi.rcBar.top)
        return mbi.rcBar.bottom - mbi.rcBar.top;
    return (r_GetWindowRect(g_menuWnd, &wr) && wr.bottom > wr.top) ? wr.bottom - wr.top : 0;
}

/* Where the popup belongs, in the calling thread's coordinates: the top edge
   of the viewport, viewport-wide, one bar tall. */
static int menu_target(RECT *r, char *info, int cb)
{
    RECT cr;
    POINT o = { 0, 0 };
    int vx, vy, vw, vh, bar;
    if (IsIconic(g_mainWnd) || !r_GetClientRect(g_mainWnd, &cr) || !r_ClientToScreen(g_mainWnd, &o))
        return 0;
    if (cr.right < KPD3_W / 2 || cr.bottom < KPD3_H / 2) return 0;
    cnc_viewport(cr.right, cr.bottom, &vx, &vy, &vw, &vh);
    bar = menu_bar_height();
    if (bar <= 0) return 0;
    SetRect(r, o.x + vx, o.y + vy, o.x + vx + vw, o.y + vy + bar);
    _snprintf(info, cb, "client %ldx%ld at %ld,%ld, viewport %d,%d %dx%d, bar %d",
              cr.right, cr.bottom, o.x, o.y, vx, vy, vw, vh, bar);
    info[cb - 1] = 0;
    return 1;
}

/* Move the popup to the band. This runs in physical pixels: KPD3's windows
   were created before cnc-ddraw made the process per-monitor aware, and a
   window procedure runs in its window's DPI context, so without this the
   boxing maths could be done in logical pixels. A second pass picks up a
   bar that wrapped at the new width. */
static int menu_place(const char *why)
{
    RECT want, have;
    HANDLE ctx = NULL;
    char info[160];
    int pass, ok = 0;
    if (g_placing || !g_menuWnd || !g_mainWnd) return 0;
    g_placing = 1;
    if (p_SetThreadDpiAwarenessContext) ctx = p_SetThreadDpiAwarenessContext(DPI_CTX_PMV2);
    for (pass = 0; pass < 3; pass++) {
        if (!menu_target(&want, info, sizeof(info))) break;
        ok = 1;
        if (r_GetWindowRect(g_menuWnd, &have) && EqualRect(&want, &have)) break;
        r_SetWindowPos(g_menuWnd, NULL, want.left, want.top, want.right - want.left, want.bottom - want.top,
                       SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
        if (g_nPlace++ < 12)
            tracef("menubar", 1, "placed at %ld,%ld %ldx%ld; %s [%s]", want.left, want.top,
                  want.right - want.left, want.bottom - want.top, info, why);
    }
    if (ctx) p_SetThreadDpiAwarenessContext(ctx);
    g_placing = 0;
    return ok;
}

/* The engine's own pin, vfunc 0xF8 of the window object (popup's extra long 0). */
static int menu_pin(int on)
{
    void *obj = (void *)r_GetWindowLongA(g_menuWnd, 0), **vt = NULL, *fn = NULL;
    __try { vt = obj ? *(void ***)obj : NULL; fn = vt ? vt[0xF8 / 4] : NULL; } __except (1) { fn = NULL; }
    if ((DWORD)fn != KPD3_PIN_FN) {
        notef("menubar", 0, "pin: object %p vtable %p slot 0xF8 = %p, expected %08X - not pinned",
              obj, vt, fn, KPD3_PIN_FN);
        return 0;
    }
    ((PFN_pin)fn)(obj, NULL, on);
    return 1;
}

/* Pinned policy: shown whenever the game has a menu attached and its main
   window is visible. Placement is refreshed every time. */
static void menu_sync(void)
{
    void *obj;
    int want;
    if (!g_menuWnd || !g_mainWnd) return;
    menu_place("sync");
    if (g_menuMode != MENU_PINNED) return;
    obj = (void *)r_GetWindowLongA(g_menuWnd, 0);
    if (!obj) return;
    want = GetMenu(g_menuWnd) != NULL && IsWindowVisible(g_mainWnd);
    if (want != (KPD3_PINNED(obj) != 0)) {
        if (menu_pin(want)) {
            tracef("menubar", 1, "%s (HMENU %p)", want ? "pinned: menu bar always shown" : "unpinned", GetMenu(g_menuWnd));
            if (want) r_DefWindowProcA(g_menuWnd, WM_NCACTIVATE, TRUE, 0);   /* draw the titles active */
        }
    } else if (want && !IsWindowVisible(g_menuWnd) && !IsIconic(g_mainWnd)) {
        r_ShowWindow(g_menuWnd, SW_SHOWNA);
    }
}

static LRESULT CALLBACK menu_subclass(HWND h, UINT m, WPARAM w, LPARAM l)
{
    WNDPROC prev = g_menuPrev;
    if (m == g_menuSyncMsg && g_menuSyncMsg) { menu_sync(); return 0; }
    switch (m) {
    case WM_USER:                       /* the game's "follow the main window" (0x508BC4) */
        menu_place("WM_USER");
        return 0;
    case WM_WINDOWPOSCHANGED: {         /* anything else that moved it */
        LRESULT r = CallWindowProcA(prev, h, m, w, l);
        const WINDOWPOS *wp = (const WINDOWPOS *)l;
        if ((wp->flags & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE))
            menu_place("moved");
        return r;
    }
    case WM_NCPAINT: {                  /* the game's version, minus the screen-vs-window offset bug */
        LRESULT r = r_DefWindowProcA(h, m, w, l);
        HDC dc = GetWindowDC(h);
        RECT rc;
        if (dc) {
            r_GetWindowRect(h, &rc);
            SetRect(&rc, 0, rc.bottom - rc.top - 1, rc.right - rc.left, rc.bottom - rc.top);
            FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            ReleaseDC(h, dc);
        }
        return r;
    }
    case WM_NCACTIVATE:                 /* the popup is almost never the active window, and Windows greys an
                                           inactive window's menu titles; keep the pinned bar looking live */
        if (g_menuMode == MENU_PINNED)
            return CallWindowProcA(prev, h, m, TRUE, l);
        break;
    case WM_ERASEBKGND: {               /* class brush is (HBRUSH)4 = inactive-caption blue; match the band */
        RECT rc;
        r_GetClientRect(h, &rc);
        FillRect((HDC)w, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    case WM_EXITMENULOOP: {             /* game hides it here unless pinned */
        LRESULT r = CallWindowProcA(prev, h, m, w, l);
        if (IsWindowVisible(h) && GetActiveWindow() == h && g_mainWnd && IsWindowEnabled(g_mainWnd)) {
            SetActiveWindow(g_mainWnd);     /* what hiding it does in the original */
            SetFocus(g_mainWnd);
            if (g_nBack++ < 3) tracef("menubar", 1, "menu closed: activation and focus back to the main window");
        }
        return r;
    }
    case WM_NCDESTROY:
        g_menuWnd = NULL;
        g_menuPrev = NULL;
        break;
    }
    return CallWindowProcA(prev, h, m, w, l);
}

/* Above cnc-ddraw's fake_WndProc: pass plain Alt/F10 to the game's own
   handler (path B @0x509260: show, SetForegroundWindow, SC_KEYMENU to the
   popup). Alt+letter and Alt+Space keep cnc-ddraw's "return 0"; the game
   ignores the first, and the second would enter the system-menu loop. */
static LRESULT CALLBACK main_subclass(HWND h, UINT m, WPARAM w, LPARAM l)
{
    WNDPROC prev = g_mainPrev;
    LRESULT r;
    if (m == WM_SYSCOMMAND && (w & 0xFFF0) == SC_KEYMENU && LOWORD(l) == 0 && g_gameProc &&
        g_menuWnd && GetMenu(g_menuWnd) && IsWindowEnabled(g_menuWnd)) {
        if (g_nKey++ < 3) tracef("menubar", 1, "Alt/F10 (SC_KEYMENU) passed to the game");
        return CallWindowProcA(g_gameProc, h, m, w, l);
    }
    if (m == WM_NCDESTROY) { g_mainPrev = NULL; g_gameProc = NULL; }
    r = CallWindowProcA(prev, h, m, w, l);
    if ((m == WM_WINDOWPOSCHANGED || m == WM_SHOWWINDOW || m == WM_DISPLAYCHANGE || m == WM_DPICHANGED) &&
        g_menuWnd && g_menuSyncMsg)
        PostMessageA(g_menuWnd, g_menuSyncMsg, 0, 0);
    return r;
}

static void main_subclass_check(void)
{
    WNDPROC cur, game;
    HMODULE m = NULL, dd = GetModuleHandleA("ddraw.dll");
    if (!g_mainWnd || !dd) return;
    cur = (WNDPROC)r_GetWindowLongA(g_mainWnd, GWL_WNDPROC);
    if (cur == main_subclass) return;
    /* only directly above cnc-ddraw's fake_WndProc */
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)cur, &m) || m != dd)
        return;
    /* KPD3's GetWindowLongA slot is cnc's fake: GWL_WNDPROC of main -> g_ddraw.wndproc */
    game = (WNDPROC)(*(PFN_GetWindowLongA *)KPD3_IAT_GetWindowLongA)(g_mainWnd, GWL_WNDPROC);
    if ((DWORD)game < KPD3_IMAGE_LO || (DWORD)game >= KPD3_IMAGE_HI) {
        if (!g_badGame++) notef("menubar", 0, "Alt/F10: game window proc %p not in KPD3.exe - not subclassed", game);
        return;
    }
    g_gameProc = game;
    g_mainPrev = cur;
    r_SetWindowLongA(g_mainWnd, GWL_WNDPROC, (LONG)(LONG_PTR)main_subclass);
    notef("menubar", 1, "Alt/F10: main window %p subclassed above cnc-ddraw (cnc proc %p, game proc %p)",
          g_mainWnd, cur, game);
}

static BOOL CALLBACK find_menu_wnd(HWND h, LPARAM lp)
{
    char c[32];
    if (GetClassNameA(h, c, sizeof(c)) && !strcmp(c, "menuWndProc")) { *(HWND *)lp = h; return FALSE; }
    return TRUE;
}

static int menu_attach(void)
{
    HWND f = NULL, owner;
    if (g_menuWnd && !IsWindow(g_menuWnd)) { g_menuWnd = NULL; g_menuPrev = NULL; }
    if (g_menuWnd) return 1;
    EnumThreadWindows(g_mainTid, find_menu_wnd, (LPARAM)&f);
    if (!f || !(owner = GetWindow(f, GW_OWNER))) return 0;
    g_mainWnd  = owner;
    g_menuPrev = (WNDPROC)r_GetWindowLongA(f, GWL_WNDPROC);
    g_menuWnd  = f;
    r_SetWindowLongA(f, GWL_WNDPROC, (LONG)(LONG_PTR)menu_subclass);
    {
        UINT d1 = p_GetDpiForWindow ? p_GetDpiForWindow(f) : 0, d2 = p_GetDpiForWindow ? p_GetDpiForWindow(owner) : 0;
        int a1 = -1, a2 = -1;
        if (p_GetWindowDpiAwarenessContext && p_GetAwarenessFromDpiAwarenessContext) {
            a1 = p_GetAwarenessFromDpiAwarenessContext(p_GetWindowDpiAwarenessContext(f));
            a2 = p_GetAwarenessFromDpiAwarenessContext(p_GetWindowDpiAwarenessContext(owner));
        }
        notef("menubar", 1, "popup %p subclassed (game proc %p, expected 00508B6D), owner %p; DPI popup %u/aw%d main %u/aw%d",
              f, g_menuPrev, owner, d1, a1, d2, a2);
    }
    return 1;
}

/* Replaces CALL [GetWindowRect] @0x509202 (click at game y == 0). The game
   builds the SC_MOUSEMENU point from this rect as rc.left+10, mid-height,
   which must be in screen coordinates. Return the bar itself. This runs in
   the main window's DPI context, and the popup has the same one. */
static BOOL WINAPI menu_click_rect(HWND h, LPRECT r)
{
    MENUBARINFO mbi;
    memset(&mbi, 0, sizeof(mbi));
    mbi.cbSize = sizeof(mbi);
    if (GetMenuBarInfo(h, OBJID_MENU, 0, &mbi) && mbi.rcBar.bottom > mbi.rcBar.top) { *r = mbi.rcBar; return TRUE; }
    return r_GetWindowRect(h, r);
}

/* Called on every KPD3 present (my_IntersectClipRect), main thread only. */
static void menu_tick(void)
{
    static DWORD last;
    DWORD now;
    if (!g_primTried || GetCurrentThreadId() != g_mainTid) return;
    if (!g_menuInit) {
        g_menuInit = 1;
        menu_config();
        if (g_menuMode != MENU_OFF &&
            redirect_icall(KPD3_CLICK_GETRECT, (void *)menu_click_rect, KPD3_IAT_GetWindowRect))
            notef("menubar", 1, "click-at-top path: GetWindowRect @%08X now returns true screen coordinates",
                  KPD3_CLICK_GETRECT);
    }
    if (g_menuMode == MENU_OFF) return;
    now = GetTickCount();
    if (now - last < 250) return;
    last = now;
    if (!menu_attach()) return;
    main_subclass_check();
    PostMessageA(g_menuWnd, g_menuSyncMsg, 0, 0);
}

#include "qtstub.inc.c"

/* ---- IAT patching -------------------------------------------------- */
/* Replace one imported function pointer in the host EXE's import table. */
static void *hook_iat(const char *dll, const char *fn, void *replacement)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;
    HMODULE hm = GetModuleHandleA(dll);
    void *target;
    /* We are a static import of the EXE, so a DLL we want to hook may not be
       loaded yet (import order is not guaranteed). Load it rather than
       silently failing to install the hook. */
    if (!hm) hm = LoadLibraryA(dll);
    target = (void *)(hm ? GetProcAddress(hm, fn) : NULL);
    if (!dir.VirtualAddress || !target) return NULL;
    imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
    for (; imp->Name; imp++) {
        IMAGE_THUNK_DATA *t = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; t->u1.Function; t++) {
            if ((void *)t->u1.Function == target) {
                DWORD old;
                void *prev = (void *)t->u1.Function;
                VirtualProtect(&t->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
                t->u1.Function = (ULONG_PTR)replacement;
                VirtualProtect(&t->u1.Function, sizeof(void *), old, &old);
                return prev;
            }
        }
    }
    return NULL;
}

#define HOOK(dll, name) \
    do { \
        o_##name = (PFN_##name)hook_iat(dll, #name, my_##name); \
        if (!o_##name) { failed++; notef("proxy", 0, "hook FAILED: %s!%s", dll, #name); } \
        else hooked++; \
    } while (0)

#define HOOK_IF_IMPORTED(dll, name) \
    do { \
        o_##name = (PFN_##name)hook_iat(dll, #name, my_##name); \
        if (o_##name) hooked++; \
    } while (0)

/* ---- forwarding thunks to the real configit.dll -------------------- */
static HMODULE g_real;
static FARPROC g_p[10];
__declspec(naked) void fwd_GetMONETInternalCfgLocDLL(void){ __asm { jmp dword ptr [g_p+0] } }
__declspec(naked) void fwd_GetMONETParamCfgLocDLL(void){ __asm { jmp dword ptr [g_p+4] } }
__declspec(naked) void fwd_comm_cfgVerifyEmailConfigDLL_at_8(void){ __asm { jmp dword ptr [g_p+8] } }
__declspec(naked) void fwd_comm_cfgVerifyInternetConfigDLL_at_8(void){ __asm { jmp dword ptr [g_p+12] } }
__declspec(naked) void fwd_commRetrieveConfigurationDLL(void){ __asm { jmp dword ptr [g_p+16] } }
__declspec(naked) void fwd_commStoreConfigurationDLL(void){ __asm { jmp dword ptr [g_p+20] } }
__declspec(naked) void fwd_comm_cfgConfigureDLL(void){ __asm { jmp dword ptr [g_p+24] } }
__declspec(naked) void fwd_comm_cfgGetConfigurationDLL(void){ __asm { jmp dword ptr [g_p+28] } }
__declspec(naked) void fwd_comm_cfgSetConfigurationDLL(void){ __asm { jmp dword ptr [g_p+32] } }
__declspec(naked) void fwd_comm_cfgVerifyConfigDLL(void){ __asm { jmp dword ptr [g_p+36] } }

static void load_real(void)
{
    char path[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA("configit.dll"), path, MAX_PATH);
    { char *q = strrchr(path, 92); if (q) q[1] = 0; }
    strcat(path, "configit_real.dll");
    g_real = LoadLibraryA(path);
    log_line(1, "proxy", g_real != NULL, path);
    if (!g_real) return;

    g_p[0] = GetProcAddress(g_real, "GetMONETInternalCfgLocDLL");
    g_p[1] = GetProcAddress(g_real, "GetMONETParamCfgLocDLL");
    g_p[2] = GetProcAddress(g_real, "_comm_cfgVerifyEmailConfigDLL@8");
    g_p[3] = GetProcAddress(g_real, "_comm_cfgVerifyInternetConfigDLL@8");
    g_p[4] = GetProcAddress(g_real, "commRetrieveConfigurationDLL");
    g_p[5] = GetProcAddress(g_real, "commStoreConfigurationDLL");
    g_p[6] = GetProcAddress(g_real, "comm_cfgConfigureDLL");
    g_p[7] = GetProcAddress(g_real, "comm_cfgGetConfigurationDLL");
    g_p[8] = GetProcAddress(g_real, "comm_cfgSetConfigurationDLL");
    g_p[9] = GetProcAddress(g_real, "comm_cfgVerifyConfigDLL");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    if (reason == DLL_PROCESS_ATTACH) {
        int hooked = 0, failed = 0;
        DWORD_PTR procMask = 0, sysMask = 0;
        /* One CPU, before any game thread exists. KPD3 starts a worker thread
           during startup that races the main thread (garbage function pointers,
           heap corruption) on a multi-core machine. Windows' Win98 layer hid this
           with SingleProcAffinity; cnc-ddraw's singlecpu comes too late, because
           DDRAW.DLL only loads after the race. */
        if (GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask) && procMask)
            SetProcessAffinityMask(GetCurrentProcess(), procMask & (~procMask + 1));
        g_t0 = GetTickCount();
        g_mainTid = GetCurrentThreadId();
        log_open(h);
        DisableProcessWindowsGhosting();   /* the Win98 layer's NoGhost: loads can stall the message loop */
        if (g_log) {
            /* Has Windows applied a compatibility layer again? Its shim DLLs
               were behind both the AcLayers hang and (probably) the winmm
               aborts. apphelp.dll is not counted: it is loaded on every
               launch, including those where no layer applies. */
            static const char *eng[] = { "AcGenral.dll", "AcLayers.dll", "AcXtrnal.dll", "AcSpecfc.dll" };
            char m[256] = "compatibility layer DLLs loaded:";
            int i, any = 0;
            for (i = 0; i < 4; i++)
                if (GetModuleHandleA(eng[i])) { strcat(m, " "); strcat(m, eng[i]); any = 1; }
            if (!any) strcat(m, " none");
            log_line(1, "proxy", !any, m);
        }
        load_real();
        qt_install();
        rename_manual();
        trap_purecall();
        if (g_log) AddVectoredExceptionHandler(1, av_logger);
        redirect_icall(KPD3_FONTCAST_START, (void *)my_FontChangeNotify, KPD3_IAT_SendMessageA);
        redirect_icall(KPD3_FONTCAST_EXIT,  (void *)my_FontChangeNotify, KPD3_IAT_SendMessageA);
        redirect_call(0x004FA3EA, pick_mode,   0x004FA863);
        redirect_call(0x004FA471, log_setmode, 0x004FEDC0);

        HOOK("kernel32.dll", CreateFileA);
        HOOK("kernel32.dll", OutputDebugStringA);
        HOOK("kernel32.dll", LoadLibraryA);
        HOOK("kernel32.dll", GetVersion);
        HOOK("kernel32.dll", GetVersionExA);
        HOOK("kernel32.dll", ExitProcess);
        HOOK("kernel32.dll", WaitForSingleObject);
        HOOK("winmm.dll",    midiOutGetDevCapsA);
        HOOK("winmm.dll",    waveOutGetDevCapsA);
        HOOK("user32.dll",   ChangeDisplaySettingsA);
        HOOK_IF_IMPORTED("user32.dll", ChangeDisplaySettingsExA);   /* KPD3.exe does not import it; covered anyway */
        HOOK("user32.dll",   PostQuitMessage);
        HOOK("user32.dll",   MessageBoxA);
        HOOK("user32.dll",   ShowWindow);
        HOOK("user32.dll",   CreateWindowExA);
        HOOK("user32.dll",   GetSystemMetrics);
        HOOK("gdi32.dll",    GetDeviceCaps);
        HOOK("gdi32.dll",    IntersectClipRect);
        HOOK("user32.dll",   ToAscii);
        HOOK("kernel32.dll", DeviceIoControl);
        HOOK("kernel32.dll", CreateProcessA);
        /* cnc-ddraw re-patches the slots it hooks when DDRAW.DLL loads, so the
           logging in ShowWindow, CreateWindowExA, LoadLibraryA and the
           GetSystemMetrics/GetDeviceCaps fakes stop there; cnc-ddraw's own
           versions report the same 800x600 from then on. */
        menu_resolve();
        notef("proxy", failed == 0, "%d hooks installed, %d failed", hooked, failed);
    }
    return TRUE;
}

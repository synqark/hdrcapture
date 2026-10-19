// hdrrec_gui - small tray front-end for hdrrec.exe
//   - global hotkey (default Alt+F9) starts / stops a recording
//   - every recording runs hdrrec.exe (same folder) as a child process without a console.
//     It is stopped by writing a newline to its stdin; if this program dies, the pipe closes,
//     hdrrec reads EOF and still finalizes the file.
//   - the capture loop's timing is untouched: this process only reads hdrrec's output
//   - settings: hdrrec_gui.ini next to the exe (UTF-16), logs: logs\<recording name>.log
//
// Usage: hdrrec_gui.exe [/tray]      /tray: start hidden in the notification area

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <dxgi1_6.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

#include "hdrrec_gui_res.h"

enum : UINT {
    WM_APP_TRAY = WM_APP + 1,   // notification icon callback
    WM_APP_LINE,                // lParam: std::string * (one output line of hdrrec, UTF-8)
    WM_APP_EXIT,                // wParam: exit code of hdrrec
    WM_APP_LLHOTKEY,            // hotkey seen by the low-level keyboard hook
    WM_APP_DPI,                 // re-create fonts after a DPI change
};
static const UINT kHotkeyId = 1;
static const UINT_PTR kTimerId = 1;

enum class State { Idle, Starting, Recording, Stopping };
enum class Kind { Normal, Good, Warn, Error };

static const UINT kDefaultHotkeyVk = VK_F9, kDefaultHotkeyMod = MOD_ALT;

struct Settings {
    UINT hk_vk = kDefaultHotkeyVk, hk_mod = kDefaultHotkeyMod;
    std::wstring out_dir, pattern = L"rec_%Y%m%d_%H%M%S";
    int monitor = 0, fps = 60, audio = 1, abitrate = 256;
    std::wstring codec = L"hevc", preset = L"p4", acodec = L"aac", extra;
    std::wstring audio_proc;   // exe name: record only this application's audio (empty = whole system)
    int again = 0;             // audio gain in dB (hdrrec -again)
    int gpuprio = 0;           // hdrrec -gpuprio 0..3 (0 = unchanged)
    // quality: rate control "cq" (target quality, per codec: hevc 1..51, av1 1..63) or "bitrate" (Mbps)
    int timelog = 1;           // write logs\<name>_times.csv (per-capture timing, ~2.5 KB/s)
    std::wstring rc = L"cq";
    int cq = 18, cq_av1 = 24, bitrate = 100;
    // HDR10 static metadata (ini only; defaults match a 1000-nit display)
    int mdl = 1000, maxcll = 1000, maxfall = 400;
};

static const int kBitrateMin = 5, kBitrateMax = 500;
static const int kGainMin = -30, kGainMax = 20;   // dB

// result of one hdrrec run, parsed from its output
struct Summary {
    double init_ms = -1;
    long long frames = -1, unique = 0, repeated = 0, capture_missed = 0, missed = 0, overflow = 0;
    std::string error;
    bool audio_fallback = false;   // the selected application was not found: system audio was recorded
    bool gpu_warning = false;      // the GPU priority could not be (fully) applied
};

static HINSTANCE g_inst;
static HWND g_dlg;
static std::wstring g_exe_dir, g_ini;
static Settings g_cfg;
static State g_state = State::Idle;
static bool g_exit_after_stop = false;

static HANDLE g_proc, g_stdin;
static ULONGLONG g_press_tick, g_rec_tick;
static std::wstring g_cur_file, g_last_file;
static Summary g_sum;
static long long g_live_unique, g_live_dup, g_live_missed, g_live_ovf;

static HICON g_icon_small[3], g_icon_big[3];   // idle, busy, recording
static HFONT g_big_font;
static UINT g_msg_taskbar, g_msg_show;
static HHOOK g_llhook;
static bool g_ll_down, g_hotkey_err, g_hdr_on = true;
static Kind g_last_kind = Kind::Normal;
static int g_idle_ticks;
static bool g_filling;   // controls are being set from code: ignore their change notifications
static std::vector<std::wstring> g_audio_apps;   // IDC_AUDIOSRC items from index 2 on
static std::vector<int> g_fps_items;             // IDC_FPS items

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
static std::wstring widen(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

static std::string narrow(const std::wstring &w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring fmt(const wchar_t *f, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, f);
    vswprintf(buf, 1024, f, ap);
    va_end(ap);
    return buf;
}

static std::wstring hms(long long sec) {
    return fmt(L"%02lld:%02lld:%02lld", sec / 3600, sec / 60 % 60, sec % 60);
}

static std::wstring file_name(const std::wstring &path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

static bool file_exists(const std::wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static std::wstring dlg_text(int id) {
    HWND h = GetDlgItem(g_dlg, id);
    std::wstring s(GetWindowTextLengthW(h) + 1, L'\0');
    s.resize(GetWindowTextW(h, &s[0], (int)s.size()));
    return s;
}

static std::wstring clean_dir(std::wstring d) {
    while (!d.empty() && (d.back() == L' ' || d.back() == L'\t')) d.pop_back();
    while (!d.empty() && (d.front() == L' ' || d.front() == L'\t')) d.erase(0, 1);
    if (d.size() >= 2 && d.front() == L'"' && d.back() == L'"') d = d.substr(1, d.size() - 2);
    while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    if (d.size() == 3 && d[1] == L':') d.pop_back();   // "F:\" -> "F:" (a separator is added later)
    return d;
}

// ---------------------------------------------------------------------------
// settings (hdrrec_gui.ini, UTF-16 so that any path survives)
// ---------------------------------------------------------------------------
static std::wstring ini_get(const wchar_t *k, const std::wstring &def) {
    wchar_t buf[2048];
    GetPrivateProfileStringW(L"hdrrec", k, def.c_str(), buf, 2048, g_ini.c_str());
    return buf;
}
static int ini_geti(const wchar_t *k, int def) { return (int)GetPrivateProfileIntW(L"hdrrec", k, def, g_ini.c_str()); }
static void ini_set(const wchar_t *k, const std::wstring &v) { WritePrivateProfileStringW(L"hdrrec", k, v.c_str(), g_ini.c_str()); }
static void ini_seti(const wchar_t *k, int v) { ini_set(k, std::to_wstring(v)); }

static void save_settings() {
    ini_seti(L"HotkeyVk", (int)g_cfg.hk_vk);
    ini_seti(L"HotkeyMod", (int)g_cfg.hk_mod);
    ini_set(L"OutputDir", g_cfg.out_dir);
    ini_set(L"FilePattern", g_cfg.pattern);
    ini_seti(L"Monitor", g_cfg.monitor);
    ini_seti(L"Fps", g_cfg.fps);
    ini_set(L"Codec", g_cfg.codec);
    ini_set(L"Preset", g_cfg.preset);
    ini_set(L"RateControl", g_cfg.rc);
    ini_seti(L"CQ", g_cfg.cq);
    ini_seti(L"CQAV1", g_cfg.cq_av1);
    ini_seti(L"Bitrate", g_cfg.bitrate);
    ini_seti(L"MDL", g_cfg.mdl);
    ini_seti(L"MaxCLL", g_cfg.maxcll);
    ini_seti(L"MaxFALL", g_cfg.maxfall);
    ini_seti(L"Audio", g_cfg.audio);
    ini_set(L"AudioProcess", g_cfg.audio_proc);
    ini_seti(L"AudioGain", g_cfg.again);
    ini_seti(L"GpuPriority", g_cfg.gpuprio);
    ini_set(L"ACodec", g_cfg.acodec);
    ini_seti(L"ABitrate", g_cfg.abitrate);
    ini_seti(L"TimeLog", g_cfg.timelog);
    ini_set(L"ExtraArgs", g_cfg.extra);
}

static void load_settings() {
    if (!file_exists(g_ini)) {   // a UTF-16 BOM makes the profile API write Unicode
        HANDLE h = CreateFileW(g_ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { DWORD w; WORD bom = 0xFEFF; WriteFile(h, &bom, 2, &w, nullptr); CloseHandle(h); }
    }
    std::wstring videos;
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &p))) videos = std::wstring(p) + L"\\hdrrec";
    CoTaskMemFree(p);

    Settings d;
    g_cfg.hk_vk = (UINT)ini_geti(L"HotkeyVk", (int)d.hk_vk);
    g_cfg.hk_mod = (UINT)ini_geti(L"HotkeyMod", (int)d.hk_mod);
    g_cfg.out_dir = ini_get(L"OutputDir", videos);
    g_cfg.pattern = ini_get(L"FilePattern", d.pattern);
    g_cfg.monitor = ini_geti(L"Monitor", d.monitor);
    g_cfg.fps = ini_geti(L"Fps", d.fps);
    g_cfg.codec = ini_get(L"Codec", d.codec);
    g_cfg.preset = ini_get(L"Preset", d.preset);
    g_cfg.rc = ini_get(L"RateControl", d.rc);
    g_cfg.cq = ini_geti(L"CQ", d.cq);
    g_cfg.cq_av1 = ini_geti(L"CQAV1", d.cq_av1);
    g_cfg.bitrate = ini_geti(L"Bitrate", d.bitrate);
    g_cfg.mdl = ini_geti(L"MDL", d.mdl);
    g_cfg.maxcll = ini_geti(L"MaxCLL", d.maxcll);
    g_cfg.maxfall = ini_geti(L"MaxFALL", d.maxfall);
    g_cfg.audio = ini_geti(L"Audio", d.audio);
    g_cfg.audio_proc = ini_get(L"AudioProcess", d.audio_proc);
    g_cfg.again = std::min(std::max(ini_geti(L"AudioGain", d.again), kGainMin), kGainMax);
    g_cfg.gpuprio = std::min(std::max(ini_geti(L"GpuPriority", d.gpuprio), 0), 3);
    g_cfg.acodec = ini_get(L"ACodec", d.acodec);
    g_cfg.abitrate = ini_geti(L"ABitrate", d.abitrate);
    g_cfg.timelog = ini_geti(L"TimeLog", d.timelog);
    g_cfg.extra = ini_get(L"ExtraArgs", d.extra);
    if (g_cfg.fps < 1 || g_cfg.fps > 480) g_cfg.fps = 60;   // hdrrec -fps accepts integers 1..480
    if (g_cfg.codec != L"av1") g_cfg.codec = L"hevc";
    // p5..p7 cannot keep up with 4K60 while a game loads the GPU (measured: hevc p6 overflowed the
    // encoder queue, av1 p6 missed captures), so the GUI offers p1..p4 only
    if (g_cfg.preset.size() != 2 || g_cfg.preset[0] != L'p' || g_cfg.preset[1] < L'1') g_cfg.preset = d.preset;
    if (g_cfg.preset[1] > L'4') g_cfg.preset = L"p4";
    if (g_cfg.rc != L"bitrate") g_cfg.rc = L"cq";
    g_cfg.cq = std::min(std::max(g_cfg.cq, 1), 51);
    g_cfg.cq_av1 = std::min(std::max(g_cfg.cq_av1, 1), 63);
    g_cfg.bitrate = std::min(std::max(g_cfg.bitrate, kBitrateMin), kBitrateMax);
    save_settings();   // write all keys (also ones added in newer versions) so the file can be edited by hand
}

// ---------------------------------------------------------------------------
// monitor info (same enumeration order as hdrrec -monitor)
// ---------------------------------------------------------------------------
struct MonitorInfo { long w, h; bool primary, hdr; };

static std::vector<MonitorInfo> enum_monitors() {
    std::vector<MonitorInfo> v;
    IDXGIFactory1 *fac = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac))) return v;
    IDXGIAdapter1 *ad = nullptr;
    for (UINT a = 0; fac->EnumAdapters1(a, &ad) != DXGI_ERROR_NOT_FOUND; a++) {
        IDXGIOutput *o = nullptr;
        for (UINT i = 0; ad->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_OUTPUT_DESC d = {};
            o->GetDesc(&d);
            MonitorInfo m = {d.DesktopCoordinates.right - d.DesktopCoordinates.left,
                             d.DesktopCoordinates.bottom - d.DesktopCoordinates.top, false, false};
            IDXGIOutput6 *o6 = nullptr;
            if (SUCCEEDED(o->QueryInterface(__uuidof(IDXGIOutput6), (void **)&o6))) {
                DXGI_OUTPUT_DESC1 d1 = {};
                if (SUCCEEDED(o6->GetDesc1(&d1))) m.hdr = d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                o6->Release();
            }
            MONITORINFO mi = {sizeof(mi)};
            m.primary = GetMonitorInfoW(d.Monitor, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY);
            v.push_back(m);
            o->Release();
        }
        ad->Release();
    }
    fac->Release();
    return v;
}

// (the device name "\\.\DISPLAYn" is not shown: Japanese fonts draw '\' as a yen sign)
static std::wstring monitor_label(int i, const MonitorInfo &m) {
    return fmt(L"%d: %ldx%ld%ls  HDR %ls", i, m.w, m.h, m.primary ? L"（メイン）" : L"", m.hdr ? L"オン" : L"オフ");
}

static std::wstring monitor_text(int index, bool *hdr) {
    std::vector<MonitorInfo> v = enum_monitors();
    *hdr = false;
    if (index < 0 || index >= (int)v.size()) return fmt(L"モニター %d が見つかりません", index);
    const MonitorInfo &m = v[index];
    *hdr = m.hdr;
    return fmt(L"モニター %d（%ldx%ld%ls）  HDR %ls", index, m.w, m.h, m.primary ? L"・メイン" : L"",
               m.hdr ? L"オン" : L"オフ（録画には HDR オンが必要）");
}

// ---------------------------------------------------------------------------
// icons: a dot drawn at runtime (gray = idle, orange = starting/stopping, red = recording)
// ---------------------------------------------------------------------------
static HICON make_dot_icon(int size, COLORREF c) {
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_RGB;
    void *bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    const double cx = size / 2.0, r = size * 0.40;
    DWORD *px = (DWORD *)bits;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int in = 0;   // 4x4 supersampling for a smooth edge
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    double dx = x + (sx + 0.5) / 4 - cx, dy = y + (sy + 0.5) / 4 - cx;
                    in += dx * dx + dy * dy <= r * r;
                }
            DWORD a = (DWORD)(in * 255 / 16);
            px[y * size + x] = (a << 24) | ((DWORD)GetRValue(c) << 16) | ((DWORD)GetGValue(c) << 8) | GetBValue(c);
        }
    std::vector<BYTE> zeros((size_t)((size + 15) / 16 * 2) * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros.data());
    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

static void make_icons() {
    const COLORREF cols[3] = {RGB(128, 128, 128), RGB(240, 150, 0), RGB(232, 17, 35)};
    UINT dpi = GetDpiForWindow(g_dlg);
    for (int i = 0; i < 3; i++) {
        if (g_icon_small[i]) DestroyIcon(g_icon_small[i]);
        if (g_icon_big[i]) DestroyIcon(g_icon_big[i]);
        g_icon_small[i] = make_dot_icon(GetSystemMetricsForDpi(SM_CXSMICON, dpi), cols[i]);
        g_icon_big[i] = make_dot_icon(GetSystemMetricsForDpi(SM_CXICON, dpi), cols[i]);
    }
}

// ---------------------------------------------------------------------------
// hotkey (RegisterHotKey; falls back to a low-level keyboard hook if the combination is
// already registered by another program, e.g. NVIDIA App's Alt+F9)
// ---------------------------------------------------------------------------
static bool is_ext_key(UINT vk) {
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
    case VK_SNAPSHOT: case VK_CANCEL:
        return true;
    }
    return false;
}

static std::wstring hotkey_name(UINT vk, UINT mod) {
    if (!vk) return L"なし";
    std::wstring s;
    if (mod & MOD_CONTROL) s += L"Ctrl+";
    if (mod & MOD_SHIFT) s += L"Shift+";
    if (mod & MOD_ALT) s += L"Alt+";
    if (mod & MOD_WIN) s += L"Win+";
    wchar_t k[64] = {0};
    LONG sc = (LONG)(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16);
    if (is_ext_key(vk)) sc |= 1 << 24;
    if (!GetKeyNameTextW(sc, k, 64)) swprintf(k, 64, L"0x%02X", vk);
    return s + k;
}

static UINT current_mods() {
    UINT m = 0;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) m |= MOD_ALT;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) m |= MOD_SHIFT;
    if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) m |= MOD_WIN;
    return m;
}

static LRESULT CALLBACK ll_keyboard(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        if (k->vkCode == g_cfg.hk_vk) {
            if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) {
                if (!g_ll_down && current_mods() == g_cfg.hk_mod) {
                    g_ll_down = true;
                    PostMessageW(g_dlg, WM_APP_LLHOTKEY, 0, 0);
                }
            } else {
                g_ll_down = false;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);   // never swallow the key
}

static void apply_hotkey() {
    UnregisterHotKey(g_dlg, kHotkeyId);
    if (g_llhook) { UnhookWindowsHookEx(g_llhook); g_llhook = nullptr; }
    const UINT vk = g_cfg.hk_vk, mod = g_cfg.hk_mod;
    const wchar_t *note = L"録画の開始 / 停止";
    g_hotkey_err = false;
    if (!vk) {
        note = L"キーが未設定です";
        g_hotkey_err = true;
    } else if (RegisterHotKey(g_dlg, kHotkeyId, mod | MOD_NOREPEAT, vk)) {
        // ok
    } else if ((g_llhook = SetWindowsHookExW(WH_KEYBOARD_LL, ll_keyboard, g_inst, 0))) {
        note = L"録画の開始 / 停止（他アプリと共用）";
    } else {
        note = L"登録できません";
        g_hotkey_err = true;
    }
    SetDlgItemTextW(g_dlg, IDC_HOTKEY_NOTE, note);
    InvalidateRect(GetDlgItem(g_dlg, IDC_HOTKEY_NOTE), nullptr, TRUE);
}

static WORD to_hotkey_ctrl(UINT vk, UINT mod) {
    BYTE f = 0;
    if (mod & MOD_ALT) f |= HOTKEYF_ALT;
    if (mod & MOD_CONTROL) f |= HOTKEYF_CONTROL;
    if (mod & MOD_SHIFT) f |= HOTKEYF_SHIFT;
    if (is_ext_key(vk)) f |= HOTKEYF_EXT;
    return MAKEWORD(vk, f);
}

static void on_hotkey_edited() {
    WORD w = (WORD)SendDlgItemMessageW(g_dlg, IDC_HOTKEY, HKM_GETHOTKEY, 0, 0);
    UINT vk = LOBYTE(w), f = HIBYTE(w), mod = 0;
    if (!vk) return;   // only modifiers so far
    if (f & HOTKEYF_ALT) mod |= MOD_ALT;
    if (f & HOTKEYF_CONTROL) mod |= MOD_CONTROL;
    if (f & HOTKEYF_SHIFT) mod |= MOD_SHIFT;
    if (!mod && !(vk >= VK_F1 && vk <= VK_F24)) {   // a plain letter would break typing everywhere
        SetDlgItemTextW(g_dlg, IDC_HOTKEY_NOTE, L"Ctrl / Alt / Shift と組み合わせてください");
        g_hotkey_err = true;
        InvalidateRect(GetDlgItem(g_dlg, IDC_HOTKEY_NOTE), nullptr, TRUE);
        return;
    }
    g_cfg.hk_vk = vk;
    g_cfg.hk_mod = mod;
    apply_hotkey();
    ini_seti(L"HotkeyVk", (int)vk);
    ini_seti(L"HotkeyMod", (int)mod);
}

static void reset_hotkey() {
    g_cfg.hk_vk = kDefaultHotkeyVk;
    g_cfg.hk_mod = kDefaultHotkeyMod;
    g_filling = true;
    SendDlgItemMessageW(g_dlg, IDC_HOTKEY, HKM_SETHOTKEY, to_hotkey_ctrl(g_cfg.hk_vk, g_cfg.hk_mod), 0);
    g_filling = false;
    apply_hotkey();
    ini_seti(L"HotkeyVk", (int)g_cfg.hk_vk);
    ini_seti(L"HotkeyMod", (int)g_cfg.hk_mod);
}

// ---------------------------------------------------------------------------
// monitor / encoder settings
// ---------------------------------------------------------------------------
static int cb_sel(int id) { return (int)SendDlgItemMessageW(g_dlg, id, CB_GETCURSEL, 0, 0); }

static void fill_monitors() {
    std::vector<MonitorInfo> v = enum_monitors();
    g_filling = true;
    SendDlgItemMessageW(g_dlg, IDC_MONITOR, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < (int)v.size(); i++)
        SendDlgItemMessageW(g_dlg, IDC_MONITOR, CB_ADDSTRING, 0, (LPARAM)monitor_label(i, v[i]).c_str());
    for (int i = (int)v.size(); i <= g_cfg.monitor; i++)   // keep a saved monitor that is currently missing
        SendDlgItemMessageW(g_dlg, IDC_MONITOR, CB_ADDSTRING, 0, (LPARAM)fmt(L"%d: 見つかりません", i).c_str());
    SendDlgItemMessageW(g_dlg, IDC_MONITOR, CB_SETCURSEL, g_cfg.monitor, 0);
    g_filling = false;
}

static bool is_av1() { return g_cfg.codec == L"av1"; }
static bool is_bitrate() { return g_cfg.rc == L"bitrate"; }
static int &quality_value() { return is_bitrate() ? g_cfg.bitrate : is_av1() ? g_cfg.cq_av1 : g_cfg.cq; }
static void quality_range(int *lo, int *hi) {
    if (is_bitrate()) { *lo = kBitrateMin; *hi = kBitrateMax; }
    else { *lo = 1; *hi = is_av1() ? 63 : 51; }
}

// show the value / range / unit of the current codec + rate control
static void fill_quality() {
    int lo, hi;
    quality_range(&lo, &hi);
    g_filling = true;
    SendDlgItemMessageW(g_dlg, IDC_QSPIN, UDM_SETRANGE32, lo, hi);
    SendDlgItemMessageW(g_dlg, IDC_QSPIN, UDM_SETPOS32, 0, quality_value());
    SetDlgItemTextW(g_dlg, IDC_QUNIT, is_bitrate() ? L"Mbps（最大 1.5 倍）" : fmt(L"%d〜%d、小さいほど高画質", lo, hi).c_str());
    g_filling = false;
}

// output frame rate: common rates, plus any other integer rate set in the ini (Fps=)
static void fill_fps() {
    g_fps_items = {30, 60, 120, 144, 240};
    if (std::find(g_fps_items.begin(), g_fps_items.end(), g_cfg.fps) == g_fps_items.end()) {
        g_fps_items.push_back(g_cfg.fps);
        std::sort(g_fps_items.begin(), g_fps_items.end());
    }
    g_filling = true;
    SendDlgItemMessageW(g_dlg, IDC_FPS, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g_fps_items.size(); i++) {
        SendDlgItemMessageW(g_dlg, IDC_FPS, CB_ADDSTRING, 0, (LPARAM)fmt(L"%d fps", g_fps_items[i]).c_str());
        if (g_fps_items[i] == g_cfg.fps) SendDlgItemMessageW(g_dlg, IDC_FPS, CB_SETCURSEL, i, 0);
    }
    g_filling = false;
}

static void fill_encoder() {
    static const wchar_t *presets[4] = {L"p1（最速）", L"p2", L"p3", L"p4（標準・最高画質）"};
    g_filling = true;
    SendDlgItemMessageW(g_dlg, IDC_CODEC, CB_RESETCONTENT, 0, 0);
    SendDlgItemMessageW(g_dlg, IDC_CODEC, CB_ADDSTRING, 0, (LPARAM)L"HEVC");
    SendDlgItemMessageW(g_dlg, IDC_CODEC, CB_ADDSTRING, 0, (LPARAM)L"AV1");
    SendDlgItemMessageW(g_dlg, IDC_CODEC, CB_SETCURSEL, is_av1() ? 1 : 0, 0);
    SendDlgItemMessageW(g_dlg, IDC_PRESET, CB_RESETCONTENT, 0, 0);
    for (const wchar_t *p : presets) SendDlgItemMessageW(g_dlg, IDC_PRESET, CB_ADDSTRING, 0, (LPARAM)p);
    SendDlgItemMessageW(g_dlg, IDC_PRESET, CB_SETCURSEL, g_cfg.preset[1] - L'1', 0);
    SendDlgItemMessageW(g_dlg, IDC_RCMODE, CB_RESETCONTENT, 0, 0);
    SendDlgItemMessageW(g_dlg, IDC_RCMODE, CB_ADDSTRING, 0, (LPARAM)L"画質指定 (CQ)");
    SendDlgItemMessageW(g_dlg, IDC_RCMODE, CB_ADDSTRING, 0, (LPARAM)L"ビットレート指定");
    SendDlgItemMessageW(g_dlg, IDC_RCMODE, CB_SETCURSEL, is_bitrate() ? 1 : 0, 0);
    static const wchar_t *prios[4] = {L"標準（変更しない）", L"やや高", L"高", L"最高（リアルタイム）"};
    SendDlgItemMessageW(g_dlg, IDC_GPUPRIO, CB_RESETCONTENT, 0, 0);
    for (const wchar_t *p : prios) SendDlgItemMessageW(g_dlg, IDC_GPUPRIO, CB_ADDSTRING, 0, (LPARAM)p);
    SendDlgItemMessageW(g_dlg, IDC_GPUPRIO, CB_SETCURSEL, g_cfg.gpuprio, 0);
    g_filling = false;
    fill_quality();
}

// the number field: accept it once it is a valid value; on leaving the field, clamp and show the stored value
static void on_quality_edited(bool leaving) {
    BOOL ok = FALSE;
    int v = (int)GetDlgItemInt(g_dlg, IDC_QVALUE, &ok, FALSE);
    int lo, hi;
    quality_range(&lo, &hi);
    if (ok && (leaving || (v >= lo && v <= hi))) {
        quality_value() = std::min(std::max(v, lo), hi);
        save_settings();
    }
    if (leaving) fill_quality();
}

static void on_setting_changed(int id) {
    switch (id) {
    case IDC_MONITOR: {
        int s = cb_sel(IDC_MONITOR);
        if (s >= 0) g_cfg.monitor = s;
        break;
    }
    case IDC_CODEC: g_cfg.codec = cb_sel(IDC_CODEC) == 1 ? L"av1" : L"hevc"; fill_quality(); break;
    case IDC_PRESET: g_cfg.preset = L"p" + std::to_wstring(std::max(cb_sel(IDC_PRESET), 0) + 1); break;
    case IDC_RCMODE: g_cfg.rc = cb_sel(IDC_RCMODE) == 1 ? L"bitrate" : L"cq"; fill_quality(); break;
    case IDC_GPUPRIO: g_cfg.gpuprio = std::max(cb_sel(IDC_GPUPRIO), 0); break;
    case IDC_FPS: {
        int s = cb_sel(IDC_FPS);
        if (s >= 0 && s < (int)g_fps_items.size()) g_cfg.fps = g_fps_items[s];
        break;
    }
    }
    save_settings();
}

// ---------------------------------------------------------------------------
// audio source: whole system, none, or one application (hdrrec -audio-process)
// ---------------------------------------------------------------------------
static const CLSID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID kIID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID kIID_IAudioSessionManager2 = {0x77AA99A0, 0x1BD6, 0x484F, {0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F}};
static const IID kIID_IAudioSessionControl2 = {0xBFB7FF88, 0x7239, 0x4FC9, {0x8F, 0xA2, 0x07, 0xC9, 0x50, 0xBE, 0x9C, 0x6D}};

static bool same_name(const std::wstring &a, const std::wstring &b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

// executables that currently have an audio session on any output device (what the volume mixer lists)
static std::vector<std::wstring> audio_apps() {
    std::vector<std::wstring> v;
    IMMDeviceEnumerator *en = nullptr;
    IMMDeviceCollection *devs = nullptr;
    if (FAILED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void **)&en))) return v;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devs))) {
        UINT nd = 0;
        devs->GetCount(&nd);
        for (UINT d = 0; d < nd; d++) {
            IMMDevice *dev = nullptr;
            IAudioSessionManager2 *mgr = nullptr;
            IAudioSessionEnumerator *se = nullptr;
            if (SUCCEEDED(devs->Item(d, &dev)) &&
                SUCCEEDED(dev->Activate(kIID_IAudioSessionManager2, CLSCTX_ALL, nullptr, (void **)&mgr)) &&
                SUCCEEDED(mgr->GetSessionEnumerator(&se))) {
                int ns = 0;
                se->GetCount(&ns);
                for (int i = 0; i < ns; i++) {
                    IAudioSessionControl *c = nullptr;
                    IAudioSessionControl2 *c2 = nullptr;
                    DWORD pid = 0;
                    AudioSessionState state = AudioSessionStateExpired;
                    if (SUCCEEDED(se->GetSession(i, &c)) &&
                        SUCCEEDED(c->QueryInterface(kIID_IAudioSessionControl2, (void **)&c2)) &&
                        c2->IsSystemSoundsSession() != S_OK && SUCCEEDED(c2->GetProcessId(&pid)) && pid &&
                        SUCCEEDED(c2->GetState(&state)) && state != AudioSessionStateExpired) {
                        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                        wchar_t path[MAX_PATH * 2];
                        DWORD len = ARRAYSIZE(path);
                        if (p && QueryFullProcessImageNameW(p, 0, path, &len)) {
                            std::wstring n = file_name(path);
                            bool dup = false;
                            for (const std::wstring &x : v) dup |= same_name(x, n);
                            if (!dup) v.push_back(n);
                        }
                        if (p) CloseHandle(p);
                    }
                    if (c2) c2->Release();
                    if (c) c->Release();
                }
            }
            if (se) se->Release();
            if (mgr) mgr->Release();
            if (dev) dev->Release();
        }
        devs->Release();
    }
    en->Release();
    std::sort(v.begin(), v.end(), [](const std::wstring &a, const std::wstring &b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return v;
}

static void fill_audio() {
    g_audio_apps = audio_apps();
    bool found = g_cfg.audio_proc.empty();
    for (const std::wstring &a : g_audio_apps) found |= same_name(a, g_cfg.audio_proc);
    if (!found) g_audio_apps.push_back(g_cfg.audio_proc);   // keep the saved application while it is silent / not running
    g_filling = true;
    SendDlgItemMessageW(g_dlg, IDC_AUDIOSRC, CB_RESETCONTENT, 0, 0);
    SendDlgItemMessageW(g_dlg, IDC_AUDIOSRC, CB_ADDSTRING, 0, (LPARAM)L"システム全体（既定の出力デバイス）");
    SendDlgItemMessageW(g_dlg, IDC_AUDIOSRC, CB_ADDSTRING, 0, (LPARAM)L"録音しない");
    int sel = g_cfg.audio ? 0 : 1;
    for (size_t i = 0; i < g_audio_apps.size(); i++) {
        const bool saved = !g_cfg.audio_proc.empty() && same_name(g_audio_apps[i], g_cfg.audio_proc);
        std::wstring label = L"アプリのみ: " + g_audio_apps[i] + (saved && !found ? L"（現在は音声なし）" : L"");
        SendDlgItemMessageW(g_dlg, IDC_AUDIOSRC, CB_ADDSTRING, 0, (LPARAM)label.c_str());
        if (saved && g_cfg.audio) sel = (int)i + 2;
    }
    SendDlgItemMessageW(g_dlg, IDC_AUDIOSRC, CB_SETCURSEL, sel, 0);
    g_filling = false;
}

static void fill_gain() {
    g_filling = true;
    SendDlgItemMessageW(g_dlg, IDC_AGAIN_SPIN, UDM_SETRANGE32, kGainMin, kGainMax);
    SendDlgItemMessageW(g_dlg, IDC_AGAIN_SPIN, UDM_SETPOS32, 0, g_cfg.again);
    g_filling = false;
}

// like the quality field: take valid values while typing, clamp when leaving the field
static void on_gain_edited(bool leaving) {
    BOOL ok = FALSE;
    int v = (int)GetDlgItemInt(g_dlg, IDC_AGAIN, &ok, TRUE);
    if (ok && (leaving || (v >= kGainMin && v <= kGainMax))) {
        g_cfg.again = std::min(std::max(v, kGainMin), kGainMax);
        save_settings();
    }
    if (leaving) fill_gain();
}

static void on_audio_changed() {
    int s = cb_sel(IDC_AUDIOSRC);
    if (s == 0) { g_cfg.audio = 1; g_cfg.audio_proc.clear(); }
    else if (s == 1) { g_cfg.audio = 0; }
    else if (s >= 2 && s - 2 < (int)g_audio_apps.size()) { g_cfg.audio = 1; g_cfg.audio_proc = g_audio_apps[s - 2]; }
    save_settings();
}

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------
static void tray(DWORD op) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_dlg;
    nid.uID = 1;
    if (op == NIM_DELETE) { Shell_NotifyIconW(NIM_DELETE, &nid); return; }
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    int ic = g_state == State::Recording ? 2 : g_state == State::Idle ? 0 : 1;
    nid.hIcon = g_icon_small[ic];
    std::wstring tip;
    switch (g_state) {
    case State::Idle:      tip = L"hdrrec - 待機中（" + hotkey_name(g_cfg.hk_vk, g_cfg.hk_mod) + L" で録画開始）"; break;
    case State::Starting:  tip = L"hdrrec - 開始中..."; break;
    case State::Recording: tip = L"hdrrec - 録画中 " + hms((long long)(GetTickCount64() - g_rec_tick) / 1000); break;
    case State::Stopping:  tip = L"hdrrec - 停止処理中..."; break;
    }
    wcsncpy(nid.szTip, tip.c_str(), ARRAYSIZE(nid.szTip) - 1);
    if (op == NIM_ADD) {
        if (!Shell_NotifyIconW(NIM_ADD, &nid)) Shell_NotifyIconW(NIM_MODIFY, &nid);
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

static void set_last(Kind k, const std::wstring &text) {
    g_last_kind = k;
    SetDlgItemTextW(g_dlg, IDC_LAST, text.c_str());
    InvalidateRect(GetDlgItem(g_dlg, IDC_LAST), nullptr, TRUE);
}

static void refresh_monitor() {
    std::wstring s = monitor_text(g_cfg.monitor, &g_hdr_on);
    if (g_state == State::Idle && s != dlg_text(IDC_STATS)) {
        SetDlgItemTextW(g_dlg, IDC_STATS, s.c_str());
        InvalidateRect(GetDlgItem(g_dlg, IDC_STATS), nullptr, TRUE);
    }
}

static void update_ui() {
    std::wstring st;
    switch (g_state) {
    case State::Idle:      st = L"● 待機中"; break;
    case State::Starting:  st = L"● 開始中..."; break;
    case State::Recording: st = L"● 録画中  " + hms((long long)(GetTickCount64() - g_rec_tick) / 1000); break;
    case State::Stopping:  st = g_exit_after_stop ? L"● 停止処理中（完了後に終了）" : L"● 停止処理中..."; break;
    }
    if (st != dlg_text(IDC_STATE)) {
        SetDlgItemTextW(g_dlg, IDC_STATE, st.c_str());
        InvalidateRect(GetDlgItem(g_dlg, IDC_STATE), nullptr, TRUE);
    }
    const bool rec = g_state == State::Recording || g_state == State::Starting;
    SetDlgItemTextW(g_dlg, IDC_TOGGLE, rec ? L"録画停止" : L"録画開始");
    EnableWindow(GetDlgItem(g_dlg, IDC_TOGGLE), g_state != State::Stopping);
    // recording settings are fixed while hdrrec runs
    for (int id : {IDC_MONITOR, IDC_CODEC, IDC_PRESET, IDC_RCMODE, IDC_QVALUE, IDC_QSPIN, IDC_AUDIOSRC, IDC_AGAIN, IDC_AGAIN_SPIN, IDC_GPUPRIO, IDC_FPS})
        EnableWindow(GetDlgItem(g_dlg, id), g_state == State::Idle);
    int ic = g_state == State::Recording ? 2 : g_state == State::Idle ? 0 : 1;
    SendMessageW(g_dlg, WM_SETICON, ICON_SMALL, (LPARAM)g_icon_small[ic]);
    SendMessageW(g_dlg, WM_SETICON, ICON_BIG, (LPARAM)g_icon_big[ic]);
    if (g_state == State::Idle) refresh_monitor();
    tray(NIM_MODIFY);
}

static void show_window() {
    if (IsIconic(g_dlg)) ShowWindow(g_dlg, SW_RESTORE);
    else ShowWindow(g_dlg, SW_SHOW);
    SetForegroundWindow(g_dlg);
}

// ---------------------------------------------------------------------------
// recording process
// ---------------------------------------------------------------------------
static void reader_thread(HANDLE out_r, HANDLE log, HANDLE proc) {
    std::string line;
    char buf[4096];
    DWORD n = 0;
    auto post = [&] {
        if (line.empty()) return;
        std::string *s = new std::string(line);
        if (!PostMessageW(g_dlg, WM_APP_LINE, 0, (LPARAM)s)) delete s;
        line.clear();
    };
    while (ReadFile(out_r, buf, sizeof(buf), &n, nullptr) && n) {
        DWORD w;
        if (log != INVALID_HANDLE_VALUE) WriteFile(log, buf, n, &w, nullptr);
        for (DWORD i = 0; i < n; i++) {
            if (buf[i] == '\r' || buf[i] == '\n') post();
            else line += buf[i];
        }
    }
    post();
    CloseHandle(out_r);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    WaitForSingleObject(proc, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(proc, &code);
    PostMessageW(g_dlg, WM_APP_EXIT, code, 0);
}

static std::wstring quote(const std::wstring &s) { return L"\"" + s + L"\""; }

static void fail_start(const std::wstring &msg) {
    set_last(Kind::Error, L"エラー: " + msg);
    show_window();
}

static void start_recording() {
    if (g_state != State::Idle) return;

    g_cfg.out_dir = clean_dir(dlg_text(IDC_FOLDER));
    SetDlgItemTextW(g_dlg, IDC_FOLDER, g_cfg.out_dir.c_str());
    ini_set(L"OutputDir", g_cfg.out_dir);
    if (g_cfg.out_dir.empty()) return fail_start(L"保存先が指定されていません");
    int r = SHCreateDirectoryExW(nullptr, g_cfg.out_dir.c_str(), nullptr);
    if (r != ERROR_SUCCESS && r != ERROR_ALREADY_EXISTS && r != ERROR_FILE_EXISTS)
        return fail_start(L"保存先フォルダを作成できません");

    refresh_monitor();
    if (!g_hdr_on) return fail_start(L"録画するモニターの HDR がオフです");

    const std::wstring engine = g_exe_dir + L"\\hdrrec.exe";
    if (!file_exists(engine)) return fail_start(L"hdrrec.exe が見つかりません（" + g_exe_dir + L"）");

    // output name from the pattern (strftime), characters invalid in file names replaced
    time_t t = time(nullptr);
    struct tm lt;
    localtime_s(&lt, &t);
    wchar_t nb[256] = {0};
    if (!wcsftime(nb, 256, g_cfg.pattern.c_str(), &lt)) wcsftime(nb, 256, L"rec_%Y%m%d_%H%M%S", &lt);
    std::wstring base = nb;
    for (wchar_t &c : base) if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    std::wstring path = g_cfg.out_dir + L"\\" + base + L".mkv";
    for (int i = 2; file_exists(path); i++) path = g_cfg.out_dir + L"\\" + base + L"_" + std::to_wstring(i) + L".mkv";

    std::wstring cmd = quote(engine) + L" -o " + quote(path) +
                       L" -monitor " + std::to_wstring(g_cfg.monitor) +
                       L" -fps " + std::to_wstring(g_cfg.fps) +
                       L" -codec " + g_cfg.codec + L" -preset " + g_cfg.preset +
                       (is_bitrate() ? L" -bitrate " + std::to_wstring(g_cfg.bitrate)
                                     : L" -cq " + std::to_wstring(is_av1() ? g_cfg.cq_av1 : g_cfg.cq)) +
                       L" -mdl " + std::to_wstring(g_cfg.mdl) + L" -maxcll " + std::to_wstring(g_cfg.maxcll) +
                       L" -maxfall " + std::to_wstring(g_cfg.maxfall) +
                       L" -audio " + std::to_wstring(g_cfg.audio) + L" -acodec " + g_cfg.acodec +
                       L" -abitrate " + std::to_wstring(g_cfg.abitrate);
    if (g_cfg.audio && !g_cfg.audio_proc.empty()) cmd += L" -audio-process " + quote(g_cfg.audio_proc);
    if (g_cfg.audio && g_cfg.again) cmd += L" -again " + std::to_wstring(g_cfg.again);
    if (g_cfg.gpuprio) cmd += L" -gpuprio " + std::to_wstring(g_cfg.gpuprio);

    // per-recording log (everything hdrrec prints) and capture time log (tools/analyze_timelog.py)
    const std::wstring log_dir = g_exe_dir + L"\\logs";
    CreateDirectoryW(log_dir.c_str(), nullptr);
    const std::wstring rec_name = file_name(path).substr(0, file_name(path).size() - 4);
    const std::wstring log_name = rec_name + L".log";
    if (g_cfg.timelog) cmd += L" -timelog " + quote(log_dir + L"\\" + rec_name + L"_times.csv");
    if (!g_cfg.extra.empty()) cmd += L" " + g_cfg.extra;
    HANDLE log = CreateFileW((log_dir + L"\\" + log_name).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log != INVALID_HANDLE_VALUE) {
        std::string head = "command: " + narrow(cmd) + "\r\n";
        DWORD w;
        WriteFile(log, head.data(), (DWORD)head.size(), &w, nullptr);
    }

    // stdin: newline = stop; stdout + stderr: status / summary. The child gets only its ends.
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr;
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 1 << 20)) {
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        return fail_start(L"パイプを作成できません");
    }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = out_w;
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(engine.c_str(), &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                             g_exe_dir.c_str(), &si, &pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (!ok) {
        CloseHandle(in_w);
        CloseHandle(out_r);
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        return fail_start(fmt(L"hdrrec.exe を起動できません (%lu)", GetLastError()));
    }
    CloseHandle(pi.hThread);
    g_proc = pi.hProcess;
    g_stdin = in_w;
    g_cur_file = path;
    g_sum = Summary();
    g_live_unique = g_live_dup = g_live_missed = g_live_ovf = 0;
    g_press_tick = GetTickCount64();
    g_rec_tick = 0;
    std::thread(reader_thread, out_r, log, g_proc).detach();

    g_state = State::Starting;
    SetDlgItemTextW(g_dlg, IDC_STATS, L"");
    update_ui();
}

static void stop_recording() {
    if (g_state != State::Recording && g_state != State::Starting) return;
    if (g_stdin) {
        DWORD w;
        WriteFile(g_stdin, "\n", 1, &w, nullptr);
        CloseHandle(g_stdin);
        g_stdin = nullptr;
    }
    g_state = State::Stopping;
    update_ui();
}

static void toggle_recording() {
    if (g_state == State::Idle) start_recording();
    else stop_recording();
}

static bool starts(const std::string &s, const char *p) { return s.compare(0, strlen(p), p) == 0; }
static long long after_colon(const std::string &s) {
    size_t p = s.find(':');
    return p == std::string::npos ? 0 : atoll(s.c_str() + p + 1);
}

static void on_engine_line(const std::string &s) {
    if (starts(s, " time=")) {
        double t;
        long long out, uniq, dup, src, miss, drop, missed;
        if (sscanf(s.c_str(), " time=%lfs out=%lld unique=%lld dup=%lld(src %lld / miss %lld) drop=%lld missed=%lld",
                   &t, &out, &uniq, &dup, &src, &miss, &drop, &missed) == 8) {
            // "miss" = output frames repeated because the capture loop missed a desktop frame.
            // (missed desktop frames alone are harmless when the source runs faster than -fps)
            g_live_unique = uniq;
            g_live_dup = dup;
            g_live_missed = miss;
            size_t p = s.find("ovf=");
            g_live_ovf = p == std::string::npos ? 0 : atoll(s.c_str() + p + 4);
            std::wstring st = fmt(L"unique %lld / dup %lld / miss %lld", uniq, dup, miss);
            if (g_live_ovf) st += fmt(L" / ovf %lld", g_live_ovf);
            SetDlgItemTextW(g_dlg, IDC_STATS, st.c_str());
        }
    } else if (starts(s, "recording to ")) {
        g_rec_tick = GetTickCount64();
        if (g_state == State::Starting) g_state = State::Recording;
        SetDlgItemTextW(g_dlg, IDC_STATS, fmt(L"起動 %.2f 秒（hdrrec 初期化 %.0f ms）",
                                              (g_rec_tick - g_press_tick) / 1000.0, g_sum.init_ms).c_str());
        update_ui();
    } else if (starts(s, "init: ")) {
        sscanf(s.c_str(), "init: %lf", &g_sum.init_ms);
    } else if (starts(s, "done: ")) {
        g_sum.frames = atoll(s.c_str() + 6);
    } else if (starts(s, "  unique captures")) {
        g_sum.unique = after_colon(s);
    } else if (starts(s, "  repeated frames")) {
        g_sum.repeated = after_colon(s);
        size_t p = s.find("capture missed:");
        if (p != std::string::npos) g_sum.capture_missed = atoll(s.c_str() + p + 15);
    } else if (starts(s, "  missed desktop frames")) {
        g_sum.missed = after_colon(s);
    } else if (starts(s, "  encoder queue")) {
        size_t p = s.find("overflow ");
        if (p != std::string::npos) g_sum.overflow = atoll(s.c_str() + p + 9);
    } else if (starts(s, "warning: gpu priority")) {
        g_sum.gpu_warning = true;
    } else if (starts(s, "warning: audio process")) {
        g_sum.audio_fallback = true;
        set_last(Kind::Warn, L"注意: " + g_cfg.audio_proc + L" が起動していないため、システム全体の音声を録音しています");
    } else if (s.find("ERROR") != std::string::npos && g_sum.error.empty()) {
        size_t p = s.find("ERROR");
        g_sum.error = s.substr(p);
    }
}

static void on_engine_exit(DWORD code) {
    if (g_proc) { CloseHandle(g_proc); g_proc = nullptr; }
    if (g_stdin) { CloseHandle(g_stdin); g_stdin = nullptr; }
    g_state = State::Idle;

    const std::wstring name = file_name(g_cur_file);
    if (code != 0 || g_sum.frames < 0) {
        std::wstring why = g_sum.error.empty() ? fmt(L"hdrrec が異常終了しました (exit %lu)", code) : widen(g_sum.error);
        set_last(Kind::Error, L"エラー: " + why + L"（logs フォルダ参照）");
        if (file_exists(g_cur_file)) g_last_file = g_cur_file;
        show_window();
    } else {
        g_last_file = g_cur_file;
        std::wstring t = L"前回: " + name + L"  " + hms(g_sum.frames / g_cfg.fps);
        if (g_sum.capture_missed || g_sum.overflow || g_sum.audio_fallback || g_sum.gpu_warning) {
            if (g_sum.capture_missed || g_sum.overflow)
                t += fmt(L"  取りこぼし %lld / ovf %lld", g_sum.capture_missed, g_sum.overflow);
            if (g_sum.audio_fallback) t += L"  音声はシステム全体（アプリ未検出）";
            if (g_sum.gpu_warning) t += L"  GPU 優先度の一部を適用できず（ログ参照）";
            set_last(Kind::Warn, t);
        } else {
            set_last(Kind::Good, t + L"  問題なし");
        }
    }
    update_ui();
    if (g_exit_after_stop) DestroyWindow(g_dlg);
}

// ---------------------------------------------------------------------------
// folder
// ---------------------------------------------------------------------------
static void browse_folder() {
    IFileOpenDialog *fd = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fd)))) return;
    FILEOPENDIALOGOPTIONS o = 0;
    fd->GetOptions(&o);
    fd->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    fd->SetTitle(L"録画の保存先フォルダ");
    IShellItem *init = nullptr;
    std::wstring cur = clean_dir(dlg_text(IDC_FOLDER));
    if (!cur.empty() && SUCCEEDED(SHCreateItemFromParsingName(cur.c_str(), nullptr, IID_PPV_ARGS(&init)))) {
        fd->SetFolder(init);
        init->Release();
    }
    if (fd->Show(g_dlg) == S_OK) {
        IShellItem *it = nullptr;
        if (SUCCEEDED(fd->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                g_cfg.out_dir = clean_dir(p);
                SetDlgItemTextW(g_dlg, IDC_FOLDER, g_cfg.out_dir.c_str());
                ini_set(L"OutputDir", g_cfg.out_dir);
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    fd->Release();
}

static void open_folder() {
    if (!g_last_file.empty() && file_exists(g_last_file)) {   // select the last recording
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(g_last_file.c_str());
        if (pidl) {
            SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
            ILFree(pidl);
            return;
        }
    }
    std::wstring dir = clean_dir(dlg_text(IDC_FOLDER));
    if (dir.empty()) return;
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    ShellExecuteW(g_dlg, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// window
// ---------------------------------------------------------------------------
static void make_fonts() {
    HFONT f = (HFONT)SendMessageW(g_dlg, WM_GETFONT, 0, 0);
    LOGFONTW lf = {};
    GetObjectW(f, sizeof(lf), &lf);
    lf.lfHeight = lf.lfHeight * 3 / 2;
    lf.lfWeight = FW_BOLD;
    HFONT nf = CreateFontIndirectW(&lf);
    SendDlgItemMessageW(g_dlg, IDC_STATE, WM_SETFONT, (WPARAM)nf, TRUE);
    if (g_big_font) DeleteObject(g_big_font);
    g_big_font = nf;
}

static void tray_menu(int x, int y) {
    const std::wstring hk = L"\t" + hotkey_name(g_cfg.hk_vk, g_cfg.hk_mod);
    const bool rec = g_state == State::Recording || g_state == State::Starting;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_state == State::Stopping ? MF_GRAYED : 0), IDM_TOGGLE,
                ((rec ? L"録画停止" : L"録画開始") + hk).c_str());
    AppendMenuW(m, MF_STRING, IDM_SHOW, L"ウィンドウを表示");
    AppendMenuW(m, MF_STRING, IDM_OPEN, L"保存先を開く");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"終了");
    SetMenuDefaultItem(m, IDM_SHOW, FALSE);
    SetForegroundWindow(g_dlg);
    TrackPopupMenuEx(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, g_dlg, nullptr);
    PostMessageW(g_dlg, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void request_exit() {
    if (g_state == State::Idle) { DestroyWindow(g_dlg); return; }
    g_exit_after_stop = true;
    if (g_state == State::Recording || g_state == State::Starting) stop_recording();
    update_ui();
}

static INT_PTR CALLBACK dlg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_msg_taskbar && g_msg_taskbar) { tray(NIM_ADD); return TRUE; }   // explorer restarted
    if (msg == g_msg_show && g_msg_show) { show_window(); return TRUE; }         // second instance started

    switch (msg) {
    case WM_INITDIALOG:
        g_dlg = h;
        make_icons();
        make_fonts();
        SendDlgItemMessageW(h, IDC_HOTKEY, HKM_SETHOTKEY, to_hotkey_ctrl(g_cfg.hk_vk, g_cfg.hk_mod), 0);
        SetDlgItemTextW(h, IDC_FOLDER, g_cfg.out_dir.c_str());
        fill_monitors();
        fill_fps();
        fill_encoder();
        fill_audio();
        fill_gain();
        apply_hotkey();
        tray(NIM_ADD);
        update_ui();
        SetTimer(h, kTimerId, 250, nullptr);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_TOGGLE: case IDM_TOGGLE: toggle_recording(); break;
        case IDC_BROWSE: browse_folder(); break;
        case IDC_OPEN: case IDM_OPEN: open_folder(); break;
        case IDM_SHOW: show_window(); break;
        case IDM_EXIT: request_exit(); break;
        case IDCANCEL: ShowWindow(h, SW_HIDE); break;          // Esc
        case IDC_HOTKEY:
            if (HIWORD(wp) == EN_CHANGE && !g_filling) on_hotkey_edited();
            break;
        case IDC_HOTKEY_RESET: reset_hotkey(); break;
        case IDC_MONITOR:
            if (HIWORD(wp) == CBN_DROPDOWN && !g_filling) fill_monitors();   // HDR may have been switched
            if (HIWORD(wp) == CBN_SELCHANGE && !g_filling) { on_setting_changed(IDC_MONITOR); refresh_monitor(); }
            break;
        case IDC_AUDIOSRC:
            if (HIWORD(wp) == CBN_DROPDOWN && !g_filling) fill_audio();   // list applications playing sound now
            if (HIWORD(wp) == CBN_SELCHANGE && !g_filling) on_audio_changed();
            break;
        case IDC_CODEC: case IDC_PRESET: case IDC_RCMODE: case IDC_GPUPRIO: case IDC_FPS:
            if (HIWORD(wp) == CBN_SELCHANGE && !g_filling) on_setting_changed(LOWORD(wp));
            break;
        case IDC_QVALUE:
            if ((HIWORD(wp) == EN_CHANGE || HIWORD(wp) == EN_KILLFOCUS) && !g_filling)
                on_quality_edited(HIWORD(wp) == EN_KILLFOCUS);
            break;
        case IDC_AGAIN:
            if ((HIWORD(wp) == EN_CHANGE || HIWORD(wp) == EN_KILLFOCUS) && !g_filling)
                on_gain_edited(HIWORD(wp) == EN_KILLFOCUS);
            break;
        case IDC_FOLDER:
            if (HIWORD(wp) == EN_KILLFOCUS) {
                g_cfg.out_dir = clean_dir(dlg_text(IDC_FOLDER));
                ini_set(L"OutputDir", g_cfg.out_dir);
            }
            break;
        }
        return TRUE;

    case WM_HOTKEY:
    case WM_APP_LLHOTKEY:
        if (msg == WM_HOTKEY && wp != kHotkeyId) return TRUE;
        // pressing the current combination inside the hotkey field must not start a recording
        if (GetForegroundWindow() == h && GetFocus() == GetDlgItem(h, IDC_HOTKEY)) return TRUE;
        toggle_recording();
        return TRUE;

    case WM_APP_LINE: {
        std::string *s = (std::string *)lp;
        on_engine_line(*s);
        delete s;
        return TRUE;
    }
    case WM_APP_EXIT:
        on_engine_exit((DWORD)wp);
        return TRUE;

    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONDBLCLK: show_window(); break;
        case WM_CONTEXTMENU: tray_menu((short)LOWORD(wp), (short)HIWORD(wp)); break;
        }
        return TRUE;

    case WM_TIMER:
        if (g_state == State::Recording) {
            update_ui();
        } else if (g_state == State::Idle && IsWindowVisible(h) && ++g_idle_ticks % 8 == 0) {
            refresh_monitor();   // HDR may be switched on/off at any time
        }
        return TRUE;

    case WM_DISPLAYCHANGE:
        fill_monitors();
        if (g_state == State::Idle) refresh_monitor();
        return FALSE;

    case WM_DPICHANGED:
        PostMessageW(h, WM_APP_DPI, 0, 0);
        return FALSE;   // let the dialog manager rescale the layout
    case WM_APP_DPI:
        make_fonts();
        make_icons();
        update_ui();
        return TRUE;

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        COLORREF c = GetSysColor(COLOR_WINDOWTEXT);
        bool custom = true;
        switch (GetDlgCtrlID((HWND)lp)) {
        case IDC_STATE:
            c = g_state == State::Recording ? RGB(210, 20, 30) : g_state == State::Idle ? RGB(110, 110, 110) : RGB(200, 120, 0);
            break;
        case IDC_STATS:
            if (g_state == State::Idle && !g_hdr_on) c = RGB(200, 120, 0);
            else if (g_state == State::Recording && (g_live_missed || g_live_ovf)) c = RGB(200, 120, 0);
            break;
        case IDC_LAST:
            c = g_last_kind == Kind::Error ? RGB(210, 20, 30) : g_last_kind == Kind::Warn ? RGB(200, 120, 0)
              : g_last_kind == Kind::Good ? RGB(0, 128, 60) : c;
            break;
        case IDC_HOTKEY_NOTE:
            if (g_hotkey_err) c = RGB(210, 20, 30);
            break;
        default:
            custom = false;
        }
        if (!custom) return FALSE;
        SetTextColor(dc, c);
        SetBkMode(dc, TRANSPARENT);
        return (INT_PTR)GetSysColorBrush(COLOR_BTNFACE);
    }

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE) {   // minimize = stay in the notification area only
            ShowWindow(h, SW_HIDE);
            return TRUE;
        }
        return FALSE;

    case WM_CLOSE:
        request_exit();           // exit; a running recording is stopped and finalized first
        return TRUE;

    case WM_QUERYENDSESSION:
        if (g_state == State::Recording || g_state == State::Starting) stop_recording();
        SetWindowLongPtrW(h, DWLP_MSGRESULT, TRUE);
        return TRUE;
    case WM_ENDSESSION:
        if (wp && g_proc) WaitForSingleObject(g_proc, 15000);   // let hdrrec finalize the file
        return TRUE;

    case WM_DESTROY:
        KillTimer(h, kTimerId);
        tray(NIM_DELETE);
        UnregisterHotKey(h, kHotkeyId);
        if (g_llhook) UnhookWindowsHookEx(g_llhook);
        PostQuitMessage(0);
        return TRUE;
    }
    return FALSE;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdline, int) {
    g_inst = inst;
    g_msg_taskbar = RegisterWindowMessageW(L"TaskbarCreated");
    g_msg_show = RegisterWindowMessageW(L"hdrrec_gui_show");

#ifndef HDRREC_GUI_MUTEX   // a test build can use another name to run next to the installed GUI
#define HDRREC_GUI_MUTEX L"Local\\hdrrec_gui_single_instance"
#endif
    HANDLE single = CreateMutexW(nullptr, TRUE, HDRREC_GUI_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        PostMessageW(HWND_BROADCAST, g_msg_show, 0, 0);
        return 0;
    }

    wchar_t exe[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    g_exe_dir = exe;
    g_exe_dir = g_exe_dir.substr(0, g_exe_dir.find_last_of(L'\\'));
    g_ini = g_exe_dir + L"\\hdrrec_gui.ini";

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_HOTKEY_CLASS | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    load_settings();

    if (!CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), nullptr, dlg_proc, 0)) return 1;
    if (!wcsstr(cmdline, L"/tray")) show_window();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_dlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    CoUninitialize();
    CloseHandle(single);
    return 0;
}

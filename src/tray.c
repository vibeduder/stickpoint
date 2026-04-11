/*
 * StickPoint — system tray icon and main status window
 *
 * UI design:
 *   - Left-click tray icon toggles a centered 480×N window.
 *   - For each connected XInput slot a row is shown:
 *       [48×48 controller image][controller name][mode toggle button][options button]
 *   - When no controllers are connected a "No controllers connected." label is shown.
 *   - The Options button opens a per-slot dialog for re-mapping the mode-switch combo.
 *   - PNG images are loaded via WIC (Windows Imaging Component).
 *
 * Change-detection note
 * ─────────────────────
 * main.c calls tray_get_infos() to get a pointer to s_last_infos[], then passes
 * that same pointer to tray_update_gamepads().  Comparing s_last_infos[i] against
 * infos[i] would therefore always be equal.  Instead we track what the UI currently
 * shows in two separate arrays — s_row_shown[] and s_shown_mode[] — and diff against
 * those.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>       /* IWICImagingFactory   — link windowscodecs.lib */
#include <wingdi.h>         /* AlphaBlend           — link msimg32.lib       */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "tray.h"
#include "gamepad.h"

/* ---- constants ----------------------------------------------------------- */

#define WM_TRAYICON         (WM_APP + 1)
#define IDM_EXIT            100

/* "No controllers" placeholder label. */
#define IDC_EMPTY           900

/* Per-row control IDs: slot N → base 1000 + N*10 */
#define ROW_IMG_ID(s)       (1000 + (s) * 10)   /* STATIC SS_OWNERDRAW */
#define ROW_NAME_ID(s)      (1001 + (s) * 10)   /* STATIC text         */
#define ROW_MODE_ID(s)      (1002 + (s) * 10)   /* BUTTON mode toggle  */
#define ROW_OPT_ID(s)       (1003 + (s) * 10)   /* BUTTON options      */

/* Options dialog controls. */
#define IDC_OPT_TITLE       2000
#define IDC_OPT_HOLD_LBL    2001
#define IDC_OPT_HOLD_CB     2002
#define IDC_OPT_PRESS_LBL   2003
#define IDC_OPT_PRESS_CB    2004
#define IDC_OPT_TIMEOUT_LBL 2005
#define IDC_OPT_TIMEOUT_ED  2006
#define IDC_OPT_OK          2007
#define IDC_OPT_CANCEL      2008

/* Main window geometry (client-area width fixed at WIN_W). */
#define WIN_W       480
#define WIN_H_EMPTY 100     /* height when no controllers are connected      */
#define ROW_H       68      /* height per controller row                     */
#define ROW_PAD_TOP 8       /* top padding above the first row               */
#define ROW_PAD_BOT 8       /* bottom padding below the last row             */

/* Row element geometry (y relative to the row's top). */
#define IMG_X       8
#define IMG_Y       10
#define IMG_W       48
#define IMG_H       48

#define NAME_X      66
#define NAME_Y      14
#define NAME_W      172
#define NAME_H      20

#define MODE_X      246
#define MODE_Y      18
#define MODE_W      100
#define MODE_H      30

#define OPT_X       354
#define OPT_Y       18
#define OPT_W       100
#define OPT_H       30

/* ---- private state ------------------------------------------------------- */

static NOTIFYICONDATAA     s_nid         = {0};
static HWND                s_hwnd_msg    = NULL;
static HWND                s_hwnd_pop    = NULL;   /* main status window    */
static HWND                s_hwnd_opts   = NULL;   /* options dialog window */
static HINSTANCE           s_hInst       = NULL;
static bool                s_pop_vis     = false;
static bool                s_initialized = false;

/*
 * s_last_infos: live array shared with main.c via tray_get_infos().
 * DO NOT use s_last_infos[i].connected or .slot_state.mode for change
 * detection — it is the same memory that the caller mutates before
 * passing to tray_update_gamepads().  Use s_row_shown / s_shown_mode.
 */
static GamepadInfo s_last_infos[XUSER_MAX_COUNT];

/*
 * UI-state tracking (what the window is actually showing right now).
 * These are updated only when the UI is mutated, not on every poll.
 */
static bool    s_row_shown[XUSER_MAX_COUNT];
static AppMode s_shown_mode[XUSER_MAX_COUNT];
static char    s_shown_name[XUSER_MAX_COUNT][256];
static wchar_t s_shown_image[XUSER_MAX_COUNT][MAX_PATH];
static HBITMAP s_bitmaps[XUSER_MAX_COUNT];

static IWICImagingFactory *s_wic         = NULL;
static int                 s_options_slot = -1;

/* ---- button name/value table (options dropdowns) ------------------------- */

static const struct { const char *name; WORD value; } s_btn_names[] = {
    {"Guide",  XINPUT_GAMEPAD_GUIDE},
    {"A",      XINPUT_GAMEPAD_A},
    {"B",      XINPUT_GAMEPAD_B},
    {"X",      XINPUT_GAMEPAD_X},
    {"Y",      XINPUT_GAMEPAD_Y},
    {"LB",     XINPUT_GAMEPAD_LEFT_SHOULDER},
    {"RB",     XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {"Start",  XINPUT_GAMEPAD_START},
    {"Back",   XINPUT_GAMEPAD_BACK},
    {"LS",     XINPUT_GAMEPAD_LEFT_THUMB},
    {"RS",     XINPUT_GAMEPAD_RIGHT_THUMB},
};
#define BTN_COUNT ((int)(sizeof(s_btn_names) / sizeof(s_btn_names[0])))

/* ---- forward declarations ------------------------------------------------ */

static LRESULT CALLBACK PopupWndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK OptionsWndProc(HWND, UINT, WPARAM, LPARAM);
static void create_row_controls(int slot);
static void destroy_row_controls(int slot);
static void update_row_mode_btn(int slot, AppMode mode);
static void recalc_window_size(void);
static void set_tooltip_summary(void);
static HBITMAP load_png_hbitmap(const wchar_t *path);
static void show_options_for(int slot);

/* ---- WIC PNG loading ----------------------------------------------------- */

static bool init_wic(void)
{
    HRESULT hr = CoCreateInstance(
        &CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
        &IID_IWICImagingFactory, (void **)&s_wic);
    return SUCCEEDED(hr);
}

static HBITMAP load_png_hbitmap(const wchar_t *path)
{
    if (!s_wic || !path || path[0] == L'\0')
        return NULL;

    HBITMAP hbmp = NULL;

    IWICBitmapDecoder *decoder = NULL;
    HRESULT hr = s_wic->lpVtbl->CreateDecoderFromFilename(
        s_wic, path, NULL, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr))
        return NULL;

    IWICBitmapFrameDecode *frame = NULL;
    hr = decoder->lpVtbl->GetFrame(decoder, 0, &frame);
    decoder->lpVtbl->Release(decoder);
    if (FAILED(hr))
        return NULL;

    /* Convert to 32-bpp pre-multiplied BGRA. */
    IWICFormatConverter *conv = NULL;
    hr = s_wic->lpVtbl->CreateFormatConverter(s_wic, &conv);
    if (SUCCEEDED(hr)) {
        hr = conv->lpVtbl->Initialize(conv,
            (IWICBitmapSource *)frame,
            &GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, NULL, 0.0,
            WICBitmapPaletteTypeCustom);
    }
    frame->lpVtbl->Release(frame);
    if (FAILED(hr)) {
        if (conv) conv->lpVtbl->Release(conv);
        return NULL;
    }

    /* Scale to IMG_W × IMG_H. */
    IWICBitmapScaler *scaler = NULL;
    hr = s_wic->lpVtbl->CreateBitmapScaler(s_wic, &scaler);
    if (SUCCEEDED(hr)) {
        hr = scaler->lpVtbl->Initialize(scaler,
            (IWICBitmapSource *)conv,
            (UINT)IMG_W, (UINT)IMG_H,
            WICBitmapInterpolationModeHighQualityCubic);
    }
    conv->lpVtbl->Release(conv);
    if (FAILED(hr)) {
        if (scaler) scaler->lpVtbl->Release(scaler);
        return NULL;
    }

    /* Copy pixels into a top-down 32-bpp DIB section. */
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = IMG_W;
    bmi.bmiHeader.biHeight      = -IMG_H;  /* negative = top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    hbmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (hbmp && bits) {
        UINT stride = (UINT)IMG_W * 4;
        UINT size   = stride * (UINT)IMG_H;
        if (FAILED(scaler->lpVtbl->CopyPixels(scaler, NULL, stride, size,
                                              (BYTE *)bits))) {
            DeleteObject(hbmp);
            hbmp = NULL;
        }
    }
    scaler->lpVtbl->Release(scaler);
    return hbmp;
}

/* ---- Window helpers ------------------------------------------------------ */

/* Count rows currently visible in the UI (from s_row_shown, not live data). */
static int shown_count(void)
{
    int n = 0;
    for (int i = 0; i < XUSER_MAX_COUNT; i++)
        if (s_row_shown[i]) n++;
    return n;
}

static void center_window(HWND hwnd, int w, int h)
{
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hwnd, HWND_TOP, (sw - w) / 2, (sh - h) / 2, w, h, SWP_NOZORDER);
}

/* Resize the outer window to fit the current number of rows, then recenter. */
static void recalc_window_size(void)
{
    int count    = shown_count();
    int client_h = (count == 0)
        ? WIN_H_EMPTY
        : (ROW_PAD_TOP + count * ROW_H + ROW_PAD_BOT);

    RECT r = {0, 0, WIN_W, client_h};
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    int new_w = r.right  - r.left;
    int new_h = r.bottom - r.top;

    RECT cur;
    GetWindowRect(s_hwnd_pop, &cur);
    if ((cur.right - cur.left) != new_w || (cur.bottom - cur.top) != new_h)
        center_window(s_hwnd_pop, new_w, new_h);
}

/* Tray tooltip: one-line summary of connected controllers. */
static void set_tooltip_summary(void)
{
    int n = shown_count();
    if (n == 0) {
        _snprintf_s(s_nid.szTip, sizeof(s_nid.szTip), _TRUNCATE,
                    "StickPoint \xe2\x80\x94 No controllers");
    } else if (n == 1) {
        for (int i = 0; i < XUSER_MAX_COUNT; i++) {
            if (!s_row_shown[i]) continue;
            _snprintf_s(s_nid.szTip, sizeof(s_nid.szTip), _TRUNCATE,
                        "StickPoint \xe2\x80\x94 %s [%s]",
                        s_last_infos[i].name,
                        s_shown_mode[i] == MODE_MOUSE ? "Mouse" : "Gamepad");
            break;
        }
    } else {
        _snprintf_s(s_nid.szTip, sizeof(s_nid.szTip), _TRUNCATE,
                    "StickPoint \xe2\x80\x94 %d controllers", n);
    }
    s_nid.uFlags = NIF_TIP;
    Shell_NotifyIconA(NIM_MODIFY, &s_nid);
    s_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

/* ---- Row control management --------------------------------------------- */

/*
 * Return the visual row index (0-based position in the window) for a given
 * XInput slot.  Uses s_row_shown[] so it reflects the current UI state.
 */
static int visual_index_for_slot(int slot)
{
    int vi = 0;
    for (int i = 0; i < XUSER_MAX_COUNT; i++) {
        if (!s_row_shown[i]) continue;
        if (i == slot) return vi;
        vi++;
    }
    return -1;
}

/* Top of the Nth visual row in client coordinates. */
static int row_top(int visual_idx)
{
    return ROW_PAD_TOP + visual_idx * ROW_H;
}

static void create_row_controls(int slot)
{
    int vi = visual_index_for_slot(slot);
    if (vi < 0) return;
    int y0 = row_top(vi);

    CreateWindowExA(0, "STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
        IMG_X, y0 + IMG_Y, IMG_W, IMG_H,
        s_hwnd_pop, (HMENU)(UINT_PTR)ROW_IMG_ID(slot), s_hInst, NULL);

    CreateWindowExA(0, "STATIC", s_last_infos[slot].name,
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        NAME_X, y0 + NAME_Y, NAME_W, NAME_H,
        s_hwnd_pop, (HMENU)(UINT_PTR)ROW_NAME_ID(slot), s_hInst, NULL);

    const char *mode_lbl = (s_shown_mode[slot] == MODE_MOUSE) ? "Mouse" : "Gamepad";
    CreateWindowExA(0, "BUTTON", mode_lbl,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        MODE_X, y0 + MODE_Y, MODE_W, MODE_H,
        s_hwnd_pop, (HMENU)(UINT_PTR)ROW_MODE_ID(slot), s_hInst, NULL);

    CreateWindowExA(0, "BUTTON", "Options",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        OPT_X, y0 + OPT_Y, OPT_W, OPT_H,
        s_hwnd_pop, (HMENU)(UINT_PTR)ROW_OPT_ID(slot), s_hInst, NULL);
}

static void destroy_row_controls(int slot)
{
    HWND h;
    h = GetDlgItem(s_hwnd_pop, ROW_IMG_ID(slot));  if (h) DestroyWindow(h);
    h = GetDlgItem(s_hwnd_pop, ROW_NAME_ID(slot)); if (h) DestroyWindow(h);
    h = GetDlgItem(s_hwnd_pop, ROW_MODE_ID(slot)); if (h) DestroyWindow(h);
    h = GetDlgItem(s_hwnd_pop, ROW_OPT_ID(slot));  if (h) DestroyWindow(h);

    if (s_bitmaps[slot]) {
        DeleteObject(s_bitmaps[slot]);
        s_bitmaps[slot] = NULL;
    }
}

static void update_row_mode_btn(int slot, AppMode mode)
{
    HWND h = GetDlgItem(s_hwnd_pop, ROW_MODE_ID(slot));
    if (h) SetWindowTextA(h, (mode == MODE_MOUSE) ? "Mouse" : "Gamepad");
}

/*
 * After adding or removing a row, slide all existing controls to their
 * new y positions.  Must be called with s_row_shown[] already updated.
 */
static void reposition_all_rows(void)
{
    int vi = 0;
    for (int s = 0; s < XUSER_MAX_COUNT; s++) {
        if (!s_row_shown[s]) continue;
        int y0 = row_top(vi++);
        HWND h;
        h = GetDlgItem(s_hwnd_pop, ROW_IMG_ID(s));
        if (h) SetWindowPos(h, NULL, IMG_X,  y0 + IMG_Y,  0, 0, SWP_NOSIZE | SWP_NOZORDER);
        h = GetDlgItem(s_hwnd_pop, ROW_NAME_ID(s));
        if (h) SetWindowPos(h, NULL, NAME_X, y0 + NAME_Y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        h = GetDlgItem(s_hwnd_pop, ROW_MODE_ID(s));
        if (h) SetWindowPos(h, NULL, MODE_X, y0 + MODE_Y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        h = GetDlgItem(s_hwnd_pop, ROW_OPT_ID(s));
        if (h) SetWindowPos(h, NULL, OPT_X,  y0 + OPT_Y,  0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
}

/* ---- Empty-state label --------------------------------------------------- */

static void show_empty_label(bool show)
{
    HWND h = GetDlgItem(s_hwnd_pop, IDC_EMPTY);
    if (show) {
        if (!h) {
            RECT r;
            GetClientRect(s_hwnd_pop, &r);
            int lw = 260, lh = 20;
            CreateWindowExA(0, "STATIC", "No controllers connected.",
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                (r.right - lw) / 2, (r.bottom - lh) / 2, lw, lh,
                s_hwnd_pop, (HMENU)(UINT_PTR)IDC_EMPTY, s_hInst, NULL);
        } else {
            ShowWindow(h, SW_SHOW);
        }
    } else if (h) {
        ShowWindow(h, SW_HIDE);
    }
}

/* ---- Options window ------------------------------------------------------ */

static void populate_combo(HWND hcb, WORD selected_val)
{
    SendMessageA(hcb, CB_RESETCONTENT, 0, 0);
    int sel_idx = 0;
    for (int i = 0; i < BTN_COUNT; i++) {
        int pos = (int)SendMessageA(hcb, CB_ADDSTRING, 0,
                                    (LPARAM)s_btn_names[i].name);
        SendMessageA(hcb, CB_SETITEMDATA, (WPARAM)pos,
                     (LPARAM)s_btn_names[i].value);
        if (s_btn_names[i].value == selected_val)
            sel_idx = pos;
    }
    SendMessageA(hcb, CB_SETCURSEL, (WPARAM)sel_idx, 0);
}

static WORD combo_selected_value(HWND hcb)
{
    int idx = (int)SendMessageA(hcb, CB_GETCURSEL, 0, 0);
    if (idx == CB_ERR) return 0;
    return (WORD)SendMessageA(hcb, CB_GETITEMDATA, (WPARAM)idx, 0);
}

static LRESULT CALLBACK OptionsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_OPT_OK) {
            int slot = s_options_slot;
            if (slot >= 0 && slot < XUSER_MAX_COUNT) {
                GamepadSlotState *ss = &s_last_infos[slot].slot_state;
                WORD hold  = combo_selected_value(GetDlgItem(hwnd, IDC_OPT_HOLD_CB));
                WORD press = combo_selected_value(GetDlgItem(hwnd, IDC_OPT_PRESS_CB));
                char buf[16];
                GetWindowTextA(GetDlgItem(hwnd, IDC_OPT_TIMEOUT_ED), buf, sizeof(buf));
                DWORD ms = (DWORD)atoi(buf);
                if (ms < 100)  ms = 100;
                if (ms > 5000) ms = 5000;
                ss->combo_hold_btn   = hold;
                ss->combo_press_btn  = press;
                ss->combo_timeout_ms = ms;
                gamepad_save_config(s_last_infos);
            }
            /* fall through to close */
        }
        if (LOWORD(wp) == IDC_OPT_OK || LOWORD(wp) == IDC_OPT_CANCEL) {
            EnableWindow(s_hwnd_pop, TRUE);
            s_options_slot = -1;
            ShowWindow(hwnd, SW_HIDE);
        }
        return 0;

    case WM_CLOSE:
        EnableWindow(s_hwnd_pop, TRUE);
        s_options_slot = -1;
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void show_options_for(int slot)
{
    if (s_options_slot != -1) return;   /* already open */
    s_options_slot = slot;

    /* Update title label. */
    char title[320];
    _snprintf_s(title, sizeof(title), _TRUNCATE,
                "Options \xe2\x80\x94 %s", s_last_infos[slot].name);
    SetWindowTextA(s_hwnd_opts, title);

    /* Populate controls from current slot config. */
    const GamepadSlotState *ss = &s_last_infos[slot].slot_state;
    populate_combo(GetDlgItem(s_hwnd_opts, IDC_OPT_HOLD_CB),  ss->combo_hold_btn);
    populate_combo(GetDlgItem(s_hwnd_opts, IDC_OPT_PRESS_CB), ss->combo_press_btn);
    char buf[16];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%u", (unsigned)ss->combo_timeout_ms);
    SetWindowTextA(GetDlgItem(s_hwnd_opts, IDC_OPT_TIMEOUT_ED), buf);

    /* Center the options window over the main window. */
    RECT pr; GetWindowRect(s_hwnd_pop, &pr);
    RECT or_ = {0, 0, 370, 200};
    AdjustWindowRect(&or_, WS_CAPTION | WS_SYSMENU, FALSE);
    int ow = or_.right - or_.left, oh = or_.bottom - or_.top;
    SetWindowPos(s_hwnd_opts, HWND_TOP,
                 pr.left + (pr.right  - pr.left - ow) / 2,
                 pr.top  + (pr.bottom - pr.top  - oh) / 2,
                 ow, oh, 0);

    EnableWindow(s_hwnd_pop, FALSE);
    ShowWindow(s_hwnd_opts, SW_SHOW);
    SetForegroundWindow(s_hwnd_opts);
}

/* ---- Main window procedure ----------------------------------------------- */

static LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lp;
        int slot = ((int)dis->CtlID - 1000) / 10;
        if (slot >= 0 && slot < XUSER_MAX_COUNT && s_bitmaps[slot]) {
            HDC mdc = CreateCompatibleDC(dis->hDC);
            HGDIOBJ old = SelectObject(mdc, s_bitmaps[slot]);
            BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            AlphaBlend(dis->hDC,
                       dis->rcItem.left, dis->rcItem.top,
                       IMG_W, IMG_H,
                       mdc, 0, 0, IMG_W, IMG_H, bf);
            SelectObject(mdc, old);
            DeleteDC(mdc);
        } else {
            FillRect(dis->hDC, &dis->rcItem,
                     GetSysColorBrush(COLOR_BTNFACE));
        }
        return TRUE;
    }

    case WM_COMMAND: {
        int id = (int)LOWORD(wp);
        /* Mode toggle: ROW_MODE_ID(s) = 1002 + s*10 */
        if (id >= 1002 && (id - 1002) % 10 == 0) {
            int slot = (id - 1002) / 10;
            if (slot < XUSER_MAX_COUNT && s_row_shown[slot]) {
                AppMode next = (s_shown_mode[slot] == MODE_MOUSE)
                               ? MODE_NORMAL : MODE_MOUSE;
                s_last_infos[slot].slot_state.mode = next;
                s_shown_mode[slot] = next;
                update_row_mode_btn(slot, next);
                set_tooltip_summary();
            }
            return 0;
        }
        /* Options: ROW_OPT_ID(s) = 1003 + s*10 */
        if (id >= 1003 && (id - 1003) % 10 == 0) {
            int slot = (id - 1003) / 10;
            if (slot < XUSER_MAX_COUNT && s_row_shown[slot])
                show_options_for(slot);
            return 0;
        }
        return 0;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        s_pop_vis = false;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- Public API ---------------------------------------------------------- */

bool tray_init(HINSTANCE hInst, HWND hwnd_msg)
{
    s_hInst    = hInst;
    s_hwnd_msg = hwnd_msg;

    CoInitialize(NULL);
    init_wic();

    HICON hIconBig   = (HICON)LoadImageA(hInst, MAKEINTRESOURCEA(1),
                                          IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    HICON hIconSmall = (HICON)LoadImageA(hInst, MAKEINTRESOURCEA(1),
                                          IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    if (!hIconBig)   hIconBig   = LoadIconA(NULL, IDI_APPLICATION);
    if (!hIconSmall) hIconSmall = LoadIconA(NULL, IDI_APPLICATION);

    /* ---- Main window class ---- */
    {
        WNDCLASSEXA wc  = {0};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = PopupWndProc;
        wc.hInstance     = hInst;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hIcon         = hIconBig;
        wc.hIconSm       = hIconSmall;
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "StickPointMain";
        if (!RegisterClassExA(&wc)) return false;
    }

    RECT r = {0, 0, WIN_W, WIN_H_EMPTY};
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    int win_w = r.right - r.left, win_h = r.bottom - r.top;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);

    s_hwnd_pop = CreateWindowExA(
        0, "StickPointMain", "StickPoint",
        WS_CAPTION | WS_SYSMENU,
        (sw - win_w) / 2, (sh - win_h) / 2, win_w, win_h,
        NULL, NULL, hInst, NULL);
    if (!s_hwnd_pop) return false;

    /* ---- Options window class ---- */
    {
        WNDCLASSEXA wc  = {0};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = OptionsWndProc;
        wc.hInstance     = hInst;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "StickPointOptions";
        if (!RegisterClassExA(&wc)) return false;
    }

    RECT or_ = {0, 0, 370, 200};
    AdjustWindowRect(&or_, WS_CAPTION | WS_SYSMENU, FALSE);
    s_hwnd_opts = CreateWindowExA(
        0, "StickPointOptions", "Options",
        WS_CAPTION | WS_SYSMENU,
        0, 0, or_.right - or_.left, or_.bottom - or_.top,
        s_hwnd_pop, NULL, hInst, NULL);
    if (!s_hwnd_opts) return false;

    /* ---- Options dialog controls ---- */
    {
        int ox = 12, oy = 12;
        /* Informational label (controller name filled in show_options_for). */
        CreateWindowExA(0, "STATIC", "",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            ox, oy, 346, 18,
            s_hwnd_opts, (HMENU)(UINT_PTR)IDC_OPT_TITLE, hInst, NULL);
        oy += 30;

        CreateWindowExA(0, "STATIC", "Hold button:",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            ox, oy + 3, 106, 18,
            s_hwnd_opts, (HMENU)(UINT_PTR)IDC_OPT_HOLD_LBL, hInst, NULL);
        CreateWindowExA(0, "COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            ox + 112, oy, 230, 200,
            s_hwnd_opts, (HMENU)(UINT_PTR)IDC_OPT_HOLD_CB, hInst, NULL);
        oy += 34;

        CreateWindowExA(0, "STATIC", "Press button:",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            ox, oy + 3, 106, 18,
            s_hwnd_opts, (HMENU)(UINT_PTR)IDC_OPT_PRESS_LBL, hInst, NULL);
        CreateWindowExA(0, "COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            ox + 112, oy, 230, 200,
            s_hwnd_opts, (HMENU)(UINT_PTR)IDC_OPT_PRESS_CB, hInst, NULL);
        oy += 34;

        CreateWindowExA(0, "STATIC", "Timeout (ms):",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            ox, oy + 3, 106, 18,
            s_hwnd_opts, (HMENU)(UINT_PTR)IDC_OPT_TIMEOUT_LBL, hInst, NULL);
        CreateWindowExA(0, "EDIT", "500",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
            ox + 112, oy, 80, 22,
            s_hwnd_opts, (HMENU)(UINT_PTR)IDC_OPT_TIMEOUT_ED, hInst, NULL);
        oy += 40;

        CreateWindowExA(0, "BUTTON", "OK",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            ox, oy, 80, 28,
            s_hwnd_opts, (HMENU)(UINT_PTR)IDC_OPT_OK, hInst, NULL);
        CreateWindowExA(0, "BUTTON", "Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            ox + 90, oy, 80, 28,
            s_hwnd_opts, (HMENU)(UINT_PTR)IDC_OPT_CANCEL, hInst, NULL);
    }

    /* ---- Tray icon ---- */
    s_nid.cbSize           = sizeof(s_nid);
    s_nid.hWnd             = hwnd_msg;
    s_nid.uID              = 1;
    s_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    s_nid.uCallbackMessage = WM_TRAYICON;
    s_nid.hIcon = (HICON)LoadImageA(hInst, MAKEINTRESOURCEA(1),
                                     IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    if (!s_nid.hIcon) s_nid.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    strncpy(s_nid.szTip, "StickPoint \xe2\x80\x94 No controllers",
            sizeof(s_nid.szTip) - 1);
    if (!Shell_NotifyIconA(NIM_ADD, &s_nid)) return false;

    show_empty_label(true);
    s_initialized = true;
    return true;
}

/*
 * tray_update_gamepads
 * ────────────────────
 * Called every polling frame (including when disconnected) with the current
 * state of all four XInput slots.  Uses s_row_shown[] and s_shown_mode[] for
 * change detection so it works correctly even when infos == s_last_infos.
 */
void tray_update_gamepads(const GamepadInfo infos[XUSER_MAX_COUNT])
{
    if (!s_initialized) return;

    bool layout_changed = false;
    bool tooltip_dirty  = false;

    for (int i = 0; i < XUSER_MAX_COUNT; i++) {
        bool now_connected = infos[i].connected;

        /* ---- Connection state changed? ---- */
        if (now_connected != s_row_shown[i]) {
            layout_changed = true;
            tooltip_dirty  = true;

            if (now_connected) {
                /* Newly connected: snapshot name/image/mode, load bitmap. */
                s_shown_mode[i] = infos[i].slot_state.mode;
                strncpy_s(s_shown_name[i], sizeof(s_shown_name[i]),
                          infos[i].name, _TRUNCATE);
                wcsncpy_s(s_shown_image[i], MAX_PATH, infos[i].image, _TRUNCATE);
                s_row_shown[i]  = true;
                if (s_bitmaps[i]) { DeleteObject(s_bitmaps[i]); s_bitmaps[i] = NULL; }
                s_bitmaps[i] = load_png_hbitmap(infos[i].image);
            } else {
                /* Newly disconnected: tear down UI for this slot. */
                destroy_row_controls(i);
                s_row_shown[i] = false;
                /* Also close options dialog if it was open for this slot. */
                if (s_options_slot == i) {
                    EnableWindow(s_hwnd_pop, TRUE);
                    s_options_slot = -1;
                    ShowWindow(s_hwnd_opts, SW_HIDE);
                }
            }
        }

        /* ---- Mode changed (only while connected)? ---- */
        if (now_connected && s_row_shown[i] &&
            infos[i].slot_state.mode != s_shown_mode[i]) {
            s_shown_mode[i] = infos[i].slot_state.mode;
            update_row_mode_btn(i, s_shown_mode[i]);
            tooltip_dirty = true;
        }

        /* ---- Name/image changed (e.g. WM_DEVICECHANGE ran detect)? ---- */
        if (now_connected && s_row_shown[i]) {
            if (strcmp(infos[i].name, s_shown_name[i]) != 0) {
                strncpy_s(s_shown_name[i], sizeof(s_shown_name[i]),
                          infos[i].name, _TRUNCATE);
                HWND hlbl = GetDlgItem(s_hwnd_pop, ROW_NAME_ID(i));
                if (hlbl) SetWindowTextA(hlbl, infos[i].name);
                tooltip_dirty = true;
            }
            if (wcscmp(infos[i].image, s_shown_image[i]) != 0) {
                wcsncpy_s(s_shown_image[i], MAX_PATH,
                          infos[i].image, _TRUNCATE);
                if (s_bitmaps[i]) {
                    DeleteObject(s_bitmaps[i]);
                    s_bitmaps[i] = NULL;
                }
                s_bitmaps[i] = load_png_hbitmap(infos[i].image);
                HWND himg = GetDlgItem(s_hwnd_pop, ROW_IMG_ID(i));
                if (himg) InvalidateRect(himg, NULL, TRUE);
            }
        }
    }

    if (layout_changed) {
        /* Reposition existing controls, then create controls for new rows. */
        reposition_all_rows();
        for (int i = 0; i < XUSER_MAX_COUNT; i++) {
            if (s_row_shown[i] && !GetDlgItem(s_hwnd_pop, ROW_IMG_ID(i)))
                create_row_controls(i);
        }
        show_empty_label(shown_count() == 0);
        recalc_window_size();
        if (s_pop_vis) InvalidateRect(s_hwnd_pop, NULL, TRUE);
    }

    if (tooltip_dirty) set_tooltip_summary();
}

GamepadInfo *tray_get_infos(void)
{
    return s_last_infos;
}

void tray_shutdown(void)
{
    if (!s_initialized) return;
    for (int i = 0; i < XUSER_MAX_COUNT; i++) {
        if (s_bitmaps[i]) { DeleteObject(s_bitmaps[i]); s_bitmaps[i] = NULL; }
    }
    if (s_wic) { s_wic->lpVtbl->Release(s_wic); s_wic = NULL; }
    CoUninitialize();
    Shell_NotifyIconA(NIM_DELETE, &s_nid);
    s_initialized = false;
}

bool tray_handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                         LRESULT *out)
{
    *out = 0;

    if (msg == WM_TRAYICON) {
        UINT evt = (UINT)lp;
        if (evt == WM_LBUTTONUP) {
            if (s_pop_vis) {
                ShowWindow(s_hwnd_pop, SW_HIDE);
                s_pop_vis = false;
            } else {
                ShowWindow(s_hwnd_pop, SW_SHOW);
                SetForegroundWindow(s_hwnd_pop);
                s_pop_vis = true;
            }
            return true;
        }
        if (evt == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            HMENU hm = CreatePopupMenu();
            AppendMenuA(hm, MF_STRING, IDM_EXIT, "Exit StickPoint");
            TrackPopupMenu(hm, TPM_BOTTOMALIGN | TPM_RIGHTALIGN | TPM_LEFTBUTTON,
                           pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hm);
            return true;
        }
    }

    if (msg == WM_COMMAND && LOWORD(wp) == IDM_EXIT) {
        DestroyWindow(hwnd);
        return true;
    }

    return false;
}

/*
 * StickPoint — system tray icon and main status window
 *
 * UI design:
 *   - Left-click tray icon toggles a centered window.
 *   - For each connected XInput slot a row is shown:
 *       [64×64 controller image] | [name (semibold)] + [Slot N] | [mode btn] [options btn]
 *   - When more rows than WIN_MAX_VIS_H allows, the window caps at that height
 *     and the user can scroll with the mouse wheel.  A 4-px indicator strip on
 *     the right edge shows the current scroll position.
 *   - Separator lines are painted between rows.
 *   - When no controllers are connected a centred label is shown instead.
 *   - PNG images are loaded via WIC (Windows Imaging Component).
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>       /* IWICImagingFactory — link windowscodecs.lib */
#include <wingdi.h>         /* AlphaBlend         — link msimg32.lib       */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "tray.h"
#include "gamepad.h"

/* ---- constants ----------------------------------------------------------- */

#define WM_TRAYICON         (WM_APP + 1)
#define IDM_EXIT            100
#define IDC_EMPTY           900

/* Per-row control IDs: slot N → base 1000 + N*10 */
#define ROW_IMG_ID(s)       (1000 + (s) * 10)   /* STATIC SS_OWNERDRAW */
#define ROW_NAME_ID(s)      (1001 + (s) * 10)   /* STATIC — controller name  */
#define ROW_MODE_ID(s)      (1002 + (s) * 10)   /* BUTTON — mode toggle      */
#define ROW_OPT_ID(s)       (1003 + (s) * 10)   /* BUTTON — options          */

/* Options dialog controls */
#define IDC_OPT_TITLE       2000
#define IDC_OPT_HOLD_LBL    2001
#define IDC_OPT_HOLD_CB     2002
#define IDC_OPT_PRESS_LBL   2003
#define IDC_OPT_PRESS_CB    2004
#define IDC_OPT_TIMEOUT_LBL 2005
#define IDC_OPT_TIMEOUT_ED  2006
#define IDC_OPT_OK          2007
#define IDC_OPT_CANCEL      2008

/* Main window layout */
#define WIN_W           560     /* client-area width (fixed)                  */
#define WIN_MAX_VIS_H   268     /* max client height — mouse-wheel to scroll  */
#define WIN_H_EMPTY     120     /* client height when no controllers present  */
#define ROW_H           80      /* height of each controller row              */
#define ROW_PAD_TOP     8       /* padding above the first row                */
#define ROW_PAD_BOT     8       /* padding below the last row                 */

/* Image cell (left of row) */
#define IMG_X           12
#define IMG_Y           8
#define IMG_W           64
#define IMG_H           64

/* Controller name (bold, single line) — vertically centred in the row */
#define NAME_X          88
#define NAME_Y          29
#define NAME_W          244
#define NAME_H          22

/* Mode-toggle button */
#define MODE_X          344
#define MODE_Y          26
#define MODE_W          96
#define MODE_H          28

/* Options button */
#define OPT_X           448
#define OPT_Y           26
#define OPT_W           96
#define OPT_H           28

/* Right-edge scroll indicator */
#define SCROLL_STRIP_W  4

/* Colours */
#define CLR_BG          RGB(245, 245, 245)
#define CLR_SEP         RGB(218, 218, 218)
#define CLR_SCROLL_BG   RGB(228, 228, 228)
#define CLR_SCROLL_FG   RGB(160, 160, 160)

/* ---- private state ------------------------------------------------------- */

static NOTIFYICONDATAA     s_nid         = {0};
static HWND                s_hwnd_msg    = NULL;
static HWND                s_hwnd_pop    = NULL;
static HWND                s_hwnd_opts   = NULL;
static HINSTANCE           s_hInst       = NULL;
static bool                s_pop_vis     = false;
static bool                s_initialized = false;
static int                 s_scroll_y    = 0;   /* current scroll offset in px */

/*
 * s_last_infos: live array shared with main.c via tray_get_infos().
 * Change detection uses s_row_shown / s_shown_mode / s_shown_name / s_shown_image
 * so it works correctly even when infos == s_last_infos.
 */
static GamepadInfo s_last_infos[XUSER_MAX_COUNT];

static bool    s_row_shown[XUSER_MAX_COUNT];
static AppMode s_shown_mode[XUSER_MAX_COUNT];
static char    s_shown_name[XUSER_MAX_COUNT][256];
static wchar_t s_shown_image[XUSER_MAX_COUNT][MAX_PATH];
static HBITMAP s_bitmaps[XUSER_MAX_COUNT];

static IWICImagingFactory *s_wic          = NULL;
static int                 s_options_slot = -1;

static HFONT  s_font_name  = NULL;  /* Segoe UI SemiBold ~14 logical px */
static HFONT  s_font_ui    = NULL;  /* Segoe UI Normal   ~12 logical px */
static HBRUSH s_brush_bg   = NULL;  /* solid brush matching CLR_BG      */

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

/* ---- Helpers ------------------------------------------------------------- */

/*
 * Strip a leading "Controller " or "Controller" prefix from a device name so
 * the UI shows e.g. "Wireless" instead of "Controller Wireless".
 */
static const char *display_name(const char *name)
{
    static const char prefix[] = "Controller ";
    if (strncmp(name, prefix, sizeof(prefix) - 1) == 0)
        return name + sizeof(prefix) - 1;
    /* Bare "Controller" with nothing after — keep as-is. */
    return name;
}

/* ---- forward declarations ------------------------------------------------ */

static LRESULT CALLBACK PopupWndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK OptionsWndProc(HWND, UINT, WPARAM, LPARAM);
static void create_row_controls(int slot);
static void destroy_row_controls(int slot);
static void update_row_mode_btn(int slot, AppMode mode);
static void recalc_window_size(void);
static void reposition_all_rows(void);
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

    /* Compute aspect-fit dimensions: scale uniformly so the image fits inside
       IMG_W × IMG_H without cropping, leaving transparent letterbox/pillarbox. */
    UINT orig_w = 0, orig_h = 0;
    ((IWICBitmapSource *)conv)->lpVtbl->GetSize((IWICBitmapSource *)conv, &orig_w, &orig_h);
    if (orig_w == 0) orig_w = (UINT)IMG_W;
    if (orig_h == 0) orig_h = (UINT)IMG_H;

    UINT fit_w, fit_h;
    if (orig_w * IMG_H >= orig_h * IMG_W) {
        /* wider than tall — constrained by width */
        fit_w = (UINT)IMG_W;
        fit_h = (orig_h * (UINT)IMG_W + orig_w / 2) / orig_w;
        if (fit_h < 1) fit_h = 1;
    } else {
        /* taller than wide — constrained by height */
        fit_h = (UINT)IMG_H;
        fit_w = (orig_w * (UINT)IMG_H + orig_h / 2) / orig_h;
        if (fit_w < 1) fit_w = 1;
    }

    IWICBitmapScaler *scaler = NULL;
    hr = s_wic->lpVtbl->CreateBitmapScaler(s_wic, &scaler);
    if (SUCCEEDED(hr)) {
        hr = scaler->lpVtbl->Initialize(scaler,
            (IWICBitmapSource *)conv,
            fit_w, fit_h,
            WICBitmapInterpolationModeHighQualityCubic);
    }
    conv->lpVtbl->Release(conv);
    if (FAILED(hr)) {
        if (scaler) scaler->lpVtbl->Release(scaler);
        return NULL;
    }

    /* Create a fully-transparent IMG_W × IMG_H DIB section, then blit the
       scaled image centred inside it (letterbox / pillarbox). */
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
        /* Zero-fill → all pixels transparent (BGRA = 0,0,0,0). */
        memset(bits, 0, (size_t)IMG_W * IMG_H * 4);

        /* Copy scaled pixels into the centred sub-rectangle. */
        int ox = ((int)IMG_W - (int)fit_w) / 2;
        int oy = ((int)IMG_H - (int)fit_h) / 2;

        UINT src_stride = fit_w * 4;
        UINT src_size   = src_stride * fit_h;
        BYTE *tmp = (BYTE *)HeapAlloc(GetProcessHeap(), 0, src_size);
        if (tmp) {
            if (SUCCEEDED(scaler->lpVtbl->CopyPixels(scaler, NULL,
                                                     src_stride, src_size, tmp))) {
                BYTE *dst = (BYTE *)bits;
                for (UINT row = 0; row < fit_h; row++) {
                    memcpy(dst + ((oy + (int)row) * IMG_W + ox) * 4,
                           tmp + row * src_stride,
                           src_stride);
                }
            }
            HeapFree(GetProcessHeap(), 0, tmp);
        } else {
            DeleteObject(hbmp);
            hbmp = NULL;
        }
    }
    scaler->lpVtbl->Release(scaler);
    return hbmp;
}

/* ---- Window helpers ------------------------------------------------------ */

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

/* Total pixel height needed to show every row without scrolling. */
static int total_content_h(void)
{
    int n = shown_count();
    return (n == 0) ? WIN_H_EMPTY
                    : (ROW_PAD_TOP + n * ROW_H + ROW_PAD_BOT);
}

/* Resize the outer window and clamp the scroll position. */
static void recalc_window_size(void)
{
    int content_h = total_content_h();
    int client_h  = (content_h > WIN_MAX_VIS_H) ? WIN_MAX_VIS_H : content_h;

    /* Clamp scroll. */
    int max_scroll = content_h - client_h;
    if (s_scroll_y > max_scroll) s_scroll_y = max_scroll;
    if (s_scroll_y < 0)          s_scroll_y = 0;

    RECT r = {0, 0, WIN_W, client_h};
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    int new_w = r.right  - r.left;
    int new_h = r.bottom - r.top;

    RECT cur;
    GetWindowRect(s_hwnd_pop, &cur);
    if ((cur.right - cur.left) != new_w || (cur.bottom - cur.top) != new_h)
        center_window(s_hwnd_pop, new_w, new_h);
}

/* Tray icon tooltip: one-line summary. */
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

/* ---- Scroll helper ------------------------------------------------------- */

static void apply_scroll(int new_y)
{
    int content_h = total_content_h();
    int client_h  = (content_h > WIN_MAX_VIS_H) ? WIN_MAX_VIS_H : content_h;
    int max_scroll = content_h - client_h;
    if (new_y < 0)          new_y = 0;
    if (new_y > max_scroll) new_y = max_scroll;
    if (new_y == s_scroll_y) return;
    s_scroll_y = new_y;
    reposition_all_rows();
    InvalidateRect(s_hwnd_pop, NULL, TRUE);
}

/* ---- Row control management --------------------------------------------- */

/*
 * Visual row index (0-based position in the window) for a given XInput slot.
 * Uses s_row_shown[] so it reflects the current UI state.
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

/* Top of the Nth visual row in client coords (scroll-adjusted). */
static int row_top(int visual_idx)
{
    return ROW_PAD_TOP + visual_idx * ROW_H - s_scroll_y;
}

static void create_row_controls(int slot)
{
    int vi = visual_index_for_slot(slot);
    if (vi < 0) return;
    int y0 = row_top(vi);

    /* Controller image (owner-draw). */
    CreateWindowExA(0, "STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
        IMG_X, y0 + IMG_Y, IMG_W, IMG_H,
        s_hwnd_pop, (HMENU)(UINT_PTR)ROW_IMG_ID(slot), s_hInst, NULL);

    /* Controller name — semibold font. */
    HWND hname = CreateWindowExA(0, "STATIC", display_name(s_last_infos[slot].name),
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS,
        NAME_X, y0 + NAME_Y, NAME_W, NAME_H,
        s_hwnd_pop, (HMENU)(UINT_PTR)ROW_NAME_ID(slot), s_hInst, NULL);
    if (hname && s_font_name)
        SendMessageA(hname, WM_SETFONT, (WPARAM)s_font_name, FALSE);

    /* Mode toggle button. */
    const char *mode_lbl = (s_shown_mode[slot] == MODE_MOUSE) ? "Mouse" : "Gamepad";
    HWND hmode = CreateWindowExA(0, "BUTTON", mode_lbl,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        MODE_X, y0 + MODE_Y, MODE_W, MODE_H,
        s_hwnd_pop, (HMENU)(UINT_PTR)ROW_MODE_ID(slot), s_hInst, NULL);
    if (hmode && s_font_ui)
        SendMessageA(hmode, WM_SETFONT, (WPARAM)s_font_ui, FALSE);

    /* Options button. */
    HWND hopt = CreateWindowExA(0, "BUTTON", "Options",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        OPT_X, y0 + OPT_Y, OPT_W, OPT_H,
        s_hwnd_pop, (HMENU)(UINT_PTR)ROW_OPT_ID(slot), s_hInst, NULL);
    if (hopt && s_font_ui)
        SendMessageA(hopt, WM_SETFONT, (WPARAM)s_font_ui, FALSE);
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
 * Slide all existing controls to their new y positions (scroll-aware).
 * Must be called with s_row_shown[] already reflecting the current layout.
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
            HWND hl = CreateWindowExA(0, "STATIC", "No controllers connected.",
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                (r.right - lw) / 2, (r.bottom - lh) / 2, lw, lh,
                s_hwnd_pop, (HMENU)(UINT_PTR)IDC_EMPTY, s_hInst, NULL);
            if (hl && s_font_ui)
                SendMessageA(hl, WM_SETFONT, (WPARAM)s_font_ui, FALSE);
        } else {
            ShowWindow(h, SW_SHOW);
        }
    } else if (h) {
        ShowWindow(h, SW_HIDE);
    }
}

/* ---- Scroll indicator strip ---------------------------------------------- */

static void draw_scroll_strip(HDC hdc)
{
    int content_h = total_content_h();
    if (content_h <= WIN_MAX_VIS_H)
        return;   /* content fits — nothing to draw */

    RECT cli;
    GetClientRect(s_hwnd_pop, &cli);
    int client_h = cli.bottom;

    /* Track */
    RECT track = { cli.right - SCROLL_STRIP_W, 0, cli.right, client_h };
    HBRUSH br = CreateSolidBrush(CLR_SCROLL_BG);
    FillRect(hdc, &track, br);
    DeleteObject(br);

    /* Thumb */
    float ratio    = (float)client_h / (float)content_h;
    int   thumb_h  = (int)(client_h * ratio);
    if (thumb_h < 16) thumb_h = 16;

    int max_scroll  = content_h - client_h;
    float frac      = (max_scroll > 0) ? (float)s_scroll_y / (float)max_scroll : 0.f;
    int   thumb_top = (int)(frac * (client_h - thumb_h));

    RECT thumb = { cli.right - SCROLL_STRIP_W, thumb_top,
                   cli.right, thumb_top + thumb_h };
    br = CreateSolidBrush(CLR_SCROLL_FG);
    FillRect(hdc, &thumb, br);
    DeleteObject(br);
}

/* ---- Main window procedure ----------------------------------------------- */

static LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    /* Owner-draw: render the 64×64 controller bitmap with alpha. */
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
            /* Placeholder rectangle when no bitmap is loaded. */
            HBRUSH br = CreateSolidBrush(RGB(210, 210, 210));
            FillRect(dis->hDC, &dis->rcItem, br);
            DeleteObject(br);
        }
        return TRUE;
    }

    /* Suppress the default erase so WM_PAINT owns the entire background. */
    case WM_ERASEBKGND:
        return 1;

    /* Paint background, separator lines between rows, and scroll indicator. */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT cli;
        GetClientRect(hwnd, &cli);

        /* Background */
        FillRect(hdc, &cli, s_brush_bg);

        /* Separators between rows */
        int n = shown_count();
        if (n > 1) {
            HPEN pen = CreatePen(PS_SOLID, 1, CLR_SEP);
            HGDIOBJ old_pen = SelectObject(hdc, pen);
            int sep_right = cli.right - SCROLL_STRIP_W - 4;
            for (int vi = 1; vi < n; vi++) {
                int sep_y = ROW_PAD_TOP + vi * ROW_H - s_scroll_y;
                if (sep_y >= 0 && sep_y < cli.bottom) {
                    MoveToEx(hdc, NAME_X, sep_y, NULL);
                    LineTo(hdc, sep_right, sep_y);
                }
            }
            SelectObject(hdc, old_pen);
            DeleteObject(pen);
        }

        /* Scroll indicator */
        draw_scroll_strip(hdc);

        EndPaint(hwnd, &ps);
        return 0;
    }

    /* Make STATIC controls transparent over the custom background. */
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        (void)lp;
        SetTextColor(hdc, RGB(25, 25, 25));
        SetBkColor(hdc, CLR_BG);
        return (LRESULT)s_brush_bg;
    }

    /* Mouse-wheel scrolling. */
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        /* One notch scrolls by 1/3 of a row. */
        int step = (ROW_H + 2) / 3;
        apply_scroll(s_scroll_y + (delta > 0 ? -step : step));
        return 0;
    }

    /* Button commands. */
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

    char title[320];
    _snprintf_s(title, sizeof(title), _TRUNCATE,
                "Options \xe2\x80\x94 %s", s_last_infos[slot].name);
    SetWindowTextA(s_hwnd_opts, title);

    const GamepadSlotState *ss = &s_last_infos[slot].slot_state;
    populate_combo(GetDlgItem(s_hwnd_opts, IDC_OPT_HOLD_CB),  ss->combo_hold_btn);
    populate_combo(GetDlgItem(s_hwnd_opts, IDC_OPT_PRESS_CB), ss->combo_press_btn);
    char buf[16];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%u", (unsigned)ss->combo_timeout_ms);
    SetWindowTextA(GetDlgItem(s_hwnd_opts, IDC_OPT_TIMEOUT_ED), buf);

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

/* ---- Public API ---------------------------------------------------------- */

bool tray_init(HINSTANCE hInst, HWND hwnd_msg)
{
    s_hInst    = hInst;
    s_hwnd_msg = hwnd_msg;

    CoInitialize(NULL);
    init_wic();

    /* Fonts */
    s_font_name = CreateFontA(
        -14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    s_font_ui = CreateFontA(
        -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

    /* Shared background brush */
    s_brush_bg = CreateSolidBrush(CLR_BG);

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
        wc.hbrBackground = NULL;   /* WM_PAINT owns the background */
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
 * Called every polling frame with the current state of all four XInput slots.
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
                s_shown_mode[i] = infos[i].slot_state.mode;
                strncpy_s(s_shown_name[i],  sizeof(s_shown_name[i]),
                          infos[i].name, _TRUNCATE);
                wcsncpy_s(s_shown_image[i], MAX_PATH,
                          infos[i].image, _TRUNCATE);
                s_row_shown[i] = true;
                if (s_bitmaps[i]) { DeleteObject(s_bitmaps[i]); s_bitmaps[i] = NULL; }
                s_bitmaps[i] = load_png_hbitmap(infos[i].image);
            } else {
                destroy_row_controls(i);
                s_row_shown[i] = false;
                if (s_options_slot == i) {
                    EnableWindow(s_hwnd_pop, TRUE);
                    s_options_slot = -1;
                    ShowWindow(s_hwnd_opts, SW_HIDE);
                }
            }
        }

        /* ---- Mode changed? ---- */
        if (now_connected && s_row_shown[i] &&
            infos[i].slot_state.mode != s_shown_mode[i]) {
            s_shown_mode[i] = infos[i].slot_state.mode;
            update_row_mode_btn(i, s_shown_mode[i]);
            tooltip_dirty = true;
        }

        /* ---- Name / image changed (e.g. after WM_DEVICECHANGE)? ---- */
        if (now_connected && s_row_shown[i]) {
            if (strcmp(infos[i].name, s_shown_name[i]) != 0) {
                strncpy_s(s_shown_name[i], sizeof(s_shown_name[i]),
                          infos[i].name, _TRUNCATE);
                HWND hlbl = GetDlgItem(s_hwnd_pop, ROW_NAME_ID(i));
                if (hlbl) SetWindowTextA(hlbl, display_name(infos[i].name));
                tooltip_dirty = true;
            }
            if (wcscmp(infos[i].image, s_shown_image[i]) != 0) {
                wcsncpy_s(s_shown_image[i], MAX_PATH,
                          infos[i].image, _TRUNCATE);
                if (s_bitmaps[i]) { DeleteObject(s_bitmaps[i]); s_bitmaps[i] = NULL; }
                s_bitmaps[i] = load_png_hbitmap(infos[i].image);
                HWND himg = GetDlgItem(s_hwnd_pop, ROW_IMG_ID(i));
                if (himg) InvalidateRect(himg, NULL, TRUE);
            }
        }
    }

    if (layout_changed) {
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
    if (s_font_name) { DeleteObject(s_font_name); s_font_name = NULL; }
    if (s_font_ui)   { DeleteObject(s_font_ui);   s_font_ui   = NULL; }
    if (s_brush_bg)  { DeleteObject(s_brush_bg);  s_brush_bg  = NULL; }
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

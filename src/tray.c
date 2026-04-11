/*
 * StickPoint — system tray icon and popup status window
 *
 * Provides a Shell_NotifyIcon tray icon that:
 *   - Shows a tooltip with the current controller state.
 *   - On left-click: toggles a small popup window showing controller status
 *     and mode, with a button to switch modes.
 *   - On right-click: shows a context menu with an "Exit" item.
 *
 * The popup auto-hides when it loses focus (WM_ACTIVATE / WA_INACTIVE).
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <stdbool.h>
#include <string.h>

#include "tray.h"
#include "gamepad.h"

/* ---- private constants --------------------------------------------------- */

#define WM_TRAYICON         (WM_APP + 1)
#define IDM_EXIT            100
#define IDC_LABEL_STATUS    200
#define IDC_LABEL_MODE      201
#define IDC_BTN_TOGGLE      202

#define POPUP_W             300
#define POPUP_H             110

/* ---- private state ------------------------------------------------------- */

static NOTIFYICONDATAA s_nid       = {0};
static HWND            s_hwnd_msg  = NULL;   /* message-only window          */
static HWND            s_hwnd_pop  = NULL;   /* status popup window          */
static bool            s_pop_vis   = false;  /* popup currently visible?     */

/* Cached last-known state so tray_update can skip redundant repaints. */
static bool    s_last_connected = false;
static AppMode s_last_mode      = MODE_NORMAL;
static bool    s_initialized    = false;

/* ---- forward declarations ------------------------------------------------ */

static LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg,
                                     WPARAM wp, LPARAM lp);
static void update_popup_controls(bool connected, AppMode mode);
static void position_popup(void);

/* ---- tray icon helpers --------------------------------------------------- */

static void set_tooltip(bool connected, AppMode mode)
{
    const char *suffix;
    if (!connected)       suffix = "No controller";
    else if (mode == MODE_MOUSE) suffix = "Mouse mode";
    else                  suffix = "Gamepad mode";

    /* szTip is 128 chars in NOTIFYICONDATAA */
    _snprintf_s(s_nid.szTip, sizeof(s_nid.szTip), _TRUNCATE,
                "StickPoint \xe2\x80\x94 %s", suffix);
    s_nid.uFlags = NIF_TIP;
    Shell_NotifyIconA(NIM_MODIFY, &s_nid);
    s_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; /* restore full flags */
}

/* ---- popup positioning --------------------------------------------------- */

static void position_popup(void)
{
    RECT wa;   /* usable desktop area (excludes taskbar) */
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);

    int x = wa.right  - POPUP_W - 4;
    int y = wa.bottom - POPUP_H - 4;
    SetWindowPos(s_hwnd_pop, HWND_TOPMOST, x, y, POPUP_W, POPUP_H,
                 SWP_NOSIZE | SWP_NOZORDER);
}

/* ---- popup control helpers ----------------------------------------------- */

static void update_popup_controls(bool connected, AppMode mode)
{
    HWND hlbl_status = GetDlgItem(s_hwnd_pop, IDC_LABEL_STATUS);
    HWND hlbl_mode   = GetDlgItem(s_hwnd_pop, IDC_LABEL_MODE);
    HWND hbtn        = GetDlgItem(s_hwnd_pop, IDC_BTN_TOGGLE);

    SetWindowTextA(hlbl_status,
                   connected ? "Controller: Connected"
                              : "Controller: Disconnected");

    if (!connected) {
        SetWindowTextA(hlbl_mode, "Mode: \xe2\x80\x94");
        SetWindowTextA(hbtn, "Switch to Mouse Mode");
    } else if (mode == MODE_MOUSE) {
        SetWindowTextA(hlbl_mode, "Mode: Mouse");
        SetWindowTextA(hbtn, "Switch to Gamepad Mode");
    } else {
        SetWindowTextA(hlbl_mode, "Mode: Gamepad");
        SetWindowTextA(hbtn, "Switch to Mouse Mode");
    }

    EnableWindow(hbtn, connected ? TRUE : FALSE);
}

/* ---- popup window procedure ---------------------------------------------- */

static LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg,
                                     WPARAM wp, LPARAM lp)
{
    (void)lp;

    switch (msg) {
    case WM_ACTIVATE:
        /*
         * Auto-hide the popup when it loses focus, mirroring the behaviour
         * of standard system-tray flyout windows.
         */
        if (LOWORD(wp) == WA_INACTIVE) {
            ShowWindow(hwnd, SW_HIDE);
            s_pop_vis = false;
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_BTN_TOGGLE) {
            AppMode cur     = gamepad_get_mode();
            AppMode toggled = (cur == MODE_MOUSE) ? MODE_NORMAL : MODE_MOUSE;
            gamepad_set_mode(toggled);
            update_popup_controls(s_last_connected, toggled);
            set_tooltip(s_last_connected, toggled);
            s_last_mode = toggled;
        }
        return 0;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        s_pop_vis = false;
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- public API ---------------------------------------------------------- */

bool tray_init(HINSTANCE hInst, HWND hwnd_msg)
{
    s_hwnd_msg = hwnd_msg;

    /* Register the popup window class. */
    WNDCLASSEXA wc  = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = PopupWndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "StickPointPopup";
    if (!RegisterClassExA(&wc))
        return false;

    /*
     * Create the popup window (initially hidden).
     * WS_CAPTION gives it a title bar so the user can see it has focus and
     * can close it with the [X] button.
     */
    s_hwnd_pop = CreateWindowExA(
        WS_EX_TOOLWINDOW,            /* omit from Alt+Tab list */
        "StickPointPopup",
        "StickPoint",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        0, 0, POPUP_W, POPUP_H,
        NULL, NULL, hInst, NULL);
    if (!s_hwnd_pop)
        return false;

    /* ---- create child controls ---- */

    /* Status label */
    CreateWindowExA(0, "STATIC", "Controller: Disconnected",
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    10, 10, 270, 20,
                    s_hwnd_pop, (HMENU)(UINT_PTR)IDC_LABEL_STATUS, hInst, NULL);

    /* Mode label */
    CreateWindowExA(0, "STATIC", "Mode: \xe2\x80\x94",
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    10, 34, 270, 20,
                    s_hwnd_pop, (HMENU)(UINT_PTR)IDC_LABEL_MODE, hInst, NULL);

    /* Toggle button */
    CreateWindowExA(0, "BUTTON", "Switch to Mouse Mode",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    10, 62, 270, 28,
                    s_hwnd_pop, (HMENU)(UINT_PTR)IDC_BTN_TOGGLE, hInst, NULL);

    EnableWindow(GetDlgItem(s_hwnd_pop, IDC_BTN_TOGGLE), FALSE);

    /* ---- register tray icon ---- */

    s_nid.cbSize           = sizeof(s_nid);
    s_nid.hWnd             = hwnd_msg;
    s_nid.uID              = 1;
    s_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    s_nid.uCallbackMessage = WM_TRAYICON;
    s_nid.hIcon            = LoadIconA(NULL, IDI_APPLICATION);
    strncpy(s_nid.szTip, "StickPoint \xe2\x80\x94 No controller",
            sizeof(s_nid.szTip) - 1);

    if (!Shell_NotifyIconA(NIM_ADD, &s_nid))
        return false;

    s_initialized = true;
    return true;
}

void tray_update(bool connected, AppMode mode)
{
    if (!s_initialized)
        return;

    bool changed = (connected != s_last_connected) || (mode != s_last_mode);
    if (!changed)
        return;

    s_last_connected = connected;
    s_last_mode      = mode;

    set_tooltip(connected, mode);

    if (s_pop_vis)
        update_popup_controls(connected, mode);
}

void tray_shutdown(void)
{
    if (!s_initialized)
        return;
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
                /* Refresh content before showing. */
                update_popup_controls(s_last_connected, s_last_mode);
                position_popup();
                ShowWindow(s_hwnd_pop, SW_SHOW);
                SetForegroundWindow(s_hwnd_pop);
                s_pop_vis = true;
            }
            return true;
        }

        if (evt == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);   /* required for TrackPopupMenu to dismiss */

            HMENU hMenu = CreatePopupMenu();
            AppendMenuA(hMenu, MF_STRING, IDM_EXIT, "Exit StickPoint");
            TrackPopupMenu(hMenu,
                           TPM_BOTTOMALIGN | TPM_RIGHTALIGN | TPM_LEFTBUTTON,
                           pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
            return true;
        }
    }

    if (msg == WM_COMMAND && LOWORD(wp) == IDM_EXIT) {
        DestroyWindow(hwnd);   /* → WM_DESTROY → PostQuitMessage → clean exit */
        *out = 0;
        return true;
    }

    return false;
}

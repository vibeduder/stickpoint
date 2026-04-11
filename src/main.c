/*
 * StickPoint — main entry point
 *
 * Runs as a background process (no console window, WinMain entry).
 * Creates a message-only window so it can receive WM_QUIT cleanly.
 * Polls all four XInput slots at POLL_INTERVAL_MS and, for each slot
 * in mouse mode, translates stick/button input into injected mouse events.
 *
 * tray_update_gamepads() is called unconditionally every frame (including
 * when controllers are disconnected) so the UI always reflects the true state.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbt.h>        /* DBT_DEVICEARRIVAL / DBT_DEVICEREMOVECOMPLETE */
#include <math.h>
#include <stdbool.h>

#include "config.h"
#include "gamepad.h"
#include "mouse.h"
#include "tray.h"

/* ---- globals ------------------------------------------------------------ */

static volatile bool s_running = true;
static HWND          s_hwnd    = NULL;

static DWORD         s_connected_mask = 0;

/* Per-slot mouse-input state (must survive frames). */
typedef struct {
    DWORD a_release_time;
    bool  a_pending_dc;
    bool  a_held;
    float scroll_acc;
    bool  lt_held;
    bool  rt_held;
} SlotMouseState;

static SlotMouseState s_mouse_state[XUSER_MAX_COUNT];

/* ---- hidden message-only window ----------------------------------------- */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT r = 0;
    if (tray_handle_message(hwnd, msg, wp, lp, &r))
        return r;

    if (msg == WM_DESTROY) {
        s_running = false;
        PostQuitMessage(0);
        return 0;
    }

    /*
     * WM_DEVICECHANGE: re-detect controller names/images when a device
     * is added or removed (covers plug-in after startup).
     */
    if (msg == WM_DEVICECHANGE) {
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
            GamepadInfo *infos = tray_get_infos();
            /* Poll first so connected flags are current before detection runs.
             * Without this, a newly-arrived controller still shows connected=false
             * and gamepad_detect_controllers skips it, leaving stale defaults. */
            gamepad_poll_all(infos);
            gamepad_detect_controllers(infos);
        }
        return 0;
    }

    if (msg == WM_INPUT) {
        gamepad_handle_raw_input((HRAWINPUT)lp);
        return DefWindowProcA(hwnd, msg, wp, lp);
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

static bool create_message_window(HINSTANCE hInst)
{
    WNDCLASSEXA wc  = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = "StickPointWnd";
    if (!RegisterClassExA(&wc))
        return false;

    s_hwnd = CreateWindowExA(0, "StickPointWnd", "StickPoint",
                              0, 0, 0, 0, 0,
                              HWND_MESSAGE, NULL, hInst, NULL);
    return s_hwnd != NULL;
}

/* ---- stick normalisation ------------------------------------------------ */

static float normalize_axis(SHORT raw)
{
    float v    = (float)raw;
    float sign = (v >= 0.0f) ? 1.0f : -1.0f;
    float mag  = fabsf(v);

    if (mag < (float)STICK_DEADZONE)
        return 0.0f;

    mag = (mag - (float)STICK_DEADZONE) / (32767.0f - (float)STICK_DEADZONE);
    if (mag > 1.0f) mag = 1.0f;
    mag = powf(mag, MOUSE_ACCEL_EXPONENT);

    return sign * mag;
}

/* ---- mouse-mode processing (per slot) ----------------------------------- */

static void process_mouse_mode(const GamepadState *state, float dt,
                               SlotMouseState *ms)
{
    /* ---- cursor movement (left stick) ----------------------------------- */
    float nx =  normalize_axis(state->lx);
    float ny = -normalize_axis(state->ly);

    if (nx != 0.0f || ny != 0.0f)
        mouse_move(nx * MOUSE_MAX_SPEED * dt, ny * MOUSE_MAX_SPEED * dt);

    /* ---- A button: left click with double-click detection --------------- */
    if (GP_JUST_PRESSED(state, XINPUT_GAMEPAD_A)) {
        if (ms->a_pending_dc) {
            mouse_double_click(MOUSE_BTN_LEFT);
            ms->a_pending_dc = false;
            ms->a_held       = false;
        } else {
            mouse_button_down(MOUSE_BTN_LEFT);
            ms->a_held = true;
        }
    }
    if (GP_JUST_RELEASED(state, XINPUT_GAMEPAD_A) && ms->a_held) {
        mouse_button_up(MOUSE_BTN_LEFT);
        ms->a_held         = false;
        ms->a_release_time = state->timestamp_ms;
        ms->a_pending_dc   = true;
    }
    if (ms->a_pending_dc &&
        (state->timestamp_ms - ms->a_release_time) >= (DWORD)DOUBLE_CLICK_MS) {
        ms->a_pending_dc = false;
    }

    /* ---- B button: right click ----------------------------------------- */
    if (GP_JUST_PRESSED(state,  XINPUT_GAMEPAD_B)) mouse_button_down(MOUSE_BTN_RIGHT);
    if (GP_JUST_RELEASED(state, XINPUT_GAMEPAD_B)) mouse_button_up(MOUSE_BTN_RIGHT);

    /* ---- Y button: middle click ---------------------------------------- */
    if (GP_JUST_PRESSED(state, XINPUT_GAMEPAD_Y))  mouse_click(MOUSE_BTN_MIDDLE);

    /* ---- Left trigger: left click (analog, held = drag) ---------------- */
    {
        bool lt_active = state->lt > TRIGGER_THRESHOLD;
        if (lt_active && !ms->lt_held) {
            mouse_button_down(MOUSE_BTN_LEFT);
            ms->lt_held = true;
        } else if (!lt_active && ms->lt_held) {
            mouse_button_up(MOUSE_BTN_LEFT);
            ms->lt_held = false;
        }
    }

    /* ---- Right trigger: right click (analog, held = drag) -------------- */
    {
        bool rt_active = state->rt > TRIGGER_THRESHOLD;
        if (rt_active && !ms->rt_held) {
            mouse_button_down(MOUSE_BTN_RIGHT);
            ms->rt_held = true;
        } else if (!rt_active && ms->rt_held) {
            mouse_button_up(MOUSE_BTN_RIGHT);
            ms->rt_held = false;
        }
    }

    /* ---- LB / RB: discrete scroll -------------------------------------- */
    if (state->buttons & XINPUT_GAMEPAD_LEFT_SHOULDER)  ms->scroll_acc += SCROLL_SPEED * dt;
    if (state->buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ms->scroll_acc -= SCROLL_SPEED * dt;

    /* ---- Right stick Y: fine scroll ------------------------------------ */
    {
        float ry = normalize_axis(state->ry);
        if (ry != 0.0f)
            ms->scroll_acc += ry * SCROLL_SPEED * dt;
    }

    if (fabsf(ms->scroll_acc) >= 1.0f) {
        int ticks       = (int)ms->scroll_acc;
        ms->scroll_acc -= (float)ticks;
        mouse_scroll(ticks);
    }
}

static DWORD get_connected_mask(const GamepadInfo infos[XUSER_MAX_COUNT])
{
    DWORD mask = 0;

    for (int i = 0; i < XUSER_MAX_COUNT; i++) {
        if (infos[i].connected)
            mask |= (1u << i);
    }

    return mask;
}

/* ---- entry point -------------------------------------------------------- */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;

    SetProcessDPIAware();

    if (!create_message_window(hInstance)) {
        MessageBoxA(NULL, "Failed to create message window.",
                    "StickPoint", MB_ICONERROR | MB_OK);
        return 1;
    }

    if (!gamepad_init()) {
        MessageBoxA(NULL,
                    "Could not load XInput.\n"
                    "Make sure XInput1_4.dll is present (Windows 8+).",
                    "StickPoint", MB_ICONERROR | MB_OK);
        return 1;
    }

    if (!tray_init(hInstance, s_hwnd)) {
        MessageBoxA(NULL, "Failed to create tray icon.",
                    "StickPoint", MB_ICONERROR | MB_OK);
        gamepad_shutdown();
        return 1;
    }

    gamepad_register_raw_input(s_hwnd);

    /* Initialise all slots with default combo config. */
    GamepadInfo *infos = tray_get_infos();
    for (int i = 0; i < XUSER_MAX_COUNT; i++) {
        infos[i].slot_state.combo_hold_btn   = XINPUT_GAMEPAD_GUIDE;
        infos[i].slot_state.combo_press_btn  = XINPUT_GAMEPAD_A;
        infos[i].slot_state.combo_timeout_ms = COMBO_TIMEOUT_MS;
        infos[i].slot_state.mode             = MODE_NORMAL;
    }
    gamepad_load_config(infos);

    /* Poll once so connected flags are set before detect runs. */
    gamepad_poll_all(infos);

    /* Initial controller name/image detection. */
    gamepad_detect_controllers(infos);
    s_connected_mask = get_connected_mask(infos);

    DWORD prev_tick = GetTickCount();

    while (s_running) {
        /* Non-blocking message pump. */
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                s_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!s_running) break;

        /* Delta time, capped to avoid large jumps. */
        DWORD now = GetTickCount();
        float dt  = (float)(now - prev_tick) / 1000.0f;
        if (dt > 0.05f) dt = 0.05f;
        prev_tick = now;

        /* Poll all four XInput slots. */
        gamepad_poll_all(infos);

        DWORD connected_mask = get_connected_mask(infos);
        if (connected_mask != s_connected_mask) {
            gamepad_detect_controllers(infos);
            s_connected_mask = connected_mask;
        }

        gamepad_reconcile_active_slot(infos);

        /* Per-slot: mode FSM + mouse injection. */
        for (int i = 0; i < XUSER_MAX_COUNT; i++) {
            if (!infos[i].connected) continue;

            AppMode mode_before = infos[i].slot_state.mode;
            bool changed = gamepad_update_mode_slot(&infos[i].state,
                                                    &infos[i].slot_state);
            if (!changed && mode_before == MODE_MOUSE)
                process_mouse_mode(&infos[i].state, dt, &s_mouse_state[i]);
        }

        /*
         * Always call tray_update_gamepads — even when all slots are
         * disconnected — so the UI immediately reflects the true state.
         * This is the fix for the "gamepad not removed on disconnect" bug.
         */
        tray_update_gamepads(infos);

        Sleep(POLL_INTERVAL_MS);
    }

    tray_shutdown();
    gamepad_shutdown();
    return 0;
}

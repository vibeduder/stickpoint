/*
 * StickPoint — main entry point
 *
 * Runs as a background process (no console window, WinMain entry).
 * Creates a message-only window so it can receive WM_QUIT cleanly.
 * Polls the gamepad at POLL_INTERVAL_MS and, when in mouse mode,
 * translates stick/button input into injected mouse events.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <math.h>
#include <stdbool.h>

#include "config.h"
#include "gamepad.h"
#include "mouse.h"

/* ---- globals ------------------------------------------------------------ */

static volatile bool s_running = true;
static HWND          s_hwnd    = NULL;

/* ---- hidden message-only window ----------------------------------------- */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)hwnd; (void)wp; (void)lp;
    if (msg == WM_DESTROY) {
        s_running = false;
        PostQuitMessage(0);
        return 0;
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

    /* HWND_MESSAGE creates an invisible message-only window. */
    s_hwnd = CreateWindowExA(0, "StickPointWnd", "StickPoint",
                              0, 0, 0, 0, 0,
                              HWND_MESSAGE, NULL, hInst, NULL);
    return s_hwnd != NULL;
}

/* ---- stick normalisation ------------------------------------------------ */

/*
 * Map a raw stick axis value to [-1, 1] after applying the deadzone and the
 * configurable acceleration curve.  Returns 0 if inside the dead zone.
 */
static float normalize_axis(SHORT raw)
{
    float v    = (float)raw;
    float sign = (v >= 0.0f) ? 1.0f : -1.0f;
    float mag  = fabsf(v);

    if (mag < (float)STICK_DEADZONE)
        return 0.0f;

    /* Remap [deadzone, 32767] → [0, 1] */
    mag = (mag - (float)STICK_DEADZONE) / (32767.0f - (float)STICK_DEADZONE);
    if (mag > 1.0f) mag = 1.0f;

    /* Non-linear curve */
    mag = powf(mag, MOUSE_ACCEL_EXPONENT);

    return sign * mag;
}

/* ---- mouse-mode processing ---------------------------------------------- */

/*
 * Double-click detection for the A button.
 * We track the time of the last A release.  A second press within
 * DOUBLE_CLICK_MS triggers mouse_double_click() instead of a normal click.
 */
static DWORD s_a_release_time  = 0;
static bool  s_a_pending_dc    = false;   /* next A press → double-click  */
static bool  s_a_held          = false;   /* A is currently physically held */

/*
 * Scroll fractional accumulator: bumper/stick inputs are continuous so we
 * accumulate fractional WHEEL_DELTA ticks and flush whole ticks each frame.
 */
static float s_scroll_acc = 0.0f;

/*
 * Trigger click state: debounce the analog trigger so we emit exactly one
 * down/up pair per threshold crossing.
 */
static bool s_lt_held = false;
static bool s_rt_held = false;

static void process_mouse_mode(const GamepadState *state, float dt)
{
    /* ---- cursor movement (left stick) ----------------------------------- */
    float nx =  normalize_axis(state->lx);
    float ny = -normalize_axis(state->ly);   /* Y axis is inverted in screen space */

    if (nx != 0.0f || ny != 0.0f) {
        mouse_move(nx * MOUSE_MAX_SPEED * dt,
                   ny * MOUSE_MAX_SPEED * dt);
    }

    /* ---- A button: left click with double-click detection --------------- */
    if (GP_JUST_PRESSED(state, XINPUT_GAMEPAD_A)) {
        if (s_a_pending_dc) {
            /* Second rapid press → double click; no held-state to track */
            mouse_double_click(MOUSE_BTN_LEFT);
            s_a_pending_dc = false;
            s_a_held       = false;
        } else {
            mouse_button_down(MOUSE_BTN_LEFT);
            s_a_held = true;
        }
    }
    if (GP_JUST_RELEASED(state, XINPUT_GAMEPAD_A) && s_a_held) {
        mouse_button_up(MOUSE_BTN_LEFT);
        s_a_held           = false;
        s_a_release_time   = state->timestamp_ms;
        s_a_pending_dc     = true;   /* arm double-click window */
    }
    /* Expire the double-click window */
    if (s_a_pending_dc &&
        (state->timestamp_ms - s_a_release_time) >= (DWORD)DOUBLE_CLICK_MS) {
        s_a_pending_dc = false;
    }

    /* ---- B button: right click ----------------------------------------- */
    if (GP_JUST_PRESSED(state, XINPUT_GAMEPAD_B))
        mouse_button_down(MOUSE_BTN_RIGHT);
    if (GP_JUST_RELEASED(state, XINPUT_GAMEPAD_B))
        mouse_button_up(MOUSE_BTN_RIGHT);

    /* ---- Y button: middle click ----------------------------------------- */
    if (GP_JUST_PRESSED(state, XINPUT_GAMEPAD_Y))
        mouse_click(MOUSE_BTN_MIDDLE);

    /* ---- Left trigger: left click (analog, held = drag) ----------------- */
    {
        bool lt_active = state->lt > TRIGGER_THRESHOLD;
        if (lt_active && !s_lt_held) {
            mouse_button_down(MOUSE_BTN_LEFT);
            s_lt_held = true;
        } else if (!lt_active && s_lt_held) {
            mouse_button_up(MOUSE_BTN_LEFT);
            s_lt_held = false;
        }
    }

    /* ---- Right trigger: right click (analog, held = drag) --------------- */
    {
        bool rt_active = state->rt > TRIGGER_THRESHOLD;
        if (rt_active && !s_rt_held) {
            mouse_button_down(MOUSE_BTN_RIGHT);
            s_rt_held = true;
        } else if (!rt_active && s_rt_held) {
            mouse_button_up(MOUSE_BTN_RIGHT);
            s_rt_held = false;
        }
    }

    /* ---- LB / RB: discrete scroll ------------------------------------- */
    if (state->buttons & XINPUT_GAMEPAD_LEFT_SHOULDER)
        s_scroll_acc += SCROLL_SPEED * dt;
    if (state->buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER)
        s_scroll_acc -= SCROLL_SPEED * dt;

    /* ---- Right stick Y: fine scroll ------------------------------------ */
    {
        float ry = normalize_axis(state->ry);
        if (ry != 0.0f)
            s_scroll_acc += ry * SCROLL_SPEED * dt;
    }

    /* Flush whole scroll ticks */
    if (fabsf(s_scroll_acc) >= 1.0f) {
        int ticks    = (int)s_scroll_acc;
        s_scroll_acc -= (float)ticks;
        mouse_scroll(ticks);
    }
}

/* ---- entry point -------------------------------------------------------- */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;

    /*
     * Tell Windows this process is DPI-aware so that SendInput coordinates
     * are not scaled by the DPI virtualisation layer.
     */
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

    GamepadState state    = {0};
    DWORD        prev_tick = GetTickCount();

    while (s_running) {
        /* Non-blocking message pump */
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                s_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!s_running)
            break;

        /* Delta time (seconds), capped to avoid large jumps after sleep. */
        DWORD now = GetTickCount();
        float dt  = (float)(now - prev_tick) / 1000.0f;
        if (dt > 0.05f) dt = 0.05f;
        prev_tick = now;

        if (!gamepad_poll(&state)) {
            /* Controller disconnected — retry next frame. */
            Sleep(POLL_INTERVAL_MS);
            continue;
        }

        AppMode mode_before = gamepad_get_mode();
        bool    changed     = gamepad_update_mode(&state);

        /*
         * Only process mouse events if we were already in mouse mode before
         * this frame's update.  This prevents the A press that activates
         * mouse mode from also registering as a click.
         */
        if (!changed && mode_before == MODE_MOUSE)
            process_mouse_mode(&state, dt);

        Sleep(POLL_INTERVAL_MS);
    }

    gamepad_shutdown();
    return 0;
}

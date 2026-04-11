#include "gamepad.h"
#include "config.h"

/* ---- XInput runtime binding -------------------------------------------- */

/*
 * XInputGetStateEx is exported from XInput1_4.dll as ordinal 100 (not by
 * name).  It returns the same data as XInputGetState but also sets the
 * XINPUT_GAMEPAD_GUIDE bit in wButtons when the Guide/Home button is held.
 */
typedef DWORD (WINAPI *PFN_GetStateEx)(DWORD, XINPUT_STATE_EX *);
typedef DWORD (WINAPI *PFN_GetState)  (DWORD, XINPUT_STATE *);

static HMODULE        s_dll          = NULL;
static PFN_GetStateEx s_GetStateEx   = NULL;
static PFN_GetState   s_GetState     = NULL;

/* ---- mode state machine ------------------------------------------------- */

static AppMode s_mode               = MODE_NORMAL;

/* Timestamp when Guide was first pressed this press-cycle. */
static DWORD   s_guide_down_time    = 0;
static bool    s_guide_active       = false;   /* Guide is currently held  */
static bool    s_guide_combo_armed  = false;   /* waiting for the A press  */

/* ---- public API --------------------------------------------------------- */

bool gamepad_init(void)
{
    /* Prefer XInput 1.4 (Win8+, ships with Windows 10). */
    s_dll = LoadLibraryA("XInput1_4.dll");
    if (!s_dll)
        s_dll = LoadLibraryA("XInput1_3.dll");
    if (!s_dll)
        return false;

    /* Ordinal 100 → XInputGetStateEx (exposes Guide button). */
    s_GetStateEx = (PFN_GetStateEx)GetProcAddress(s_dll, (LPCSTR)(ULONG_PTR)100);
    /* Fallback: standard XInputGetState (Guide button NOT available). */
    s_GetState   = (PFN_GetState)  GetProcAddress(s_dll, "XInputGetState");

    return (s_GetStateEx != NULL || s_GetState != NULL);
}

bool gamepad_poll(GamepadState *state)
{
    DWORD result;
    XINPUT_GAMEPAD gp = {0};

    if (s_GetStateEx) {
        XINPUT_STATE_EX xs = {0};
        result = s_GetStateEx(0, &xs);
        if (result != ERROR_SUCCESS) goto disconnected;
        gp = xs.Gamepad;
    } else {
        XINPUT_STATE xs = {0};
        result = s_GetState(0, &xs);
        if (result != ERROR_SUCCESS) goto disconnected;
        gp = xs.Gamepad;
    }

    state->prev_buttons = state->buttons;
    state->prev_lt      = state->lt;
    state->prev_rt      = state->rt;

    state->buttons      = gp.wButtons;
    state->lx           = gp.sThumbLX;
    state->ly           = gp.sThumbLY;
    state->rx           = gp.sThumbRX;
    state->ry           = gp.sThumbRY;
    state->lt           = gp.bLeftTrigger;
    state->rt           = gp.bRightTrigger;
    state->timestamp_ms = GetTickCount();
    state->connected    = true;
    return true;

disconnected:
    state->connected = false;
    return false;
}

void gamepad_shutdown(void)
{
    if (s_dll) {
        FreeLibrary(s_dll);
        s_dll        = NULL;
        s_GetStateEx = NULL;
        s_GetState   = NULL;
    }
}

AppMode gamepad_get_mode(void)
{
    return s_mode;
}

bool gamepad_update_mode(GamepadState *state)
{
    AppMode prev_mode = s_mode;

    bool guide_now  = (state->buttons      & XINPUT_GAMEPAD_GUIDE) != 0;
    bool guide_prev = (state->prev_buttons & XINPUT_GAMEPAD_GUIDE) != 0;
    bool a_just_pressed = GP_JUST_PRESSED(state, XINPUT_GAMEPAD_A) != 0;

    /* Detect Guide button leading edge. */
    if (guide_now && !guide_prev) {
        s_guide_down_time   = state->timestamp_ms;
        s_guide_active      = true;
        s_guide_combo_armed = true;
    }

    /* Guide released. */
    if (!guide_now && guide_prev) {
        s_guide_active      = false;
        s_guide_combo_armed = false;
    }

    /*
     * Combo: Guide held + A pressed within COMBO_TIMEOUT_MS.
     * Toggle the mode and consume the combo so A doesn't also fire a click.
     */
    if (s_guide_combo_armed && guide_now && a_just_pressed) {
        DWORD held = state->timestamp_ms - s_guide_down_time;
        if (held < (DWORD)COMBO_TIMEOUT_MS) {
            s_mode = (s_mode == MODE_NORMAL) ? MODE_MOUSE : MODE_NORMAL;
            s_guide_combo_armed = false;
            /*
             * Mask out the A button in prev_buttons so the caller sees it
             * as NOT just-pressed this frame (suppresses spurious click).
             */
            state->prev_buttons |= XINPUT_GAMEPAD_A;
        }
    }

    /*
     * Guide alone held for GUIDE_HOLD_EXIT_MS → force Normal mode.
     * Only fires when no other button was pressed during the hold.
     */
    if (s_mode == MODE_MOUSE && s_guide_active && s_guide_combo_armed) {
        DWORD held = state->timestamp_ms - s_guide_down_time;
        if (held >= (DWORD)GUIDE_HOLD_EXIT_MS) {
            s_mode              = MODE_NORMAL;
            s_guide_combo_armed = false;
        }
    }

    return (s_mode != prev_mode);
}

#include "mouse.h"

/* Sub-pixel accumulators: carry fractional pixels across frames. */
static float s_acc_x = 0.0f;
static float s_acc_y = 0.0f;

/* ---- internal helpers --------------------------------------------------- */

static const DWORD k_down_flag[3] = {
    MOUSEEVENTF_LEFTDOWN,
    MOUSEEVENTF_RIGHTDOWN,
    MOUSEEVENTF_MIDDLEDOWN,
};

static const DWORD k_up_flag[3] = {
    MOUSEEVENTF_LEFTUP,
    MOUSEEVENTF_RIGHTUP,
    MOUSEEVENTF_MIDDLEUP,
};

/* ---- public API --------------------------------------------------------- */

void mouse_move(float dx, float dy)
{
    s_acc_x += dx;
    s_acc_y += dy;

    int ix = (int)s_acc_x;
    int iy = (int)s_acc_y;

    if (ix == 0 && iy == 0)
        return;

    s_acc_x -= (float)ix;
    s_acc_y -= (float)iy;

    INPUT in = {0};
    in.type    = INPUT_MOUSE;
    in.mi.dx   = (LONG)ix;
    in.mi.dy   = (LONG)iy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(INPUT));
}

void mouse_button_down(MouseButton btn)
{
    INPUT in = {0};
    in.type       = INPUT_MOUSE;
    in.mi.dwFlags = k_down_flag[btn];
    SendInput(1, &in, sizeof(INPUT));
}

void mouse_button_up(MouseButton btn)
{
    INPUT in = {0};
    in.type       = INPUT_MOUSE;
    in.mi.dwFlags = k_up_flag[btn];
    SendInput(1, &in, sizeof(INPUT));
}

void mouse_click(MouseButton btn)
{
    INPUT ins[2] = {0};
    ins[0].type       = INPUT_MOUSE;
    ins[0].mi.dwFlags = k_down_flag[btn];
    ins[1].type       = INPUT_MOUSE;
    ins[1].mi.dwFlags = k_up_flag[btn];
    SendInput(2, ins, sizeof(INPUT));
}

void mouse_double_click(MouseButton btn)
{
    /*
     * Send four events (down/up/down/up) in one SendInput call so they land
     * within the same message pump batch — virtually guaranteed to be inside
     * the system double-click interval.
     */
    INPUT ins[4] = {0};
    for (int i = 0; i < 4; ++i) {
        ins[i].type       = INPUT_MOUSE;
        ins[i].mi.dwFlags = (i % 2 == 0) ? k_down_flag[btn] : k_up_flag[btn];
    }
    SendInput(4, ins, sizeof(INPUT));
}

void mouse_scroll(int delta)
{
    if (delta == 0)
        return;
    INPUT in = {0};
    in.type           = INPUT_MOUSE;
    in.mi.dwFlags     = MOUSEEVENTF_WHEEL;
    in.mi.mouseData   = (DWORD)(delta * WHEEL_DELTA);
    SendInput(1, &in, sizeof(INPUT));
}

#include "mouse.h"

#include <array>

namespace stickpoint {

namespace {

constexpr std::array<DWORD, 3> kDownFlags = {
    MOUSEEVENTF_LEFTDOWN,
    MOUSEEVENTF_RIGHTDOWN,
    MOUSEEVENTF_MIDDLEDOWN,
};

constexpr std::array<DWORD, 3> kUpFlags = {
    MOUSEEVENTF_LEFTUP,
    MOUSEEVENTF_RIGHTUP,
    MOUSEEVENTF_MIDDLEUP,
};

}  // namespace

DWORD MouseInjector::buttonFlag(MouseButton button, bool isDown)
{
    const size_t index = static_cast<size_t>(button);
    return isDown ? kDownFlags[index] : kUpFlags[index];
}

void MouseInjector::move(float dx, float dy)
{
    accX_ += dx;
    accY_ += dy;

    const int deltaX = static_cast<int>(accX_);
    const int deltaY = static_cast<int>(accY_);

    if (deltaX == 0 && deltaY == 0) {
        return;
    }

    accX_ -= static_cast<float>(deltaX);
    accY_ -= static_cast<float>(deltaY);

    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>(deltaX);
    input.mi.dy = static_cast<LONG>(deltaY);
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));
}

void MouseInjector::buttonDown(MouseButton button) const
{
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = buttonFlag(button, true);
    SendInput(1, &input, sizeof(INPUT));
}

void MouseInjector::buttonUp(MouseButton button) const
{
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = buttonFlag(button, false);
    SendInput(1, &input, sizeof(INPUT));
}

void MouseInjector::click(MouseButton button) const
{
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = buttonFlag(button, true);
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = buttonFlag(button, false);
    SendInput(2, inputs, sizeof(INPUT));
}

void MouseInjector::doubleClick(MouseButton button) const
{
    INPUT inputs[4] = {};
    for (int index = 0; index < 4; ++index) {
        inputs[index].type = INPUT_MOUSE;
        inputs[index].mi.dwFlags = buttonFlag(button, index % 2 == 0);
    }
    SendInput(4, inputs, sizeof(INPUT));
}

void MouseInjector::scroll(int delta) const
{
    if (delta == 0) {
        return;
    }

    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(delta * WHEEL_DELTA);
    SendInput(1, &input, sizeof(INPUT));
}

}  // namespace stickpoint
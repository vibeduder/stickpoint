#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace stickpoint {

enum class MouseButton {
    Left = 0,
    Right = 1,
    Middle = 2,
};

class MouseInjector {
public:
    void move(float dx, float dy);
    void buttonDown(MouseButton button) const;
    void buttonUp(MouseButton button) const;
    void click(MouseButton button) const;
    void doubleClick(MouseButton button) const;
    void scroll(int delta) const;

private:
    static DWORD buttonFlag(MouseButton button, bool isDown);

    float accX_ = 0.0f;
    float accY_ = 0.0f;
};

}  // namespace stickpoint

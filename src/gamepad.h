#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>

#include <array>
#include <memory>
#include <string>

/*
 * StickPoint — gamepad input layer (XInput wrapper)
 *
 * Supports all four XInput slots simultaneously.
 * The Guide/Home button is accessed via XInputGetStateEx (ordinal 100).
 */

/* Guide/Home button bit — not present in public <xinput.h>. */
#ifndef XINPUT_GAMEPAD_GUIDE
#define XINPUT_GAMEPAD_GUIDE    0x0400
#endif

/*
 * Extended state structure returned by XInputGetStateEx (ordinal 100).
 * Identical layout to XINPUT_STATE; the Guide bit appears in wButtons.
 */
struct XINPUT_STATE_EX {
    DWORD dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
};

/* -------------------------------------------------------------------------
 * Application mode
 * ------------------------------------------------------------------------- */
enum AppMode {
    MODE_NORMAL = 0,
    MODE_MOUSE = 1,
};

/* -------------------------------------------------------------------------
 * Per-frame gamepad snapshot
 * ------------------------------------------------------------------------- */
struct GamepadState {
    WORD buttons = 0;
    WORD prev_buttons = 0;
    DWORD packet_number = 0;
    DWORD prev_packet_number = 0;

    SHORT lx = 0;
    SHORT ly = 0;
    SHORT rx = 0;
    SHORT ry = 0;

    BYTE lt = 0;
    BYTE rt = 0;
    BYTE prev_lt = 0;
    BYTE prev_rt = 0;

    DWORD timestamp_ms = 0;
    bool connected = false;

    bool justPressed(WORD button) const;
    bool justReleased(WORD button) const;
    bool held(WORD button) const;
    void beginFrame();
    void applySnapshot(const XINPUT_GAMEPAD& gamepad, DWORD packetNumber, DWORD timestampMs);
    void markDisconnected();
};

/* -------------------------------------------------------------------------
 * Per-slot mode state machine + configurable combo
 * ------------------------------------------------------------------------- */
struct GamepadSlotState {
    AppMode mode = MODE_NORMAL;
    DWORD guide_down_time = 0;
    bool guide_active = false;
    bool guide_combo_armed = false;
    WORD combo_hold_btn = XINPUT_GAMEPAD_GUIDE;
    WORD combo_press_btn = XINPUT_GAMEPAD_A;
    DWORD combo_timeout_ms = 0;

    bool updateMode(GamepadState& state);
};

/* -------------------------------------------------------------------------
 * All information about one controller (used by the UI)
 * ------------------------------------------------------------------------- */
struct GamepadInfo {
    int slot = 0;
    bool connected = false;
    GamepadState state = {};
    GamepadSlotState slot_state = {};
    std::string name;
    std::wstring image;

    void prepareForPoll(int slotIndex);
    void applySnapshot(const XINPUT_GAMEPAD& gamepad, DWORD packetNumber, DWORD timestampMs);
    void markDisconnected();
};

using GamepadInfos = std::array<GamepadInfo, XUSER_MAX_COUNT>;

class GamepadConfigStore {
public:
    static void load(GamepadInfos& infos);
    static void save(const GamepadInfos& infos);

private:
    static std::wstring iniPath();
};

class GamepadManager {
public:
    GamepadManager();
    ~GamepadManager();

    GamepadManager(const GamepadManager&) = delete;
    GamepadManager& operator=(const GamepadManager&) = delete;
    GamepadManager(GamepadManager&&) = delete;
    GamepadManager& operator=(GamepadManager&&) = delete;

    bool init();
    bool registerRawInput(HWND hwnd);
    void pollAll(GamepadInfos& infos);
    void detectControllers(GamepadInfos& infos);
    void handleRawInput(HRAWINPUT rawInput);
    void reconcileActiveSlot(GamepadInfos& infos);
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/*
 * StickPoint — gamepad input layer
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#include <initguid.h>
#include <devpkey.h>
#include <setupapi.h>
#include <hidsdi.h>

#include <array>
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gamepad.h"
#include "config.h"

namespace {

using PFN_GetStateEx = DWORD(WINAPI*)(DWORD, XINPUT_STATE_EX*);
using PFN_GetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using PFN_GetCapabilitiesEx = DWORD(WINAPI*)(DWORD, DWORD, DWORD, void*);

struct XInputCapabilitiesEx {
    XINPUT_CAPABILITIES capabilities;
    WORD vendor_id;
    WORD product_id;
    WORD product_version;
    WORD reserved;
    DWORD unk2;
};

constexpr int DEVICE_INSTANCE_ID_CHARS = 256;
constexpr int HID_PATH_CHARS = 512;
constexpr int kMaxHidEntries = XUSER_MAX_COUNT * 4;

struct HidEntry {
    std::string path;
    std::string name;
    std::string instanceId;
    std::optional<GUID> containerId;
    WORD vid = 0;
    WORD pid = 0;
};

struct RecentRawInput {
    bool valid = false;
    DWORD timestampMs = 0;
    std::string path;
};

constexpr struct {
    WORD vid;
    WORD pid;
    const wchar_t* image;
} kKnownControllers[] = {
    {0x045E, 0x028E, L"xbox-360.png"},
    {0x045E, 0x028F, L"xbox-360.png"},
    {0x045E, 0x0291, L"xbox-360.png"},
    {0x045E, 0x02D1, L"xbox-one.png"},
    {0x045E, 0x02DD, L"xbox-one.png"},
    {0x045E, 0x02EA, L"xbox-one.png"},
    {0x045E, 0x02FD, L"xbox-one.png"},
    {0x045E, 0x0719, L"xbox-one.png"},
    {0x045E, 0x0B12, L"xbox-one.png"},
    {0x045E, 0x02E3, L"xbox-elite-1.png"},
    {0x045E, 0x02FF, L"xbox-elite-1.png"},
    {0x045E, 0x0B00, L"xbox-elite-2.png"},
    {0x045E, 0x0B05, L"xbox-elite-2.png"},
    {0x045E, 0x0B0A, L"xbox-elite-2.png"},
    {0x2DC8, 0x3012, L"8bitdo-ultimate.png"},
    {0x2DC8, 0x3016, L"8bitdo-ultimate.png"},
    {0x1532, 0x0900, L"razer-volverine-v3.png"},
    {0x1532, 0x0A14, L"razer-volverine-v3.png"},
    {0x1532, 0x0A29, L"razer-volverine-v3.png"},
    {0x3537, 0x1015, L"gamesir-t7.png"},
    {0x146B, 0x0604, L"nacon.png"},
    {0x146B, 0x0605, L"nacon.png"},
    {0x24C6, 0x543A, L"power-a.png"},
    {0x24C6, 0x5300, L"power-a.png"},
    {0x20D6, 0x2001, L"power-a.png"},
    {0x20D6, 0x2002, L"power-a.png"},
};

class PathResolver {
public:
    static std::filesystem::path moduleDirectory()
    {
        std::array<wchar_t, MAX_PATH> path = {};
        GetModuleFileNameW(NULL, path.data(), static_cast<DWORD>(path.size()));
        return std::filesystem::path(path.data()).parent_path();
    }

    static std::wstring buildImagePath(std::wstring_view filename)
    {
        return (moduleDirectory() / L"assets" / L"controllers" / filename).wstring();
    }

    static std::wstring iniPath()
    {
        return (moduleDirectory() / L"StickPoint.ini").wstring();
    }
};

class XInputRuntime {
public:
    bool init()
    {
        dll_ = LoadLibraryA("XInput1_4.dll");
        if (!dll_) {
            dll_ = LoadLibraryA("XInput1_3.dll");
        }
        if (!dll_) {
            return false;
        }

        getStateEx_ = reinterpret_cast<PFN_GetStateEx>(GetProcAddress(
            dll_, reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(100))));
        getState_ = reinterpret_cast<PFN_GetState>(GetProcAddress(dll_, "XInputGetState"));
        getCapabilitiesEx_ = reinterpret_cast<PFN_GetCapabilitiesEx>(GetProcAddress(
            dll_, reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(108))));

        return getStateEx_ != NULL || getState_ != NULL;
    }

    void shutdown()
    {
        if (dll_) {
            FreeLibrary(dll_);
        }

        dll_ = NULL;
        getStateEx_ = NULL;
        getState_ = NULL;
        getCapabilitiesEx_ = NULL;
    }

    bool pollSlot(DWORD index, GamepadInfo& info) const
    {
        XINPUT_GAMEPAD gamepad = {};
        DWORD packet_number = 0;
        DWORD result = ERROR_DEVICE_NOT_CONNECTED;

        if (getStateEx_) {
            XINPUT_STATE_EX extended_state = {};
            result = getStateEx_(index, &extended_state);
            if (result == ERROR_SUCCESS) {
                gamepad = extended_state.Gamepad;
                packet_number = extended_state.dwPacketNumber;
            }
        } else if (getState_) {
            XINPUT_STATE state_value = {};
            result = getState_(index, &state_value);
            if (result == ERROR_SUCCESS) {
                gamepad = state_value.Gamepad;
                packet_number = state_value.dwPacketNumber;
            }
        }

        if (result != ERROR_SUCCESS) {
            return false;
        }

        info.applySnapshot(gamepad, packet_number, GetTickCount());
        return true;
    }

    bool getSlotVidPid(int slot, WORD* vid, WORD* pid) const
    {
        XInputCapabilitiesEx capabilities = {};

        if (!getCapabilitiesEx_) {
            return false;
        }

        if (getCapabilitiesEx_(1, static_cast<DWORD>(slot), XINPUT_FLAG_GAMEPAD, &capabilities) != ERROR_SUCCESS) {
            return false;
        }

        if (capabilities.vendor_id == 0 && capabilities.product_id == 0) {
            return false;
        }

        if (capabilities.product_id == 0 && (capabilities.capabilities.Flags & XINPUT_CAPS_WIRELESS)) {
            capabilities.vendor_id = 0x045E;
            capabilities.product_id = 0x028E;
        }

        *vid = capabilities.vendor_id;
        *pid = capabilities.product_id;
        return true;
    }

private:
    HMODULE dll_ = NULL;
    PFN_GetStateEx getStateEx_ = NULL;
    PFN_GetState getState_ = NULL;
    PFN_GetCapabilitiesEx getCapabilitiesEx_ = NULL;
};

class HidDeviceCatalog {
public:
    void clear()
    {
        hid_entries_.fill({});
        hid_entry_count_ = 0;
        slot_binding_ = {-1, -1, -1, -1};
        recent_raw_ = {};
    }

    void detect(GamepadInfos& infos, const XInputRuntime& runtime)
    {
        resetDefaultControllerInfo(infos);

        GUID hid_guid = {};
        HidD_GetHidGuid(&hid_guid);

        HDEVINFO dev_info = SetupDiGetClassDevsA(&hid_guid, NULL, NULL,
                                                 DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
        if (dev_info == INVALID_HANDLE_VALUE) {
            return;
        }

        std::array<HidEntry, kMaxHidEntries> entries = {};
        int entry_count = 0;

        SP_DEVICE_INTERFACE_DATA interface_data = {};
        interface_data.cbSize = sizeof(interface_data);

        for (DWORD index = 0;
             SetupDiEnumDeviceInterfaces(dev_info, NULL, &hid_guid, index, &interface_data);
             ++index) {
            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailA(dev_info, &interface_data, NULL, 0, &needed, NULL);
            if (needed == 0 || needed > 512 + offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_A, DevicePath)) {
                continue;
            }

            auto* detail = static_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A*>(LocalAlloc(LMEM_ZEROINIT, needed));
            if (!detail) {
                continue;
            }
            detail->cbSize = sizeof(*detail);

            SP_DEVINFO_DATA device_data = {};
            device_data.cbSize = sizeof(device_data);

            if (!SetupDiGetDeviceInterfaceDetailA(dev_info, &interface_data, detail, needed, NULL, &device_data)) {
                LocalFree(detail);
                continue;
            }

            const std::string devicePath = detail->DevicePath;
            if (!isXInputDevicePath(devicePath)) {
                LocalFree(detail);
                continue;
            }

            const auto vidPid = parseVidPid(devicePath);
            if (!vidPid) {
                LocalFree(detail);
                continue;
            }
            const auto [vid, pid] = *vidPid;

            const std::optional<GUID> containerId = getContainerId(dev_info, &device_data);

            const std::string instanceId = getDeviceInstanceId(dev_info, &device_data);

            const std::string productName = readProductName(devicePath, vid, pid);

            int existing = -1;
            for (int entry_index = 0; entry_index < entry_count; ++entry_index) {
                const bool sameContainer = entries[entry_index].containerId && containerId &&
                                           IsEqualGUID(*entries[entry_index].containerId, *containerId);
                const bool sameInstance = !instanceId.empty() && !entries[entry_index].instanceId.empty() &&
                                          entries[entry_index].instanceId == instanceId;
                if (sameContainer || sameInstance) {
                    existing = entry_index;
                    break;
                }
            }

            if (existing >= 0) {
                HidEntry* entry = &entries[existing];
                if (scoreProductName(productName) > scoreProductName(entry->name)) {
                    entry->name = productName;
                }
                if (!entry->containerId && containerId) {
                    entry->containerId = containerId;
                }
                if (entry->instanceId.empty() && !instanceId.empty()) {
                    entry->instanceId = instanceId;
                }
                LocalFree(detail);
                continue;
            }

            if (entry_count >= static_cast<int>(entries.size())) {
                LocalFree(detail);
                break;
            }

            HidEntry* entry = &entries[entry_count++];
            entry->path = devicePath;
            entry->name = productName;
            entry->instanceId = instanceId;
            entry->containerId = containerId;
            entry->vid = vid;
            entry->pid = pid;
            LocalFree(detail);
        }

        SetupDiDestroyDeviceInfoList(dev_info);

        rebuildCachedEntries(entries.data(), entry_count);
        applyCachedBindings(infos, runtime);
    }

    void handleRawInput(HRAWINPUT raw_input)
    {
        RAWINPUTHEADER header = {};
        UINT size = sizeof(header);

        if (GetRawInputData(raw_input, RID_HEADER, &header, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) {
            return;
        }

        if (header.dwType != RIM_TYPEHID || header.hDevice == NULL) {
            return;
        }

        std::array<char, HID_PATH_CHARS> path = {};
        size = static_cast<UINT>(path.size());
        if (GetRawInputDeviceInfoA(header.hDevice, RIDI_DEVICENAME, path.data(), &size) == static_cast<UINT>(-1)) {
            return;
        }

        if (!isXInputDevicePath(path.data())) {
            return;
        }

        recent_raw_.path = path.data();
        recent_raw_.timestampMs = GetTickCount();
        recent_raw_.valid = true;
    }

    void reconcileActiveSlot(GamepadInfos& infos)
    {
        (void)infos;
        recent_raw_.valid = false;
    }

private:
    void applyCachedBindings(GamepadInfos& infos, const XInputRuntime& runtime)
    {
        std::array<bool, kMaxHidEntries> used = {};

        resetDefaultControllerInfo(infos);

        for (int slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            if (!infos[slot].connected) {
                continue;
            }

            WORD slot_vid = 0;
            WORD slot_pid = 0;
            int index = -1;

            if (runtime.getSlotVidPid(slot, &slot_vid, &slot_pid)) {
                for (int hid_index = 0; hid_index < hid_entry_count_; ++hid_index) {
                    if (used[hid_index]) {
                        continue;
                    }
                    if (hid_entries_[hid_index].vid == slot_vid && hid_entries_[hid_index].pid == slot_pid) {
                        index = hid_index;
                        break;
                    }
                }
            }

            if (index < 0) {
                const int bound_index = slot_binding_[slot];
                if (bound_index >= 0 && bound_index < hid_entry_count_ && !used[bound_index]) {
                    index = bound_index;
                }
            }

            if (index < 0) {
                continue;
            }

            slot_binding_[slot] = index;
            infos[slot].name = hid_entries_[index].name;
            infos[slot].image = PathResolver::buildImagePath(lookupControllerImage(hid_entries_[index].vid,
                                                                                   hid_entries_[index].pid));
            used[index] = true;
        }

        int next_unbound = 0;
        for (int slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            if (!infos[slot].connected) {
                continue;
            }

            const int index = slot_binding_[slot];
            if (index >= 0 && index < hid_entry_count_ && used[index]) {
                continue;
            }

            while (next_unbound < hid_entry_count_ && used[next_unbound]) {
                ++next_unbound;
            }
            if (next_unbound >= hid_entry_count_) {
                break;
            }

            slot_binding_[slot] = next_unbound;
            infos[slot].name = hid_entries_[next_unbound].name;
            infos[slot].image = PathResolver::buildImagePath(lookupControllerImage(hid_entries_[next_unbound].vid,
                                                                                   hid_entries_[next_unbound].pid));
            used[next_unbound] = true;
        }
    }

    void rebuildCachedEntries(const HidEntry* entries, int entry_count)
    {
        std::array<int, XUSER_MAX_COUNT> new_binding = {-1, -1, -1, -1};

        for (int slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            const int old_index = slot_binding_[slot];
            if (old_index < 0 || old_index >= hid_entry_count_) {
                continue;
            }

            for (int entry_index = 0; entry_index < entry_count; ++entry_index) {
                if (hidEntriesMatch(hid_entries_[old_index], entries[entry_index])) {
                    new_binding[slot] = entry_index;
                    break;
                }
            }
        }

        hid_entries_.fill({});
        for (int entry_index = 0; entry_index < entry_count; ++entry_index) {
            hid_entries_[entry_index] = entries[entry_index];
        }
        hid_entry_count_ = entry_count;
        slot_binding_ = new_binding;
    }

    std::array<HidEntry, kMaxHidEntries> hid_entries_ = {};
    int hid_entry_count_ = 0;
    std::array<int, XUSER_MAX_COUNT> slot_binding_ = {-1, -1, -1, -1};
    RecentRawInput recent_raw_ = {};

    static bool isXInputDevicePath(std::string_view path)
    {
        return path.find("IG_") != std::string_view::npos || path.find("ig_") != std::string_view::npos;
    }

    static std::optional<WORD> parseHexWord(std::string_view value)
    {
        unsigned parsed = 0;
        const char* begin = value.data();
        const char* end = begin + value.size();
        const auto [ptr, error] = std::from_chars(begin, end, parsed, 16);
        if (error != std::errc() || ptr == begin) {
            return std::nullopt;
        }
        return static_cast<WORD>(parsed);
    }

    static std::optional<std::pair<WORD, WORD>> parseVidPid(std::string_view path)
    {
        const size_t vidPosition = path.find("VID_");
        const size_t altVidPosition = path.find("vid_");
        const size_t resolvedVidPosition = (vidPosition != std::string_view::npos) ? vidPosition : altVidPosition;
        if (resolvedVidPosition == std::string_view::npos || resolvedVidPosition + 8 > path.size()) {
            return std::nullopt;
        }

        const size_t pidPosition = path.find("PID_");
        const size_t altPidPosition = path.find("pid_");
        const size_t resolvedPidPosition = (pidPosition != std::string_view::npos) ? pidPosition : altPidPosition;
        if (resolvedPidPosition == std::string_view::npos || resolvedPidPosition + 8 > path.size()) {
            return std::nullopt;
        }

        const auto vid = parseHexWord(path.substr(resolvedVidPosition + 4, 4));
        const auto pid = parseHexWord(path.substr(resolvedPidPosition + 4, 4));
        if (!vid || !pid) {
            return std::nullopt;
        }

        return std::pair<WORD, WORD>(*vid, *pid);
    }

    static std::wstring_view lookupControllerImage(WORD vid, WORD pid)
    {
        for (const auto& controller : kKnownControllers) {
            if (controller.vid == vid && controller.pid == pid) {
                return controller.image;
            }
        }
        return L"xbox-360.png";
    }

    static int scoreProductName(std::string_view name)
    {
        if (name.empty()) {
            return 0;
        }

        if (name.substr(0, 16) == "Controller (VID ") {
            return 1;
        }

        if (name.size() <= 1) {
            return 1;
        }

        const bool digitsOnly = std::all_of(name.begin(), name.end(), [](char character) {
            return character >= '0' && character <= '9';
        });

        return digitsOnly ? 1 : 2;
    }

    static std::string trimWhitespace(std::string_view value)
    {
        const auto start = value.find_first_not_of(" \t");
        if (start == std::string_view::npos) {
            return {};
        }

        const auto end = value.find_last_not_of(" \t");
        return std::string(value.substr(start, end - start + 1));
    }

    static void sanitizeProductName(std::string& name)
    {
        const std::string trimmed = trimWhitespace(name);
        if (trimmed.size() < 2 || trimmed.front() != '(' || trimmed.back() != ')') {
            name = trimmed;
            return;
        }

        const std::string inner = trimWhitespace(std::string_view(trimmed).substr(1, trimmed.size() - 2));
        name = inner.empty() ? trimmed : inner;
    }

    static std::string fallbackProductName(WORD vid, WORD pid)
    {
        char buffer[64] = {};
        _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "Controller (VID %04X PID %04X)", vid, pid);
        return buffer;
    }

    static std::string readProductName(const std::string& path, WORD vid, WORD pid)
    {
        HANDLE handle = CreateFileA(path.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    NULL, OPEN_EXISTING, 0, NULL);
        if (handle == INVALID_HANDLE_VALUE) {
            return fallbackProductName(vid, pid);
        }

        std::array<wchar_t, 256> wideName = {};
        if (!HidD_GetProductString(handle, wideName.data(), static_cast<ULONG>(wideName.size() * sizeof(wchar_t))) ||
            wideName[0] == L'\0') {
            CloseHandle(handle);
            return fallbackProductName(vid, pid);
        }

        const int required = WideCharToMultiByte(CP_UTF8, 0, wideName.data(), -1, NULL, 0, NULL, NULL);
        std::vector<char> buffer(static_cast<size_t>(required > 0 ? required : 1), '\0');
        if (required > 0) {
            WideCharToMultiByte(CP_UTF8, 0, wideName.data(), -1, buffer.data(), required, NULL, NULL);
        }
        CloseHandle(handle);

        std::string result = buffer.data();
        sanitizeProductName(result);
        return result.empty() ? fallbackProductName(vid, pid) : result;
    }

    static std::optional<GUID> getContainerId(HDEVINFO dev_info, SP_DEVINFO_DATA* dev_data)
    {
        DEVPROPTYPE prop_type = 0;
        GUID containerId = {};

        if (!SetupDiGetDevicePropertyW(dev_info, dev_data, &DEVPKEY_Device_ContainerId,
                                       &prop_type, reinterpret_cast<PBYTE>(&containerId),
                                       static_cast<DWORD>(sizeof(containerId)), NULL, 0) ||
            prop_type != DEVPROP_TYPE_GUID) {
            return std::nullopt;
        }

        return containerId;
    }

    static std::string getDeviceInstanceId(HDEVINFO dev_info, SP_DEVINFO_DATA* dev_data)
    {
        std::array<char, DEVICE_INSTANCE_ID_CHARS> buffer = {};
        if (!SetupDiGetDeviceInstanceIdA(dev_info, dev_data, buffer.data(), static_cast<DWORD>(buffer.size()), NULL)) {
            return {};
        }
        return buffer.data();
    }

    static bool hidEntriesMatch(const HidEntry& left, const HidEntry& right)
    {
        if (left.containerId && right.containerId && IsEqualGUID(*left.containerId, *right.containerId)) {
            return true;
        }

        if (!left.instanceId.empty() && !right.instanceId.empty() && left.instanceId == right.instanceId) {
            return true;
        }

        if (!left.path.empty() && !right.path.empty() && left.path == right.path) {
            return true;
        }

        return false;
    }

    void resetDefaultControllerInfo(GamepadInfos& infos) const
    {
        for (int index = 0; index < XUSER_MAX_COUNT; ++index) {
            infos[index].name = "Controller " + std::to_string(index + 1);
            infos[index].image = PathResolver::buildImagePath(L"xbox-360.png");
        }
    }
};

}  // namespace

bool GamepadState::justPressed(WORD button) const
{
    return ((buttons & ~prev_buttons) & button) != 0;
}

bool GamepadState::justReleased(WORD button) const
{
    return ((~buttons & prev_buttons) & button) != 0;
}

bool GamepadState::held(WORD button) const
{
    return ((buttons & prev_buttons) & button) != 0;
}

void GamepadState::beginFrame()
{
    prev_buttons = buttons;
    prev_packet_number = packet_number;
    prev_lt = lt;
    prev_rt = rt;
}

void GamepadState::applySnapshot(const XINPUT_GAMEPAD& gamepad, DWORD packetNumber, DWORD timestampMs)
{
    buttons = gamepad.wButtons;
    packet_number = packetNumber;
    lx = gamepad.sThumbLX;
    ly = gamepad.sThumbLY;
    rx = gamepad.sThumbRX;
    ry = gamepad.sThumbRY;
    lt = gamepad.bLeftTrigger;
    rt = gamepad.bRightTrigger;
    timestamp_ms = timestampMs;
    connected = true;
}

void GamepadState::markDisconnected()
{
    buttons = 0;
    prev_buttons = 0;
    packet_number = 0;
    prev_packet_number = 0;
    lx = 0;
    ly = 0;
    rx = 0;
    ry = 0;
    lt = 0;
    rt = 0;
    prev_lt = 0;
    prev_rt = 0;
    connected = false;
}

bool GamepadSlotState::updateMode(GamepadState& state)
{
    const AppMode previous_mode = mode;

    const bool hold_now = (state.buttons & combo_hold_btn) != 0;
    const bool hold_prev = (state.prev_buttons & combo_hold_btn) != 0;
    const bool press_just = ((state.buttons & ~state.prev_buttons) & combo_press_btn) != 0;

    if (hold_now && !hold_prev) {
        guide_down_time = state.timestamp_ms;
        guide_active = true;
        guide_combo_armed = true;
    }

    if (!hold_now && hold_prev) {
        guide_active = false;
        guide_combo_armed = false;
    }

    if (guide_combo_armed && hold_now && press_just) {
        const DWORD held = state.timestamp_ms - guide_down_time;
        if (held < combo_timeout_ms) {
            mode = (mode == MODE_NORMAL) ? MODE_MOUSE : MODE_NORMAL;
            guide_combo_armed = false;
            state.prev_buttons |= combo_press_btn;
        }
    }

    if (mode == MODE_MOUSE && guide_active && guide_combo_armed) {
        const DWORD held = state.timestamp_ms - guide_down_time;
        if (held >= static_cast<DWORD>(GUIDE_HOLD_EXIT_MS)) {
            mode = MODE_NORMAL;
            guide_combo_armed = false;
        }
    }

    return mode != previous_mode;
}

void GamepadInfo::prepareForPoll(int slotIndex)
{
    slot = slotIndex;
    state.beginFrame();
}

void GamepadInfo::applySnapshot(const XINPUT_GAMEPAD& gamepad, DWORD packetNumber, DWORD timestampMs)
{
    state.applySnapshot(gamepad, packetNumber, timestampMs);
    connected = true;
}

void GamepadInfo::markDisconnected()
{
    state.markDisconnected();
    connected = false;
}

std::wstring GamepadConfigStore::iniPath()
{
    return PathResolver::iniPath();
}

void GamepadConfigStore::load(GamepadInfos& infos)
{
    const std::wstring path = iniPath();

    wchar_t section[16] = {};
    for (int index = 0; index < XUSER_MAX_COUNT; ++index) {
        GamepadSlotState& slot = infos[index].slot_state;
        _snwprintf_s(section, std::size(section), _TRUNCATE, L"Slot%d", index);

        const UINT hold = static_cast<UINT>(GetPrivateProfileIntW(section, L"ComboHoldBtn",
                                                                  static_cast<int>(slot.combo_hold_btn), path.c_str()));
        const UINT press = static_cast<UINT>(GetPrivateProfileIntW(section, L"ComboPressBtn",
                                                                   static_cast<int>(slot.combo_press_btn), path.c_str()));
        const UINT timeout = static_cast<UINT>(GetPrivateProfileIntW(section, L"ComboTimeoutMs",
                                                                     static_cast<int>(slot.combo_timeout_ms), path.c_str()));
        slot.combo_hold_btn = static_cast<WORD>(hold);
        slot.combo_press_btn = static_cast<WORD>(press);
        slot.combo_timeout_ms = timeout;
    }
}

void GamepadConfigStore::save(const GamepadInfos& infos)
{
    const std::wstring path = iniPath();

    wchar_t section[16] = {};
    wchar_t value[16] = {};
    for (int index = 0; index < XUSER_MAX_COUNT; ++index) {
        const GamepadSlotState& slot = infos[index].slot_state;
        _snwprintf_s(section, std::size(section), _TRUNCATE, L"Slot%d", index);

        _snwprintf_s(value, std::size(value), _TRUNCATE, L"%u", static_cast<unsigned>(slot.combo_hold_btn));
        WritePrivateProfileStringW(section, L"ComboHoldBtn", value, path.c_str());

        _snwprintf_s(value, std::size(value), _TRUNCATE, L"%u", static_cast<unsigned>(slot.combo_press_btn));
        WritePrivateProfileStringW(section, L"ComboPressBtn", value, path.c_str());

        _snwprintf_s(value, std::size(value), _TRUNCATE, L"%u", static_cast<unsigned>(slot.combo_timeout_ms));
        WritePrivateProfileStringW(section, L"ComboTimeoutMs", value, path.c_str());
    }
}

struct GamepadManager::Impl {
    XInputRuntime xinput;
    HidDeviceCatalog hid_catalog;
};

GamepadManager::GamepadManager() : impl_(std::make_unique<Impl>())
{
}

GamepadManager::~GamepadManager()
{
    shutdown();
}

bool GamepadManager::init()
{
    return impl_->xinput.init();
}

bool GamepadManager::registerRawInput(HWND hwnd)
{
    RAWINPUTDEVICE devices[2] = {
        {0x01, 0x04, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, hwnd},
        {0x01, 0x05, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, hwnd},
    };

    return RegisterRawInputDevices(devices,
                                   static_cast<UINT>(std::size(devices)),
                                   sizeof(devices[0])) == TRUE;
}

void GamepadManager::shutdown()
{
    impl_->xinput.shutdown();
    impl_->hid_catalog.clear();
}

void GamepadManager::pollAll(GamepadInfos& infos)
{
    for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
        GamepadInfo& info = infos[index];
        info.prepareForPoll(static_cast<int>(index));
        if (!impl_->xinput.pollSlot(index, info)) {
            info.markDisconnected();
        }
    }
}

void GamepadManager::detectControllers(GamepadInfos& infos)
{
    impl_->hid_catalog.detect(infos, impl_->xinput);
}

void GamepadManager::handleRawInput(HRAWINPUT raw_input)
{
    impl_->hid_catalog.handleRawInput(raw_input);
}

void GamepadManager::reconcileActiveSlot(GamepadInfos& infos)
{
    impl_->hid_catalog.reconcileActiveSlot(infos);
}

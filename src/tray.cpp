/*
 * StickPoint — system tray icon and main status window
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wingdi.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

#include "tray.h"
#include "gamepad.h"

namespace {

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr int IDM_EXIT = 100;
constexpr int IDC_EMPTY = 900;

constexpr int ROW_IMG_ID(int slot) { return 1000 + slot * 10; }
constexpr int ROW_NAME_ID(int slot) { return 1001 + slot * 10; }
constexpr int ROW_MODE_ID(int slot) { return 1002 + slot * 10; }
constexpr int ROW_OPT_ID(int slot) { return 1003 + slot * 10; }

constexpr int IDC_OPT_TITLE = 2000;
constexpr int IDC_OPT_HOLD_LBL = 2001;
constexpr int IDC_OPT_HOLD_CB = 2002;
constexpr int IDC_OPT_PRESS_LBL = 2003;
constexpr int IDC_OPT_PRESS_CB = 2004;
constexpr int IDC_OPT_TIMEOUT_LBL = 2005;
constexpr int IDC_OPT_TIMEOUT_ED = 2006;
constexpr int IDC_OPT_OK = 2007;
constexpr int IDC_OPT_CANCEL = 2008;

constexpr int WIN_W = 660;
constexpr int WIN_MAX_VIS_H = 268;
constexpr int WIN_H_EMPTY = 120;
constexpr int ROW_H = 80;
constexpr int ROW_PAD_TOP = 8;
constexpr int ROW_PAD_BOT = 8;

constexpr int IMG_X = 12;
constexpr int IMG_Y = 8;
constexpr int IMG_W = 64;
constexpr int IMG_H = 64;

constexpr int NAME_X = 88;
constexpr int NAME_Y = 29;
constexpr int NAME_W = 344;
constexpr int NAME_H = 22;

constexpr int MODE_X = 444;
constexpr int MODE_Y = 26;
constexpr int MODE_W = 96;
constexpr int MODE_H = 28;

constexpr int OPT_X = 548;
constexpr int OPT_Y = 26;
constexpr int OPT_W = 96;
constexpr int OPT_H = 28;

constexpr int SCROLL_STRIP_W = 4;

constexpr COLORREF CLR_BG = RGB(245, 245, 245);
constexpr COLORREF CLR_SEP = RGB(218, 218, 218);
constexpr COLORREF CLR_SCROLL_BG = RGB(228, 228, 228);
constexpr COLORREF CLR_SCROLL_FG = RGB(160, 160, 160);

constexpr char kPopupClassName[] = "StickPointMain";
constexpr char kOptionsClassName[] = "StickPointOptions";

struct ButtonOption {
    const char* name;
    WORD value;
};

constexpr ButtonOption kButtonOptions[] = {
    {"Guide", XINPUT_GAMEPAD_GUIDE},
    {"A", XINPUT_GAMEPAD_A},
    {"B", XINPUT_GAMEPAD_B},
    {"X", XINPUT_GAMEPAD_X},
    {"Y", XINPUT_GAMEPAD_Y},
    {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER},
    {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {"Start", XINPUT_GAMEPAD_START},
    {"Back", XINPUT_GAMEPAD_BACK},
    {"LS", XINPUT_GAMEPAD_LEFT_THUMB},
    {"RS", XINPUT_GAMEPAD_RIGHT_THUMB},
};

template <typename T>
struct ComReleaser {
    void operator()(T* object) const
    {
        if (object) {
            object->Release();
        }
    }
};

template <typename T>
using ComPtr = std::unique_ptr<T, ComReleaser<T>>;

struct BitmapDeleter {
    void operator()(HBITMAP bitmap) const
    {
        if (bitmap) {
            DeleteObject(bitmap);
        }
    }
};

using ScopedBitmap = std::unique_ptr<std::remove_pointer_t<HBITMAP>, BitmapDeleter>;

struct BrushDeleter {
    void operator()(HBRUSH brush) const
    {
        if (brush) {
            DeleteObject(brush);
        }
    }
};

struct PenDeleter {
    void operator()(HPEN pen) const
    {
        if (pen) {
            DeleteObject(pen);
        }
    }
};

struct DeviceContextDeleter {
    void operator()(HDC deviceContext) const
    {
        if (deviceContext) {
            DeleteDC(deviceContext);
        }
    }
};

using ScopedBrush = std::unique_ptr<std::remove_pointer_t<HBRUSH>, BrushDeleter>;
using ScopedPen = std::unique_ptr<std::remove_pointer_t<HPEN>, PenDeleter>;
using ScopedMemoryDc = std::unique_ptr<std::remove_pointer_t<HDC>, DeviceContextDeleter>;

class ScopedSelectObject {
public:
    ScopedSelectObject(HDC deviceContext, HGDIOBJ object)
        : deviceContext_(deviceContext), oldObject_(nullptr)
    {
        if (deviceContext_ && object) {
            oldObject_ = SelectObject(deviceContext_, object);
        }
    }

    ~ScopedSelectObject()
    {
        if (deviceContext_ && oldObject_) {
            SelectObject(deviceContext_, oldObject_);
        }
    }

    ScopedSelectObject(const ScopedSelectObject&) = delete;
    ScopedSelectObject& operator=(const ScopedSelectObject&) = delete;

private:
    HDC deviceContext_;
    HGDIOBJ oldObject_;
};

bool register_window_class(const char* className, WNDPROC proc, HINSTANCE instance,
                           HBRUSH backgroundBrush, HICON icon, HICON smallIcon)
{
    WNDCLASSEXA windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = proc;
    windowClass.hInstance = instance;
    windowClass.hbrBackground = backgroundBrush;
    windowClass.hCursor = LoadCursorW(NULL, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    windowClass.hIcon = icon;
    windowClass.hIconSm = smallIcon;
    windowClass.lpszClassName = className;

    if (RegisterClassExA(&windowClass)) {
        return true;
    }

    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

}  // namespace

struct TrayApplication::Impl {
    NOTIFYICONDATAA nid_ = {};
    HWND hwndMsg_ = nullptr;
    HWND hwndPop_ = nullptr;
    HWND hwndOpts_ = nullptr;
    HINSTANCE hInst_ = nullptr;
    bool popVisible_ = false;
    bool initialized_ = false;
    int scrollY_ = 0;

    GamepadInfos lastInfos_ = {};
    std::array<bool, XUSER_MAX_COUNT> rowShown_ = {};
    std::array<AppMode, XUSER_MAX_COUNT> shownMode_ = {};
    std::array<std::string, XUSER_MAX_COUNT> shownName_ = {};
    std::array<std::wstring, XUSER_MAX_COUNT> shownImage_ = {};
    std::array<HBITMAP, XUSER_MAX_COUNT> bitmaps_ = {};

    IWICImagingFactory* wic_ = nullptr;
    int optionsSlot_ = -1;

    HFONT fontName_ = nullptr;
    HFONT fontUi_ = nullptr;
    HBRUSH brushBg_ = nullptr;

    bool init(HINSTANCE instance, HWND messageWindow);
    void updateGamepads(const GamepadInfos& infos);
    GamepadInfos& getInfos();
    void shutdown();
    bool handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* out);

    static LRESULT CALLBACK PopupWndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK OptionsWndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    LRESULT popupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT optionsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    static Impl* windowInstance(HWND hwnd);
    static const char* displayName(const std::string& name);

    bool initWic();
    HBITMAP loadPngHbitmap(const wchar_t* path);
    int shownCount() const;
    void centerWindow(HWND hwnd, int width, int height) const;
    int totalContentHeight() const;
    void recalcWindowSize();
    void setTooltipSummary();
    void applyScroll(int newY);
    int visualIndexForSlot(int slot) const;
    int rowTop(int visualIndex) const;
    void createRowControls(int slot);
    void destroyRowControls(int slot);
    void updateRowModeButton(int slot, AppMode mode);
    void repositionAllRows();
    void showEmptyLabel(bool show);
    void drawScrollStrip(HDC hdc);
    void populateCombo(HWND comboBox, WORD selectedValue);
    WORD comboSelectedValue(HWND comboBox) const;
    void showOptionsFor(int slot);
    void setTooltip(std::string_view text);
    static std::string timeoutText(DWORD timeoutMs);
    HWND createChildControl(const char* className, const char* text, DWORD style,
                            int x, int y, int width, int height, int controlId) const;
    void applyFont(HWND control, HFONT font) const;
    void destroyControl(int controlId) const;
    void createOptionsControls();
    static HICON loadDefaultIcon();
    void releaseBitmap(int slot);
};

TrayApplication::TrayApplication() : impl_(std::make_unique<Impl>())
{
}

TrayApplication::~TrayApplication()
{
    shutdown();
}

bool TrayApplication::init(HINSTANCE instance, HWND messageWindow)
{
    return impl_->init(instance, messageWindow);
}

void TrayApplication::updateGamepads(const GamepadInfos& infos)
{
    impl_->updateGamepads(infos);
}

GamepadInfos& TrayApplication::getInfos()
{
    return impl_->getInfos();
}

void TrayApplication::shutdown()
{
    impl_->shutdown();
}

bool TrayApplication::handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* out)
{
    return impl_->handleMessage(hwnd, msg, wp, lp, out);
}

TrayApplication::Impl* TrayApplication::Impl::windowInstance(HWND hwnd)
{
    return reinterpret_cast<Impl*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
}

const char* TrayApplication::Impl::displayName(const std::string& name)
{
    constexpr std::string_view prefix = "Controller ";
    const char* full_name = name.c_str();
    if (name.compare(0, prefix.size(), prefix) == 0) {
        const char* suffix = full_name + prefix.size();
        bool digitsOnly = (*suffix != '\0');

        if (*suffix == '(') {
            return full_name;
        }

        for (const char* current = suffix; *current != '\0'; ++current) {
            if (*current < '0' || *current > '9') {
                digitsOnly = false;
                break;
            }
        }

        if (!digitsOnly) {
            return suffix;
        }
    }

    return full_name;
}

bool TrayApplication::Impl::initWic()
{
    const HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IWICImagingFactory,
        reinterpret_cast<void**>(&wic_));
    return SUCCEEDED(hr);
}

HBITMAP TrayApplication::Impl::loadPngHbitmap(const wchar_t* path)
{
    if (!wic_ || !path || path[0] == L'\0') {
        return nullptr;
    }

    IWICBitmapDecoder* decoderRaw = nullptr;
    HRESULT hr = wic_->CreateDecoderFromFilename(
        path,
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoderRaw);
    if (FAILED(hr)) {
        return nullptr;
    }
    ComPtr<IWICBitmapDecoder> decoder(decoderRaw);

    IWICBitmapFrameDecode* frameRaw = nullptr;
    hr = decoder->GetFrame(0, &frameRaw);
    if (FAILED(hr)) {
        return nullptr;
    }
    ComPtr<IWICBitmapFrameDecode> frame(frameRaw);

    IWICFormatConverter* converterRaw = nullptr;
    hr = wic_->CreateFormatConverter(&converterRaw);
    if (SUCCEEDED(hr)) {
        hr = converterRaw->Initialize(
            frame.get(),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr)) {
        return nullptr;
    }
    ComPtr<IWICFormatConverter> converter(converterRaw);

    UINT originalWidth = 0;
    UINT originalHeight = 0;
    converter->GetSize(&originalWidth, &originalHeight);
    if (originalWidth == 0) {
        originalWidth = IMG_W;
    }
    if (originalHeight == 0) {
        originalHeight = IMG_H;
    }

    UINT fitWidth = 0;
    UINT fitHeight = 0;
    if (originalWidth * IMG_H >= originalHeight * IMG_W) {
        fitWidth = IMG_W;
        fitHeight = (originalHeight * IMG_W + originalWidth / 2) / originalWidth;
        if (fitHeight < 1) {
            fitHeight = 1;
        }
    } else {
        fitHeight = IMG_H;
        fitWidth = (originalWidth * IMG_H + originalHeight / 2) / originalHeight;
        if (fitWidth < 1) {
            fitWidth = 1;
        }
    }

    IWICBitmapScaler* scalerRaw = nullptr;
    hr = wic_->CreateBitmapScaler(&scalerRaw);
    if (SUCCEEDED(hr)) {
        hr = scalerRaw->Initialize(converter.get(), fitWidth, fitHeight,
                                   WICBitmapInterpolationModeHighQualityCubic);
    }
    if (FAILED(hr)) {
        return nullptr;
    }
    ComPtr<IWICBitmapScaler> scaler(scalerRaw);

    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = IMG_W;
    bitmapInfo.bmiHeader.biHeight = -IMG_H;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    ScopedBitmap bitmap(CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!bitmap || !bits) {
        return nullptr;
    }

    std::memset(bits, 0, static_cast<size_t>(IMG_W) * IMG_H * 4);

    const int offsetX = (IMG_W - static_cast<int>(fitWidth)) / 2;
    const int offsetY = (IMG_H - static_cast<int>(fitHeight)) / 2;

    const UINT sourceStride = fitWidth * 4;
    const UINT sourceSize = sourceStride * fitHeight;
    std::vector<BYTE> pixels(sourceSize);
    if (SUCCEEDED(scaler->CopyPixels(nullptr, sourceStride, sourceSize, pixels.data()))) {
        BYTE* destination = static_cast<BYTE*>(bits);
        for (UINT row = 0; row < fitHeight; ++row) {
            std::memcpy(destination + ((offsetY + static_cast<int>(row)) * IMG_W + offsetX) * 4,
                        pixels.data() + row * sourceStride,
                        sourceStride);
        }
    }

    return bitmap.release();
}

int TrayApplication::Impl::shownCount() const
{
    int count = 0;
    for (bool shown : rowShown_) {
        if (shown) {
            ++count;
        }
    }
    return count;
}

void TrayApplication::Impl::centerWindow(HWND hwnd, int width, int height) const
{
    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hwnd, HWND_TOP, (screenWidth - width) / 2, (screenHeight - height) / 2,
                 width, height, SWP_NOZORDER);
}

int TrayApplication::Impl::totalContentHeight() const
{
    const int count = shownCount();
    return (count == 0) ? WIN_H_EMPTY : (ROW_PAD_TOP + count * ROW_H + ROW_PAD_BOT);
}

void TrayApplication::Impl::recalcWindowSize()
{
    const int contentHeight = totalContentHeight();
    const int clientHeight = (contentHeight > WIN_MAX_VIS_H) ? WIN_MAX_VIS_H : contentHeight;

    const int maxScroll = contentHeight - clientHeight;
    if (scrollY_ > maxScroll) {
        scrollY_ = maxScroll;
    }
    if (scrollY_ < 0) {
        scrollY_ = 0;
    }

    RECT rect = {0, 0, WIN_W, clientHeight};
    AdjustWindowRect(&rect, WS_CAPTION | WS_SYSMENU, FALSE);
    const int newWidth = rect.right - rect.left;
    const int newHeight = rect.bottom - rect.top;

    RECT current = {};
    GetWindowRect(hwndPop_, &current);
    if ((current.right - current.left) != newWidth || (current.bottom - current.top) != newHeight) {
        centerWindow(hwndPop_, newWidth, newHeight);
    }
}

void TrayApplication::Impl::setTooltipSummary()
{
    const int count = shownCount();
    if (count == 0) {
        setTooltip("StickPoint \xe2\x80\x94 No controllers");
    } else if (count == 1) {
        for (int slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            if (!rowShown_[slot]) {
                continue;
            }

            setTooltip("StickPoint \xe2\x80\x94 " + lastInfos_[slot].name +
                       " [" + (shownMode_[slot] == MODE_MOUSE ? std::string("Mouse") : std::string("Gamepad")) + "]");
            break;
        }
    } else {
        setTooltip("StickPoint \xe2\x80\x94 " + std::to_string(count) + " controllers");
    }

    nid_.uFlags = NIF_TIP;
    Shell_NotifyIconA(NIM_MODIFY, &nid_);
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

void TrayApplication::Impl::setTooltip(std::string_view text)
{
    strncpy_s(nid_.szTip, sizeof(nid_.szTip), std::string(text).c_str(), _TRUNCATE);
}

std::string TrayApplication::Impl::timeoutText(DWORD timeoutMs)
{
    return std::to_string(static_cast<unsigned>(timeoutMs));
}

HWND TrayApplication::Impl::createChildControl(const char* className, const char* text, DWORD style,
                                               int x, int y, int width, int height, int controlId) const
{
    return CreateWindowExA(0, className, text, style,
                           x, y, width, height,
                           hwndPop_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(controlId)),
                           hInst_, nullptr);
}

void TrayApplication::Impl::applyFont(HWND control, HFONT font) const
{
    if (control && font) {
        SendMessageA(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    }
}

void TrayApplication::Impl::destroyControl(int controlId) const
{
    if (HWND control = GetDlgItem(hwndPop_, controlId)) {
        DestroyWindow(control);
    }
}

HICON TrayApplication::Impl::loadDefaultIcon()
{
    return LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
}

void TrayApplication::Impl::releaseBitmap(int slot)
{
    if (bitmaps_[slot]) {
        DeleteObject(bitmaps_[slot]);
        bitmaps_[slot] = nullptr;
    }
}

void TrayApplication::Impl::createOptionsControls()
{
    int originX = 12;
    int originY = 12;
    const auto createOptionsControl = [this](const char* className, const char* text, DWORD style,
                                             int x, int y, int width, int height, int controlId) {
        return CreateWindowExA(0, className, text, style,
                               x, y, width, height,
                               hwndOpts_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(controlId)),
                               hInst_, nullptr);
    };

    createOptionsControl("STATIC", "", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                         originX, originY, 346, 18, IDC_OPT_TITLE);
    originY += 30;

    createOptionsControl("STATIC", "Hold button:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                         originX, originY + 3, 106, 18, IDC_OPT_HOLD_LBL);
    createOptionsControl("COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                         originX + 112, originY, 230, 200, IDC_OPT_HOLD_CB);
    originY += 34;

    createOptionsControl("STATIC", "Press button:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                         originX, originY + 3, 106, 18, IDC_OPT_PRESS_LBL);
    createOptionsControl("COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                         originX + 112, originY, 230, 200, IDC_OPT_PRESS_CB);
    originY += 34;

    createOptionsControl("STATIC", "Timeout (ms):", WS_CHILD | WS_VISIBLE | SS_LEFT,
                         originX, originY + 3, 106, 18, IDC_OPT_TIMEOUT_LBL);
    createOptionsControl("EDIT", "500", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                         originX + 112, originY, 80, 22, IDC_OPT_TIMEOUT_ED);
    originY += 40;

    createOptionsControl("BUTTON", "OK", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                         originX, originY, 80, 28, IDC_OPT_OK);
    createOptionsControl("BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                         originX + 90, originY, 80, 28, IDC_OPT_CANCEL);
}

void TrayApplication::Impl::applyScroll(int newY)
{
    const int contentHeight = totalContentHeight();
    const int clientHeight = (contentHeight > WIN_MAX_VIS_H) ? WIN_MAX_VIS_H : contentHeight;
    const int maxScroll = contentHeight - clientHeight;
    if (newY < 0) {
        newY = 0;
    }
    if (newY > maxScroll) {
        newY = maxScroll;
    }
    if (newY == scrollY_) {
        return;
    }

    scrollY_ = newY;
    repositionAllRows();
    InvalidateRect(hwndPop_, NULL, TRUE);
}

int TrayApplication::Impl::visualIndexForSlot(int slot) const
{
    int visualIndex = 0;
    for (int current = 0; current < XUSER_MAX_COUNT; ++current) {
        if (!rowShown_[current]) {
            continue;
        }
        if (current == slot) {
            return visualIndex;
        }
        ++visualIndex;
    }
    return -1;
}

int TrayApplication::Impl::rowTop(int visualIndex) const
{
    return ROW_PAD_TOP + visualIndex * ROW_H - scrollY_;
}

void TrayApplication::Impl::createRowControls(int slot)
{
    const int visualIndex = visualIndexForSlot(slot);
    if (visualIndex < 0) {
        return;
    }
    const int y0 = rowTop(visualIndex);

    createChildControl("STATIC", "", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
                       IMG_X, y0 + IMG_Y, IMG_W, IMG_H, ROW_IMG_ID(slot));

    HWND nameLabel = createChildControl("STATIC", displayName(lastInfos_[slot].name),
                                        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS,
                                        NAME_X, y0 + NAME_Y, NAME_W, NAME_H, ROW_NAME_ID(slot));
    applyFont(nameLabel, fontName_);

    const char* modeLabel = (shownMode_[slot] == MODE_MOUSE) ? "Mouse" : "Gamepad";
    HWND modeButton = createChildControl("BUTTON", modeLabel,
                                         WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                         MODE_X, y0 + MODE_Y, MODE_W, MODE_H, ROW_MODE_ID(slot));
    applyFont(modeButton, fontUi_);

    HWND optionsButton = createChildControl("BUTTON", "Options",
                                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                            OPT_X, y0 + OPT_Y, OPT_W, OPT_H, ROW_OPT_ID(slot));
    applyFont(optionsButton, fontUi_);
}

void TrayApplication::Impl::destroyRowControls(int slot)
{
    destroyControl(ROW_IMG_ID(slot));
    destroyControl(ROW_NAME_ID(slot));
    destroyControl(ROW_MODE_ID(slot));
    destroyControl(ROW_OPT_ID(slot));
    releaseBitmap(slot);
}

void TrayApplication::Impl::updateRowModeButton(int slot, AppMode mode)
{
    HWND control = GetDlgItem(hwndPop_, ROW_MODE_ID(slot));
    if (control) {
        SetWindowTextA(control, (mode == MODE_MOUSE) ? "Mouse" : "Gamepad");
    }
}

void TrayApplication::Impl::repositionAllRows()
{
    int visualIndex = 0;
    for (int slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
        if (!rowShown_[slot]) {
            continue;
        }

        const int y0 = rowTop(visualIndex++);
        HWND control = GetDlgItem(hwndPop_, ROW_IMG_ID(slot));
        if (control) {
            SetWindowPos(control, NULL, IMG_X, y0 + IMG_Y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        control = GetDlgItem(hwndPop_, ROW_NAME_ID(slot));
        if (control) {
            SetWindowPos(control, NULL, NAME_X, y0 + NAME_Y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        control = GetDlgItem(hwndPop_, ROW_MODE_ID(slot));
        if (control) {
            SetWindowPos(control, NULL, MODE_X, y0 + MODE_Y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        control = GetDlgItem(hwndPop_, ROW_OPT_ID(slot));
        if (control) {
            SetWindowPos(control, NULL, OPT_X, y0 + OPT_Y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
    }
}

void TrayApplication::Impl::showEmptyLabel(bool show)
{
    HWND emptyLabel = GetDlgItem(hwndPop_, IDC_EMPTY);
    if (show) {
        if (!emptyLabel) {
            RECT clientRect = {};
            GetClientRect(hwndPop_, &clientRect);
            const int labelWidth = 260;
            const int labelHeight = 20;
            HWND label = CreateWindowExA(0, "STATIC", "No controllers connected.",
                                         WS_CHILD | WS_VISIBLE | SS_CENTER,
                                         (clientRect.right - labelWidth) / 2,
                                         (clientRect.bottom - labelHeight) / 2,
                                         labelWidth, labelHeight,
                                         hwndPop_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_EMPTY)),
                                         hInst_, nullptr);
            applyFont(label, fontUi_);
        } else {
            ShowWindow(emptyLabel, SW_SHOW);
        }
    } else if (emptyLabel) {
        ShowWindow(emptyLabel, SW_HIDE);
    }
}

void TrayApplication::Impl::drawScrollStrip(HDC hdc)
{
    const int contentHeight = totalContentHeight();
    if (contentHeight <= WIN_MAX_VIS_H) {
        return;
    }

    RECT clientRect = {};
    GetClientRect(hwndPop_, &clientRect);
    const int clientHeight = clientRect.bottom;

    RECT track = {clientRect.right - SCROLL_STRIP_W, 0, clientRect.right, clientHeight};
    ScopedBrush trackBrush(CreateSolidBrush(CLR_SCROLL_BG));
    if (trackBrush) {
        FillRect(hdc, &track, trackBrush.get());
    }

    const float ratio = static_cast<float>(clientHeight) / static_cast<float>(contentHeight);
    int thumbHeight = static_cast<int>(clientHeight * ratio);
    if (thumbHeight < 16) {
        thumbHeight = 16;
    }

    const int maxScroll = contentHeight - clientHeight;
    const float fraction = (maxScroll > 0) ? static_cast<float>(scrollY_) / static_cast<float>(maxScroll) : 0.0f;
    const int thumbTop = static_cast<int>(fraction * (clientHeight - thumbHeight));

    RECT thumb = {clientRect.right - SCROLL_STRIP_W, thumbTop,
                  clientRect.right, thumbTop + thumbHeight};
    ScopedBrush thumbBrush(CreateSolidBrush(CLR_SCROLL_FG));
    if (thumbBrush) {
        FillRect(hdc, &thumb, thumbBrush.get());
    }
}

void TrayApplication::Impl::populateCombo(HWND comboBox, WORD selectedValue)
{
    SendMessageA(comboBox, CB_RESETCONTENT, 0, 0);
    int selectedIndex = 0;
    for (int index = 0; index < static_cast<int>(std::size(kButtonOptions)); ++index) {
        const int position = static_cast<int>(SendMessageA(comboBox, CB_ADDSTRING, 0,
                                                           reinterpret_cast<LPARAM>(kButtonOptions[index].name)));
        SendMessageA(comboBox, CB_SETITEMDATA, static_cast<WPARAM>(position),
                     static_cast<LPARAM>(kButtonOptions[index].value));
        if (kButtonOptions[index].value == selectedValue) {
            selectedIndex = position;
        }
    }
    SendMessageA(comboBox, CB_SETCURSEL, static_cast<WPARAM>(selectedIndex), 0);
}

WORD TrayApplication::Impl::comboSelectedValue(HWND comboBox) const
{
    const int index = static_cast<int>(SendMessageA(comboBox, CB_GETCURSEL, 0, 0));
    if (index == CB_ERR) {
        return 0;
    }
    return static_cast<WORD>(SendMessageA(comboBox, CB_GETITEMDATA, static_cast<WPARAM>(index), 0));
}

void TrayApplication::Impl::showOptionsFor(int slot)
{
    if (optionsSlot_ != -1) {
        return;
    }
    optionsSlot_ = slot;

    const std::string title = "Options \xe2\x80\x94 " + lastInfos_[slot].name;
    SetWindowTextA(hwndOpts_, title.c_str());

    const GamepadSlotState* slotState = &lastInfos_[slot].slot_state;
    populateCombo(GetDlgItem(hwndOpts_, IDC_OPT_HOLD_CB), slotState->combo_hold_btn);
    populateCombo(GetDlgItem(hwndOpts_, IDC_OPT_PRESS_CB), slotState->combo_press_btn);

    const std::string timeout = timeoutText(slotState->combo_timeout_ms);
    SetWindowTextA(GetDlgItem(hwndOpts_, IDC_OPT_TIMEOUT_ED), timeout.c_str());

    RECT popupRect = {};
    GetWindowRect(hwndPop_, &popupRect);
    RECT optionsRect = {0, 0, 370, 200};
    AdjustWindowRect(&optionsRect, WS_CAPTION | WS_SYSMENU, FALSE);
    const int optionsWidth = optionsRect.right - optionsRect.left;
    const int optionsHeight = optionsRect.bottom - optionsRect.top;
    SetWindowPos(hwndOpts_, HWND_TOP,
                 popupRect.left + (popupRect.right - popupRect.left - optionsWidth) / 2,
                 popupRect.top + (popupRect.bottom - popupRect.top - optionsHeight) / 2,
                 optionsWidth, optionsHeight, 0);

    EnableWindow(hwndPop_, FALSE);
    ShowWindow(hwndOpts_, SW_SHOW);
    SetForegroundWindow(hwndOpts_);
}

LRESULT CALLBACK TrayApplication::Impl::PopupWndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTA* create = reinterpret_cast<const CREATESTRUCTA*>(lp);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    Impl* self = windowInstance(hwnd);
    if (self) {
        const LRESULT result = self->popupWndProc(hwnd, msg, wp, lp);
        if (msg == WM_NCDESTROY) {
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        }
        return result;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

LRESULT CALLBACK TrayApplication::Impl::OptionsWndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTA* create = reinterpret_cast<const CREATESTRUCTA*>(lp);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    Impl* self = windowInstance(hwnd);
    if (self) {
        const LRESULT result = self->optionsWndProc(hwnd, msg, wp, lp);
        if (msg == WM_NCDESTROY) {
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        }
        return result;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

LRESULT TrayApplication::Impl::popupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* drawItem = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        const int slot = (static_cast<int>(drawItem->CtlID) - 1000) / 10;
        if (slot >= 0 && slot < XUSER_MAX_COUNT && bitmaps_[slot]) {
            ScopedMemoryDc memoryDc(CreateCompatibleDC(drawItem->hDC));
            if (memoryDc) {
                ScopedSelectObject selectBitmap(memoryDc.get(), bitmaps_[slot]);
                BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                AlphaBlend(drawItem->hDC, drawItem->rcItem.left, drawItem->rcItem.top,
                           IMG_W, IMG_H, memoryDc.get(), 0, 0, IMG_W, IMG_H, blend);
            }
        } else {
            ScopedBrush brush(CreateSolidBrush(RGB(210, 210, 210)));
            if (brush) {
                FillRect(drawItem->hDC, &drawItem->rcItem, brush.get());
            }
        }
        return TRUE;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC hdc = BeginPaint(hwnd, &paint);

        RECT clientRect = {};
        GetClientRect(hwnd, &clientRect);
        FillRect(hdc, &clientRect, brushBg_);

        const int count = shownCount();
        if (count > 1) {
            ScopedPen pen(CreatePen(PS_SOLID, 1, CLR_SEP));
            if (pen) {
                ScopedSelectObject selectPen(hdc, pen.get());
                const int separatorRight = clientRect.right - SCROLL_STRIP_W - 4;
                for (int visualIndex = 1; visualIndex < count; ++visualIndex) {
                    const int separatorY = ROW_PAD_TOP + visualIndex * ROW_H - scrollY_;
                    if (separatorY >= 0 && separatorY < clientRect.bottom) {
                        MoveToEx(hdc, NAME_X, separatorY, nullptr);
                        LineTo(hdc, separatorRight, separatorY);
                    }
                }
            }
        }

        drawScrollStrip(hdc);

        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        SetTextColor(hdc, RGB(25, 25, 25));
        SetBkColor(hdc, CLR_BG);
        return reinterpret_cast<LRESULT>(brushBg_);
    }

    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        const int step = (ROW_H + 2) / 3;
        applyScroll(scrollY_ + (delta > 0 ? -step : step));
        return 0;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id >= 1002 && (id - 1002) % 10 == 0) {
            const int slot = (id - 1002) / 10;
            if (slot < XUSER_MAX_COUNT && rowShown_[slot]) {
                const AppMode next = (shownMode_[slot] == MODE_MOUSE) ? MODE_NORMAL : MODE_MOUSE;
                lastInfos_[slot].slot_state.mode = next;
                shownMode_[slot] = next;
                updateRowModeButton(slot, next);
                setTooltipSummary();
            }
            return 0;
        }
        if (id >= 1003 && (id - 1003) % 10 == 0) {
            const int slot = (id - 1003) / 10;
            if (slot < XUSER_MAX_COUNT && rowShown_[slot]) {
                showOptionsFor(slot);
            }
            return 0;
        }
        return 0;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        popVisible_ = false;
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

LRESULT TrayApplication::Impl::optionsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_OPT_OK) {
            const int slot = optionsSlot_;
            if (slot >= 0 && slot < XUSER_MAX_COUNT) {
                GamepadSlotState* slotState = &lastInfos_[slot].slot_state;
                const WORD hold = comboSelectedValue(GetDlgItem(hwnd, IDC_OPT_HOLD_CB));
                const WORD press = comboSelectedValue(GetDlgItem(hwnd, IDC_OPT_PRESS_CB));
                char buffer[16] = {};
                GetWindowTextA(GetDlgItem(hwnd, IDC_OPT_TIMEOUT_ED), buffer, sizeof(buffer));
                DWORD timeoutMs = static_cast<DWORD>(std::strtoul(buffer, nullptr, 10));
                if (timeoutMs < 100) {
                    timeoutMs = 100;
                }
                if (timeoutMs > 5000) {
                    timeoutMs = 5000;
                }
                slotState->combo_hold_btn = hold;
                slotState->combo_press_btn = press;
                slotState->combo_timeout_ms = timeoutMs;
                GamepadConfigStore::save(lastInfos_);
            }
        }

        if (LOWORD(wp) == IDC_OPT_OK || LOWORD(wp) == IDC_OPT_CANCEL) {
            EnableWindow(hwndPop_, TRUE);
            optionsSlot_ = -1;
            ShowWindow(hwnd, SW_HIDE);
        }
        return 0;

    case WM_CLOSE:
        EnableWindow(hwndPop_, TRUE);
        optionsSlot_ = -1;
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

bool TrayApplication::Impl::init(HINSTANCE instance, HWND messageWindow)
{
    hInst_ = instance;
    hwndMsg_ = messageWindow;

    CoInitialize(nullptr);
    initWic();

    fontName_ = CreateFontA(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    fontUi_ = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    brushBg_ = CreateSolidBrush(CLR_BG);

    HICON largeIcon = static_cast<HICON>(LoadImageA(instance, MAKEINTRESOURCEA(1),
                                                    IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    HICON smallIcon = static_cast<HICON>(LoadImageA(instance, MAKEINTRESOURCEA(1),
                                                    IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    if (!largeIcon) {
        largeIcon = loadDefaultIcon();
    }
    if (!smallIcon) {
        smallIcon = loadDefaultIcon();
    }

    if (!register_window_class(kPopupClassName, PopupWndProcThunk, instance, nullptr, largeIcon, smallIcon)) {
        shutdown();
        return false;
    }

    RECT popupRect = {0, 0, WIN_W, WIN_H_EMPTY};
    AdjustWindowRect(&popupRect, WS_CAPTION | WS_SYSMENU, FALSE);
    const int popupWidth = popupRect.right - popupRect.left;
    const int popupHeight = popupRect.bottom - popupRect.top;
    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    hwndPop_ = CreateWindowExA(0, kPopupClassName, "StickPoint",
                               WS_CAPTION | WS_SYSMENU,
                               (screenWidth - popupWidth) / 2,
                               (screenHeight - popupHeight) / 2,
                               popupWidth, popupHeight,
                               nullptr, nullptr, instance, this);
    if (!hwndPop_) {
        shutdown();
        return false;
    }

    if (!register_window_class(kOptionsClassName, OptionsWndProcThunk, instance,
                               reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1), nullptr, nullptr)) {
        shutdown();
        return false;
    }

    RECT optionsRect = {0, 0, 370, 200};
    AdjustWindowRect(&optionsRect, WS_CAPTION | WS_SYSMENU, FALSE);
    hwndOpts_ = CreateWindowExA(0, kOptionsClassName, "Options",
                                WS_CAPTION | WS_SYSMENU,
                                0, 0,
                                optionsRect.right - optionsRect.left,
                                optionsRect.bottom - optionsRect.top,
                                hwndPop_, nullptr, instance, this);
    if (!hwndOpts_) {
        shutdown();
        return false;
    }

    createOptionsControls();

    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = messageWindow;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_TRAYICON;
    nid_.hIcon = static_cast<HICON>(LoadImageA(instance, MAKEINTRESOURCEA(1),
                                               IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    if (!nid_.hIcon) {
        nid_.hIcon = loadDefaultIcon();
    }
    strncpy_s(nid_.szTip, sizeof(nid_.szTip), "StickPoint \xe2\x80\x94 No controllers", _TRUNCATE);
    if (!Shell_NotifyIconA(NIM_ADD, &nid_)) {
        shutdown();
        return false;
    }

    showEmptyLabel(true);
    initialized_ = true;
    return true;
}

void TrayApplication::Impl::updateGamepads(const GamepadInfos& infos)
{
    if (!initialized_) {
        return;
    }

    bool layoutChanged = false;
    bool tooltipDirty = false;

    for (int slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
        const bool connected = infos[slot].connected;

        if (connected != rowShown_[slot]) {
            layoutChanged = true;
            tooltipDirty = true;

            if (connected) {
                shownMode_[slot] = infos[slot].slot_state.mode;
                shownName_[slot] = infos[slot].name;
                shownImage_[slot] = infos[slot].image;
                rowShown_[slot] = true;
                if (bitmaps_[slot]) {
                    releaseBitmap(slot);
                }
                bitmaps_[slot] = loadPngHbitmap(infos[slot].image.c_str());
            } else {
                destroyRowControls(slot);
                rowShown_[slot] = false;
                if (optionsSlot_ == slot) {
                    EnableWindow(hwndPop_, TRUE);
                    optionsSlot_ = -1;
                    ShowWindow(hwndOpts_, SW_HIDE);
                }
            }
        }

        if (connected && rowShown_[slot] && infos[slot].slot_state.mode != shownMode_[slot]) {
            shownMode_[slot] = infos[slot].slot_state.mode;
            updateRowModeButton(slot, shownMode_[slot]);
            tooltipDirty = true;
        }

        if (connected && rowShown_[slot]) {
            if (infos[slot].name != shownName_[slot]) {
                shownName_[slot] = infos[slot].name;
                HWND label = GetDlgItem(hwndPop_, ROW_NAME_ID(slot));
                if (label) {
                    SetWindowTextA(label, displayName(infos[slot].name));
                }
                tooltipDirty = true;
            }

            if (infos[slot].image != shownImage_[slot]) {
                shownImage_[slot] = infos[slot].image;
                if (bitmaps_[slot]) {
                    releaseBitmap(slot);
                }
                bitmaps_[slot] = loadPngHbitmap(infos[slot].image.c_str());
                HWND image = GetDlgItem(hwndPop_, ROW_IMG_ID(slot));
                if (image) {
                    InvalidateRect(image, NULL, TRUE);
                }
            }
        }

        lastInfos_[slot] = infos[slot];
    }

    if (layoutChanged) {
        repositionAllRows();
        for (int slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            if (rowShown_[slot] && !GetDlgItem(hwndPop_, ROW_IMG_ID(slot))) {
                createRowControls(slot);
            }
        }
        showEmptyLabel(shownCount() == 0);
        recalcWindowSize();
        if (popVisible_) {
            InvalidateRect(hwndPop_, NULL, TRUE);
        }
    }

    if (tooltipDirty) {
        setTooltipSummary();
    }
}

GamepadInfos& TrayApplication::Impl::getInfos()
{
    return lastInfos_;
}

void TrayApplication::Impl::shutdown()
{
    if (hwndOpts_) {
        DestroyWindow(hwndOpts_);
        hwndOpts_ = nullptr;
    }
    if (hwndPop_) {
        DestroyWindow(hwndPop_);
        hwndPop_ = nullptr;
    }

    for (int slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
        releaseBitmap(slot);
    }
    if (fontName_) {
        DeleteObject(fontName_);
        fontName_ = nullptr;
    }
    if (fontUi_) {
        DeleteObject(fontUi_);
        fontUi_ = nullptr;
    }
    if (brushBg_) {
        DeleteObject(brushBg_);
        brushBg_ = nullptr;
    }
    if (wic_) {
        wic_->Release();
        wic_ = nullptr;
    }
    if (nid_.hWnd) {
        Shell_NotifyIconA(NIM_DELETE, &nid_);
    }

    CoUninitialize();

    nid_ = {};
    hwndMsg_ = nullptr;
    hInst_ = nullptr;
    popVisible_ = false;
    initialized_ = false;
    scrollY_ = 0;
    optionsSlot_ = -1;
}

bool TrayApplication::Impl::handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* out)
{
    *out = 0;

    if (msg == WM_TRAYICON) {
        const UINT event = static_cast<UINT>(lp);
        if (event == WM_LBUTTONUP) {
            if (popVisible_) {
                ShowWindow(hwndPop_, SW_HIDE);
                popVisible_ = false;
            } else {
                ShowWindow(hwndPop_, SW_SHOW);
                SetForegroundWindow(hwndPop_);
                popVisible_ = true;
            }
            return true;
        }

        if (event == WM_RBUTTONUP) {
            POINT cursor = {};
            GetCursorPos(&cursor);
            SetForegroundWindow(hwnd);
            HMENU menu = CreatePopupMenu();
            AppendMenuA(menu, MF_STRING, IDM_EXIT, "Exit StickPoint");
            TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_RIGHTALIGN | TPM_LEFTBUTTON,
                           cursor.x, cursor.y, 0, hwnd, NULL);
            DestroyMenu(menu);
            return true;
        }
    }

    if (msg == WM_COMMAND && LOWORD(wp) == IDM_EXIT) {
        DestroyWindow(hwnd);
        return true;
    }

    return false;
}

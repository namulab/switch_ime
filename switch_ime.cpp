#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <imm.h>
#include <shellapi.h>
#include <cwchar>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include "resource.h"

namespace
{
constexpr wchar_t kClass[] = L"switch_ime.Window";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kSwitchMessage = WM_APP + 2; // Notification to our own HWND only.
constexpr UINT kCaptureKey = WM_APP + 3;
constexpr UINT kSettingsCommand = 1;
constexpr UINT kExitCommand = 2;
constexpr WPARAM kGetOpen = 0x0005; // Legacy IMC_GETOPENSTATUS
constexpr WPARAM kGetConversion = 0x0001; // Legacy IMC_GETCONVERSIONMODE
enum class ImeState { Unknown, Alphanumeric, Japanese };

struct Keys
{
    UINT on = 0;
    UINT off = 0;
    bool Valid() const { return on >= 8 && on <= 254 && off >= 8 && off <= 254 && on != off; }
};

struct Target
{
    HWND foreground = nullptr;
    HWND focus = nullptr;
    DWORD thread = 0;
    HKL layout = nullptr;
    bool operator==(const Target& other) const
    {
        return foreground == other.foreground && focus == other.focus &&
            thread == other.thread && layout == other.layout;
    }
};

bool CaptureTarget(Target& target)
{
    target = {};
    target.foreground = GetForegroundWindow();
    if (!target.foreground) return false;
    target.thread = GetWindowThreadProcessId(target.foreground, nullptr);
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (!target.thread || !GetGUIThreadInfo(target.thread, &info) || !info.hwndFocus) return false;
    target.focus = info.hwndFocus;
    target.layout = GetKeyboardLayout(target.thread);
    return GetForegroundWindow() == target.foreground;
}

bool QueryIme(HWND ime, WPARAM command, DWORD_PTR& response)
{
    response = 0;
    // Delivery success is distinct from an IME response of zero.
    return SendMessageTimeoutW(ime, WM_IME_CONTROL, command, 0,
        SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 20, &response) != 0;
}

ImeState ReadIme(const Target& target)
{
    const HWND ime = ImmGetDefaultIMEWnd(target.focus);
    if (!ime) return ImeState::Unknown;
    DWORD_PTR open = 0;
    if (!QueryIme(ime, kGetOpen, open)) return ImeState::Unknown;
    ImeState state = ImeState::Unknown;
    if (open == 0) state = ImeState::Alphanumeric;
    else if (open == 1)
    {
        DWORD_PTR mode = 0;
        if (!QueryIme(ime, kGetConversion, mode)) return ImeState::Unknown;
        constexpr DWORD known = IME_CMODE_NATIVE | IME_CMODE_KATAKANA |
            IME_CMODE_FULLSHAPE | IME_CMODE_ROMAN | IME_CMODE_CHARCODE |
            IME_CMODE_HANJACONVERT | IME_CMODE_SOFTKBD | IME_CMODE_NOCONVERSION |
            IME_CMODE_EUDC | IME_CMODE_SYMBOL | IME_CMODE_FIXED;
        if ((mode & ~static_cast<DWORD_PTR>(known)) != 0) return ImeState::Unknown;
        if ((mode & IME_CMODE_NATIVE) == 0) state = ImeState::Alphanumeric;
        else if (target.layout && PRIMARYLANGID(LOWORD(
            reinterpret_cast<ULONG_PTR>(target.layout))) == LANG_JAPANESE)
            state = ImeState::Japanese;
    }
    Target after{};
    if (!CaptureTarget(after) || !(after == target) ||
        ImmGetDefaultIMEWnd(after.focus) != ime) return ImeState::Unknown;
    return state;
}

bool NeedsSwitch(bool turnOn, ImeState state)
{
    return turnOn ? state == ImeState::Alphanumeric : state == ImeState::Japanese;
}

void MakeInputs(bool turnOn, INPUT (&inputs)[2])
{
    inputs[0] = {};
    inputs[1] = {};
    inputs[0].type = inputs[1].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = inputs[1].ki.wVk = turnOn ? VK_IME_ON : VK_IME_OFF;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
}

bool GetExePath(std::wstring& path)
{
    std::vector<wchar_t> buffer(260);
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!length) return false;
        if (length < buffer.size())
        {
            path.assign(buffer.data(), length);
            return true;
        }
        if (buffer.size() >= 32768) return false;
        buffer.resize(buffer.size() * 2);
    }
}

bool GetIniPath(std::wstring& path)
{
    if (!GetExePath(path)) return false;
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    path.resize(slash + 1);
    path += L"switch_ime.ini";
    return true;
}

bool ParseVk(const wchar_t* text, UINT& result)
{
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(text, &end, 0);
    while (end && (*end == L' ' || *end == L'\t')) ++end;
    if (errno || end == text || !end || *end || value < 8 || value > 254) return false;
    result = static_cast<UINT>(value);
    return true;
}

bool LoadKeys(const std::wstring& path, Keys& keys)
{
    wchar_t on[64]{}, off[64]{};
    GetPrivateProfileStringW(L"Keys", L"IME_ON", L"", on, 64, path.c_str());
    GetPrivateProfileStringW(L"Keys", L"IME_OFF", L"", off, 64, path.c_str());
    Keys loaded;
    if (!ParseVk(on, loaded.on) || !ParseVk(off, loaded.off) || !loaded.Valid()) return false;
    keys = loaded;
    return true;
}

bool SaveKeys(const std::wstring& path, const Keys& keys)
{
    if (!keys.Valid()) return false;
    char contents[80]{};
    const int length = sprintf_s(contents, "[Keys]\r\nIME_ON=0x%02X\r\nIME_OFF=0x%02X\r\n", keys.on, keys.off);
    if (length <= 0) return false;
    // Replace both values together, so a write error does not leave half a configuration.
    const std::wstring temporary = path + L".tmp." + std::to_wstring(GetCurrentProcessId());
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool saved = WriteFile(file, contents, static_cast<DWORD>(length), &written, nullptr) &&
        written == static_cast<DWORD>(length) && FlushFileBuffers(file);
    CloseHandle(file);
    if (saved) saved = MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!saved) DeleteFileW(temporary.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    return saved;
}

std::wstring KeyName(UINT vk, LPARAM keyData = 0)
{
    if (!vk) return L"";
    if (!keyData)
    {
        const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
        keyData = static_cast<LPARAM>((scan & 0xFF) << 16);
        if (scan & 0xFF00) keyData |= static_cast<LPARAM>(1) << 24;
    }
    wchar_t name[128]{};
    if (GetKeyNameTextW(static_cast<LONG>(keyData), name, 128) > 0) return name;
    swprintf_s(name, L"VK 0x%02X", vk);
    return name;
}

// Only our named value is changed. The current-user Run key needs no elevation.
struct StartupEntry
{
    bool exists = false;
    DWORD type = REG_SZ;
    std::vector<BYTE> data;
};

bool ReadStartup(HKEY key, StartupEntry& entry)
{
    entry = {};
    DWORD size = 0;
    LSTATUS status = RegQueryValueExW(key, L"switch_ime", nullptr, &entry.type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND) return true;
    if (status != ERROR_SUCCESS) return false;
    entry.data.resize(size);
    status = RegQueryValueExW(key, L"switch_ime", nullptr, &entry.type, entry.data.data(), &size);
    if (status != ERROR_SUCCESS) return false;
    entry.data.resize(size);
    entry.exists = true;
    return true;
}

bool WriteStartup(HKEY key, const StartupEntry& entry)
{
    const LSTATUS status = entry.exists ? RegSetValueExW(key, L"switch_ime", 0,
        entry.type, entry.data.data(), static_cast<DWORD>(entry.data.size())) :
        RegDeleteValueW(key, L"switch_ime");
    return status == ERROR_SUCCESS || (!entry.exists && status == ERROR_FILE_NOT_FOUND);
}

bool MakeStartupEntry(const std::wstring& exe, StartupEntry& entry)
{
    const std::wstring command = L"\"" + exe + L"\"";
    // Run command lines have a documented 260-character limit.
    if (exe.empty() || command.size() > 260) return false;
    entry = {};
    entry.exists = true;
    entry.data.resize((command.size() + 1) * sizeof(wchar_t));
    std::memcpy(entry.data.data(), command.c_str(), entry.data.size());
    return true;
}

struct DialogState
{
    Keys keys;
    const std::wstring* ini;
    HFONT font = nullptr;
    const POINT* anchor = nullptr;
    HKEY startupKey = nullptr;
    StartupEntry originalStartup;
};
WNDPROC gEditProc = nullptr;
HWND gCaptureEdits[2]{};
HWND gSettingsDialog = nullptr;

void PositionSettings(HWND dialog, const POINT& point)
{
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    RECT rect{};
    if (!GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor) ||
        !GetWindowRect(dialog, &rect)) return;
    const RECT& work = monitor.rcWork;
    const LONG width = rect.right - rect.left, height = rect.bottom - rect.top;
    // Mouse coordinates are already screen pixels. Keep the whole dialog in
    // the clicked monitor's work area, including when the tray is at an edge.
    const LONG x = (std::max)(work.left, (std::min)(point.x - width / 2, work.right - width));
    const LONG y = (std::max)(work.top, (std::min)(point.y, work.bottom - height));
    SetWindowPos(dialog, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

void LayoutSettings(HWND dialog, HFONT uiFont)
{
    // MapDialogRect retains the resource-template base units after WM_SETFONT.
    // Correct its result to the actual system message font's DLU base units.
    RECT templateUnits{0, 0, 4, 8};
    MapDialogRect(dialog, &templateUnits);
    const WPARAM font = uiFont ? reinterpret_cast<WPARAM>(uiFont) : SendMessageW(dialog, WM_GETFONT, 0, 0);
    HDC dc = GetDC(dialog);
    const auto previousFont = SelectObject(dc, reinterpret_cast<HFONT>(font));
    SIZE alphabet{};
    TEXTMETRICW metrics{};
    GetTextExtentPoint32W(dc, L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", 52, &alphabet);
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, previousFont);
    ReleaseDC(dialog, dc);
    const int baseX = (alphabet.cx / 26 + 1) / 2, baseY = metrics.tmHeight;
    const auto map = [&](RECT& rect)
    {
        MapDialogRect(dialog, &rect);
        rect.left = MulDiv(rect.left, baseX, templateUnits.right);
        rect.right = MulDiv(rect.right, baseX, templateUnits.right);
        rect.top = MulDiv(rect.top, baseY, templateUnits.bottom);
        rect.bottom = MulDiv(rect.bottom, baseY, templateUnits.bottom);
    };
    // All dimensions below are DLU, not fixed pixels.
    struct Control { int id; RECT rect; };
    const Control controls[] = {
        {IDC_ON_LABEL, {7, 9, 67, 17}}, {IDC_ON_KEY, {71, 7, 170, 18}},
        {IDC_OFF_LABEL, {7, 22, 67, 30}}, {IDC_OFF_KEY, {71, 20, 170, 31}},
        {IDC_STARTUP, {7, 37, 170, 47}},
        {IDOK, {43, 53, 103, 67}}, {IDCANCEL, {110, 53, 170, 67}}
    };
    RECT previous{};
    GetWindowRect(dialog, &previous);
    RECT size{0, 0, 177, 72};
    map(size);
    AdjustWindowRectEx(&size, static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE)),
        FALSE, static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE)));
    const int width = size.right - size.left, height = size.bottom - size.top;
    SetWindowPos(dialog, nullptr, (previous.left + previous.right - width) / 2,
        (previous.top + previous.bottom - height) / 2, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    for (const auto& control : controls)
    {
        const HWND child = GetDlgItem(dialog, control.id);
        SendMessageW(child, WM_SETFONT, font, FALSE);
        RECT rect = control.rect;
        map(rect);
        SetWindowPos(child, nullptr, rect.left, rect.top, rect.right - rect.left,
            rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

LRESULT CALLBACK KeyEditProc(HWND edit, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_GETDLGCODE) return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTTAB;
    if (message == kCaptureKey)
    {
        auto* dialog = reinterpret_cast<DialogState*>(GetWindowLongPtrW(GetParent(edit), DWLP_USER));
        // The low-level hook supplies the actual VK before IME translation.
        const UINT vk = static_cast<UINT>(wParam);
        if (dialog && vk >= 8 && vk <= 254)
        {
            if (GetDlgCtrlID(edit) == IDC_ON_KEY) dialog->keys.on = vk;
            else dialog->keys.off = vk;
            SetWindowTextW(edit, KeyName(vk, lParam).c_str());
        }
        return 0;
    }
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_IME_KEYDOWN ||
        message == WM_KEYUP || message == WM_SYSKEYUP || message == WM_IME_KEYUP || message == WM_CHAR ||
        message == WM_SYSCHAR || message == WM_DEADCHAR || message == WM_SYSDEADCHAR) return 0;
    return CallWindowProcW(gEditProc, edit, message, wParam, lParam);
}

INT_PTR CALLBACK SettingsProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG)
    {
        state = reinterpret_cast<DialogState*>(lParam);
        gSettingsDialog = dialog;
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
            state->font = CreateFontIndirectW(&metrics.lfMessageFont);
        if (state->font) SendMessageW(dialog, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), FALSE);
        LayoutSettings(dialog, state->font);
        if (state->startupKey && !ReadStartup(state->startupKey, state->originalStartup))
        {
            MessageBoxW(dialog, L"自動起動の登録状態を取得できません。", L"switch_ime", MB_OK | MB_ICONERROR);
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        CheckDlgButton(dialog, IDC_STARTUP, state->originalStartup.exists ? BST_CHECKED : BST_UNCHECKED);
        EnableWindow(GetDlgItem(dialog, IDC_STARTUP), state->startupKey != nullptr);
        for (const int id : {IDC_ON_KEY, IDC_OFF_KEY})
        {
            const HWND edit = GetDlgItem(dialog, id);
            gCaptureEdits[id == IDC_ON_KEY ? 0 : 1] = edit;
            const auto previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(edit,
                GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(KeyEditProc)));
            if (!gEditProc) gEditProc = previous;
            SetWindowTextW(edit, KeyName(id == IDC_ON_KEY ? state->keys.on : state->keys.off).c_str());
        }
        if (state->anchor) PositionSettings(dialog, *state->anchor);
        SetForegroundWindow(dialog);
        return TRUE;
    }
    if (message == WM_COMMAND && state)
    {
        if (LOWORD(wParam) == IDOK)
        {
            if (!state->keys.Valid())
            {
                MessageBoxW(dialog, L"ONとOFFに異なるキーを登録してください。", L"switch_ime", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            StartupEntry desired;
            if (state->startupKey && IsDlgButtonChecked(dialog, IDC_STARTUP) == BST_CHECKED)
            {
                std::wstring exe;
                if (!GetExePath(exe) || !MakeStartupEntry(exe, desired))
                {
                    MessageBoxW(dialog, L"自動起動に登録できません。EXEのパスが長すぎないか確認してください。",
                        L"switch_ime", MB_OK | MB_ICONERROR);
                    return TRUE;
                }
            }
            if (state->startupKey && !WriteStartup(state->startupKey, desired))
            {
                MessageBoxW(dialog, L"自動起動の登録を変更できません。", L"switch_ime", MB_OK | MB_ICONERROR);
                return TRUE;
            }
            if (!SaveKeys(*state->ini, state->keys))
            {
                if (state->startupKey && !WriteStartup(state->startupKey, state->originalStartup))
                    MessageBoxW(dialog, L"自動起動の登録状態を元に戻せません。設定を再確認してください。",
                        L"switch_ime", MB_OK | MB_ICONERROR);
                MessageBoxW(dialog, L"設定を保存できません。EXEのフォルダへの書き込み権限を確認してください。",
                    L"switch_ime", MB_OK | MB_ICONERROR);
                return TRUE;
            }
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
    }
    if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
    if (message == WM_DESTROY)
    {
        gCaptureEdits[0] = gCaptureEdits[1] = nullptr;
        if (gSettingsDialog == dialog) gSettingsDialog = nullptr;
    }
    if (message == WM_NCDESTROY && state && state->font)
    {
        DeleteObject(state->font);
        state->font = nullptr;
    }
    return FALSE;
}

struct KeyFilter
{
    // VK can change between make and break for Japanese mode keys.
    // Pair physical events by scan code; retain a VK fallback for scan-less keys.
    bool down[768]{};
    bool consumed[768]{};
    static UINT Index(UINT vk, UINT scan, bool extended)
    {
        return scan ? 256 + (scan & 0xFF) + (extended ? 256 : 0) : vk;
    }
    struct Decision { bool consume; bool request; bool turnOn; };
    Decision Process(UINT vk, bool keyDown, bool injected, bool paused, const Keys& keys,
        bool capture = false, UINT scan = 0, bool extended = false)
    {
        if (injected || vk >= 256) return {false, false, false};
        const UINT index = Index(vk, scan, extended);
        if (!keyDown)
        {
            const bool swallow = consumed[index];
            down[index] = consumed[index] = false;
            return {swallow, false, false};
        }
        // Registration is not an IME switching request. Capture every keydown,
        // including a key whose async state was already down when the hook started.
        if (capture) { down[index] = consumed[index] = true; return {true, false, false}; }
        if (down[index]) return {consumed[index], false, false};
        down[index] = true;
        if (paused || (vk != keys.on && vk != keys.off)) return {false, false, false};
        consumed[index] = true;
        return {true, true, vk == keys.on};
    }
};

struct Request { bool turnOn; Target target; unsigned generation; };
struct App;
App* gApp = nullptr;
LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam);

struct App
{
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HHOOK hook = nullptr;
    Keys keys;
    KeyFilter filter;
    std::wstring ini;
    HICON icon = nullptr;
    NOTIFYICONDATAW tray{};
    bool trayAdded = false;
    bool paused = false;
    unsigned generation = 0;
    std::deque<Request> requests;
    UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    ~App()
    {
        if (hook) UnhookWindowsHookEx(hook);
        gApp = nullptr;
        if (trayAdded) Shell_NotifyIconW(NIM_DELETE, &tray);
        if (icon) DestroyIcon(icon);
    }

    bool Settings(const POINT* anchor = nullptr)
    {
        HKEY startupKey = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &startupKey, nullptr) != ERROR_SUCCESS)
        {
            MessageBoxW(window, L"自動起動の設定を開けません。", L"switch_ime", MB_OK | MB_ICONERROR);
            return false;
        }
        paused = true;
        ++generation;
        DialogState dialog{keys, &ini, nullptr, anchor};
        dialog.startupKey = startupKey;
        const INT_PTR result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS),
            window, SettingsProc, reinterpret_cast<LPARAM>(&dialog));
        RegCloseKey(startupKey);
        if (result == IDOK) keys = dialog.keys;
        ++generation;
        paused = false;
        return result == IDOK;
    }

    void Enqueue(bool turnOn)
    {
        Target target{};
        if (!CaptureTarget(target)) return;
        requests.push_back({turnOn, target, generation});
        if (!PostMessageW(window, kSwitchMessage, 0, 0)) requests.pop_back();
    }

    void Switch()
    {
        if (requests.empty()) return;
        const Request request = requests.front();
        requests.pop_front();
        if (paused || request.generation != generation) return;
        Target current{};
        if (!CaptureTarget(current) || !(current == request.target)) return;
        const ImeState state = ReadIme(current); // Read only for a registered physical key press.
        Target finalTarget{};
        if (!NeedsSwitch(request.turnOn, state) || !CaptureTarget(finalTarget) ||
            !(finalTarget == current)) return;
        INPUT inputs[2]{};
        MakeInputs(request.turnOn, inputs);
        SendInput(2, inputs, sizeof(INPUT)); // One attempt, without fallback or retry.
    }

    void Menu()
    {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, kSettingsCommand, L"設定");
        AppendMenuW(menu, MF_STRING, kExitCommand, L"終了");
        POINT point{};
        GetCursorPos(&point);
        // The hidden owner is disabled while DialogBox runs. Use the existing
        // dialog as the menu owner so tray interaction remains available.
        const HWND owner = gSettingsDialog ? gSettingsDialog : window;
        SetForegroundWindow(owner);
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            point.x, point.y, 0, owner, nullptr);
        DestroyMenu(menu);
        PostMessageW(owner, WM_NULL, 0, 0);
        if (command == kSettingsCommand)
        {
            POINT selected = point;
            GetCursorPos(&selected);
            if (gSettingsDialog)
            {
                ShowWindow(gSettingsDialog, SW_RESTORE);
                PositionSettings(gSettingsDialog, selected);
                SetForegroundWindow(gSettingsDialog);
            }
            else Settings(&selected);
        }
        else if (command == kExitCommand)
        {
            if (gSettingsDialog) EndDialog(gSettingsDialog, IDCANCEL);
            DestroyWindow(window);
        }
    }
};

LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && gApp && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN ||
        wParam == WM_KEYUP || wParam == WM_SYSKEYUP))
    {
        const auto& event = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        const bool keyDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
        const HWND focus = GetFocus();
        const bool capture = gApp->paused && focus &&
            (focus == gCaptureEdits[0] || focus == gCaptureEdits[1]);
        const auto decision = gApp->filter.Process(event.vkCode, keyDown,
            (event.flags & LLKHF_INJECTED) != 0, gApp->paused, gApp->keys, capture,
            event.scanCode, (event.flags & LLKHF_EXTENDED) != 0);
        if (capture && keyDown && decision.consume)
        {
            LPARAM data = static_cast<LPARAM>(event.scanCode & 0xFF) << 16;
            if (event.flags & LLKHF_EXTENDED) data |= static_cast<LPARAM>(1) << 24;
            PostMessageW(focus, kCaptureKey, event.vkCode, data);
        }
        if (decision.request) gApp->Enqueue(decision.turnOn);
        if (decision.consume) return 1;
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        return TRUE;
    }
    if (app)
    {
        if (message == kSwitchMessage) { app->Switch(); return 0; }
        if (message == kTrayMessage)
        {
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) app->Menu();
            return 0;
        }
        if (message == app->taskbarCreated && app->taskbarCreated)
        {
            app->trayAdded = Shell_NotifyIconW(NIM_ADD, &app->tray) != FALSE;
            return 0;
        }
    }
    if (message == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void Error(const wchar_t* message)
{
    MessageBoxW(nullptr, message, L"switch_ime", MB_OK | MB_ICONERROR);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    App app;
    app.instance = instance;
    if (!GetIniPath(app.ini)) { Error(L"EXEの配置フォルダを取得できません。"); return 1; }
    WNDCLASSW cls{};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = instance;
    cls.lpszClassName = kClass;
    if (!RegisterClassW(&cls)) { Error(L"アプリを初期化できません。"); return 1; }
    app.window = CreateWindowExW(WS_EX_TOOLWINDOW, kClass, L"switch_ime", WS_OVERLAPPED,
        0, 0, 0, 0, nullptr, nullptr, instance, &app);
    if (!app.window) { Error(L"アプリを初期化できません。"); return 1; }
    gApp = &app;
    for (UINT vk = 1; vk < 255; ++vk)
    {
        // DBE mode VKs describe logical mode state, not a physical held key.
        if (vk >= 0xF0 && vk <= 0xFD) continue; // VK_DBE_* range.
        const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
        if (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000)
            app.filter.down[KeyFilter::Index(vk, scan & 0xFF, (scan & 0xFF00) != 0)] = true;
    }
    app.hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, instance, 0);
    if (!app.hook)
    {
        Error(L"キーボードフックを開始できません。");
        DestroyWindow(app.window);
        return 1;
    }
    const DWORD attributes = GetFileAttributesW(app.ini.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        if (GetLastError() != ERROR_FILE_NOT_FOUND)
        {
            Error(L"INIファイルを確認できません。");
            DestroyWindow(app.window);
            return 1;
        }
        if (!app.Settings()) { DestroyWindow(app.window); return 0; }
    }
    else if (!LoadKeys(app.ini, app.keys))
    {
        Error(L"INIのキー設定が無効です。異なる2つのキーを登録してください。");
        if (!app.Settings()) { DestroyWindow(app.window); return 0; }
    }
    app.icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_TRAY), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    app.tray.cbSize = sizeof(app.tray);
    app.tray.hWnd = app.window;
    app.tray.uID = 1;
    app.tray.uFlags = NIF_ICON | NIF_MESSAGE;
    app.tray.uCallbackMessage = kTrayMessage;
    app.tray.hIcon = app.icon;
    if (!app.icon)
    {
        Error(L"トレイアイコンを作成できません。");
        DestroyWindow(app.window);
        return 1;
    }
    app.trayAdded = Shell_NotifyIconW(NIM_ADD, &app.tray) != FALSE;
    if (!app.trayAdded)
    {
        Error(L"タスクトレイにアイコンを登録できません。");
        DestroyWindow(app.window);
        return 1;
    }
    MSG message{};
    BOOL status = 0;
    while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (IsWindow(app.window)) DestroyWindow(app.window);
    return status == -1 ? 1 : 0;
}

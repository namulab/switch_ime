#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <imm.h>
#include <shellapi.h>
#include <objbase.h>
#include <commctrl.h>
#include <process.h>
#include <wtsapi32.h>
#include <sddl.h>
#include <cwchar>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <atomic>
#include "resource.h"

namespace
{
constexpr wchar_t kClass[] = L"switch_ime.Window";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kSwitchMessage = WM_APP + 2; // Notification to our own HWND only.
constexpr UINT kCaptureKey = WM_APP + 3;
constexpr UINT kElevationMessage = WM_APP + 4;
constexpr UINT kStartupErrorMessage = WM_APP + 5;
constexpr UINT kSettingsClosedMessage = WM_APP + 6;
constexpr UINT kShowSettingsMessage = WM_APP + 7;
constexpr UINT kApplySettingsMessage = WM_APP + 8;
constexpr UINT kActivateSettingsMessage = WM_APP + 9;
constexpr wchar_t kStartupUserError[] =
    L"管理者権限で実行中ですが\n"
    L"サインイン中のユーザーを取得できなかったため\n"
    L"自動起動を設定できませんでした。\n\n"
    L"通常のユーザー権限で起動し直し\n"
    L"（UACの確認をキャンセルして）\n"
    L"自動起動を設定し直してください。";
constexpr wchar_t kStartupRegistryError[] =
    L"自動起動の設定にアクセスできません。\n\n"
    L"通常のユーザー権限で起動し直し\n"
    L"（UACの確認をキャンセルして）\n"
    L"自動起動を設定し直してください。";
constexpr wchar_t kAdminHelp[] = L"管理者権限で起動されたアプリ上でもIME切り替えを有効にしたい場合は、このオプションをONにします。";
constexpr wchar_t kStartupHelp[] = L"現在サインインしているユーザーにのみ適用されます。";
void Error(const wchar_t* message);
constexpr UINT kSettingsCommand = 1;
constexpr UINT kExitCommand = 2;
constexpr WPARAM kGetOpen = 0x0005; // Legacy IMC_GETOPENSTATUS
constexpr WPARAM kGetConversion = 0x0001; // Legacy IMC_GETCONVERSIONMODE
enum class ImeState { Unknown, Alphanumeric, Japanese };

struct Keys
{
    UINT on = 0;
    UINT off = 0;
    bool runAsAdmin = false;
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

bool CaptureTarget(Target& target, bool* changed = nullptr)
{
    if (changed) *changed = false;
    target = {};
    target.foreground = GetForegroundWindow();
    if (!target.foreground) return false;
    target.thread = GetWindowThreadProcessId(target.foreground, nullptr);
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (!target.thread || !GetGUIThreadInfo(target.thread, &info) || !info.hwndFocus)
    {
        if (changed) *changed = GetForegroundWindow() != target.foreground;
        return false;
    }
    target.focus = info.hwndFocus;
    target.layout = GetKeyboardLayout(target.thread);
    const bool stable = GetForegroundWindow() == target.foreground;
    if (changed) *changed = !stable;
    return stable;
}

bool QueryIme(HWND ime, WPARAM command, DWORD_PTR& response)
{
    response = 0;
    // Delivery success is distinct from an IME response of zero.
    return SendMessageTimeoutW(ime, WM_IME_CONTROL, command, 0,
        SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 20, &response) != 0;
}

ImeState ReadIme(const Target& target, bool* changed = nullptr)
{
    if (changed) *changed = false;
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
    bool moved = false;
    if (!CaptureTarget(after, &moved))
    {
        if (changed) *changed = moved;
        return ImeState::Unknown;
    }
    if (!(after == target) || ImmGetDefaultIMEWnd(after.focus) != ime)
    {
        if (changed) *changed = true;
        return ImeState::Unknown;
    }
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

bool GetElevation(bool& elevated)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION info{};
    DWORD size = 0;
    const bool success = GetTokenInformation(token, TokenElevation, &info, sizeof(info), &size) != FALSE;
    CloseHandle(token);
    if (success) elevated = info.TokenIsElevated != 0;
    return success;
}

enum class ElevationResult { Continue, Restarted, Failed };

enum class InstanceResult { Acquired, AlreadyRunning, Failed };

struct SingleInstance
{
    HANDLE handle = nullptr;
    SingleInstance() = default;
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;
    ~SingleInstance() { if (handle) CloseHandle(handle); }

    InstanceResult Acquire(const wchar_t* name = L"Local\\namulab.switch_ime.Instance")
    {
        SetLastError(ERROR_SUCCESS);
        // Presence is the lock. SYNCHRONIZE avoids requesting write access to
        // an existing mutex created by an elevated process.
        handle = CreateMutexExW(nullptr, name, 0, SYNCHRONIZE);
        if (!handle) return InstanceResult::Failed;
        if (GetLastError() != ERROR_ALREADY_EXISTS) return InstanceResult::Acquired;
        CloseHandle(handle);
        handle = nullptr;
        return InstanceResult::AlreadyRunning;
    }
};

bool ReadElevationParent(const wchar_t* arguments, DWORD& parent)
{
    parent = 0;
    if (!arguments || !*arguments) return true;
    constexpr wchar_t prefix[] = L"--elevated ";
    constexpr size_t length = sizeof(prefix) / sizeof(wchar_t) - 1;
    if (std::wcsncmp(arguments, prefix, length) != 0) return false;
    const wchar_t* number = arguments + length;
    if (*number < L'0' || *number > L'9') return false;
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long value = std::wcstoul(number, &end, 10);
    if (errno || !value || !end || *end || value == GetCurrentProcessId()) return false;
    parent = value;
    return true;
}

bool WaitForElevationParent(DWORD parent)
{
    if (!parent) return true;
    bool elevated = false;
    if (!GetElevation(elevated) || !elevated) return false;
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, parent);
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER; // Already exited.
    const DWORD waited = WaitForSingleObject(process, 10000);
    CloseHandle(process);
    return waited == WAIT_OBJECT_0;
}

ElevationResult EnsureElevated(bool relaunched)
{
    bool elevated = false;
    if (!GetElevation(elevated)) return ElevationResult::Failed;
    if (elevated) return ElevationResult::Continue;
    // Verify the token again in the child; the argument never grants permission.
    if (relaunched) return ElevationResult::Failed;
    std::wstring exe;
    if (!GetExePath(exe)) return ElevationResult::Failed;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) return ElevationResult::Failed;
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    execute.lpVerb = L"runas";
    execute.lpFile = exe.c_str();
    const std::wstring parameters = L"--elevated " + std::to_wstring(GetCurrentProcessId());
    execute.lpParameters = parameters.c_str();
    execute.nShow = SW_HIDE;
    const bool started = ShellExecuteExW(&execute) != FALSE;
    const DWORD error = started ? ERROR_SUCCESS : GetLastError();
    CoUninitialize();
    if (started) return ElevationResult::Restarted;
    // Keep the current process usable so the user can turn this option off.
    if (error == ERROR_CANCELLED)
    {
        MessageBoxW(nullptr, L"管理者権限での起動がキャンセルされたため\n"
            L"通常のユーザー権限で動作を続けます。\n\n"
            L"管理者権限で実行中のアプリではIMEを切り替えられません。",
            L"switch_ime", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return ElevationResult::Continue;
    }
    return ElevationResult::Failed;
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
    loaded.runAsAdmin = GetPrivateProfileIntW(L"Options", L"RunAsAdmin", 0, path.c_str()) != 0;
    keys = loaded;
    return true;
}

bool SaveKeys(const std::wstring& path, const Keys& keys)
{
    if (!keys.Valid()) return false;
    char contents[128]{};
    const int length = sprintf_s(contents, "[Keys]\r\nIME_ON=0x%02X\r\nIME_OFF=0x%02X\r\n\r\n[Options]\r\nRunAsAdmin=%d\r\n",
        keys.on, keys.off, keys.runAsAdmin ? 1 : 0);
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

// Resolve the user of this session, not the account used for UAC credentials.
bool SessionUserSid(std::wstring& sid)
{
    auto query = [](WTS_INFO_CLASS kind, std::wstring& value)
    {
        LPWSTR buffer = nullptr;
        DWORD bytes = 0;
        const BOOL success = WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,
            WTS_CURRENT_SESSION, kind, &buffer, &bytes);
        bool valid = success && buffer && bytes >= sizeof(wchar_t) &&
            bytes % sizeof(wchar_t) == 0;
        if (valid)
        {
            const size_t count = bytes / sizeof(wchar_t);
            const size_t length = wcsnlen_s(buffer, count);
            valid = length > 0 && length < count;
            if (valid) value.assign(buffer, length);
        }
        if (buffer) WTSFreeMemory(buffer);
        return valid;
    };
    std::wstring user, domain;
    if (!query(WTSUserName, user) || !query(WTSDomainName, domain)) return false;
    const std::wstring account = domain + L"\\" + user;
    DWORD sidBytes = 0, domainChars = 0;
    SID_NAME_USE use{};
    LookupAccountNameW(nullptr, account.c_str(), nullptr, &sidBytes, nullptr, &domainChars, &use);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !sidBytes || !domainChars) return false;
    std::vector<BYTE> binarySid(sidBytes);
    std::vector<wchar_t> resolvedDomain(domainChars);
    if (!LookupAccountNameW(nullptr, account.c_str(), binarySid.data(), &sidBytes,
        resolvedDomain.data(), &domainChars, &use) || use != SidTypeUser ||
        !IsValidSid(binarySid.data())) return false;
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(binarySid.data(), &text)) return false;
    sid = text;
    LocalFree(text);
    return true;
}

bool OpenStartupKey(HKEY& key, const wchar_t*& error)
{
    key = nullptr;
    error = kStartupRegistryError;
    bool elevated = false;
    if (!GetElevation(elevated)) return false;
    HKEY root = HKEY_CURRENT_USER;
    if (elevated)
    {
        std::wstring sid;
        if (!SessionUserSid(sid)) { error = kStartupUserError; return false; }
        // Open only an already-loaded profile. Never create a user hive or fall back to HKCU.
        if (RegOpenKeyExW(HKEY_USERS, sid.c_str(), 0, KEY_CREATE_SUB_KEY, &root) != ERROR_SUCCESS)
            return false;
    }
    const LSTATUS status = RegCreateKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    if (elevated) RegCloseKey(root);
    if (status != ERROR_SUCCESS) { key = nullptr; return false; }
    error = nullptr;
    return true;
}

// Only our named value is changed.
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

bool StartupMatchesExe(const StartupEntry& entry, const std::wstring& exe)
{
    // Our registration is an absolute REG_SZ command. Do not expand another
    // user's environment using the account that supplied UAC credentials.
    if (!entry.exists || entry.type != REG_SZ ||
        entry.data.empty() || entry.data.size() % sizeof(wchar_t) != 0) return false;
    std::wstring command(entry.data.size() / sizeof(wchar_t), L'\0');
    std::memcpy(command.data(), entry.data.data(), entry.data.size());
    const auto end = command.find(L'\0');
    if (end == std::wstring::npos) return false;
    command.resize(end);
    const auto start = command.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return false; // Empty command lines have special argv behavior.
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(command.c_str() + start, &count);
    if (!args) return false;
    const std::wstring registered = count == 1 ? args[0] : L"";
    LocalFree(args);
    auto fullPath = [](const std::wstring& path)
    {
        // Do not infer what a relative Run command would resolve to at sign-in.
        const bool absolute = (path.size() >= 3 && path[1] == L':' &&
            (path[2] == L'\\' || path[2] == L'/')) ||
            (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\');
        if (!absolute) return std::wstring{};
        const DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
        if (!size) return std::wstring{};
        std::vector<wchar_t> buffer(size);
        const DWORD length = GetFullPathNameW(path.c_str(), size, buffer.data(), nullptr);
        return length && length < size ? std::wstring(buffer.data(), length) : std::wstring{};
    };
    const auto registeredPath = fullPath(registered), ownPath = fullPath(exe);
    return !registeredPath.empty() && !ownPath.empty() &&
        CompareStringOrdinal(registeredPath.c_str(), -1, ownPath.c_str(), -1, TRUE) == CSTR_EQUAL;
}

struct DialogState
{
    Keys keys;
    const std::wstring* ini;
    HFONT font = nullptr;
    const POINT* anchor = nullptr;
    HKEY startupKey = nullptr;
    StartupEntry originalStartup;
    HWND tooltip = nullptr;
    const wchar_t* startupError = nullptr;
    std::atomic<HWND>* visibleDialog = nullptr;
    std::atomic<bool>* closing = nullptr;
    HWND applyWindow = nullptr;
};
thread_local WNDPROC gEditProc = nullptr;
thread_local HWND gCaptureEdits[2]{};
thread_local HWND gSettingsDialog = nullptr;
struct KeyboardMonitor;
thread_local KeyboardMonitor* gSettingsMonitor = nullptr;
void PublishKeyboardSettings();

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
        {IDC_ADMIN, {7, 50, 170, 60}},
        {IDOK, {43, 66, 103, 80}}, {IDCANCEL, {110, 66, 170, 80}}
    };
    RECT previous{};
    GetWindowRect(dialog, &previous);
    RECT size{0, 0, 177, 85};
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
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (state && state->tooltip) SendMessageW(state->tooltip, WM_SETFONT, font, FALSE);
}

bool CreateSettingsTooltips(HWND dialog, DialogState& state)
{
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    if (!InitCommonControlsEx(&controls)) return false;
    state.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, dialog, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!state.tooltip) return false;
    SendMessageW(state.tooltip, WM_SETFONT,
        SendMessageW(GetDlgItem(dialog, IDC_ADMIN), WM_GETFONT, 0, 0), FALSE);
    RECT width{0, 0, 190, 0};
    MapDialogRect(dialog, &width);
    SendMessageW(state.tooltip, TTM_SETMAXTIPWIDTH, 0, width.right);
    SendMessageW(state.tooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 100);
    SendMessageW(state.tooltip, TTM_SETDELAYTIME, TTDT_RESHOW, 100);
    struct Help { int id; const wchar_t* text; };
    for (const auto& help : {Help{IDC_ADMIN, kAdminHelp}, Help{IDC_STARTUP, kStartupHelp}})
    {
        TOOLINFOW tool{};
        tool.cbSize = TTTOOLINFOW_V2_SIZE;
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = dialog;
        tool.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(dialog, help.id));
        tool.lpszText = const_cast<wchar_t*>(help.text);
        if (!SendMessageW(state.tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool))) return false;
    }
    return true;
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
        if (state->visibleDialog) state->visibleDialog->store(dialog);
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
            state->font = CreateFontIndirectW(&metrics.lfMessageFont);
        if (state->font) SendMessageW(dialog, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), FALSE);
        LayoutSettings(dialog, state->font);
        CheckDlgButton(dialog, IDC_ADMIN, state->keys.runAsAdmin ? BST_CHECKED : BST_UNCHECKED);
        CreateSettingsTooltips(dialog, *state);
        if (state->startupKey && !ReadStartup(state->startupKey, state->originalStartup))
        {
            state->startupKey = nullptr;
            state->originalStartup = {};
            state->startupError = kStartupRegistryError;
        }
        std::wstring exe;
        const bool registered = GetExePath(exe) && StartupMatchesExe(state->originalStartup, exe);
        CheckDlgButton(dialog, IDC_STARTUP, registered ? BST_CHECKED : BST_UNCHECKED);
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
        PublishKeyboardSettings();
        if (state->anchor) PositionSettings(dialog, *state->anchor);
        // DialogBox performs its initial show after WM_INITDIALOG. A hidden
        // startup can suppress that first show, so activate from its message loop.
        PostMessageW(dialog, kActivateSettingsMessage, 0, 0);
        if (state->startupError) PostMessageW(dialog, kStartupErrorMessage, 0, 0);
        if (state->closing && state->closing->load()) PostMessageW(dialog, WM_CLOSE, 0, 0);
        return TRUE;
    }
    if (message == kShowSettingsMessage || message == kActivateSettingsMessage)
    {
        if (message == kShowSettingsMessage)
            PositionSettings(dialog, {static_cast<LONG>(wParam), static_cast<LONG>(lParam)});
        ShowWindow(dialog, SW_RESTORE);
        SetForegroundWindow(dialog);
        return TRUE;
    }
    if (message == kStartupErrorMessage && state && state->startupError)
    {
        MessageBoxW(dialog, state->startupError, L"switch_ime", MB_OK | MB_ICONERROR);
        state->startupError = nullptr;
        return TRUE;
    }
    if (message == WM_COMMAND && state)
    {
        if (LOWORD(wParam) == IDOK)
        {
            state->keys.runAsAdmin = IsDlgButtonChecked(dialog, IDC_ADMIN) == BST_CHECKED;
            if (!state->keys.Valid())
            {
                MessageBoxW(dialog, L"ONとOFFに異なるキーを登録してください。", L"switch_ime", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            StartupEntry desired;
            StartupEntry beforeStartup;
            std::wstring exe;
            if (state->startupKey && (!ReadStartup(state->startupKey, beforeStartup) || !GetExePath(exe)))
            {
                MessageBoxW(dialog, L"自動起動の登録状態を取得できません。", L"switch_ime", MB_OK | MB_ICONERROR);
                return TRUE;
            }
            const bool enableStartup = IsDlgButtonChecked(dialog, IDC_STARTUP) == BST_CHECKED;
            const bool updateStartup = state->startupKey &&
                (enableStartup || StartupMatchesExe(beforeStartup, exe));
            if (state->startupKey && enableStartup)
            {
                if (!MakeStartupEntry(exe, desired))
                {
                    MessageBoxW(dialog, L"自動起動に登録できません。EXEのパスが長すぎないか確認してください。",
                        L"switch_ime", MB_OK | MB_ICONERROR);
                    return TRUE;
                }
            }
            if (updateStartup && !WriteStartup(state->startupKey, desired))
            {
                MessageBoxW(dialog, L"自動起動の登録を変更できません。", L"switch_ime", MB_OK | MB_ICONERROR);
                return TRUE;
            }
            if (!SaveKeys(*state->ini, state->keys))
            {
                if (updateStartup && !WriteStartup(state->startupKey, beforeStartup))
                    MessageBoxW(dialog, L"自動起動の登録状態を元に戻せません。設定を再確認してください。",
                        L"switch_ime", MB_OK | MB_ICONERROR);
                MessageBoxW(dialog, L"設定を保存できません。EXEのフォルダへの書き込み権限を確認してください。",
                    L"switch_ime", MB_OK | MB_ICONERROR);
                return TRUE;
            }
            // Notify only our own main window; do not close until the new mapping is applied.
            if (state->applyWindow) SendMessageW(state->applyWindow, kApplySettingsMessage,
                0, reinterpret_cast<LPARAM>(&state->keys));
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
    }
    if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
    if (message == WM_DESTROY)
    {
        if (state && state->tooltip) { DestroyWindow(state->tooltip); state->tooltip = nullptr; }
        gCaptureEdits[0] = gCaptureEdits[1] = nullptr;
        PublishKeyboardSettings();
        if (gSettingsDialog == dialog) gSettingsDialog = nullptr;
        if (state && state->visibleDialog) state->visibleDialog->store(nullptr);
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

struct Request { bool turnOn; };
struct KeyboardSettings
{
    Keys keys;
    bool paused = false;
    HWND edits[2]{};
    HWND window = nullptr;
    DWORD uiThread = 0;
};

struct KeyboardMonitor;
thread_local KeyboardMonitor* gKeyboardMonitor = nullptr;
LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam);

struct KeyboardMonitor
{
    SRWLOCK lock = SRWLOCK_INIT;
    KeyboardSettings settings;
    std::deque<Request> requests;
    KeyFilter filter; // Accessed only by the main thread.
    HHOOK hook = nullptr;

    ~KeyboardMonitor() { Stop(); }

    void Publish(const KeyboardSettings& next)
    {
        AcquireSRWLockExclusive(&lock);
        settings = next;
        ReleaseSRWLockExclusive(&lock);
    }

    KeyboardSettings Snapshot()
    {
        AcquireSRWLockShared(&lock);
        const auto result = settings;
        ReleaseSRWLockShared(&lock);
        return result;
    }

    void Enqueue(bool turnOn, const KeyboardSettings& current)
    {
        AcquireSRWLockExclusive(&lock);
        requests.push_back({turnOn});
        ReleaseSRWLockExclusive(&lock);
        if (!PostMessageW(current.window, kSwitchMessage, 0, 0))
        {
            AcquireSRWLockExclusive(&lock);
            if (!requests.empty()) requests.pop_back();
            ReleaseSRWLockExclusive(&lock);
        }
    }

    void Defer(const Request& request)
    {
        // Keep the intent and its position in the sequence; query the new input target next.
        AcquireSRWLockExclusive(&lock);
        requests.push_front(request);
        const HWND window = settings.window;
        ReleaseSRWLockExclusive(&lock);
        PostMessageW(window, kSwitchMessage, 0, 0);
    }

    bool Take(Request& request)
    {
        AcquireSRWLockExclusive(&lock);
        const bool available = !requests.empty();
        if (available) { request = requests.front(); requests.pop_front(); }
        ReleaseSRWLockExclusive(&lock);
        return available;
    }

    void UpdateKeys(const Keys& keys, bool paused, HWND window)
    {
        AcquireSRWLockExclusive(&lock);
        settings.keys = keys;
        settings.paused = paused;
        settings.window = window;
        ReleaseSRWLockExclusive(&lock);
    }

    void CaptureFields(HWND on, HWND off, DWORD threadId)
    {
        AcquireSRWLockExclusive(&lock);
        settings.edits[0] = on;
        settings.edits[1] = off;
        settings.uiThread = threadId;
        ReleaseSRWLockExclusive(&lock);
    }

    bool Start()
    {
        gKeyboardMonitor = this;
        for (UINT vk = 1; vk < 255; ++vk)
        {
            if (vk >= 0xF0 && vk <= 0xFD) continue;
            const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
            if (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000)
                filter.down[KeyFilter::Index(vk, scan & 0xFF, (scan & 0xFF00) != 0)] = true;
        }
        hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, GetModuleHandleW(nullptr), 0);
        if (!hook) gKeyboardMonitor = nullptr;
        return hook != nullptr;
    }

    void Stop()
    {
        if (hook) { UnhookWindowsHookEx(hook); hook = nullptr; }
        if (gKeyboardMonitor == this) gKeyboardMonitor = nullptr;
    }

};

// The main thread owns this object. The worker owns its dialog and file/registry I/O.
// Results are read only after joining the worker; HWND and close requests are atomic.
struct SettingsTask
{
    HANDLE thread = nullptr;
    HINSTANCE instance = nullptr;
    HWND mainWindow = nullptr;
    KeyboardMonitor* monitor = nullptr;
    Keys keys;
    std::wstring ini;
    POINT anchor{};
    bool anchored = false;
    bool initial = false;
    INT_PTR result = IDCANCEL;
    std::atomic<HWND> dialog{nullptr};
    std::atomic<bool> closing{false};

    static unsigned __stdcall Run(void* context)
    {
        auto& task = *static_cast<SettingsTask*>(context);
        gSettingsMonitor = task.monitor;
        HKEY startupKey = nullptr;
        const wchar_t* error = nullptr;
        OpenStartupKey(startupKey, error);
        if (!task.closing.load())
        {
            DialogState state{task.keys, &task.ini, nullptr, task.anchored ? &task.anchor : nullptr};
            state.startupKey = startupKey;
            state.startupError = error;
            state.visibleDialog = &task.dialog;
            state.closing = &task.closing;
            state.applyWindow = task.mainWindow;
            // No cross-thread owner: never disable or attach the main thread's input queue.
            task.result = DialogBoxParamW(task.instance, MAKEINTRESOURCEW(IDD_SETTINGS),
                nullptr, SettingsProc, reinterpret_cast<LPARAM>(&state));
            if (task.result == IDOK) task.keys = state.keys;
        }
        if (startupKey) RegCloseKey(startupKey);
        gSettingsMonitor = nullptr;
        PostMessageW(task.mainWindow, kSettingsClosedMessage, 0, 0);
        return 0;
    }

    bool Start()
    {
        result = IDCANCEL;
        closing.store(false);
        thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, Run, this, 0, nullptr));
        return thread != nullptr;
    }

    void Close()
    {
        closing.store(true);
        if (const HWND view = dialog.load()) PostMessageW(view, WM_CLOSE, 0, 0);
    }

    void Join()
    {
        if (!thread) return;
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
        thread = nullptr;
    }

    ~SettingsTask() { Close(); Join(); }
};

struct App
{
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    Keys keys;
    KeyboardMonitor monitor;
    SettingsTask settings;
    bool exiting = false;
    std::wstring ini;
    HICON icon = nullptr;
    NOTIFYICONDATAW tray{};
    bool trayAdded = false;
    bool paused = false;
    UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    ~App()
    {
        monitor.Stop();
        settings.Close();
        settings.Join();
        if (trayAdded) Shell_NotifyIconW(NIM_DELETE, &tray);
        if (icon) DestroyIcon(icon);
    }

    bool AddTray()
    {
        icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_TRAY), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        tray.cbSize = sizeof(tray);
        tray.hWnd = window;
        tray.uID = 1;
        tray.uFlags = NIF_ICON | NIF_MESSAGE;
        tray.uCallbackMessage = kTrayMessage;
        tray.hIcon = icon;
        if (!icon)
        {
            Error(L"トレイアイコンを作成できません。");
            return false;
        }
        trayAdded = Shell_NotifyIconW(NIM_ADD, &tray) != FALSE;
        if (!trayAdded)
        {
            Error(L"タスクトレイにアイコンを登録できません。");
            return false;
        }
        return true;
    }

    void Publish()
    {
        monitor.UpdateKeys(keys, paused, window);
    }

    bool Settings(const POINT* anchor = nullptr)
    {
        if (settings.thread)
        {
            if (const HWND view = settings.dialog.load())
            {
                POINT point{};
                if (anchor) point = *anchor;
                else GetCursorPos(&point);
                PostMessageW(view, kShowSettingsMessage,
                    static_cast<WPARAM>(point.x), static_cast<LPARAM>(point.y));
            }
            return true;
        }
        settings.instance = instance;
        settings.mainWindow = window;
        settings.monitor = &monitor;
        settings.keys = keys;
        settings.ini = ini;
        settings.initial = !keys.Valid();
        settings.anchored = anchor != nullptr;
        if (anchor) settings.anchor = *anchor;
        paused = !keys.Valid();
        Publish();
        if (settings.Start()) return true;
        Error(L"設定画面を開始できません。");
        return false;
    }

    void SettingsClosed()
    {
        if (!settings.thread) return;
        settings.Join();
        if (settings.result == -1) Error(L"設定画面を表示できません。");
        if (exiting || (settings.initial && settings.result != IDOK))
        {
            DestroyWindow(window);
            return;
        }
        paused = !keys.Valid();
        Publish();
        if (settings.initial && !AddTray()) { DestroyWindow(window); return; }
        if (settings.result == IDOK && keys.runAsAdmin) PostMessageW(window, kElevationMessage, 0, 0);
    }

    void Exit()
    {
        exiting = true;
        if (settings.thread) settings.Close();
        else DestroyWindow(window);
    }

    void Switch()
    {
        Request request{};
        if (!monitor.Take(request)) return;
        Target current{};
        bool changed = false;
        if (!CaptureTarget(current, &changed))
        {
            if (changed) monitor.Defer(request);
            return;
        }
        bool readTargetChanged = false;
        const ImeState state = ReadIme(current, &readTargetChanged);
        Target finalTarget{};
        if (!CaptureTarget(finalTarget, &changed))
        {
            if (changed) monitor.Defer(request);
            return;
        }
        if (readTargetChanged || !(finalTarget == current))
        {
            monitor.Defer(request);
            return;
        }
        if (!NeedsSwitch(request.turnOn, state)) return;
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
        const HWND owner = window;
        SetForegroundWindow(owner);
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            point.x, point.y, 0, owner, nullptr);
        DestroyMenu(menu);
        PostMessageW(owner, WM_NULL, 0, 0);
        if (command == kSettingsCommand)
        {
            POINT selected = point;
            GetCursorPos(&selected);
            Settings(&selected);
        }
        else if (command == kExitCommand) Exit();
    }
};

void PublishKeyboardSettings()
{
    if (gSettingsMonitor) gSettingsMonitor->CaptureFields(
        gCaptureEdits[0], gCaptureEdits[1], GetCurrentThreadId());
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && gKeyboardMonitor && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN ||
        wParam == WM_KEYUP || wParam == WM_SYSKEYUP))
    {
        const auto& event = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (event.flags & LLKHF_INJECTED) return CallNextHookEx(nullptr, code, wParam, lParam);
        const bool keyDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
        auto& monitor = *gKeyboardMonitor;
        const auto current = monitor.Snapshot();
        GUITHREADINFO gui{};
        gui.cbSize = sizeof(gui);
        const HWND focus = (current.edits[0] || current.edits[1]) && current.uiThread &&
            GetWindowThreadProcessId(GetForegroundWindow(), nullptr) == current.uiThread &&
            GetGUIThreadInfo(current.uiThread, &gui)
            ? gui.hwndFocus : nullptr;
        const bool capture = focus && (focus == current.edits[0] || focus == current.edits[1]);
        const auto decision = monitor.filter.Process(event.vkCode, keyDown,
            (event.flags & LLKHF_INJECTED) != 0, current.paused, current.keys, capture,
            event.scanCode, (event.flags & LLKHF_EXTENDED) != 0);
        if (capture && keyDown && decision.consume)
        {
            LPARAM data = static_cast<LPARAM>(event.scanCode & 0xFF) << 16;
            if (event.flags & LLKHF_EXTENDED) data |= static_cast<LPARAM>(1) << 24;
            PostMessageW(focus, kCaptureKey, event.vkCode, data);
        }
        if (decision.request) monitor.Enqueue(decision.turnOn, current);
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
        if (message == kApplySettingsMessage)
        {
            const auto& keys = *reinterpret_cast<const Keys*>(lParam);
            if (app->exiting || !keys.Valid()) return FALSE;
            app->keys = keys;
            app->paused = false;
            app->Publish();
            return TRUE;
        }
        if (message == kElevationMessage)
        {
            if (!app->keys.runAsAdmin) return 0;
            const auto elevation = EnsureElevated(false);
            if (elevation != ElevationResult::Continue)
            {
                if (elevation == ElevationResult::Failed) Error(L"管理者権限で起動できません。");
                DestroyWindow(window);
            }
            return 0;
        }
        if (message == kSettingsClosedMessage) { app->SettingsClosed(); return 0; }
        if (message == WM_CLOSE) { app->Exit(); return 0; }
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
        if (app) app->monitor.Stop();
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

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int)
{
    DWORD parent = 0;
    if (!ReadElevationParent(arguments, parent) || !WaitForElevationParent(parent))
    {
        Error(L"管理者権限での再起動を完了できません。");
        return 1;
    }
    // Declared before App so hooks and the tray are removed before releasing
    // the instance handle, including on failed startup or normal shutdown.
    SingleInstance singleInstance;
    const auto acquired = singleInstance.Acquire();
    if (acquired == InstanceResult::AlreadyRunning) return 0;
    if (acquired != InstanceResult::Acquired)
    {
        Error(L"多重起動の確認に失敗しました。");
        return 1;
    }
    App app;
    app.instance = instance;
    if (!GetIniPath(app.ini)) { Error(L"EXEの配置フォルダを取得できません。"); return 1; }
    const bool loaded = LoadKeys(app.ini, app.keys);
    const bool relaunched = parent != 0;
    if ((loaded && app.keys.runAsAdmin) || relaunched)
    {
        const auto elevation = EnsureElevated(relaunched);
        if (elevation != ElevationResult::Continue)
        {
            if (elevation == ElevationResult::Failed) Error(L"管理者権限で起動できません。");
            return elevation == ElevationResult::Failed ? 1 : 0;
        }
    }
    WNDCLASSW cls{};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = instance;
    cls.lpszClassName = kClass;
    if (!RegisterClassW(&cls)) { Error(L"アプリを初期化できません。"); return 1; }
    app.window = CreateWindowExW(WS_EX_TOOLWINDOW, kClass, L"switch_ime", WS_OVERLAPPED,
        0, 0, 0, 0, nullptr, nullptr, instance, &app);
    if (!app.window) { Error(L"アプリを初期化できません。"); return 1; }
    // Explorer may run at a lower integrity level than this process.
    if (!ChangeWindowMessageFilterEx(app.window, kTrayMessage, MSGFLT_ALLOW, nullptr) ||
        (app.taskbarCreated && !ChangeWindowMessageFilterEx(app.window, app.taskbarCreated, MSGFLT_ALLOW, nullptr)))
    {
        Error(L"タスクトレイの通知を受信できません。");
        DestroyWindow(app.window);
        return 1;
    }
    app.Publish();
    if (!app.monitor.Start())
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
    else if (!loaded)
    {
        Error(L"INIのキー設定が無効です。異なる2つのキーを登録してください。");
        if (!app.Settings()) { DestroyWindow(app.window); return 0; }
    }
    if (loaded && !app.AddTray()) { DestroyWindow(app.window); return 1; }
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

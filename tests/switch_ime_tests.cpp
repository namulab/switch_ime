// Separate executable; these checks are not compiled into switch_ime.exe.
#include <windows.h>
#include <imm.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include <sddl.h>
#include <string>
#include <atomic>
HANDLE testIoEntered = nullptr, testIoRelease = nullptr;
std::atomic<int> testIoStage{0}; // 1: open settings; 2: flush INI.
HKEY testStartupOverride = nullptr;
void WaitForTestIo(int stage)
{
    int expected = stage;
    if (!testIoStage.compare_exchange_strong(expected, 0)) return;
    SetEvent(testIoEntered);
    WaitForSingleObject(testIoRelease, 10000);
}
BOOL WINAPI TestFlushFileBuffers(HANDLE file)
{
    WaitForTestIo(2);
    return FlushFileBuffers(file);
}
bool testSessionFailure = false;
bool testSessionAccount = false;
bool testRegistryProbe = false;
HKEY testRegistryRoot = nullptr;
std::wstring testRegistryPath;
BOOL WINAPI TestWTSQuerySessionInformation(HANDLE server, DWORD session, WTS_INFO_CLASS kind,
    LPWSTR* buffer, DWORD* bytes)
{
    if (testSessionFailure) { *buffer = nullptr; *bytes = 0; return FALSE; }
    if (testSessionAccount)
    {
        const wchar_t* value = kind == WTSUserName ? L"InteractiveUser" : L"TESTDOMAIN";
        *bytes = static_cast<DWORD>((lstrlenW(value) + 1) * sizeof(wchar_t));
        *buffer = static_cast<LPWSTR>(HeapAlloc(GetProcessHeap(), 0, *bytes));
        if (!*buffer) return FALSE;
        CopyMemory(*buffer, value, *bytes);
        return TRUE;
    }
    return WTSQuerySessionInformationW(server, session, kind, buffer, bytes);
}
void WINAPI TestWTSFreeMemory(void* buffer)
{
    if (testSessionAccount) HeapFree(GetProcessHeap(), 0, buffer);
    else WTSFreeMemory(buffer);
}
BOOL WINAPI TestLookupAccountName(LPCWSTR system, LPCWSTR account, PSID sid, LPDWORD bytes,
    LPWSTR domain, LPDWORD chars, PSID_NAME_USE use)
{
    if (!testSessionAccount) return LookupAccountNameW(system, account, sid, bytes, domain, chars, use);
    if (lstrcmpW(account, L"TESTDOMAIN\\InteractiveUser") != 0) return FALSE;
    PSID expected = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-5-21-111-222-333-1001", &expected)) return FALSE;
    const DWORD needed = GetLengthSid(expected);
    const DWORD domainChars = 11;
    if (!sid || !domain || *bytes < needed || *chars < domainChars)
    {
        *bytes = needed;
        *chars = domainChars;
        LocalFree(expected);
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    CopySid(*bytes, sid, expected);
    lstrcpyW(domain, L"TESTDOMAIN");
    *use = SidTypeUser;
    LocalFree(expected);
    return TRUE;
}
LSTATUS WINAPI TestRegOpenKeyEx(HKEY root, LPCWSTR path, DWORD options, REGSAM access, PHKEY key)
{
    if (testRegistryProbe)
    {
        testRegistryRoot = root;
        testRegistryPath = path;
        return ERROR_ACCESS_DENIED;
    }
    return RegOpenKeyExW(root, path, options, access, key);
}
LSTATUS WINAPI TestRegCreateKeyEx(HKEY root, LPCWSTR path, DWORD reserved, LPWSTR cls,
    DWORD options, REGSAM access, LPSECURITY_ATTRIBUTES security, PHKEY key, DWORD* disposition)
{
    if (testStartupOverride && root == HKEY_CURRENT_USER &&
        lstrcmpW(path, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run") == 0)
    {
        WaitForTestIo(1);
        return RegOpenKeyExW(testStartupOverride, L"", 0, access, key);
    }
    if (testRegistryProbe)
    {
        testRegistryRoot = root;
        testRegistryPath = path;
        return ERROR_ACCESS_DENIED;
    }
    return RegCreateKeyExW(root, path, reserved, cls, options, access, security, key, disposition);
}
HOOKPROC testHookCallback = nullptr;
HANDLE testHookEvent = nullptr;
volatile LONG testHookCommand = 0, testHookKey = 0, testHookResult = 0;
DWORD testHookInstallThread = 0, testHookRemoveThread = 0;
UINT_PTR testHookTimer = 0;
bool testHookFailure = false;
void CALLBACK TestHookTimer(HWND, UINT, UINT_PTR, DWORD)
{
    if (!InterlockedExchange(&testHookCommand, 0)) return;
    KBDLLHOOKSTRUCT key{};
    key.vkCode = static_cast<DWORD>(InterlockedCompareExchange(&testHookKey, 0, 0));
    const LRESULT down = testHookCallback(HC_ACTION, WM_KEYDOWN, reinterpret_cast<LPARAM>(&key));
    const LRESULT up = testHookCallback(HC_ACTION, WM_KEYUP, reinterpret_cast<LPARAM>(&key));
    InterlockedExchange(&testHookResult, down == 1 && up == 1 ? 1 : down == 0 && up == 0 ? 0 : -1);
    SetEvent(testHookEvent);
}
HHOOK WINAPI TestSetWindowsHookEx(int, HOOKPROC callback, HINSTANCE, DWORD)
{
    testHookInstallThread = GetCurrentThreadId();
    if (testHookFailure) return nullptr;
    testHookCallback = callback;
    testHookTimer = SetTimer(nullptr, 0, 20, TestHookTimer);
    return reinterpret_cast<HHOOK>(1);
}
BOOL WINAPI TestUnhookWindowsHookEx(HHOOK)
{
    testHookRemoveThread = GetCurrentThreadId();
    KillTimer(nullptr, testHookTimer);
    return TRUE;
}
LRESULT WINAPI TestCallNextHookEx(HHOOK, int, WPARAM, LPARAM) { return 0; }
int testElevation = -1; // -1: real query; -2: fail; 0/1: simulated token.
unsigned testElevationLaunches = 0;
DWORD testElevationLaunchError = ERROR_SUCCESS;
bool testElevationCommand = false;
BOOL WINAPI TestGetTokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS kind,
    LPVOID info, DWORD size, PDWORD returned)
{
    if (testElevation == -1 || kind != TokenElevation)
        return GetTokenInformation(token, kind, info, size, returned);
    if (testElevation == -2) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    if (size < sizeof(TOKEN_ELEVATION)) return FALSE;
    static_cast<TOKEN_ELEVATION*>(info)->TokenIsElevated = static_cast<DWORD>(testElevation);
    *returned = sizeof(TOKEN_ELEVATION);
    return TRUE;
}
BOOL WINAPI TestShellExecuteEx(SHELLEXECUTEINFOW* execute)
{
    ++testElevationLaunches;
    wchar_t expected[64]{};
    wsprintfW(expected, L"--elevated %lu", GetCurrentProcessId());
    testElevationCommand = lstrcmpW(execute->lpVerb, L"runas") == 0 &&
        execute->lpFile && *execute->lpFile && lstrcmpW(execute->lpParameters, expected) == 0 &&
        (execute->fMask & SEE_MASK_NOASYNC) && execute->nShow == SW_HIDE;
    SetLastError(testElevationLaunchError);
    return testElevationLaunchError == ERROR_SUCCESS;
}
unsigned testImeQueries = 0;
HWND testImeWindow = nullptr;
HWND testLastImeFocus = nullptr;
HWND testSwitchForeground = nullptr, testSwitchFocus = nullptr;
bool testJapaneseMode = false;
HKL testLayout = nullptr;
HKL WINAPI TestGetKeyboardLayout(DWORD thread)
{
    return testLayout ? testLayout : GetKeyboardLayout(thread);
}
HWND WINAPI TestImmGetDefaultIMEWnd(HWND focus)
{
    ++testImeQueries;
    testLastImeFocus = focus;
    return testImeWindow;
}
unsigned testSendInputs = 0;
DWORD testSendInputThread = 0;
WORD testLastInput = 0;
UINT WINAPI TestSendInput(UINT count, LPINPUT inputs, int size)
{
    if (count != 2 || size != sizeof(INPUT) ||
        (inputs[0].ki.wVk != VK_IME_ON && inputs[0].ki.wVk != VK_IME_OFF) ||
        inputs[1].ki.wVk != inputs[0].ki.wVk || inputs[0].ki.dwFlags != 0 ||
        inputs[1].ki.dwFlags != KEYEVENTF_KEYUP) return 0;
    ++testSendInputs;
    testSendInputThread = GetCurrentThreadId();
    testLastInput = inputs[0].ki.wVk;
    return count;
}
UINT testMenuCommand = 0;
HWND testMenuOwner = nullptr;
unsigned testMenuCalls = 0;
unsigned testErrorMessages = 0;
std::wstring testMessageText;
std::atomic<HWND> testForeground{nullptr};
unsigned testForegroundReads = 0;
HWND WINAPI TestGetForegroundWindow()
{
    ++testForegroundReads;
    const HWND window = testForeground.load();
    return window ? window : GetForegroundWindow();
}
int WINAPI TestMessageBox(HWND, LPCWSTR text, LPCWSTR, UINT)
{
    ++testErrorMessages;
    testMessageText = text;
    return IDOK;
}
BOOL WINAPI TestTrackPopupMenu(HMENU, UINT, int, int, int, HWND owner, const RECT*)
{
    testMenuOwner = owner;
    ++testMenuCalls;
    return static_cast<BOOL>(testMenuCommand);
}
#define TrackPopupMenu TestTrackPopupMenu
#define MessageBoxW TestMessageBox
#define ImmGetDefaultIMEWnd TestImmGetDefaultIMEWnd
#define GetTokenInformation TestGetTokenInformation
#define ShellExecuteExW TestShellExecuteEx
#define SetWindowsHookExW TestSetWindowsHookEx
#define UnhookWindowsHookEx TestUnhookWindowsHookEx
#define CallNextHookEx TestCallNextHookEx
#define GetForegroundWindow TestGetForegroundWindow
#define WTSQuerySessionInformationW TestWTSQuerySessionInformation
#define WTSFreeMemory TestWTSFreeMemory
#define LookupAccountNameW TestLookupAccountName
#define RegOpenKeyExW TestRegOpenKeyEx
#define RegCreateKeyExW TestRegCreateKeyEx
#define FlushFileBuffers TestFlushFileBuffers
#define SendInput TestSendInput
#define GetKeyboardLayout TestGetKeyboardLayout
#include "../switch_ime.cpp"
#undef GetKeyboardLayout
#undef SendInput
#undef FlushFileBuffers
#undef RegCreateKeyExW
#undef RegOpenKeyExW
#undef WTSQuerySessionInformationW
#undef WTSFreeMemory
#undef LookupAccountNameW
#undef GetForegroundWindow
#undef TrackPopupMenu
#undef MessageBoxW
#undef ImmGetDefaultIMEWnd
#undef GetTokenInformation
#undef ShellExecuteExW
#undef SetWindowsHookExW
#undef UnhookWindowsHookEx
#undef CallNextHookEx
#include <cstdlib>
#include "../version.h"

namespace
{
void Check(bool condition, const char* description)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", description); std::exit(1); }
    std::printf("PASS: %s\n", description);
}

void CheckKeyboardMonitor()
{
    KeyboardMonitor monitor;
    const DWORD uiThread = GetCurrentThreadId();
    monitor.Publish({{VK_F13, VK_F14}, false, {}, nullptr, uiThread});
    testHookEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(testHookEvent != nullptr && monitor.Start(), "keyboard monitor starts on main thread");
    Check(testHookInstallThread == uiThread, "hook is installed on main thread");
    auto press = [&](UINT vk, LONG expected)
    {
        ResetEvent(testHookEvent);
        InterlockedExchange(&testHookKey, static_cast<LONG>(vk));
        InterlockedExchange(&testHookCommand, 1);
        TestHookTimer(nullptr, 0, 0, 0);
        Check(InterlockedCompareExchange(&testHookResult, 0, 0) == expected,
            "hook uses current published key configuration");
    };
    press(VK_F13, 1);
    monitor.Publish({{VK_F15, VK_F16}, false, {}, nullptr, uiThread});
    press(VK_F13, 0);
    press(VK_F15, 1);
    monitor.Publish({{VK_F15, VK_F16}, true, {}, nullptr, uiThread});
    press(VK_F15, 0);
    monitor.Publish({{VK_F15, VK_F16}, false, {}, nullptr, uiThread});
    press(VK_F15, 1);
    monitor.Stop();
    Check(!monitor.hook && testHookRemoveThread == uiThread, "monitor unhooks on main thread");
    CloseHandle(testHookEvent);
    testHookEvent = nullptr;
    testHookFailure = true;
    Check(!monitor.Start() && !monitor.hook, "failed hook installation reports failure");
    testHookFailure = false;
}

void CheckStartupUser()
{
    testRegistryProbe = true;
    testSessionFailure = true;
    testElevation = 1;
    HKEY key = nullptr;
    const wchar_t* error = nullptr;
    Check(!OpenStartupKey(key, error) && !key && error == kStartupUserError &&
        !testRegistryRoot, "unknown session user blocks all startup registry access");
    testSessionFailure = false;
    testSessionAccount = true;
    std::wstring sid;
    Check(SessionUserSid(sid) && sid == L"S-1-5-21-111-222-333-1001",
        "interactive session account resolves to a user SID");
    Check(!OpenStartupKey(key, error) && !key && error == kStartupRegistryError &&
        testRegistryRoot == HKEY_USERS && testRegistryPath == sid,
        "elevated startup uses session user's loaded hive without HKCU fallback on access denial");
    testSessionFailure = true;
    testElevation = 0;
    Check(!OpenStartupKey(key, error) && error == kStartupRegistryError &&
        testRegistryRoot == HKEY_CURRENT_USER,
        "normal startup uses current user's registry even when WTS is unavailable");
    testSessionFailure = false;
    testSessionAccount = false;
    testRegistryProbe = false;
    testRegistryRoot = nullptr;
    testElevation = -1;
}

void CheckSingleInstance()
{
    const std::wstring name = L"Local\\switch_ime.Test.Instance." + std::to_wstring(GetCurrentProcessId());
    {
        SingleInstance first;
        Check(first.Acquire(name.c_str()) == InstanceResult::Acquired, "first instance acquires named mutex");
        SingleInstance second;
        Check(second.Acquire(name.c_str()) == InstanceResult::AlreadyRunning,
            "second instance is rejected without affecting first");
    }
    SingleInstance next;
    Check(next.Acquire(name.c_str()) == InstanceResult::Acquired,
        "instance guard releases on exit and permits a new instance");
    DWORD parent = 1;
    Check(ReadElevationParent(L"", parent) && parent == 0, "normal startup has no elevation parent");
    const std::wstring argument = L"--elevated " + std::to_wstring(GetCurrentProcessId() + 1);
    Check(ReadElevationParent(argument.c_str(), parent) && parent == GetCurrentProcessId() + 1,
        "elevated restart identifies the process that must exit first");
    Check(!ReadElevationParent(L"--elevated", parent) && !ReadElevationParent(L"--elevated 0", parent) &&
        !ReadElevationParent(L"--elevated -1", parent) && !ReadElevationParent(L"--elevated 12x", parent),
        "invalid restart parent is rejected");
    const std::wstring self = L"--elevated " + std::to_wstring(GetCurrentProcessId());
    Check(!ReadElevationParent(self.c_str(), parent), "restart cannot wait on itself");
    testElevation = 0;
    Check(!WaitForElevationParent(GetCurrentProcessId()), "restart marker cannot bypass elevation verification");
    testElevation = -1;
}

void CheckElevation()
{
    testElevation = 1;
    Check(EnsureElevated(false) == ElevationResult::Continue && testElevationLaunches == 0,
        "elevated process starts without another launch");
    testElevation = 0;
    Check(EnsureElevated(false) == ElevationResult::Restarted && testElevationLaunches == 1 &&
        testElevationCommand, "unelevated startup requests runas of own executable then exits");
    testElevationLaunchError = ERROR_CANCELLED;
    const unsigned messages = testErrorMessages;
    Check(EnsureElevated(false) == ElevationResult::Continue && testErrorMessages == messages + 1,
        "UAC cancellation explains normal-privilege operation and continues");
    testElevationLaunchError = ERROR_ACCESS_DENIED;
    Check(EnsureElevated(false) == ElevationResult::Failed, "failed elevation does not start resident app");
    const unsigned launches = testElevationLaunches;
    Check(EnsureElevated(true) == ElevationResult::Failed && testElevationLaunches == launches,
        "relaunch marker cannot bypass token verification or cause a restart loop");
    testElevation = -2;
    Check(EnsureElevated(false) == ElevationResult::Failed && testElevationLaunches == launches,
        "token query failure cannot be treated as elevated");
    testElevation = -1;
}

void CheckExecutableVersion()
{
    std::wstring path;
    Check(GetIniPath(path), "version test locates executable folder");
    path.resize(path.find_last_of(L"\\/")); // tests folder
    path.resize(path.find_last_of(L"\\/") + 1); // configuration folder
    path += L"switch_ime.exe";
    DWORD unused = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
    Check(size != 0, "product executable has VERSIONINFO");
    std::vector<BYTE> data(size);
    Check(GetFileVersionInfoW(path.c_str(), 0, size, data.data()) != FALSE, "product version resource readable");
    VS_FIXEDFILEINFO* version = nullptr;
    UINT length = 0;
    Check(VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&version), &length) &&
        length >= sizeof(*version) && version->dwSignature == VS_FFI_SIGNATURE,
        "valid fixed version information");
    const DWORD high = MAKELONG(SWITCH_IME_VERSION_MINOR, SWITCH_IME_VERSION_MAJOR);
    const DWORD low = MAKELONG(0, SWITCH_IME_VERSION_PATCH);
    Check(version->dwFileVersionMS == high && version->dwFileVersionLS == low &&
        version->dwProductVersionMS == high && version->dwProductVersionLS == low,
        "EXE numeric file/product versions match build version");
    const std::wstring expected = std::to_wstring(SWITCH_IME_VERSION_MAJOR) + L"." +
        std::to_wstring(SWITCH_IME_VERSION_MINOR) + L"." + std::to_wstring(SWITCH_IME_VERSION_PATCH);
    for (const wchar_t* field : {L"\\StringFileInfo\\040904B0\\FileVersion",
        L"\\StringFileInfo\\040904B0\\ProductVersion"})
    {
        wchar_t* value = nullptr;
        Check(VerQueryValueW(data.data(), field, reinterpret_cast<void**>(&value), &length) &&
            value && expected == value, "EXE string version matches build version");
    }
}

void CheckFixedTray()
{
    for (const int size : {16, 24, 32, 48})
    {
        const auto icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDI_TRAY), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
        Check(icon != nullptr, "embedded fixed tray icon loads at requested size");
        DestroyIcon(icon);
    }
    WNDCLASSW cls{};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"switch_ime.TestIdle";
    Check(RegisterClassW(&cls) != 0, "idle test window class registered");
    App app;
    app.window = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, cls.hInstance, &app);
    Check(app.window != nullptr, "idle test window created");
    testImeQueries = 0;
    for (int i = 0; i < 10; ++i) SendMessageW(app.window, WM_TIMER, 1, 0);
    Check(testImeQueries == 0, "timer messages do not query IME state");
    Check(ReadIme({}) == ImeState::Unknown && testImeQueries == 1,
        "explicit IME request still queries state and preserves acquisition failure");
    app.keys.runAsAdmin = true;
    testElevation = 0;
    testElevationLaunchError = ERROR_CANCELLED;
    const unsigned launches = testElevationLaunches;
    SendMessageW(app.window, kElevationMessage, 0, 0);
    Check(IsWindow(app.window) && app.keys.runAsAdmin && testElevationLaunches == launches + 1,
        "cancelled settings elevation preserves resident window and configured option");
    app.keys.runAsAdmin = false;
    SendMessageW(app.window, kElevationMessage, 0, 0);
    Check(IsWindow(app.window) && testElevationLaunches == launches + 1,
        "turning administrator option off prevents another elevation request");
    testElevation = -1;
    testElevationLaunchError = ERROR_SUCCESS;
    DestroyWindow(app.window);
    UnregisterClassW(cls.lpszClassName, cls.hInstance);
}

void CheckFilter()
{
    Keys keys{VK_F9, VK_F10};
    KeyFilter filter;
    auto result = filter.Process('A', true, false, false, keys);
    Check(!result.consume && !result.request, "unregistered key passes");
    Check(!filter.Process('A', false, false, false, keys).consume, "unregistered keyup passes");
    result = filter.Process(VK_F9, true, false, false, keys);
    Check(result.consume && result.request && result.turnOn, "ON keydown consumed, requests ON");
    result = filter.Process(VK_F9, true, false, false, keys);
    Check(result.consume && !result.request, "repeat consumed without another request");
    result = filter.Process(VK_F9, false, true, false, keys);
    Check(!result.consume && !result.request && filter.down[VK_F9], "injection does not reset physical held key");
    result = filter.Process(VK_F9, false, false, false, keys);
    Check(result.consume && !result.request, "physical registered keyup consumed");
    Check(filter.Process(VK_F9, true, false, false, keys).request, "next physical press can request again");
    keys.on = VK_F11;
    Check(filter.Process(VK_F9, false, false, true, keys).consume, "old consumed pair finishes across settings change");
    result = filter.Process(VK_F10, true, true, false, keys);
    Check(!result.consume && !result.request, "injected registered key passes untouched");
    result = filter.Process(VK_F10, true, false, true, keys);
    Check(!result.consume && !result.request, "settings pause passes keys");
    result = filter.Process(VK_F10, true, false, false, keys);
    Check(!result.consume && !result.request, "held key during pause does not activate after resume");
    Check(!filter.Process(VK_F10, false, false, false, keys).consume, "paused key pair stays untouched");
    result = filter.Process(VK_F10, true, false, false, keys);
    Check(result.consume && result.request && !result.turnOn, "OFF requests once after release");
    result = filter.Process(VK_KANA, true, false, true, keys, true);
    Check(result.consume && !result.request, "settings captures native VK without switching IME");
    Check(filter.Process(VK_KANA, false, false, true, keys).consume,
        "captured keyup remains consumed after edit loses focus");
    filter.down[0xF2] = true;
    result = filter.Process(0xF2, true, false, true, keys, true);
    Check(result.consume && !result.request, "Hiragana registration ignores stale held state");
    Check(filter.Process(0xF2, true, false, true, keys, true).consume,
        "Hiragana repeat can update registration without an IME request");
    Check(filter.Process(0xF2, false, false, true, keys).consume,
        "Hiragana registration releases cleanly");

    KeyFilter physical;
    const Keys japanese{0xF2, VK_CONVERT};
    const UINT hiragana = KeyFilter::Index(0xF2, 0x70, false);
    physical.down[0xF2] = true; // Logical mode state must not block a physical press.
    result = physical.Process(0xF2, true, false, false, japanese, false, 0x70);
    Check(result.consume && result.request && result.turnOn,
        "physical Hiragana press is independent of logical VK state");
    Check(!physical.Process(0xF2, true, false, false, japanese, false, 0x70).request,
        "physical Hiragana repeat does not switch again");
    result = physical.Process(VK_KANA, false, true, false, japanese, false, 0x70);
    Check(!result.consume && physical.down[hiragana], "injected release cannot end a physical press");
    result = physical.Process(VK_KANA, false, false, false, japanese, false, 0x70);
    Check(result.consume && !physical.down[hiragana],
        "Hiragana release with a different VK consumes and clears the same physical key");
    Check(physical.Process(0xF2, true, false, false, japanese, false, 0x70).request,
        "next Hiragana press works after different-VK release");
    Check(KeyFilter::Index(VK_RETURN, 0x1C, false) != KeyFilter::Index(VK_RETURN, 0x1C, true),
        "extended and ordinary keys with the same scan code remain distinct");

    Check(NeedsSwitch(true, ImeState::Alphanumeric) && !NeedsSwitch(true, ImeState::Japanese) &&
        !NeedsSwitch(true, ImeState::Unknown) && NeedsSwitch(false, ImeState::Japanese) &&
        !NeedsSwitch(false, ImeState::Alphanumeric) && !NeedsSwitch(false, ImeState::Unknown),
        "exact ON/OFF state table; unknown never switches");
    INPUT inputs[2]{};
    for (const bool on : {true, false})
    {
        MakeInputs(on, inputs);
        const WORD vk = on ? 0x16 : 0x1A;
        Check(inputs[0].type == INPUT_KEYBOARD && inputs[1].type == INPUT_KEYBOARD &&
            inputs[0].ki.wVk == vk && inputs[1].ki.wVk == vk &&
            inputs[0].ki.dwFlags == 0 && inputs[1].ki.dwFlags == KEYEVENTF_KEYUP,
            "switch input contains exact matching KEYDOWN/KEYUP");
    }
    App app;
    app.paused = true;
    app.monitor.requests.push_back({true});
    app.Switch();
    Check(app.monitor.requests.empty(), "accepted request is processed independently of reception pause");
    app.paused = false;
    app.monitor.requests.push_back({true});
    app.Switch();
    Check(app.monitor.requests.empty(), "request with unavailable target is discarded");
}

LRESULT CALLBACK FakeIme(HWND window, UINT message, WPARAM command, LPARAM data)
{
    if (message == WM_IME_CONTROL)
    {
        if (testSwitchForeground)
        {
            testForeground = testSwitchForeground;
            SetFocus(testSwitchFocus);
            testSwitchForeground = testSwitchFocus = nullptr;
        }
        return command == kGetOpen ? (testJapaneseMode ? 1 : 0) : IME_CMODE_NATIVE;
    }
    return DefWindowProcW(window, message, command, data);
}

bool saveDialog = false;
int startupChoice = -1;
int adminChoice = -1;
bool expectSettingsFailure = false;
std::wstring screenshot;

void Snapshot(HWND dialog)
{
    RECT rect{};
    GetWindowRect(dialog, &rect);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    HDC dc = GetDC(dialog), memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    const auto old = SelectObject(memory, bitmap);
    PrintWindow(dialog, memory, 0);
    SelectObject(memory, old);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    std::vector<DWORD> pixels(static_cast<size_t>(width) * height);
    Check(GetDIBits(memory, bitmap, 0, static_cast<UINT>(height), pixels.data(), &info, DIB_RGB_COLORS) != 0,
        "settings dialog rendered");
    BITMAPFILEHEADER header{};
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size() * sizeof(DWORD));
    HANDLE file = CreateFileW(screenshot.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD written = 0;
    WriteFile(file, &header, sizeof(header), &written, nullptr);
    WriteFile(file, &info.bmiHeader, sizeof(info.bmiHeader), &written, nullptr);
    WriteFile(file, pixels.data(), static_cast<DWORD>(pixels.size() * sizeof(DWORD)), &written, nullptr);
    CloseHandle(file);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(dialog, dc);
}

INT_PTR CALLBACK TestDialog(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_INITDIALOG)
    {
        SettingsProc(dialog, message, wParam, lParam);
        SetTimer(dialog, 1, 50, nullptr);
        return TRUE;
    }
    if (message == WM_TIMER)
    {
        KillTimer(dialog, 1);
        testForeground = dialog; // Do not depend on Windows granting foreground activation to the test runner.
        Snapshot(dialog);
        auto* initial = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (!initial->startupKey)
            Check(!IsWindowEnabled(GetDlgItem(dialog, IDC_STARTUP)) &&
                IsDlgButtonChecked(dialog, IDC_STARTUP) == BST_UNCHECKED,
                "unavailable startup settings are unchecked and disabled without blocking key settings");
        const HWND on = GetDlgItem(dialog, IDC_ON_KEY), off = GetDlgItem(dialog, IDC_OFF_KEY);
        const LPARAM data = static_cast<LPARAM>(MapVirtualKeyW(VK_F9, MAPVK_VK_TO_VSC)) << 16;
        auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
        App captureApp;
        captureApp.keys = state->keys;
        captureApp.paused = !captureApp.keys.Valid();
        gSettingsMonitor = &captureApp.monitor; // Exercise the callback with a simulated hook.
        captureApp.Publish();
        PublishKeyboardSettings();
        captureApp.monitor.filter.down[0xF2] = true;
        testHookEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Check(testHookEvent && captureApp.monitor.Start(), "settings registration monitor starts");
        auto capture = [&](HWND edit, UINT vk)
        {
            SetFocus(edit);
            captureApp.Publish();
            ResetEvent(testHookEvent);
            InterlockedExchange(&testHookKey, static_cast<LONG>(vk));
            InterlockedExchange(&testHookCommand, 1);
            TestHookTimer(nullptr, 0, 0, 0);
            Check(InterlockedCompareExchange(&testHookResult, 0, 0) == 1,
                "hook captures settings keydown and keyup");
            MSG pending{};
            while (PeekMessageW(&pending, edit, kCaptureKey, kCaptureKey, PM_REMOVE))
                DispatchMessageW(&pending);
        };
        capture(on, VK_KANA);
        Check(state->tooltip && SendMessageW(state->tooltip, TTM_GETTOOLCOUNT, 0, 0) == 2,
            "both checkbox tooltips are attached");
        for (const int id : {IDC_ADMIN, IDC_STARTUP})
        {
            wchar_t helpText[256]{};
            TOOLINFOW help{};
            help.cbSize = TTTOOLINFOW_V2_SIZE;
            help.hwnd = dialog;
            help.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(dialog, id));
            help.lpszText = helpText;
            SendMessageW(state->tooltip, TTM_GETTEXTW, 256, reinterpret_cast<LPARAM>(&help));
            Check(lstrcmpW(helpText, id == IDC_ADMIN ? kAdminHelp : kStartupHelp) == 0,
                "checkbox tooltip contains its explanation");
        }
        Check((IsDlgButtonChecked(dialog, IDC_ADMIN) == BST_CHECKED) == state->keys.runAsAdmin,
            "administrator checkbox reflects saved setting");
        if (adminChoice >= 0) CheckDlgButton(dialog, IDC_ADMIN, adminChoice ? BST_CHECKED : BST_UNCHECKED);
        if (state->startupKey)
            Check((IsDlgButtonChecked(dialog, IDC_STARTUP) == BST_CHECKED) == state->originalStartup.exists,
                "startup checkbox reflects registry entry");
        if (startupChoice >= 0)
            CheckDlgButton(dialog, IDC_STARTUP, startupChoice ? BST_CHECKED : BST_UNCHECKED);
        Check(state->keys.on == VK_KANA, "native IME key registered before translation");
        capture(on, 0xF2);
        Check(state->keys.on == 0xF2, "hook registers actual Hiragana VK even if initially held");
        capture(on, VK_F9);
        capture(off, VK_F10);
        Check(state->keys.on == VK_F9 && state->keys.off == VK_F10, "dialog captures actual VK values");
        wchar_t name[128]{};
        GetWindowTextW(on, name, 128);
        Check(*name != 0, "dialog displays key name");
        SendMessageW(on, WM_CHAR, 'x', 0);
        wchar_t after[128]{};
        GetWindowTextW(on, after, 128);
        Check(lstrcmpW(name, after) == 0, "text typing does not overwrite registration");
        SendMessageW(on, WM_KEYDOWN, VK_PROCESSKEY, data);
        Check(state->keys.on == VK_F9, "IME-translated VK_PROCESSKEY cannot overwrite captured VK");
        SetFocus(GetDlgItem(dialog, IDC_ADMIN));
        auto press = [&](UINT vk, bool consumed)
        {
            ResetEvent(testHookEvent);
            InterlockedExchange(&testHookKey, static_cast<LONG>(vk));
            InterlockedExchange(&testHookCommand, 1);
            TestHookTimer(nullptr, 0, 0, 0);
            Check(InterlockedCompareExchange(&testHookResult, 0, 0) == (consumed ? 1 : 0),
                "outside registration fields only committed keys remain active; initial setup stays inactive");
        };
        const auto committed = captureApp.monitor.Snapshot();
        Check(committed.keys.on == captureApp.keys.on && committed.keys.off == captureApp.keys.off,
            "editing draft keys does not change committed hook settings");
        if (captureApp.keys.Valid()) press(captureApp.keys.on, true);
        press(VK_F9, captureApp.keys.Valid() &&
            (captureApp.keys.on == VK_F9 || captureApp.keys.off == VK_F9));
        captureApp.monitor.Stop();
        gSettingsMonitor = nullptr;
        testForeground = nullptr;
        CloseHandle(testHookEvent);
        testHookEvent = nullptr;
        const unsigned messages = testErrorMessages;
        const INT_PTR result = SettingsProc(dialog, WM_COMMAND, saveDialog ? IDOK : IDCANCEL, 0);
        if (expectSettingsFailure)
        {
            Check(testErrorMessages == messages + 1 && IsWindow(dialog) &&
                gSettingsDialog == dialog &&
                GetWindowLongPtrW(dialog, DWLP_USER) == reinterpret_cast<LONG_PTR>(state),
                "failed settings save reports error and leaves the same dialog open");
            const auto unchanged = captureApp.monitor.Snapshot();
            Check(unchanged.keys.on == captureApp.keys.on && unchanged.keys.off == captureApp.keys.off &&
                unchanged.keys.runAsAdmin == captureApp.keys.runAsAdmin,
                "failed settings save preserves committed keyboard settings");
            return SettingsProc(dialog, WM_COMMAND, IDCANCEL, 0);
        }
        return result;
    }
    return SettingsProc(dialog, message, wParam, lParam);
}
}

namespace
{
template<class Predicate> bool PumpUntil(Predicate done)
{
    const ULONGLONG deadline = GetTickCount64() + 4000;
    while (!done() && GetTickCount64() < deadline)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message != WM_QUIT) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    return done();
}

void CheckIntentTargets()
{
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW cls{};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = instance;
    cls.lpszClassName = L"switch_ime.TestIntent";
    RegisterClassW(&cls);
    WNDCLASSW imeClass{};
    imeClass.lpfnWndProc = FakeIme;
    imeClass.hInstance = instance;
    imeClass.lpszClassName = L"switch_ime.TestIntentIme";
    RegisterClassW(&imeClass);
    App app;
    app.window = CreateWindowExW(0, cls.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
        0, 0, 200, 100, nullptr, nullptr, instance, &app);
    const HWND other = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW,
        0, 0, 200, 100, nullptr, nullptr, instance, nullptr);
    const HWND first = CreateWindowExW(0, L"EDIT", L"", WS_CHILD,
        0, 0, 80, 20, app.window, nullptr, instance, nullptr);
    const HWND second = CreateWindowExW(0, L"EDIT", L"", WS_CHILD,
        0, 0, 80, 20, other, nullptr, instance, nullptr);
    const HWND third = CreateWindowExW(0, L"EDIT", L"", WS_CHILD,
        0, 20, 80, 20, other, nullptr, instance, nullptr);
    testImeWindow = CreateWindowExW(0, imeClass.lpszClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, instance, nullptr);
    testLayout = reinterpret_cast<HKL>(static_cast<ULONG_PTR>(0x0411));
    app.keys = {VK_F13, VK_F14};
    app.Publish();
    auto enqueue = [&](bool on)
    {
        const unsigned reads = testForegroundReads;
        app.monitor.Enqueue(on, app.monitor.Snapshot());
        Check(testForegroundReads == reads, "acceptance stores ON/OFF intent without querying or binding an input target");
    };
    auto expectSwitch = [&](WORD vk, HWND focus)
    {
        const unsigned sent = testSendInputs;
        app.Switch();
        Check(testSendInputs == sent + 1 && testLastInput == vk && testLastImeFocus == focus,
            "accepted intent uses current input target and retains requested ON/OFF direction");
    };
    testForeground = app.window;
    SetFocus(first);
    enqueue(true);
    testForeground = other;
    SetFocus(second);
    expectSwitch(VK_IME_ON, second);
    enqueue(true);
    SetFocus(third);
    expectSwitch(VK_IME_ON, third);

    testForeground = app.window;
    SetFocus(first);
    enqueue(true);
    enqueue(false);
    testSwitchForeground = other;
    testSwitchFocus = second;
    unsigned sent = testSendInputs;
    app.Switch();
    Check(testSendInputs == sent && app.monitor.requests.size() == 2 &&
        app.monitor.requests.front().turnOn && !app.monitor.requests.back().turnOn,
        "target change during IME query keeps ON pending instead of using stale state or cancelling intent");
    expectSwitch(VK_IME_ON, second);
    testJapaneseMode = true;
    expectSwitch(VK_IME_OFF, second);
    testJapaneseMode = false;

    enqueue(true);
    testSwitchForeground = other;
    testSwitchFocus = third;
    sent = testSendInputs;
    app.Switch();
    Check(testSendInputs == sent && app.monitor.requests.size() == 1,
        "focus change within one window also schedules a fresh state query");
    expectSwitch(VK_IME_ON, third);

    testJapaneseMode = true;
    enqueue(false);
    testForeground = app.window;
    SetFocus(first);
    expectSwitch(VK_IME_OFF, first);
    // Pausing reception later must not cancel intent that was already accepted.
    testJapaneseMode = false;
    enqueue(true);
    app.paused = true;
    expectSwitch(VK_IME_ON, first);
    app.paused = false;
    testJapaneseMode = true;
    enqueue(true);
    sent = testSendInputs;
    app.Switch();
    Check(testSendInputs == sent && app.monitor.requests.empty(), "already-requested mode needs no input");
    testJapaneseMode = false;
    const HWND ime = testImeWindow;
    testImeWindow = nullptr;
    enqueue(true);
    app.Switch();
    Check(testSendInputs == sent && app.monitor.requests.empty(), "state acquisition failure sends no guessed input");
    testForeground = nullptr;
    testLayout = nullptr;
    DestroyWindow(ime);
    DestroyWindow(other);
    DestroyWindow(app.window);
    UnregisterClassW(imeClass.lpszClassName, instance);
    UnregisterClassW(cls.lpszClassName, instance);
}

unsigned testAppliedWhileOpen = 0;
LRESULT CALLBACK TestSettingsMainProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == kApplySettingsMessage)
    {
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (app && IsWindow(app->settings.dialog.load())) ++testAppliedWhileOpen;
    }
    return WindowProc(window, message, wParam, lParam);
}

void CheckSettingsThread(const std::wstring& folder)
{
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW cls{};
    cls.lpfnWndProc = TestSettingsMainProc;
    cls.hInstance = instance;
    cls.lpszClassName = L"switch_ime.TestSettingsThread";
    Check(RegisterClassW(&cls) != 0, "settings-thread test main window class registered");
    WNDCLASSW imeClass{};
    imeClass.lpfnWndProc = FakeIme;
    imeClass.hInstance = instance;
    imeClass.lpszClassName = L"switch_ime.TestThreadIme";
    RegisterClassW(&imeClass);
    testImeWindow = CreateWindowExW(0, imeClass.lpszClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, instance, nullptr);
    const std::wstring registryPath = L"Software\\namulab\\switch_ime.ThreadTest." +
        std::to_wstring(GetCurrentProcessId());
    Check(RegCreateKeyExW(HKEY_CURRENT_USER, registryPath.c_str(), 0, nullptr, REG_OPTION_VOLATILE,
        KEY_ALL_ACCESS, nullptr, &testStartupOverride, nullptr) == ERROR_SUCCESS,
        "settings worker uses isolated startup registry key");
    testIoEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    testIoRelease = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    testHookEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    testElevation = 0;
    {
        App app;
        app.instance = instance;
        app.ini = folder + L"\\thread-settings.ini";
        app.keys = {VK_F13, VK_F14};
        Check(SaveKeys(app.ini, app.keys), "thread-test initial INI saved");
        app.window = CreateWindowExW(0, cls.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
            0, 0, 200, 100, nullptr, nullptr, instance, &app);
        const HWND targetEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD,
            0, 0, 80, 20, app.window, nullptr, instance, nullptr);
        app.Publish();
        Check(app.monitor.Start(), "thread-test main hook starts");
        auto press = [&](UINT vk, bool consumed)
        {
            InterlockedExchange(&testHookKey, static_cast<LONG>(vk));
            InterlockedExchange(&testHookCommand, 1);
            TestHookTimer(nullptr, 0, 0, 0);
            Check(InterlockedCompareExchange(&testHookResult, 0, 0) == (consumed ? 1 : 0),
                "main hook handles key with current committed configuration");
        };
        auto switchWhileBlocked = [&]()
        {
            testForeground = app.window;
            SetFocus(targetEdit);
            const unsigned sent = testSendInputs;
            press(VK_F13, true);
            Check(PumpUntil([&]() { return testSendInputs == sent + 1; }) &&
                WaitForSingleObject(testIoRelease, 0) == WAIT_TIMEOUT &&
                testSendInputThread == GetCurrentThreadId(),
                "main thread reads IME and sends switch while settings I/O is still blocked");
        };
        testIoStage = 1;
        Check(app.Settings() && WaitForSingleObject(testIoEntered, 2000) == WAIT_OBJECT_0,
            "settings registry preparation runs independently on worker");
        HANDLE firstThread = app.settings.thread;
        app.Settings();
        Check(app.settings.thread == firstThread, "reopening during initialization does not create another settings worker");
        switchWhileBlocked();
        SetEvent(testIoRelease);
        Check(PumpUntil([&]() { return app.monitor.Snapshot().edits[0] != nullptr; }),
            "settings dialog publishes registration controls");
        const HWND view = app.settings.dialog.load();
        const DWORD settingsThread = GetWindowThreadProcessId(view, nullptr);
        Check(settingsThread != GetCurrentThreadId() && IsWindowEnabled(app.window),
            "settings dialog runs on another thread and leaves main window enabled");
        testMenuCommand = kSettingsCommand;
        const unsigned menus = testMenuCalls;
        app.Menu();
        app.Menu();
        Check(testMenuCalls == menus + 2 && testMenuOwner == app.window &&
            app.settings.thread == firstThread, "tray settings can be selected repeatedly without nesting dialogs");
        testMenuCommand = 0;
        auto focus = [&](HWND control)
        {
            PostMessageW(view, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(control), TRUE);
            Check(PumpUntil([&]()
            {
                GUITHREADINFO info{};
                info.cbSize = sizeof(info);
                return GetGUIThreadInfo(settingsThread, &info) && info.hwndFocus == control;
            }), "settings worker moves focus to requested control");
        };
        auto registerKey = [&](int id, UINT vk)
        {
            const HWND edit = GetDlgItem(view, id);
            focus(edit);
            testForeground = view;
            const unsigned sent = testSendInputs;
            press(vk, true);
            const auto expected = KeyName(vk);
            Check(PumpUntil([&]()
            {
                wchar_t name[128]{};
                GetWindowTextW(edit, name, 128);
                return expected == name;
            }) && testSendInputs == sent, "registration focus captures key without IME switching");
        };
        registerKey(IDC_ON_KEY, VK_F15);
        registerKey(IDC_OFF_KEY, VK_F16);
        focus(GetDlgItem(view, IDOK));
        ResetEvent(testIoEntered);
        ResetEvent(testIoRelease);
        testIoStage = 2;
        PostMessageW(view, WM_COMMAND, IDOK, 0);
        Check(WaitForSingleObject(testIoEntered, 2000) == WAIT_OBJECT_0,
            "INI flush runs on settings worker and can be held without blocking main thread");
        Check(app.keys.on == VK_F13 && IsWindow(view), "uncommitted save keeps dialog and old keys");
        switchWhileBlocked();
        press(VK_F15, false);
        testForeground = app.window;
        SetFocus(targetEdit);
        const unsigned sentBeforeUpdate = testSendInputs;
        press(VK_F13, true);
        // Commit a new mapping before executing an already-accepted ON request.
        const Keys updated{VK_F15, VK_F16};
        SendMessageW(app.window, kApplySettingsMessage, 0, reinterpret_cast<LPARAM>(&updated));
        app.Switch();
        Check(testSendInputs == sentBeforeUpdate + 1 && testLastInput == VK_IME_ON,
            "accepted ON request survives mapping change and still executes as ON");
        const unsigned appliedWhileOpen = testAppliedWhileOpen;
        SetEvent(testIoRelease);
        Check(PumpUntil([&]() { return app.settings.thread == nullptr &&
            testSendInputs == sentBeforeUpdate + 1; }) &&
            app.keys.on == VK_F15 && app.keys.off == VK_F16 &&
            testAppliedWhileOpen == appliedWhileOpen + 1,
            "saved mapping is applied on main thread before settings closes");
        press(VK_F13, false);
        press(VK_F15, true);
        Check(app.Settings() && PumpUntil([&]() { return app.settings.dialog.load() != nullptr; }),
            "settings can be reopened after save");
        PostMessageW(app.settings.dialog.load(), WM_CLOSE, 0, 0);
        Check(PumpUntil([&]() { return app.settings.thread == nullptr; }) && app.keys.on == VK_F15,
            "settings cancellation keeps committed keys");
        ResetEvent(testIoEntered);
        ResetEvent(testIoRelease);
        testIoStage = 1;
        Check(app.Settings() && WaitForSingleObject(testIoEntered, 2000) == WAIT_OBJECT_0,
            "exit test holds settings initialization");
        app.Exit();
        Check(IsWindow(app.window) && app.monitor.hook,
            "exit requests settings cancellation without blocking or destroying its shared state");
        SetEvent(testIoRelease);
        Check(PumpUntil([&]() { return !IsWindow(app.window); }) && !app.monitor.hook,
            "exit finishes after worker cleanup and removes main hook");
        DeleteFileW(app.ini.c_str());
    }
    {
        App first;
        first.instance = instance;
        first.ini = folder + L"\\first-thread-settings.ini";
        first.window = CreateWindowExW(0, cls.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
            0, 0, 200, 100, nullptr, nullptr, instance, &first);
        const HWND edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD,
            0, 0, 80, 20, first.window, nullptr, instance, nullptr);
        first.Publish();
        Check(first.monitor.Start() && first.Settings() && PumpUntil([&]()
            { return first.monitor.Snapshot().edits[0] != nullptr; }),
            "initial settings dialog starts on worker");
        testForeground = first.window;
        SetFocus(edit);
        const unsigned sent = testSendInputs;
        InterlockedExchange(&testHookKey, VK_F13);
        InterlockedExchange(&testHookCommand, 1);
        TestHookTimer(nullptr, 0, 0, 0);
        Check(first.paused && testHookResult == 0 && testSendInputs == sent,
            "initial setup never switches IME before valid settings are saved");
        PostMessageW(first.settings.dialog.load(), WM_CLOSE, 0, 0);
        Check(PumpUntil([&]() { return !IsWindow(first.window); }) &&
            GetFileAttributesW(first.ini.c_str()) == INVALID_FILE_ATTRIBUTES,
            "initial settings cancellation exits without creating INI or tray icon");
    }
    testForeground = nullptr;
    testElevation = -1;
    CloseHandle(testIoEntered);
    CloseHandle(testIoRelease);
    CloseHandle(testHookEvent);
    testIoEntered = testIoRelease = testHookEvent = nullptr;
    RegCloseKey(testStartupOverride);
    testStartupOverride = nullptr;
    RegDeleteKeyW(HKEY_CURRENT_USER, registryPath.c_str());
    DestroyWindow(testImeWindow);
    testImeWindow = nullptr;
    UnregisterClassW(imeClass.lpszClassName, instance);
    UnregisterClassW(cls.lpszClassName, instance);
}
}

int wmain(int argc, wchar_t** argv)
{
    Check(argc == 1 || argc == 2, "optional test output directory argument");
    std::wstring folder;
    if (argc == 2) folder = argv[1];
    else
    {
        Check(GetIniPath(folder), "test executable folder available");
        folder.resize(folder.find_last_of(L"\\/") + 1);
        folder += L"test-output";
    }
    Check(CreateDirectoryW(folder.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS,
        "test output directory available");
    const std::wstring ini = folder + L"\\設定.ini";
    screenshot = folder + L"\\settings.bmp";
    DeleteFileW(ini.c_str());
    CheckSingleInstance();
    CheckKeyboardMonitor();
    CheckElevation();
    CheckStartupUser();
    CheckExecutableVersion();
    CheckFilter();
    UINT vk = 0;
    Check(ParseVk(L"0x15", vk) && vk == 0x15 && ParseVk(L"28", vk) && vk == 28,
        "INI accepts hexadecimal and decimal VK");
    Check(!ParseVk(L"", vk) && !ParseVk(L"0x15oops", vk) && !ParseVk(L"-1", vk) &&
        !ParseVk(L"0x100", vk), "invalid VK values rejected");
    Check(!Keys{VK_F9, VK_F9}.Valid(), "duplicate ON/OFF keys rejected");
    Keys loaded{VK_F1, VK_F2};
    Check(!LoadKeys(ini, loaded) && loaded.on == VK_F1, "missing INI does not modify settings");

    DialogState first{{}, &ini};
    first.startupError = kStartupUserError;
    const unsigned startupMessages = testErrorMessages;
    Check(DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), nullptr,
        TestDialog, reinterpret_cast<LPARAM>(&first)) == IDCANCEL, "initial cancel closes dialog");
    Check(testErrorMessages == startupMessages + 1 && testMessageText == kStartupUserError &&
        !first.startupError, "session-user failure displays the line-broken recovery message once");
    Check(GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES, "initial cancel creates no INI");
    saveDialog = true;
    first.keys = {};
    Check(DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), nullptr,
        TestDialog, reinterpret_cast<LPARAM>(&first)) == IDOK, "initial OK saves configuration");
    Check(LoadKeys(ini, loaded) && loaded.on == VK_F9 && loaded.off == VK_F10, "Unicode INI path roundtrip");
    Check(SaveKeys(ini, {VK_F11, VK_F12}) && LoadKeys(ini, loaded) &&
        loaded.on == VK_F11 && loaded.off == VK_F12, "existing INI replaced with both keys");
    saveDialog = false;
    DialogState cancelled{loaded, &ini};
    Check(DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), nullptr,
        TestDialog, reinterpret_cast<LPARAM>(&cancelled)) == IDCANCEL &&
        LoadKeys(ini, loaded) && loaded.on == VK_F11 && loaded.off == VK_F12,
        "settings cancel preserves existing INI");
    Check(!SaveKeys(ini, {VK_F9, VK_F9}) && LoadKeys(ini, loaded) && loaded.on == VK_F11,
        "invalid save preserves existing configuration");
    Check(!loaded.runAsAdmin, "administrator option defaults to off");
    DialogState admin{loaded, &ini};
    adminChoice = 1;
    saveDialog = true;
    Check(DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), nullptr,
        TestDialog, reinterpret_cast<LPARAM>(&admin)) == IDOK && LoadKeys(ini, loaded) && loaded.runAsAdmin,
        "administrator checkbox OK saves enabled option");
    adminChoice = 0;
    saveDialog = false;
    Check(DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), nullptr,
        TestDialog, reinterpret_cast<LPARAM>(&admin)) == IDCANCEL && LoadKeys(ini, loaded) && loaded.runAsAdmin,
        "administrator checkbox cancel preserves enabled option");
    admin.keys = loaded;
    saveDialog = true;
    Check(DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), nullptr,
        TestDialog, reinterpret_cast<LPARAM>(&admin)) == IDOK && LoadKeys(ini, loaded) && !loaded.runAsAdmin,
        "administrator checkbox OK saves disabled option");
    adminChoice = -1;
    saveDialog = false;

    // A volatile, isolated key: these tests never touch the real Run key.
    const std::wstring testKey = L"Software\\namulab\\switch_ime.Tests." + std::to_wstring(GetCurrentProcessId());
    HKEY startupKey = nullptr;
    Check(RegCreateKeyExW(HKEY_CURRENT_USER, testKey.c_str(), 0, nullptr, REG_OPTION_VOLATILE,
        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &startupKey, nullptr) == ERROR_SUCCESS,
        "isolated volatile registry key available");
    const DWORD marker = 42;
    Check(RegSetValueExW(startupKey, L"other_app", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&marker), sizeof(marker)) == ERROR_SUCCESS, "unrelated entry seeded");
    DialogState startup{loaded, &ini};
    startup.startupKey = startupKey;
    StartupEntry entry;
    auto showStartup = [&]()
    {
        return DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), nullptr,
            TestDialog, reinterpret_cast<LPARAM>(&startup));
    };
    startupChoice = 1;
    Check(showStartup() == IDCANCEL && ReadStartup(startupKey, entry) && !entry.exists,
        "checking startup then cancelling leaves no registration");
    saveDialog = true;
    Check(showStartup() == IDOK && ReadStartup(startupKey, entry) && entry.exists && entry.type == REG_SZ,
        "checking startup then OK registers executable");
    std::wstring exe;
    StartupEntry expected;
    Check(GetExePath(exe) && MakeStartupEntry(exe, expected) && entry.data == expected.data,
        "startup command quotes exact executable path");
    Check(MakeStartupEntry(L"C:\\日本語 folder\\switch_ime.exe", entry), "Unicode and spaces accepted in startup path");
    Check(!MakeStartupEntry(std::wstring(259, L'x'), entry), "overlong Run command rejected");
    saveDialog = false;
    startupChoice = 0;
    Check(showStartup() == IDCANCEL && ReadStartup(startupKey, entry) && entry.data == expected.data,
        "unchecking startup then cancelling preserves registration");
    saveDialog = true;
    Keys beforeFailure{};
    Check(LoadKeys(ini, beforeFailure), "saved settings available before startup write failure");
    HKEY readOnlyStartup = nullptr;
    Check(RegOpenKeyExW(HKEY_CURRENT_USER, testKey.c_str(), 0, KEY_QUERY_VALUE,
        &readOnlyStartup) == ERROR_SUCCESS, "read-only startup handle available for write-failure test");
    startup.startupKey = readOnlyStartup;
    expectSettingsFailure = true;
    Check(showStartup() == IDCANCEL && ReadStartup(startupKey, entry) && entry.data == expected.data,
        "startup write failure leaves registration unchanged and permits cancel");
    Check(LoadKeys(ini, loaded) && loaded.on == beforeFailure.on && loaded.off == beforeFailure.off &&
        loaded.runAsAdmin == beforeFailure.runAsAdmin,
        "startup write failure does not save draft INI settings");
    RegCloseKey(readOnlyStartup);
    startup.startupKey = startupKey;
    expectSettingsFailure = false;
    const std::wstring badIni = folder + L"\\missing-folder\\settings.ini";
    startup.ini = &badIni;
    expectSettingsFailure = true;
    Check(showStartup() == IDCANCEL && ReadStartup(startupKey, entry) && entry.data == expected.data,
        "failed INI save restores original startup entry");
    expectSettingsFailure = false;
    startup.ini = &ini;
    Check(showStartup() == IDOK && ReadStartup(startupKey, entry) && !entry.exists,
        "unchecking startup then OK removes registration");
    DWORD untouched = 0, size = sizeof(untouched);
    Check(RegQueryValueExW(startupKey, L"other_app", nullptr, nullptr,
        reinterpret_cast<BYTE*>(&untouched), &size) == ERROR_SUCCESS && untouched == marker,
        "startup changes preserve other applications");
    RegCloseKey(startupKey);
    Check(RegDeleteKeyW(HKEY_CURRENT_USER, testKey.c_str()) == ERROR_SUCCESS, "isolated registry key cleaned up");

    WNDCLASSW cls{};
    cls.lpfnWndProc = FakeIme;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"switch_ime.TestIme";
    RegisterClassW(&cls);
    const HWND fake = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
    DWORD_PTR response = 99;
    Check(QueryIme(fake, kGetOpen, response) && response == 0, "delivered zero differs from send failure");
    DestroyWindow(fake);
    Check(!QueryIme(fake, kGetOpen, response), "destroyed IME window is acquisition failure");
    CheckIntentTargets();
    CheckSettingsThread(folder);
    CheckFixedTray();
    DeleteFileW(ini.c_str());
    std::puts("All tests passed. No real IME switching or global hook installed by tests.");
}

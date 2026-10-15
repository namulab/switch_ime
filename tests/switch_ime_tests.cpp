// Separate executable; these checks are not compiled into switch_ime.exe.
#include <windows.h>
#include <imm.h>
#include <shellapi.h>
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
HWND WINAPI TestImmGetDefaultIMEWnd(HWND)
{
    ++testImeQueries;
    return nullptr;
}
UINT testMenuCommand = 0;
HWND testMenuOwner = nullptr;
unsigned testMenuCalls = 0;
unsigned testErrorMessages = 0;
int WINAPI TestMessageBox(HWND, LPCWSTR, LPCWSTR, UINT)
{
    ++testErrorMessages;
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
#include "../switch_ime.cpp"
#undef TrackPopupMenu
#undef MessageBoxW
#undef ImmGetDefaultIMEWnd
#undef GetTokenInformation
#undef ShellExecuteExW
#include <cstdlib>
#include "../version.h"

namespace
{
void Check(bool condition, const char* description)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", description); std::exit(1); }
    std::printf("PASS: %s\n", description);
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
    app.requests.push_back({true, {}, 0});
    app.Switch();
    Check(app.requests.empty(), "paused pending request discarded");
    app.paused = false;
    app.generation = 1;
    app.requests.push_back({true, {}, 0});
    app.Switch();
    Check(app.requests.empty(), "request from before settings is discarded");
}

LRESULT CALLBACK FakeIme(HWND window, UINT message, WPARAM command, LPARAM data)
{
    if (message == WM_IME_CONTROL)
        return command == kGetOpen ? 0 : IME_CMODE_NATIVE;
    return DefWindowProcW(window, message, command, data);
}

bool saveDialog = false;
int startupChoice = -1;
int adminChoice = -1;
bool expectSaveFailure = false;
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
        Snapshot(dialog);
        const HWND on = GetDlgItem(dialog, IDC_ON_KEY), off = GetDlgItem(dialog, IDC_OFF_KEY);
        const LPARAM data = static_cast<LPARAM>(MapVirtualKeyW(VK_F9, MAPVK_VK_TO_VSC)) << 16;
        App captureApp;
        captureApp.paused = true;
        gApp = &captureApp; // Exercise the real callback without installing a global hook.
        testMenuCommand = kSettingsCommand;
        const unsigned calls = testMenuCalls;
        captureApp.Menu();
        captureApp.Menu();
        Check(testMenuCalls == calls + 2 && testMenuOwner == dialog &&
            gSettingsDialog == dialog && captureApp.paused,
            "tray settings menu remains usable repeatedly during modal settings without nesting dialogs");
        auto capture = [&](HWND edit, UINT vk)
        {
            SetFocus(edit);
            KBDLLHOOKSTRUCT key{};
            key.vkCode = vk;
            key.scanCode = 0x3A;
            Check(KeyboardProc(HC_ACTION, WM_KEYDOWN, reinterpret_cast<LPARAM>(&key)) == 1,
                "hook captures physical VK in settings edit");
            MSG pending{};
            while (PeekMessageW(&pending, edit, kCaptureKey, kCaptureKey, PM_REMOVE))
                DispatchMessageW(&pending);
            key.flags = LLKHF_UP;
            Check(KeyboardProc(HC_ACTION, WM_KEYUP, reinterpret_cast<LPARAM>(&key)) == 1,
                "hook consumes captured release without switching");
        };
        capture(on, VK_KANA);
        auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
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
        captureApp.filter.down[0xF2] = true;
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
        const unsigned messages = testErrorMessages;
        const INT_PTR result = SettingsProc(dialog, WM_COMMAND, saveDialog ? IDOK : IDCANCEL, 0);
        if (expectSaveFailure)
        {
            Check(testErrorMessages == messages + 1, "failed INI save reports error without closing dialog");
            return SettingsProc(dialog, WM_COMMAND, IDCANCEL, 0);
        }
        return result;
    }
    return SettingsProc(dialog, message, wParam, lParam);
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
    CheckElevation();
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
    Check(DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), nullptr,
        TestDialog, reinterpret_cast<LPARAM>(&first)) == IDCANCEL, "initial cancel closes dialog");
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
    const std::wstring badIni = folder + L"\\missing-folder\\settings.ini";
    startup.ini = &badIni;
    expectSaveFailure = true;
    Check(showStartup() == IDCANCEL && ReadStartup(startupKey, entry) && entry.data == expected.data,
        "failed INI save restores original startup entry");
    expectSaveFailure = false;
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
    CheckFixedTray();
    DeleteFileW(ini.c_str());
    std::puts("All tests passed. No real IME switching or global hook installed by tests.");
}

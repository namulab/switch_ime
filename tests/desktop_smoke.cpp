// Desktop integration checks only; no key events are injected by this test.
#include "../switch_ime.cpp"
#include <filesystem>

namespace
{
struct WindowSearch { DWORD process; HWND window = nullptr; };
BOOL CALLBACK FindWindowForProcess(HWND window, LPARAM context)
{
    auto& search = *reinterpret_cast<WindowSearch*>(context);
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    wchar_t name[128]{};
    GetClassNameW(window, name, 128);
    if (process == search.process && lstrcmpW(name, kClass) == 0) search.window = window;
    return TRUE;
}

bool HasTray(HWND window)
{
    NOTIFYICONIDENTIFIER id{};
    id.cbSize = sizeof(id);
    id.hWnd = window;
    id.uID = 1;
    RECT rect{};
    return SUCCEEDED(Shell_NotifyIconGetRect(&id, &rect));
}

bool WaitForTray(HWND window)
{
    const ULONGLONG deadline = GetTickCount64() + 3000;
    do
    {
        if (HasTray(window)) return true;
        Sleep(25);
    } while (GetTickCount64() < deadline);
    return false;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc > 2) { std::fputs("Usage: switch_ime_smoke.exe [path-to-switch_ime.exe]\n", stderr); return 1; }
    std::wstring ownPath;
    if (!GetIniPath(ownPath)) return 1;
    const auto testsFolder = std::filesystem::path(ownPath).parent_path();
    const auto source = argc == 2 ? std::filesystem::absolute(argv[1]) :
        testsFolder.parent_path() / L"switch_ime.exe";
    const auto folder = testsFolder / (L"desktop-smoke-" + std::to_wstring(GetCurrentProcessId()));
    if (!CreateDirectoryW(folder.c_str(), nullptr)) return 1;
    const auto exe = folder / L"switch_ime.exe", ini = folder / L"switch_ime.ini";
    const auto duplicateExe = folder / L"duplicate.exe";
    PROCESS_INFORMATION child{};
    PROCESS_INFORMATION duplicate{};
    bool passed = false;
    do
    {
        if (!CopyFileW(source.c_str(), exe.c_str(), TRUE) || !SaveKeys(ini.wstring(), {VK_F23, VK_F24})) break;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        std::wstring command = L"\"" + exe.wstring() + L"\"";
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
            nullptr, folder.c_str(), &startup, &child)) break;
        WindowSearch search{child.dwProcessId};
        const ULONGLONG deadline = GetTickCount64() + 3000;
        do
        {
            EnumWindows(FindWindowForProcess, reinterpret_cast<LPARAM>(&search));
            if (search.window && HasTray(search.window)) break;
            Sleep(25);
        } while (GetTickCount64() < deadline && WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT);
        if (!search.window || !HasTray(search.window) || IsWindowVisible(search.window)) break;
        std::puts("PASS: normal startup stays hidden and registers tray icon");
        if (!CopyFileW(source.c_str(), duplicateExe.c_str(), TRUE)) break;
        std::wstring duplicateCommand = L"\"" + duplicateExe.wstring() + L"\"";
        if (!CreateProcessW(duplicateExe.c_str(), duplicateCommand.data(), nullptr, nullptr, FALSE, 0,
            nullptr, folder.c_str(), &startup, &duplicate)) break;
        DWORD duplicateExit = 1;
        if (WaitForSingleObject(duplicate.hProcess, 3000) != WAIT_OBJECT_0 ||
            !GetExitCodeProcess(duplicate.hProcess, &duplicateExit) || duplicateExit != 0 ||
            WaitForSingleObject(child.hProcess, 0) != WAIT_TIMEOUT || !HasTray(search.window)) break;
        std::puts("PASS: duplicate executable exits while original remains resident");
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = search.window;
        icon.uID = 1;
        if (!Shell_NotifyIconW(NIM_DELETE, &icon) || HasTray(search.window)) break;
        const UINT recreated = RegisterWindowMessageW(L"TaskbarCreated");
        if (!recreated || !PostMessageW(search.window, recreated, 0, 0) || !WaitForTray(search.window)) break;
        std::puts("PASS: icon restored after deletion and TaskbarCreated (Explorer not restarted)");
        if (!PostMessageW(search.window, WM_CLOSE, 0, 0) ||
            WaitForSingleObject(child.hProcess, 3000) != WAIT_OBJECT_0) break;
        DWORD result = 1;
        if (!GetExitCodeProcess(child.hProcess, &result) || result != 0 || HasTray(search.window)) break;
        std::puts("PASS: clean exit and tray icon removal");
        passed = true;
    } while (false);
    if (child.hProcess)
    {
        if (WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT)
        {
            // Only terminate the isolated child created by this test.
            TerminateProcess(child.hProcess, 1);
            WaitForSingleObject(child.hProcess, 3000);
        }
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
    }
    if (duplicate.hProcess)
    {
        if (WaitForSingleObject(duplicate.hProcess, 0) == WAIT_TIMEOUT)
        {
            TerminateProcess(duplicate.hProcess, 1);
            WaitForSingleObject(duplicate.hProcess, 3000);
        }
        CloseHandle(duplicate.hThread);
        CloseHandle(duplicate.hProcess);
    }
    DeleteFileW(duplicateExe.c_str());
    DeleteFileW(exe.c_str());
    DeleteFileW(ini.c_str());
    RemoveDirectoryW(folder.c_str());
    if (!passed) std::fputs("FAIL: desktop integration check (run on the normal Windows desktop)\n", stderr);
    return passed ? 0 : 1;
}

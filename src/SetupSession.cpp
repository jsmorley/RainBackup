#include "SetupSession.h"

#include <tlhelp32.h>
#include <cwchar>

namespace rb {
namespace {
struct Handle {
    HANDLE value;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct Instance {
    DWORD id;
    HWND window = nullptr;
};

BOOL CALLBACK FindMainWindow(HWND window, LPARAM context) {
    auto& instance = *reinterpret_cast<Instance*>(context);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != instance.id) return TRUE;
    wchar_t name[32]{};
    if (!GetClassNameW(window, name, 32) || wcscmp(name, L"RainBackup") != 0) return TRUE;
    instance.window = window;
    return FALSE;
}
}

int CloseRainBackupForSetup() {
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot.value == INVALID_HANDLE_VALUE) return 1;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.value, &entry)) return 1;
    bool blocked = false;
    do {
        if (entry.th32ProcessID == GetCurrentProcessId() || _wcsicmp(entry.szExeFile, L"RainBackup.exe") != 0) continue;
        Handle process{OpenProcess(SYNCHRONIZE, FALSE, entry.th32ProcessID)};
        if (!process.value) {
            if (GetLastError() != ERROR_INVALID_PARAMETER) blocked = true;
            continue;
        }
        if (WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) continue;
        Instance instance{entry.th32ProcessID};
        EnumWindows(FindMainWindow, reinterpret_cast<LPARAM>(&instance));
        if (!instance.window) {
            // Scheduled runs have no window and must finish on their own.
            blocked = true;
            continue;
        }
        DWORD_PTR reply = 0;
        if (!SendMessageTimeoutW(instance.window, SetupCloseMessage, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &reply)) {
            // An idle instance may have exited while handling the message.
            blocked |= WaitForSingleObject(process.value, 0) != WAIT_OBJECT_0;
            continue;
        }
        // Older versions already guard WM_CLOSE while work is running.
        if (!reply && IsWindowEnabled(instance.window)) PostMessageW(instance.window, WM_CLOSE, 0, 0);
        if (reply == SetupCloseBusy || WaitForSingleObject(process.value, 2500) != WAIT_OBJECT_0) blocked = true;
    } while (Process32NextW(snapshot.value, &entry));
    return (blocked || GetLastError() != ERROR_NO_MORE_FILES) ? 1 : 0;
}
}

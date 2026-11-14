#include "RainmeterSession.h"

#include <tlhelp32.h>
#include <shellapi.h>
#include <map>

namespace rb {
namespace {
struct Instance {
    HANDLE process = nullptr;
    HWND window = nullptr;
    fs::path executable;
    ~Instance() { if (process) CloseHandle(process); }
};

BOOL CALLBACK FindWindowForProcess(HWND window, LPARAM context) {
    wchar_t name[64]{};
    if (!GetClassNameW(window, name, static_cast<int>(std::size(name))) ||
        wcscmp(name, L"DummyRainWClass") != 0) return TRUE;
    DWORD id = 0;
    GetWindowThreadProcessId(window, &id);
    auto& instances = *reinterpret_cast<std::map<DWORD, Instance>*>(context);
    auto found = instances.find(id);
    if (found != instances.end()) found->second.window = window;
    return TRUE;
}

std::map<DWORD, Instance> Running() {
    std::map<DWORD, Instance> result;
    DWORD currentSession = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &currentSession)) Fail(L"Could not identify the Windows session.");
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) Fail(L"Could not check for running Rainmeter instances.");
    struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{snapshot};
    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);
    if (!Process32FirstW(snapshot, &process)) Fail(L"Could not check for running Rainmeter instances.");
    do {
        if (_wcsicmp(process.szExeFile, L"Rainmeter.exe") != 0) continue;
        DWORD session = 0;
        if (ProcessIdToSessionId(process.th32ProcessID, &session) && session != currentSession) continue;
        auto& entry = result[process.th32ProcessID];
        entry.process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, process.th32ProcessID);
        if (!entry.process) Fail(L"Could not access a running Rainmeter instance. Close it before restoring.");
        wchar_t path[32768]{};
        DWORD size = static_cast<DWORD>(std::size(path));
        if (!QueryFullProcessImageNameW(entry.process, 0, path, &size)) Fail(L"Could not identify a running Rainmeter instance.");
        entry.executable = path;
    } while (Process32NextW(snapshot, &process));
    EnumWindows(FindWindowForProcess, reinterpret_cast<LPARAM>(&result));
    return result;
}
}

void RainmeterSession::Stop(const Progress& progress) {
    auto instances = Running();
    if (!instances.empty() && progress) progress(L"Closing Rainmeter…");
    for (auto& [id, instance] : instances) {
        if (WaitForSingleObject(instance.process, 0) == WAIT_OBJECT_0) continue;
        if (!instance.window || !PostMessageW(instance.window, WM_CLOSE, 0, 0)) {
            Fail(L"Could not close Rainmeter. Close it before restoring.");
        }
        if (WaitForSingleObject(instance.process, 10000) != WAIT_OBJECT_0) {
            Fail(L"Rainmeter did not exit in time. Close it before restoring.");
        }
        stopped_.push_back(instance.executable);
    }
    if (!Running().empty()) Fail(L"Rainmeter is still running. Close it before restoring.");
}

std::wstring RainmeterSession::Restart() {
    std::wstring failures;
    for (const auto& exe : stopped_) {
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr,
            exe.parent_path().c_str(), SW_SHOWNORMAL)) <= 32) {
            if (!failures.empty()) failures += L"\n";
            failures += exe.wstring();
        }
    }
    stopped_.clear();
    return failures.empty() ? failures : L"Could not restart Rainmeter. Start it manually:\n" + failures;
}

void RestoreWithRainmeterRestart(const OpenZip& archive, const RestorePlan& plan,
    const Progress& progress, const Transfer& transfer) {
    RainmeterSession session;
    std::exception_ptr error;
    try {
        if (transfer.stop.stop_requested()) throw Cancelled("Restore canceled before writing any files.");
        session.Stop(progress);
        Restore(archive, plan, transfer);
    } catch (...) { error = std::current_exception(); }
    auto restart = session.Restart();
    if (error) {
        if (restart.empty()) std::rethrow_exception(error);
        try { std::rethrow_exception(error); }
        catch (const std::exception& failure) { Fail(Utf16(failure.what()) + L"\n\n" + restart); }
    }
    if (!restart.empty()) Fail(restart);
}
}

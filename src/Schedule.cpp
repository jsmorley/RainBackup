#include "Schedule.h"
#include "Archive.h"
#include <shlobj.h>
#include <sddl.h>
#include <taskschd.h>
#include <wrl/client.h>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace rb {
namespace {
fs::path SchedulePath(const wchar_t* file) {
    return fs::path(KnownFolder(FOLDERID_LocalAppData)) / L"RainBackup" / file;
}

std::wstring TaskName() {
    auto profile = KnownFolder(FOLDERID_Profile);
    uint64_t hash = 14695981039346656037ULL;
    for (wchar_t c : profile) hash = (hash ^ c) * 1099511628211ULL;
    return L"RainBackup-" + std::to_wstring(hash);
}

using Microsoft::WRL::ComPtr;
struct BString {
    BSTR value;
    explicit BString(const wchar_t* text) : value(SysAllocString(text)) {
        if (!value) throw std::bad_alloc();
    }
    ~BString() { SysFreeString(value); }
    operator BSTR() const { return value; }
};

void TaskCheck(HRESULT result) {
    if (SUCCEEDED(result)) return;
    wchar_t code[16]{};
    swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(result));
    Fail(std::wstring(L"Windows Task Scheduler could not complete the request (") + code + L").");
}

ComPtr<ITaskFolder> TaskFolder() {
    ComPtr<ITaskService> service;
    TaskCheck(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service)));
    VARIANT empty{};
    TaskCheck(service->Connect(empty, empty, empty, empty));
    ComPtr<ITaskFolder> folder;
    TaskCheck(service->GetFolder(BString(L"\\"), &folder));
    return folder;
}

std::wstring UserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) Fail(L"Could not identify this Windows account.");
    DWORD length = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &length);
    std::vector<unsigned char> storage(length);
    BOOL read = GetTokenInformation(token, TokenUser, storage.data(), length, &length);
    CloseHandle(token);
    wchar_t* sid = nullptr;
    if (!read || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid)) {
        Fail(L"Could not identify this Windows account.");
    }
    std::wstring result(sid);
    LocalFree(sid);
    return result;
}

std::wstring Xml(std::wstring_view text) {
    std::wstring result;
    for (wchar_t ch : text) {
        switch (ch) {
        case L'&': result += L"&amp;"; break;
        case L'<': result += L"&lt;"; break;
        case L'>': result += L"&gt;"; break;
        default: result += ch;
        }
    }
    return result;
}

uint64_t Clock(const SYSTEMTIME& time) {
    FILETIME file{};
    if (!SystemTimeToFileTime(&time, &file)) Fail(L"Could not calculate the backup schedule.");
    return (uint64_t(file.dwHighDateTime) << 32) | file.dwLowDateTime;
}

uint64_t Due(const Schedule& schedule) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const auto current = Clock(now);
    unsigned days = schedule.weekly ? (now.wDayOfWeek + 6) % 7 : 0;
    now.wHour = static_cast<WORD>(std::stoi(schedule.time.substr(0, 2)));
    now.wMinute = static_cast<WORD>(std::stoi(schedule.time.substr(3, 2)));
    now.wSecond = now.wMilliseconds = 0;
    constexpr uint64_t day = 864000000000ULL;
    auto due = Clock(now) - days * day;
    if (due > current) due -= (schedule.weekly ? 7 : 1) * day;
    return due;
}

bool IsManaged(std::wstring_view name) {
    if (name.size() != 35 || !name.starts_with(L"RainBackup-") || !name.ends_with(L".zip")) return false;
    for (size_t i = 11; i < 31; ++i) {
        if (i == 19 || i == 26) {
            if (name[i] != L'-') return false;
        } else if (name[i] < L'0' || name[i] > L'9') return false;
    }
    return true;
}

class LogLock {
public:
    LogLock() {
        handle = CreateMutexW(nullptr, FALSE, L"Local\\RainBackup-OperationLog");
        if (!handle) Fail(L"Could not open the operation log.");
        auto wait = WaitForSingleObject(handle, 5000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            CloseHandle(handle);
            Fail(L"The operation log is busy.");
        }
    }
    ~LogLock() { ReleaseMutex(handle); CloseHandle(handle); }
    LogLock(const LogLock&) = delete;
    LogLock& operator=(const LogLock&) = delete;
private:
    HANDLE handle;
};
}

void LogOperation(std::wstring_view operation, std::wstring_view result, std::wstring_view detail,
    const fs::path& archive) {
    std::error_code sizeError;
    const auto size = archive.empty() ? 0 : fs::file_size(archive, sizeError);
    LogLock lock;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    auto path = SchedulePath(L"operations.log");
    fs::create_directories(path.parent_path());
    wchar_t date[32]{};
    swprintf_s(date, L"%04u-%02u-%02u %02u:%02u:%02u",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    std::ofstream log(path, std::ios::binary | std::ios::app);
    log << Utf8(date) << "  |  " << Utf8(operation) << "  |  " << Utf8(result) << "\r\n";
    if (!detail.empty()) {
        auto text = Utf8(detail);
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line)) log << "    " << line << "\r\n";
    }
    if (!archive.empty() && !sizeError) {
        std::ostringstream formatted;
        constexpr uintmax_t KB = 1000;
        constexpr uintmax_t MB = KB * 1000;
        constexpr uintmax_t GB = MB * 1000;
        constexpr uintmax_t TB = GB * 1000;
        if (size >= KB) {
            const auto [unit, label] = size >= TB ? std::pair{TB, "TB"} :
                size >= GB ? std::pair{GB, "GB"} : size >= MB ? std::pair{MB, "MB"} : std::pair{KB, "kB"};
            formatted << std::fixed << std::setprecision(3) << static_cast<double>(size) / unit
                << ' ' << label << " (" << size << " bytes)";
        } else {
            formatted << size << " bytes";
        }
        log << "    ZIP size: " << formatted.str() << "\r\n";
    }
    log << "\r\n";
    log.close();
    if (!log) Fail(L"Could not write the operation log.");
}

std::wstring ReadOperationLog() {
    LogLock lock;
    auto path = SchedulePath(L"operations.log");
    std::ifstream log(path, std::ios::binary);
    if (!log && fs::exists(path)) Fail(L"Could not read the operation log.");
    return Utf16(std::string((std::istreambuf_iterator<char>(log)), {}));
}

std::wstring LastScheduledBackup() {
    LogLock lock;
    std::ifstream log(SchedulePath(L"operations.log"), std::ios::binary);
    if (!log) {
        std::ifstream previous(SchedulePath(L"scheduled.log"), std::ios::binary);
        std::string date, time, detail;
        if (previous >> date >> time && std::getline(previous, detail) && !detail.empty()) {
            return L"Last scheduled backup " + std::wstring(detail.starts_with(" Saved ") ? L"succeeded" : L"failed") +
                L" · " + Utf16(date + " " + time);
        }
        return {};
    }
    constexpr std::string_view marker = "  |  Scheduled backup  |  ";
    std::string line;
    std::wstring latest;
    while (std::getline(log, line)) {
        if (line.size() <= 19 || !std::string_view(line).substr(19).starts_with(marker)) continue;
        const auto result = std::string_view(line).substr(19 + marker.size());
        if (result == "Success\r" || result == "Failure\r") {
            latest = L"Last scheduled backup " + std::wstring(result == "Success\r" ? L"succeeded" : L"failed") +
                L" · " + Utf16(std::string_view(line).substr(0, 19));
        }
    }
    return latest;
}

void EmptyOperationLog() {
    LogLock lock;
    std::ofstream log(SchedulePath(L"operations.log"), std::ios::binary | std::ios::trunc);
    log.close();
    if (!log) Fail(L"Could not empty the operation log.");
}

Schedule LoadSchedule() {
    std::ifstream input(SchedulePath(L"schedule.txt"), std::ios::binary);
    Schedule schedule;
    if (input) {
        std::string time, folder;
        if (!(input >> schedule.copies >> schedule.weekly >> std::quoted(time) >> std::quoted(folder))) {
            Fail(L"Could not read the saved backup schedule.");
        }
        schedule.time = Utf16(time);
        schedule.folder = Utf16(folder);
    }
    return schedule;
}

void SaveSchedule(const Schedule& schedule) {
    CheckPath(schedule.folder);
    if (!fs::is_directory(schedule.folder)) Fail(L"Choose an existing backup folder.");
    if (schedule.copies < 1 || schedule.copies > 999) Fail(L"Enter a number of copies from 1 to 999.");
    auto time = schedule.time;
    auto digit = [](wchar_t c) { return c >= L'0' && c <= L'9'; };
    if (time.size() != 5 || time[2] != L':' || !digit(time[0]) || !digit(time[1]) ||
        !digit(time[3]) || !digit(time[4]) || time.substr(0, 2) > L"23" || time.substr(3, 2) > L"59") {
        Fail(L"Enter a 24-hour time such as 18:00.");
    }
    auto path = SchedulePath(L"schedule.txt");
    fs::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << schedule.copies << ' ' << schedule.weekly << ' ' << std::quoted(Utf8(schedule.time))
        << ' ' << std::quoted(Utf8(schedule.folder.native())) << '\n';
    output.close();
    if (!output) Fail(L"Could not save the backup schedule.");
    wchar_t executable[32768]{};
    const auto count = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (!count || count >= std::size(executable)) Fail(L"Could not locate RainBackup.");
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t date[32]{};
    swprintf_s(date, L"%04u-%02u-%02uT", now.wYear, now.wMonth, now.wDay);
    auto sid = UserSid();
    auto boundary = std::wstring(date) + schedule.time + L":00";
    auto interval = schedule.weekly
        ? L"<ScheduleByWeek><WeeksInterval>1</WeeksInterval><DaysOfWeek><Monday/></DaysOfWeek></ScheduleByWeek>"
        : L"<ScheduleByDay><DaysInterval>1</DaysInterval></ScheduleByDay>";
    // Both triggers invoke the same due-date check, so a logon and a queued run cannot create duplicate copies.
    auto task = L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
        L"<Triggers><CalendarTrigger><StartBoundary>" + boundary + L"</StartBoundary><Enabled>true</Enabled>" + interval +
        L"</CalendarTrigger><LogonTrigger><Enabled>true</Enabled><UserId>" + sid + L"</UserId></LogonTrigger></Triggers>"
        L"<Principals><Principal id=\"User\"><UserId>" + sid + L"</UserId><LogonType>InteractiveToken</LogonType>"
        L"<RunLevel>LeastPrivilege</RunLevel></Principal></Principals>"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><StartWhenAvailable>true</StartWhenAvailable></Settings>"
        L"<Actions Context=\"User\"><Exec><Command>" + Xml(executable) +
        L"</Command><Arguments>--scheduled</Arguments></Exec></Actions></Task>";
    auto taskFolder = TaskFolder();
    ComPtr<IRegisteredTask> registered;
    BString user(sid.c_str());
    VARIANT account{}, empty{};
    account.vt = VT_BSTR;
    account.bstrVal = user.value;
    TaskCheck(taskFolder->RegisterTask(BString(TaskName().c_str()), BString(task.c_str()),
        TASK_CREATE_OR_UPDATE, account, empty, TASK_LOGON_INTERACTIVE_TOKEN, empty, &registered));
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        Fail(L"Could not save the backup schedule settings.");
    }
}

void DeleteSchedule() {
    auto folder = TaskFolder();
    HRESULT result = folder->DeleteTask(BString(TaskName().c_str()), 0);
    if (result != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) TaskCheck(result);
    std::error_code error;
    fs::remove(SchedulePath(L"schedule.txt"), error);
    if (error) Fail(L"Could not remove the saved backup schedule.");
}

void ApplyIgnored(Tree& tree, const std::vector<fs::path>& ignored) {
    auto apply = [&](auto&& self, Node& node) -> void {
        if (!node.path.empty() && std::any_of(ignored.begin(), ignored.end(),
            [&](const auto& path) { return EqualPath(path, node.path); })) {
            node.Select(false);
            return;
        }
        for (auto& child : node.children) self(self, *child);
    };
    apply(apply, tree.root);
    tree.root.Count();
}

fs::path SaveManagedBackup(const Tree& tree, const Schedule& schedule, const Transfer& transfer) {
    if (schedule.copies < 1 || schedule.copies > 999) Fail(L"Invalid number of backup copies.");
    CheckPath(schedule.folder);
    if (!fs::is_directory(schedule.folder)) Fail(L"The backup folder is unavailable.");
    auto mutex = CreateMutexW(nullptr, FALSE, L"Local\\RainBackup-ManagedBackups");
    if (!mutex) Fail(L"Could not coordinate the scheduled backup.");
    struct Release {
        HANDLE mutex;
        bool owned = false;
        ~Release() { if (owned) ReleaseMutex(mutex); CloseHandle(mutex); }
    } lock{mutex};
    auto wait = WaitForSingleObject(mutex, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) Fail(L"A managed backup is already running.");
    lock.owned = true;

    SYSTEMTIME now{};
    GetSystemTime(&now);
    wchar_t filename[64]{};
    swprintf_s(filename, L"RainBackup-%04u%02u%02u-%02u%02u%02u-%04u.zip",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    auto zip = schedule.folder / filename;
    if (fs::exists(zip)) Fail(L"A backup with the same timestamp already exists.");
    WriteZip(tree, zip, transfer);

    std::vector<fs::path> backups;
    for (const auto& item : fs::directory_iterator(schedule.folder)) {
        if (!IsManaged(item.path().filename().native()) || IsReparse(item.path()) || !item.is_regular_file()) continue;
        try {
            auto archive = ReadZip(item.path());
            if (archive.originalLocations) backups.push_back(item.path());
        } catch (const std::exception&) {
        }
    }
    std::sort(backups.begin(), backups.end());
    size_t remaining = backups.size();
    for (const auto& path : backups) {
        if (remaining <= schedule.copies) break;
        if (EqualPath(path, zip)) continue;
        if (!DeleteFileW(path.c_str())) Fail(L"Backup saved, but could not remove an older copy:\n" + path.wstring());
        --remaining;
    }
    return zip;
}

int RunScheduled(bool catchUp) {
    fs::path zip;
    try {
        auto schedule = LoadSchedule();
        if (schedule.folder.empty()) return 0;
        auto due = Due(schedule);
        auto lastPath = SchedulePath(L"last-run.txt");
        if (catchUp) {
            std::ifstream previous(lastPath);
            uint64_t last = 0;
            std::string folder, time;
            bool weekly = false;
            if (previous >> last >> weekly >> std::quoted(time) >> std::quoted(folder) &&
                last >= due && weekly == schedule.weekly && time == Utf8(schedule.time) &&
                EqualPath(Utf16(folder), schedule.folder)) return 0;
        }
        auto tree = Build(LoadSources());
        ApplyIgnored(*tree, LoadIgnored());
        if (tree->incomplete) Fail(L"Backup stopped because a source item could not be read. Review Scan issues in RainBackup.");
        zip = SaveManagedBackup(*tree, schedule);
        std::ofstream last(lastPath, std::ios::trunc);
        last << due << ' ' << schedule.weekly << ' ' << std::quoted(Utf8(schedule.time))
            << ' ' << std::quoted(Utf8(schedule.folder.native())) << '\n';
        last.close();
        if (!last) Fail(L"Backup saved, but the scheduled run record could not be written.");
        LogOperation(L"Scheduled backup", L"Success", L"Saved " + zip.wstring() + (tree->warnings.empty() ? L"" :
            L"\nSkipped " + std::to_wstring(tree->warnings.size()) + L" linked item(s)."), zip);
        return 0;
    } catch (const std::exception& error) {
        try { LogOperation(L"Scheduled backup", L"Failure", Utf16(error.what()), zip); } catch (const std::exception&) {}
        return 1;
    }
}
}

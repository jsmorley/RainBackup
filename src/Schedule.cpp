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
fs::path SchedulePath(std::wstring_view file) {
    return fs::path(KnownFolder(FOLDERID_LocalAppData)) / L"RainBackup" / file;
}

bool ValidScheduleId(std::wstring_view id) {
    return id.size() == 32 && id.find_first_not_of(L"0123456789abcdefABCDEF") == id.npos;
}

std::wstring ScheduleSuffix(std::wstring_view id) {
    if (id.empty()) return {};
    if (!ValidScheduleId(id)) Fail(L"Invalid backup schedule ID.");
    return L"-" + std::wstring(id);
}

fs::path ScheduleFile(std::wstring_view id) {
    return SchedulePath(L"schedule" + ScheduleSuffix(id) + L".txt");
}

fs::path ProfileFile(std::wstring_view id) {
    return SchedulePath(L"profiles") / (L"backup" + ScheduleSuffix(id) + L".txt");
}

std::wstring TaskName(std::wstring_view id) {
    auto profile = KnownFolder(FOLDERID_Profile);
    uint64_t hash = 14695981039346656037ULL;
    for (wchar_t c : profile) hash = (hash ^ c) * 1099511628211ULL;
    return L"RainBackup-" + std::to_wstring(hash) + ScheduleSuffix(id);
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

bool IsManaged(std::wstring_view name, std::wstring_view suffix) {
    if (name.size() != 35 + suffix.size() || !name.starts_with(L"RainBackup-") ||
        !name.ends_with(L".zip") || name.substr(31, suffix.size()) != suffix) return false;
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

static Schedule LoadSchedule(std::wstring_view id) {
    std::ifstream input(ScheduleFile(id), std::ios::binary);
    Schedule schedule;
    schedule.id = id;
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

std::wstring NewScheduleId() {
    GUID guid{};
    wchar_t text[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, text, 40)) Fail(L"Could not create a schedule ID.");
    std::wstring id;
    for (wchar_t ch : std::wstring_view(text)) {
        if (ch != L'{' && ch != L'}' && ch != L'-') id += ch;
    }
    return id;
}

static std::vector<Schedule> LoadSchedules() {
    std::vector<Schedule> result;
    auto legacy = LoadSchedule({});
    if (!legacy.folder.empty()) result.push_back(std::move(legacy));
    auto folder = SchedulePath(L"");
    if (!fs::exists(folder)) return result;
    for (const auto& item : fs::directory_iterator(folder)) {
        const auto name = item.path().filename().wstring();
        if (name.size() != 45 || !name.starts_with(L"schedule-") || !name.ends_with(L".txt")) continue;
        const auto id = std::wstring_view(name).substr(9, 32);
        if (ValidScheduleId(id)) result.push_back(LoadSchedule(id));
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    return result;
}

static std::optional<BackupProfile> ReadProfile(std::wstring_view id) {
    std::ifstream input(ProfileFile(id), std::ios::binary);
    if (!input) {
        if (fs::exists(ProfileFile(id))) Fail(L"Could not read the backup profile.");
        return std::nullopt;
    }
    BackupProfile profile;
    profile.schedule.id = id;
    std::string version, name, folder, time, sources;
    size_t count = 0;
    if (!std::getline(input, version) || version != "RainBackup profile 1" ||
        !(input >> std::quoted(name) >> std::quoted(folder) >> profile.automatic >> profile.schedule.weekly
            >> std::quoted(time) >> profile.schedule.copies >> std::quoted(sources) >> count) ||
        name.empty() || count > 1000000) Fail(L"Invalid saved backup profile.");
    profile.name = Utf16(name);
    profile.schedule.folder = Utf16(folder);
    profile.schedule.time = Utf16(time);
    profile.sources = DecodeSources(sources);
    for (size_t i = 0; i < count; ++i) {
        std::string path;
        if (!(input >> std::quoted(path))) Fail(L"Invalid backup exclusions.");
        profile.excluded.emplace_back(Utf16(path));
    }
    return profile;
}

void SaveBackupProfile(const BackupProfile& profile) {
    const auto& schedule = profile.schedule;
    const auto path = ProfileFile(schedule.id);
    fs::create_directories(path.parent_path());
    const auto temporary = path.parent_path() / (NewScheduleId() + L".tmp");
    std::ofstream output(temporary, std::ios::binary);
    output << "RainBackup profile 1\n" << std::quoted(Utf8(profile.name)) << ' '
        << std::quoted(Utf8(schedule.folder.native())) << ' ' << profile.automatic << ' ' << schedule.weekly
        << ' ' << std::quoted(Utf8(schedule.time)) << ' ' << schedule.copies << '\n'
        << std::quoted(EncodeSources(profile.sources)) << '\n' << profile.excluded.size() << '\n';
    for (const auto& excluded : profile.excluded) output << std::quoted(Utf8(excluded.native())) << '\n';
    output.close();
    if (!output || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        Fail(L"Could not save the backup profile.");
    }
}

std::vector<BackupProfile> LoadBackupProfiles(const fs::path& defaultFolder) {
    const auto folder = ProfileFile({}).parent_path();
    const auto migrated = folder / L"imported";
    if (!fs::exists(migrated)) {
        const auto sources = fs::exists(SchedulePath(L"sources.txt")) ? LoadSources() : Scan();
        const auto excluded = LoadIgnored();
        constexpr auto defaultId = L"00000000000000000000000000000001";
        if (!ReadProfile(defaultId)) {
            BackupProfile manual{L"Rainmeter backup", {}, false, sources, excluded};
            manual.schedule.id = defaultId;
            manual.schedule.folder = defaultFolder;
            SaveBackupProfile(manual);
        }
        size_t number = 0;
        for (const auto& schedule : LoadSchedules()) {
            ++number;
            if (ReadProfile(schedule.id)) continue;
            auto name = std::wstring(L"Scheduled backup ") + std::to_wstring(number);
            SaveBackupProfile({std::move(name), schedule, true, sources, excluded});
        }
        std::ofstream marker(migrated);
        if (!marker) Fail(L"Could not finish importing existing backup settings.");
    }
    std::vector<BackupProfile> result;
    if (auto legacy = ReadProfile({})) result.push_back(std::move(*legacy));
    for (const auto& item : fs::directory_iterator(folder)) {
        const auto name = item.path().filename().wstring();
        if (name.size() != 43 || !name.starts_with(L"backup-") || !name.ends_with(L".txt")) continue;
        const auto id = std::wstring_view(name).substr(7, 32);
        if (ValidScheduleId(id)) {
            if (auto profile = ReadProfile(id)) result.push_back(std::move(*profile));
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return result;
}

void DeleteBackupProfile(std::wstring_view id) {
    std::error_code error;
    fs::remove(ProfileFile(id), error);
    if (error) Fail(L"Could not remove the backup profile.");
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
    auto path = ScheduleFile(schedule.id);
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
        L"</Command><Arguments>--scheduled" + (schedule.id.empty() ? L"" : L" " + schedule.id) +
        L"</Arguments></Exec></Actions></Task>";
    auto taskFolder = TaskFolder();
    ComPtr<IRegisteredTask> registered;
    BString user(sid.c_str());
    VARIANT account{}, empty{};
    account.vt = VT_BSTR;
    account.bstrVal = user.value;
    TaskCheck(taskFolder->RegisterTask(BString(TaskName(schedule.id).c_str()), BString(task.c_str()),
        TASK_CREATE_OR_UPDATE, account, empty, TASK_LOGON_INTERACTIVE_TOKEN, empty, &registered));
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        Fail(L"Could not save the backup schedule settings.");
    }
}

void DeleteSchedule(std::wstring_view id) {
    auto folder = TaskFolder();
    HRESULT result = folder->DeleteTask(BString(TaskName(id).c_str()), 0);
    if (result != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) TaskCheck(result);
    std::error_code error;
    fs::remove(ScheduleFile(id), error);
    if (error) Fail(L"Could not remove the saved backup schedule.");
}

void DeleteAllSchedules() {
    for (const auto& schedule : LoadSchedules()) DeleteSchedule(schedule.id);
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

bool IsProfileArchive(const fs::path& path, std::wstring_view id) {
    const auto name = path.filename().wstring();
    if (IsManaged(name, ScheduleSuffix(id))) return true;
    if (name.starts_with(L"RainBackup-") && name.ends_with(L".zip") &&
        ReadZipComment(path) == "RainBackup profile:" + Utf8(id)) return true;
    // Manual backups from before profiles belonged to the single default backup.
    if (id != L"00000000000000000000000000000001" || name.size() < 34 ||
        !name.starts_with(L"RainBackup-") || !name.ends_with(L".zip")) return false;
    constexpr std::wstring_view stamp = L"0000-00-00_00-00-00";
    const auto date = std::wstring_view(name).substr(11, stamp.size());
    for (size_t i = 0; i < stamp.size(); ++i) {
        if (stamp[i] == L'0' ? (date[i] < L'0' || date[i] > L'9') : date[i] != stamp[i]) return false;
    }
    auto suffix = std::wstring_view(name).substr(30, name.size() - 34);
    if (suffix.empty()) return true;
    return suffix.size() > 1 && suffix.front() == L'-' &&
        suffix.substr(1).find_first_not_of(L"0123456789") == suffix.npos;
}

fs::path SaveManagedBackup(const Tree& tree, const Schedule& schedule, const Transfer& transfer, bool retain,
    std::wstring_view name) {
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
    DWORD wait;
    do {
        if (transfer.stop.stop_requested()) throw Cancelled("Backup canceled.");
        wait = WaitForSingleObject(mutex, 250);
        if (wait == WAIT_TIMEOUT && transfer.report) transfer.report(L"Waiting for another backup to finish…", 0);
    } while (wait == WAIT_TIMEOUT);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) Fail(L"Could not coordinate the scheduled backup.");
    lock.owned = true;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t timestamp[32]{};
    swprintf_s(timestamp, L"%04u-%02u-%02u-%02u-%02u-%02u",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    std::wstring safeName(name);
    for (auto& ch : safeName) {
        if (ch < 32 || wcschr(L"<>:\"/\\|?*", ch)) ch = L'-';
    }
    const auto stem = L"RainBackup-" + safeName + L"-" + timestamp;
    auto zip = schedule.folder / (stem + L".zip");
    for (unsigned copy = 2; fs::exists(zip); ++copy) {
        zip = schedule.folder / (stem + L"-" + std::to_wstring(copy) + L".zip");
    }
    const auto suffix = ScheduleSuffix(schedule.id);
    WriteZip(tree, zip, transfer, ZipDisposition::CreateNew, "RainBackup profile:" + Utf8(schedule.id));
    if (!retain) return zip;

    std::vector<std::pair<fs::file_time_type, fs::path>> backups;
    for (const auto& item : fs::directory_iterator(schedule.folder)) {
        if (!EqualPath(item.path().extension(), L".zip") || IsReparse(item.path()) || !item.is_regular_file()) continue;
        if (!IsManaged(item.path().filename().native(), suffix) &&
            ReadZipComment(item.path()) != "RainBackup profile:" + Utf8(schedule.id)) continue;
        try {
            auto archive = ReadZip(item.path());
            if (archive.originalLocations) backups.emplace_back(item.last_write_time(), item.path());
        } catch (const std::exception&) {
        }
    }
    std::sort(backups.begin(), backups.end());
    size_t remaining = backups.size();
    for (const auto& [modified, path] : backups) {
        if (remaining <= schedule.copies) break;
        if (EqualPath(path, zip)) continue;
        if (!DeleteFileW(path.c_str())) Fail(L"Backup saved, but could not remove an older copy:\n" + path.wstring());
        --remaining;
    }
    return zip;
}

int RunScheduled(bool catchUp, std::wstring_view id) {
    fs::path zip;
    fs::path destination;
    std::wstring backupName;
    try {
        const auto profile = ReadProfile(id);
        if (profile && !profile->automatic) return 0;
        if (profile) backupName = profile->name + L"\n";
        auto schedule = profile ? profile->schedule : LoadSchedule(id);
        if (schedule.folder.empty()) return 0;
        destination = schedule.folder;
        auto due = Due(schedule);
        auto lastPath = SchedulePath(L"last-run" + ScheduleSuffix(id) + L".txt");
        if (catchUp) {
            std::ifstream previous(lastPath);
            uint64_t last = 0;
            std::string folder, time;
            bool weekly = false;
            if (previous >> last >> weekly >> std::quoted(time) >> std::quoted(folder) &&
                last >= due && weekly == schedule.weekly && time == Utf8(schedule.time) &&
                EqualPath(Utf16(folder), schedule.folder)) return 0;
        }
        auto tree = Build(profile ? profile->sources : LoadSources());
        ApplyIgnored(*tree, profile ? profile->excluded : LoadIgnored());
        if (tree->incomplete) Fail(L"Backup stopped because a source item could not be read. Review Scan issues in RainBackup.");
        zip = SaveManagedBackup(*tree, schedule, {}, true, profile ? profile->name : L"Backup");
        std::ofstream last(lastPath, std::ios::trunc);
        last << due << ' ' << schedule.weekly << ' ' << std::quoted(Utf8(schedule.time))
            << ' ' << std::quoted(Utf8(schedule.folder.native())) << '\n';
        last.close();
        if (!last) Fail(L"Backup saved, but the scheduled run record could not be written.");
        LogOperation(L"Scheduled backup", L"Success", backupName + L"Saved " + zip.wstring() + (tree->warnings.empty() ? L"" :
            L"\nSkipped " + std::to_wstring(tree->warnings.size()) + L" linked item(s)."), zip);
        return 0;
    } catch (const std::exception& error) {
        try { LogOperation(L"Scheduled backup", L"Failure", backupName + destination.wstring() + L"\n" + Utf16(error.what()), zip); }
        catch (const std::exception&) {}
        return 1;
    }
}
}

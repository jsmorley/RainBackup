#include "CommandLine.h"
#include "RainmeterSession.h"
#include "Schedule.h"

#include <shellapi.h>
#include <shlobj.h>

using namespace rb;

namespace {
bool SameName(const wchar_t* first, const wchar_t* second) {
    return CompareStringOrdinal(first, -1, second, -1, TRUE) == CSTR_EQUAL;
}

fs::path LatestArchive(const BackupProfile& profile) {
    CheckPath(profile.schedule.folder);
    std::pair<fs::file_time_type, fs::path> latest;
    for (const auto& entry : fs::directory_iterator(profile.schedule.folder)) {
        const auto& path = entry.path();
        if (!EqualPath(path.extension(), L".zip") || IsReparse(path) ||
            !entry.is_regular_file() || !IsProfileArchive(path, profile.schedule.id)) continue;
        const auto candidate = std::pair{entry.last_write_time(), path};
        if (latest.second.empty() || latest < candidate) latest = candidate;
    }
    if (latest.second.empty()) Fail(L"No backup ZIP was found in " + profile.schedule.folder.wstring());
    return latest.second;
}
}

int RunNamedCommand() {
    int count = 0;
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    struct Arguments { wchar_t** value; ~Arguments() { LocalFree(value); } } parsed{arguments};
    if (!arguments || count != 3 || !*arguments[1]) return 2;

    const bool backup = SameName(arguments[2], L"Backup");
    if (!backup && !SameName(arguments[2], L"Restore")) return 2;

    const std::wstring name = arguments[1];
    const std::wstring operation = backup ? L"Command-line backup" : L"Command-line restore";
    const auto started = std::chrono::steady_clock::now();
    auto elapsed = [&]() -> std::optional<std::chrono::seconds> {
        if (!backup) return std::nullopt;
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);
    };
    fs::path archive;
    try {
        auto defaultFolder = LoadBackupFolder();
        if (defaultFolder.empty()) defaultFolder = KnownFolder(FOLDERID_Documents);
        auto profiles = LoadBackupProfiles(defaultFolder);
        const BackupProfile* profile = nullptr;
        for (const auto& candidate : profiles) {
            if (!SameName(candidate.name.c_str(), name.c_str())) continue;
            if (profile) Fail(L"More than one backup matches this name.");
            profile = &candidate;
        }
        if (!profile) Fail(L"No backup scheme matches this name.");

        if (backup) {
            auto tree = Build(profile->sources);
            ApplyIgnored(*tree, profile->excluded);
            if (tree->incomplete) Fail(L"Backup stopped because a source item could not be read. Review Scan issues in RainBackup.");
            archive = SaveManagedBackup(*tree, profile->schedule, {}, profile->automatic, profile->name);
        } else {
            archive = LatestArchive(*profile);
            const auto opened = ReadZip(archive);
            if (!opened.originalLocations) Fail(L"The backup ZIP does not contain original restore locations.");
            if (!RainmeterInstalled(profile->sources)) Fail(L"Install Rainmeter or register a portable setup before restoring.");
            std::vector<fs::path> destinations;
            for (const auto& source : opened.tree->sources) destinations.push_back(source.path);
            RestoreWithRainmeterRestart(opened, PlanRestore(opened, destinations, false));
        }
        LogOperation(operation, L"Success", name + L"\n" + archive.wstring(), archive, elapsed());
        return 0;
    } catch (const std::exception& error) {
        try {
            auto detail = name;
            if (!archive.empty()) detail += L"\n" + archive.wstring();
            LogOperation(operation, L"Failure", detail + L"\n" + Utf16(error.what()), archive, elapsed());
        }
        catch (const std::exception&) {}
        return 1;
    }
}

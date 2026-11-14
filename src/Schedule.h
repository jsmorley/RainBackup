#pragma once
#include "Archive.h"
namespace rb {
struct Schedule {
    fs::path folder;
    unsigned copies = 7;
    bool weekly = false;
    std::wstring time = L"18:00";
    std::wstring id;
};
struct BackupProfile {
    std::wstring name;
    Schedule schedule;
    bool automatic = false;
    std::vector<Source> sources;
    std::vector<fs::path> excluded;
};
std::vector<BackupProfile> LoadBackupProfiles(const fs::path& defaultFolder);
void SaveBackupProfile(const BackupProfile& profile);
void DeleteBackupProfile(std::wstring_view id);
bool IsProfileArchive(const fs::path& path, std::wstring_view id);
std::wstring NewScheduleId();
void SaveSchedule(const Schedule& schedule);
void DeleteSchedule(std::wstring_view id);
void DeleteAllSchedules();
fs::path SaveManagedBackup(const Tree& tree, const Schedule& schedule, const Transfer& transfer = {},
    bool retain = true, std::wstring_view name = L"Backup");
int RunScheduled(bool catchUp, std::wstring_view id = {});
void LogOperation(std::wstring_view operation, std::wstring_view result, std::wstring_view detail = {},
    const fs::path& archive = {});
std::wstring ReadOperationLog();
std::wstring LastScheduledBackup();
void EmptyOperationLog();
void ApplyIgnored(Tree& tree, const std::vector<fs::path>& ignored);
}

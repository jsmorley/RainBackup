#pragma once
#include "Archive.h"
namespace rb {
struct Schedule {
    fs::path folder;
    unsigned copies = 7;
    bool weekly = false;
    std::wstring time = L"18:00";
};
Schedule LoadSchedule();
void SaveSchedule(const Schedule& schedule);
void DeleteSchedule();
fs::path SaveManagedBackup(const Tree& tree, const Schedule& schedule, const Transfer& transfer = {});
int RunScheduled(bool catchUp);
void ApplyIgnored(Tree& tree, const std::vector<fs::path>& ignored);
}

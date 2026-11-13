#include "Archive.h"
#include "Schedule.h"

#include <fstream>
#include <iostream>

using namespace rb;
namespace {
void Require(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}

void Write(const fs::path& path, std::string_view content) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << content;
    Require(bool(stream), "fixture write failed");
}

std::string Read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
}

int main() {
    auto workspace = fs::current_path() / L"fixtures";
    try {
        auto nonce = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        workspace /= nonce;
        auto skins = workspace / L"Old Account" / L"Skins";
        auto settings = workspace / L"Old Account" / L"Settings";
        Write(skins / L"Clock" / L"Clock.ini", "clock");
        Write(skins / L"Clock" / L"Variables.inc", "vars");
        Write(settings / L"Rainmeter.ini", "[Rainmeter]\r\nSkinPath=C:\\Old Account\\Skins\r\n");
        Write(settings / L"Plugins" / L"sample.dll", "plugin");
        fs::create_directories(skins / L"Empty");
        auto extra = workspace / L"notes.txt";
        Write(extra, "notes");
        auto tree = Build({{L"Installed Rainmeter", Kind::Skins, skins},
            {L"Installed Rainmeter", Kind::Settings, settings},
            {L"Installed Rainmeter", Kind::Extra, extra}});
        Require(tree->root.checked == 6, "scan did not enumerate expected files and empty directory");
        ApplyIgnored(*tree, {skins / L"Clock" / L"Variables.inc"});
        Require(tree->root.checked == 5, "exclusion did not update the tree");
        auto archive = workspace / L"Backup.zip";
        int backupProgress = 0;
        WriteZip(*tree, archive, {[&](const std::wstring&, int percent) {
            Require(percent >= backupProgress && percent < 100, "invalid backup progress");
            backupProgress = percent;
        }});
        Require(backupProgress >= 98, "backup progress did not advance");
        auto zip = ReadZip(archive);
        Require(zip.originalLocations && zip.tree->root.checked == 5, "ZIP selection did not round-trip");
        auto targets = NewLocations(zip, workspace / L"New Account");
        Write(targets[0] / L"Clock" / L"Clock.ini", "existing");
        auto plan = PlanRestore(zip, targets, true);
        Require(plan.files == 4 && plan.overwrites == 1, "restore preview has wrong totals");
        int restoreProgress = 0;
        Restore(zip, plan, {[&](const std::wstring&, int percent) {
            Require(percent >= restoreProgress && percent < 100, "invalid restore progress");
            restoreProgress = percent;
        }});
        Require(restoreProgress >= 98, "restore progress did not advance");
        Require(Read(targets[0] / L"Clock" / L"Clock.ini") == "clock", "overwrite did not restore the file");
        Require(!fs::exists(targets[0] / L"Clock" / L"Variables.inc"), "excluded file was restored");
        Require(fs::is_directory(targets[0] / L"Empty"), "empty folder missing");
        Require(Read(targets[1] / L"Plugins" / L"sample.dll") == "plugin", "nested plugin missing");
        Require(Read(targets[2]) == "notes", "added file did not restore");
        wchar_t value[2048]{};
        GetPrivateProfileStringW(L"Rainmeter", L"SkinPath", L"", value, 2048, (targets[1] / L"Rainmeter.ini").c_str());
        Require(EqualPath(value, targets[0]), "SkinPath was not updated for new Windows account");
        zip.tree->root.Select(false);
        auto& selection = zip.tree->root.children.front()->children.at(1);
        selection->children.front()->Select(true);
        zip.tree->root.Count();
        auto partial = PlanRestore(zip, {skins, settings, extra}, false);
        Require(partial.files == 1 && partial.overwrites == 1, "partial original-location restore is incorrect");
        Restore(zip, partial);
        zip.tree->root.Select(true);
        zip.tree->root.Count();
        Require(PlanRestore(zip, targets, true).files == 4, "select all did not restore full selection");

        auto originalZip = Read(archive);
        auto cancelTargets = NewLocations(zip, workspace / L"Canceled Restore");
        Write(cancelTargets[0] / L"Clock" / L"Clock.ini", "keep original");
        auto cancelPlan = PlanRestore(zip, cancelTargets, true);
        std::stop_source restoreStop;
        bool restoreCanceled = false;
        int restoreUpdates = 0;
        try {
            Restore(zip, cancelPlan, {[&](const std::wstring& label, int) {
                if (label.ends_with(L"Clock.ini") && ++restoreUpdates == 2) restoreStop.request_stop();
            }, restoreStop.get_token()});
        } catch (const Cancelled&) { restoreCanceled = true; }
        Require(restoreCanceled, "restore cancellation was ignored");
        Require(Read(cancelTargets[0] / L"Clock" / L"Clock.ini") == "keep original",
            "canceled restore changed the destination file");
        zip.archive.reset();

        std::stop_source backupStop;
        bool backupCanceled = false;
        int backupUpdates = 0;
        try {
            WriteZip(*tree, archive, {[&](const std::wstring& label, int) {
                if (label.ends_with(L"Clock.ini") && ++backupUpdates == 2) backupStop.request_stop();
            }, backupStop.get_token()});
        } catch (const Cancelled&) { backupCanceled = true; }
        Require(backupCanceled && Read(archive) == originalZip, "canceled backup replaced the existing ZIP");
        for (const auto& item : fs::recursive_directory_iterator(workspace)) {
            Require(!item.path().filename().wstring().starts_with(L".RainBackup-"), "cancellation leaked a temporary file");
        }

        auto managed = workspace / L"managed";
        fs::create_directories(managed);
        Write(managed / L"unrelated.zip", "unrelated");
        Schedule schedule{managed, 2};
        auto first = SaveManagedBackup(*tree, schedule);
        Sleep(20);
        auto second = SaveManagedBackup(*tree, schedule);
        Sleep(20);
        auto third = SaveManagedBackup(*tree, schedule);
        Require(!fs::exists(first) && fs::exists(second) && fs::exists(third), "FIFO retention removed the wrong backups");
        Require(Read(managed / L"unrelated.zip") == "unrelated", "FIFO removed an unrelated file");

        fs::remove(skins / L"Clock" / L"Clock.ini");
        bool failed = false;
        auto oldZip = Read(archive);
        try { WriteZip(*tree, archive); }
        catch (const std::exception&) { failed = true; }
        Require(failed && Read(archive) == oldZip, "failed backup replaced the existing ZIP");
        fs::remove_all(workspace);
        std::cout << "Backup, selective restore, relocated settings, and FIFO passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nFixture: " << workspace << '\n';
        return 1;
    }
}

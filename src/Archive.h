#pragma once
#include "Model.h"
#include <stop_token>

namespace rb {
struct Archive;
struct OpenZip {
    std::unique_ptr<Tree> tree;
    std::shared_ptr<Archive> archive;
    bool originalLocations = false;
};

struct Transfer {
    std::function<void(const std::wstring&, int)> report;
    std::stop_token stop;
};

enum class ZipDisposition { ReplaceExisting, CreateNew };
void WriteZip(const Tree& tree, const fs::path& zip, const Transfer& transfer = {},
    ZipDisposition disposition = ZipDisposition::ReplaceExisting);
OpenZip ReadZip(const fs::path& path);
std::vector<fs::path> NewLocations(const OpenZip& zip, const fs::path& folder);
std::vector<fs::path> InstalledLocations(const OpenZip& zip, const std::vector<Source>& detected);
struct RestorePlan {
    struct Item {
        fs::path target;
        size_t entry;
        bool directory;
        fs::path skinPath;
    };
    std::vector<Item> items;
    std::vector<fs::path> destinations;
    size_t files = 0;
    size_t overwrites = 0;
};
RestorePlan PlanRestore(const OpenZip& zip, const std::vector<fs::path>& destinations, bool relocate);
void Restore(const OpenZip& zip, const RestorePlan& plan, const Transfer& transfer = {});
}

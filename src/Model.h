#pragma once

#include <windows.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace rb {
namespace fs = std::filesystem;

enum class Kind { Skins, Settings, PortableSettings, Extra };
struct Source {
    std::wstring setup;
    Kind kind;
    fs::path path;
};

struct Node {
    std::wstring name;
    fs::path path;
    bool directory = true;
    bool selected = true;
    bool available = true;
    Node* parent = nullptr;
    size_t source = 0;
    size_t entry = 0;
    size_t total = 0;
    size_t checked = 0;
    std::vector<std::unique_ptr<Node>> children;

    Node& Add(std::wstring name, bool directory, size_t source = 0);
    void Count();
    void Select(bool value);
};

struct Tree {
    Node root;
    std::vector<Source> sources;
    std::vector<std::wstring> warnings;
    bool incomplete = false;
};

using Progress = std::function<void(std::wstring)>;
struct Cancelled : std::runtime_error {
    using std::runtime_error::runtime_error;
};
std::wstring KindName(Kind kind);
std::wstring KnownFolder(const GUID& id);
std::wstring Utf16(std::string_view text, unsigned codePage = 65001);
std::string Utf8(std::wstring_view text);
void Fail(std::wstring text);
bool EqualPath(const fs::path& a, const fs::path& b);
bool IsWithin(const fs::path& base, const fs::path& path);
bool IsAbsolutePath(const fs::path& path);
bool IsReparse(const fs::path& path);
void CheckPath(const fs::path& path);
bool Merge(std::vector<Source>& sources, Source source);
std::vector<Source> Scan(const fs::path& searchFolder = {});
bool RainmeterInstalled(const std::vector<Source>& configured = {});
std::unique_ptr<Tree> Build(std::vector<Source> sources, const Progress& progress = {}, std::stop_token stop = {});
std::string EncodeSources(const std::vector<Source>& sources);
std::vector<Source> DecodeSources(std::string_view data);
std::vector<Source> LoadSources();
std::vector<fs::path> LoadIgnored();
fs::path LoadBackupFolder();
std::vector<fs::path> LoadBackupDestinations();
void SaveBackupDestinations(const std::vector<fs::path>& paths);
bool LoadButtonTooltips();
void SaveButtonTooltips(bool enabled);
std::optional<bool> LoadDarkMode();
void SaveDarkMode(bool enabled);
bool Load24HourTime();
void Save24HourTime(bool enabled);
}

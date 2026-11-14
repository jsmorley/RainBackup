#include "Model.h"

#include <shlobj.h>
#include <tlhelp32.h>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>

namespace rb {
namespace {
bool Same(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

fs::path Clean(fs::path path) {
    path = path.lexically_normal().make_preferred();
    if (path != path.root_path() && path.filename().empty()) path = path.parent_path();
    return path;
}

fs::path Config(std::wstring_view name) {
    return fs::path(KnownFolder(FOLDERID_LocalAppData)) / L"RainBackup" / name;
}

void WriteConfig(const fs::path& path, std::string_view data) {
    fs::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    stream.write(data.data(), data.size());
    stream.close();
    if (!stream || !MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        Fail(L"Could not save " + path.wstring());
    }
}

std::wstring RegistryPath(HKEY root, REGSAM view) {
    wchar_t buffer[32768]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(root, L"SOFTWARE\\Rainmeter", nullptr,
        RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | view, nullptr, buffer, &size) == ERROR_SUCCESS) {
        return buffer;
    }
    return {};
}

fs::path SkinPath(const fs::path& settings, const fs::path& fallback) {
    wchar_t value[32768]{};
    GetPrivateProfileStringW(L"Rainmeter", L"SkinPath", L"", value,
        static_cast<DWORD>(std::size(value)), (settings / L"Rainmeter.ini").c_str());
    if (!*value) return fallback;
    wchar_t expanded[32768]{};
    DWORD length = ExpandEnvironmentStringsW(value, expanded, static_cast<DWORD>(std::size(expanded)));
    fs::path path = (length && length <= std::size(expanded)) ? expanded : value;
    if (!IsAbsolutePath(path)) return fallback;
    return Clean(path);
}

bool Exists(const fs::path& path) {
    std::error_code error;
    return fs::exists(path, error);
}

void ReadFolder(Node& node, const Source& source, const fs::path& excluded,
    Tree& tree, std::stop_token stop) {
    std::error_code error;
    std::vector<fs::directory_entry> entries;
    for (fs::directory_iterator it(node.path, error), end; !error && it != end; it.increment(error)) {
        if (stop.stop_requested()) throw Cancelled("Backup canceled.");
        entries.push_back(*it);
    }
    if (error) {
        tree.warnings.push_back(L"Could not list " + node.path.wstring());
        tree.incomplete = true;
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return CompareStringOrdinal(a.path().filename().c_str(), -1,
            b.path().filename().c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    for (const auto& entry : entries) {
        if (stop.stop_requested()) throw Cancelled("Backup canceled.");
        const auto& path = entry.path();
        if (!excluded.empty() && EqualPath(path, excluded)) continue;
        if (source.kind == Kind::PortableSettings && EqualPath(node.path, source.path)) {
            auto name = path.filename().wstring();
            if (Same(name, L"Rainmeter.exe") || Same(name, L"Rainmeter.dll") ||
                Same(name, L"Rainmeter.exe.config") || Same(name, L"SkinInstaller.exe") ||
                Same(name, L"uninst.exe") || Same(name, L"Defaults") ||
                Same(name, L"Languages") || Same(name, L"Skins")) continue;
        }
        DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            tree.warnings.push_back(L"Unavailable: " + path.wstring());
            tree.incomplete = true;
            continue;
        }
        if (IsReparse(path)) {
            tree.warnings.push_back(L"Skipped linked item " + path.wstring());
            continue;
        }
        auto& child = node.Add(path.filename().wstring(), (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0, node.source);
        child.path = path;
        if (child.directory) ReadFolder(child, source, excluded, tree, stop);
    }
}
}

Node& Node::Add(std::wstring label, bool folder, size_t index) {
    auto child = std::make_unique<Node>();
    child->name = std::move(label);
    child->directory = folder;
    child->parent = this;
    child->source = index;
    children.push_back(std::move(child));
    return *children.back();
}

void Node::Count() {
    total = available ? 1 : 0;
    checked = selected && available ? 1 : 0;
    if (children.empty()) return;
    total = checked = 0;
    for (auto& child : children) {
        child->Count();
        total += child->total;
        checked += child->checked;
    }
}

void Node::Select(bool value) {
    selected = value && available;
    for (auto& child : children) child->Select(value);
}

std::wstring KindName(Kind kind) {
    switch (kind) {
    case Kind::Skins: return L"Skins";
    case Kind::Settings:
    case Kind::PortableSettings: return L"Settings & Plugins";
    default: return L"Additional items";
    }
}

std::wstring KnownFolder(const GUID& id) {
    wchar_t* path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &path))) return {};
    std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}

std::wstring Utf16(std::string_view text, unsigned codePage) {
    if (text.empty()) return {};
    DWORD flags = codePage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
    int count = MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) throw std::runtime_error("Invalid text encoding.");
    std::wstring result(count, L'\0');
    MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::string Utf8(std::wstring_view text) {
    if (text.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("Invalid Unicode path.");
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}

void Fail(std::wstring text) {
    throw std::runtime_error(Utf8(text));
}

bool EqualPath(const fs::path& a, const fs::path& b) {
    return Same(Clean(a).native(), Clean(b).native());
}

bool IsWithin(const fs::path& base, const fs::path& path) {
    auto root = Clean(base);
    auto item = Clean(path);
    auto part = item.begin();
    for (const auto& component : root) {
        if (part == item.end() || !Same(component.native(), part->native())) return false;
        ++part;
    }
    return true;
}

bool IsAbsolutePath(const fs::path& path) {
    auto name = path.native();
    return path.is_absolute() && !name.starts_with(L"\\\\?\\") &&
        !name.starts_with(L"\\\\.\\") && name.find(L':', path.has_root_name() ? 2 : 0) == std::wstring::npos;
}

bool IsReparse(const fs::path& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    WIN32_FIND_DATAW data{};
    auto handle = FindFirstFileW(path.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) return true;
    FindClose(handle);
    const DWORD tag = data.dwReserved0;
    return (tag & ~IO_REPARSE_TAG_CLOUD_MASK) != IO_REPARSE_TAG_CLOUD &&
        tag != IO_REPARSE_TAG_ONEDRIVE;
}

void CheckPath(const fs::path& path) {
    if (!IsAbsolutePath(path) || !EqualPath(path, path.lexically_normal()) ||
        std::any_of(path.begin(), path.end(), [](const auto& part) { return part == L".." || part == L"."; })) {
        Fail(L"Invalid path " + path.wstring());
    }
    for (const auto& part : path.relative_path()) {
        const auto& name = part.native();
        if (name.empty() || name.ends_with(L' ') || name.ends_with(L'.') ||
            name.find_first_of(L"<>:\"|?*") != std::wstring::npos) Fail(L"Invalid path " + path.wstring());
    }
    for (auto current = path; current != current.root_path(); current = current.parent_path()) {
        if (IsReparse(current)) Fail(L"Linked paths are skipped: " + current.wstring());
    }
}

bool Merge(std::vector<Source>& sources, Source source) {
    source.path = Clean(std::move(source.path));
    for (const auto& item : sources) {
        if (item.setup == source.setup && item.kind == source.kind && EqualPath(item.path, source.path)) return false;
    }
    sources.push_back(std::move(source));
    return true;
}

namespace {
std::vector<fs::path> FindPrograms(const fs::path& searchFolder) {
    std::vector<fs::path> programs;
    auto addProgram = [&](const fs::path& folder) {
        if (IsAbsolutePath(folder) && Exists(folder / L"Rainmeter.exe") &&
            std::none_of(programs.begin(), programs.end(), [&](const auto& p) { return EqualPath(p, folder); })) {
            programs.push_back(folder);
        }
    };
    if (searchFolder.empty()) {
        addProgram(fs::path(KnownFolder(FOLDERID_ProgramFiles)) / L"Rainmeter");
        addProgram(fs::path(KnownFolder(FOLDERID_ProgramFilesX86)) / L"Rainmeter");
        for (auto key : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
            for (auto view : {RRF_SUBKEY_WOW6464KEY, RRF_SUBKEY_WOW6432KEY}) {
                addProgram(RegistryPath(key, view));
            }
        }
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W process{};
            process.dwSize = sizeof(process);
            for (BOOL more = Process32FirstW(snapshot, &process); more; more = Process32NextW(snapshot, &process)) {
                if (!Same(process.szExeFile, L"Rainmeter.exe")) continue;
                HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.th32ProcessID);
                if (!handle) continue;
                wchar_t path[32768]{};
                DWORD length = static_cast<DWORD>(std::size(path));
                if (QueryFullProcessImageNameW(handle, 0, path, &length)) addProgram(fs::path(path).parent_path());
                CloseHandle(handle);
            }
            CloseHandle(snapshot);
        }
    } else {
        CheckPath(searchFolder);
        addProgram(searchFolder);
        std::error_code error;
        for (fs::recursive_directory_iterator it(searchFolder, fs::directory_options::skip_permission_denied, error), end;
             !error && it != end; it.increment(error)) {
            if (IsReparse(it->path())) it.disable_recursion_pending();
            else if (Same(it->path().filename().native(), L"Rainmeter.exe")) addProgram(it->path().parent_path());
        }
        if (error) Fail(L"Could not finish scanning " + searchFolder.wstring());
    }
    return programs;
}

bool HasProgram(const fs::path& folder) {
    std::error_code error;
    return fs::is_regular_file(folder / L"Rainmeter.exe", error) &&
        fs::is_regular_file(folder / L"Rainmeter.dll", error);
}
}

bool RainmeterInstalled(const std::vector<Source>& configured) {
    auto programs = FindPrograms({});
    return std::any_of(programs.begin(), programs.end(), HasProgram) ||
        std::any_of(configured.begin(), configured.end(), [](const Source& source) {
            constexpr std::wstring_view prefix = L"Portable Rainmeter — ";
            return source.setup.starts_with(prefix) && HasProgram(source.setup.substr(prefix.size()));
        });
}

std::vector<Source> Scan(const fs::path& searchFolder) {
    std::vector<Source> sources;
    auto programs = FindPrograms(searchFolder);
    if (searchFolder.empty()) {
        fs::path settings = fs::path(KnownFolder(FOLDERID_RoamingAppData)) / L"Rainmeter";
        if (Exists(settings) || std::any_of(programs.begin(), programs.end(),
            [&](const auto& p) { return !Exists(p / L"Rainmeter.ini"); })) {
            auto documents = fs::path(KnownFolder(FOLDERID_Documents));
            auto fallback = documents / L"Rainmeter" / L"Skins";
            if (!Exists(fallback) && Exists(documents / L"Skins")) fallback = documents / L"Skins";
            Merge(sources, {L"Installed Rainmeter", Kind::Skins,
                SkinPath(settings, fallback)});
            Merge(sources, {L"Installed Rainmeter", Kind::Settings, settings});
        }
    }
    for (const auto& folder : programs) {
        if (!Exists(folder / L"Rainmeter.ini")) continue;
        const auto name = L"Portable Rainmeter — " + folder.wstring();
        Merge(sources, {name, Kind::Skins, SkinPath(folder, folder / L"Skins")});
        Merge(sources, {name, Kind::PortableSettings, folder});
    }
    return sources;
}

std::unique_ptr<Tree> Build(std::vector<Source> sources, const Progress& progress, std::stop_token stop) {
    auto tree = std::make_unique<Tree>();
    tree->sources = std::move(sources);
    std::map<std::wstring, Node*> setups;
    for (size_t index = 0; index < tree->sources.size(); ++index) {
        if (stop.stop_requested()) throw Cancelled("Backup canceled.");
        const auto& source = tree->sources[index];
        if (progress) progress(L"Reading " + source.path.wstring());
        auto& setup = setups[source.setup];
        if (!setup) setup = &tree->root.Add(source.setup, true);
        auto attributes = GetFileAttributesW(source.path.c_str());
        const bool folder = attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY);
        auto& node = setup->Add(KindName(source.kind) + L"   •   " + source.path.wstring(), folder, index);
        node.path = source.path;
        try {
            CheckPath(source.path);
        } catch (const std::exception& error) {
            tree->warnings.push_back(Utf16(error.what()));
            tree->incomplete = true;
            node.selected = false;
            node.available = false;
            continue;
        }
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            node.selected = false;
            node.available = false;
            tree->warnings.push_back(L"Unavailable: " + source.path.wstring());
            tree->incomplete = true;
            continue;
        }
        if (folder) {
            fs::path excluded;
            if (source.kind == Kind::Settings || source.kind == Kind::PortableSettings) {
                for (const auto& candidate : tree->sources) {
                    if (candidate.setup == source.setup && candidate.kind == Kind::Skins &&
                        !EqualPath(source.path, candidate.path) && IsWithin(source.path, candidate.path)) {
                        excluded = candidate.path;
                        break;
                    }
                }
            }
            ReadFolder(node, source, excluded, *tree, stop);
        }
    }
    tree->root.Count();
    return tree;
}

std::string EncodeSources(const std::vector<Source>& sources) {
    std::ostringstream stream;
    stream << "RainBackup 1\n";
    for (const auto& source : sources) {
        stream << static_cast<int>(source.kind) << ' ' << std::quoted(Utf8(source.setup))
            << ' ' << std::quoted(Utf8(source.path.native())) << '\n';
    }
    return stream.str();
}

std::vector<Source> DecodeSources(std::string_view data) {
    std::istringstream stream{std::string(data)};
    std::string line;
    if (!std::getline(stream, line) || line != "RainBackup 1") Fail(L"Unsupported RainBackup metadata.");
    std::vector<Source> result;
    while (std::getline(stream, line)) {
        std::istringstream row(line);
        int kind = -1;
        std::string setup, path;
        if (!(row >> kind >> std::quoted(setup) >> std::quoted(path)) || kind < 0 || kind > 3 ||
            setup.empty() || (row >> std::ws && !row.eof())) Fail(L"Invalid RainBackup metadata.");
        auto decoded = fs::path(Utf16(path));
        if (!IsAbsolutePath(decoded) || std::any_of(decoded.begin(), decoded.end(),
            [](const auto& part) { return part == L"." || part == L".."; })) Fail(L"Invalid original location.");
        result.push_back({Utf16(setup), static_cast<Kind>(kind), std::move(decoded)});
    }
    return result;
}

std::vector<Source> LoadSources() {
    std::ifstream stream(Config(L"sources.txt"), std::ios::binary);
    if (!stream) return {};
    std::string data((std::istreambuf_iterator<char>(stream)), {});
    return DecodeSources(data);
}

std::vector<fs::path> LoadIgnored() {
    std::ifstream stream(Config(L"excluded.txt"), std::ios::binary);
    std::vector<fs::path> result;
    std::string line;
    while (std::getline(stream, line)) result.emplace_back(Utf16(line));
    return result;
}

fs::path LoadBackupFolder() {
    std::ifstream stream(Config(L"backup-folder.txt"), std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(stream)), {});
    return Utf16(data);
}

fs::path LoadOpenZipFolder() {
    std::ifstream stream(Config(L"open-zip-folder.txt"), std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(stream)), {});
    auto folder = fs::path(Utf16(data));
    return IsAbsolutePath(folder) ? folder : fs::path{};
}

void SaveOpenZipFolder(const fs::path& folder) {
    WriteConfig(Config(L"open-zip-folder.txt"), Utf8(folder.native()));
}

bool LoadButtonTooltips() {
    std::ifstream input(Config(L"button-tooltips.txt"), std::ios::binary);
    return input.get() != '0';
}

void SaveButtonTooltips(bool enabled) {
    WriteConfig(Config(L"button-tooltips.txt"), enabled ? "1" : "0");
}

std::optional<bool> LoadDarkMode() {
    std::ifstream input(Config(L"dark-mode.txt"), std::ios::binary);
    const auto state = input.get();
    if (state == '0' || state == '1') return state == '1';
    return std::nullopt;
}

void SaveDarkMode(bool enabled) {
    WriteConfig(Config(L"dark-mode.txt"), enabled ? "1" : "0");
}

bool Load24HourTime() {
    std::ifstream input(Config(L"24-hour-time.txt"), std::ios::binary);
    return input.get() != '0';
}

void Save24HourTime(bool enabled) {
    WriteConfig(Config(L"24-hour-time.txt"), enabled ? "1" : "0");
}

std::optional<POINT> LoadMainWindowPosition() {
    std::ifstream input(Config(L"main-window-position.txt"));
    POINT position{};
    if (input >> position.x >> position.y && (input >> std::ws).eof()) return position;
    return std::nullopt;
}

void SaveMainWindowPosition(POINT position) {
    WriteConfig(Config(L"main-window-position.txt"),
        std::to_string(position.x) + " " + std::to_string(position.y) + "\n");
}
}

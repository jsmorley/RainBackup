#include "Archive.h"

#include <shlobj.h>
#include <aclapi.h>
#include <unzip.h>
#include <zip.h>
#include <iowin32.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <map>
#include <set>

namespace rb {
struct Archive {
    struct Entry {
        std::wstring name;
        unz64_file_pos position{};
        unz_file_info64 info{};
        bool directory = false;
    };
    fs::path path;
    unzFile reader = nullptr;
    std::vector<Entry> entries;
    ~Archive() { if (reader) unzClose(reader); }
};

namespace {
constexpr auto Metadata = L"RainBackup/locations.txt";
constexpr size_t MetadataLimit = 8 * 1024 * 1024;

struct PathLess {
    bool operator()(const fs::path& a, const fs::path& b) const {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    }
};

std::wstring ErrorText(DWORD error) {
    wchar_t* buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring result = buffer ? buffer : L"Windows error " + std::to_wstring(error);
    if (buffer) LocalFree(buffer);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    return result;
}

[[noreturn]] void FileError(std::wstring action, const fs::path& path, DWORD error) {
    Fail(std::move(action) + L"\n" + path.wstring() + L"\n\n" + ErrorText(error));
}

class TemporaryFile {
public:
    explicit TemporaryFile(const fs::path& directory) {
        GUID id{};
        wchar_t name[40]{};
        if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, name, static_cast<int>(std::size(name)))) {
            Fail(L"Could not generate a temporary file name.");
        }
        path = directory / (std::wstring(L".RainBackup-") + name + L".tmp");
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) FileError(L"Could not create a temporary file.", path, GetLastError());
        CloseHandle(file);
    }
    ~TemporaryFile() { DeleteFileW(path.c_str()); }
    fs::path path;
};

std::wstring RootKey(size_t index) {
    return L"RainBackup/items/" + std::to_wstring(index);
}

bool Available(const Node& node) {
    return node.checked && (node.children.empty() || node.directory);
}

class TransferProgress {
public:
    explicit TransferProgress(const Transfer& transfer) : transfer_(transfer) {}
    void Check() const {
        if (transfer_.stop.stop_requested()) throw Cancelled("Operation canceled.");
    }
    void Begin(std::wstring label) {
        label_ = std::move(label);
        Advance(0);
        Check();
    }
    void Advance(uintmax_t bytes) {
        Check();
        done_ += static_cast<double>(bytes);
        if (transfer_.report) transfer_.report(label_, static_cast<int>(std::min(99.0, 99 * done_ / total)));
    }
    double total = 1;
private:
    const Transfer& transfer_;
    double done_ = 0;
    std::wstring label_;
};

// Give even empty files and folders some weight.
constexpr uintmax_t EntryWork = 4096;

double BackupWork(const Node& node, const TransferProgress& progress) {
    if (!Available(node)) return 0;
    progress.Check();
    double work = EntryWork;
    if (!node.directory) work += static_cast<double>(fs::file_size(node.path));
    for (const auto& child : node.children) work += BackupWork(*child, progress);
    return work;
}

void AddEntry(zipFile zip, std::wstring name, const fs::path& path, bool directory,
    TransferProgress& progress, std::string_view data = {}) {
    if (directory) name += L'/';
    auto utf8 = Utf8(name);
    if (utf8.size() > 65535) Fail(L"ZIP entry name is too long: " + name);
    zip_fileinfo info{};
    uintmax_t size = data.size();
    if (!path.empty()) {
        CheckPath(path);
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
            FileError(L"Could not read the selected item.", path, GetLastError());
        }
        if (directory != ((attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) {
            Fail(L"The selected item changed type: " + path.wstring());
        }
        ULARGE_INTEGER length{};
        length.LowPart = attributes.nFileSizeLow;
        length.HighPart = attributes.nFileSizeHigh;
        size = length.QuadPart;
        FILETIME local{};
        WORD date = 0, time = 0;
        if (FileTimeToLocalFileTime(&attributes.ftLastWriteTime, &local) &&
            FileTimeToDosDateTime(&local, &date, &time)) info.dosDate = (DWORD(date) << 16) | time;
    }
    info.external_fa = directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (!info.dosDate) info.dosDate = (1 << 21) | (1 << 16);
    if (zipOpenNewFileInZip4_64(zip, utf8.c_str(), &info, nullptr, 0, nullptr, 0, nullptr,
        directory ? 0 : Z_DEFLATED, Z_DEFAULT_COMPRESSION, 0, -MAX_WBITS, DEF_MEM_LEVEL,
        Z_DEFAULT_STRATEGY, nullptr, 0, 0, 1 << 11, size >= 0xff000000ULL) != ZIP_OK) {
        Fail(L"Could not add " + name + L" to the ZIP.");
    }
    if (!directory && !path.empty()) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) FileError(L"Could not open a selected file. If it is online-only, check OneDrive and try again.", path, GetLastError());
        struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{file};
        std::array<char, 65536> buffer{};
        uintmax_t read = 0;
        for (;;) {
            DWORD count = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
                FileError(L"Could not read a selected file. If it is online-only, check OneDrive and try again.", path, GetLastError());
            }
            if (!count) break;
            read += count;
            if (zipWriteInFileInZip(zip, buffer.data(), count) != ZIP_OK) Fail(L"Could not write to the ZIP.");
            progress.Advance(count);
        }
        if (read != size) Fail(L"The selected file changed size during backup: " + path.wstring());
    } else if (!data.empty() &&
        zipWriteInFileInZip(zip, data.data(), static_cast<unsigned>(data.size())) != ZIP_OK) {
        Fail(L"Could not write the ZIP metadata.");
    }
    if (zipCloseFileInZip(zip) != ZIP_OK) Fail(L"Could not finish ZIP entry " + name);
}

void WriteNode(zipFile zip, const Node& node, const std::wstring& name, TransferProgress& progress) {
    if (!Available(node)) return;
    progress.Begin(L"Backing up " + node.path.wstring());
    AddEntry(zip, name, node.path, node.directory, progress);
    progress.Advance(EntryWork);
    for (const auto& child : node.children) WriteNode(zip, *child, name + L"/" + child->name, progress);
}

std::vector<std::wstring> Components(std::wstring name) {
    std::replace(name.begin(), name.end(), L'\\', L'/');
    if (name.ends_with(L'/')) name.pop_back();
    std::vector<std::wstring> parts;
    for (size_t start = 0; start <= name.size();) {
        const auto end = name.find(L'/', start);
        auto part = name.substr(start, end == std::wstring::npos ? end : end - start);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ' ||
            std::any_of(part.begin(), part.end(), [](wchar_t ch) {
                return ch < 32 || std::wstring_view(L":*?\"<>|").find(ch) != std::wstring_view::npos;
            })) Fail(L"The ZIP contains an invalid Windows path.");
        auto device = part.substr(0, part.find(L'.'));
        while (device.ends_with(L' ')) device.pop_back();
        for (auto& ch : device) if (ch >= L'a' && ch <= L'z') ch -= L'a' - L'A';
        if (device == L"CON" || device == L"PRN" || device == L"AUX" || device == L"NUL" ||
            device == L"CONIN$" || device == L"CONOUT$" ||
            (device.size() == 4 && (device.starts_with(L"COM") || device.starts_with(L"LPT")) &&
                std::wstring_view(L"0123456789¹²³").find(device.back()) != std::wstring_view::npos)) {
            Fail(L"The ZIP contains a reserved Windows file name.");
        }
        parts.push_back(std::move(part));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return parts;
}

void Extract(Archive& archive, size_t index, const std::function<void(const char*, size_t)>& write) {
    const auto& entry = archive.entries.at(index - 1);
    if (unzGoToFilePos64(archive.reader, &entry.position) != UNZ_OK || unzOpenCurrentFile(archive.reader) != UNZ_OK) {
        Fail(L"Could not open ZIP entry " + entry.name);
    }
    std::array<char, 65536> buffer{};
    uint64_t total = 0;
    try {
        for (;;) {
            int count = unzReadCurrentFile(archive.reader, buffer.data(), static_cast<unsigned>(buffer.size()));
            if (count < 0) Fail(L"Could not decompress " + entry.name);
            if (!count) break;
            total += count;
            if (total > entry.info.uncompressed_size) Fail(L"ZIP entry exceeds its stated size: " + entry.name);
            write(buffer.data(), count);
        }
    } catch (...) {
        unzCloseCurrentFile(archive.reader);
        throw;
    }
    if (unzCloseCurrentFile(archive.reader) != UNZ_OK || total != entry.info.uncompressed_size) {
        Fail(L"ZIP integrity check failed for " + entry.name);
    }
}

void List(Archive& archive) {
    int result = unzGoToFirstFile(archive.reader);
    for (; result == UNZ_OK; result = unzGoToNextFile(archive.reader)) {
        Archive::Entry entry;
        auto& info = entry.info;
        if (unzGetCurrentFileInfo64(archive.reader, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK ||
            unzGetFilePos64(archive.reader, &entry.position) != UNZ_OK) Fail(L"Could not read the ZIP directory.");
        std::string name(info.size_filename + 1, '\0');
        if (unzGetCurrentFileInfo64(archive.reader, nullptr, name.data(), static_cast<unsigned>(name.size()),
            nullptr, 0, nullptr, 0) != UNZ_OK) Fail(L"Could not read a ZIP entry name.");
        name.resize(info.size_filename);
        entry.name = Utf16(name, (info.flag & (1 << 11)) ? CP_UTF8 : 437);
        entry.directory = entry.name.ends_with(L'/') || entry.name.ends_with(L'\\');
        Components(entry.name);
        if (info.flag & 1 || (info.compression_method != 0 && info.compression_method != Z_DEFLATED)) {
            Fail(L"This ZIP uses encryption or a compression method RainBackup does not support.");
        }
        const auto type = (info.external_fa >> 16) & 0170000;
        if (((info.version >> 8) == 3 && type != 0 && type != 0100000 && type != 0040000) ||
            (info.external_fa & FILE_ATTRIBUTE_REPARSE_POINT)) Fail(L"The ZIP contains a linked or special file.");
        archive.entries.push_back(std::move(entry));
    }
    if (result != UNZ_END_OF_LIST_OF_FILE) Fail(L"Could not read the ZIP directory.");
}

std::wstring RootName(const Source& source) {
    if (source.kind == Kind::Skins) return L"Skins";
    if (source.kind == Kind::Settings || source.kind == Kind::PortableSettings) return L"Settings";
    auto name = source.path.filename().wstring();
    return name.empty() ? L"Files" : name;
}

std::wstring SetupName(const Source& source) {
    if (source.setup.starts_with(L"Portable Rainmeter — ")) {
        auto folder = fs::path(source.setup.substr(std::wstring_view(L"Portable Rainmeter — ").size()));
        auto name = folder.filename().wstring();
        return name.empty() ? L"Portable Rainmeter" : L"Portable Rainmeter - " + name;
    }
    return source.setup;
}

std::wstring SafeComponent(std::wstring name, std::wstring fallback) {
    try {
        auto parts = Components(name);
        if (parts.size() == 1) return parts.front();
    } catch (const std::exception&) {
    }
    return fallback;
}

void UpdateSkinPath(const fs::path& ini, const fs::path& skins) {
    std::ifstream input(ini, std::ios::binary);
    if (!input) Fail(L"Could not read restored Rainmeter.ini.");
    std::string data((std::istreambuf_iterator<char>(input)), {});
    input.close();
    std::wstring contents;
    if (data.starts_with("\xff\xfe") && data.size() % 2 == 0) {
        contents.resize((data.size() - 2) / sizeof(wchar_t));
        memcpy(contents.data(), data.data() + 2, contents.size() * sizeof(wchar_t));
    } else {
        if (data.starts_with("\xef\xbb\xbf")) data.erase(0, 3);
        try { contents = Utf16(data); }
        catch (const std::exception&) { contents = Utf16(data, CP_ACP); }
    }
    std::ofstream output(ini, std::ios::binary | std::ios::trunc);
    output.write("\xff\xfe", 2);
    output.write(reinterpret_cast<const char*>(contents.data()), contents.size() * sizeof(wchar_t));
    output.close();
    if (!output || !WritePrivateProfileStringW(L"Rainmeter", L"SkinPath", skins.c_str(), ini.c_str())) {
        Fail(L"Could not update SkinPath in the restored Rainmeter.ini.");
    }
}

void PreserveOwner(const fs::path& target, const fs::path& temporary) {
    wchar_t volume[MAX_PATH]{};
    DWORD flags = 0;
    if (GetVolumePathNameW(target.c_str(), volume, MAX_PATH) &&
        GetVolumeInformationW(volume, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0) &&
        !(flags & FILE_PERSISTENT_ACLS)) return;
    struct Owner {
        PSID sid = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        ~Owner() { if (descriptor) LocalFree(descriptor); }
        DWORD Read(const fs::path& path) {
            return GetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
                OWNER_SECURITY_INFORMATION, &sid, nullptr, nullptr, nullptr, &descriptor);
        }
    } existing, replacement;
    auto status = existing.Read(target);
    if (status == ERROR_SUCCESS) status = replacement.Read(temporary);
    if (status == ERROR_SUCCESS && !EqualSid(existing.sid, replacement.sid)) {
        status = SetNamedSecurityInfoW(const_cast<wchar_t*>(temporary.c_str()), SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION, existing.sid, nullptr, nullptr, nullptr);
    }
    if (status != ERROR_SUCCESS) FileError(L"Could not preserve the existing file owner.", target, status);
}
}

void WriteZip(const Tree& tree, const fs::path& path, const Transfer& transfer, ZipDisposition disposition) {
    TransferProgress progress(transfer);
    progress.Begin(L"Measuring selected backup items…");
    CheckPath(path);
    std::vector<const Node*> roots;
    std::vector<Source> sources;
    for (const auto& setup : tree.root.children) {
        for (const auto& node : setup->children) {
            if (!Available(*node)) continue;
            if (EqualPath(node->path, path)) Fail(L"The ZIP cannot replace a selected source file.");
            if (node->directory && IsWithin(node->path, path)) Fail(L"Choose a ZIP location outside the selected source folders.");
            roots.push_back(node.get());
            sources.push_back(tree.sources.at(node->source));
        }
    }
    if (roots.empty()) Fail(L"Select at least one folder or file.");
    if (transfer.report) {
        for (const auto* root : roots) progress.total += BackupWork(*root, progress);
    }
    auto metadata = EncodeSources(sources);
    if (metadata.size() > MetadataLimit) Fail(L"The backup has too many source locations.");
    TemporaryFile temporary(path.parent_path());
    zlib_filefunc64_def io{};
    fill_win32_filefunc64W(&io);
    auto zip = zipOpen2_64(temporary.path.c_str(), APPEND_STATUS_CREATE, nullptr, &io);
    if (!zip) Fail(L"Could not create the ZIP.");
    try {
        AddEntry(zip, Metadata, {}, false, progress, metadata);
        for (size_t i = 0; i < roots.size(); ++i) {
            WriteNode(zip, *roots[i], RootKey(i), progress);
        }
    } catch (...) {
        zipClose(zip, nullptr);
        throw;
    }
    if (zipClose(zip, nullptr) != ZIP_OK) Fail(L"Could not finish writing the ZIP.");
    progress.Begin(L"Saving the finished ZIP…");
    CheckPath(path);
    progress.Check();
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (disposition == ZipDisposition::ReplaceExisting) flags |= MOVEFILE_REPLACE_EXISTING;
    if (!MoveFileExW(temporary.path.c_str(), path.c_str(), flags)) {
        FileError(L"Could not save the ZIP.", path, GetLastError());
    }
}

OpenZip ReadZip(const fs::path& path) {
    CheckPath(path);
    OpenZip result{std::make_unique<Tree>(), std::make_shared<Archive>()};
    auto& archive = *result.archive;
    archive.path = path;
    zlib_filefunc64_def io{};
    fill_win32_filefunc64W(&io);
    archive.reader = unzOpen2_64(path.c_str(), &io);
    if (!archive.reader) Fail(L"Could not open the ZIP.");
    List(archive);
    auto& tree = *result.tree;
    std::vector<Node*> roots;
    std::map<std::wstring, Node*> setups;
    std::map<fs::path, Node*, PathLess> nodes;
    for (size_t i = 0; i < archive.entries.size(); ++i) {
        const auto& entry = archive.entries[i];
        if (entry.name != Metadata) continue;
        if (result.originalLocations || entry.info.uncompressed_size > MetadataLimit) Fail(L"Invalid RainBackup metadata.");
        std::string data;
        Extract(archive, i + 1, [&](const char* text, size_t count) { data.append(text, count); });
        tree.sources = DecodeSources(data);
        result.originalLocations = true;
    }
    if (result.originalLocations) {
        for (size_t i = 0; i < tree.sources.size(); ++i) {
            const auto& source = tree.sources[i];
            auto& setup = setups[source.setup];
            if (!setup) setup = &tree.root.Add(source.setup, true);
            auto& root = setup->Add(KindName(source.kind) + L"   •   " + source.path.wstring(), true, i);
            roots.push_back(&root);
            nodes.emplace(fs::path(RootKey(i)).make_preferred(), &root);
        }
    } else {
        tree.sources.push_back({L"ZIP contents", Kind::Extra, {}});
        roots.push_back(&tree.root.Add(L"ZIP contents", true));
    }
    for (size_t i = 0; i < archive.entries.size(); ++i) {
        const auto& entry = archive.entries[i];
        if (entry.name == Metadata) continue;
        auto parts = Components(entry.name);
        Node* node = roots.empty() ? nullptr : roots.front();
        fs::path key;
        size_t first = 0;
        if (result.originalLocations) {
            if (parts.size() < 3 || parts[0] != L"RainBackup" || parts[1] != L"items") Fail(L"Unexpected entry in RainBackup ZIP.");
            key = fs::path(parts[0]) / parts[1] / parts[2];
            auto found = nodes.find(key);
            if (found == nodes.end()) Fail(L"ZIP entry has no matching source location.");
            node = found->second;
            first = 3;
        }
        for (size_t j = first; j < parts.size(); ++j) {
            if (!node->directory) Fail(L"The ZIP contains a file and a folder with the same path.");
            key /= parts[j];
            auto [position, inserted] = nodes.emplace(key, nullptr);
            if (inserted) {
                auto& child = node->Add(parts[j], true, node->source);
                child.path = node->path / parts[j];
                position->second = &child;
            }
            node = position->second;
        }
        if (node->entry || (!entry.directory && !node->children.empty())) Fail(L"The ZIP contains conflicting entries.");
        node->entry = i + 1;
        node->directory = entry.directory;
    }
    if (archive.entries.empty() || (result.originalLocations &&
        (roots.empty() || std::any_of(roots.begin(), roots.end(), [](const Node* node) { return !node->entry; })))) {
        Fail(L"The ZIP has no complete backup tree.");
    }
    tree.root.Count();
    return result;
}

std::vector<fs::path> NewLocations(const OpenZip& zip, const fs::path& folder) {
    if (!zip.originalLocations) return {folder};
    std::set<fs::path, PathLess> used;
    std::vector<fs::path> result;
    for (const auto& source : zip.tree->sources) {
        auto base = folder / SafeComponent(SetupName(source), L"Rainmeter");
        auto name = SafeComponent(RootName(source), L"Files");
        auto path = base / name;
        for (int i = 2; !used.insert(path).second; ++i) path = base / (name + L" (" + std::to_wstring(i) + L")");
        result.push_back(path);
    }
    return result;
}

RestorePlan PlanRestore(const OpenZip& zip, const std::vector<fs::path>& destinations, bool relocate) {
    if (destinations.size() != zip.tree->sources.size()) Fail(L"Choose restore destinations.");
    RestorePlan plan;
    std::map<fs::path, bool, PathLess> seen;
    auto collect = [&](auto&& self, const Node& node, const fs::path& target, const fs::path& skins) -> void {
        if (!Available(node)) return;
        CheckPath(target);
        if (EqualPath(target, zip.archive->path)) Fail(L"A restore cannot overwrite the open backup ZIP.");
        auto [at, inserted] = seen.emplace(target, node.directory);
        if (!inserted && (!at->second || !node.directory)) Fail(L"More than one entry maps to " + target.wstring());
        auto attributes = GetFileAttributesW(target.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && node.directory != ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) {
            Fail(L"A file and folder conflict at " + target.wstring());
        }
        if (!node.directory) {
            ++plan.files;
            if (attributes != INVALID_FILE_ATTRIBUTES) ++plan.overwrites;
        }
        if (inserted) plan.items.push_back({target, node.entry, node.directory,
            !node.directory && EqualPath(node.path, L"Rainmeter.ini") ? skins : fs::path{}});
        for (const auto& child : node.children) self(self, *child, target / child->name, skins);
    };
    auto collectRoot = [&](const Node& node) {
        if (!Available(node)) return;
        const auto& target = destinations.at(node.source);
        if (target.empty()) Fail(L"Choose a destination for " + node.name);
        plan.destinations.push_back(target);
        fs::path skins;
        const auto& source = zip.tree->sources[node.source];
        if (relocate && (source.kind == Kind::Settings || source.kind == Kind::PortableSettings)) {
            for (size_t i = 0; i < destinations.size(); ++i) {
                const auto& candidate = zip.tree->sources[i];
                if (candidate.setup == source.setup && candidate.kind == Kind::Skins && !destinations[i].empty()) {
                    skins = destinations[i];
                    break;
                }
            }
        }
        collect(collect, node, target, skins);
    };
    if (zip.originalLocations) {
        for (const auto& setup : zip.tree->root.children) {
            for (const auto& node : setup->children) collectRoot(*node);
        }
    } else {
        collectRoot(*zip.tree->root.children.front());
    }
    if (plan.items.empty()) Fail(L"Select at least one folder or file to restore.");
    for (auto& item : plan.items) {
        for (auto parent = item.target.parent_path(); !parent.empty() && parent != parent.root_path(); parent = parent.parent_path()) {
            const auto found = seen.find(parent);
            if (found != seen.end() && !found->second) Fail(L"A restored file would block the folder " + parent.wstring());
            DWORD attributes = GetFileAttributesW(parent.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                Fail(L"A file blocks the folder " + parent.wstring());
            }
        }
    }
    return plan;
}

void Restore(const OpenZip& zip, const RestorePlan& plan, const Transfer& transfer) {
    TransferProgress progress(transfer);
    size_t restored = 0;
    try {
        if (transfer.report) {
            for (const auto& item : plan.items) {
                progress.Check();
                progress.total += EntryWork;
                if (!item.directory) progress.total += static_cast<double>(
                    zip.archive->entries.at(item.entry - 1).info.uncompressed_size);
            }
        }
        for (const auto& item : plan.items) {
            progress.Begin(L"Restoring " + item.target.wstring());
            CheckPath(item.target);
            if (item.directory) {
                fs::create_directories(item.target);
                progress.Advance(EntryWork);
                continue;
            }
            fs::create_directories(item.target.parent_path());
            TemporaryFile temporary(item.target.parent_path());
            std::ofstream output(temporary.path, std::ios::binary | std::ios::trunc);
            Extract(*zip.archive, item.entry, [&](const char* data, size_t length) {
                output.write(data, length);
                if (!output) Fail(L"Could not write " + item.target.wstring());
                progress.Advance(length);
            });
            output.close();
            if (!output) Fail(L"Could not finish writing " + item.target.wstring());
            if (!item.skinPath.empty()) UpdateSkinPath(temporary.path, item.skinPath);
            CheckPath(item.target);
            progress.Check();
            auto attributes = GetFileAttributesW(item.target.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                if (!MoveFileExW(temporary.path.c_str(), item.target.c_str(), MOVEFILE_WRITE_THROUGH)) {
                    FileError(L"Could not create the restored file.", item.target, GetLastError());
                }
            } else {
                PreserveOwner(item.target, temporary.path);
                if (!ReplaceFileW(item.target.c_str(), temporary.path.c_str(), nullptr, 0, nullptr, nullptr)) {
                    FileError(L"Could not replace the existing file while preserving its access controls.", item.target, GetLastError());
                }
            }
            ++restored;
            progress.Advance(EntryWork);
        }
    } catch (const Cancelled&) {
        throw Cancelled("Restore canceled. " + std::to_string(restored) + " file(s) had already been restored.");
    } catch (const std::exception& error) {
        Fail(std::to_wstring(restored) + L" file(s) restored before stopping.\n\n" + Utf16(error.what()));
    }
}
}

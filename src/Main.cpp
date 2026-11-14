#include "Archive.h"
#include "CommandLine.h"
#include "RainmeterSession.h"
#include "Schedule.h"
#include "SetupSession.h"

#include <commctrl.h>
#include <chrono>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <algorithm>
#include <map>
#include <thread>

using namespace rb;
namespace {
constexpr UINT Finished = WM_APP + 1, ProgressUpdate = WM_APP + 2;
constexpr int RainBackupIcon = 101;
constexpr int HeaderLogoSize = 64;
constexpr int ProfileRowHeight = 38;
constexpr wchar_t DestinationArrow[] = L"⬆️";
enum Id {
    BackupPage = 100, RestorePage, ScanNow, ScanFolder, AddFolder, AddFile, Change, Remove, ScheduleButton,
    SelectAll, ClearAll, Run, Open, Original, NewLocation, Details, ZipList, TreeId, Destinations,
    ScheduleFolder, Frequency, Time, Copies, SaveTask, RemoveTask, CloseDialog,
    BackupPath, CancelTransfer, DeleteBackup, ShowLog, EmptyLog, MoreMenu, Help, Releases, Support,
    ScheduleSavedFolders, ToggleTooltips, ToggleDarkMode, ProfileChoice, NewProfile, Time12Hour, Time24Hour,
    OpenBackupDestination, SaveSelection
};

std::wstring DisplayTime(std::wstring_view time, bool use24Hour) {
    if (use24Hour || time.size() < 5 || time[2] != L':' ||
        time[0] < L'0' || time[0] > L'2' || time[1] < L'0' || time[1] > L'9') return std::wstring(time);
    const int hour = 10 * (time[0] - L'0') + time[1] - L'0';
    if (hour > 23) return std::wstring(time);
    return std::to_wstring(hour % 12 == 0 ? 12 : hour % 12) +
        std::wstring(time.substr(2)) + (hour < 12 ? L" AM" : L" PM");
}

std::wstring DisplayLogTimes(std::wstring_view text, bool use24Hour) {
    if (use24Hour) return std::wstring(text);
    std::wstring result;
    for (size_t start = 0; start < text.size();) {
        auto end = text.find(L'\n', start);
        if (end == text.npos) end = text.size();
        auto line = text.substr(start, end - start);
        if (line.size() >= 19 && line[4] == L'-' && line[7] == L'-' && line[10] == L' ' &&
            line[16] == L':') {
            result += line.substr(0, 11);
            result += DisplayTime(line.substr(11, 8), false);
            result += line.substr(19);
        } else result += line;
        if (end < text.size()) result += L'\n';
        start = end + 1;
    }
    return result;
}

std::wstring ScheduleTime(std::wstring input, bool use24Hour) {
    if (use24Hour) return input;
    const auto separator = input.find(L':');
    if (separator < 1 || separator > 2 || input.size() != separator + 6 ||
        input[separator + 3] != L' ') Fail(L"Enter a time such as 6:00 PM.");
    const auto suffix = input.substr(separator + 4);
    const bool pm = _wcsicmp(suffix.c_str(), L"PM") == 0;
    if (!pm && _wcsicmp(suffix.c_str(), L"AM") != 0) Fail(L"Enter a time such as 6:00 PM.");
    const auto hours = input.substr(0, separator);
    if (hours.find_first_not_of(L"0123456789") != hours.npos) Fail(L"Enter a time such as 6:00 PM.");
    const int hour = std::stoi(hours);
    if (hour < 1 || hour > 12) Fail(L"Enter a time such as 6:00 PM.");
    const int converted = hour % 12 + (pm ? 12 : 0);
    return (converted < 10 ? L"0" : L"") + std::to_wstring(converted) + input.substr(separator, 3);
}

const wchar_t* ButtonTip(int id, bool restore) {
    switch (id) {
    case BackupPage: return L"Choose the files and folders to back up.";
    case RestorePage: return L"Choose an archive for this backup or open another ZIP.";
    case ScanNow: return L"Detect Rainmeter setups and refresh the source tree.";
    case ScanFolder: return L"Look for portable Rainmeter installations in a folder.";
    case AddFolder: return L"Add another folder to the backup sources.";
    case AddFile: return L"Add another file to the backup sources.";
    case Change: return L"Set a new path for this backup source.";
    case Remove: return L"Remove this source from future backups.";
    case ScheduleButton: return L"Edit this backup's name, destination, and schedule.";
    case NewProfile: return L"Create a named backup with its own sources.";
    case ShowLog: return L"View backup and restore results.";
    case MoreMenu: return L"Open Settings, Help, Releases, and Support.";
    case SelectAll: return L"Check all available folders and files.";
    case ClearAll: return L"Uncheck everything in the tree.";
    case SaveSelection: return L"Save the current file and folder selection for this backup.";
    case Run: return restore ? L"Review the selected files and restore destinations." : L"Review and create a new backup ZIP.";
    case Open: return L"Browse for a backup ZIP, including in saved destinations.";
    case OpenBackupDestination: return L"Open the backup destination in Windows Explorer.";
    case Original: return L"Restore to the paths recorded in the ZIP.";
    case NewLocation: return L"Choose one folder for the restored files.";
    case Details: return L"Show unreadable paths and skipped links from the scan.";
    case ScheduleFolder: return L"Browse for the folder this backup will use.";
    case ScheduleSavedFolders: return L"Choose a saved backup destination.";
    case CancelTransfer: return L"Request cancellation of the current operation.";
    case DeleteBackup: return L"Permanently delete a backup ZIP after confirmation.";
    case SaveTask: return L"Save this named backup.";
    case RemoveTask: return L"Remove this named backup and optionally delete its ZIP files.";
    case EmptyLog: return L"Clear the backup and restore history after confirmation.";
    case CloseDialog: return L"Close this dialog.";
    default: return nullptr;
    }
}

HWND CreateButtonTooltips(HWND owner, HINSTANCE instance) {
    auto tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, owner, nullptr, instance, nullptr);
    if (tooltip) SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, MulDiv(300, GetDpiForWindow(owner), 96));
    return tooltip;
}

void SetButtonTip(HWND tooltip, HWND owner, HWND button, const wchar_t* text, bool update = false) {
    if (!tooltip || !button || !text) return;
    TOOLINFOW tool{};
    tool.cbSize = sizeof(tool);
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = owner;
    tool.uId = reinterpret_cast<UINT_PTR>(button);
    tool.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(tooltip, update ? TTM_UPDATETIPTEXTW : TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
}

std::wstring Text(HWND control) {
    std::wstring result(GetWindowTextLengthW(control) + 1, L'\0');
    result.resize(GetWindowTextW(control, result.data(), static_cast<int>(result.size())));
    return result;
}

bool Confirm(HWND window, const std::wstring& heading, const std::wstring& details = {},
    bool destructive = false, const wchar_t* checkbox = nullptr, BOOL* checked = nullptr) {
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof(dialog);
    dialog.hwndParent = window;
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    dialog.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
    dialog.pszWindowTitle = L"RainBackup";
    dialog.pszMainIcon = destructive ? TD_WARNING_ICON : TD_INFORMATION_ICON;
    dialog.pszMainInstruction = heading.c_str();
    dialog.pszContent = details.empty() ? nullptr : details.c_str();
    dialog.pszVerificationText = checkbox;
    dialog.nDefaultButton = IDNO;
    dialog.cxWidth = 360;
    int choice = IDNO;
    BOOL verification = FALSE;
    if (checked) *checked = FALSE;
    if (SUCCEEDED(TaskDialogIndirect(&dialog, &choice, nullptr, checked ? &verification : nullptr))) {
        if (checked) *checked = choice == IDYES && verification;
        return choice == IDYES;
    }
    const auto fallback = details.empty() ? heading : heading + L"\n\n" + details;
    return MessageBoxW(window, fallback.c_str(), L"RainBackup",
        MB_YESNO | MB_DEFBUTTON2 | (destructive ? MB_ICONWARNING : MB_ICONQUESTION)) == IDYES;
}

void Message(HWND window, const std::wstring& message, UINT icon = MB_ICONINFORMATION) {
    MessageBoxW(window, message.c_str(), L"RainBackup", MB_OK | icon);
}

enum class Picker { Folder, Open };

void CALLBACK OpenBackupPlaces(HWND dialog, UINT, UINT_PTR timer, DWORD) {
    KillTimer(dialog, timer);
    HWND tree = nullptr;
    EnumChildWindows(dialog, [](HWND control, LPARAM context) -> BOOL {
        wchar_t name[32]{};
        GetClassNameW(control, name, static_cast<int>(std::size(name)));
        if (wcscmp(name, WC_TREEVIEWW) != 0 || GetDlgCtrlID(control) != 100) return TRUE;
        *reinterpret_cast<HWND*>(context) = control;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&tree));
    if (!tree) return;

    // The common dialog creates this group from AddPlace entries when it opens.
    for (auto item = TreeView_GetChild(tree, TreeView_GetRoot(tree)); item;
        item = TreeView_GetNextSibling(tree, item)) {
        wchar_t name[32]{};
        TVITEMW info{};
        info.mask = TVIF_TEXT | TVIF_CHILDREN;
        info.hItem = item;
        info.pszText = name;
        info.cchTextMax = static_cast<int>(std::size(name));
        if (TreeView_GetItem(tree, &info) && info.cChildren && wcscmp(name, L"RainBackup") == 0) {
            TreeView_SelectItem(tree, item);
            break;
        }
    }
}

class BackupPlacesEvents final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IFileDialogEvents> {
public:
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog* dialog) override {
        if (!pending) return S_OK;
        Microsoft::WRL::ComPtr<IOleWindow> window;
        HWND handle = nullptr;
        if (SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(window.GetAddressOf()))) &&
            SUCCEEDED(window->GetWindow(&handle)) && handle) {
            pending = false;
            SetTimer(handle, reinterpret_cast<UINT_PTR>(this), USER_TIMER_MINIMUM, OpenBackupPlaces);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE*) override { return S_OK; }
private:
    bool pending = true;
};

fs::path Browse(HWND owner, Picker type, const fs::path& initial = {}, bool zipOnly = false,
    const std::vector<fs::path>& places = {}, bool openBackupZip = false) {
    IFileDialog* dialog = nullptr;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog,
        nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(result)) Fail(L"Could not open the Windows file picker.");
    struct Release { IFileDialog* dialog; ~Release() { dialog->Release(); } } release{dialog};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
        (type == Picker::Folder ? FOS_PICKFOLDERS : 0) |
        (type == Picker::Open ? FOS_FILEMUSTEXIST : 0));
    if (zipOnly) {
        COMDLG_FILTERSPEC filter{L"ZIP archives", L"*.zip"};
        dialog->SetFileTypes(1, &filter);
        dialog->SetDefaultExtension(L"zip");
    }
    for (const auto& folder : places) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            dialog->AddPlace(item, FDAP_BOTTOM);
            item->Release();
        }
    }

    bool startedInFolder = false;
    if (!initial.empty()) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            startedInFolder = SUCCEEDED(dialog->SetFolder(item));
            item->Release();
        }
    }
    DWORD cookie = 0;
    const auto events = openBackupZip && !startedInFolder && !places.empty()
        ? Microsoft::WRL::Make<BackupPlacesEvents>() : Microsoft::WRL::ComPtr<BackupPlacesEvents>{};
    const bool advised = events && SUCCEEDED(dialog->Advise(events.Get(), &cookie));
    result = dialog->Show(owner);
    if (advised) dialog->Unadvise(cookie);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    if (FAILED(result)) Fail(L"The Windows file picker failed.");
    IShellItem* item = nullptr;
    if (FAILED(dialog->GetResult(&item))) Fail(L"Could not read the chosen location.");
    wchar_t* value = nullptr;
    result = item->GetDisplayName(SIGDN_FILESYSPATH, &value);
    item->Release();
    if (FAILED(result)) Fail(L"The selected location is unavailable.");
    fs::path path(value);
    CoTaskMemFree(value);
    return path;
}

struct Palette {
    COLORREF bg{}, panel{}, text{}, quiet{}, border{}, accent{}, onAccent{}, hover{};
    HBRUSH background = nullptr, surface = nullptr;
    bool dark = false;
    ~Palette() {
        DeleteObject(background);
        DeleteObject(surface);
    }
    void Load(HWND window, std::optional<bool> preference) {
        DWORD light = 1, size = sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
        HIGHCONTRASTW contrast{};
        contrast.cbSize = sizeof(contrast);
        SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
        dark = preference.value_or(light == 0) && !(contrast.dwFlags & HCF_HIGHCONTRASTON);
        bg = dark ? RGB(25, 30, 39) : RGB(241, 245, 249);
        panel = dark ? RGB(33, 40, 52) : RGB(255, 255, 255);
        text = dark ? RGB(232, 239, 250) : RGB(27, 42, 65);
        quiet = dark ? RGB(163, 179, 200) : RGB(77, 98, 123);
        border = dark ? RGB(65, 79, 98) : RGB(203, 215, 230);
        accent = dark ? RGB(116, 192, 255) : RGB(16, 95, 166);
        onAccent = dark ? RGB(12, 30, 46) : RGB(255, 255, 255);
        hover = dark ? RGB(50, 62, 80) : RGB(225, 237, 248);
        if (contrast.dwFlags & HCF_HIGHCONTRASTON) {
            bg = panel = GetSysColor(COLOR_WINDOW);
            text = quiet = border = GetSysColor(COLOR_WINDOWTEXT);
            accent = hover = GetSysColor(COLOR_HIGHLIGHT);
            onAccent = GetSysColor(COLOR_HIGHLIGHTTEXT);
        }
        DeleteObject(background);
        DeleteObject(surface);
        background = CreateSolidBrush(bg);
        surface = CreateSolidBrush(panel);
        BOOL immersive = dark;
        DwmSetWindowAttribute(window, 20, &immersive, sizeof(immersive));
    }
};

void Box(HDC dc, RECT r, int radius, COLORREF fill, COLORREF edge) {
    auto brush = CreateSolidBrush(fill);
    auto pen = CreatePen(PS_SOLID, 1, edge);
    auto oldBrush = SelectObject(dc, brush);
    auto oldPen = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

struct JobResult {
    std::function<void()> done;
    std::wstring error;
    bool cancelled = false;
    bool logFailed = false;
};

struct ProgressMessage {
    std::wstring text;
    int percent;
};

struct App {
    HWND window{}, tree{}, title{}, sub{}, detail{}, status{}, summary{}, zipList{}, destinations{}, mode{},
        backupPath{}, backupPathLabel{}, progressBar{}, percentLabel{};
    HFONT regular{}, heading{}, section{}, profileFont{};
    HIMAGELIST checks{}, icons{};
    HINSTANCE instance{};
    HWND tooltips{};
    bool showTooltips = true;
    std::optional<bool> darkMode;
    bool use24Hour = true;
    HICON headerIcon{};
    RECT backupField{};
    Palette palette;
    int dpi = 96;
    bool restorePage = false, busy = false, original = false, updating = false;
    bool transferring = false, loadingZip = false;
    std::stop_source stopTransfer;
    std::unique_ptr<Tree> backup;
    std::vector<Source> sources;
    std::vector<fs::path> ignored;
    OpenZip opened;
    fs::path currentZip, lastFolder, restoreFolder;
    std::vector<BackupProfile> profiles;
    size_t activeProfile = 0;
    HWND profileChoice{};
    bool showProfileZips = true;
    std::vector<fs::path> savedFolders;
    std::vector<fs::path> zips, targets;
    std::map<int, HWND> buttons;
    std::map<Node*, HTREEITEM> items;
    std::thread worker;
    std::wstring notice;

    ~App() {
        if (worker.joinable()) worker.join();
        if (headerIcon) DestroyIcon(headerIcon);
        for (auto font : {regular, heading, section, profileFont}) DeleteObject(font);
        ImageList_Destroy(checks);
        ImageList_Destroy(icons);
    }

    int Px(int value) const { return MulDiv(value, dpi, 96); }
    void LoadHeaderIcon() {
        if (headerIcon) DestroyIcon(headerIcon);
        headerIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(RainBackupIcon),
            IMAGE_ICON, Px(HeaderLogoSize), Px(HeaderLogoSize), 0));
    }
    HWND Control(const wchar_t* type, const wchar_t* text, DWORD style, int id = 0) const {
        auto control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(regular), FALSE);
        return control;
    }

    void Fonts() {
        for (auto font : {regular, heading, section, profileFont}) DeleteObject(font);
        auto font = [&](int size, int weight) {
            return CreateFontW(-Px(size), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        };
        regular = font(14, FW_NORMAL);
        heading = font(32, FW_SEMIBOLD);
        section = font(16, FW_SEMIBOLD);
        profileFont = font(18, FW_NORMAL);
        for (HWND c = GetWindow(window, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            auto use = c == title ? heading : c == summary ? section : c == profileChoice ? profileFont : regular;
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(use), TRUE);
        }
        if (profileChoice) {
            RECT bounds{};
            const int fieldHeight = static_cast<int>(SendMessageW(profileChoice, CB_GETITEMHEIGHT, static_cast<WPARAM>(-1), 0));
            if (GetWindowRect(profileChoice, &bounds) && fieldHeight > 0) {
                const int borderHeight = bounds.bottom - bounds.top - fieldHeight;
                ComboBox_SetItemHeight(profileChoice, -1, Px(ProfileRowHeight) - borderHeight - 1);
            }
            ComboBox_SetItemHeight(profileChoice, 0, Px(28));
        }
    }

    void Theme() {
        palette.Load(window, darkMode);
        SetWindowTheme(tree, palette.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        SetWindowTheme(destinations, palette.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        TreeView_SetBkColor(tree, palette.panel);
        TreeView_SetTextColor(tree, palette.text);
        ListView_SetBkColor(destinations, palette.panel);
        ListView_SetTextBkColor(destinations, palette.panel);
        ListView_SetTextColor(destinations, palette.text);
        TreeView_SetItemHeight(tree, Px(28));
        auto state = ImageList_Create(Px(19), Px(19), ILC_COLOR24, 4, 0);
        auto symbols = ImageList_Create(Px(20), Px(19), ILC_COLOR24, 2, 0);
        HDC screen = GetDC(window);
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, Px(20), Px(19));
        auto old = SelectObject(dc, bitmap);
        auto line = [&](COLORREF color, int x, int y, int toX, int toY, int width) {
            auto pen = CreatePen(PS_SOLID, Px(width), color);
            auto previous = SelectObject(dc, pen);
            MoveToEx(dc, Px(x), Px(y), nullptr);
            LineTo(dc, Px(toX), Px(toY));
            SelectObject(dc, previous);
            DeleteObject(pen);
        };
        RECT rect{0, 0, Px(20), Px(19)};
        for (int i = -1; i < 3; ++i) {
            FillRect(dc, &rect, palette.surface);
            if (i >= 0) Box(dc, {Px(2), Px(2), Px(17), Px(17)}, Px(5), i ? palette.accent : palette.panel,
                i ? palette.accent : palette.quiet);
            if (i == 1) {
                line(palette.onAccent, 5, 9, 8, 12, 2);
                line(palette.onAccent, 8, 12, 14, 6, 2);
            }
            if (i == 2) line(palette.onAccent, 5, 9, 14, 9, 2);
            auto checkbox = CreateCompatibleBitmap(screen, Px(19), Px(19));
            SelectObject(dc, checkbox);
            HDC copy = CreateCompatibleDC(screen);
            auto previous = SelectObject(copy, bitmap);
            BitBlt(dc, 0, 0, Px(19), Px(19), copy, 0, 0, SRCCOPY);
            SelectObject(copy, previous);
            DeleteDC(copy);
            SelectObject(dc, bitmap);
            ImageList_Add(state, checkbox, nullptr);
            DeleteObject(checkbox);
        }
        for (int i = 0; i < 2; ++i) {
            FillRect(dc, &rect, palette.surface);
            if (i == 0) {
                Box(dc, {Px(2), Px(4), Px(11), Px(12)}, Px(3), palette.hover, palette.accent);
                Box(dc, {Px(2), Px(7), Px(18), Px(16)}, Px(3), palette.hover, palette.accent);
            } else {
                Box(dc, {Px(5), Px(2), Px(15), Px(17)}, Px(3), palette.panel, palette.quiet);
                line(palette.quiet, 7, 8, 13, 8, 1);
                line(palette.quiet, 7, 11, 13, 11, 1);
            }
            ImageList_Add(symbols, bitmap, nullptr);
        }
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
        ReleaseDC(window, screen);
        TreeView_SetImageList(tree, state, TVSIL_STATE);
        TreeView_SetImageList(tree, symbols, TVSIL_NORMAL);
        ImageList_Destroy(checks);
        ImageList_Destroy(icons);
        checks = state;
        icons = symbols;
        InvalidateRect(window, nullptr, TRUE);
    }

    void Init() {
        dpi = static_cast<int>(GetDpiForWindow(window));
        LoadHeaderIcon();
        Fonts();
        title = Control(L"STATIC", L"RainBackup", SS_NOPREFIX);
        sub = Control(L"STATIC", L"Backup and restore your Rainmeter setups", SS_NOPREFIX);
        summary = Control(L"STATIC", L"", SS_NOPREFIX);
        detail = Control(L"STATIC", L"", SS_PATHELLIPSIS | SS_NOPREFIX);
        status = Control(L"STATIC", L"", SS_PATHELLIPSIS | SS_NOPREFIX);
        progressBar = Control(PROGRESS_CLASSW, L"", PBS_SMOOTH);
        percentLabel = Control(L"STATIC", L"", SS_RIGHT | SS_NOPREFIX);
        SendMessageW(progressBar, PBM_SETRANGE32, 0, 100);
        mode = Control(L"STATIC", L"", SS_NOPREFIX | SS_PATHELLIPSIS);
        backupPathLabel = Control(L"STATIC", L"Backup destination", SS_NOPREFIX);
        savedFolders = LoadBackupDestinations();
        lastFolder = LoadBackupFolder();
        if (lastFolder.empty()) lastFolder = KnownFolder(FOLDERID_Documents);
        profiles = LoadBackupProfiles(lastFolder);
        profileChoice = Control(WC_COMBOBOXW, L"",
            WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, ProfileChoice);
        RefreshProfiles();
        backupPath = Control(L"EDIT", L"",
            WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY, BackupPath);
        SendMessageW(backupPath, EM_SETLIMITTEXT, 32767, 0);
        tree = Control(WC_TREEVIEWW, L"", WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
            TVS_SHOWSELALWAYS | TVS_DISABLEDRAGDROP | TVS_INFOTIP, TreeId);
        TreeView_SetExtendedStyle(tree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
        destinations = Control(WC_LISTVIEWW, L"", LVS_REPORT, Destinations);
        ListView_SetExtendedListViewStyle(destinations, LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
        const wchar_t* destinationColumns[] = {L"Backup type", L"Original location", L"Restore destination"};
        for (int i = 0; i < static_cast<int>(std::size(destinationColumns)); ++i) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT;
            col.pszText = const_cast<wchar_t*>(destinationColumns[i]);
            ListView_InsertColumn(destinations, i, &col);
        }
        zipList = Control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, ZipList);
        const std::pair<int, const wchar_t*> labels[] = {
            {BackupPage,L"&Backup"}, {RestorePage,L"&Restore"}, {ScanNow,L"Scan"}, {ScanFolder,L"Scan folder…"},
            {AddFolder,L"Add folder…"}, {AddFile,L"Add file…"}, {Change,L"Change path…"}, {Remove,L"Remove folder"},
            {ShowLog,L"Log"}, {MoreMenu,L"More options"}, {SelectAll,L"Select all"}, {ClearAll,L"Clear selection"}, {Run,L"Back up now…"},
            {Open,L"Open ZIP"}, {Original,L"Original location"},
            {NewLocation,L"New location"}, {Details,L"Scan issues…"},
            {NewProfile,L"New"}, {ScheduleButton,L"Edit"}, {CancelTransfer,L"Cancel"}, {DeleteBackup,L"Delete ZIP"},
            {OpenBackupDestination,DestinationArrow}, {SaveSelection,L"Save"}
        };
        showTooltips = LoadButtonTooltips();
        darkMode = LoadDarkMode();
        use24Hour = Load24HourTime();
        tooltips = CreateButtonTooltips(window, instance);
        for (const auto& [id, text] : labels) {
            buttons[id] = Control(L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, id);
            SetButtonTip(tooltips, window, buttons[id], ButtonTip(id, false));
        }
        SendMessageW(tooltips, TTM_ACTIVATE, showTooltips, 0);
        Fonts();
        Theme();
        Refresh();
        SelectProfile(0, true);
    }

    void Layout() {
        RECT r{};
        GetClientRect(window, &r);
        const int width = MulDiv(r.right, 96, dpi), height = MulDiv(r.bottom, 96, dpi);
        auto place = [&](HWND control, int x, int y, int w, int h) {
            MoveWindow(control, Px(x), Px(y), Px(w), Px(h), TRUE);
        };
        auto button = [&](int id, int x, int y, int w, int h = 34) { place(buttons.at(id), x, y, w, h); };
        place(title, 108, 20, width - 280, 42);
        place(sub, 110, 64, width - 282, 24);
        button(ShowLog, width - 160, 36, 80, 38);
        button(MoreMenu, width - 72, 36, 42, 38);
        button(BackupPage, 30, 101, 119, ProfileRowHeight);
        button(RestorePage, 157, 101, 119, ProfileRowHeight);
        place(profileChoice, 294, 101, width - (restorePage ? 324 : 484), 265);
        button(NewProfile, width - 182, 101, 72, ProfileRowHeight);
        button(ScheduleButton, width - 102, 101, 72, ProfileRowHeight);
        int y = 159;
        if (restorePage) {
            button(Open, 30, y, 110);
            place(zipList, 150, y + 4, width - 300, 230);
            button(DeleteBackup, width - 140, y, 110);
        } else {
            int x = 30;
            for (auto [id, w] : {std::pair{ScanNow,72}, {ScanFolder,112}, {AddFolder,108},
                {AddFile,91}, {Change,123}, {Remove,103}}) {
                button(id, x, y, w);
                x += w + 8;
            }
        }
        y += 50;
        const int saveWidth = restorePage ? 0 : 80;
        place(summary, 30, y + 5, width - 350 - saveWidth, 27);
        button(SelectAll, width - 252 - saveWidth, y, 95, 31);
        button(ClearAll, width - 149 - saveWidth, y, 119, 31);
        if (!restorePage) button(SaveSelection, width - 102, y, 72, 31);
        int lower = height - (restorePage ? 256 : 151);
        place(tree, 30, y + 42, width - 60, std::max(80, lower - y - 42));
        if (restorePage) {
            place(mode, 30, lower + 10, width - 60, 23);
            button(Original, 30, lower + 39, 132, 32);
            button(NewLocation, 170, lower + 39, 132, 32);
            place(destinations, 30, lower + 78, width - 60, 107);
            const int typeWidth = Px(154);
            const int pathWidth = Px(width - 60) - typeWidth - GetSystemMetricsForDpi(SM_CXVSCROLL, dpi);
            ListView_SetColumnWidth(destinations, 0, typeWidth);
            ListView_SetColumnWidth(destinations, 1, pathWidth / 2);
            ListView_SetColumnWidth(destinations, 2, pathWidth - pathWidth / 2);
        } else {
            place(detail, 30, lower + 7, width - 195, 25);
            button(Details, width - 149, lower + 2, 119, 30);
            button(OpenBackupDestination, 30, lower + 42, 27, 29);
            place(backupPathLabel, 64, lower + 47, 130, 25);
            backupField = {Px(196), Px(lower + 43), Px(width - 30), Px(lower + 72)};
            HDC dc = GetDC(backupPath);
            auto previous = SelectObject(dc, regular);
            TEXTMETRICW metrics{};
            GetTextMetricsW(dc, &metrics);
            SelectObject(dc, previous);
            ReleaseDC(backupPath, dc);
            const int fieldHeight = backupField.bottom - backupField.top;
            const int textHeight = std::clamp<int>(metrics.tmHeight + 1, 1, fieldHeight - 2);
            MoveWindow(backupPath, backupField.left + Px(5), backupField.top + (fieldHeight - textHeight) / 2,
                backupField.right - backupField.left - Px(10), textHeight, TRUE);
        }
        InvalidateRect(window, nullptr, TRUE);
        place(status, 30, height - (transferring ? 57 : 46), width - 242, 21);
        place(progressBar, 30, height - 31, width - 291, 13);
        place(percentLabel, width - 253, height - 35, 43, 22);
        button(Run, width - 195, height - 57, 165, 38);
        button(CancelTransfer, width - 195, height - 57, 165, 38);
    }

    Tree* Active() const { return restorePage ? opened.tree.get() : backup.get(); }

    void Refresh() {
        if (!tree) return;
        const bool has = Active() && !Active()->root.children.empty();
        for (auto id : {ScanNow, ScanFolder, AddFolder, AddFile, Change, Remove, Details, NewProfile,
            ScheduleButton, OpenBackupDestination, SaveSelection}) {
            ShowWindow(buttons[id], restorePage ? SW_HIDE : SW_SHOW);
        }
        for (auto id : {Open, Original, NewLocation, DeleteBackup}) {
            ShowWindow(buttons[id], restorePage ? SW_SHOW : SW_HIDE);
        }
        ShowWindow(zipList, restorePage ? SW_SHOW : SW_HIDE);
        ShowWindow(tree, loadingZip ? SW_HIDE : SW_SHOW);
        for (auto control : {mode, destinations}) {
            ShowWindow(control, restorePage && !loadingZip ? SW_SHOW : SW_HIDE);
        }
        ShowWindow(detail, restorePage ? SW_HIDE : SW_SHOW);
        ShowWindow(backupPathLabel, restorePage ? SW_HIDE : SW_SHOW);
        ShowWindow(backupPath, restorePage ? SW_HIDE : SW_SHOW);
        ShowWindow(progressBar, transferring ? SW_SHOW : SW_HIDE);
        ShowWindow(percentLabel, transferring ? SW_SHOW : SW_HIDE);
        ShowWindow(buttons[CancelTransfer], transferring ? SW_SHOW : SW_HIDE);
        ShowWindow(buttons[Run], transferring ? SW_HIDE : SW_SHOW);
        const bool hasProfile = !profiles.empty();
        EnableWindow(backupPath, !busy && hasProfile);
        EnableWindow(profileChoice, !busy && !loadingZip && hasProfile);
        auto selectedSource = SelectedSource();
        SetWindowTextW(buttons[Remove], selectedSource && !selectedSource->directory ? L"Remove file" : L"Remove folder");
        for (auto& [id, control] : buttons) {
            bool enabled = !busy && !loadingZip;
            if (id == ScheduleButton || id == ScanNow || id == ScanFolder || id == AddFolder || id == AddFile) {
                enabled &= hasProfile;
            }
            if (id == OpenBackupDestination) enabled &= hasProfile && !profiles.at(activeProfile).schedule.folder.empty();
            if (id == CancelTransfer) enabled = transferring && !stopTransfer.stop_requested();
            if (id == SelectAll || id == ClearAll) enabled &= has;
            if (id == SaveSelection) enabled &= hasProfile && backup != nullptr;
            if (id == Run) enabled &= has && Active()->root.checked != 0;
            if (id == Details) enabled &= backup && !backup->warnings.empty();
            if (id == Change) enabled &= Selected() && !Selected()->path.empty();
            if (id == Remove) enabled &= selectedSource != nullptr;
            if (id == Original) enabled &= opened.originalLocations;
            if (id == NewLocation) enabled &= opened.tree != nullptr;
            if (id == Run && restorePage) enabled &= original || !restoreFolder.empty();
            EnableWindow(control, enabled);
            InvalidateRect(control, nullptr, TRUE);
        }
        EnableWindow(tree, !busy && !loadingZip);
        EnableWindow(destinations, !busy && !loadingZip);
        EnableWindow(zipList, !busy && !loadingZip && !zips.empty());
        SetWindowTextW(buttons[Run], restorePage ? L"Restore selected…" : L"Back up now…");
        SetButtonTip(tooltips, window, buttons[Run], ButtonTip(Run, restorePage), true);
        SetWindowTextW(summary, restorePage ? L"Archive contents" : L"Rainmeter setups");
        const std::wstring restoreMode = !opened.tree ? L"" :
            original ? L"Restore to original locations" :
            restoreFolder.empty() ? L"Choose a restore folder." : L"Restore under " + restoreFolder.wstring();
        SetWindowTextW(mode, restoreMode.c_str());
        if (!busy && !loadingZip) {
            std::wstring label = notice;
            if (label.empty() && has) label = std::to_wstring(Active()->root.checked) + L" of " +
                std::to_wstring(Active()->root.total) + L" items selected";
            if (label.empty()) label = restorePage ? L"Choose a backup ZIP to see what it contains." :
                hasProfile ? L"Scan or add a source to start." : L"Click New beside the backup name to get started.";
            if (!use24Hour && label.starts_with(L"Last scheduled backup ") && label.size() >= 8 &&
                label[label.size() - 6] == L':' && label[label.size() - 3] == L':') {
                label.replace(label.size() - 8, 8, DisplayTime(std::wstring_view(label).substr(label.size() - 8), false));
            }
            SetWindowTextW(status, label.c_str());
        }
        Layout();
    }

    Node* Selected() const {
        TVITEMW item{};
        item.hItem = TreeView_GetSelection(tree);
        item.mask = TVIF_PARAM;
        return item.hItem && TreeView_GetItem(tree, &item) ? reinterpret_cast<Node*>(item.lParam) : nullptr;
    }

    Node* SelectedRoot() const {
        auto node = Selected();
        if (!node || node->path.empty()) return nullptr;
        while (node->parent && !node->parent->path.empty()) node = node->parent;
        return node;
    }

    Node* SelectedSource() const {
        if (restorePage || !backup) return nullptr;
        auto node = Selected();
        return node && node->parent && node->parent->parent == &backup->root ? node : nullptr;
    }

    void UpdateCheck(Node& node) {
        auto found = items.find(&node);
        if (found != items.end()) {
            TVITEMW item{};
            item.mask = TVIF_STATE;
            item.hItem = found->second;
            item.stateMask = TVIS_STATEIMAGEMASK;
            item.state = INDEXTOSTATEIMAGEMASK(node.checked == 0 ? 1 : node.checked == node.total ? 2 : 3);
            TreeView_SetItem(tree, &item);
        }
    }

    void AddChildren(Node& node, HTREEITEM parent) {
        if (parent != TVI_ROOT && TreeView_GetChild(tree, parent)) return;
        for (auto child = node.children.rbegin(); child != node.children.rend(); ++child) {
            auto& item = **child;
            TVINSERTSTRUCTW insertion{};
            insertion.hParent = parent;
            insertion.hInsertAfter = TVI_FIRST;
            insertion.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_CHILDREN;
            insertion.item.pszText = item.name.data();
            insertion.item.lParam = reinterpret_cast<LPARAM>(&item);
            insertion.item.iImage = insertion.item.iSelectedImage = item.directory ? 0 : 1;
            insertion.item.cChildren = static_cast<int>(!item.children.empty());
            items.emplace(&item, TreeView_InsertItem(tree, &insertion));
            UpdateCheck(item);
        }
    }

    void Populate() {
        // WM_SETREDRAW(TRUE) makes a hidden window visible.
        if (!loadingZip) SendMessageW(tree, WM_SETREDRAW, FALSE, 0);
        updating = true;
        TreeView_DeleteAllItems(tree);
        items.clear();
        if (auto model = Active()) {
            AddChildren(model->root, TVI_ROOT);
            for (const auto& setup : model->root.children) {
                auto row = items.at(setup.get());
                AddChildren(*setup, row);
                TreeView_Expand(tree, row, TVE_EXPAND);
            }
        }
        updating = false;
        if (!loadingZip) SendMessageW(tree, WM_SETREDRAW, TRUE, 0);
        ScrollTreeToTop();
        InvalidateRect(tree, nullptr, TRUE);
        SetWindowTextW(detail, !backup || backup->warnings.empty() ? L"Select folders or expand them to choose individual files." :
            (std::to_wstring(backup->warnings.size()) + L" scan notices. Review before backing up.").c_str());
        FillDestinations();
        Refresh();
    }

    void ScrollTreeToTop() const {
        if (auto first = TreeView_GetRoot(tree)) TreeView_SelectSetFirstVisible(tree, first);
    }

    void FillDestinations() {
        ListView_DeleteAllItems(destinations);
        if (!opened.tree) return;
        for (size_t i = 0; i < opened.tree->sources.size(); ++i) {
            const auto& source = opened.tree->sources[i];
            auto label = source.path.empty() ? L"ZIP contents" : KindName(source.kind);
            LVITEMW row{};
            row.mask = LVIF_TEXT;
            row.iItem = static_cast<int>(i);
            row.pszText = label.data();
            auto at = ListView_InsertItem(destinations, &row);
            auto value = i < targets.size() && !targets[i].empty() ? targets[i].wstring() : L"Choose a destination";
            auto originalPath = source.path.wstring();
            ListView_SetItemText(destinations, at, 1, originalPath.data());
            ListView_SetItemText(destinations, at, 2, value.data());
        }
    }

    void RecordSelection() {
        if (restorePage || !backup) return;
        std::vector<fs::path> fresh;
        auto contains = [&](auto&& self, const Node& node, const fs::path& path) -> bool {
            if (!node.path.empty() && EqualPath(node.path, path)) return true;
            return std::any_of(node.children.begin(), node.children.end(),
                [&](const auto& child) { return self(self, *child, path); });
        };
        for (const auto& path : ignored) {
            if (!contains(contains, backup->root, path)) fresh.push_back(path);
        }
        auto collect = [&](auto&& self, const Node& node) -> void {
            if (!node.path.empty() && node.total && !node.checked && !node.selected) {
                fresh.push_back(node.path);
                return;
            }
            for (const auto& child : node.children) self(self, *child);
        };
        collect(collect, backup->root);
        auto profile = profiles.at(activeProfile);
        profile.excluded = fresh;
        SaveBackupProfile(profile);
        profiles[activeProfile] = std::move(profile);
        ignored = std::move(fresh);
    }

    void Toggle(Node* node, bool value) {
        if (busy || !node || !node->total) return;
        node->Select(value);
        for (auto ancestor = node->parent; ancestor; ancestor = ancestor->parent) ancestor->selected = true;
        Active()->root.Count();
        for (auto& [item, row] : items) UpdateCheck(*item);
        notice.clear();
        RecordSelection();
        Refresh();
    }

    template<class Work>
    void Start(std::wstring label, Work work, std::wstring operation = {}, fs::path location = {}) {
        if (busy) return;
        busy = true;
        transferring = !operation.empty();
        stopTransfer = std::stop_source{};
        if (transferring) {
            SendMessageW(progressBar, PBM_SETPOS, 0, 0);
            SetWindowTextW(percentLabel, L"0%");
        }
        SetWindowTextW(status, label.c_str());
        Refresh();
        worker = std::thread([this, work = std::move(work), stop = stopTransfer.get_token(),
            operation = std::move(operation), location = std::move(location)]() mutable {
            auto publish = [this, last = ULONGLONG{}](const std::wstring& text, int percent) mutable {
                auto now = GetTickCount64();
                if (now - last < 120) return;
                last = now;
                auto message = std::make_unique<ProgressMessage>(ProgressMessage{text, percent});
                if (PostMessageW(window, ProgressUpdate, 0, reinterpret_cast<LPARAM>(message.get()))) message.release();
            };
            auto result = std::make_unique<JobResult>();
            try {
                const Progress update = [&](std::wstring text) { publish(text, -1); };
                if constexpr (std::is_invocable_v<Work, const Progress&, const Transfer&, fs::path&>) {
                    result->done = work(update, Transfer{publish, stop}, location);
                } else if constexpr (std::is_invocable_v<Work, const Progress&, const Transfer&>) {
                    result->done = work(update, Transfer{publish, stop});
                } else {
                    result->done = work(update);
                }
            }
            catch (const Cancelled& error) {
                result->cancelled = true;
                result->error = Utf16(error.what());
            }
            catch (const std::exception& error) { result->error = Utf16(error.what(), CP_ACP); try { result->error = Utf16(error.what()); } catch (...) {} }
            if (!operation.empty()) {
                auto detail = location.wstring();
                if (!result->error.empty()) detail += L"\n" + result->error;
                try {
                    LogOperation(operation, result->cancelled ? L"Canceled" :
                        result->error.empty() ? L"Success" : L"Failure", detail,
                        result->error.empty() || operation == L"Restore" ? location : fs::path{});
                } catch (const std::exception&) {
                    result->logFailed = true;
                }
            }
            if (PostMessageW(window, Finished, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
        });
    }

    void Adopt(std::unique_ptr<Tree> model, std::wstring message) {
        auto profile = profiles.at(activeProfile);
        profile.sources = model->sources;
        if (profile.sources.size() != sources.size() ||
            !std::equal(profile.sources.begin(), profile.sources.end(), sources.begin(),
                [](const Source& a, const Source& b) {
                    return a.setup == b.setup && a.kind == b.kind && EqualPath(a.path, b.path);
                })) {
            SaveBackupProfile(profile);
            profiles[activeProfile] = std::move(profile);
        }
        sources = model->sources;
        backup = std::move(model);
        notice = std::move(message);
        Populate();
    }

    void ScanSources(fs::path folder, bool initial = false) {
        Start(L"Scanning Rainmeter…", [this, folder = std::move(folder), initial](const Progress& progress) {
            auto list = sources;
            auto exclusions = ignored;
            auto found = initial ? std::vector<Source>{} : Scan(folder);
            size_t added = 0;
            for (auto& entry : found) {
                // Respect an edited root for a known setup and content type.
                bool configured = std::any_of(list.begin(), list.end(), [&](const Source& source) {
                    return source.setup == entry.setup && source.kind == entry.kind;
                });
                if (!configured && Merge(list, std::move(entry))) ++added;
            }
            auto model = Build(std::move(list), progress);
            ApplyIgnored(*model, exclusions);
            auto holder = std::make_shared<std::unique_ptr<Tree>>(std::move(model));
            std::wstring startupNotice;
            if (initial) {
                try { startupNotice = LastScheduledBackup(); }
                catch (const std::exception&) {}
            }
            return [this, holder, exclusions = std::move(exclusions), added, initial, folder,
                startupNotice = std::move(startupNotice)] {
                ignored = exclusions;
                Adopt(std::move(*holder), !initial && !folder.empty() && !added
                    ? L"No new portable Rainmeter setups found." : startupNotice);
            };
        });
    }

    void Rebuild(std::vector<Source> list) {
        Start(L"Reading backup sources…", [this, list = std::move(list)](const Progress& progress) mutable {
            auto model = Build(std::move(list), progress);
            ApplyIgnored(*model, ignored);
            auto holder = std::make_shared<std::unique_ptr<Tree>>(std::move(model));
            return [this, holder] { Adopt(std::move(*holder), L""); };
        });
    }

    void RefreshProfiles() {
        SendMessageW(profileChoice, CB_RESETCONTENT, 0, 0);
        if (profiles.empty()) {
            SendMessageW(profileChoice, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"No named backups"));
        }
        for (const auto& profile : profiles) {
            SendMessageW(profileChoice, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(profile.name.c_str()));
        }
        SendMessageW(profileChoice, CB_SETCURSEL, activeProfile, 0);
    }

    void SelectProfile(size_t index, bool startup = false) {
        activeProfile = index;
        const auto* profile = profiles.empty() ? nullptr : &profiles.at(index);
        SetWindowTextW(backupPath, profile ? profile->schedule.folder.c_str() : L"");
        sources = profile ? profile->sources : std::vector<Source>{};
        ignored = profile ? profile->excluded : std::vector<fs::path>{};
        backup.reset();
        ShowProfileArchives();
        Populate();
        if (!profile) return;
        if (startup) ScanSources({}, true);
        else Rebuild(sources);
    }

    void ShowProfileArchives() {
        opened = {};
        currentZip.clear();
        targets.clear();
        original = false;
        restoreFolder.clear();
        showProfileZips = true;
        PopulateZips(profiles.empty() ? fs::path{} : profiles.at(activeProfile).schedule.folder, {});
    }

    void PopulateZips(const fs::path& folder, const fs::path& selected) {
        zips.clear();
        std::error_code error;
        for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
            if (EqualPath(it->path().extension(), L".zip") && it->is_regular_file(error) &&
                (!showProfileZips || (!profiles.empty() && IsProfileArchive(it->path(), profiles.at(activeProfile).schedule.id)))) {
                zips.push_back(it->path());
            }
            error.clear();
        }
        if (!selected.empty() && fs::is_regular_file(selected, error) &&
            std::none_of(zips.begin(), zips.end(), [&](const auto& item) { return EqualPath(item, selected); })) {
            zips.push_back(selected);
        }
        std::map<fs::path, fs::file_time_type> modified;
        for (const auto& path : zips) modified.emplace(path, fs::last_write_time(path, error));
        std::sort(zips.begin(), zips.end(), [&](const auto& a, const auto& b) {
            return modified.at(a) == modified.at(b) ? a > b : modified.at(a) > modified.at(b);
        });
        SendMessageW(zipList, CB_RESETCONTENT, 0, 0);
        for (size_t i = 0; i < zips.size(); ++i) {
            auto label = zips[i].filename().wstring();
            if (showProfileZips && modified.at(zips[i]) != fs::file_time_type::min()) {
                const auto stamp = std::chrono::system_clock::to_time_t(
                    std::chrono::clock_cast<std::chrono::system_clock>(modified.at(zips[i])));
                tm date{};
                wchar_t formatted[64]{};
                if (localtime_s(&date, &stamp) == 0 && wcsftime(formatted, std::size(formatted),
                    use24Hour ? L"%Y-%m-%d  %H:%M:%S" : L"%Y-%m-%d  %I:%M:%S %p", &date)) {
                    label = formatted;
                }
            }
            SendMessageW(zipList, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (EqualPath(zips[i], selected)) SendMessageW(zipList, CB_SETCURSEL, i, 0);
        }
    }

    void LoadZip(fs::path path, bool allZips = false) {
        loadingZip = true;
        Start(L"Reading ZIP contents…", [this, path = std::move(path), allZips](const Progress&) {
            auto zip = std::make_shared<OpenZip>(ReadZip(path));
            return [this, path, zip, allZips] {
                opened = std::move(*zip);
                currentZip = path;
                lastFolder = path.parent_path();
                if (allZips) showProfileZips = false;
                PopulateZips(lastFolder, currentZip);
                original = opened.originalLocations;
                restoreFolder.clear();
                targets.clear();
                for (const auto& source : opened.tree->sources) targets.push_back(source.path);
                notice.clear();
                Populate();
            };
        });
    }

    void DeleteZip() {
        auto row = SendMessageW(zipList, CB_GETCURSEL, 0, 0);
        auto path = row == CB_ERR ? Browse(window, Picker::Open, lastFolder, true, BackupPlaces()) : zips.at(row);
        if (path.empty()) return;
        CheckPath(path);
        if (!EqualPath(path.extension(), L".zip") || !fs::is_regular_file(path)) {
            Fail(L"Choose an existing backup ZIP.");
        }
        if (!Confirm(window, L"Permanently delete this backup ZIP?", path.wstring(), true)) return;

        const bool wasOpen = EqualPath(path, currentZip);
        if (wasOpen) opened.archive.reset();
        const bool removed = DeleteFileW(path.c_str()) != FALSE;
        const DWORD error = removed ? ERROR_SUCCESS : GetLastError();
        if (wasOpen && !removed) {
            // Reload the whole archive so its tree and entry offsets always match.
            try {
                auto reopened = ReadZip(path);
                opened = std::move(reopened);
                original = opened.originalLocations;
                restoreFolder.clear();
                targets.clear();
                for (const auto& source : opened.tree->sources) targets.push_back(source.path);
            } catch (const std::exception&) {}
        }
        if (wasOpen && !opened.archive) {
            opened = {};
            currentZip.clear();
            restoreFolder.clear();
            targets.clear();
            original = false;
        }
        lastFolder = currentZip.empty() ? path.parent_path() : currentZip.parent_path();
        notice = removed ? L"Backup ZIP deleted." : L"Could not delete the backup ZIP.";
        PopulateZips(lastFolder, currentZip);
        Populate();
        if (!removed) Fail(L"Could not delete this backup ZIP.\n\n" + path.wstring() + L"\n\n" +
            Utf16(std::system_category().message(static_cast<int>(error)), CP_ACP));
    }

    void RunBackup() {
        const auto profile = profiles.at(activeProfile);
        auto folder = profile.schedule.folder;
        if (folder.empty()) Fail(L"Click Edit beside the backup name to choose a destination.");
        folder.make_preferred();
        if (folder != folder.root_path() && folder.filename().empty()) folder = folder.parent_path();
        CheckPath(folder);
        if (!fs::is_directory(folder)) Fail(L"Choose an existing backup folder.");
        if (!Confirm(window, L"Back up " + std::to_wstring(backup->root.checked) + L" selected items?",
            profile.name + L"\n" + folder.wstring() +
            (profile.automatic ? L"\n\nThis backup keeps " + std::to_wstring(profile.schedule.copies) +
                L" copies. Older excess copies will be deleted after the ZIP is saved." : L"") +
            (!backup->warnings.empty() ? L"\n\nSome source items were unavailable. See Scan issues for paths." : L"") +
            L"\n\nSelected online-only files may download from OneDrive.", profile.automatic)) return;
        Start(L"Creating backup…", [this, profile](const Progress& progress, const Transfer& transfer, fs::path& location) {
            auto model = Build(profile.sources, progress, transfer.stop);
            ApplyIgnored(*model, profile.excluded);
            if (model->incomplete) Fail(L"Some backup sources could not be read. Rescan and review Scan issues.");
            location = SaveManagedBackup(*model, profile.schedule, transfer, profile.automatic, profile.name);
            return [this, path = location] {
                lastFolder = path.parent_path();
                notice = L"Backup saved";
                Message(window, L"Backup saved to\n" + path.wstring());
            };
        }, L"Backup – " + profile.name, folder);
    }

    void RememberFolder(const fs::path& path) {
        auto folders = savedFolders;
        if (std::any_of(folders.begin(), folders.end(), [&](const auto& item) { return EqualPath(item, path); })) return;
        folders.push_back(path);
        SaveBackupDestinations(folders);
        savedFolders = std::move(folders);
    }

    fs::path PickSavedFolder(HWND owner, HWND button, const fs::path& current) {
        auto menu = CreatePopupMenu();
        if (!menu) Fail(L"Could not open saved destinations.");
        for (size_t i = 0; i < savedFolders.size(); ++i) {
            auto name = savedFolders[i].wstring();
            for (size_t pos = 0; (pos = name.find(L'&', pos)) != name.npos; pos += 2) name.insert(pos, 1, L'&');
            AppendMenuW(menu, EqualPath(savedFolders[i], current) ? MF_CHECKED : MF_STRING, i + 1, name.c_str());
        }
        if (savedFolders.empty()) AppendMenuW(menu, MF_GRAYED, 0, L"No saved destinations");
        RECT rect{};
        GetWindowRect(button, &rect);
        const auto choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, rect.left, rect.bottom, 0, owner, nullptr);
        DestroyMenu(menu);
        if (choice > 0 && static_cast<size_t>(choice) <= savedFolders.size()) return savedFolders[choice - 1];
        return {};
    }

    std::vector<fs::path> BackupPlaces() const {
        auto places = savedFolders;
        for (const auto& profile : profiles) {
            const auto& folder = profile.schedule.folder;
            if (!folder.empty() && std::none_of(places.begin(), places.end(), [&](const auto& path) { return EqualPath(path, folder); })) {
                places.push_back(folder);
            }
        }
        return places;
    }

    void RunRestore() {
        if (!opened.tree) return;
        if (!RainmeterInstalled(sources)) Fail(L"Install Rainmeter first, or use Backup → Scan folder to register a portable installation.");
        auto plan = PlanRestore(opened, targets, !original);
        std::wstring mapping;
        for (const auto& target : plan.destinations) {
            if (mapping.size() > 780) { mapping += L"\nSee the destination list for the remaining locations."; break; }
            mapping += L"\n" + target.wstring();
        }
        const auto question = plan.overwrites
            ? L"Restore " + std::to_wstring(plan.files) + L" file(s) and overwrite " +
                std::to_wstring(plan.overwrites) + L" existing file(s)?"
            : L"Restore " + std::to_wstring(plan.files) + L" file(s)?";
        if (!Confirm(window, question, L"Backup ZIP\n" + currentZip.filename().wstring() +
            L"\n\nSelected folders will be created as needed. Rainmeter will close and restart if it is running."
            L"\n\nRestore destinations" + mapping, plan.overwrites != 0)) return;
        Start(L"Preparing restore…", [this, plan = std::move(plan)](const Progress& progress, const Transfer& transfer) {
            RestoreWithRainmeterRestart(opened, plan, progress, transfer);
            return [this, count = plan.files] {
                notice = L"Restore completed";
                Message(window, std::to_wstring(count) + L" file(s) restored.");
            };
        }, L"Restore", currentZip);
    }

    void Command(int id, int notification);
    void EditProfile(bool create);
};

struct ScheduleDialog {
    static constexpr int Width = 612;
    static constexpr int Height = 330;
    static constexpr int Padding = 24;
    static constexpr int Footer = Height - Padding - 32;
    static constexpr int SaveWidth = 132;
    static constexpr int SaveLeft = Width - Padding - SaveWidth;

    App& app;
    BackupProfile profile;
    bool creating = false, saved = false, removed = false;
    HWND name{}, removeTask{}, savedFolderButton{};
    HWND tooltips{};
    HWND dialog{}, folder{}, time{}, copies{}, frequency{};
    HWND logText{}, emptyLog{}, closeLog{};
    bool open = true, showLog = false;
    std::wstring logContents;

    HWND Add(const wchar_t* type, const wchar_t* label, DWORD style, int id, int x, int y, int w, int h) {
        auto control = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style, app.Px(x), app.Px(y),
            app.Px(w), app.Px(h), dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.instance, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(app.regular), TRUE);
        if (wcscmp(type, L"BUTTON") == 0) SetButtonTip(tooltips, dialog, control, ButtonTip(id, false));
        return control;
    }

    void Init() {
        if (app.showTooltips) tooltips = CreateButtonTooltips(dialog, app.instance);
        if (showLog) {
            logText = Add(L"EDIT", L"", WS_BORDER | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP |
                ES_MULTILINE | ES_AUTOHSCROLL | ES_AUTOVSCROLL | ES_READONLY, 0,
                Padding, Padding, Width - 2 * Padding, Footer - Padding - 16);
            emptyLog = Add(L"BUTTON", L"Empty log", WS_TABSTOP, EmptyLog, Padding, Footer, 110, 32);
            closeLog = Add(L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, CloseDialog,
                Width - Padding - 84, Footer, 84, 32);
            RefreshLog();
            return;
        }
        const auto& schedule = profile.schedule;
        Add(L"STATIC", L"Backup name", 0, 0, Padding, 18, 360, 20);
        name = Add(L"EDIT", profile.name.c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 0,
            Padding, 42, Width - 2 * Padding, 27);
        SendMessageW(name, EM_SETLIMITTEXT, 100, 0);
        Add(L"STATIC", L"Backup destination", 0, 0, Padding, 78, 360, 20);
        folder = Add(L"EDIT", schedule.folder.c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 0,
            Padding, 102, 370, 27);
        savedFolderButton = Add(L"BUTTON", L"Saved…", WS_TABSTOP, ScheduleSavedFolders, 406, 101, 74, 29);
        Add(L"BUTTON", L"Browse…", WS_TABSTOP, ScheduleFolder, 492, 101, 96, 29);
        Add(L"STATIC", L"Schedule", 0, 0, Padding, 148, 190, 22);
        Add(L"STATIC", app.use24Hour ? L"Local time (24-hour)" : L"Local time (AM/PM)", 0, 0, 238, 148, 155, 22);
        Add(L"STATIC", L"Copies to keep", 0, 0, 417, 148, 140, 22);
        frequency = Add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, Frequency, Padding, 172, 190, 110);
        for (auto label : {L"Manual only", L"Daily", L"Weekly on Monday"}) {
            SendMessageW(frequency, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        }
        SendMessageW(frequency, CB_SETCURSEL, profile.automatic ? (schedule.weekly ? 2 : 1) : 0, 0);
        time = Add(L"EDIT", DisplayTime(schedule.time, app.use24Hour).c_str(), WS_BORDER | WS_TABSTOP, Time, 238, 172, 130, 27);
        copies = Add(L"EDIT", std::to_wstring(schedule.copies).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP,
            Copies, 417, 172, 100, 27);
        SetScheduleControls();
        Add(L"STATIC", L"Choose sources and exclusions in Rainmeter setups on the Backup screen.",
            0, 0, Padding, 218, Width - 2 * Padding, 32);
        if (!creating) {
            Add(L"BUTTON", L"Remove backup…", WS_TABSTOP, RemoveTask, Padding, Footer, 140, 32);
        }
        Add(L"BUTTON", L"Close", WS_TABSTOP, CloseDialog, SaveLeft - 96, Footer, 84, 32);
        Add(L"BUTTON", L"Save backup", BS_DEFPUSHBUTTON | WS_TABSTOP, SaveTask, SaveLeft, Footer, SaveWidth, 32);
    }

    void SetScheduleControls() const {
        const bool automatic = SendMessageW(frequency, CB_GETCURSEL, 0, 0) > 0;
        EnableWindow(time, automatic);
        EnableWindow(copies, automatic);
    }

    void RefreshLog() {
        const auto shown = DisplayLogTimes(logContents, app.use24Hour);
        SetWindowTextW(logText, shown.empty() ? L"No operations recorded yet." : shown.c_str());
        SendMessageW(logText, EM_SETSEL, shown.size(), shown.size());
        SendMessageW(logText, EM_SCROLLCARET, 0, 0);
        EnableWindow(emptyLog, !logContents.empty());
    }

    BackupProfile Values() const {
        auto value = profile;
        value.name = Text(name);
        const auto first = value.name.find_first_not_of(L" \t");
        if (first == value.name.npos) Fail(L"Enter a name for this backup.");
        value.name = value.name.substr(first, value.name.find_last_not_of(L" \t") - first + 1);
        for (const auto& other : app.profiles) {
            if (other.schedule.id != profile.schedule.id &&
                CompareStringOrdinal(other.name.c_str(), -1, value.name.c_str(), -1, TRUE) == CSTR_EQUAL) {
                Fail(L"Choose a name that is different from the other backups.");
            }
        }
        auto& schedule = value.schedule;
        schedule.folder = Text(folder);
        schedule.folder.make_preferred();
        CheckPath(schedule.folder);
        if (!fs::is_directory(schedule.folder)) Fail(L"Choose an existing backup destination.");
        const auto selection = SendMessageW(frequency, CB_GETCURSEL, 0, 0);
        value.automatic = selection > 0;
        if (value.automatic) {
            auto count = Text(copies);
            if (count.empty() || count.size() > 3 || !std::all_of(count.begin(), count.end(),
                [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; })) Fail(L"Enter a number of copies from 1 to 999.");
            schedule.copies = static_cast<unsigned>(std::stoi(count));
            schedule.weekly = selection == 2;
            schedule.time = ScheduleTime(Text(time), app.use24Hour);
        }
        return value;
    }

    void Command(int id, int notification) {
        switch (id) {
        case Frequency:
            if (notification == CBN_SELCHANGE) SetScheduleControls();
            break;
        case ScheduleSavedFolders: {
            auto selected = app.PickSavedFolder(dialog, savedFolderButton, Text(folder));
            if (!selected.empty()) SetWindowTextW(folder, selected.c_str());
            break;
        }
        case EmptyLog:
            if (Confirm(dialog, L"Empty the backup and restore log?",
                L"This permanently clears the operation history.", true)) {
                EmptyOperationLog();
                logContents = ReadOperationLog();
                RefreshLog();
                SetFocus(closeLog);
            }
            break;
        case ScheduleFolder: {
            auto selected = Browse(dialog, Picker::Folder, Text(folder));
            if (!selected.empty()) SetWindowTextW(folder, selected.c_str());
            break;
        }
        case SaveTask: {
            auto value = Values();
            auto timing = value.automatic ? std::wstring(value.schedule.weekly ? L"Monday at " : L"Daily at ") +
                DisplayTime(value.schedule.time, app.use24Hour) + L"\nKeep " +
                    std::to_wstring(value.schedule.copies) + L" copies" : L"Manual only";
            if (!Confirm(dialog, L"Save " + value.name + L"?", value.schedule.folder.wstring() +
                L"\n\n" + timing)) break;
            SaveBackupProfile(value);
            try {
                if (value.automatic) SaveSchedule(value.schedule);
                else if (profile.automatic) DeleteSchedule(profile.schedule.id);
            } catch (...) {
                if (creating) DeleteBackupProfile(value.schedule.id);
                else SaveBackupProfile(profile);
                throw;
            }
            profile = std::move(value);
            saved = true;
            open = false;
            break;
        }
        case RemoveTask: {
            const auto& destination = profile.schedule.folder;
            std::vector<fs::path> archives;
            std::error_code error;
            if (destination.empty() || IsReparse(destination)) error = std::make_error_code(std::errc::invalid_argument);
            for (fs::directory_iterator it = error ? fs::directory_iterator{} : fs::directory_iterator(destination, error), end;
                !error && it != end; it.increment(error)) {
                const auto& path = it->path();
                if (IsReparse(path) || !it->is_regular_file(error) || !IsProfileArchive(path, profile.schedule.id)) continue;
                try {
                    if (ReadZip(path).originalLocations) archives.push_back(path);
                } catch (const std::exception&) {
                    // Leave files we cannot verify as RainBackup archives in place.
                }
            }
            auto details = profile.name + (profile.automatic ? L"\n\nIts automatic schedule will also be removed." : L"");
            const bool canDelete = !error && !archives.empty();
            if (canDelete) {
                details += L"\n\nChecking the box permanently deletes " + std::to_wstring(archives.size()) +
                    L" matching ZIP file(s) in:\n" + destination.wstring();
            } else if (error) {
                details += L"\n\nThe destination could not be inspected. Its ZIP files will be kept.";
            }
            BOOL deleteArchives = FALSE;
            if (!Confirm(dialog, L"Remove this named backup?", details, true,
                canDelete ? L"Remove all .zip files as well" : nullptr, &deleteArchives)) break;
            if (deleteArchives) CheckPath(destination);
            if (profile.automatic) DeleteSchedule(profile.schedule.id);
            DeleteBackupProfile(profile.schedule.id);
            removed = true;
            open = false;
            if (!deleteArchives) break;
            app.opened = {};
            size_t failed = 0;
            for (const auto& path : archives) {
                if (IsReparse(path) || (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)) ++failed;
            }
            if (failed) Message(dialog, L"The backup was removed, but " + std::to_wstring(failed) +
                L" ZIP file(s) could not be deleted.\n\n" + destination.wstring(), MB_ICONWARNING);
            break;
        }
        case IDCANCEL:
        case CloseDialog: open = false; break;
        }
    }

    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto self = reinterpret_cast<ScheduleDialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<ScheduleDialog*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->dialog = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wp, lp);
        switch (message) {
        case WM_CREATE: self->Init(); return 0;
        case WM_CLOSE: self->open = false; return 0;
        case WM_COMMAND:
            try { self->Command(LOWORD(wp), HIWORD(wp)); }
            catch (const std::exception& error) { Message(window, Utf16(error.what()), MB_ICONERROR); }
            return 0;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, self->app.palette.text);
            if (reinterpret_cast<HWND>(lp) == self->logText) {
                SetBkColor(dc, self->app.palette.panel);
                return reinterpret_cast<LRESULT>(self->app.palette.surface);
            }
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(self->app.palette.background);
        }
        case WM_ERASEBKGND: {
            RECT r{};
            GetClientRect(window, &r);
            FillRect(reinterpret_cast<HDC>(wp), &r, self->app.palette.background);
            return 1;
        }
        default: return DefWindowProcW(window, message, wp, lp);
        }
    }

    void Show() {
        const wchar_t* windowClass = L"RainBackupBackupEditor";
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = app.instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = windowClass;
        RegisterClassW(&wc);
        RECT owner{};
        GetWindowRect(app.window, &owner);
        DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
        RECT r{0, 0, app.Px(Width), app.Px(Height)};
        AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_DLGMODALFRAME, app.dpi);
        int w = r.right - r.left, h = r.bottom - r.top;
        dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, windowClass,
            showLog ? L"Backup and restore log" : creating ? L"New backup" : L"Edit backup", style,
            (owner.left + owner.right - w) / 2, (owner.top + owner.bottom - h) / 2,
            w, h, app.window, nullptr, app.instance, this);
        if (!dialog) Fail(showLog ? L"Could not open the operation log." : L"Could not open the backup editor.");
        BOOL dark = app.palette.dark;
        DwmSetWindowAttribute(dialog, 20, &dark, sizeof(dark));
        EnableWindow(app.window, FALSE);
        ShowWindow(dialog, SW_SHOW);
        SetFocus(showLog ? closeLog : name);
        MSG message{};
        while (open && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) { open = false; continue; }
            if (!IsDialogMessageW(dialog, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        EnableWindow(app.window, TRUE);
        DestroyWindow(dialog);
        SetActiveWindow(app.window);
        if (message.message == WM_QUIT) PostQuitMessage(static_cast<int>(message.wParam));
    }
};

void App::EditProfile(bool create) {
    ScheduleDialog dialog{*this, create ? BackupProfile{} : profiles.at(activeProfile)};
    dialog.creating = create;
    if (create) {
        dialog.profile.schedule.id = NewScheduleId();
        dialog.profile.schedule.folder = profiles.empty() ? lastFolder : profiles.at(activeProfile).schedule.folder;
        dialog.profile.sources = Scan();
    }
    dialog.Show();
    if (dialog.removed) {
        auto folder = profiles.at(activeProfile).schedule.folder;
        profiles.erase(profiles.begin() + activeProfile);
        if (!folder.empty()) RememberFolder(folder);
        activeProfile = profiles.empty() ? 0 : std::min(activeProfile, profiles.size() - 1);
    } else if (dialog.saved) {
        if (create) {
            profiles.push_back(std::move(dialog.profile));
            activeProfile = profiles.size() - 1;
        } else {
            auto folder = profiles[activeProfile].schedule.folder;
            profiles[activeProfile] = std::move(dialog.profile);
            if (!folder.empty()) RememberFolder(folder);
        }
        RememberFolder(profiles.at(activeProfile).schedule.folder);
    }
    if (dialog.saved || dialog.removed) {
        RefreshProfiles();
        SelectProfile(activeProfile);
    }
}

void App::Command(int id, int notification) {
    if (id == CancelTransfer && transferring && stopTransfer.request_stop()) {
        SetWindowTextW(status, L"Canceling…");
        EnableWindow(buttons[CancelTransfer], FALSE);
    }
    if (busy) return;
    switch (id) {
    case BackupPage:
    case RestorePage:
        restorePage = id == RestorePage;
        if (restorePage && currentZip.empty()) ShowProfileArchives();
        notice.clear();
        Populate();
        break;
    case ScanNow:
        ScanSources({});
        break;
    case ScanFolder: {
        auto folder = Browse(window, Picker::Folder);
        if (!folder.empty()) ScanSources(folder);
        break;
    }
    case AddFile:
    case AddFolder: {
        auto path = Browse(window, id == AddFile ? Picker::Open : Picker::Folder);
        if (path.empty()) break;
        auto list = sources;
        auto item = Selected();
        auto setup = item ? (item->parent == &backup->root ? item->name : backup->sources.at(item->source).setup)
            : L"Additional sources";
        if (!Merge(list, {setup, Kind::Extra, path})) Fail(L"This source is already in the list.");
        if (Confirm(window, L"Add this backup source?", setup + L"\n" + path.wstring())) Rebuild(std::move(list));
        break;
    }
    case Change:
    case Remove: {
        auto root = id == Remove ? SelectedSource() : SelectedRoot();
        if (!root) break;
        auto list = sources;
        auto& source = list.at(root->source);
        if (id == Remove) {
            std::wstring action = root->directory
                ? L"Remove this folder and its contents from future backups?"
                : L"Remove this file from future backups?";
            if (!Confirm(window, action, source.path.wstring() +
                L"\n\nYour files on disk and existing backup ZIPs will stay in place.")) break;
            list.erase(list.begin() + root->source);
        } else {
            auto path = Browse(window, root->directory ? Picker::Folder : Picker::Open,
                root->directory ? source.path : source.path.parent_path());
            if (path.empty() || !Confirm(window, L"Change the backup source?",
                L"Current folder or file\n" + source.path.wstring() + L"\n\nNew folder or file\n" + path.wstring())) break;
            source.path = path;
        }
        Rebuild(std::move(list));
        break;
    }
    case SelectAll: if (Active()) Toggle(&Active()->root, true); break;
    case ClearAll: if (Active()) Toggle(&Active()->root, false); break;
    case SaveSelection:
        if (restorePage || !backup || profiles.empty()) break;
        try {
            RecordSelection();
            notice = L"Saved current selection";
        } catch (const std::exception&) {
            notice = L"Could not save the selection for " + profiles.at(activeProfile).name + L".";
        }
        Refresh();
        break;
    case Run: restorePage ? RunRestore() : RunBackup(); break;
    case ProfileChoice:
        if (notification == CBN_SELCHANGE) {
            auto row = SendMessageW(profileChoice, CB_GETCURSEL, 0, 0);
            if (row != CB_ERR && !profiles.empty()) SelectProfile(static_cast<size_t>(row));
        }
        break;
    case NewProfile: EditProfile(true); break;
    case Open: {
        auto folder = LoadOpenZipFolder();
        std::error_code error;
        if (!fs::is_directory(folder, error)) folder.clear();
        auto path = Browse(window, Picker::Open, folder, true, BackupPlaces(), true);
        if (!path.empty()) {
            SaveOpenZipFolder(path.parent_path());
            LoadZip(path, true);
        }
        break;
    }
    case DeleteBackup: DeleteZip(); break;
    case ZipList:
        if (notification == CBN_SELCHANGE) {
            auto row = SendMessageW(zipList, CB_GETCURSEL, 0, 0);
            if (row != CB_ERR && !EqualPath(zips.at(row), currentZip)) LoadZip(zips.at(row));
        }
        break;
    case Original:
        original = true;
        restoreFolder.clear();
        targets.clear();
        for (const auto& source : opened.tree->sources) targets.push_back(source.path);
        FillDestinations();
        Refresh();
        break;
    case NewLocation: {
        auto folder = Browse(window, Picker::Folder, restoreFolder.empty() ? lastFolder : restoreFolder);
        if (folder.empty()) break;
        CheckPath(folder);
        targets = NewLocations(opened, folder);
        restoreFolder = std::move(folder);
        original = false;
        FillDestinations();
        Refresh();
        break;
    }
    case ScheduleButton: EditProfile(false); break;
    case OpenBackupDestination: {
        if (restorePage || profiles.empty()) break;
        const auto& folder = profiles.at(activeProfile).schedule.folder;
        std::error_code error;
        if (!fs::is_directory(folder, error) ||
            reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"explore", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
            Fail(L"Could not open the backup destination in Windows Explorer.\n\n" + folder.wstring());
        }
        break;
    }
    case MoreMenu: {
        auto menu = CreatePopupMenu();
        if (!menu) Fail(L"Could not open the RainBackup menu.");
        auto settings = CreatePopupMenu();
        if (!settings) {
            DestroyMenu(menu);
            Fail(L"Could not open the RainBackup settings menu.");
        }
        AppendMenuW(settings, MF_STRING | (showTooltips ? MF_CHECKED : MF_UNCHECKED),
            ToggleTooltips, L"Show button tooltips");
        AppendMenuW(settings, MF_STRING | (palette.dark ? MF_CHECKED : MF_UNCHECKED),
            ToggleDarkMode, L"Dark Mode");
        AppendMenuW(settings, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(settings, MF_STRING | (use24Hour ? MF_UNCHECKED : MF_CHECKED), Time12Hour, L"12-hour time");
        AppendMenuW(settings, MF_STRING | (use24Hour ? MF_CHECKED : MF_UNCHECKED), Time24Hour, L"24-hour time");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(settings), L"&Settings");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, Help, L"&Help");
        AppendMenuW(menu, MF_STRING, Releases, L"&Releases");
        AppendMenuW(menu, MF_STRING, Support, L"&Support");
        RECT anchor{};
        GetWindowRect(buttons.at(MoreMenu), &anchor);
        const auto choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN,
            anchor.right, anchor.bottom, 0, window, nullptr);
        DestroyMenu(menu);
        if (choice) Command(choice, 0);
        break;
    }
    case ToggleTooltips:
        SaveButtonTooltips(!showTooltips);
        showTooltips = !showTooltips;
        SendMessageW(tooltips, TTM_ACTIVATE, showTooltips, 0);
        break;
    case Time12Hour:
    case Time24Hour:
        Save24HourTime(id == Time24Hour);
        use24Hour = id == Time24Hour;
        if (restorePage) {
            auto folder = currentZip.empty() ? (profiles.empty() ? fs::path{} :
                profiles.at(activeProfile).schedule.folder) : currentZip.parent_path();
            PopulateZips(folder, currentZip);
        }
        Refresh();
        break;
    case ToggleDarkMode:
        SaveDarkMode(!palette.dark);
        darkMode = !palette.dark;
        Theme();
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        break;
    case Releases:
    case Support: {
        const auto url = id == Releases ? L"https://github.com/jsmorley/RainBackup/releases" :
            L"https://forum.rainmeter.net/viewtopic.php?p=246995#p246988";
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", url, nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
            Fail(L"Could not open the page in the default browser.");
        }
        break;
    }
    case Help: {
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length >= executable.size()) Fail(L"Could not find the RainBackup installation folder.");
        executable.resize(length);
        const auto manual = fs::path(executable).parent_path() / L"RainBackup-Manual.txt";
        if (!fs::is_regular_file(manual)) Fail(L"Could not find the RainBackup manual.\n\n" + manual.wstring());
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", manual.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
            Fail(L"Could not open the RainBackup manual in the default text editor.");
        }
        break;
    }
    case ShowLog: {
        ScheduleDialog dialog{*this, {}};
        dialog.showLog = true;
        dialog.logContents = ReadOperationLog();
        dialog.Show();
        break;
    }
    case Details: {
        std::wstring warnings;
        for (const auto& warning : backup->warnings) {
            if (warnings.size() > 6000) { warnings += L"\nAdditional items were skipped."; break; }
            warnings += warning + L"\n";
        }
        Message(window, warnings, MB_ICONWARNING);
        break;
    }
    }
}

void DrawDestinationArrow(const DRAWITEMSTRUCT& item, const App& app) {
    using Microsoft::WRL::ComPtr;

    const RECT& bounds = item.rcItem;
    const auto& palette = app.palette;
    const bool disabled = item.itemState & ODS_DISABLED;
    const COLORREF ink = disabled ? palette.quiet : palette.text;
    auto color = [](COLORREF value) {
        return D2D1::ColorF(GetRValue(value) / 255.0f, GetGValue(value) / 255.0f, GetBValue(value) / 255.0f);
    };
    auto drawEmoji = [&] {
        ComPtr<ID2D1Factory> graphics;
        ComPtr<IDWriteFactory> fonts;
        ComPtr<IDWriteTextFormat> format;
        ComPtr<ID2D1DCRenderTarget> target;
        ComPtr<ID2D1SolidColorBrush> brush;
        const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, graphics.GetAddressOf())) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(fonts.GetAddressOf()))) ||
            FAILED(fonts->CreateTextFormat(L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(app.Px(18)),
                L"en-US", format.GetAddressOf())) ||
            FAILED(graphics->CreateDCRenderTarget(&properties, target.GetAddressOf())) ||
            FAILED(target->BindDC(item.hDC, &bounds)) ||
            FAILED(target->CreateSolidColorBrush(color(ink), brush.GetAddressOf()))) return false;

        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        target->BeginDraw();
        target->Clear(color(palette.bg));
        target->DrawText(DestinationArrow, static_cast<UINT32>(std::size(DestinationArrow) - 1),
            format.Get(), D2D1::RectF(0, 0, static_cast<float>(bounds.right - bounds.left),
                static_cast<float>(bounds.bottom - bounds.top)), brush.Get(),
            disabled ? D2D1_DRAW_TEXT_OPTIONS_NONE : D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        return SUCCEEDED(target->EndDraw());
    };

    if (!drawEmoji()) {
        FillRect(item.hDC, &bounds, palette.background);
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, ink);
        const auto previous = SelectObject(item.hDC, app.regular);
        RECT textBounds = bounds;
        DrawTextW(item.hDC, DestinationArrow, -1, &textBounds, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        SelectObject(item.hDC, previous);
    }
    if (!disabled && (item.itemState & ODS_SELECTED ||
        (item.itemState & ODS_FOCUS && !(item.itemState & ODS_NOFOCUSRECT)))) {
        RECT underline{bounds.left + app.Px(6), bounds.bottom - app.Px(3),
            bounds.right - app.Px(6), bounds.bottom - app.Px(2)};
        auto brush = CreateSolidBrush(palette.accent);
        FillRect(item.hDC, &underline, brush);
        DeleteObject(brush);
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wp, lp);
    try {
        switch (message) {
        case WM_CREATE: app->Init(); return 0;
        case WM_SIZE: if (app->tree) app->Layout(); return 0;
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMinTrackSize = {app->Px(775), app->Px(712)};
            return 0;
        }
        case WM_DPICHANGED: {
            app->dpi = HIWORD(wp);
            app->LoadHeaderIcon();
            app->Fonts();
            app->Theme();
            auto r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            auto dc = BeginPaint(window, &paint);
            if (app->headerIcon) DrawIconEx(dc, app->Px(30), app->Px(20), app->headerIcon,
                app->Px(HeaderLogoSize), app->Px(HeaderLogoSize), 0, nullptr, DI_NORMAL);
            if (!app->restorePage && app->backupPath) {
                FillRect(dc, &app->backupField, app->palette.surface);
                auto border = CreateSolidBrush(GetFocus() == app->backupPath ? app->palette.accent : app->palette.border);
                FrameRect(dc, &app->backupField, border);
                DeleteObject(border);
            }
            EndPaint(window, &paint);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (!app->restorePage && IsWindowEnabled(app->backupPath) && PtInRect(&app->backupField, point)) {
                SetFocus(app->backupPath);
                return 0;
            }
            break;
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            if (app->tree) app->Theme();
            return 0;
        case WM_CTLCOLORSTATIC: {
            auto dc = reinterpret_cast<HDC>(wp);
            auto child = reinterpret_cast<HWND>(lp);
            if (child == app->backupPath) {
                SetTextColor(dc, app->palette.quiet);
                SetBkColor(dc, app->palette.panel);
                return reinterpret_cast<LRESULT>(app->palette.surface);
            }
            SetTextColor(dc, child == app->title || child == app->summary ? app->palette.text : app->palette.quiet);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(app->palette.background);
        }
        case WM_CTLCOLOREDIT: {
            auto dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, app->palette.text);
            SetBkColor(dc, app->palette.panel);
            return reinterpret_cast<LRESULT>(app->palette.surface);
        }
        case WM_ERASEBKGND: {
            RECT r{};
            GetClientRect(window, &r);
            FillRect(reinterpret_cast<HDC>(wp), &r, app->palette.background);
            return 1;
        }
        case WM_DRAWITEM: {
            auto row = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (row->CtlType == ODT_COMBOBOX && row->CtlID == ProfileChoice) {
                const bool selected = row->itemState & ODS_SELECTED;
                const bool disabled = row->itemState & ODS_DISABLED;
                auto& palette = app->palette;
                auto fill = CreateSolidBrush(selected ? palette.accent : palette.panel);
                FillRect(row->hDC, &row->rcItem, fill);
                DeleteObject(fill);
                if (row->itemID < app->profiles.size() || app->profiles.empty()) {
                    const auto* name = app->profiles.empty() ? L"No named backups" : app->profiles[row->itemID].name.c_str();
                    auto bounds = row->rcItem;
                    bounds.left += app->Px(5);
                    bounds.right -= app->Px(3);
                    const auto previous = SelectObject(row->hDC, app->profileFont);
                    SetBkMode(row->hDC, TRANSPARENT);
                    SetTextColor(row->hDC, selected ? palette.onAccent : disabled ? palette.quiet : palette.text);
                    DrawTextW(row->hDC, name, -1, &bounds,
                        DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
                    SelectObject(row->hDC, previous);
                }
                if ((row->itemState & ODS_FOCUS) && !(row->itemState & ODS_NOFOCUSRECT)) {
                    DrawFocusRect(row->hDC, &row->rcItem);
                }
                return TRUE;
            }
            if (row->CtlType != ODT_BUTTON) break;
            auto id = row->CtlID;
            if (id == OpenBackupDestination) {
                DrawDestinationArrow(*row, *app);
                return TRUE;
            }
            auto r = row->rcItem;
            auto& p = app->palette;
            bool disabled = row->itemState & ODS_DISABLED;
            bool primary = !disabled && (id == Run || (id == BackupPage && !app->restorePage) ||
                (id == RestorePage && app->restorePage));
            bool down = row->itemState & ODS_SELECTED;
            auto fill = primary ? p.accent : down ? p.hover : disabled ? p.bg : p.panel;
            FillRect(row->hDC, &r, p.background);
            Box(row->hDC, r, app->Px(12), fill, primary ? p.accent : p.border);
            SetBkMode(row->hDC, TRANSPARENT);
            SetTextColor(row->hDC, primary ? p.onAccent : disabled ? p.quiet : p.text);
            if (id == MoreMenu) {
                const int x = (r.left + r.right) / 2;
                const int y = (r.top + r.bottom) / 2;
                const int halfWidth = app->Px(8);
                const int thickness = std::max(1, app->Px(2));
                auto ink = CreateSolidBrush(disabled ? p.quiet : p.text);
                for (int offset : {-5, 0, 5}) {
                    const int top = y + app->Px(offset) - thickness / 2;
                    RECT line{x - halfWidth, top, x + halfWidth, top + thickness};
                    FillRect(row->hDC, &line, ink);
                }
                DeleteObject(ink);
            } else {
                auto text = Text(row->hwndItem);
                auto previous = SelectObject(row->hDC, id == Run ? app->section : app->regular);
                DrawTextW(row->hDC, text.c_str(), -1, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_HIDEPREFIX);
                SelectObject(row->hDC, previous);
            }
            if ((row->itemState & ODS_FOCUS) && !(row->itemState & ODS_NOFOCUSRECT)) {
                InflateRect(&r, -3, -3);
                DrawFocusRect(row->hDC, &r);
            }
            return TRUE;
        }
        case WM_NOTIFY: {
            auto event = reinterpret_cast<NMHDR*>(lp);
            if (event->hwndFrom == app->destinations && event->code == LVN_ITEMCHANGING) {
                const auto* change = reinterpret_cast<const NMLISTVIEW*>(event);
                // Destination rows are a preview. Keep scrolling and column resizing available.
                if ((change->uChanged & LVIF_STATE) &&
                    (change->uNewState & (LVIS_SELECTED | LVIS_FOCUSED))) return TRUE;
            }
            if (event->hwndFrom == app->tree && !app->updating) {
                if (event->code == TVN_SELCHANGEDW) app->Refresh();
                if (event->code == TVN_ITEMEXPANDINGW) {
                    auto info = reinterpret_cast<NMTREEVIEWW*>(lp);
                    auto node = reinterpret_cast<Node*>(info->itemNew.lParam);
                    if (node && info->action == TVE_EXPAND) app->AddChildren(*node, info->itemNew.hItem);
                }
                if (event->code == NM_CLICK && !app->busy) {
                    POINT point{};
                    GetCursorPos(&point);
                    ScreenToClient(app->tree, &point);
                    TVHITTESTINFO hit{};
                    hit.pt = point;
                    TreeView_HitTest(app->tree, &hit);
                    if (hit.hItem && (hit.flags & TVHT_ONITEMSTATEICON)) {
                        TreeView_SelectItem(app->tree, hit.hItem);
                        auto node = app->Selected();
                        if (node) app->Toggle(node, node->checked != node->total);
                        return TRUE;
                    }
                    if (!app->restorePage && (!hit.hItem || (hit.flags & TVHT_ONITEMRIGHT))) {
                        TreeView_SelectItem(app->tree, nullptr);
                        return TRUE;
                    }
                }
                if (event->code == TVN_KEYDOWN && reinterpret_cast<NMTVKEYDOWN*>(lp)->wVKey == VK_SPACE) {
                    auto node = app->Selected();
                    if (node) app->Toggle(node, node->checked != node->total);
                    return TRUE;
                }
                if (event->code == TVN_GETINFOTIPW) {
                    auto tip = reinterpret_cast<NMTVGETINFOTIPW*>(lp);
                    auto node = reinterpret_cast<Node*>(tip->lParam);
                    if (node && !node->path.empty()) wcsncpy_s(tip->pszText, tip->cchTextMax, node->path.c_str(), _TRUNCATE);
                }
            }
            break;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == BackupPath && (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS)) {
                InvalidateRect(window, &app->backupField, FALSE);
            }
            app->Command(LOWORD(wp), HIWORD(wp));
            return 0;
        case ProgressUpdate: {
            std::unique_ptr<ProgressMessage> progress(reinterpret_cast<ProgressMessage*>(lp));
            if (app->busy && !app->stopTransfer.stop_requested()) {
                SetWindowTextW(app->status, progress->text.c_str());
                if (app->transferring && progress->percent >= 0) {
                    SendMessageW(app->progressBar, PBM_SETPOS, progress->percent, 0);
                    SetWindowTextW(app->percentLabel, (std::to_wstring(progress->percent) + L"%").c_str());
                }
            }
            return 0;
        }
        case Finished: {
            std::unique_ptr<JobResult> result(reinterpret_cast<JobResult*>(lp));
            app->worker.join();
            if (app->transferring && result->error.empty()) {
                SendMessageW(app->progressBar, PBM_SETPOS, 100, 0);
                SetWindowTextW(app->percentLabel, L"100%");
                SetWindowTextW(app->status, L"Completed");
                EnableWindow(app->buttons[CancelTransfer], FALSE);
            }
            const bool loadedZip = app->loadingZip;
            // Keep the loading state through the UI commit, as well as the ZIP read.
            app->busy = loadedZip;
            app->transferring = false;
            if (!result->error.empty()) {
                app->busy = false;
                app->loadingZip = false;
                app->notice = result->cancelled ? L"Operation canceled" : L"Operation stopped";
                if (!app->currentZip.empty()) app->PopulateZips(app->currentZip.parent_path(), app->currentZip);
                app->Refresh();
                Message(window, result->error, result->cancelled ? MB_ICONINFORMATION : MB_ICONERROR);
            } else if (result->done) {
                app->Refresh();
                try {
                    result->done();
                } catch (...) {
                    app->busy = false;
                    app->loadingZip = false;
                    app->Refresh();
                    throw;
                }
            }
            if (loadedZip) app->ScrollTreeToTop();
            app->busy = false;
            app->loadingZip = false;
            if (result->logFailed) app->notice += L" · Could not write the operation log.";
            app->Refresh();
            return 0;
        }
        case SetupCloseMessage:
            if (app->busy || !IsWindowEnabled(window)) return SetupCloseBusy;
            PostMessageW(window, WM_CLOSE, 0, 0);
            return SetupCloseAccepted;
        case WM_CLOSE:
            if (app->busy) Message(window, L"Wait for the current operation to finish.");
            else DestroyWindow(window);
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
    } catch (const std::exception& error) {
        if (message == WM_CREATE) return -1;
        try { Message(window, Utf16(error.what()), MB_ICONERROR); }
        catch (...) { Message(window, L"RainBackup encountered an error.", MB_ICONERROR); }
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int show) {
    std::wstring_view args(arguments);
    if (args == L"--close-for-setup") return CloseRainBackupForSetup();
    if (args == L"--scheduled") return RunScheduled(true);
    if (args.starts_with(L"--scheduled ")) return RunScheduled(true, args.substr(12));
    if (args == L"--remove-schedule") {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        int result = 0;
        try { DeleteAllSchedules(); } catch (...) { result = 1; }
        CoUninitialize();
        return result;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (args.find_first_not_of(L" \t") != args.npos) {
        const int result = RunNamedCommand();
        CoUninitialize();
        return result;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES |
        ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    App app;
    app.instance = instance;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = L"RainBackup";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(RainBackupIcon));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    auto window = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"RainBackup " RAINBACKUP_VERSION,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        MulDiv(965, dpi, 96), MulDiv(790, dpi, 96), nullptr, nullptr, instance, &app);
    if (!window) { CoUninitialize(); return 1; }
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CoUninitialize();
    return static_cast<int>(message.wParam);
}

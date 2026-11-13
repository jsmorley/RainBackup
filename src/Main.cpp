#include "Archive.h"
#include "RainmeterSession.h"
#include "Schedule.h"
#include "SetupSession.h"

#include <commctrl.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <map>
#include <thread>

using namespace rb;
namespace {
constexpr UINT Finished = WM_APP + 1, ProgressUpdate = WM_APP + 2;
constexpr int RainBackupIcon = 101;
constexpr int HeaderLogoSize = 64;
enum Id {
    BackupPage = 100, RestorePage, ScanNow, ScanFolder, AddFolder, AddFile, Change, Remove, ScheduleButton,
    SelectAll, ClearAll, Run, Open, Original, NewLocation, UseDetected, Details, ZipList, TreeId, Destinations,
    EditDestination, ScheduleFolder, Frequency, Time, Copies, SaveTask, RemoveTask, RunTask, CloseDialog,
    BrowseBackup, BackupPath, CancelTransfer, DeleteBackup, ShowLog, EmptyLog
};

std::wstring Text(HWND control) {
    std::wstring result(GetWindowTextLengthW(control) + 1, L'\0');
    result.resize(GetWindowTextW(control, result.data(), static_cast<int>(result.size())));
    return result;
}

bool Confirm(HWND window, const std::wstring& message) {
    return MessageBoxW(window, message.c_str(), L"RainBackup", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
}

void Message(HWND window, const std::wstring& message, UINT icon = MB_ICONINFORMATION) {
    MessageBoxW(window, message.c_str(), L"RainBackup", MB_OK | icon);
}

enum class Picker { Folder, Open };
fs::path NewBackupPath(const fs::path& folder) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t name[80]{};
    swprintf_s(name, L"RainBackup-%04u-%02u-%02u_%02u-%02u-%02u.zip",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    fs::path path = folder / name;
    const auto stem = path.stem().wstring();
    for (unsigned copy = 2; fs::exists(path); ++copy) {
        path = folder / (stem + L"-" + std::to_wstring(copy) + L".zip");
    }
    return path;
}

fs::path Browse(HWND owner, Picker type, const fs::path& initial = {}, bool zipOnly = false) {
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

    if (!initial.empty()) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            dialog->SetFolder(item);
            item->Release();
        }
    }
    result = dialog->Show(owner);
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
    void Load(HWND window) {
        DWORD light = 1, size = sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
        HIGHCONTRASTW contrast{};
        contrast.cbSize = sizeof(contrast);
        SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
        dark = !light && !(contrast.dwFlags & HCF_HIGHCONTRASTON);
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
    HFONT regular{}, heading{}, section{};
    HIMAGELIST checks{}, icons{};
    HINSTANCE instance{};
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
    fs::path currentZip, lastFolder;
    std::vector<fs::path> zips, targets;
    std::map<int, HWND> buttons;
    std::map<Node*, HTREEITEM> items;
    std::thread worker;
    std::wstring notice;

    ~App() {
        if (worker.joinable()) worker.join();
        if (headerIcon) DestroyIcon(headerIcon);
        for (auto font : {regular, heading, section}) DeleteObject(font);
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
        for (auto font : {regular, heading, section}) DeleteObject(font);
        auto font = [&](int size, int weight) {
            return CreateFontW(-Px(size), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        };
        regular = font(14, FW_NORMAL);
        heading = font(32, FW_SEMIBOLD);
        section = font(16, FW_SEMIBOLD);
        for (HWND c = GetWindow(window, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            auto use = c == title ? heading : c == summary ? section : regular;
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(use), TRUE);
        }
    }

    void Theme() {
        palette.Load(window);
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
        mode = Control(L"STATIC", L"", SS_NOPREFIX);
        backupPathLabel = Control(L"STATIC", L"Backup destination", SS_NOPREFIX);
        lastFolder = LoadBackupFolder();
        if (lastFolder.empty()) lastFolder = KnownFolder(FOLDERID_Documents);
        backupPath = Control(L"EDIT", lastFolder.c_str(),
            WS_TABSTOP | ES_AUTOHSCROLL, BackupPath);
        SendMessageW(backupPath, EM_SETLIMITTEXT, 32767, 0);
        tree = Control(WC_TREEVIEWW, L"", WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
            TVS_SHOWSELALWAYS | TVS_DISABLEDRAGDROP | TVS_INFOTIP, TreeId);
        TreeView_SetExtendedStyle(tree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
        destinations = Control(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, Destinations);
        ListView_SetExtendedListViewStyle(destinations, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
        for (int i = 0; i < 2; ++i) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = const_cast<wchar_t*>(i == 0 ? L"Backup root" : L"Restore destination");
            col.cx = Px(i == 0 ? 240 : 450);
            ListView_InsertColumn(destinations, i, &col);
        }
        zipList = Control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, ZipList);
        const std::pair<int, const wchar_t*> labels[] = {
            {BackupPage,L"&Backup"}, {RestorePage,L"&Restore"}, {ScanNow,L"Scan"}, {ScanFolder,L"Scan folder…"},
            {AddFolder,L"Add folder…"}, {AddFile,L"Add file…"}, {Change,L"Change path…"}, {Remove,L"Remove folder"},
            {ShowLog,L"Log…"}, {ScheduleButton,L"Schedule…"}, {SelectAll,L"Select all"}, {ClearAll,L"Clear selection"}, {Run,L"Back up now…"},
            {Open,L"Open ZIP…"}, {Original,L"Original location"}, {NewLocation,L"New location…"},
            {UseDetected,L"Use this PC’s folders"}, {Details,L"Scan issues…"}, {EditDestination,L"Change destination…"},
            {BrowseBackup,L"Browse…"}, {CancelTransfer,L"Cancel"}, {DeleteBackup,L"Delete backup…"}
        };
        for (const auto& [id, text] : labels) buttons[id] = Control(L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, id);
        Fonts();
        Theme();
        Refresh();
        ScanSources({}, true);
    }

    void Layout() {
        RECT r{};
        GetClientRect(window, &r);
        const int width = MulDiv(r.right, 96, dpi), height = MulDiv(r.bottom, 96, dpi);
        auto place = [&](HWND control, int x, int y, int w, int h) {
            MoveWindow(control, Px(x), Px(y), Px(w), Px(h), TRUE);
        };
        auto button = [&](int id, int x, int y, int w, int h = 34) { place(buttons.at(id), x, y, w, h); };
        place(title, 108, 20, width - 350, 42);
        place(sub, 110, 64, width - 352, 24);
        button(ScheduleButton, width - 142, 36, 112, 38);
        button(ShowLog, width - 230, 36, 80, 38);
        button(BackupPage, 30, 101, 119, 38);
        button(RestorePage, 157, 101, 119, 38);
        int y = 159;
        if (restorePage) {
            button(Open, 30, y, 110);
            place(zipList, 150, y + 4, width - 326, 230);
            button(DeleteBackup, width - 166, y, 136);
        } else {
            int x = 30;
            for (auto [id, w] : {std::pair{ScanNow,72}, {ScanFolder,112}, {AddFolder,108},
                {AddFile,91}, {Change,123}, {Remove,103}}) {
                button(id, x, y, w);
                x += w + 8;
            }
        }
        y += 50;
        place(summary, 30, y + 5, width - 350, 27);
        button(SelectAll, width - 252, y, 95, 31);
        button(ClearAll, width - 149, y, 119, 31);
        int lower = height - (restorePage ? 256 : 151);
        place(tree, 30, y + 42, width - 60, std::max(80, lower - y - 42));
        if (restorePage) {
            place(mode, 30, lower + 10, width - 60, 23);
            button(Original, 30, lower + 39, 132, 32);
            button(NewLocation, 170, lower + 39, 128, 32);
            button(UseDetected, 306, lower + 39, 157, 32);
            button(EditDestination, width - 191, lower + 39, 161, 32);
            place(destinations, 30, lower + 78, width - 60, 107);
            ListView_SetColumnWidth(destinations, 0, Px(242));
            ListView_SetColumnWidth(destinations, 1, Px(width - 325));
        } else {
            place(detail, 30, lower + 7, width - 195, 25);
            button(Details, width - 149, lower + 2, 119, 30);
            place(backupPathLabel, 30, lower + 47, 130, 25);
            backupField = {Px(162), Px(lower + 43), Px(width - 143), Px(lower + 72)};
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
            button(BrowseBackup, width - 132, lower + 40, 102, 34);
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
        for (auto id : {ScanNow, ScanFolder, AddFolder, AddFile, Change, Remove, Details, BrowseBackup}) {
            ShowWindow(buttons[id], restorePage ? SW_HIDE : SW_SHOW);
        }
        for (auto id : {Open, Original, NewLocation, UseDetected, EditDestination, DeleteBackup}) {
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
        EnableWindow(backupPath, !busy);
        auto selectedSource = SelectedSource();
        SetWindowTextW(buttons[Remove], selectedSource && !selectedSource->directory ? L"Remove file" : L"Remove folder");
        for (auto& [id, control] : buttons) {
            bool enabled = !busy && !loadingZip;
            if (id == CancelTransfer) enabled = transferring && !stopTransfer.stop_requested();
            if (id == SelectAll || id == ClearAll) enabled &= has;
            if (id == Run) enabled &= has && Active()->root.checked != 0;
            if (id == Details) enabled &= backup && !backup->warnings.empty();
            if (id == Change) enabled &= Selected() && !Selected()->path.empty();
            if (id == Remove) enabled &= selectedSource != nullptr;
            if (id == Original || id == UseDetected) enabled &= opened.originalLocations;
            if (id == NewLocation || id == EditDestination) enabled &= opened.tree != nullptr;
            EnableWindow(control, enabled);
            InvalidateRect(control, nullptr, TRUE);
        }
        EnableWindow(tree, !busy && !loadingZip);
        EnableWindow(destinations, !busy && !loadingZip);
        EnableWindow(zipList, !busy && !loadingZip && !zips.empty());
        SetWindowTextW(buttons[Run], restorePage ? L"Restore selected…" : L"Back up now…");
        SetWindowTextW(summary, restorePage ? L"Archive contents" : L"Rainmeter setups");
        SetWindowTextW(mode, original ? L"Restore to original locations" : L"Restore to new locations");
        if (!busy && !loadingZip) {
            std::wstring label = notice;
            if (label.empty() && has) label = std::to_wstring(Active()->root.checked) + L" of " +
                std::to_wstring(Active()->root.total) + L" items selected";
            if (label.empty()) label = restorePage ? L"Open a ZIP to choose what to restore." : L"Scan or add a source to start.";
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
            auto label = source.path.empty() ? L"ZIP contents" : KindName(source.kind) + L" · " + source.path.wstring();
            LVITEMW row{};
            row.mask = LVIF_TEXT;
            row.iItem = static_cast<int>(i);
            row.pszText = label.data();
            auto at = ListView_InsertItem(destinations, &row);
            auto value = i < targets.size() && !targets[i].empty() ? targets[i].wstring() : L"Choose a destination";
            ListView_SetItemText(destinations, at, 1, value.data());
        }
        if (!targets.empty()) ListView_SetItemState(destinations, 0, LVIS_SELECTED, LVIS_SELECTED);
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
        SaveIgnored(fresh);
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
        sources = model->sources;
        backup = std::move(model);
        SaveSources(sources);
        notice = std::move(message);
        Populate();
    }

    void ScanSources(fs::path folder, bool initial = false) {
        Start(L"Scanning Rainmeter…", [this, folder = std::move(folder), initial](const Progress& progress) {
            auto list = initial ? LoadSources() : sources;
            auto exclusions = initial ? LoadIgnored() : ignored;
            auto found = initial && !list.empty() ? std::vector<Source>{} : Scan(folder);
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

    void PopulateZips(const fs::path& folder, const fs::path& selected) {
        zips.clear();
        std::error_code error;
        for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
            if (EqualPath(it->path().extension(), L".zip") && it->is_regular_file(error)) zips.push_back(it->path());
            error.clear();
        }
        if (!selected.empty() && fs::is_regular_file(selected, error) &&
            std::none_of(zips.begin(), zips.end(), [&](const auto& item) { return EqualPath(item, selected); })) {
            zips.push_back(selected);
        }
        std::sort(zips.begin(), zips.end(), std::greater<>());
        SendMessageW(zipList, CB_RESETCONTENT, 0, 0);
        for (size_t i = 0; i < zips.size(); ++i) {
            SendMessageW(zipList, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(zips[i].filename().c_str()));
            if (EqualPath(zips[i], selected)) SendMessageW(zipList, CB_SETCURSEL, i, 0);
        }
    }

    void LoadZip(fs::path path) {
        loadingZip = true;
        Start(L"Reading ZIP contents…", [this, path = std::move(path)](const Progress&) {
            auto zip = std::make_shared<OpenZip>(ReadZip(path));
            return [this, path, zip] {
                opened = std::move(*zip);
                currentZip = path;
                lastFolder = path.parent_path();
                PopulateZips(lastFolder, currentZip);
                original = opened.originalLocations;
                targets.clear();
                for (const auto& source : opened.tree->sources) targets.push_back(source.path);
                notice.clear();
                Populate();
            };
        });
    }

    void DeleteZip() {
        auto row = SendMessageW(zipList, CB_GETCURSEL, 0, 0);
        auto path = row == CB_ERR ? Browse(window, Picker::Open, lastFolder, true) : zips.at(row);
        if (path.empty()) return;
        CheckPath(path);
        if (!EqualPath(path.extension(), L".zip") || !fs::is_regular_file(path)) {
            Fail(L"Choose an existing backup ZIP.");
        }
        if (!Confirm(window, L"Permanently delete this backup ZIP?\n\n" + path.wstring())) return;

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
                targets.clear();
                for (const auto& source : opened.tree->sources) targets.push_back(source.path);
            } catch (const std::exception&) {}
        }
        if (wasOpen && !opened.archive) {
            opened = {};
            currentZip.clear();
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
        fs::path folder(Text(backupPath));
        if (folder.empty()) Fail(L"Enter a backup folder, or click Browse.");
        folder.make_preferred();
        if (folder != folder.root_path() && folder.filename().empty()) folder = folder.parent_path();
        CheckPath(folder);
        if (!fs::is_directory(folder)) Fail(L"Choose an existing backup folder.");
        if (!Confirm(window, L"Create a backup ZIP with " + std::to_wstring(backup->root.checked) +
            L" selected items in this folder?\n\n" + folder.wstring() +
            (!backup->warnings.empty() ? L"\n\nSome source items were unavailable. See Scan issues for paths." : L"") +
            L"\n\nSelected online-only files may download from OneDrive.")) return;
        Start(L"Creating backup…", [this, folder](const Progress&, const Transfer& transfer, fs::path& location) {
            location = NewBackupPath(folder);
            WriteZip(*backup, location, transfer, ZipDisposition::CreateNew);
            return [this, path = location] {
                lastFolder = path.parent_path();
                notice = L"Backup saved";
                SetWindowTextW(backupPath, lastFolder.c_str());
                Message(window, L"Backup saved to\n" + path.wstring());
                SaveBackupFolder(lastFolder);
            };
        }, L"Backup", folder);
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
        if (!Confirm(window, L"Restore " + std::to_wstring(plan.files) + L" file(s) from " +
            currentZip.filename().wstring() + L"?\n\n" + std::to_wstring(plan.overwrites) +
            L" existing file(s) will be overwritten. Selected folders will be created as needed.\n\n"
            L"Rainmeter will close and restart if it is running.\n\nDestinations:" + mapping)) return;
        Start(L"Preparing restore…", [this, plan = std::move(plan)](const Progress& progress, const Transfer& transfer) {
            RainmeterSession session;
            std::exception_ptr error;
            try {
                if (transfer.stop.stop_requested()) throw Cancelled("Restore canceled before writing any files.");
                session.Stop(progress);
                Restore(opened, plan, transfer);
            } catch (...) { error = std::current_exception(); }
            auto restart = session.Restart();
            if (error) {
                if (restart.empty()) std::rethrow_exception(error);
                try { std::rethrow_exception(error); }
                catch (const std::exception& failure) { Fail(Utf16(failure.what()) + L"\n\n" + restart); }
            }
            if (!restart.empty()) Fail(restart);
            return [this, count = plan.files] {
                notice = L"Restore completed";
                Message(window, std::to_wstring(count) + L" file(s) restored.");
            };
        }, L"Restore", currentZip);
    }

    void ChangeDestination() {
        int row = ListView_GetNextItem(destinations, -1, LVNI_SELECTED);
        if (row < 0 || !opened.tree) Fail(L"Select a root in the destination list.");
        const auto& source = opened.tree->sources.at(row);
        bool file = false;
        if (opened.originalLocations) {
            for (const auto& setup : opened.tree->root.children) {
                for (const auto& node : setup->children) if (node->source == static_cast<size_t>(row)) file = !node->directory;
            }
        }
        auto folder = Browse(window, Picker::Folder, file ? targets[row].parent_path() : targets[row]);
        if (folder.empty()) return;
        targets[row] = file ? folder / source.path.filename() : folder;
        original = false;
        FillDestinations();
        Refresh();
    }

    void Command(int id, int notification);
    void ShowSchedule();
};

struct ScheduleDialog {
    static constexpr int Width = 612;
    static constexpr int Height = 272;
    static constexpr int Padding = 24;
    static constexpr int Footer = Height - Padding - 32;
    static constexpr int SaveWidth = 132;
    static constexpr int SaveLeft = Width - Padding - SaveWidth;

    App& app;
    Schedule schedule;
    HWND dialog{}, folder{}, time{}, copies{}, frequency{};
    HWND logText{}, emptyLog{}, closeLog{};
    bool open = true, runNow = false, showLog = false;
    std::wstring logContents;

    HWND Add(const wchar_t* type, const wchar_t* label, DWORD style, int id, int x, int y, int w, int h) {
        auto control = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style, app.Px(x), app.Px(y),
            app.Px(w), app.Px(h), dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.instance, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(app.regular), TRUE);
        return control;
    }

    void Init() {
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
        Add(L"STATIC", L"Backup destination", 0, 0, Padding, 18, 360, 20);
        folder = Add(L"EDIT", schedule.folder.c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 0, Padding, 42, 456, 27);
        Add(L"BUTTON", L"Browse…", WS_TABSTOP, ScheduleFolder, 492, 41, 96, 29);
        Add(L"STATIC", L"Frequency", 0, 0, Padding, 88, 190, 22);
        Add(L"STATIC", L"Local time (24-hour)", 0, 0, 238, 88, 155, 22);
        Add(L"STATIC", L"Copies to keep", 0, 0, 417, 88, 140, 22);
        frequency = Add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, Frequency, Padding, 112, 190, 95);
        SendMessageW(frequency, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Daily"));
        SendMessageW(frequency, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Weekly on Monday"));
        SendMessageW(frequency, CB_SETCURSEL, schedule.weekly ? 1 : 0, 0);
        time = Add(L"EDIT", schedule.time.c_str(), WS_BORDER | WS_TABSTOP, Time, 238, 112, 130, 27);
        copies = Add(L"EDIT", std::to_wstring(schedule.copies).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, Copies, 417, 112, 100, 27);
        Add(L"STATIC", L"Missed runs catch up when your account is available.\nSuccessful backups remove the oldest excess copies.", 0, 0, Padding, 158, SaveLeft - Padding - 16, 42);
        Add(L"BUTTON", L"Remove schedule", WS_TABSTOP, RemoveTask, Padding, Footer, 140, 32);
        Add(L"BUTTON", L"Close", WS_TABSTOP, CloseDialog, 226, Footer, 84, 32);
        Add(L"BUTTON", L"Back up now…", WS_TABSTOP, RunTask, 322, Footer, 122, 32);
        Add(L"BUTTON", L"Save schedule", BS_DEFPUSHBUTTON | WS_TABSTOP, SaveTask, SaveLeft, Footer, SaveWidth, 32);
    }

    void RefreshLog() {
        SetWindowTextW(logText, logContents.empty() ? L"No operations recorded yet." : logContents.c_str());
        SendMessageW(logText, EM_SETSEL, logContents.size(), logContents.size());
        SendMessageW(logText, EM_SCROLLCARET, 0, 0);
        EnableWindow(emptyLog, !logContents.empty());
    }

    Schedule Values() const {
        auto count = Text(copies);
        if (count.empty() || count.size() > 3 || !std::all_of(count.begin(), count.end(),
            [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; })) Fail(L"Enter a number of copies from 1 to 999.");
        return {Text(folder), static_cast<unsigned>(std::stoi(count)), SendMessageW(frequency, CB_GETCURSEL, 0, 0) == 1, Text(time)};
    }

    std::wstring Explain(const Schedule& value) const {
        return L"\n\n" + value.folder.wstring() + L"\nKeep " + std::to_wstring(value.copies) +
            L" managed ZIP(s). The oldest excess copies will be deleted after a new ZIP is saved.";
    }

    void Command(int id) {
        switch (id) {
        case EmptyLog:
            if (Confirm(dialog, L"Empty the backup and restore log?")) {
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
            if (!Confirm(dialog, std::wstring(L"Schedule an unattended backup ") +
                (value.weekly ? L"every Monday at " : L"every day at ") + value.time + L"?" +
                Explain(value) + L"\n\nSelected online-only files may download from OneDrive.")) break;
            if (!app.backup || !app.backup->root.checked) Fail(L"Select backup sources before scheduling.");
            SaveSchedule(value);
            app.notice = L"Backup schedule saved.";
            open = false;
            break;
        }
        case RunTask: {
            auto value = Values();
            if (!app.backup || !app.backup->root.checked) Fail(L"Select at least one backup source.");
            if (!Confirm(dialog, L"Create a managed backup now?" + Explain(value) +
                L"\n\nSelected online-only files may download from OneDrive.")) break;
            schedule = std::move(value);
            runNow = true;
            open = false;
            break;
        }
        case RemoveTask:
            if (Confirm(dialog, L"Remove the automatic backup schedule? Existing ZIP files will be kept.")) {
                DeleteSchedule();
                app.notice = L"Backup schedule removed.";
                open = false;
            }
            break;
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
            try { self->Command(LOWORD(wp)); }
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
        const wchar_t* name = L"RainBackupSchedule";
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = app.instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = name;
        RegisterClassW(&wc);
        RECT owner{};
        GetWindowRect(app.window, &owner);
        DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
        RECT r{0, 0, app.Px(Width), app.Px(Height)};
        AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_DLGMODALFRAME, app.dpi);
        int w = r.right - r.left, h = r.bottom - r.top;
        dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, name,
            showLog ? L"Backup and restore log" : L"Schedule backups", style,
            (owner.left + owner.right - w) / 2, (owner.top + owner.bottom - h) / 2,
            w, h, app.window, nullptr, app.instance, this);
        if (!dialog) Fail(showLog ? L"Could not open the operation log." : L"Could not open backup scheduling.");
        BOOL dark = app.palette.dark;
        DwmSetWindowAttribute(dialog, 20, &dark, sizeof(dark));
        EnableWindow(app.window, FALSE);
        ShowWindow(dialog, SW_SHOW);
        SetFocus(showLog ? closeLog : folder);
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

void App::ShowSchedule() {
    ScheduleDialog dialog{*this, LoadSchedule()};
    if (dialog.schedule.folder.empty()) dialog.schedule.folder = lastFolder;
    dialog.Show();
    if (dialog.runNow) {
        const auto destination = dialog.schedule.folder;
        Start(L"Creating managed backup…", [this, schedule = std::move(dialog.schedule)](
            const Progress& progress, const Transfer& transfer, fs::path& location) {
            auto model = Build(sources, progress, transfer.stop);
            ApplyIgnored(*model, ignored);
            if (model->incomplete) Fail(L"Backup stopped because a source item could not be read. Review Scan issues in RainBackup.");
            auto path = SaveManagedBackup(*model, schedule, transfer);
            location = path;
            return [this, path] {
                lastFolder = path.parent_path();
                notice = L"Managed backup saved";
                Message(window, L"Backup saved to\n" + path.wstring());
            };
        }, L"Managed backup", destination);
    }
    Refresh();
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
        if (Confirm(window, L"Add this source to " + setup + L"?\n\n" + path.wstring())) Rebuild(std::move(list));
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
            if (!Confirm(window, action + L"\n\n" + source.path.wstring() +
                L"\n\nYour files on disk and existing backup ZIPs will stay in place.")) break;
            list.erase(list.begin() + root->source);
        } else {
            auto path = Browse(window, root->directory ? Picker::Folder : Picker::Open,
                root->directory ? source.path : source.path.parent_path());
            if (path.empty() || !Confirm(window, L"Change the backup source?\n\nFrom " + source.path.wstring() +
                L"\nTo " + path.wstring())) break;
            source.path = path;
        }
        Rebuild(std::move(list));
        break;
    }
    case SelectAll: if (Active()) Toggle(&Active()->root, true); break;
    case ClearAll: if (Active()) Toggle(&Active()->root, false); break;
    case Run: restorePage ? RunRestore() : RunBackup(); break;
    case BrowseBackup: {
        fs::path current(Text(backupPath));
        auto path = Browse(window, Picker::Folder, fs::is_directory(current) ? current : lastFolder);
        if (!path.empty()) SetWindowTextW(backupPath, path.c_str());
        break;
    }
    case Open: {
        auto path = Browse(window, Picker::Open, lastFolder, true);
        if (!path.empty()) LoadZip(path);
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
        targets.clear();
        for (const auto& source : opened.tree->sources) targets.push_back(source.path);
        FillDestinations();
        Refresh();
        break;
    case NewLocation: {
        auto folder = Browse(window, Picker::Folder, lastFolder);
        if (!folder.empty()) {
            original = false;
            targets = NewLocations(opened, folder);
            FillDestinations();
            Refresh();
        }
        break;
    }
    case UseDetected:
        original = false;
        targets = InstalledLocations(opened, sources);
        FillDestinations();
        Refresh();
        break;
    case EditDestination: ChangeDestination(); break;
    case ScheduleButton: ShowSchedule(); break;
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
            if (row->CtlType != ODT_BUTTON) break;
            auto id = row->CtlID;
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
            auto text = Text(row->hwndItem);
            auto previous = SelectObject(row->hDC, id == Run ? app->section : app->regular);
            DrawTextW(row->hDC, text.c_str(), -1, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_HIDEPREFIX);
            SelectObject(row->hDC, previous);
            if ((row->itemState & ODS_FOCUS) && !(row->itemState & ODS_NOFOCUSRECT)) {
                InflateRect(&r, -3, -3);
                DrawFocusRect(row->hDC, &r);
            }
            return TRUE;
        }
        case WM_NOTIFY: {
            auto event = reinterpret_cast<NMHDR*>(lp);
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
            if (event->hwndFrom == app->destinations && event->code == NM_DBLCLK && !app->busy) app->ChangeDestination();
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
    if (args == L"--remove-schedule") {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        int result = 0;
        try { DeleteSchedule(); } catch (...) { result = 1; }
        CoUninitialize();
        return result;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
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
    auto window = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"RainBackup",
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

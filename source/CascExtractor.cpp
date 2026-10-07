// CASC Extractor - browse a local WoW (CASC) install, tick files/folders, extract them.
// Files that fail are logged and skipped. Interrupted jobs can be resumed.
// Built with MinGW-w64 + CascLib (static). Uses the UTF-8 ANSI code page via manifest.

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wininet.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "CascLib.h"

static const char *APP_TITLE = "CASC Extractor";
static const char *LISTFILE_URL =
    "https://github.com/wowdev/wow-listfile/releases/latest/download/community-listfile.csv";

// ---------------------------------------------------------------- ids / messages
enum {
    ID_INSTALL = 100, ID_BR_INSTALL, ID_OPEN,
    ID_LIST, ID_BR_LIST, ID_DL_LIST,
    ID_OUT, ID_BR_OUT, ID_OPEN_OUT,
    ID_FILTER, ID_CHECK, ID_UNCHECK, ID_LISTCHK, ID_CLEAR,
    ID_TREE, ID_STATUS, ID_PROGRESS, ID_EXTRACT, ID_RESUME, ID_STOP, ID_LOG,
    ID_L1, ID_L2, ID_L3, ID_L4,
};
enum {
    WM_APP_STATUS = WM_APP + 1,  // lParam = std::string*
    WM_APP_LOG,                  // lParam = std::string*
    WM_APP_OPENED,               // wParam = ok
    WM_APP_PROGRESS,             // wParam = done, lParam = total
    WM_APP_DONE,                 // extraction finished
    WM_APP_DLDONE,               // listfile download finished, wParam = ok
};

// ---------------------------------------------------------------- data model
struct FileRec {
    uint32_t name;   // offset into g_pool (full path, lowercase, '/')
    int32_t dir;
    uint32_t fdid;
    uint64_t size;
    uint8_t avail;
};
struct DirRec {
    std::string name;
    int parent = -1;
    std::vector<int> subdirs;
    std::vector<uint32_t> files;
    uint32_t total = 0, checked = 0;
    HTREEITEM item = nullptr;
    bool populated = false;
};

static std::vector<char> g_pool;
static std::vector<FileRec> g_files;
static std::vector<uint8_t> g_checked;
static std::vector<DirRec> g_dirs;   // g_dirs[0] = invisible root
static HANDLE g_storage = nullptr;

static HWND g_wnd, g_tree, g_log, g_status, g_progress;
static bool g_openAfterDownload = false;
static bool g_demo = false;  // --demo: build the tree from the listfile only (UI testing)
static HFONT g_font;
static int g_dpi = 96;
static std::atomic<bool> g_busy{false}, g_stop{false};
static std::string g_exeDir, g_iniPath;

static const char *PathOf(uint32_t f) { return &g_pool[g_files[f].name]; }
static const char *PlainOf(uint32_t f) {
    const char *p = PathOf(f);
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}
static int S(int v) { return MulDiv(v, g_dpi, 96); }

// ---------------------------------------------------------------- helpers
static void PostStr(UINT msg, const std::string &s) {
    PostMessageA(g_wnd, msg, 0, (LPARAM) new std::string(s));
}
static void Status(const std::string &s) { PostStr(WM_APP_STATUS, s); }
static void Log(const std::string &s) { PostStr(WM_APP_LOG, s); }

static std::string GetText(int id) {
    HWND h = GetDlgItem(g_wnd, id);
    int n = GetWindowTextLengthA(h);
    std::string s(n, '\0');
    GetWindowTextA(h, &s[0], n + 1);
    return s;
}
static void SetText(int id, const std::string &s) { SetWindowTextA(GetDlgItem(g_wnd, id), s.c_str()); }

static std::string Lower(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}
static std::string Trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n\""), b = s.find_last_not_of(" \t\r\n\"");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static std::string Num(uint64_t v) {
    std::string s = std::to_string(v), o;
    int n = 0;
    for (int i = (int)s.size() - 1; i >= 0; --i) {
        o.insert(o.begin(), s[i]);
        if (++n % 3 == 0 && i) o.insert(o.begin(), ',');
    }
    return o;
}
static std::string Bytes(uint64_t b) {
    char buf[64];
    if (b < 1024) snprintf(buf, sizeof buf, "%llu B", (unsigned long long)b);
    else if (b < 1024ull * 1024) snprintf(buf, sizeof buf, "%.1f KB", b / 1024.0);
    else if (b < 1024ull * 1024 * 1024) snprintf(buf, sizeof buf, "%.1f MB", b / 1048576.0);
    else snprintf(buf, sizeof buf, "%.2f GB", b / 1073741824.0);
    return buf;
}
static bool FileExists(const std::string &p, uint64_t *size = nullptr) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &a)) return false;
    if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    if (size) *size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    return true;
}
static void MakeDirs(const std::string &dir) {
    for (size_t i = 3; i <= dir.size(); ++i)
        if (i == dir.size() || dir[i] == '\\') CreateDirectoryA(dir.substr(0, i).c_str(), nullptr);
}
static std::string ErrText(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: return "not found in this build";
    case ERROR_FILE_OFFLINE: return "not on disk (never downloaded by the launcher)";
    case ERROR_FILE_ENCRYPTED: return "encrypted (decryption key not known)";
    case ERROR_FILE_CORRUPT: return "corrupt data";
    case ERROR_FILE_INCOMPLETE: return "incomplete data";
    case ERROR_BAD_FORMAT: return "bad format";
    case ERROR_DISK_FULL: return "disk full";
    }
    char *msg = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, (LPSTR)&msg, 0, nullptr);
    std::string s = msg ? Trim(msg) : "";
    if (msg) LocalFree(msg);
    return "error " + std::to_string(e) + (s.empty() ? "" : " (" + s + ")");
}
static std::string IniGet(const char *key, const char *def) {
    char buf[1024];
    GetPrivateProfileStringA("settings", key, def, buf, sizeof buf, g_iniPath.c_str());
    return buf;
}
static void IniSet(const char *key, const std::string &v) {
    WritePrivateProfileStringA("settings", key, v.c_str(), g_iniPath.c_str());
}
static void SaveSettings() {
    IniSet("install", GetText(ID_INSTALL));
    IniSet("listfile", GetText(ID_LIST));
    IniSet("output", GetText(ID_OUT));
    IniSet("filter", GetText(ID_FILTER));
}

static std::string PickFolder(const std::string &start) {
    std::string result;
    IFileOpenDialog *dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                (void **)&dlg)))
        return result;
    DWORD opts;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (!start.empty()) {
        wchar_t w[1024];
        MultiByteToWideChar(CP_UTF8, 0, start.c_str(), -1, w, 1024);
        IShellItem *si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(w, nullptr, IID_IShellItem, (void **)&si))) {
            dlg->SetFolder(si);
            si->Release();
        }
    }
    if (SUCCEEDED(dlg->Show(g_wnd))) {
        IShellItem *si = nullptr;
        if (SUCCEEDED(dlg->GetResult(&si))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(si->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                char u[2048];
                WideCharToMultiByte(CP_UTF8, 0, p, -1, u, sizeof u, nullptr, nullptr);
                result = u;
                CoTaskMemFree(p);
            }
            si->Release();
        }
    }
    dlg->Release();
    return result;
}
static std::string PickFile(const char *filter, const std::string &start) {
    char buf[2048] = {0};
    if (!start.empty()) lstrcpynA(buf, start.c_str(), sizeof buf);
    OPENFILENAMEA o = {sizeof o};
    o.hwndOwner = g_wnd;
    o.lpstrFilter = filter;
    o.lpstrFile = buf;
    o.nMaxFile = sizeof buf;
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&o) ? buf : "";
}

// ---------------------------------------------------------------- tree view
static int StateOfDir(int d) {
    const DirRec &r = g_dirs[d];
    return r.checked == 0 ? 1 : (r.checked == r.total ? 2 : 3);
}
static LPARAM DirParam(int d) { return ((LPARAM)d << 1); }
static LPARAM FileParam(uint32_t f) { return ((LPARAM)f << 1) | 1; }

static void SetItemStateImg(HTREEITEM h, int img) {
    TreeView_SetItemState(g_tree, h, INDEXTOSTATEIMAGEMASK(img), TVIS_STATEIMAGEMASK);
}

static HTREEITEM InsertItem(HTREEITEM parent, LPARAM param, int img, bool children) {
    TVINSERTSTRUCTA ins = {};
    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE | TVIF_CHILDREN;
    ins.item.pszText = LPSTR_TEXTCALLBACKA;
    ins.item.lParam = param;
    ins.item.stateMask = TVIS_STATEIMAGEMASK;
    ins.item.state = INDEXTOSTATEIMAGEMASK(img);
    ins.item.cChildren = children ? 1 : 0;
    return (HTREEITEM)SendMessageA(g_tree, TVM_INSERTITEMA, 0, (LPARAM)&ins);
}

static void PopulateDir(int d) {
    DirRec &r = g_dirs[d];
    if (r.populated) return;
    r.populated = true;
    HTREEITEM parent = d == 0 ? TVI_ROOT : r.item;
    SendMessageA(g_tree, WM_SETREDRAW, FALSE, 0);
    for (int s : r.subdirs)
        g_dirs[s].item = InsertItem(parent, DirParam(s), StateOfDir(s), true);
    for (uint32_t f : r.files) InsertItem(parent, FileParam(f), g_checked[f] ? 2 : 1, false);
    SendMessageA(g_tree, WM_SETREDRAW, TRUE, 0);
}

// refresh check images of an item and all its populated descendants
static void RefreshSubtree(HTREEITEM h) {
    for (HTREEITEM c = TreeView_GetChild(g_tree, h); c; c = TreeView_GetNextSibling(g_tree, c)) {
        TVITEMA it = {};
        it.mask = TVIF_PARAM;
        it.hItem = c;
        SendMessageA(g_tree, TVM_GETITEMA, 0, (LPARAM)&it);
        if (it.lParam & 1) {
            SetItemStateImg(c, g_checked[it.lParam >> 1] ? 2 : 1);
        } else {
            int d = (int)(it.lParam >> 1);
            SetItemStateImg(c, StateOfDir(d));
            if (g_dirs[d].populated) RefreshSubtree(c);
        }
    }
}
static void RefreshAll() {
    SendMessageA(g_tree, WM_SETREDRAW, FALSE, 0);
    RefreshSubtree(TVI_ROOT);
    SendMessageA(g_tree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_tree, nullptr, FALSE);
}
static void RefreshAncestors(int d) {
    for (; d > 0; d = g_dirs[d].parent)
        if (g_dirs[d].item) SetItemStateImg(g_dirs[d].item, StateOfDir(d));
}

static void SetFile(uint32_t f, bool v) {
    if (g_checked[f] == (uint8_t)v) return;
    g_checked[f] = v;
    for (int d = g_files[f].dir; d >= 0; d = g_dirs[d].parent) g_dirs[d].checked += v ? 1 : (uint32_t)-1;
}
static void SetDir(int d, bool v) {
    for (uint32_t f : g_dirs[d].files) SetFile(f, v);
    for (int s : g_dirs[d].subdirs) SetDir(s, v);
}

static void UpdateSummary() {
    uint64_t n = 0, bytes = 0;
    for (size_t i = 0; i < g_files.size(); ++i)
        if (g_checked[i]) { ++n; bytes += g_files[i].size; }
    std::string s = Num(g_files.size()) + " files in the install.   Checked: " + Num(n) + " files (" +
                    Bytes(bytes) + ")";
    SetWindowTextA(g_status, s.c_str());
}

static void ToggleItem(HTREEITEM h) {
    if (!h || g_busy) return;
    TVITEMA it = {};
    it.mask = TVIF_PARAM;
    it.hItem = h;
    SendMessageA(g_tree, TVM_GETITEMA, 0, (LPARAM)&it);
    if (it.lParam & 1) {
        uint32_t f = (uint32_t)(it.lParam >> 1);
        SetFile(f, !g_checked[f]);
        SetItemStateImg(h, g_checked[f] ? 2 : 1);
        RefreshAncestors(g_files[f].dir);
    } else {
        int d = (int)(it.lParam >> 1);
        SetDir(d, StateOfDir(d) != 2);
        SetItemStateImg(h, StateOfDir(d));
        if (g_dirs[d].populated) {
            SendMessageA(g_tree, WM_SETREDRAW, FALSE, 0);
            RefreshSubtree(h);
            SendMessageA(g_tree, WM_SETREDRAW, TRUE, 0);
        }
        RefreshAncestors(g_dirs[d].parent);
        InvalidateRect(g_tree, nullptr, FALSE);
    }
    UpdateSummary();
}

static HIMAGELIST MakeCheckImages() {
    int sz = S(16);
    HIMAGELIST il = ImageList_Create(sz, sz, ILC_COLOR24 | ILC_MASK, 4, 0);
    HDC screen = GetDC(nullptr);
    for (int i = 0; i < 4; ++i) {
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, sz, sz);
        HGDIOBJ old = SelectObject(dc, bmp);
        RECT rc = {0, 0, sz, sz};
        HBRUSH mag = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(dc, &rc, mag);
        DeleteObject(mag);
        if (i > 0) {
            RECT b = {S(2), S(2), sz - S(1), sz - S(1)};
            UINT st = DFCS_BUTTONCHECK | DFCS_FLAT;
            if (i == 2) st |= DFCS_CHECKED;
            if (i == 3) st |= DFCS_CHECKED | DFCS_INACTIVE;
            DrawFrameControl(dc, &b, DFC_BUTTON, st);
            if (i == 3) {  // partial: draw a filled square instead of the greyed tick
                RECT in = {b.left + S(3), b.top + S(3), b.right - S(3), b.bottom - S(3)};
                HBRUSH fill = CreateSolidBrush(RGB(60, 120, 215));
                FillRect(dc, &in, fill);
                DeleteObject(fill);
            }
        }
        SelectObject(dc, old);
        ImageList_AddMasked(il, bmp, RGB(255, 0, 255));
        DeleteObject(bmp);
        DeleteDC(dc);
    }
    ReleaseDC(nullptr, screen);
    return il;
}

// ---------------------------------------------------------------- opening the storage
static int DirFor(std::unordered_map<std::string, int> &map, const std::string &dir) {
    if (dir.empty()) return 0;
    auto it = map.find(dir);
    if (it != map.end()) return it->second;
    size_t slash = dir.rfind('/');
    int parent = DirFor(map, slash == std::string::npos ? "" : dir.substr(0, slash));
    int idx = (int)g_dirs.size();
    g_dirs.emplace_back();
    g_dirs[idx].name = slash == std::string::npos ? dir : dir.substr(slash + 1);
    g_dirs[idx].parent = parent;
    g_dirs[parent].subdirs.push_back(idx);
    map[dir] = idx;
    return idx;
}
static uint32_t SumTotals(int d) {
    DirRec &r = g_dirs[d];
    r.total = (uint32_t)r.files.size();
    for (int s : r.subdirs) r.total += SumTotals(s);
    return r.total;
}

static std::vector<std::string> ReadLines(const std::string &p);
static void OpenWorker(std::string install, std::string listfile) {
    bool ok = false;
    if (g_demo) {
        std::vector<char> pool;
        std::vector<FileRec> files;
        for (auto &l : ReadLines(listfile)) {
            size_t semi = l.find(';');
            if (semi == std::string::npos) continue;
            std::string name = Lower(l.substr(semi + 1));
            FileRec r{(uint32_t)pool.size(), -1, (uint32_t)strtoul(l.c_str(), 0, 10), 1000, (uint8_t)(files.size() % 7 != 0)};
            pool.insert(pool.end(), name.begin(), name.end());
            pool.push_back(0);
            files.push_back(r);
        }
        g_pool.swap(pool);
        g_files.swap(files);
    }
    do {
        if (g_demo) break;
        if (g_storage) { CascCloseStorage(g_storage); g_storage = nullptr; }
        Status("Opening the CASC storage in " + install + " ...");
        if (!CascOpenStorage(install.c_str(), 0, &g_storage)) {
            DWORD e = GetCascError();
            Log("Could not open the storage (" + (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND
                                                     ? std::string("no .build.info found there")
                                                     : ErrText(e)) +
                "). Point 'WoW install' at the folder that contains .build.info.");
            g_storage = nullptr;
            break;
        }
        Status("Reading the file list ...");
        std::vector<char> pool;
        std::vector<FileRec> files;
        std::unordered_map<uint32_t, uint32_t> byId;
        CASC_FIND_DATA fd;
        HANDLE hf = CascFindFirstFile(g_storage, "*", &fd, listfile.empty() ? nullptr : listfile.c_str());
        if (!hf) {
            Log("Could not list the files: " + ErrText(GetCascError()));
            break;
        }
        uint64_t n = 0;
        do {
            if (++n % 50000 == 0) Status("Reading the file list ... " + Num(n));
            if (fd.dwFileDataId != CASC_INVALID_ID) {
                auto it = byId.find(fd.dwFileDataId);
                if (it != byId.end()) {  // same file in another locale: keep the one that is on disk
                    if (!files[it->second].avail && fd.bFileAvailable) files[it->second].avail = 1;
                    continue;
                }
            }
            std::string name;
            if (fd.NameType == CascNameFull) {
                name = Lower(fd.szFileName);
                for (auto &c : name) if (c == '\\') c = '/';
            } else if (fd.dwFileDataId != CASC_INVALID_ID) {
                char b[64];
                snprintf(b, sizeof b, "_unnamed/%lu.dat", (unsigned long)fd.dwFileDataId);
                name = b;
            } else {
                name = std::string("_unnamed/") + Lower(fd.szFileName);
            }
            FileRec r;
            r.name = (uint32_t)pool.size();
            r.dir = -1;
            r.fdid = fd.dwFileDataId;
            r.size = fd.FileSize == CASC_INVALID_SIZE64 ? 0 : fd.FileSize;
            r.avail = fd.bFileAvailable ? 1 : 0;
            pool.insert(pool.end(), name.begin(), name.end());
            pool.push_back(0);
            if (fd.dwFileDataId != CASC_INVALID_ID) byId[fd.dwFileDataId] = (uint32_t)files.size();
            files.push_back(r);
        } while (CascFindNextFile(hf, &fd));
        CascFindClose(hf);
        g_pool.swap(pool);
        g_files.swap(files);
        ok = true;
    } while (false);
    if (g_demo) ok = !g_files.empty();
    if (ok) {
        Status("Building the folder tree ...");
        g_checked.assign(g_files.size(), 0);
        g_dirs.clear();
        g_dirs.emplace_back();
        std::unordered_map<std::string, int> map;
        for (uint32_t i = 0; i < g_files.size(); ++i) {
            std::string p = PathOf(i);
            size_t s = p.rfind('/');
            int d = DirFor(map, s == std::string::npos ? "" : p.substr(0, s));
            g_files[i].dir = d;
            g_dirs[d].files.push_back(i);
        }
        for (auto &d : g_dirs) {
            std::sort(d.subdirs.begin(), d.subdirs.end(),
                      [](int a, int b) { return g_dirs[a].name < g_dirs[b].name; });
            std::sort(d.files.begin(), d.files.end(),
                      [](uint32_t a, uint32_t b) { return strcmp(PlainOf(a), PlainOf(b)) < 0; });
        }
        SumTotals(0);
        size_t missing = 0, unnamed = 0;
        for (auto &f : g_files) missing += !f.avail;
        for (auto &f : g_files) unnamed += strncmp(&g_pool[f.name], "_unnamed/", 9) == 0;
        Log("Opened: " + Num(g_files.size()) + " files (" + Num(unnamed) + " without a name in the listfile, " +
            Num(missing) + " not on disk).");
    }
    PostMessageA(g_wnd, WM_APP_OPENED, ok, 0);
}

// ---------------------------------------------------------------- listfile download
static void DownloadWorker(std::string dest) {
    bool ok = false;
    HINTERNET net = InternetOpenA(APP_TITLE, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    HINTERNET url = net ? InternetOpenUrlA(net, LISTFILE_URL, nullptr, 0,
                                           INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0)
                        : nullptr;
    if (!url) {
        Log("Listfile download failed: " + ErrText(GetLastError()));
    } else {
        char lenbuf[32] = {0};
        DWORD ll = sizeof lenbuf;
        uint64_t total = 0;
        if (HttpQueryInfoA(url, HTTP_QUERY_CONTENT_LENGTH, lenbuf, &ll, nullptr)) total = _strtoui64(lenbuf, 0, 10);
        std::string part = dest + ".part";
        FILE *f = fopen(part.c_str(), "wb");
        if (f) {
            std::vector<char> buf(1 << 20);
            uint64_t got = 0;
            DWORD rd = 0;
            ok = true;
            while (InternetReadFile(url, buf.data(), (DWORD)buf.size(), &rd)) {
                if (rd == 0) break;
                fwrite(buf.data(), 1, rd, f);
                got += rd;
                Status("Downloading listfile ... " + Bytes(got) + (total ? " of " + Bytes(total) : ""));
                if (g_stop) { ok = false; break; }
            }
            fclose(f);
            if (total && got != total) ok = false;
            if (ok) ok = MoveFileExA(part.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
            if (!ok) { DeleteFileA(part.c_str()); Log("Listfile download did not complete."); }
            else Log("Listfile saved: " + dest + " (" + Bytes(got) + ")");
        }
    }
    if (url) InternetCloseHandle(url);
    if (net) InternetCloseHandle(net);
    PostMessageA(g_wnd, WM_APP_DLDONE, ok, 0);
}

// ---------------------------------------------------------------- extraction
struct JobItem {
    uint32_t fdid;
    uint64_t size;
    std::string path;
};

static std::string JobLine(const JobItem &j) {
    return std::to_string(j.fdid) + ";" + std::to_string(j.size) + ";" + j.path;
}
static bool ParseJobLine(const std::string &line, JobItem &j) {
    size_t a = line.find(';'), b = a == std::string::npos ? a : line.find(';', a + 1);
    if (b == std::string::npos) return false;
    j.fdid = (uint32_t)strtoul(line.substr(0, a).c_str(), nullptr, 10);
    j.size = _strtoui64(line.substr(a + 1, b - a - 1).c_str(), nullptr, 10);
    j.path = Trim(line.substr(b + 1));
    return !j.path.empty();
}
static std::vector<std::string> ReadLines(const std::string &p) {
    std::vector<std::string> out;
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return out;
    std::string cur;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') { out.push_back(Trim(cur)); cur.clear(); }
        else cur += (char)c;
    }
    if (!Trim(cur).empty()) out.push_back(Trim(cur));
    fclose(f);
    return out;
}
static void WriteText(const std::string &p, const std::string &s) {
    FILE *f = fopen(p.c_str(), "wb");
    if (f) { fwrite(s.data(), 1, s.size(), f); fclose(f); }
}
static void AppendText(const std::string &p, const std::string &s) {
    FILE *f = fopen(p.c_str(), "ab");
    if (f) { fwrite(s.data(), 1, s.size(), f); fclose(f); }
}

static bool ExtractOne(const JobItem &j, const std::string &out, std::string &why) {
    HANDLE hf = nullptr;
    bool opened = j.fdid != CASC_INVALID_ID
                      ? CascOpenFile(g_storage, CASC_FILE_DATA_ID(j.fdid), 0, CASC_OPEN_BY_FILEID, &hf)
                      : CascOpenFile(g_storage, j.path.c_str(), 0, CASC_OPEN_BY_NAME, &hf);
    if (!opened) { why = ErrText(GetCascError()); return false; }
    std::string part = out + ".part";
    HANDLE w = CreateFileA(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (w == INVALID_HANDLE_VALUE) {
        why = "cannot write output: " + ErrText(GetLastError());
        CascCloseFile(hf);
        return false;
    }
    static std::vector<char> buf(4 << 20);
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!CascReadFile(hf, buf.data(), (DWORD)buf.size(), &got)) {
            why = ErrText(GetCascError());
            ok = false;
            break;
        }
        if (got == 0) break;
        DWORD wr = 0;
        if (!WriteFile(w, buf.data(), got, &wr, nullptr) || wr != got) {
            why = "cannot write output: " + ErrText(GetLastError());
            ok = false;
            break;
        }
        if (g_stop) { why = "stopped"; ok = false; break; }
    }
    CloseHandle(w);
    CascCloseFile(hf);
    if (ok && !MoveFileExA(part.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        why = "cannot rename output: " + ErrText(GetLastError());
        ok = false;
    }
    if (!ok) DeleteFileA(part.c_str());
    return ok;
}

static void ExtractWorker(std::string outDir, std::vector<JobItem> job) {
    std::string jobFile = outDir + "\\_extract_job.txt";
    std::string curFile = outDir + "\\_extract_current.txt";
    std::string failFile = outDir + "\\_extract_failed.txt";

    // A file that was being extracted when the program died is skipped this time.
    std::unordered_set<std::string> crashed;
    for (auto &l : ReadLines(curFile)) {
        JobItem j;
        if (ParseJobLine(l, j)) {
            crashed.insert(j.path);
            AppendText(failFile, JobLine(j) + ";crashed the extractor last time, skipped\r\n");
            Log("Skipping " + j.path + " (it crashed the extractor last time)");
        }
    }
    {
        std::string s;
        for (auto &j : job) s += JobLine(j) + "\r\n";
        WriteText(jobFile, s);
    }
    size_t done = 0, written = 0, skipped = 0, failed = 0;
    uint64_t bytes = 0;
    DWORD lastUi = 0;
    for (auto &j : job) {
        if (g_stop) break;
        ++done;
        std::string rel = j.path;
        for (auto &c : rel) if (c == '/') c = '\\';
        std::string out = outDir + "\\" + rel;
        uint64_t have = 0;
        if (crashed.count(j.path)) { ++failed; continue; }
        if (FileExists(out, &have) && (j.size == 0 ? have > 0 : have == j.size)) { ++skipped; continue; }
        MakeDirs(out.substr(0, out.rfind('\\')));
        WriteText(curFile, JobLine(j) + "\r\n");
        std::string why;
        if (ExtractOne(j, out, why)) {
            ++written;
            bytes += j.size;
        } else if (!g_stop) {
            ++failed;
            AppendText(failFile, JobLine(j) + ";" + why + "\r\n");
            Log("FAILED  " + j.path + "  [" + std::to_string(j.fdid) + "]  " + why);
        }
        DWORD now = GetTickCount();
        if (now - lastUi > 150) {
            lastUi = now;
            PostMessageA(g_wnd, WM_APP_PROGRESS, done, job.size());
            Status("Extracting " + Num(done) + " / " + Num(job.size()) + "   written " + Num(written) +
                   ", already there " + Num(skipped) + ", failed " + Num(failed) + "   " + j.path);
        }
    }
    DeleteFileA(curFile.c_str());
    bool finished = !g_stop;
    if (finished) DeleteFileA(jobFile.c_str());
    PostMessageA(g_wnd, WM_APP_PROGRESS, done, job.size());
    Log(std::string(finished ? "Done. " : "Stopped. ") + "Written " + Num(written) + " (" + Bytes(bytes) +
        "), already there " + Num(skipped) + ", failed " + Num(failed) + "." +
        (failed ? " Failures are listed in " + failFile : "") +
        (finished ? "" : " Press 'Resume last job' to continue."));
    Status(finished ? "Extraction finished." : "Extraction stopped.");
    PostMessageA(g_wnd, WM_APP_DONE, 0, 0);
}

// ---------------------------------------------------------------- matching
struct Matcher {
    std::vector<std::string> subs, wilds;
    std::vector<uint32_t> ids;
    std::vector<std::regex> res;
    bool empty() const { return subs.empty() && wilds.empty() && ids.empty() && res.empty(); }
};
static bool WildMatch(const char *p, const char *s) {
    const char *star = nullptr, *ss = nullptr;
    while (*s) {
        if (*p == '?' || *p == *s) { ++p; ++s; }
        else if (*p == '*') { star = p++; ss = s; }
        else if (star) { p = star + 1; s = ++ss; }
        else return false;
    }
    while (*p == '*') ++p;
    return !*p;
}
static bool BuildMatcher(const std::string &text, Matcher &m, std::string &err) {
    std::string t = text;
    size_t pos = 0;
    while (pos <= t.size()) {
        size_t e = t.find(';', pos);
        if (e == std::string::npos) e = t.size();
        std::string p = Trim(t.substr(pos, e - pos));
        pos = e + 1;
        if (p.empty()) continue;
        if (p.compare(0, 3, "re:") == 0) {
            try {
                m.res.emplace_back(p.substr(3), std::regex::icase | std::regex::optimize);
            } catch (std::exception &ex) {
                err = "Bad regex '" + p.substr(3) + "': " + ex.what();
                return false;
            }
            continue;
        }
        p = Lower(p);
        for (auto &c : p) if (c == '\\') c = '/';
        if (p.find_first_not_of("0123456789") == std::string::npos) m.ids.push_back((uint32_t)strtoul(p.c_str(), 0, 10));
        else if (p.find_first_of("*?") != std::string::npos) m.wilds.push_back(p);
        else m.subs.push_back(p);
    }
    return true;
}
static bool Matches(const Matcher &m, uint32_t f) {
    const char *p = PathOf(f);
    for (auto id : m.ids) if (g_files[f].fdid == id) return true;
    for (auto &s : m.subs) if (strstr(p, s.c_str())) return true;
    for (auto &w : m.wilds) if (WildMatch(w.c_str(), p)) return true;
    for (auto &r : m.res) if (std::regex_search(p, r)) return true;
    return false;
}

static bool NeedOpen() {
    if (g_busy) { Log("Busy, wait for the current step to finish."); return true; }
    if (g_files.empty()) { Log("Press Open first, the tree and filters work once the install is loaded."); return true; }
    return false;
}
static void CheckMatching(bool value) {
    if (NeedOpen()) return;
    Matcher m;
    std::string err;
    if (!BuildMatcher(GetText(ID_FILTER), m, err)) { Log(err); return; }
    if (m.empty()) { Log("Type something in the filter box first."); return; }
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    size_t n = 0;
    for (uint32_t i = 0; i < g_files.size(); ++i)
        if (Matches(m, i)) { SetFile(i, value); ++n; }
    RefreshAll();
    UpdateSummary();
    SetCursor(old);
    Log(std::string(value ? "Checked " : "Unchecked ") + Num(n) + " files matching \"" + GetText(ID_FILTER) + "\"");
}

static void CheckFromList() {
    if (NeedOpen()) return;
    std::string p = PickFile("Text files (*.txt;*.csv)\0*.txt;*.csv\0All files\0*.*\0", "");
    if (p.empty()) return;
    std::unordered_set<std::string> paths;
    std::unordered_set<uint32_t> ids;
    for (auto l : ReadLines(p)) {
        if (l.empty() || l[0] == '#') continue;
        size_t semi = l.find(';');
        std::string a = Trim(l.substr(0, semi));
        std::string b = semi == std::string::npos ? "" : Trim(l.substr(semi + 1));
        if (!a.empty() && a.find_first_not_of("0123456789") == std::string::npos) ids.insert((uint32_t)strtoul(a.c_str(), 0, 10));
        else if (!a.empty()) paths.insert(a);
    }
    std::unordered_set<std::string> lp;
    for (auto s : paths) {
        s = Lower(s);
        for (auto &c : s) if (c == '\\') c = '/';
        lp.insert(s);
    }
    size_t n = 0;
    std::unordered_set<std::string> foundP;
    std::unordered_set<uint32_t> foundI;
    for (uint32_t i = 0; i < g_files.size(); ++i) {
        bool hit = false;
        if (ids.count(g_files[i].fdid)) { hit = true; foundI.insert(g_files[i].fdid); }
        if (!lp.empty() && lp.count(PathOf(i))) { hit = true; foundP.insert(PathOf(i)); }
        if (hit) { SetFile(i, true); ++n; }
    }
    RefreshAll();
    UpdateSummary();
    size_t missing = (ids.size() - foundI.size()) + (lp.size() - foundP.size());
    Log("List: checked " + Num(n) + " files; " + Num(missing) + " entries were not found in this install.");
    for (auto id : ids) if (!foundI.count(id)) Log("  not in this install: " + std::to_string(id));
    for (auto &s : lp) if (!foundP.count(s)) Log("  not in this install: " + s);
}

// ---------------------------------------------------------------- UI state
static void UpdateButtons() {
    bool busy = g_busy, loaded = !g_files.empty() && g_storage;
    for (int id : {ID_OPEN, ID_BR_INSTALL, ID_BR_LIST, ID_DL_LIST, ID_CHECK, ID_UNCHECK, ID_LISTCHK, ID_CLEAR})
        EnableWindow(GetDlgItem(g_wnd, id), !busy);
    EnableWindow(GetDlgItem(g_wnd, ID_EXTRACT), !busy && loaded && !g_demo);
    std::string jf = GetText(ID_OUT) + "\\_extract_job.txt";
    EnableWindow(GetDlgItem(g_wnd, ID_RESUME), !busy && g_storage && FileExists(jf));
    EnableWindow(GetDlgItem(g_wnd, ID_STOP), busy);
}

static void StartOpen() {
    std::string install = Trim(GetText(ID_INSTALL)), listfile = Trim(GetText(ID_LIST));
    if (install.empty()) { Log("Choose the WoW install folder first."); return; }
    if (!listfile.empty() && !FileExists(listfile)) {
        Log("No listfile yet, downloading the community listfile first (about 150 MB, one time only) ...");
        g_openAfterDownload = true;
        SendMessageA(g_wnd, WM_COMMAND, ID_DL_LIST, 0);
        return;
    }
    SaveSettings();
    Log("Opening " + install + " ... this takes a minute (reading about 2 million file entries).");
    TreeView_DeleteAllItems(g_tree);
    g_files.clear();
    g_dirs.clear();
    g_busy = true;
    g_stop = false;
    UpdateButtons();
    std::thread(OpenWorker, install, listfile).detach();
}

static void StartExtract(bool resume) {
    std::string outDir = Trim(GetText(ID_OUT));
    while (!outDir.empty() && (outDir.back() == '\\' || outDir.back() == '/')) outDir.pop_back();
    if (outDir.empty()) { Log("Choose an output folder first."); return; }
    MakeDirs(outDir);
    std::vector<JobItem> job;
    if (resume) {
        for (auto &l : ReadLines(outDir + "\\_extract_job.txt")) {
            JobItem j;
            if (ParseJobLine(l, j)) job.push_back(j);
        }
    } else {
        for (uint32_t i = 0; i < g_files.size(); ++i)
            if (g_checked[i]) job.push_back({g_files[i].fdid, g_files[i].size, PathOf(i)});
    }
    if (job.empty()) { Log(resume ? "There is no unfinished job in the output folder." : "Nothing is checked."); return; }
    SaveSettings();
    g_busy = true;
    g_stop = false;
    UpdateButtons();
    SendMessageA(g_progress, PBM_SETRANGE32, 0, (LPARAM)job.size());
    SendMessageA(g_progress, PBM_SETPOS, 0, 0);
    Log(std::string(resume ? "Resuming: " : "Extracting ") + Num(job.size()) + " files to " + outDir);
    std::thread(ExtractWorker, outDir, std::move(job)).detach();
}

// ---------------------------------------------------------------- layout
static HWND Ctl(const char *cls, const char *text, int id, DWORD style, DWORD ex = 0) {
    HWND h = CreateWindowExA(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleA(nullptr), nullptr);
    SendMessageA(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}

static void Layout() {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    int W = rc.right, H = rc.bottom, m = S(8), rh = S(24), lw = S(80), bw = S(90), gap = S(6);
    auto mv = [](int id, int x, int y, int w, int h) { MoveWindow(GetDlgItem(g_wnd, id), x, y, w, h, TRUE); };
    int y = m;
    int rows[3][4] = {{ID_L1, ID_INSTALL, ID_BR_INSTALL, ID_OPEN},
                      {ID_L2, ID_LIST, ID_BR_LIST, ID_DL_LIST},
                      {ID_L3, ID_OUT, ID_BR_OUT, ID_OPEN_OUT}};
    for (auto &r : rows) {
        mv(r[0], m, y + S(4), lw, rh);
        int ew = W - m * 2 - lw - (bw + gap) * 2 - gap;
        mv(r[1], m + lw, y, ew, rh);
        mv(r[2], m + lw + ew + gap, y, bw, rh);
        mv(r[3], m + lw + ew + gap * 2 + bw, y, bw, rh);
        y += rh + gap;
    }
    {
        int bw2 = S(120);
        mv(ID_L4, m, y + S(4), lw, rh);
        int ew = W - m * 2 - lw - (bw2 + gap) * 4 - gap;
        mv(ID_FILTER, m + lw, y, ew, rh);
        int x = m + lw + ew + gap;
        for (int id : {ID_CHECK, ID_UNCHECK, ID_LISTCHK, ID_CLEAR}) { mv(id, x, y, bw2, rh); x += bw2 + gap; }
        y += rh + gap;
    }
    int logH = S(130), bottom = rh * 2 + gap * 3 + logH;
    mv(ID_TREE, m, y, W - 2 * m, H - y - bottom - m);
    int y2 = H - bottom - m + gap;
    mv(ID_STATUS, m, y2 + S(4), W - 2 * m, rh);
    y2 += rh + gap;
    int bx = W - m - (S(120) + gap) * 3;
    mv(ID_PROGRESS, m, y2, bx - m - gap, rh);
    mv(ID_EXTRACT, bx, y2, S(120), rh);
    mv(ID_RESUME, bx + S(120) + gap, y2, S(120), rh);
    mv(ID_STOP, bx + (S(120) + gap) * 2, y2, S(120), rh);
    y2 += rh + gap;
    mv(ID_LOG, m, y2, W - 2 * m, H - y2 - m);
}

static void AppendLog(const std::string &s) {
    std::string line = s + "\r\n";
    int len = GetWindowTextLengthA(g_log);
    if (len > 400000) {  // keep the box responsive
        SendMessageA(g_log, EM_SETSEL, 0, 100000);
        SendMessageA(g_log, EM_REPLACESEL, FALSE, (LPARAM) "");
        len = GetWindowTextLengthA(g_log);
    }
    SendMessageA(g_log, EM_SETSEL, len, len);
    SendMessageA(g_log, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
}

static void CreateUi() {
    NONCLIENTMETRICSA ncm = {sizeof ncm};
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectA(&ncm.lfMessageFont);
    const DWORD B = BS_PUSHBUTTON | WS_TABSTOP, E = ES_AUTOHSCROLL | WS_TABSTOP;
    Ctl("STATIC", "WoW install:", ID_L1, 0);
    Ctl("EDIT", "", ID_INSTALL, E, WS_EX_CLIENTEDGE);
    Ctl("BUTTON", "Browse...", ID_BR_INSTALL, B);
    Ctl("BUTTON", "Open", ID_OPEN, B);
    Ctl("STATIC", "Listfile:", ID_L2, 0);
    Ctl("EDIT", "", ID_LIST, E, WS_EX_CLIENTEDGE);
    Ctl("BUTTON", "Browse...", ID_BR_LIST, B);
    Ctl("BUTTON", "Download", ID_DL_LIST, B);
    Ctl("STATIC", "Output:", ID_L3, 0);
    Ctl("EDIT", "", ID_OUT, E, WS_EX_CLIENTEDGE);
    Ctl("BUTTON", "Browse...", ID_BR_OUT, B);
    Ctl("BUTTON", "Show folder", ID_OPEN_OUT, B);
    Ctl("STATIC", "Filter:", ID_L4, 0);
    Ctl("EDIT", "", ID_FILTER, E, WS_EX_CLIENTEDGE);
    Ctl("BUTTON", "Check matching", ID_CHECK, B);
    Ctl("BUTTON", "Uncheck matching", ID_UNCHECK, B);
    Ctl("BUTTON", "Check from list...", ID_LISTCHK, B);
    Ctl("BUTTON", "Uncheck all", ID_CLEAR, B);
    g_tree = Ctl(WC_TREEVIEWA, "", ID_TREE,
                 TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | WS_TABSTOP | WS_VSCROLL,
                 WS_EX_CLIENTEDGE);
    TreeView_SetImageList(g_tree, MakeCheckImages(), TVSIL_STATE);
    g_status = Ctl("STATIC", "Choose the WoW install folder and press Open.", ID_STATUS, SS_LEFTNOWORDWRAP);
    g_progress = Ctl(PROGRESS_CLASSA, "", ID_PROGRESS, 0);
    Ctl("BUTTON", "Extract checked", ID_EXTRACT, B);
    Ctl("BUTTON", "Resume last job", ID_RESUME, B);
    Ctl("BUTTON", "Stop", ID_STOP, B);
    g_log = Ctl("EDIT", "", ID_LOG, ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, WS_EX_CLIENTEDGE);
    SendMessageA(g_log, EM_SETLIMITTEXT, 0, 0);

    SetText(ID_INSTALL, IniGet("install", "F:\\World of Warcraft - Classic Forever"));
    SetText(ID_LIST, IniGet("listfile", (g_exeDir + "\\community-listfile.csv").c_str()));
    SetText(ID_OUT, IniGet("output", "E:\\TEMP\\WoW stuff\\HD PC models\\extracted"));
    SetText(ID_FILTER, IniGet("filter", ""));
    AppendLog("Filter syntax: plain text = path contains it; * and ? = wildcards on the full path; "
              "a number = file ID; re:... = regular expression. Separate several with ';'.");
    AppendLog("Click a checkbox (or press Space) to tick a file or a whole folder. Files that fail are skipped "
              "and listed in _extract_failed.txt in the output folder.");
    Layout();
    UpdateButtons();
}

// ---------------------------------------------------------------- window proc
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_wnd = h;
        CreateUi();
        return 0;
    case WM_SIZE:
        Layout();
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize = {S(820), S(560)};
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BR_INSTALL: {
            std::string p = PickFolder(GetText(ID_INSTALL));
            if (!p.empty()) SetText(ID_INSTALL, p);
            break;
        }
        case ID_BR_LIST: {
            std::string p = PickFile("Listfile (*.csv;*.txt)\0*.csv;*.txt\0All files\0*.*\0", "");
            if (!p.empty()) SetText(ID_LIST, p);
            break;
        }
        case ID_BR_OUT: {
            std::string p = PickFolder(GetText(ID_OUT));
            if (!p.empty()) { SetText(ID_OUT, p); UpdateButtons(); }
            break;
        }
        case ID_OPEN_OUT: {
            std::string p = Trim(GetText(ID_OUT));
            MakeDirs(p);
            ShellExecuteA(h, "open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case ID_DL_LIST: {
            std::string dest = Trim(GetText(ID_LIST));
            if (dest.empty()) { dest = g_exeDir + "\\community-listfile.csv"; SetText(ID_LIST, dest); }
            g_busy = true;
            g_stop = false;
            UpdateButtons();
            std::thread(DownloadWorker, dest).detach();
            break;
        }
        case ID_OPEN: StartOpen(); break;
        case ID_CHECK: CheckMatching(true); break;
        case ID_UNCHECK: CheckMatching(false); break;
        case ID_LISTCHK: CheckFromList(); break;
        case ID_CLEAR:
            if (!g_busy && !g_files.empty()) {
                std::fill(g_checked.begin(), g_checked.end(), 0);
                for (auto &d : g_dirs) d.checked = 0;
                RefreshAll();
                UpdateSummary();
            }
            break;
        case ID_EXTRACT: StartExtract(false); break;
        case ID_RESUME: StartExtract(true); break;
        case ID_STOP: g_stop = true; break;
        case ID_OUT:
            if (HIWORD(wp) == EN_CHANGE) UpdateButtons();
            break;
        case IDOK:  // Enter in the filter box = check matching
            if (GetFocus() == GetDlgItem(h, ID_FILTER)) CheckMatching(true);
            break;
        }
        return 0;
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->idFrom != ID_TREE) break;
        if (n->code == TVN_GETDISPINFOA) {
            NMTVDISPINFOA *di = (NMTVDISPINFOA *)lp;
            if (di->item.mask & TVIF_TEXT) {
                std::string t;
                LPARAM p = di->item.lParam;
                if (p & 1) {
                    uint32_t f = (uint32_t)(p >> 1);
                    t = std::string(PlainOf(f)) + "    " + Bytes(g_files[f].size);
                    if (g_files[f].fdid != CASC_INVALID_ID) t += "   [" + std::to_string(g_files[f].fdid) + "]";
                    if (!g_files[f].avail) t += "   (not on disk)";
                } else {
                    int d = (int)(p >> 1);
                    t = g_dirs[d].name + "    (" + Num(g_dirs[d].total) + ")";
                }
                lstrcpynA(di->item.pszText, t.c_str(), di->item.cchTextMax);
            }
            return 0;
        }
        if (n->code == TVN_ITEMEXPANDINGA) {
            NMTREEVIEWA *tv = (NMTREEVIEWA *)lp;
            if (tv->action == TVE_EXPAND && !(tv->itemNew.lParam & 1)) PopulateDir((int)(tv->itemNew.lParam >> 1));
            return 0;
        }
        if (n->code == NM_CLICK) {
            TVHITTESTINFO ht = {};
            DWORD pos = GetMessagePos();
            ht.pt = {(short)LOWORD(pos), (short)HIWORD(pos)};
            ScreenToClient(g_tree, &ht.pt);
            TreeView_HitTest(g_tree, &ht);
            if (ht.hItem && (ht.flags & TVHT_ONITEMSTATEICON)) ToggleItem(ht.hItem);
            return 0;
        }
        if (n->code == TVN_KEYDOWN && ((NMTVKEYDOWN *)lp)->wVKey == VK_SPACE) {
            ToggleItem(TreeView_GetSelection(g_tree));
            return 1;
        }
        break;
    }
    case WM_APP_STATUS:
    case WM_APP_LOG: {
        std::string *s = (std::string *)lp;
        if (msg == WM_APP_STATUS) SetWindowTextA(g_status, s->c_str());
        else AppendLog(*s);
        delete s;
        return 0;
    }
    case WM_APP_OPENED:
        g_busy = false;
        if (wp) {
            PopulateDir(0);
            UpdateSummary();
        } else {
            SetWindowTextA(g_status, "Could not open the install. See the log below.");
        }
        UpdateButtons();
        return 0;
    case WM_APP_PROGRESS:
        SendMessageA(g_progress, PBM_SETRANGE32, 0, lp);
        SendMessageA(g_progress, PBM_SETPOS, wp, 0);
        return 0;
    case WM_APP_DONE:
    case WM_APP_DLDONE:
        g_busy = false;
        UpdateButtons();
        if (msg == WM_APP_DLDONE) {
            SetWindowTextA(g_status, wp ? "Listfile downloaded." : "Listfile download failed.");
            if (wp && (g_openAfterDownload || g_files.empty())) StartOpen();
            g_openAfterDownload = false;
        }
        return 0;
    case WM_CLOSE:
        if (g_busy) {
            if (MessageBoxA(h, "Work is still running. Stop and quit?", APP_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            g_stop = true;
            Sleep(300);
        }
        SaveSettings();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_TREEVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    g_exeDir = exe;
    g_exeDir = g_exeDir.substr(0, g_exeDir.rfind('\\'));
    g_iniPath = g_exeDir + "\\CascExtractor.ini";
    g_demo = strstr(GetCommandLineA(), "--demo") != nullptr;
    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);

    WNDCLASSEXA wc = {sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "CascExtractorWnd";
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
    RegisterClassExA(&wc);
    HWND w = CreateWindowExA(0, wc.lpszClassName, APP_TITLE, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             S(1100), S(780), nullptr, nullptr, inst, nullptr);
    ShowWindow(w, show);
    if (g_demo) PostMessageA(w, WM_COMMAND, ID_OPEN, 0);
    MSG m;
    while (GetMessageA(&m, nullptr, 0, 0)) {
        if (IsDialogMessageA(w, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    CoUninitialize();
    return 0;
}

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <stdint.h>
#include <stdexcept>

#define IDC_BUTTON_APPLY 1002
#define IDC_STATUS 1005
#define IDC_BUTTON_DNS 1006
#define IDC_BUTTON_IPV6 1007
#define IDC_HOSTS_DIALOG_EDIT 2001
#define IDC_HOSTS_DIALOG_OK 2002
#define IDC_HOSTS_DIALOG_CANCEL 2003
#define IDC_DNS_DIALOG_EDIT 2101
#define IDC_DNS_DIALOG_OK 2102
#define IDC_DNS_DIALOG_CANCEL 2103
#define IDI_APPICON 101
#define IDT_REFRESH_STATE 3001

static const wchar_t *kWindowTitle = L"Windows 网络配置小工具";
static const char *kMarkerBegin = "# >>> TempWindowsHosts BEGIN";
static const char *kMarkerEnd = "# <<< TempWindowsHosts END";

static HWND g_status = NULL;
static HWND g_applyButton = NULL;
static HWND g_dnsButton = NULL;
static HWND g_ipv6Button = NULL;
static std::string g_originalHosts;
static std::wstring g_backupPath;
static std::wstring g_latestBackupPath;
static bool g_restored = false;
static bool g_hostsModified = false;
static bool g_restoringOnClose = false;
static bool g_recoveredPreviousSession = false;
static bool g_dnsModified = false;
static bool g_ipv6Modified = false;
static std::wstring g_dnsAdapterFriendlyName;
static DWORD g_dnsInterfaceIndex = 0;
static DWORD g_ipv6InterfaceIndex = 0;
static std::wstring g_activeSessionPath;
static std::wstring g_lastDnsInput;

static void FlushDns();

static std::wstring Utf8ToWide(const std::string &text);
static std::string WideToUtf8(const std::wstring &text);

static bool IsRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(
            &ntAuthority,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0,
            &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }

    return isAdmin == TRUE;
}

static bool RelaunchAsAdmin() {
    wchar_t exePath[MAX_PATH] = {0};
    if (!GetModuleFileNameW(NULL, exePath, MAX_PATH)) {
        return false;
    }

    SHELLEXECUTEINFOW info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    info.lpVerb = L"runas";
    info.lpFile = exePath;
    info.nShow = SW_SHOWNORMAL;

    return ShellExecuteExW(&info) == TRUE;
}

static std::wstring GetLastErrorText(DWORD error) {
    wchar_t *buffer = NULL;
    DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&buffer,
        0,
        NULL);

    std::wstring message = size ? std::wstring(buffer, size) : L"Unknown error";
    if (buffer) LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}

static std::runtime_error Win32Error(const char *action, const std::wstring &path, DWORD error) {
    std::wstring message = Utf8ToWide(action);
    message += L"\r\n";
    message += path;
    message += L"\r\n";
    message += L"错误码 ";
    message += std::to_wstring(error);
    message += L": ";
    message += GetLastErrorText(error);
    return std::runtime_error(WideToUtf8(message));
}

static std::wstring GetHostsPath() {
    wchar_t systemDir[MAX_PATH] = {0};
    GetSystemDirectoryW(systemDir, MAX_PATH);
    std::wstring path(systemDir);
    path += L"\\drivers\\etc\\hosts";
    return path;
}

static std::wstring GetStateDir() {
    wchar_t programData[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, SHGFP_TYPE_CURRENT, programData))) {
        return L"C:\\ProgramData\\TempWindowsHosts";
    }
    std::wstring path(programData);
    path += L"\\TempWindowsHosts";
    return path;
}

static std::string ReadFileBytes(const std::wstring &path) {
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (file == INVALID_HANDLE_VALUE) {
        throw Win32Error("读取文件失败", path, GetLastError());
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size)) {
        DWORD error = GetLastError();
        CloseHandle(file);
        throw Win32Error("获取文件大小失败", path, error);
    }
    if (size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        throw std::runtime_error("hosts 文件异常过大，已拒绝读取。");
    }

    std::string data(static_cast<size_t>(size.QuadPart), '\0');
    DWORD total = 0;
    while (total < data.size()) {
        DWORD chunk = 0;
        DWORD want = static_cast<DWORD>(std::min<size_t>(data.size() - total, 1024 * 1024));
        if (!ReadFile(file, &data[total], want, &chunk, NULL)) {
            DWORD error = GetLastError();
            CloseHandle(file);
            throw Win32Error("读取文件失败", path, error);
        }
        if (chunk == 0) break;
        total += chunk;
    }
    CloseHandle(file);
    data.resize(total);
    return data;
}

static void WriteFileBytes(const std::wstring &path, const std::string &data) {
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);

    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (file == INVALID_HANDLE_VALUE) {
        throw Win32Error("写入文件失败", path, GetLastError());
    }

    DWORD total = 0;
    while (total < data.size()) {
        DWORD written = 0;
        DWORD want = static_cast<DWORD>(std::min<size_t>(data.size() - total, 1024 * 1024));
        if (!WriteFile(file, data.data() + total, want, &written, NULL)) {
            DWORD error = GetLastError();
            CloseHandle(file);
            throw Win32Error("写入文件失败", path, error);
        }
        if (written == 0) {
            CloseHandle(file);
            throw std::runtime_error("写入文件失败：系统返回 0 字节写入。");
        }
        total += written;
    }

    if (!FlushFileBuffers(file)) {
        DWORD error = GetLastError();
        CloseHandle(file);
        throw Win32Error("刷新文件失败", path, error);
    }

    CloseHandle(file);
}

static std::wstring Utf8ToWide(const std::string &text) {
    if (text.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), NULL, 0);
    if (size <= 0) {
        size = MultiByteToWideChar(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), NULL, 0);
        std::wstring out(size, L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size);
        return out;
    }
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size);
    return out;
}

static std::string WideToUtf8(const std::wstring &text) {
    if (text.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), NULL, 0, NULL, NULL);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size, NULL, NULL);
    return out;
}

static std::string TrimRight(const std::string &text) {
    size_t end = text.size();
    while (end > 0 && (text[end - 1] == '\r' || text[end - 1] == '\n' || text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    return text.substr(0, end);
}

static std::string Trim(const std::string &text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == '\r' || text[begin] == '\n' || text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == '\r' || text[end - 1] == '\n' || text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

static std::string RemoveManagedBlock(const std::string &text, bool *removed) {
    std::string out = text;
    bool didRemove = false;
    for (;;) {
        size_t begin = out.find(kMarkerBegin);
        if (begin == std::string::npos) break;
        size_t lineBegin = out.rfind('\n', begin);
        lineBegin = (lineBegin == std::string::npos) ? 0 : lineBegin + 1;
        size_t end = out.find(kMarkerEnd, begin);
        if (end == std::string::npos) break;
        end += strlen(kMarkerEnd);
        if (end < out.size() && out[end] == '\r') ++end;
        if (end < out.size() && out[end] == '\n') ++end;
        out.erase(lineBegin, end - lineBegin);
        didRemove = true;
    }
    if (removed) *removed = didRemove;
    return out;
}

static uint64_t Fnv1a64(const std::string &data) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static std::wstring SaveBackup(const std::string &data) {
    std::wstring dir = GetStateDir();
    CreateDirectoryW(dir.c_str(), NULL);

    std::wstringstream name;
    name << dir << L"\\hosts.baseline." << std::hex << std::setw(16) << std::setfill(L'0') << Fnv1a64(data) << L".bak";
    std::wstring backup = name.str();

    DWORD attrs = GetFileAttributesW(backup.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        WriteFileBytes(backup, data);
    }

    g_latestBackupPath = dir + L"\\hosts.latest-baseline.bak";
    WriteFileBytes(g_latestBackupPath, data);
    return backup;
}

static bool FileExists(const std::wstring &path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring GetLatestBackupPath() {
    std::wstring dir = GetStateDir();
    return dir + L"\\hosts.latest-baseline.bak";
}

static std::wstring GetActiveSessionPath() {
    std::wstring dir = GetStateDir();
    return dir + L"\\hosts.active-session";
}

static std::string LoadStartupBaseline(const std::string &rawHosts, bool *removedTempBlock) {
    g_latestBackupPath = GetLatestBackupPath();
    g_activeSessionPath = GetActiveSessionPath();
    if (FileExists(g_activeSessionPath) && FileExists(g_latestBackupPath)) {
        g_recoveredPreviousSession = true;
        std::string latest = ReadFileBytes(g_latestBackupPath);
        WriteFileBytes(GetHostsPath(), latest);
        FlushDns();
        DeleteFileW(g_activeSessionPath.c_str());
        if (removedTempBlock) *removedTempBlock = false;
        return latest;
    }

    bool removed = false;
    std::string cleaned = TrimRight(RemoveManagedBlock(rawHosts, &removed)) + "\r\n";
    if (removedTempBlock) *removedTempBlock = removed;

    if (removed && FileExists(g_latestBackupPath)) {
        g_recoveredPreviousSession = true;
        std::string latest = ReadFileBytes(g_latestBackupPath);
        WriteFileBytes(GetHostsPath(), latest);
        FlushDns();
        DeleteFileW(g_activeSessionPath.c_str());
        return latest;
    }

    return cleaned;
}

static void SetText(HWND hwnd, const std::wstring &text) {
    SetWindowTextW(hwnd, text.c_str());
}

static void AppendLog(const std::wstring &text) {
    if (!g_status) return;

    int len = GetWindowTextLengthW(g_status);
    std::wstring entry = text;
    if (len > 0) {
        entry = L"\r\n\r\n" + entry;
    }

    SendMessageW(g_status, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_status, EM_REPLACESEL, FALSE, (LPARAM)entry.c_str());
    SendMessageW(g_status, EM_SCROLLCARET, 0, 0);
}

static void RefreshHostsModifiedState(bool updateButtons);

static void UpdateActionButtons() {
    if (g_applyButton) {
        SetWindowTextW(g_applyButton, g_hostsModified ? L"恢复 hosts" : L"修改 hosts");
    }
    if (g_dnsButton) {
        SetWindowTextW(g_dnsButton, g_dnsModified ? L"恢复 DNS" : L"修改 DNS");
    }
    if (g_ipv6Button) {
        SetWindowTextW(g_ipv6Button, g_ipv6Modified ? L"恢复 IPv6" : L"关闭 IPv6");
    }
}

static void RefreshHostsModifiedState(bool updateButtons) {
    try {
        g_hostsModified = ReadFileBytes(GetHostsPath()) != g_originalHosts;
    } catch (...) {
        return;
    }
    if (updateButtons) {
        UpdateActionButtons();
    }
}

static std::wstring GetEditText(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    std::wstring text(len, L'\0');
    GetWindowTextW(hwnd, &text[0], len + 1);
    return text;
}

struct HostsDialogState {
    std::wstring text;
    bool accepted;
};

static LRESULT CALLBACK HostsDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HostsDialogState *state = reinterpret_cast<HostsDialogState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        state = reinterpret_cast<HostsDialogState *>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        HFONT mono = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");

        HWND label = CreateWindowW(L"STATIC", L"请输入临时 hosts 记录", WS_CHILD | WS_VISIBLE, 16, 14, 260, 22, hwnd, NULL, NULL, NULL);
        SendMessageW(label, WM_SETFONT, (WPARAM)font, TRUE);

        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            16, 42, 520, 190, hwnd, (HMENU)IDC_HOSTS_DIALOG_EDIT, NULL, NULL);
        SendMessageW(edit, WM_SETFONT, (WPARAM)mono, TRUE);
        SetWindowTextW(edit, state->text.c_str());

        HWND ok = CreateWindowW(L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 330, 248, 96, 32, hwnd, (HMENU)IDC_HOSTS_DIALOG_OK, NULL, NULL);
        HWND cancel = CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 440, 248, 96, 32, hwnd, (HMENU)IDC_HOSTS_DIALOG_CANCEL, NULL, NULL);
        SendMessageW(ok, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(cancel, WM_SETFONT, (WPARAM)font, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_HOSTS_DIALOG_OK) {
            state->text = GetEditText(GetDlgItem(hwnd, IDC_HOSTS_DIALOG_EDIT));
            state->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == IDC_HOSTS_DIALOG_CANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool PromptHostsInput(HWND parent, std::wstring *outText) {
    const wchar_t CLASS_NAME[] = L"TempWindowsHostsInputDialog";
    static bool registered = false;
    HINSTANCE instance = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = HostsDialogProc;
        wc.hInstance = instance;
        wc.lpszClassName = CLASS_NAME;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
        RegisterClassW(&wc);
        registered = true;
    }

    HostsDialogState state;
    state.text = Utf8ToWide(ReadFileBytes(GetHostsPath()));
    state.accepted = false;

    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        CLASS_NAME,
        L"修改 hosts",
        WS_CAPTION | WS_SYSMENU | WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, 570, 330,
        parent,
        NULL,
        instance,
        &state);

    if (!dialog) {
        return false;
    }

    RECT parentRect;
    RECT dialogRect;
    GetWindowRect(parent, &parentRect);
    GetWindowRect(dialog, &dialogRect);
    int width = dialogRect.right - dialogRect.left;
    int height = dialogRect.bottom - dialogRect.top;
    int x = parentRect.left + ((parentRect.right - parentRect.left) - width) / 2;
    int y = parentRect.top + ((parentRect.bottom - parentRect.top) - height) / 2;
    SetWindowPos(dialog, HWND_TOP, x, y, width, height, SWP_SHOWWINDOW);

    EnableWindow(parent, FALSE);
    MSG msg;
    while (IsWindow(dialog) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    if (state.accepted) {
        *outText = state.text;
        return true;
    }
    return false;
}

static LRESULT CALLBACK DnsDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HostsDialogState *state = reinterpret_cast<HostsDialogState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        state = reinterpret_cast<HostsDialogState *>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        HFONT mono = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");

        HWND label = CreateWindowW(L"STATIC", L"请输入 DNS 服务器", WS_CHILD | WS_VISIBLE, 16, 14, 260, 22, hwnd, NULL, NULL, NULL);
        SendMessageW(label, WM_SETFONT, (WPARAM)font, TRUE);

        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            16, 42, 360, 90, hwnd, (HMENU)IDC_DNS_DIALOG_EDIT, NULL, NULL);
        SendMessageW(edit, WM_SETFONT, (WPARAM)mono, TRUE);
        SetWindowTextW(edit, state->text.c_str());

        HWND ok = CreateWindowW(L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 170, 148, 96, 32, hwnd, (HMENU)IDC_DNS_DIALOG_OK, NULL, NULL);
        HWND cancel = CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 280, 148, 96, 32, hwnd, (HMENU)IDC_DNS_DIALOG_CANCEL, NULL, NULL);
        SendMessageW(ok, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(cancel, WM_SETFONT, (WPARAM)font, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_DNS_DIALOG_OK) {
            state->text = GetEditText(GetDlgItem(hwnd, IDC_DNS_DIALOG_EDIT));
            state->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == IDC_DNS_DIALOG_CANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool PromptDnsInput(HWND parent, std::wstring *outText) {
    const wchar_t CLASS_NAME[] = L"TempWindowsHostsDnsDialog";
    static bool registered = false;
    HINSTANCE instance = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = DnsDialogProc;
        wc.hInstance = instance;
        wc.lpszClassName = CLASS_NAME;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
        RegisterClassW(&wc);
        registered = true;
    }

    HostsDialogState state;
    state.text = g_lastDnsInput.empty() ? L"1.1.1.1\r\n8.8.8.8" : g_lastDnsInput;
    state.accepted = false;

    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        CLASS_NAME,
        L"修改 DNS",
        WS_CAPTION | WS_SYSMENU | WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, 410, 230,
        parent,
        NULL,
        instance,
        &state);

    if (!dialog) return false;

    RECT parentRect;
    RECT dialogRect;
    GetWindowRect(parent, &parentRect);
    GetWindowRect(dialog, &dialogRect);
    int width = dialogRect.right - dialogRect.left;
    int height = dialogRect.bottom - dialogRect.top;
    int x = parentRect.left + ((parentRect.right - parentRect.left) - width) / 2;
    int y = parentRect.top + ((parentRect.bottom - parentRect.top) - height) / 2;
    SetWindowPos(dialog, HWND_TOP, x, y, width, height, SWP_SHOWWINDOW);

    EnableWindow(parent, FALSE);
    MSG msg;
    while (IsWindow(dialog) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    if (state.accepted) {
        g_lastDnsInput = state.text;
        *outText = state.text;
        return true;
    }
    return false;
}

static void FlushDns() {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    wchar_t cmd[] = L"ipconfig.exe /flushdns";
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

static DWORD GetActiveInterfaceIndex() {
    DWORD ifIndex = 0;
    IPAddr destination = inet_addr("8.8.8.8");
    DWORD result = GetBestInterface(destination, &ifIndex);
    if (result != NO_ERROR || ifIndex == 0) {
        throw std::runtime_error("无法定位当前正在使用的网卡。");
    }
    return ifIndex;
}

static std::wstring GetInterfaceFriendlyName(DWORD ifIndex) {
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
    ULONG size = 0;
    GetAdaptersAddresses(AF_UNSPEC, flags, NULL, NULL, &size);
    std::vector<unsigned char> buffer(size);
    IP_ADAPTER_ADDRESSES *addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(&buffer[0]);
    DWORD result = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, addresses, &size);
    if (result != NO_ERROR) {
        return L"InterfaceIndex " + std::to_wstring(ifIndex);
    }

    for (IP_ADAPTER_ADDRESSES *item = addresses; item; item = item->Next) {
        if (item->IfIndex == ifIndex || item->Ipv6IfIndex == ifIndex) {
            return item->FriendlyName ? item->FriendlyName : (L"InterfaceIndex " + std::to_wstring(ifIndex));
        }
    }
    return L"InterfaceIndex " + std::to_wstring(ifIndex);
}

static void RunCommandHidden(const std::wstring &commandLine, const char *action) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    if (!CreateProcessW(NULL, &mutableCommand[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        throw Win32Error(action, commandLine, GetLastError());
    }

    WaitForSingleObject(pi.hProcess, 30000);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0) {
        std::wstring message = Utf8ToWide(action);
        message += L"失败，退出码 ";
        message += std::to_wstring(exitCode);
        message += L"\r\n";
        message += commandLine;
        throw std::runtime_error(WideToUtf8(message));
    }
}

static std::wstring QuotePowerShellString(const std::wstring &text) {
    std::wstring out = L"'";
    for (wchar_t ch : text) {
        if (ch == L'\'') out += L"''";
        else out += ch;
    }
    out += L"'";
    return out;
}

static void RunPowerShellHidden(const std::wstring &script, const char *action) {
    std::wstring logPath = GetStateDir() + L"\\last-powershell-error.log";
    CreateDirectoryW(GetStateDir().c_str(), NULL);

    std::wstring command = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"";
    command += L"try { ";
    command += script;
    command += L" } catch { $_ | Out-String | Set-Content -Encoding UTF8 -Path ";
    command += QuotePowerShellString(logPath);
    command += L"; exit 1 }\"";

    try {
        RunCommandHidden(command, action);
    } catch (const std::exception &ex) {
        std::wstring message = Utf8ToWide(ex.what());
        if (FileExists(logPath)) {
            std::string output = ReadFileBytes(logPath);
            if (!output.empty()) {
                message += L"\r\n\r\nPowerShell 输出：\r\n";
                message += Utf8ToWide(output);
            }
        }
        throw std::runtime_error(WideToUtf8(message));
    }
}

static std::vector<std::wstring> ParseDnsServers(const std::wstring &input) {
    std::vector<std::wstring> servers;
    std::wstring current;
    for (wchar_t ch : input) {
        if (ch == L',' || ch == L';' || ch == L'\r' || ch == L'\n' || ch == L'\t' || ch == L' ') {
            if (!current.empty()) {
                servers.push_back(current);
                current.clear();
            }
        } else {
            current += ch;
        }
    }
    if (!current.empty()) {
        servers.push_back(current);
    }
    return servers;
}

static std::wstring PowerShellArray(const std::vector<std::wstring> &items) {
    std::wstring out = L"(";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += L",";
        out += QuotePowerShellString(items[i]);
    }
    out += L")";
    return out;
}

static void ApplyDns(HWND hwnd, const std::wstring &dnsInput) {
    std::vector<std::wstring> servers = ParseDnsServers(dnsInput);
    if (servers.empty()) {
        MessageBoxW(hwnd, L"请输入 DNS 服务器。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }

    DWORD ifIndex = GetActiveInterfaceIndex();
    std::wstring adapterName = GetInterfaceFriendlyName(ifIndex);

    std::wstring ps = L"$ErrorActionPreference='Stop'; ";
    ps += L"$i=" + std::to_wstring(ifIndex) + L"; ";
    ps += L"Set-DnsClientServerAddress -InterfaceIndex $i -ServerAddresses ";
    ps += PowerShellArray(servers);

    RunPowerShellHidden(ps, "修改 DNS");
    FlushDns();

    g_dnsInterfaceIndex = ifIndex;
    g_dnsAdapterFriendlyName = adapterName;
    g_dnsModified = true;
    UpdateActionButtons();

    std::wstring status = L"DNS 修改成功。\r\n网卡：";
    status += adapterName;
    status += L"\r\nDNS：";
    for (size_t i = 0; i < servers.size(); ++i) {
        if (i) status += L", ";
        status += servers[i];
    }
    status += L"\r\n恢复时会改回自动获取 DNS。";
    AppendLog(status);
}

static void RestoreDns() {
    DWORD ifIndex = g_dnsInterfaceIndex ? g_dnsInterfaceIndex : GetActiveInterfaceIndex();
    std::wstring ps = L"$ErrorActionPreference='Stop'; ";
    ps += L"$i=" + std::to_wstring(ifIndex) + L"; ";
    ps += L"Set-DnsClientServerAddress -InterfaceIndex $i -ResetServerAddresses";

    RunPowerShellHidden(ps, "恢复 DNS");
    FlushDns();
    g_dnsModified = false;
    UpdateActionButtons();
}

static void DisableIpv6(HWND hwnd) {
    DWORD ifIndex = GetActiveInterfaceIndex();
    std::wstring adapterName = GetInterfaceFriendlyName(ifIndex);

    std::wstring ps = L"$ErrorActionPreference='Stop'; ";
    ps += L"$i=" + std::to_wstring(ifIndex) + L"; ";
    ps += L"$n=(Get-NetAdapter | Where-Object {$_.ifIndex -eq $i} | Select-Object -First 1 -ExpandProperty Name); ";
    ps += L"if(-not $n){throw '找不到当前网卡'}; ";
    ps += L"Disable-NetAdapterBinding -Name $n -ComponentID ms_tcpip6 -Confirm:$false";

    RunPowerShellHidden(ps, "关闭 IPv6");
    g_ipv6InterfaceIndex = ifIndex;
    g_dnsAdapterFriendlyName = adapterName;
    g_ipv6Modified = true;
    UpdateActionButtons();

    std::wstring status = L"IPv6 关闭成功。\r\n网卡：";
    status += adapterName;
    status += L"\r\n恢复时会重新启用 IPv6。";
    AppendLog(status);
}

static void RestoreIpv6() {
    DWORD ifIndex = g_ipv6InterfaceIndex ? g_ipv6InterfaceIndex : GetActiveInterfaceIndex();
    std::wstring ps = L"$ErrorActionPreference='Stop'; ";
    ps += L"$i=" + std::to_wstring(ifIndex) + L"; ";
    ps += L"$n=(Get-NetAdapter | Where-Object {$_.ifIndex -eq $i} | Select-Object -First 1 -ExpandProperty Name); ";
    ps += L"if(-not $n){throw '找不到当前网卡'}; ";
    ps += L"Enable-NetAdapterBinding -Name $n -ComponentID ms_tcpip6 -Confirm:$false";

    RunPowerShellHidden(ps, "恢复 IPv6");
    g_ipv6Modified = false;
    UpdateActionButtons();
}

static void RestoreOriginalHosts() {
    WriteFileBytes(GetHostsPath(), g_originalHosts);
    FlushDns();
    g_activeSessionPath = GetActiveSessionPath();
    DeleteFileW(g_activeSessionPath.c_str());
    g_restored = true;
    g_hostsModified = false;
    UpdateActionButtons();
}

static void RestoreAll() {
    RestoreOriginalHosts();
    if (g_dnsModified) {
        RestoreDns();
    }
    if (g_ipv6Modified) {
        RestoreIpv6();
    }
}

static void ApplyHostsContent(HWND hwnd, const std::wstring &inputText) {
    std::string input = Trim(WideToUtf8(inputText));
    if (input.empty()) {
        MessageBoxW(hwnd, L"hosts 内容不能为空。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::string data = TrimRight(input) + "\r\n";
    g_activeSessionPath = GetActiveSessionPath();
    WriteFileBytes(g_activeSessionPath, "active\r\n");
    WriteFileBytes(GetHostsPath(), data);
    FlushDns();
    g_restored = false;
    g_hostsModified = true;
    UpdateActionButtons();

    std::wstring status = L"写入成功。\r\n关闭窗口时会自动恢复 hosts 备份。\r\n\r\n";
    status += Utf8ToWide(data);
    AppendLog(status);
    MessageBoxW(hwnd, L"写入成功。", L"完成", MB_OK | MB_ICONINFORMATION);
}

static void Layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    MoveWindow(GetDlgItem(hwnd, IDC_BUTTON_APPLY), 18, 18, 120, 34, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_BUTTON_DNS), 152, 18, 110, 34, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_BUTTON_IPV6), 276, 18, 110, 34, TRUE);
    MoveWindow(g_status, 18, 72, std::max(200, w - 36), std::max(120, h - 90), TRUE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        HFONT mono = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");

        g_applyButton = CreateWindowW(L"BUTTON", L"修改 hosts", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 18, 18, 120, 34, hwnd, (HMENU)IDC_BUTTON_APPLY, NULL, NULL);
        g_dnsButton = CreateWindowW(L"BUTTON", L"修改 DNS", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 152, 18, 110, 34, hwnd, (HMENU)IDC_BUTTON_DNS, NULL, NULL);
        g_ipv6Button = CreateWindowW(L"BUTTON", L"关闭 IPv6", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 276, 18, 110, 34, hwnd, (HMENU)IDC_BUTTON_IPV6, NULL, NULL);
        SendMessageW(g_applyButton, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(g_dnsButton, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(g_ipv6Button, WM_SETFONT, (WPARAM)font, TRUE);

        g_status = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
            18, 72, 764, 462, hwnd, (HMENU)IDC_STATUS, NULL, NULL);
        SendMessageW(g_status, WM_SETFONT, (WPARAM)mono, TRUE);

        try {
            std::string raw = ReadFileBytes(GetHostsPath());
            bool removed = false;
            g_originalHosts = LoadStartupBaseline(raw, &removed);
            g_backupPath = SaveBackup(g_originalHosts);
            std::wstring status = L"已备份当前 hosts。关闭窗口时会自动恢复。\r\n备份文件：";
            status += g_backupPath;
            if (g_recoveredPreviousSession) {
                status += L"\r\n检测到上次异常退出残留的临时块，已先用 latest-baseline 自动恢复。";
            } else if (removed) {
                status += L"\r\n检测到上次残留的临时块，本次备份已自动排除它。";
            }
            AppendLog(status);
            RefreshHostsModifiedState(true);
            SetTimer(hwnd, IDT_REFRESH_STATE, 1500, NULL);
        } catch (const std::exception &ex) {
            std::wstring message = L"读取或备份 hosts 失败。请确认以管理员权限运行。\r\n\r\n";
            message += Utf8ToWide(ex.what());
            MessageBoxW(hwnd, message.c_str(), L"启动失败", MB_OK | MB_ICONERROR);
            PostQuitMessage(1);
        }
        return 0;
    }
    case WM_SIZE:
        Layout(hwnd);
        return 0;
    case WM_TIMER:
        if (wParam == IDT_REFRESH_STATE) {
            RefreshHostsModifiedState(true);
            return 0;
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BUTTON_APPLY:
            try {
                RefreshHostsModifiedState(true);
                if (g_hostsModified) {
                    RestoreOriginalHosts();
                    std::wstring status = L"恢复成功。\r\n已恢复打开程序时的 hosts 备份。\r\n备份文件：";
                    status += g_backupPath;
                    AppendLog(status);
                } else {
                    std::wstring input;
                    if (PromptHostsInput(hwnd, &input)) {
                        ApplyHostsContent(hwnd, input);
                    }
                }
            } catch (const std::exception &ex) {
                std::wstring message = g_hostsModified ? L"恢复 hosts 失败。\r\n\r\n" : L"修改 hosts 失败。请确认以管理员权限运行。\r\n\r\n";
                message += Utf8ToWide(ex.what());
                MessageBoxW(hwnd, message.c_str(), g_hostsModified ? L"恢复失败" : L"修改失败", MB_OK | MB_ICONERROR);
            }
            return 0;
        case IDC_BUTTON_DNS:
            try {
                if (g_dnsModified) {
                    RestoreDns();
                    AppendLog(L"恢复成功。\r\nDNS 已改回自动获取。");
                    UpdateActionButtons();
                } else {
                    std::wstring dnsInput;
                    if (PromptDnsInput(hwnd, &dnsInput)) {
                        ApplyDns(hwnd, dnsInput);
                        UpdateActionButtons();
                    }
                }
            } catch (const std::exception &ex) {
                std::wstring message = g_dnsModified ? L"恢复 DNS 失败。\r\n\r\n" : L"修改 DNS 失败。\r\n\r\n";
                message += Utf8ToWide(ex.what());
                MessageBoxW(hwnd, message.c_str(), g_dnsModified ? L"恢复失败" : L"修改失败", MB_OK | MB_ICONERROR);
            }
            return 0;
        case IDC_BUTTON_IPV6:
            try {
                if (g_ipv6Modified) {
                    AppendLog(L"正在恢复 IPv6，请稍候...");
                    RestoreIpv6();
                    AppendLog(L"恢复成功。\r\nIPv6 已重新启用。");
                    UpdateActionButtons();
                } else {
                    AppendLog(L"正在关闭 IPv6，请稍候...");
                    DisableIpv6(hwnd);
                    UpdateActionButtons();
                }
            } catch (const std::exception &ex) {
                std::wstring message = g_ipv6Modified ? L"恢复 IPv6 失败。\r\n\r\n" : L"关闭 IPv6 失败。\r\n\r\n";
                message += Utf8ToWide(ex.what());
                MessageBoxW(hwnd, message.c_str(), g_ipv6Modified ? L"恢复失败" : L"修改失败", MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        break;
    case WM_CLOSE:
        RefreshHostsModifiedState(false);
        if ((g_hostsModified || g_dnsModified || g_ipv6Modified) && !g_restoringOnClose) {
            try {
                g_restoringOnClose = true;
                RestoreAll();
            } catch (const std::exception &ex) {
                std::wstring message = L"关闭时自动恢复配置失败。仍然关闭程序吗？\r\n\r\n";
                message += Utf8ToWide(ex.what());
                int result = MessageBoxW(hwnd, message.c_str(), L"恢复失败", MB_YESNO | MB_ICONERROR);
                g_restoringOnClose = false;
                if (result == IDNO) return 0;
            }
            g_restoringOnClose = false;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    if (!IsRunningAsAdmin()) {
        if (!RelaunchAsAdmin()) {
            MessageBoxW(NULL, L"需要管理员权限才能修改 hosts。请在 UAC 弹窗中选择“是”，或右键以管理员身份运行。", L"需要管理员权限", MB_OK | MB_ICONERROR);
        }
        return 0;
    }

    const wchar_t CLASS_NAME[] = L"TempWindowsHostsNativeWindow";

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));

    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        CLASS_NAME,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 820, 460,
        NULL,
        NULL,
        hInstance,
        NULL);

    if (!hwnd) return 1;

    HICON appIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    if (appIcon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)appIcon);
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)appIcon);
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}

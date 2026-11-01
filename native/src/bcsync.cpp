// bcsync.exe - the resident agent.
//
// Two jobs:
//
//  1. Keep bcdll.dll injected into ShellHost.exe. Windows restarts ShellHost from
//     time to time and the DLL goes with it, so this process re-injects whenever
//     the host it was watching exits, and picks up a host that appears later.
//
//  2. Write the external panel's brightness. ShellHost cannot do it: there,
//     GetPhysicalMonitorsFromHMONITOR hands back a null handle for every monitor.
//     The injected DLL signals this process through a shared page and a named
//     event, and it wakes only when the shell's slider moves.
//
//   bcsync.exe              run as the agent (started at logon)
//   bcsync.exe --set 42     write once and exit, for testing
//   bcsync.exe --read       print the current level and exit

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>

#pragma comment(lib, "dxva2.lib")

static const wchar_t* kMappingName = L"Local\\BrightnessControlSync";
static const wchar_t* kEventName = L"Local\\BrightnessControlEvent";
static const wchar_t* kHostExe = L"ShellHost.exe";
static const wchar_t* kDllName = L"bcdll.dll";

struct Shared
{
    volatile LONG brightness;  // 0..100, the level the shell slider is at
    volatile LONG sequence;    // bumped on every write, for diagnostics
};

static PHYSICAL_MONITOR g_monitor = {};
static bool g_haveMonitor = false;

static BOOL CALLBACK FindMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM)
{
    if (g_haveMonitor)
        return FALSE;

    // Pick by capability rather than "is it primary": the Dell is the primary
    // display on this machine, and the laptop panel has no DDC handle.
    DWORD count = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(monitor, &count)) {
        fwprintf(stderr, L"GetNumberOfPhysicalMonitors failed %lu\n", GetLastError());
        return TRUE;
    }
    if (count == 0) {
        fwprintf(stderr, L"monitor reports 0 physical monitors\n");
        return TRUE;
    }
    fwprintf(stderr, L"monitor: %lu physical monitor(s)\n", count);

    PHYSICAL_MONITOR stack[8] = {};
    const bool onStack = (count <= 8);
    PHYSICAL_MONITOR* monitors = onStack ? stack : static_cast<PHYSICAL_MONITOR*>(
        HeapAlloc(GetProcessHeap(), 0, count * sizeof(PHYSICAL_MONITOR)));
    if (!monitors)
        return TRUE;

    if (GetPhysicalMonitorsFromHMONITOR(monitor, count, monitors)) {
        for (DWORD i = 0; i < count && !g_haveMonitor; ++i) {
            fwprintf(stderr, L"  [%lu] handle=0x%p desc='%s'\n",
                     i, monitors[i].hPhysicalMonitor, monitors[i].szPhysicalMonitorDescription);
            if (!monitors[i].hPhysicalMonitor)
                continue;
            DWORD current = 0, maximum = 0;
            BOOL ok = GetVCPFeatureAndVCPFeatureReply(monitors[i].hPhysicalMonitor, 0x10,
                                                      nullptr, &current, &maximum);
            fwprintf(stderr, L"  [%lu] '%s' handle=0x%p vcp10=%d err=%lu %lu/%lu\n",
                     i, monitors[i].szPhysicalMonitorDescription, monitors[i].hPhysicalMonitor,
                     ok ? 1 : 0, ok ? 0 : GetLastError(), current, maximum);
            if (ok) {
                g_monitor = monitors[i];
                g_haveMonitor = true;
            }
        }

        for (DWORD i = 0; i < count; ++i) {
            if (g_haveMonitor && monitors[i].hPhysicalMonitor == g_monitor.hPhysicalMonitor)
                continue;
            if (monitors[i].hPhysicalMonitor)
                DestroyPhysicalMonitor(monitors[i].hPhysicalMonitor);
        }
    } else {
        fwprintf(stderr, L"GetPhysicalMonitorsFromHMONITOR failed %lu\n", GetLastError());
    }

    // Only free what came from the heap: freeing the stack array corrupts the
    // heap and kills the process part way through the monitor list, which is
    // what hid the DDC-capable display from this helper.
    if (!onStack)
        HeapFree(GetProcessHeap(), 0, monitors);
    return g_haveMonitor ? FALSE : TRUE;
}

// Handle creation is unreliable here: the same call returns a real handle on one
// run and 0 on the next, and the VCP read comes back with
// ERROR_GRAPHICS_I2C_ERROR_RECEIVING_DATA (0xC02625E2) when the bus is busy.
// Both clear up on a retry, so give each a few goes before giving up.
static bool EnsureMonitor()
{
    if (g_haveMonitor)
        return true;

    for (int attempt = 0; attempt < 5; ++attempt) {
        EnumDisplayMonitors(nullptr, nullptr, FindMonitor, 0);
        if (g_haveMonitor) {
            DWORD current = 0, maximum = 0;
            if (GetVCPFeatureAndVCPFeatureReply(g_monitor.hPhysicalMonitor, 0x10,
                                                nullptr, &current, &maximum))
                return true;
            DestroyPhysicalMonitor(g_monitor.hPhysicalMonitor);
            g_haveMonitor = false;
        }
        Sleep(120);
    }
    return false;
}

static bool WriteBrightness(int value)
{
    if (!EnsureMonitor())
        return false;
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    if (!SetVCPFeature(g_monitor.hPhysicalMonitor, 0x10, static_cast<DWORD>(value))) {
        // The handle can go stale when the display sleeps or is replugged.
        g_haveMonitor = false;
        return false;
    }
    return true;
}

static int ReadBrightness()
{
    if (!EnsureMonitor())
        return -1;
    DWORD current = 0, maximum = 0;
    if (!GetVCPFeatureAndVCPFeatureReply(g_monitor.hPhysicalMonitor, 0x10,
                                         nullptr, &current, &maximum)) {
        g_haveMonitor = false;
        return -1;
    }
    if (maximum == 0)
        return -1;
    return static_cast<int>(current * 100 / maximum);
}

// ---------------------------------------------------------------------------
// Injection
// ---------------------------------------------------------------------------

// ShellHost runs more than one instance and the Control Center is not
// necessarily in the first one, so every match gets the DLL.
static std::vector<DWORD> FindProcessIds(const wchar_t* exeName)
{
    std::vector<DWORD> pids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return pids;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, exeName) == 0)
                pids.push_back(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pids;
}

static bool IsDllLoaded(DWORD pid, const wchar_t* dllName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    MODULEENTRY32W module = {};
    module.dwSize = sizeof(module);
    bool found = false;
    if (Module32FirstW(snapshot, &module)) {
        do {
            if (_wcsicmp(module.szModule, dllName) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &module));
    }
    CloseHandle(snapshot);
    return found;
}

static bool InjectInto(DWORD pid, const wchar_t* dllPath)
{
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process)
        return false;

    const size_t bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        CloseHandle(process);
        return false;
    }

    bool ok = false;
    if (WriteProcessMemory(process, remote, dllPath, bytes, nullptr)) {
        // LoadLibraryW has the same signature as a thread start routine, so the
        // remote thread can call it directly with the path we just wrote.
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(kernel32, "LoadLibraryW"));
        HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
        if (thread) {
            WaitForSingleObject(thread, 10000);
            DWORD moduleBase = 0;
            GetExitCodeThread(thread, &moduleBase);
            ok = moduleBase != 0;
            CloseHandle(thread);
        }
    }

    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return ok;
}

// Inject into any ShellHost that does not already have the DLL, and hand back a
// handle per host so the caller can wait on it and notice a restart.
static std::vector<HANDLE> EnsureInjected(const std::wstring& dllPath)
{
    std::vector<HANDLE> hosts;

    for (DWORD pid : FindProcessIds(kHostExe)) {
        HANDLE host = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!host)
            continue;

        if (!IsDllLoaded(pid, kDllName)) {
            if (InjectInto(pid, dllPath.c_str()))
                fwprintf(stderr, L"injected into %s (pid %lu)\n", kHostExe, pid);
            else
                fwprintf(stderr, L"injection into %s (pid %lu) failed\n", kHostExe, pid);
        }

        hosts.push_back(host);
        if (hosts.size() >= 8) {
            CloseHandle(host);
            break;
        }
    }
    return hosts;
}

static void CloseAll(std::vector<HANDLE>& handles)
{
    for (HANDLE h : handles)
        CloseHandle(h);
    handles.clear();
}

int wmain(int argc, wchar_t** argv)
{
    // dxva2 hands back a null hPhysicalMonitor to a non-DPI-aware process on
    // this build, even though the description is filled in.
    SetProcessDPIAware();

    if (argc >= 3 && wcscmp(argv[1], L"--set") == 0)
        return WriteBrightness(_wtoi(argv[2])) ? 0 : 1;

    if (argc >= 2 && wcscmp(argv[1], L"--read") == 0) {
        int value = ReadBrightness();
        if (value < 0)
            return 1;
        wprintf(L"%d\n", value);
        return 0;
    }

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, sizeof(Shared), kMappingName);
    if (!mapping) {
        fwprintf(stderr, L"CreateFileMapping failed: %lu\n", GetLastError());
        return 1;
    }

    Shared* shared = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!shared) {
        fwprintf(stderr, L"MapViewOfFile failed: %lu\n", GetLastError());
        CloseHandle(mapping);
        return 1;
    }

    HANDLE signal = CreateEventW(nullptr, FALSE, FALSE, kEventName);
    if (!signal) {
        fwprintf(stderr, L"CreateEvent failed: %lu\n", GetLastError());
        CloseHandle(mapping);
        return 1;
    }

    // The DLL sits next to this executable.
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dllPath(self);
    size_t slash = dllPath.find_last_of(L"\\/");
    dllPath = (slash == std::wstring::npos) ? L"" : dllPath.substr(0, slash + 1);
    dllPath += kDllName;

    std::vector<HANDLE> hosts = EnsureInjected(dllPath);

    for (;;) {
        HANDLE handles[9] = {};
        handles[0] = signal;
        DWORD count = 1;
        for (HANDLE h : hosts) {
            if (count < 9)
                handles[count++] = h;
        }

        DWORD result = WaitForMultipleObjects(count, handles, FALSE, 2000);

        if (result == WAIT_OBJECT_0) {
            LONG value = shared->brightness;
            if (value >= 0) {  // -1 means "nothing pending"
                WriteBrightness(static_cast<int>(value));
                InterlockedIncrement(&shared->sequence);
            }
        } else if (result == WAIT_TIMEOUT) {
            // Safety net: a ShellHost may have appeared since the last scan.
            CloseAll(hosts);
            hosts = EnsureInjected(dllPath);
        } else if (result > WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count) {
            // A watched host exited - Windows restarted the shell. Re-inject.
            CloseAll(hosts);
            hosts = EnsureInjected(dllPath);
        }
    }
}

// bcsync.exe - DDC/CI writer for the external panel.
//
// ShellHost cannot open physical monitors: GetPhysicalMonitorsFromHMONITOR
// succeeds there but hands back a null handle for every monitor, so the injected
// DLL cannot write brightness itself. This process does the write instead, and
// the DLL signals it through a shared page and a named event. It has no window,
// no UI and no polling loop - it sleeps on the event and wakes when the shell's
// brightness slider moves.
//
//   bcsync.exe              run as the writer (normally started at logon)
//   bcsync.exe --set 42     write once and exit, for testing
//   bcsync.exe --read       print the current level and exit

#include <windows.h>
#include <stdio.h>
#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>

#pragma comment(lib, "dxva2.lib")

static const wchar_t* kMappingName = L"Local\\BrightnessControlSync";
static const wchar_t* kEventName = L"Local\\BrightnessControlEvent";

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

int wmain(int argc, wchar_t** argv)
{
    // dxva2 hands back a null hPhysicalMonitor to a non-DPI-aware process on
    // this build, even though the description is filled in. PowerShell's host is
    // aware, which is why the same enumeration works there.
    SetProcessDPIAware();
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    fwprintf(stderr, L"session=%lu pid=%lu sizeof=%zu desc_off=%zu\n",
             session, GetCurrentProcessId(),
             sizeof(PHYSICAL_MONITOR),
             offsetof(PHYSICAL_MONITOR, szPhysicalMonitorDescription));

    if (argc >= 3 && wcscmp(argv[1], L"--set") == 0) {
        int value = _wtoi(argv[2]);
        return WriteBrightness(value) ? 0 : 1;
    }
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
        wprintf(L"CreateFileMapping failed: %lu\n", GetLastError());
        return 1;
    }

    Shared* shared = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!shared) {
        wprintf(L"MapViewOfFile failed: %lu\n", GetLastError());
        CloseHandle(mapping);
        return 1;
    }

    HANDLE signal = CreateEventW(nullptr, FALSE, FALSE, kEventName);
    if (!signal) {
        wprintf(L"CreateEvent failed: %lu\n", GetLastError());
        return 1;
    }

    // Nothing to do until the shell asks for something.
    for (;;) {
        if (WaitForSingleObject(signal, INFINITE) != WAIT_OBJECT_0)
            continue;
        LONG value = shared->brightness;
        if (value < 0)
            continue;  // -1 means "nothing pending"
        WriteBrightness(static_cast<int>(value));
        InterlockedIncrement(&shared->sequence);
    }
}

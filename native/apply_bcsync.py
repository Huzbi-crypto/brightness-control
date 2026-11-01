p = "src/bcdll.cpp"
s = open(p, encoding="utf-8").read()

start = s.index("// DDC/CI for the external panel")
start = s.rindex("// ---------------------------------------------------------------------------", 0, start)
end = s.index("// The stock brightness slider")
end = s.rindex("// ---------------------------------------------------------------------------", 0, end)

new = '''// ---------------------------------------------------------------------------
// Handing the level to bcsync.exe
//
// ShellHost cannot open physical monitors - GetPhysicalMonitorsFromHMONITOR
// returns a null handle for every monitor here, though the same call works from
// an ordinary process. So the DLL does not touch DDC at all: it publishes the
// level in a shared page and signals a named event, and bcsync.exe does the
// SetVCPFeature.
// ---------------------------------------------------------------------------

static const wchar_t* kMappingName = L"Local\\\\BrightnessControlSync";
static const wchar_t* kEventName = L"Local\\\\BrightnessControlEvent";

struct SharedState
{
    volatile LONG brightness;  // 0..100
    volatile LONG sequence;    // bumped by the writer, for diagnostics
};

static HANDLE g_mapping = nullptr;
static HANDLE g_signal = nullptr;
static SharedState* g_shared = nullptr;

static void InitSync()
{
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                   0, sizeof(SharedState), kMappingName);
    if (g_mapping)
        g_shared = static_cast<SharedState*>(
            MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    g_signal = CreateEventW(nullptr, FALSE, FALSE, kEventName);
    Log(L"sync: mapping=0x%p shared=0x%p event=0x%p", g_mapping, g_shared, g_signal);
}

static void QueueExternalBrightness(double value)
{
    if (!g_shared || !g_signal)
        return;
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    g_shared->brightness = static_cast<LONG>(value);
    SetEvent(g_signal);
}

'''
s = s[:start] + new + s[end:]

# the DLL no longer needs the dxva2 headers
s = s.replace("#include <physicalmonitorenumerationapi.h>\n", "")
s = s.replace("#include <lowlevelmonitorconfigurationapi.h>\n", "")
s = s.replace('#pragma comment(lib, "dxva2.lib")\n', "")

# swap the DDC worker startup for the shared-memory setup
s = s.replace('''    g_ddcEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_ddcEvent) {
        HANDLE thread = CreateThread(nullptr, 0, DdcWorker, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }''',
'''    InitSync();''')

# drop the dispatcher-thread enumeration probe, it has no meaning any more
s = s.replace('''    // Enumerate here first: this runs on the dispatcher thread, and dxva2 may
    // need that thread's desktop context. If the handles are real here but null
    // on the worker, the fix is to keep all DDC work on this thread.
    Log(L"stock slider: enumerating monitors on the dispatcher thread");
    EnumDisplayMonitors(nullptr, nullptr, FindExternalMonitor, 0);
    Log(L"stock slider: dispatcher enumeration done, haveExternal=%d", g_haveExternal ? 1 : 0);

''', "")

open(p, "w", encoding="utf-8").write(s)
print("DLL now signals bcsync instead of doing DDC")

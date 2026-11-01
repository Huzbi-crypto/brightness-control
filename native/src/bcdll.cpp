// bcdll.dll — injected into ShellHost.exe.
//
// Milestone 1: prove we are running inside the host and can see the modules we
// care about. The XAML work hangs off this once it is confirmed.

#include <windows.h>
#include <stdio.h>

static void Log(const wchar_t* format, ...)
{
    wchar_t path[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH))
        return;
    wcscat_s(path, L"\\BrightnessControl");
    CreateDirectoryW(path, nullptr);
    wcscat_s(path, L"\\native.log");

    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"a, ccs=UTF-8") != 0 || !file)
        return;

    SYSTEMTIME now;
    GetLocalTime(&now);
    fwprintf(file, L"%02u:%02u:%02u.%03u  ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);

    va_list args;
    va_start(args, format);
    vfwprintf(file, format, args);
    va_end(args);

    fwprintf(file, L"\n");
    fclose(file);
}

static DWORD WINAPI InitThread(LPVOID module)
{
    wchar_t hostPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, hostPath, MAX_PATH);

    Log(L"attached: host=%s pid=%lu dll=0x%p",
        hostPath, GetCurrentProcessId(), module);

    // ControlCenter.dll carries the Quick Settings XAML and is not statically
    // imported by the host, so it may or may not be mapped yet. Whichever it is
    // tells us which discovery path to take next.
    HMODULE controlCenter = GetModuleHandleW(L"ControlCenter.dll");
    Log(L"ControlCenter.dll %s (0x%p)",
        controlCenter ? L"already mapped" : L"not mapped yet", controlCenter);

    // The two hook targets, resolved from ControlCenter.pdb (GUID
    // 69549732A6CF1C43FDF7ED1FD2227E421, age 1) off Microsoft's symbol server.
    // Connect is ICF-folded with the Connect of other classes in this DLL, so a
    // hook there has to identify the object rather than trust the address.
    if (controlCenter) {
        const struct { const wchar_t* name; DWORD rva; } targets[] = {
            { L"ControlCenterView::OnGotFocus", 0x000C0950 },
            { L"ControlCenterView::Connect",    0x000A5490 },
        };
        for (const auto& t : targets) {
            const BYTE* fn = reinterpret_cast<const BYTE*>(controlCenter) + t.rva;
            Log(L"%s rva=0x%X va=0x%p bytes=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                t.name, t.rva, fn,
                fn[0], fn[1], fn[2], fn[3], fn[4], fn[5],
                fn[6], fn[7], fn[8], fn[9], fn[10], fn[11]);
        }
    }

    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        // DllMain must not do real work: touching the loader or creating UI here
        // deadlocks. Everything happens on our own thread.
        DisableThreadLibraryCalls(module);
        if (HANDLE thread = CreateThread(nullptr, 0, InitThread, module, 0, nullptr))
            CloseHandle(thread);
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

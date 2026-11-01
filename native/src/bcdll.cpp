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

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    Log(L"user32=0x%p LoadLibraryW=0x%p",
        user32, GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));

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

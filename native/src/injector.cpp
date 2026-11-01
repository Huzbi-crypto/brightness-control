// bcinject.exe — one-shot DLL injector.
//
// Finds a running process by executable name and loads a DLL into it, then
// exits. There is no service and nothing resident: this runs once at logon.
//
//   bcinject.exe ShellHost.exe C:\path\to\bcdll.dll [--wait=SECONDS]
//
// The --wait option keeps retrying until the process appears, which matters at
// logon because ShellHost may start after us.

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

// ShellHost runs more than one instance on this build and the Control Center is
// not necessarily in the first one, so every match gets the DLL.
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

static bool InjectInto(DWORD pid, const wchar_t* dllPath)
{
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        wprintf(L"OpenProcess(%lu) failed: %lu\n", pid, GetLastError());
        return false;
    }

    const size_t bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        wprintf(L"VirtualAllocEx failed: %lu\n", GetLastError());
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
            if (!ok)
                wprintf(L"LoadLibraryW in target returned 0 (error %lu)\n", GetLastError());
            CloseHandle(thread);
        } else {
            wprintf(L"CreateRemoteThread failed: %lu\n", GetLastError());
        }
    } else {
        wprintf(L"WriteProcessMemory failed: %lu\n", GetLastError());
    }

    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return ok;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) {
        wprintf(L"usage: %s <exe-name> <dll-path> [--wait=SECONDS]\n", argv[0]);
        return 2;
    }

    const wchar_t* exeName = argv[1];
    const wchar_t* dllPath = argv[2];

    int waitSeconds = 0;
    for (int i = 3; i < argc; ++i) {
        if (wcsncmp(argv[i], L"--wait=", 7) == 0)
            waitSeconds = _wtoi(argv[i] + 7);
    }

    wchar_t fullPath[MAX_PATH];
    if (!GetFullPathNameW(dllPath, MAX_PATH, fullPath, nullptr)) {
        wprintf(L"bad dll path: %s\n", dllPath);
        return 2;
    }
    if (GetFileAttributesW(fullPath) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"dll not found: %s\n", fullPath);
        return 2;
    }

    std::vector<DWORD> pids;
    for (int elapsed = 0; elapsed <= waitSeconds; ++elapsed) {
        pids = FindProcessIds(exeName);
        if (!pids.empty())
            break;
        if (elapsed < waitSeconds)
            Sleep(1000);
    }

    if (pids.empty()) {
        wprintf(L"%s is not running\n", exeName);
        return 1;
    }

    int injected = 0;
    for (DWORD pid : pids) {
        if (InjectInto(pid, fullPath)) {
            wprintf(L"injected %s into %s (pid %lu)\n", fullPath, exeName, pid);
            ++injected;
        } else {
            wprintf(L"injection into %s (pid %lu) failed\n", exeName, pid);
        }
    }

    return injected > 0 ? 0 : 1;
}

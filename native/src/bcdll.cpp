// bcdll.dll — injected into ShellHost.exe.
//
// Milestone 1: prove we are running inside the host and can see the modules we
// care about. The XAML work hangs off this once it is confirmed.

#include <windows.h>
#include <stdio.h>
#include <stdint.h>

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

// ---------------------------------------------------------------------------
// Detour plumbing
// ---------------------------------------------------------------------------

static BYTE* AllocNear(const void* target, size_t size)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t granularity = si.dwAllocationGranularity;
    const uintptr_t base = reinterpret_cast<uintptr_t>(target);

    // E9 only reaches +-2 GB, so the relay has to live near the target.
    for (uintptr_t delta = granularity; delta < 0x70000000ULL; delta += granularity) {
        for (int sign = -1; sign <= 1; sign += 2) {
            uintptr_t hint = (sign < 0) ? (base - delta) : (base + delta);
            if (hint < 0x10000)
                continue;
            void* p = VirtualAlloc(reinterpret_cast<void*>(hint), size,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p)
                return static_cast<BYTE*>(p);
        }
    }
    return nullptr;
}

// Emits `mov rax, imm64; jmp rax` - position independent, any distance.
static void EmitAbsoluteJump(BYTE* at, const void* destination)
{
    at[0] = 0x48;
    at[1] = 0xB8;
    *reinterpret_cast<const void**>(at + 2) = destination;
    at[10] = 0xFF;
    at[11] = 0xE0;
}

// Returns a trampoline that runs the stolen bytes and continues in the original
// function, or nullptr.
static void* InstallDetour(BYTE* target, int stealLength, const void* hook)
{
    BYTE* page = AllocNear(target, 4096);
    if (!page) {
        Log(L"detour: no executable memory within 2 GB of 0x%p", target);
        return nullptr;
    }

    BYTE* trampoline = page;
    BYTE* relay = page + 64;

    memcpy(trampoline, target, stealLength);
    EmitAbsoluteJump(trampoline + stealLength, target + stealLength);
    EmitAbsoluteJump(relay, hook);

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, stealLength, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log(L"detour: VirtualProtect failed %lu", GetLastError());
        return nullptr;
    }

    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) = static_cast<int32_t>(relay - (target + 5));
    for (int i = 5; i < stealLength; ++i)
        target[i] = 0x90;

    VirtualProtect(target, stealLength, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, stealLength);
    return trampoline;
}

// virtual int __cdecl ...::OnGotFocus(void*) - one calling convention on x64.
typedef int (*OnGotFocusFn)(void* self, void* args);
static OnGotFocusFn g_onGotFocus = nullptr;

static int Hook_OnGotFocus(void* self, void* args)
{
    Log(L"OnGotFocus fired: this=0x%p args=0x%p", self, args);
    return g_onGotFocus(self, args);
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

    // ControlCenterView::OnGotFocus, from ControlCenter.pdb (GUID
    // 69549732A6CF1C43FDF7ED1FD2227E421, age 1) on Microsoft's symbol server.
    // Its first five bytes are `push rbx` + `sub rsp,0x40` — an instruction
    // boundary and position independent, so the stolen bytes need no relocation.
    const DWORD onGotFocusRva = 0x000C0950;
    BYTE* target = reinterpret_cast<BYTE*>(controlCenter) + onGotFocusRva;
    Log(L"OnGotFocus: rva=0x%X va=0x%p first bytes %02X %02X %02X %02X %02X",
        onGotFocusRva, target, target[0], target[1], target[2], target[3], target[4]);

    // A leading E9 means another hook (the Windhawk mod) already owns this
    // address; patching over its trampoline would corrupt it.
    if (target[0] == 0xE9) {
        Log(L"OnGotFocus already detoured by another hook - refusing to patch");
        return 0;
    }

    void* trampoline = InstallDetour(target, 5, reinterpret_cast<const void*>(&Hook_OnGotFocus));
    if (!trampoline) {
        Log(L"OnGotFocus: detour install failed");
        return 0;
    }

    g_onGotFocus = reinterpret_cast<OnGotFocusFn>(trampoline);
    Log(L"OnGotFocus hooked; trampoline=0x%p hook=0x%p", trampoline, &Hook_OnGotFocus);

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

// bcdll.dll — injected into ShellHost.exe.
//
// Milestone 1: prove we are running inside the host and can see the modules we
// care about. The XAML work hangs off this once it is confirmed.

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <hstring.h>
#pragma comment(lib, "ole32.lib")

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

// ---------------------------------------------------------------------------
// WinRT ABI, declared minimally - just the interfaces the walk needs
// ---------------------------------------------------------------------------

// combase.lib is not shipped with the Build Tools, so resolve the two string
// helpers out of combase.dll at run time.
typedef PCWSTR (WINAPI* GetStringRawBufferFn)(HSTRING, UINT32*);
typedef HRESULT (WINAPI* DeleteStringFn)(HSTRING);
static GetStringRawBufferFn g_getStringRawBuffer = nullptr;
static DeleteStringFn g_deleteString = nullptr;

static void ResolveHStringHelpers()
{
    HMODULE combase = GetModuleHandleW(L"combase.dll");
    if (!combase)
        combase = LoadLibraryW(L"combase.dll");
    if (!combase)
        return;
    g_getStringRawBuffer = reinterpret_cast<GetStringRawBufferFn>(
        GetProcAddress(combase, "WindowsGetStringRawBuffer"));
    g_deleteString = reinterpret_cast<DeleteStringFn>(
        GetProcAddress(combase, "WindowsDeleteString"));
}

struct IInspectableABI : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** iids) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* name) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(int* level) = 0;
};

// a26dd1dc-cd44-435c-be94-01d6241c231c
struct IContentControlABI : IInspectableABI
{
    virtual HRESULT STDMETHODCALLTYPE get_Content(IInspectableABI** value) = 0;
};

// a50a4bbd-8361-469c-90da-e9a40c7474df
struct IPanelABI : IInspectableABI
{
    virtual HRESULT STDMETHODCALLTYPE get_Children(IInspectableABI** value) = 0;
};

// IVector<T>: GetAt, get_Size, GetView, IndexOf, ...
struct IVectorABI : IInspectableABI
{
    virtual HRESULT STDMETHODCALLTYPE GetAt(unsigned index, IInspectableABI** item) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(unsigned* size) = 0;
};

static const IID kIContentControl = { 0xa26dd1dc, 0xcd44, 0x435c, { 0xbe, 0x94, 0x01, 0xd6, 0x24, 0x1c, 0x23, 0x1c } };
static const IID kIPanel = { 0xa50a4bbd, 0x8361, 0x469c, { 0x90, 0xda, 0xe9, 0xa4, 0x0c, 0x74, 0x74, 0xdf } };
static const IID kIInspectable = { 0xaf86e2e0, 0xb12d, 0x4c6a, { 0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90 } };

static void LogClassName(const wchar_t* label, IInspectableABI* object)
{
    if (!object) {
        Log(L"%s: <null>", label);
        return;
    }
    HSTRING name = nullptr;
    if (SUCCEEDED(object->GetRuntimeClassName(&name)) && name) {
        UINT32 length = 0;
        const wchar_t* raw = g_getStringRawBuffer ? g_getStringRawBuffer(name, &length) : L"<?>";
        Log(L"%s: %s", label, raw);
        if (g_deleteString)
            g_deleteString(name);
    } else {
        Log(L"%s: <no runtime class name>", label);
    }
}

static void InspectView(void* self)
{
    auto inspectable = reinterpret_cast<IInspectableABI*>(self);

    LogClassName(L"view class", inspectable);

    // Dump the interface list: the view is not a ContentControl, so the way to
    // its children has to come from what it actually implements.
    ULONG iidCount = 0;
    IID* iids = nullptr;
    if (SUCCEEDED(inspectable->GetIids(&iidCount, &iids)) && iids) {
        for (ULONG i = 0; i < iidCount; ++i) {
            const IID& id = iids[i];
            Log(L"  iid {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                id.Data1, id.Data2, id.Data3,
                id.Data4[0], id.Data4[1], id.Data4[2], id.Data4[3],
                id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7]);
        }
        CoTaskMemFree(iids);
    }

    IPanelABI* selfPanel = nullptr;
    HRESULT selfHr = inspectable->QueryInterface(kIPanel, reinterpret_cast<void**>(&selfPanel));
    Log(L"QI IPanel on view: 0x%08X", selfHr);
    if (SUCCEEDED(selfHr) && selfPanel) {
        IInspectableABI* children = nullptr;
        if (SUCCEEDED(selfPanel->get_Children(&children)) && children) {
            IVectorABI* vector = nullptr;
            if (SUCCEEDED(children->QueryInterface(kIInspectable, reinterpret_cast<void**>(&vector)))) {
                unsigned count = 0;
                if (SUCCEEDED(vector->get_Size(&count)))
                    Log(L"  view children: %u", count);
                vector->Release();
            }
            children->Release();
        }
        selfPanel->Release();
    }

    IContentControlABI* contentControl = nullptr;
    HRESULT hr = inspectable->QueryInterface(kIContentControl, reinterpret_cast<void**>(&contentControl));
    Log(L"QI IContentControl: 0x%08X", hr);
    if (FAILED(hr) || !contentControl)
        return;

    IInspectableABI* content = nullptr;
    hr = contentControl->get_Content(&content);
    Log(L"get_Content: 0x%08X", hr);
    contentControl->Release();

    if (FAILED(hr) || !content)
        return;

    LogClassName(L"  content class", content);

    IPanelABI* panel = nullptr;
    hr = content->QueryInterface(kIPanel, reinterpret_cast<void**>(&panel));
    Log(L"  QI IPanel: 0x%08X", hr);
    if (SUCCEEDED(hr) && panel) {
        IInspectableABI* children = nullptr;
        if (SUCCEEDED(panel->get_Children(&children)) && children) {
            IVectorABI* vector = nullptr;
            if (SUCCEEDED(children->QueryInterface(kIInspectable, reinterpret_cast<void**>(&vector)))) {
                unsigned count = 0;
                if (SUCCEEDED(vector->get_Size(&count)))
                    Log(L"  children: %u", count);
                vector->Release();
            }
            children->Release();
        }
        panel->Release();
    }
    content->Release();
}

static int Hook_OnGotFocus(void* self, void* args)
{
    static bool inspected = false;
    if (!inspected) {
        inspected = true;
        Log(L"OnGotFocus fired: this=0x%p args=0x%p", self, args);
        InspectView(self);
    }
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
    ResolveHStringHelpers();

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

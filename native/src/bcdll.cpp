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

// Set while the per-monitor panel drives a single panel, so the mirror
// path does not immediately copy it to the other one.
static bool g_suspendMirror = false;

// ---------------------------------------------------------------------------
// WinRT ABI, declared minimally - just the interfaces the walk needs
// ---------------------------------------------------------------------------

// combase.lib is not shipped with the Build Tools, so resolve the two string
// helpers out of combase.dll at run time.
typedef PCWSTR (WINAPI* GetStringRawBufferFn)(HSTRING, UINT32*);
typedef HRESULT (WINAPI* DeleteStringFn)(HSTRING);
static GetStringRawBufferFn g_getStringRawBuffer = nullptr;
static DeleteStringFn g_deleteString = nullptr;
typedef HRESULT (WINAPI* RoGetActivationFactoryFn)(HSTRING, REFIID, void**);
typedef HRESULT (WINAPI* WindowsCreateStringFn)(PCNZWCH, UINT32, HSTRING*);
typedef HRESULT (WINAPI* RoActivateInstanceFn)(HSTRING, void**);
static RoGetActivationFactoryFn g_roGetActivationFactory = nullptr;
static RoActivateInstanceFn g_roActivateInstance = nullptr;
static WindowsCreateStringFn g_createString = nullptr;


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
    g_roGetActivationFactory = reinterpret_cast<RoGetActivationFactoryFn>(
        GetProcAddress(combase, "RoGetActivationFactory"));
    g_createString = reinterpret_cast<WindowsCreateStringFn>(
        GetProcAddress(combase, "WindowsCreateString"));
    g_roActivateInstance = reinterpret_cast<RoActivateInstanceFn>(
        GetProcAddress(combase, "RoActivateInstance"));
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
    virtual HRESULT STDMETHODCALLTYPE put_Content(IInspectableABI* value) = 0;
};

// a50a4bbd-8361-469c-90da-e9a40c7474df
struct IPanelABI : IInspectableABI
{
    virtual HRESULT STDMETHODCALLTYPE get_Children(IInspectableABI** value) = 0;
};

// IVector<T>: GetAt, get_Size, GetView, IndexOf, SetAt, InsertAt, RemoveAt,
// Append - Append is slot 13.
struct IVectorABI : IInspectableABI
{
    virtual HRESULT STDMETHODCALLTYPE GetAt(unsigned index, IInspectableABI** item) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(unsigned* size) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetView(IInspectableABI** view) = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(IInspectableABI* item, unsigned* index, BOOL* found) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetAt(unsigned index, IInspectableABI* item) = 0;
    virtual HRESULT STDMETHODCALLTYPE InsertAt(unsigned index, IInspectableABI* item) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveAt(unsigned index) = 0;
    virtual HRESULT STDMETHODCALLTYPE Append(IInspectableABI* item) = 0;
};

static const IID kIContentControl = { 0xa26dd1dc, 0xcd44, 0x435c, { 0xbe, 0x94, 0x01, 0xd6, 0x24, 0x1c, 0x23, 0x1c } };
static const IID kIPanel = { 0xa50a4bbd, 0x8361, 0x469c, { 0x90, 0xda, 0xe9, 0xa4, 0x0c, 0x74, 0x74, 0xdf } };
static const IID kIUIElement = { 0x676d0be9, 0xb65c, 0x41c6, { 0xba, 0x40, 0x58, 0xcf, 0x87, 0xf2, 0x01, 0xc1 } };
static IInspectableABI* g_panelSliders[2] = { nullptr, nullptr };

// IUIElement::put_Visibility is method 16 (slot 22).
struct IUIElementVisibilityABI : IInspectableABI
{
    virtual void* v0(); virtual void* v1(); virtual void* v2(); virtual void* v3();
    virtual void* v4(); virtual void* v5(); virtual void* v6(); virtual void* v7();
    virtual void* v8(); virtual void* v9(); virtual void* v10(); virtual void* v11();
    virtual void* v12(); virtual void* v13(); virtual void* v14();
    virtual void* unusedGetVisibility();                            // slot 21
    virtual HRESULT STDMETHODCALLTYPE put_Visibility(int value) = 0;  // slot 22
};

static IInspectableABI* g_monitorPanel = nullptr;
static bool g_panelVisible = false;

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

// ---------------------------------------------------------------------------
// Visual tree walk
// ---------------------------------------------------------------------------

// e75758c4-d25d-4b1d-971f-596f17f12baa — the four Find* methods come first, so
// GetChild/GetChildrenCount sit at slots 10 and 11.
struct IDependencyObjectABI : IInspectableABI
{
};

struct IVisualTreeHelperStaticsABI : IInspectableABI
{
    virtual void* unused0();
    virtual void* unused1();
    virtual void* unused2();
    virtual void* unused3();
    virtual HRESULT STDMETHODCALLTYPE GetChild(IDependencyObjectABI* reference, INT32 childIndex,
                                                IDependencyObjectABI** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChildrenCount(IDependencyObjectABI* reference, UINT32* result) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetParent(IDependencyObjectABI* reference,
                                                 IDependencyObjectABI** result) = 0;
};

// a391d09b-4a99-4b7c-9d8d-6fa5d01f6fbf — ActualWidth/ActualHeight are methods
// 7 and 8, i.e. vtable slots 13 and 14.
struct IFrameworkElementABI : IInspectableABI
{
    virtual void* unused0();
    virtual void* unused1();
    virtual void* unused2();
    virtual void* unused3();
    virtual void* unused4();
    virtual void* unused5();
    virtual void* unused6();
    virtual HRESULT STDMETHODCALLTYPE get_ActualWidth(double* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ActualHeight(double* value) = 0;
    virtual void* unused7();                                        // slot 15 get_Width
    virtual HRESULT STDMETHODCALLTYPE put_Width(double value) = 0;   // slot 16
    virtual void* unused8();                                        // slot 17 get_Height
    virtual HRESULT STDMETHODCALLTYPE put_Height(double value) = 0;  // slot 18
    virtual void* a0(); virtual void* a1(); virtual void* a2();      // slots 19..21
    virtual void* a3(); virtual void* a4(); virtual void* a5();      // slots 22..24
    virtual void* a6(); virtual void* a7(); virtual void* a8();      // slots 25..27
    virtual HRESULT STDMETHODCALLTYPE put_HorizontalAlignment(int value) = 0;  // slot 28
};

static const IID kIActivationFactory = { 0x00000035, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const IID kIVisualTreeHelperStatics = { 0xe75758c4, 0xd25d, 0x4b1d, { 0x97, 0x1f, 0x59, 0x6f, 0x17, 0xf1, 0x2b, 0xaa } };
static const IID kIFrameworkElement = { 0xa391d09b, 0x4a99, 0x4b7c, { 0x9d, 0x8d, 0x6f, 0xa5, 0xd0, 0x1f, 0x6f, 0xbf } };
static const IID kIDependencyObject = { 0x5c526665, 0xf60e, 0x4912, { 0xaf, 0x59, 0x5f, 0xe0, 0x68, 0x0f, 0x08, 0x9d } };


// fd104460-2e15-4ba3-8b8f-fa693a4161e9 - only the two definition vectors.
struct IGridABI : IInspectableABI
{
    virtual HRESULT STDMETHODCALLTYPE get_RowDefinitions(IInspectableABI** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ColumnDefinitions(IInspectableABI** value) = 0;
};

// 4abae829-d80c-4a5e-a48c-f8b3d3b6533d
struct GridLengthABI
{
    double value;
    int type;   // GridUnitType: Auto 0, Pixel 1, Star 2
    int pad;
};

struct IRowDefinitionABI : IInspectableABI
{
    virtual HRESULT STDMETHODCALLTYPE get_Height(GridLengthABI* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Height(GridLengthABI value) = 0;
};

// 64fe2e9f-f951-42b6-a9ce-bb179af11595 - SetRow is method 2.
struct IGridStaticsABI : IInspectableABI
{
    virtual HRESULT STDMETHODCALLTYPE get_RowProperty(IDependencyObjectABI** value) = 0;
    virtual void* unused1();                                       // slot 7 GetRow
    virtual HRESULT STDMETHODCALLTYPE SetRow(IInspectableABI* element, int value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ColumnProperty(IDependencyObjectABI** value) = 0;  // slot 9
};

// IDependencyObject::SetValue is slot 7.
struct IDependencyObjectSetValueABI : IInspectableABI
{
    virtual void* unusedGetValue();                                // slot 6
    virtual HRESULT STDMETHODCALLTYPE SetValue(IDependencyObjectABI* dp, IInspectableABI* value) = 0;
};

// 629bdbc8-d932-4ff4-96b9-8d96c5c1e858 - CreateInt32 is method 4.
struct IPropertyValueStaticsABI : IInspectableABI
{
    virtual void* unused0();
    virtual void* unused1();
    virtual void* unused2();
    virtual void* unused3();
    virtual HRESULT STDMETHODCALLTYPE CreateInt32(int value, IInspectableABI** result) = 0;
};

static const IID kIPropertyValueStatics = { 0x629bdbc8, 0xd932, 0x4ff4, { 0x96, 0xb9, 0x8d, 0x96, 0xc5, 0xc1, 0xe8, 0x58 } };

static const IID kIAgileObject = { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };
static const IID kIGrid = { 0xfd104460, 0x2e15, 0x4ba3, { 0x8b, 0x8f, 0xfa, 0x69, 0x3a, 0x41, 0x61, 0xe9 } };
static const IID kIRowDefinition = { 0x4abae829, 0xd80c, 0x4a5e, { 0xa4, 0x8c, 0xf8, 0xb3, 0xd3, 0xb6, 0x53, 0x3d } };
static const IID kIGridStatics = { 0x64fe2e9f, 0xf951, 0x42b6, { 0xa9, 0xce, 0xbb, 0x17, 0x9a, 0xf1, 0x15, 0x95 } };

// ---------------------------------------------------------------------------
// Handing the level to bcsync.exe
//
// ShellHost cannot open physical monitors - GetPhysicalMonitorsFromHMONITOR
// returns a null handle for every monitor here, though the same call works from
// an ordinary process. So the DLL does not touch DDC at all: it publishes the
// level in a shared page and signals a named event, and bcsync.exe does the
// SetVCPFeature.
// ---------------------------------------------------------------------------

static const wchar_t* kMappingName = L"Local\\BrightnessControlSync";
static const wchar_t* kEventName = L"Local\\BrightnessControlEvent";

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

// ---------------------------------------------------------------------------
// The stock brightness slider
// ---------------------------------------------------------------------------

// fa002c1a-494e-46cf-91d4-e14a8d798675 - Value at slot 14, add_ValueChanged 16.
struct IRangeBaseABI : IInspectableABI
{
    virtual void* unused0();
    virtual void* unused1();
    virtual void* unused2();                                     // slot 8  get_Maximum
    virtual void* unused3();
    virtual void* unused4();
    virtual void* unused5();
    virtual void* unused6();
    virtual void* unused7();
    virtual HRESULT STDMETHODCALLTYPE get_Value(double* value) = 0;   // slot 14
    virtual HRESULT STDMETHODCALLTYPE put_Value(double value) = 0;    // slot 15
    virtual HRESULT STDMETHODCALLTYPE add_ValueChanged(void* handler,
                                                      INT64* token) = 0;  // slot 16
};

// e3906fd9-4d1b-4ac8-a43c-c3b908742799
static const IID kIRangeBase = { 0xfa002c1a, 0x494e, 0x46cf, { 0x91, 0xd4, 0xe1, 0x4a, 0x8d, 0x79, 0x86, 0x75 } };
static const IID kIRangeBaseValueChangedHandler = { 0xe3906fd9, 0x4d1b, 0x4ac8, { 0xa4, 0x3c, 0xc3, 0xb9, 0x08, 0x74, 0x27, 0x99 } };

struct ValueChangedHandler : IUnknown
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == kIRangeBaseValueChangedHandler || riid == IID_IUnknown || riid == kIAgileObject) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    // Invoke(IInspectable* sender, IRangeBaseValueChangedEventArgs* e)
    virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectableABI* sender, void*)
    {
        IRangeBaseABI* range = nullptr;
        if (sender && SUCCEEDED(sender->QueryInterface(kIRangeBase, reinterpret_cast<void**>(&range))) && range) {
            double value = 0;
            if (SUCCEEDED(range->get_Value(&value)) && !g_suspendMirror) {
                QueueExternalBrightness(value);

                // The stock slider moves both panels, so the per-monitor sliders
                // have to show the new level instead of drifting out of date.
                g_suspendMirror = true;
                for (int i = 0; i < 2; ++i) {
                    IRangeBaseABI* panelRange = nullptr;
                    if (g_panelSliders[i] &&
                        SUCCEEDED(g_panelSliders[i]->QueryInterface(kIRangeBase, reinterpret_cast<void**>(&panelRange))) &&
                        panelRange) {
                        panelRange->put_Value(value);
                        panelRange->Release();
                    }
                }
                g_suspendMirror = false;
            }
            range->Release();
        }
        return S_OK;
    }
};

static ValueChangedHandler g_valueChangedHandler;
static IDependencyObjectABI* g_stockSlider = nullptr;

static void BuildChevron();
static void AppendChild(IInspectableABI* parent, IInspectableABI* child);

static void AttachStockSlider()
{
    if (!g_stockSlider) {
        Log(L"stock slider: not captured");
        return;
    }

    IRangeBaseABI* range = nullptr;
    HRESULT hr = reinterpret_cast<IInspectableABI*>(g_stockSlider)->QueryInterface(
        kIRangeBase, reinterpret_cast<void**>(&range));
    Log(L"stock slider: QI IRangeBase 0x%08X", hr);
    if (FAILED(hr) || !range)
        return;

    double value = 0;
    range->get_Value(&value);
    Log(L"stock slider: current value %.0f", value);

    INT64 token = 0;
    hr = range->add_ValueChanged(&g_valueChangedHandler, &token);
    Log(L"stock slider: add_ValueChanged 0x%08X", hr);
    range->Release();

    QueueExternalBrightness(value);
    BuildChevron();
}

static IVisualTreeHelperStaticsABI* GetVisualTreeHelper()
{
    if (!g_roGetActivationFactory || !g_createString)
        return nullptr;

    // Length has to be exact: a hand-counted 39 for a 38-char name makes
    // RoGetActivationFactory return E_INVALIDARG.
    const wchar_t* className = L"Windows.UI.Xaml.Media.VisualTreeHelper";
    HSTRING name = nullptr;
    if (FAILED(g_createString(className, static_cast<UINT32>(wcslen(className)), &name)) || !name)
        return nullptr;

    void* factory = nullptr;
    HRESULT hr = g_roGetActivationFactory(name, kIActivationFactory, &factory);
    g_deleteString(name);
    Log(L"RoGetActivationFactory(VisualTreeHelper): 0x%08X", hr);
    if (FAILED(hr) || !factory)
        return nullptr;

    IVisualTreeHelperStaticsABI* statics = nullptr;
    hr = reinterpret_cast<IInspectableABI*>(factory)->QueryInterface(
        kIVisualTreeHelperStatics, reinterpret_cast<void**>(&statics));
    reinterpret_cast<IInspectableABI*>(factory)->Release();
    Log(L"QI IVisualTreeHelperStatics: 0x%08X", hr);
    return SUCCEEDED(hr) ? statics : nullptr;
}

static void DescribeNode(IDependencyObjectABI* node, wchar_t* className, size_t cch,
                         double* width, double* height)
{
    auto inspectable = reinterpret_cast<IInspectableABI*>(node);

    HSTRING hname = nullptr;
    if (SUCCEEDED(inspectable->GetRuntimeClassName(&hname)) && hname) {
        UINT32 length = 0;
        const wchar_t* raw = g_getStringRawBuffer ? g_getStringRawBuffer(hname, &length) : L"?";
        wcsncpy_s(className, cch, raw, _TRUNCATE);
        if (g_deleteString)
            g_deleteString(hname);
    }

    IFrameworkElementABI* element = nullptr;
    if (SUCCEEDED(inspectable->QueryInterface(kIFrameworkElement, reinterpret_cast<void**>(&element))) && element) {
        element->get_ActualWidth(width);
        element->get_ActualHeight(height);
        element->Release();
    }
}

static void GetElementName(IDependencyObjectABI* node, wchar_t* out, size_t cch)
{
    out[0] = 0;

    // get_Name lives at slot 33, past the end of IDependencyObject. Calling it
    // on an object that is not a FrameworkElement is a wild call that takes the
    // shell down, so confirm the interface first.
    IFrameworkElementABI* element = nullptr;
    if (FAILED(reinterpret_cast<IInspectableABI*>(node)->QueryInterface(
            kIFrameworkElement, reinterpret_cast<void**>(&element))) || !element)
        return;
    // Call through the IFrameworkElement pointer, not through `node`: `node`'s
    // vtable is IDependencyObject's, which has only a handful of slots, so
    // reading slot 33 off it lands in the middle of unrelated code.
    void** vtable = *reinterpret_cast<void***>(element);
    if (!vtable) {
        element->Release();
        return;
    }
    typedef HRESULT (STDMETHODCALLTYPE* GetNameFn)(void*, HSTRING*);
    auto getName = reinterpret_cast<GetNameFn>(vtable[33]);
    if (!getName) {
        element->Release();
        return;
    }
    HSTRING name = nullptr;
    if (SUCCEEDED(getName(element, &name)) && name) {
        UINT32 length = 0;
        const wchar_t* raw = g_getStringRawBuffer ? g_getStringRawBuffer(name, &length) : L"";
        wcsncpy_s(out, cch, raw, _TRUNCATE);
        if (g_deleteString)
            g_deleteString(name);
    }
    element->Release();
}

static void WalkTree(IVisualTreeHelperStaticsABI* helper, IDependencyObjectABI* node, int depth)
{
    if (depth > 9)
        return;

    UINT32 count = 0;
    if (FAILED(helper->GetChildrenCount(node, &count)))
        return;

    for (UINT32 i = 0; i < count && i < 8; ++i) {
        IDependencyObjectABI* child = nullptr;
        if (FAILED(helper->GetChild(node, static_cast<INT32>(i), &child)) || !child)
            continue;

        wchar_t className[128] = L"?";
        double width = -1, height = -1;
        DescribeNode(child, className, 128, &width, &height);

        UINT32 grandchildren = 0;
        helper->GetChildrenCount(child, &grandchildren);

        wchar_t indent[32] = {};
        for (int k = 0; k < depth * 2 + 2 && k < 30; ++k)
            indent[k] = L' ';
        Log(L"%s[%u] %s  %.0fx%.0f  children=%u", indent, i, className, width, height, grandchildren);

        WalkTree(helper, child, depth + 1);
        child->Release();
    }
}

static IDependencyObjectABI* g_itemsPanel = nullptr;
static IDependencyObjectABI* g_slidersGroup = nullptr;
static IDependencyObjectABI* g_l1Grid = nullptr;

static IInspectableABI* Activate(const wchar_t* className)
{
    HSTRING name = nullptr;
    if (FAILED(g_createString(className, static_cast<UINT32>(wcslen(className)), &name)) || !name)
        return nullptr;
    void* instance = nullptr;
    HRESULT hr = g_roActivateInstance(name, &instance);
    g_deleteString(name);
    if (FAILED(hr) || !instance) {
        Log(L"activate %s: 0x%08X", className, hr);
        return nullptr;
    }
    return reinterpret_cast<IInspectableABI*>(instance);
}

// ---------------------------------------------------------------------------
// The chevron button on the brightness row
// ---------------------------------------------------------------------------

// ae2d9271-3b4a-45fc-8468-f7949548f4d5 - put_Text is method 21.
struct ITextBlockABI : IInspectableABI
{
    virtual void* u0();
    virtual HRESULT STDMETHODCALLTYPE put_FontSize(double value) = 0;  // slot 7
    virtual void* u2();
    virtual HRESULT STDMETHODCALLTYPE put_FontFamily(IInspectableABI* value) = 0;  // slot 9
    virtual void* u4(); virtual void* u5(); virtual void* u6(); virtual void* u7();
    virtual void* u8(); virtual void* u9(); virtual void* u10(); virtual void* u11();
    virtual void* u12();
    virtual HRESULT STDMETHODCALLTYPE put_Foreground(IInspectableABI* value) = 0;  // slot 19
    virtual void* u14(); virtual void* u15(); virtual void* u16(); virtual void* u17();
    virtual void* u18(); virtual void* u19(); virtual void* u20();
    virtual HRESULT STDMETHODCALLTYPE put_Text(HSTRING value) = 0;  // slot 27
};

// 9d850850-66f3-48df-9a8f-824bd5e070af - put_Color is method 1.
struct ColorABI
{
    BYTE a, r, g, b;
};

struct ISolidColorBrushABI : IInspectableABI
{
    virtual void* unusedGetColor();                                 // slot 6
    virtual HRESULT STDMETHODCALLTYPE put_Color(ColorABI value) = 0;  // slot 7
};

// d5603377-3dae-4dcd-af09-f9498e9ec659 - CreateInstanceWithName is method 0.
struct IFontFamilyFactoryABI : IInspectableABI
{
    virtual HRESULT STDMETHODCALLTYPE CreateInstanceWithName(HSTRING familyName,
                                                             IInspectableABI* baseUri,
                                                             IInspectableABI** result) = 0;
};

static const IID kIFontFamilyFactory = { 0xd5603377, 0x3dae, 0x4dcd, { 0xaf, 0x09, 0xf9, 0x49, 0x8e, 0x9e, 0xc6, 0x59 } };

static IInspectableABI* MakeFontFamily(const wchar_t* family)
{
    HSTRING className = nullptr;
    const wchar_t* cls = L"Windows.UI.Xaml.Media.FontFamily";
    g_createString(cls, static_cast<UINT32>(wcslen(cls)), &className);
    void* factory = nullptr;
    HRESULT hr = g_roGetActivationFactory(className, kIActivationFactory, &factory);
    g_deleteString(className);
    if (FAILED(hr) || !factory) {
        Log(L"fontfamily: factory 0x%08X", hr);
        return nullptr;
    }

    IFontFamilyFactoryABI* fontFactory = nullptr;
    hr = reinterpret_cast<IInspectableABI*>(factory)->QueryInterface(
        kIFontFamilyFactory, reinterpret_cast<void**>(&fontFactory));
    if (FAILED(hr) || !fontFactory) {
        Log(L"fontfamily: QI 0x%08X", hr);
        reinterpret_cast<IInspectableABI*>(factory)->Release();
        return nullptr;
    }

    HSTRING name = nullptr;
    g_createString(family, static_cast<UINT32>(wcslen(family)), &name);
    IInspectableABI* result = nullptr;
    hr = fontFactory->CreateInstanceWithName(name, nullptr, &result);
    g_deleteString(name);
    fontFactory->Release();
    reinterpret_cast<IInspectableABI*>(factory)->Release();
    Log(L"fontfamily: '%s' 0x%08X", family, hr);
    return SUCCEEDED(hr) ? result : nullptr;
}

static const IID kISolidColorBrush = { 0x9d850850, 0x66f3, 0x48df, { 0x9a, 0x8f, 0x82, 0x4b, 0xd5, 0xe0, 0x70, 0xaf } };

// fa002c1a-494e-46cf-91d4-e14a8d798674 - add_Click is method 8.
struct IButtonBaseABI : IInspectableABI
{
    virtual void* u0(); virtual void* u1(); virtual void* u2(); virtual void* u3();
    virtual void* u4(); virtual void* u5(); virtual void* u6(); virtual void* u7();
    virtual HRESULT STDMETHODCALLTYPE add_Click(void* handler, INT64* token) = 0;  // slot 14
};

// IControl: put_Background is method 31 (slot 37), put_BorderBrush 32 (slot 38).
struct IControlABI : IInspectableABI
{
    virtual void* c0(); virtual void* c1(); virtual void* c2(); virtual void* c3();
    virtual void* c4(); virtual void* c5(); virtual void* c6(); virtual void* c7();
    virtual void* c8(); virtual void* c9(); virtual void* c10(); virtual void* c11();
    virtual void* c12(); virtual void* c13(); virtual void* c14(); virtual void* c15();
    virtual void* c16(); virtual void* c17(); virtual void* c18(); virtual void* c19();
    virtual void* c20(); virtual void* c21(); virtual void* c22(); virtual void* c23();
    virtual void* c24(); virtual void* c25(); virtual void* c26(); virtual void* c27();
    virtual void* c28(); virtual void* c29(); virtual void* c30();
    virtual HRESULT STDMETHODCALLTYPE put_Background(IInspectableABI* value) = 0;   // slot 37
    virtual HRESULT STDMETHODCALLTYPE put_BorderBrush(IInspectableABI* value) = 0;  // slot 38
};

// IStackPanel: put_Orientation is method 3 (slot 9).
struct IStackPanelABI : IInspectableABI
{
    virtual void* s0(); virtual void* s1(); virtual void* s2();
    virtual HRESULT STDMETHODCALLTYPE put_Orientation(int value) = 0;  // slot 9
};

static const IID kIControl = { 0xa8912263, 0x2951, 0x4f58, { 0xa9, 0xc5, 0x5a, 0x13, 0x4e, 0xaa, 0x7f, 0x07 } };
static const IID kIStackPanel = { 0xb8ae8fe2, 0xd641, 0x4fd7, { 0x80, 0xb4, 0x74, 0x39, 0x20, 0x7d, 0x27, 0x98 } };

static const IID kITextBlock = { 0xae2d9271, 0x3b4a, 0x45fc, { 0x84, 0x68, 0xf7, 0x94, 0x95, 0x48, 0xf4, 0xd5 } };
static const IID kIButtonBase = { 0xfa002c1a, 0x494e, 0x46cf, { 0x91, 0xd4, 0xe1, 0x4a, 0x8d, 0x79, 0x86, 0x74 } };
static const IID kIRoutedEventHandler = { 0x68e0e810, 0xf6ea, 0x42bc, { 0x85, 0x5b, 0x5d, 0x9b, 0x67, 0xe6, 0xa2, 0x62 } };

static void ShowMonitorPanel();  // built later

struct ClickHandler : IUnknown
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == kIRoutedEventHandler || riid == IID_IUnknown || riid == kIAgileObject) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectableABI*, void*)
    {
        Log(L"chevron: clicked");
        ShowMonitorPanel();
        return S_OK;
    }
};

static ClickHandler g_clickHandler;

static void BuildChevron()
{
    if (!g_stockSlider) {
        Log(L"chevron: no stock slider");
        return;
    }

    IVisualTreeHelperStaticsABI* helper = GetVisualTreeHelper();
    if (!helper)
        return;

    IDependencyObjectABI* sliderObject = nullptr;
    if (FAILED(reinterpret_cast<IInspectableABI*>(g_stockSlider)->QueryInterface(
            kIDependencyObject, reinterpret_cast<void**>(&sliderObject))) || !sliderObject) {
        helper->Release();
        return;
    }

    // The row is the Grid that holds the icon and the slider.
    IDependencyObjectABI* row = nullptr;
    HRESULT hr = helper->GetParent(sliderObject, &row);
    Log(L"chevron: GetParent(row) 0x%08X", hr);
    sliderObject->Release();
    helper->Release();
    if (FAILED(hr) || !row)
        return;

    wchar_t rowName[128] = L"?";
    double rowW = -1, rowH = -1;
    DescribeNode(row, rowName, 128, &rowW, &rowH);
    Log(L"chevron: row is %s %.0fx%.0f", rowName, rowW, rowH);

    // A glyph, because an empty Button has no visual content.
    IInspectableABI* text = Activate(L"Windows.UI.Xaml.Controls.TextBlock");
    if (text) {
        ITextBlockABI* textBlock = nullptr;
        if (SUCCEEDED(text->QueryInterface(kITextBlock, reinterpret_cast<void**>(&textBlock))) && textBlock) {
            HSTRING glyph = nullptr;
            const wchar_t* chevron = L"\xE76C";  // Segoe Fluent Icons: ChevronRight
            g_createString(chevron, 1, &glyph);
            HRESULT hrText = textBlock->put_Text(glyph);
            Log(L"chevron: put_Text 0x%08X", hrText);
            g_deleteString(glyph);
            textBlock->put_FontSize(14);

            IInspectableABI* fontFamily = MakeFontFamily(L"Segoe Fluent Icons");
            if (fontFamily) {
                HRESULT hrFont = textBlock->put_FontFamily(fontFamily);
                Log(L"chevron: put_FontFamily 0x%08X", hrFont);
                fontFamily->Release();
            }

            IInspectableABI* brush = Activate(L"Windows.UI.Xaml.Media.SolidColorBrush");
            if (brush) {
                ISolidColorBrushABI* solid = nullptr;
                if (SUCCEEDED(brush->QueryInterface(kISolidColorBrush, reinterpret_cast<void**>(&solid))) && solid) {
                    ColorABI white = { 255, 255, 255, 255 };
                    HRESULT hrColor = solid->put_Color(white);
                    HRESULT hrFg = textBlock->put_Foreground(brush);
                    Log(L"chevron: put_Color 0x%08X put_Foreground 0x%08X", hrColor, hrFg);
                    solid->Release();
                }
                brush->Release();
            }
            textBlock->Release();
        }
    }

    IInspectableABI* button = Activate(L"Windows.UI.Xaml.Controls.Button");
    if (!button) {
        if (text) text->Release();
        row->Release();
        return;
    }

    if (text) {
        IContentControlABI* content = nullptr;
        if (SUCCEEDED(button->QueryInterface(kIContentControl, reinterpret_cast<void**>(&content))) && content) {
            hr = content->put_Content(text);
            Log(L"chevron: put_Content(glyph) 0x%08X", hr);
            content->Release();
        }
        text->Release();
    }

    IFrameworkElementABI* element = nullptr;
    if (SUCCEEDED(button->QueryInterface(kIFrameworkElement, reinterpret_cast<void**>(&element))) && element) {
        // Big enough that the glyph is not clipped by the button's padding.
        element->put_Width(40);
        element->put_Height(32);
        element->put_HorizontalAlignment(2);  // Right
        element->Release();
    }

    IPanelABI* rowPanel = nullptr;
    if (SUCCEEDED(row->QueryInterface(kIPanel, reinterpret_cast<void**>(&rowPanel))) && rowPanel) {
        IInspectableABI* children = nullptr;
        if (SUCCEEDED(rowPanel->get_Children(&children)) && children) {
            IVectorABI* vector = reinterpret_cast<IVectorABI*>(children);
            unsigned before = 0, after = 0;
            vector->get_Size(&before);
            hr = vector->Append(button);
            Log(L"chevron: append button 0x%08X", hr);
            vector->get_Size(&after);
            Log(L"chevron: row children %u -> %u", before, after);
            children->Release();
        }
        rowPanel->Release();
    }

    // The row is a Grid of icon | slider, so a child appended without a column
    // lands in column 0 and is clipped behind the icon.
    IGridABI* rowGrid = nullptr;
    unsigned columnCount = 0;
    if (SUCCEEDED(row->QueryInterface(kIGrid, reinterpret_cast<void**>(&rowGrid))) && rowGrid) {
        IInspectableABI* columnsRaw = nullptr;
        if (SUCCEEDED(rowGrid->get_ColumnDefinitions(&columnsRaw)) && columnsRaw) {
            unsigned size = 0;
            reinterpret_cast<IVectorABI*>(columnsRaw)->get_Size(&size);
            columnCount = size;
            columnsRaw->Release();
        }
        rowGrid->Release();
    }
    Log(L"chevron: row has %u column(s)", columnCount);

    if (columnCount > 0) {
        // Same route as Grid.Row: the static setter faults, the property works.
        HSTRING gridName = nullptr;
        const wchar_t* gridClass = L"Windows.UI.Xaml.Controls.Grid";
        g_createString(gridClass, static_cast<UINT32>(wcslen(gridClass)), &gridName);
        void* gridFactory = nullptr;
        if (SUCCEEDED(g_roGetActivationFactory(gridName, kIActivationFactory, &gridFactory)) && gridFactory) {
            IGridStaticsABI* statics = nullptr;
            if (SUCCEEDED(reinterpret_cast<IInspectableABI*>(gridFactory)->QueryInterface(
                    kIGridStatics, reinterpret_cast<void**>(&statics))) && statics) {
                IDependencyObjectABI* columnProperty = nullptr;
                if (SUCCEEDED(statics->get_ColumnProperty(&columnProperty)) && columnProperty) {
                    IInspectableABI* buttonObject = nullptr;
                    if (SUCCEEDED(button->QueryInterface(kIDependencyObject, reinterpret_cast<void**>(&buttonObject))) && buttonObject) {
                        HSTRING pvName = nullptr;
                        const wchar_t* pvClass = L"Windows.Foundation.PropertyValue";
                        g_createString(pvClass, static_cast<UINT32>(wcslen(pvClass)), &pvName);
                        void* pvFactory = nullptr;
                        if (SUCCEEDED(g_roGetActivationFactory(pvName, kIActivationFactory, &pvFactory)) && pvFactory) {
                            IPropertyValueStaticsABI* pv = nullptr;
                            if (SUCCEEDED(reinterpret_cast<IInspectableABI*>(pvFactory)->QueryInterface(
                                    kIPropertyValueStatics, reinterpret_cast<void**>(&pv))) && pv) {
                                IInspectableABI* boxed = nullptr;
                                int column = static_cast<int>(columnCount - 1);
                                if (SUCCEEDED(pv->CreateInt32(column, &boxed)) && boxed) {
                                    HRESULT hrColumn = reinterpret_cast<IDependencyObjectSetValueABI*>(buttonObject)
                                                           ->SetValue(columnProperty, boxed);
                                    Log(L"chevron: SetValue(Grid.Column, %d) 0x%08X", column, hrColumn);
                                    boxed->Release();
                                }
                                pv->Release();
                            }
                            reinterpret_cast<IInspectableABI*>(pvFactory)->Release();
                        }
                        g_deleteString(pvName);
                        columnProperty->Release();
                        buttonObject->Release();
                    }
                }
                statics->Release();
            }
            reinterpret_cast<IInspectableABI*>(gridFactory)->Release();
        }
        g_deleteString(gridName);
    }

    IButtonBaseABI* buttonBase = nullptr;
    if (SUCCEEDED(button->QueryInterface(kIButtonBase, reinterpret_cast<void**>(&buttonBase))) && buttonBase) {
        INT64 token = 0;
        hr = buttonBase->add_Click(&g_clickHandler, &token);
        Log(L"chevron: add_Click 0x%08X", hr);
        buttonBase->Release();
    }

    button->Release();
    row->Release();
}

// Adds a row to L1Grid and puts the panel in it, which is how the shell lays out
// its own sections. Appending into the virtualizing sliders panel does not
// survive a layout pass, and re-parenting the GridView is refused outright.
static void InsertPanelIntoGrid()
{
    if (!g_l1Grid) {
        Log(L"insert: no L1Grid captured");
        return;
    }

    IGridABI* grid = nullptr;
    HRESULT hr = reinterpret_cast<IInspectableABI*>(g_l1Grid)->QueryInterface(
        kIGrid, reinterpret_cast<void**>(&grid));
    Log(L"insert: QI IGrid 0x%08X", hr);
    if (FAILED(hr) || !grid)
        return;

    IInspectableABI* rowsRaw = nullptr;
    hr = grid->get_RowDefinitions(&rowsRaw);
    Log(L"insert: get_RowDefinitions 0x%08X", hr);
    if (FAILED(hr) || !rowsRaw) {
        grid->Release();
        return;
    }
    IVectorABI* rows = reinterpret_cast<IVectorABI*>(rowsRaw);

    unsigned rowCount = 0;
    rows->get_Size(&rowCount);
    Log(L"insert: L1Grid has %u row(s)", rowCount);

    // Appending a row to a grid that declares none re-flows every existing child
    // out of the implicit row 0, so refuse rather than wreck the panel.
    if (rowCount == 0) {
        Log(L"insert: no RowDefinitions - refusing to re-flow");
        rowsRaw->Release();
        grid->Release();
        return;
    }

    IInspectableABI* rowDefinition = Activate(L"Windows.UI.Xaml.Controls.RowDefinition");
    if (!rowDefinition) {
        rowsRaw->Release();
        grid->Release();
        return;
    }

    IRowDefinitionABI* row = nullptr;
    hr = rowDefinition->QueryInterface(kIRowDefinition, reinterpret_cast<void**>(&row));
    Log(L"insert: QI IRowDefinition 0x%08X", hr);
    if (SUCCEEDED(hr) && row) {
        GridLengthABI autoHeight = { 0.0, 0, 0 };  // GridUnitType::Auto
        hr = row->put_Height(autoHeight);
        Log(L"insert: put_Height(Auto) 0x%08X", hr);
        row->Release();
    }

    hr = rows->Append(rowDefinition);
    Log(L"insert: append RowDefinition 0x%08X", hr);
    rowDefinition->Release();

    // Rows 0..2 are the shell's (toggles, sliders, footer). Ours goes in row 2
    // so it sits under the volume slider, and the footer moves to the appended
    // row 3 to make room.
    unsigned newRow = (rowCount >= 2) ? 2u : rowCount;
    unsigned footerRow = rowCount;

    IInspectableABI* panel = Activate(L"Windows.UI.Xaml.Controls.StackPanel");
    if (!panel) {
        rowsRaw->Release();
        grid->Release();
        return;
    }

    IInspectableABI* slider = Activate(L"Windows.UI.Xaml.Controls.Slider");
    if (slider) {
        IFrameworkElementABI* element = nullptr;
        if (SUCCEEDED(slider->QueryInterface(kIFrameworkElement, reinterpret_cast<void**>(&element))) && element) {
            element->put_Width(332);
            element->put_Height(44);
            element->Release();
        }

        IPanelABI* panelInterface = nullptr;
        if (SUCCEEDED(panel->QueryInterface(kIPanel, reinterpret_cast<void**>(&panelInterface))) && panelInterface) {
            IInspectableABI* panelChildren = nullptr;
            if (SUCCEEDED(panelInterface->get_Children(&panelChildren)) && panelChildren) {
                hr = reinterpret_cast<IVectorABI*>(panelChildren)->Append(slider);
                Log(L"insert: append Slider to panel 0x%08X", hr);
                panelChildren->Release();
            }
            panelInterface->Release();
        }
        slider->Release();
    }

    // SetRow on the factory's static interface faults no matter what it is
    // given, so set Grid.RowProperty directly. The element has to be QI'd to
    // IDependencyObject first: RoActivateInstance hands back IInspectable,
    // whose vtable is not IDependencyObject's.
    IInspectableABI* panelObject = nullptr;
    HRESULT hrObject = panel->QueryInterface(kIDependencyObject, reinterpret_cast<void**>(&panelObject));
    Log(L"insert: QI IDependencyObject on panel 0x%08X", hrObject);
    if (SUCCEEDED(hrObject) && panelObject) {
        HSTRING gridName = nullptr;
        const wchar_t* gridClass = L"Windows.UI.Xaml.Controls.Grid";
        g_createString(gridClass, static_cast<UINT32>(wcslen(gridClass)), &gridName);
        void* gridFactory = nullptr;
        HRESULT hrFactory = g_roGetActivationFactory(gridName, kIActivationFactory, &gridFactory);
        g_deleteString(gridName);
        if (SUCCEEDED(hrFactory) && gridFactory) {
            IGridStaticsABI* statics = nullptr;
            if (SUCCEEDED(reinterpret_cast<IInspectableABI*>(gridFactory)->QueryInterface(
                    kIGridStatics, reinterpret_cast<void**>(&statics))) && statics) {
                IDependencyObjectABI* rowProperty = nullptr;
                if (SUCCEEDED(statics->get_RowProperty(&rowProperty)) && rowProperty) {
                    HSTRING pvName = nullptr;
                    const wchar_t* pvClass = L"Windows.Foundation.PropertyValue";
                    g_createString(pvClass, static_cast<UINT32>(wcslen(pvClass)), &pvName);
                    void* pvFactory = nullptr;
                    if (SUCCEEDED(g_roGetActivationFactory(pvName, kIActivationFactory, &pvFactory)) && pvFactory) {
                        IPropertyValueStaticsABI* pv = nullptr;
                        if (SUCCEEDED(reinterpret_cast<IInspectableABI*>(pvFactory)->QueryInterface(
                                kIPropertyValueStatics, reinterpret_cast<void**>(&pv))) && pv) {
                            IInspectableABI* boxed = nullptr;
                            if (SUCCEEDED(pv->CreateInt32(static_cast<int>(newRow), &boxed)) && boxed) {
                                HRESULT hrSet = reinterpret_cast<IDependencyObjectSetValueABI*>(panelObject)
                                                    ->SetValue(rowProperty, boxed);
                                Log(L"insert: SetValue(Grid.Row, %u) 0x%08X", newRow, hrSet);
                                boxed->Release();
                            }
                            pv->Release();
                        }
                        reinterpret_cast<IInspectableABI*>(pvFactory)->Release();
                    }
                    g_deleteString(pvName);
                    rowProperty->Release();
                }
                statics->Release();
            }
            reinterpret_cast<IInspectableABI*>(gridFactory)->Release();
        }
        panelObject->Release();
    }

    IPanelABI* gridPanel = nullptr;
    if (SUCCEEDED(reinterpret_cast<IInspectableABI*>(g_l1Grid)->QueryInterface(
            kIPanel, reinterpret_cast<void**>(&gridPanel))) && gridPanel) {
        IInspectableABI* gridChildren = nullptr;
        if (SUCCEEDED(gridPanel->get_Children(&gridChildren)) && gridChildren) {
            IVectorABI* children = reinterpret_cast<IVectorABI*>(gridChildren);
            unsigned before = 0, after = 0;
            children->get_Size(&before);
            hr = children->Append(panel);
            Log(L"insert: append panel to L1Grid 0x%08X", hr);
            children->get_Size(&after);
            Log(L"insert: L1Grid children %u -> %u", before, after);

            // Push the footer down into the row we appended.
            IInspectableABI* footer = nullptr;
            if (SUCCEEDED(children->GetAt(3, &footer)) && footer) {
                IInspectableABI* footerObject = nullptr;
                if (SUCCEEDED(footer->QueryInterface(kIDependencyObject, reinterpret_cast<void**>(&footerObject))) && footerObject) {
                    HSTRING gridName2 = nullptr;
                    const wchar_t* gridClass2 = L"Windows.UI.Xaml.Controls.Grid";
                    g_createString(gridClass2, static_cast<UINT32>(wcslen(gridClass2)), &gridName2);
                    void* gridFactory2 = nullptr;
                    if (SUCCEEDED(g_roGetActivationFactory(gridName2, kIActivationFactory, &gridFactory2)) && gridFactory2) {
                        IGridStaticsABI* statics2 = nullptr;
                        if (SUCCEEDED(reinterpret_cast<IInspectableABI*>(gridFactory2)->QueryInterface(
                                kIGridStatics, reinterpret_cast<void**>(&statics2))) && statics2) {
                            IDependencyObjectABI* rowProperty2 = nullptr;
                            if (SUCCEEDED(statics2->get_RowProperty(&rowProperty2)) && rowProperty2) {
                                HSTRING pvName2 = nullptr;
                                const wchar_t* pvClass2 = L"Windows.Foundation.PropertyValue";
                                g_createString(pvClass2, static_cast<UINT32>(wcslen(pvClass2)), &pvName2);
                                void* pvFactory2 = nullptr;
                                if (SUCCEEDED(g_roGetActivationFactory(pvName2, kIActivationFactory, &pvFactory2)) && pvFactory2) {
                                    IPropertyValueStaticsABI* pv2 = nullptr;
                                    if (SUCCEEDED(reinterpret_cast<IInspectableABI*>(pvFactory2)->QueryInterface(
                                            kIPropertyValueStatics, reinterpret_cast<void**>(&pv2))) && pv2) {
                                        IInspectableABI* boxed2 = nullptr;
                                        if (SUCCEEDED(pv2->CreateInt32(static_cast<int>(footerRow), &boxed2)) && boxed2) {
                                            HRESULT hrFooter = reinterpret_cast<IDependencyObjectSetValueABI*>(footerObject)
                                                                   ->SetValue(rowProperty2, boxed2);
                                            Log(L"insert: footer SetValue(Grid.Row, %u) 0x%08X", footerRow, hrFooter);
                                            boxed2->Release();
                                        }
                                        pv2->Release();
                                    }
                                    reinterpret_cast<IInspectableABI*>(pvFactory2)->Release();
                                }
                                g_deleteString(pvName2);
                                rowProperty2->Release();
                            }
                            statics2->Release();
                        }
                        reinterpret_cast<IInspectableABI*>(gridFactory2)->Release();
                    }
                    g_deleteString(gridName2);
                    footerObject->Release();
                }
                footer->Release();
            }
            gridChildren->Release();
        }
        gridPanel->Release();
    }

    panel->Release();
    rowsRaw->Release();
    grid->Release();
}

// Depth-first search for Slider elements, logging the chain that leads to them.
static void SearchSliders(IVisualTreeHelperStaticsABI* helper, IDependencyObjectABI* node,
                          int depth, int* hits)
{
    if (depth > 30 || *hits > 24)
        return;

    UINT32 count = 0;
    if (FAILED(helper->GetChildrenCount(node, &count)))
        return;

    for (UINT32 i = 0; i < count && *hits <= 24; ++i) {
        IDependencyObjectABI* child = nullptr;
        if (FAILED(helper->GetChild(node, static_cast<INT32>(i), &child)) || !child)
            continue;

        wchar_t className[128] = L"?";
        double width = -1, height = -1;
        DescribeNode(child, className, 128, &width, &height);

        wchar_t elementName[128] = {};
        GetElementName(child, elementName, 128);

        if (wcscmp(elementName, L"L1Grid") == 0 && !g_l1Grid) {
            child->AddRef();
            g_l1Grid = child;
            Log(L"captured L1Grid: %s %.0fx%.0f", className, width, height);
        }

        if (wcsstr(className, L"ItemsStackPanel") && !g_itemsPanel) {
            child->AddRef();
            g_itemsPanel = child;
            Log(L"captured ItemsStackPanel: %.0fx%.0f", width, height);
        }

        if (wcscmp(elementName, L"SlidersGroup") == 0 && !g_slidersGroup) {
            child->AddRef();
            g_slidersGroup = child;
        }
        if (wcscmp(elementName, L"SlidersGroup") == 0) {
            Log(L"FOUND SlidersGroup at depth %d: %s %.0fx%.0f", depth, className, width, height);
            Log(L"--- SlidersGroup subtree ---");
            WalkTree(helper, child, 0);
            Log(L"--- end subtree ---");
            int groupHits = 0;
            SearchSliders(helper, child, 0, &groupHits);
            Log(L"sliders under SlidersGroup: %d", groupHits);
        }

        if (wcsstr(className, L"Slider") && !g_stockSlider) {
            child->AddRef();
            g_stockSlider = child;
            Log(L"captured stock slider: %s %.0fx%.0f", className, width, height);
        }

        if (wcsstr(className, L"Slider") || wcsstr(className, L"Brightness")) {
            wchar_t parentName[128] = L"?";
            double pw = -1, ph = -1;
            IDependencyObjectABI* parent = nullptr;
            UINT32 parentKids = 0;
            if (SUCCEEDED(helper->GetParent(child, &parent)) && parent) {
                DescribeNode(parent, parentName, 128, &pw, &ph);
                helper->GetChildrenCount(parent, &parentKids);
                parent->Release();
            }
            Log(L"HIT depth=%d %s name='%s' %.0fx%.0f  parent=%s %.0fx%.0f children=%u",
                depth, className, elementName, width, height, parentName, pw, ph, parentKids);
            (*hits)++;
        }

        SearchSliders(helper, child, depth + 1, hits);
        child->Release();
    }
}

static void InspectView(void* self)
{
    auto inspectable = reinterpret_cast<IInspectableABI*>(self);

    LogClassName(L"view class", inspectable);

    IVisualTreeHelperStaticsABI* helper = GetVisualTreeHelper();
    if (!helper) {
        Log(L"VisualTreeHelper unavailable");
        return;
    }

    IDependencyObjectABI* root = nullptr;
    if (FAILED(inspectable->QueryInterface(kIDependencyObject, reinterpret_cast<void**>(&root))) || !root) {
        Log(L"view is not a DependencyObject");
        helper->Release();
        return;
    }

    WalkTree(helper, root, 0);

    // The view is one card; the sliders are in a sibling, so climb first.
    IDependencyObjectABI* node = root;
    node->AddRef();
    for (int up = 0; up < 8; ++up) {
        IDependencyObjectABI* parent = nullptr;
        if (FAILED(helper->GetParent(node, &parent)) || !parent) {
            Log(L"  up %d: <no parent>", up + 1);
            break;
        }
        wchar_t name[128] = L"?";
        double w = -1, h = -1;
        DescribeNode(parent, name, 128, &w, &h);
        UINT32 kids = 0;
        helper->GetChildrenCount(parent, &kids);
        Log(L"  up %d: %s  %.0fx%.0f  children=%u", up + 1, name, w, h, kids);
        node->Release();
        node = parent;
    }

    Log(L"searching for Slider below the top ancestor");
    int hits = 0;
    SearchSliders(helper, node, 0, &hits);
    Log(L"slider hits: %d", hits);

    // The test row is gone: the chevron panel is the real UI now.
    AttachStockSlider();

    node->Release();
    root->Release();
    helper->Release();
}

static void WalkLight(IVisualTreeHelperStaticsABI* helper, IDependencyObjectABI* node, int depth)
{
    if (depth > 12)
        return;

    UINT32 count = 0;
    if (FAILED(helper->GetChildrenCount(node, &count)))
        return;

    for (UINT32 i = 0; i < count && i < 6; ++i) {
        IDependencyObjectABI* child = nullptr;
        if (FAILED(helper->GetChild(node, static_cast<INT32>(i), &child)) || !child)
            continue;

        // Class name only: no IFrameworkElement QI, no size read.
        wchar_t className[128] = L"?";
        HSTRING hname = nullptr;
        if (SUCCEEDED(reinterpret_cast<IInspectableABI*>(child)->GetRuntimeClassName(&hname)) && hname) {
            UINT32 length = 0;
            const wchar_t* raw = g_getStringRawBuffer ? g_getStringRawBuffer(hname, &length) : L"?";
            wcsncpy_s(className, raw, _TRUNCATE);
            if (g_deleteString)
                g_deleteString(hname);
        }

        wchar_t indent[40] = {};
        for (int k = 0; k < depth * 2 + 2 && k < 38; ++k)
            indent[k] = L' ';
        Log(L"%s[%u] %s", indent, i, className);

        WalkLight(helper, child, depth + 1);
        child->Release();
    }
}

// ---------------------------------------------------------------------------
// Dispatcher deferral
//
// The XAML work must not run inside the focus event: doing it there takes
// ShellHost down with 0xc000001d right after the calls return. So the hook only
// captures the view and posts the work to the thread's dispatcher queue.
// ---------------------------------------------------------------------------

struct IDispatcherQueueABI;

// a96d83d7-9371-4517-9245-d0824ac12c74
struct IDispatcherQueueStaticsABI : IInspectableABI
{
    virtual HRESULT STDMETHODCALLTYPE GetForCurrentThread(IDispatcherQueueABI** result) = 0;
};

// 603e88e4-a338-4ffe-a457-a5cfb9ceb899
struct IDispatcherQueueABI : IInspectableABI
{
    virtual HRESULT STDMETHODCALLTYPE CreateTimer(void** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryEnqueue(void* callback, BOOL* result) = 0;
};

static const IID kIDispatcherQueueStatics = { 0xa96d83d7, 0x9371, 0x4517, { 0x92, 0x45, 0xd0, 0x82, 0x4a, 0xc1, 0x2c, 0x74 } };
static const IID kIDispatcherQueue = { 0x603e88e4, 0xa338, 0x4ffe, { 0xa4, 0x57, 0xa5, 0xcf, 0xb9, 0xce, 0xb8, 0x99 } };
static const IID kIDispatcherQueueHandler = { 0xdfa2dc9c, 0x1a2d, 0x4917, { 0x98, 0xf2, 0x93, 0x9a, 0xf1, 0xd6, 0xe0, 0xc8 } };
// TryEnqueue refuses a handler that is not agile: 0x8000001C, "The object must
// support the IAgileObject interface".

// The shell rebuilds the flyout's content, so a cached panel pointer goes stale
// and hiding it silently does nothing. Everything goes through the current tree,
// finding the panel by name.
static const wchar_t* kPanelName = L"BCMonitorPanel";

// put_Name is IFrameworkElement method 28 (slot 34).
static void SetElementName(IInspectableABI* element, const wchar_t* name)
{
    IFrameworkElementABI* framework = nullptr;
    if (FAILED(element->QueryInterface(kIFrameworkElement, reinterpret_cast<void**>(&framework))) || !framework)
        return;
    void** vtable = *reinterpret_cast<void***>(framework);
    typedef HRESULT (STDMETHODCALLTYPE* PutNameFn)(void*, HSTRING);
    auto putName = reinterpret_cast<PutNameFn>(vtable[34]);
    if (putName) {
        HSTRING value = nullptr;
        g_createString(name, static_cast<UINT32>(wcslen(name)), &value);
        putName(framework, value);
        g_deleteString(value);
    }
    framework->Release();
}

// Visibility lives on IUIElement. Calling it through a DependencyObject pointer
// is a wild vtable call - it returns an error and hides nothing, which is why
// the panel kept showing.
static HRESULT SetVisibility(IDependencyObjectABI* node, int visibility)
{
    if (!node)
        return E_POINTER;
    IInspectableABI* element = nullptr;
    if (FAILED(reinterpret_cast<IInspectableABI*>(node)->QueryInterface(
            kIUIElement, reinterpret_cast<void**>(&element))) || !element)
        return E_NOINTERFACE;
    HRESULT hr = reinterpret_cast<IUIElementVisibilityABI*>(element)->put_Visibility(visibility);
    element->Release();
    return hr;
}

static int CollapseAllByName(IVisualTreeHelperStaticsABI* helper, IDependencyObjectABI* node,
                             const wchar_t* name, int depth)
{
    if (depth > 24 || !node)
        return 0;

    int collapsed = 0;

    wchar_t elementName[128] = {};
    GetElementName(node, elementName, 128);
    if (wcscmp(elementName, name) == 0) {
        HRESULT hr = SetVisibility(node, 1);
        Log(L"panel: hide -> 0x%08X", hr);
        if (SUCCEEDED(hr))
            ++collapsed;
    }

    UINT32 count = 0;
    if (FAILED(helper->GetChildrenCount(node, &count)))
        return collapsed;

    for (UINT32 i = 0; i < count; ++i) {
        IDependencyObjectABI* child = nullptr;
        if (FAILED(helper->GetChild(node, static_cast<INT32>(i), &child)) || !child)
            continue;
        collapsed += CollapseAllByName(helper, child, name, depth + 1);
        child->Release();
    }
    return collapsed;
}

static IDependencyObjectABI* FindByName(IVisualTreeHelperStaticsABI* helper, IDependencyObjectABI* node,
                                        const wchar_t* name, int depth)
{
    if (depth > 24 || !node)
        return nullptr;

    wchar_t elementName[128] = {};
    GetElementName(node, elementName, 128);
    if (wcscmp(elementName, name) == 0) {
        node->AddRef();
        return node;
    }

    UINT32 count = 0;
    if (FAILED(helper->GetChildrenCount(node, &count)))
        return nullptr;

    for (UINT32 i = 0; i < count; ++i) {
        IDependencyObjectABI* child = nullptr;
        if (FAILED(helper->GetChild(node, static_cast<INT32>(i), &child)) || !child)
            continue;
        IDependencyObjectABI* found = FindByName(helper, child, name, depth + 1);
        child->Release();
        if (found)
            return found;
    }
    return nullptr;
}

static void* g_pendingView = nullptr;
static void* g_views[8] = {};
static int g_viewCount = 0;
static void RememberView(void* view)
{
    if (!view)
        return;
    for (int i = 0; i < g_viewCount; ++i)
        if (g_views[i] == view)
            return;
    if (g_viewCount < 8)
        g_views[g_viewCount++] = view;
}
static int g_pendingAction = 0;  // 0 = inspect, 1 = collapse panel

static void DoDeferredInspect()
{
    if (g_pendingAction == 1) {
        // Every open starts collapsed. The shell rebuilds the flyout's content,
        // so the panel is looked up in the current tree rather than trusted from
        // a cached pointer - hiding a stale element silently does nothing.
        IVisualTreeHelperStaticsABI* finder = GetVisualTreeHelper();
        IDependencyObjectABI* viewObject = nullptr;
        IDependencyObjectABI* live = nullptr;

        if (finder && g_pendingView &&
            SUCCEEDED(reinterpret_cast<IInspectableABI*>(g_pendingView)->QueryInterface(
                kIDependencyObject, reinterpret_cast<void**>(&viewObject))) && viewObject) {
            live = FindByName(finder, viewObject, kPanelName, 0);
        }

        int collapsed = 0;
        for (int v = 0; v < g_viewCount; ++v) {
            IDependencyObjectABI* root = nullptr;
            if (SUCCEEDED(reinterpret_cast<IInspectableABI*>(g_views[v])->QueryInterface(
                    kIDependencyObject, reinterpret_cast<void**>(&root))) && root) {
                collapsed += CollapseAllByName(finder, root, kPanelName, 0);
                root->Release();
            }
        }

        if (collapsed > 0) {
            Log(L"panel: collapsed %d instance(s) on open", collapsed);
            g_panelVisible = false;
        } else if (g_monitorPanel) {
            Log(L"panel: gone from the tree - dropping the stale reference");
            g_monitorPanel->Release();
            g_monitorPanel = nullptr;
            g_panelVisible = false;
        } else {
            Log(L"panel: nothing to collapse");
        }

        if (viewObject)
            viewObject->Release();
        if (finder)
            finder->Release();
        return;
    }

    void* view = g_pendingView;
    Log(L"deferred callback: view=0x%p", view);
    if (view)
        InspectView(view);
    Log(L"deferred callback: done");
}

// A minimal IDispatcherQueueHandler: the three IUnknown slots, then Invoke.
struct QueueHandler : IUnknown
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == kIDispatcherQueueHandler || riid == IID_IUnknown || riid == kIAgileObject) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    virtual HRESULT STDMETHODCALLTYPE Invoke()
    {
        DoDeferredInspect();
        return S_OK;
    }
};

static QueueHandler g_handler;

static void QueueDeferredInspect(void* view)
{
    g_pendingView = view;

    const wchar_t* className = L"Windows.System.DispatcherQueue";
    HSTRING name = nullptr;
    if (FAILED(g_createString(className, static_cast<UINT32>(wcslen(className)), &name)) || !name) {
        Log(L"DispatcherQueue: HSTRING failed");
        return;
    }

    void* factory = nullptr;
    HRESULT hr = g_roGetActivationFactory(name, kIActivationFactory, &factory);
    g_deleteString(name);
    if (FAILED(hr) || !factory) {
        Log(L"DispatcherQueue factory: 0x%08X", hr);
        return;
    }

    auto inspectable = reinterpret_cast<IInspectableABI*>(factory);
    IDispatcherQueueStaticsABI* statics = nullptr;
    hr = inspectable->QueryInterface(kIDispatcherQueueStatics, reinterpret_cast<void**>(&statics));
    inspectable->Release();
    if (FAILED(hr) || !statics) {
        Log(L"QI IDispatcherQueueStatics: 0x%08X", hr);
        return;
    }

    IDispatcherQueueABI* queue = nullptr;
    hr = statics->GetForCurrentThread(&queue);
    statics->Release();
    if (FAILED(hr) || !queue) {
        Log(L"GetForCurrentThread: 0x%08X", hr);
        return;
    }

    BOOL queued = FALSE;
    hr = queue->TryEnqueue(&g_handler, &queued);
    Log(L"TryEnqueue: 0x%08X queued=%d", hr, queued);
    queue->Release();
}

// ---------------------------------------------------------------------------
// Per-monitor panel, opened from the chevron
// ---------------------------------------------------------------------------





struct PanelSliderHandler : IUnknown
{
    int monitor;  // 0 = the laptop panel, 1 = the external one

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == kIRangeBaseValueChangedHandler || riid == IID_IUnknown || riid == kIAgileObject) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectableABI* sender, void*)
    {
        if (g_suspendMirror)
            return S_OK;  // we are writing this value in, not reacting to the user

        IRangeBaseABI* range = nullptr;
        if (!sender || FAILED(sender->QueryInterface(kIRangeBase, reinterpret_cast<void**>(&range))) || !range)
            return S_OK;

        double value = 0;
        if (SUCCEEDED(range->get_Value(&value))) {
            if (monitor == 1) {
                QueueExternalBrightness(value);
            } else if (g_stockSlider) {
                // Driving the shell's own slider is how the laptop panel gets set,
                // and the suspend flag keeps it from being mirrored straight back.
                IRangeBaseABI* stock = nullptr;
                if (SUCCEEDED(reinterpret_cast<IInspectableABI*>(g_stockSlider)->QueryInterface(
                        kIRangeBase, reinterpret_cast<void**>(&stock))) && stock) {
                    g_suspendMirror = true;
                    stock->put_Value(value);
                    g_suspendMirror = false;
                    stock->Release();
                }
            }
        }
        range->Release();
        return S_OK;
    }
};

static PanelSliderHandler g_panelHandlers[2];

static IInspectableABI* MakeLabel(const wchar_t* text)
{
    IInspectableABI* block = Activate(L"Windows.UI.Xaml.Controls.TextBlock");
    if (!block)
        return nullptr;
    ITextBlockABI* textBlock = nullptr;
    if (SUCCEEDED(block->QueryInterface(kITextBlock, reinterpret_cast<void**>(&textBlock))) && textBlock) {
        HSTRING value = nullptr;
        g_createString(text, static_cast<UINT32>(wcslen(text)), &value);
        textBlock->put_Text(value);
        textBlock->put_FontSize(12);
        g_deleteString(value);

        IInspectableABI* brush = Activate(L"Windows.UI.Xaml.Media.SolidColorBrush");
        if (brush) {
            ISolidColorBrushABI* solid = nullptr;
            if (SUCCEEDED(brush->QueryInterface(kISolidColorBrush, reinterpret_cast<void**>(&solid))) && solid) {
                ColorABI white = { 255, 255, 255, 255 };
                solid->put_Color(white);
                textBlock->put_Foreground(brush);
                solid->Release();
            }
            brush->Release();
        }
        textBlock->Release();
    }
    return block;
}

static IInspectableABI* MakeSlider(double value, int monitor)
{
    IInspectableABI* slider = Activate(L"Windows.UI.Xaml.Controls.Slider");
    if (!slider)
        return nullptr;

    IFrameworkElementABI* element = nullptr;
    if (SUCCEEDED(slider->QueryInterface(kIFrameworkElement, reinterpret_cast<void**>(&element))) && element) {
        element->put_Width(300);
        element->put_Height(32);
        element->Release();
    }

    IRangeBaseABI* range = nullptr;
    if (SUCCEEDED(slider->QueryInterface(kIRangeBase, reinterpret_cast<void**>(&range))) && range) {
        range->put_Value(value);
        INT64 token = 0;
        HRESULT hr = range->add_ValueChanged(&g_panelHandlers[monitor], &token);
        Log(L"panel: slider %d add_ValueChanged 0x%08X", monitor, hr);
        range->Release();
    }
    return slider;
}

static void AppendChild(IInspectableABI* parent, IInspectableABI* child)
{
    IPanelABI* panel = nullptr;
    if (FAILED(parent->QueryInterface(kIPanel, reinterpret_cast<void**>(&panel))) || !panel)
        return;
    IInspectableABI* children = nullptr;
    if (SUCCEEDED(panel->get_Children(&children)) && children) {
        reinterpret_cast<IVectorABI*>(children)->Append(child);
        children->Release();
    }
    panel->Release();
}

// Grid.Row through the dependency property: the static setter faults.
static void SetGridRow(IInspectableABI* element, int row)
{
    if (!element)
        return;
    HSTRING gridName = nullptr;
    const wchar_t* gridClass = L"Windows.UI.Xaml.Controls.Grid";
    g_createString(gridClass, static_cast<UINT32>(wcslen(gridClass)), &gridName);
    void* gridFactory = nullptr;
    if (SUCCEEDED(g_roGetActivationFactory(gridName, kIActivationFactory, &gridFactory)) && gridFactory) {
        IGridStaticsABI* statics = nullptr;
        if (SUCCEEDED(reinterpret_cast<IInspectableABI*>(gridFactory)->QueryInterface(
                kIGridStatics, reinterpret_cast<void**>(&statics))) && statics) {
            IDependencyObjectABI* rowProperty = nullptr;
            if (SUCCEEDED(statics->get_RowProperty(&rowProperty)) && rowProperty) {
                IInspectableABI* object = nullptr;
                if (SUCCEEDED(element->QueryInterface(kIDependencyObject, reinterpret_cast<void**>(&object))) && object) {
                    HSTRING pvName = nullptr;
                    const wchar_t* pvClass = L"Windows.Foundation.PropertyValue";
                    g_createString(pvClass, static_cast<UINT32>(wcslen(pvClass)), &pvName);
                    void* pvFactory = nullptr;
                    if (SUCCEEDED(g_roGetActivationFactory(pvName, kIActivationFactory, &pvFactory)) && pvFactory) {
                        IPropertyValueStaticsABI* pv = nullptr;
                        if (SUCCEEDED(reinterpret_cast<IInspectableABI*>(pvFactory)->QueryInterface(
                                kIPropertyValueStatics, reinterpret_cast<void**>(&pv))) && pv) {
                            IInspectableABI* boxed = nullptr;
                            if (SUCCEEDED(pv->CreateInt32(row, &boxed)) && boxed) {
                                HRESULT hr = reinterpret_cast<IDependencyObjectSetValueABI*>(object)
                                                 ->SetValue(rowProperty, boxed);
                                Log(L"gridrow: SetValue(%d) 0x%08X", row, hr);
                                boxed->Release();
                            }
                            pv->Release();
                        }
                        reinterpret_cast<IInspectableABI*>(pvFactory)->Release();
                    }
                    g_deleteString(pvName);
                    object->Release();
                }
                rowProperty->Release();
            }
            statics->Release();
        }
        reinterpret_cast<IInspectableABI*>(gridFactory)->Release();
    }
    g_deleteString(gridName);
}

// Puts the panel in its own row of L1Grid, the same way the brightness row goes in.
static void AddPanelRow(IInspectableABI* content)
{
    if (!g_l1Grid)
        return;

    IGridABI* grid = nullptr;
    if (FAILED(reinterpret_cast<IInspectableABI*>(g_l1Grid)->QueryInterface(
            kIGrid, reinterpret_cast<void**>(&grid))) || !grid)
        return;

    IInspectableABI* rowsRaw = nullptr;
    if (SUCCEEDED(grid->get_RowDefinitions(&rowsRaw)) && rowsRaw) {
        IVectorABI* rows = reinterpret_cast<IVectorABI*>(rowsRaw);
        unsigned count = 0;
        rows->get_Size(&count);

        // Row 2 is the footer, so insert at 2 and push the footer down to 3.
        // Appending instead would put the panel below the battery row.
        const unsigned insertAt = 2;
        IInspectableABI* rowDefinition = Activate(L"Windows.UI.Xaml.Controls.RowDefinition");
        if (rowDefinition) {
            IRowDefinitionABI* row = nullptr;
            if (SUCCEEDED(rowDefinition->QueryInterface(kIRowDefinition, reinterpret_cast<void**>(&row))) && row) {
                GridLengthABI autoHeight = { 0.0, 0, 0 };
                row->put_Height(autoHeight);
                row->Release();
            }
            if (count > insertAt)
                rows->InsertAt(insertAt, rowDefinition);
            else
                rows->Append(rowDefinition);
            rowDefinition->Release();
        }
        rowsRaw->Release();
        count = insertAt;

        // Grid.Row via the property, same as the brightness row.
        HSTRING gridName = nullptr;
        const wchar_t* gridClass = L"Windows.UI.Xaml.Controls.Grid";
        g_createString(gridClass, static_cast<UINT32>(wcslen(gridClass)), &gridName);
        void* gridFactory = nullptr;
        if (SUCCEEDED(g_roGetActivationFactory(gridName, kIActivationFactory, &gridFactory)) && gridFactory) {
            IGridStaticsABI* statics = nullptr;
            if (SUCCEEDED(reinterpret_cast<IInspectableABI*>(gridFactory)->QueryInterface(
                    kIGridStatics, reinterpret_cast<void**>(&statics))) && statics) {
                IDependencyObjectABI* rowProperty = nullptr;
                if (SUCCEEDED(statics->get_RowProperty(&rowProperty)) && rowProperty) {
                    IInspectableABI* object = nullptr;
                    if (SUCCEEDED(content->QueryInterface(kIDependencyObject, reinterpret_cast<void**>(&object))) && object) {
                        HSTRING pvName = nullptr;
                        const wchar_t* pvClass = L"Windows.Foundation.PropertyValue";
                        g_createString(pvClass, static_cast<UINT32>(wcslen(pvClass)), &pvName);
                        void* pvFactory = nullptr;
                        if (SUCCEEDED(g_roGetActivationFactory(pvName, kIActivationFactory, &pvFactory)) && pvFactory) {
                            IPropertyValueStaticsABI* pv = nullptr;
                            if (SUCCEEDED(reinterpret_cast<IInspectableABI*>(pvFactory)->QueryInterface(
                                    kIPropertyValueStatics, reinterpret_cast<void**>(&pv))) && pv) {
                                IInspectableABI* boxed = nullptr;
                                if (SUCCEEDED(pv->CreateInt32(static_cast<int>(count), &boxed)) && boxed) {
                                    HRESULT hr = reinterpret_cast<IDependencyObjectSetValueABI*>(object)
                                                     ->SetValue(rowProperty, boxed);
                                    Log(L"panel: SetValue(Grid.Row, %u) 0x%08X", count, hr);
                                    boxed->Release();
                                }
                                pv->Release();
                            }
                            reinterpret_cast<IInspectableABI*>(pvFactory)->Release();
                        }
                        g_deleteString(pvName);
                        rowProperty->Release();
                        object->Release();
                    }
                }
                statics->Release();
            }
            reinterpret_cast<IInspectableABI*>(gridFactory)->Release();
        }
        g_deleteString(gridName);

        AppendChild(reinterpret_cast<IInspectableABI*>(g_l1Grid), content);
        Log(L"panel: added to L1Grid as row %u", count);

        // The footer used to be row 2; it now belongs one below the panel.
        IPanelABI* gridPanel = nullptr;
        if (SUCCEEDED(reinterpret_cast<IInspectableABI*>(g_l1Grid)->QueryInterface(
                kIPanel, reinterpret_cast<void**>(&gridPanel))) && gridPanel) {
            IInspectableABI* children = nullptr;
            if (SUCCEEDED(gridPanel->get_Children(&children)) && children) {
                IInspectableABI* footer = nullptr;
                if (SUCCEEDED(reinterpret_cast<IVectorABI*>(children)->GetAt(3, &footer)) && footer) {
                    SetGridRow(footer, static_cast<int>(count + 1));
                    footer->Release();
                }
                children->Release();
            }
            gridPanel->Release();
        }
    }
    grid->Release();
}

static void ShowMonitorPanel()
{
    if (g_monitorPanel) {
        g_panelVisible = !g_panelVisible;
        IDependencyObjectABI* object = nullptr;
        if (SUCCEEDED(g_monitorPanel->QueryInterface(kIDependencyObject, reinterpret_cast<void**>(&object))) && object) {
            HRESULT hr = SetVisibility(object, g_panelVisible ? 0 : 1);
            Log(L"panel: toggled visible=%d hr=0x%08X", g_panelVisible ? 1 : 0, hr);
            object->Release();
        }
        return;
    }

    IInspectableABI* panel = Activate(L"Windows.UI.Xaml.Controls.StackPanel");
    if (!panel) {
        Log(L"panel: could not create StackPanel");
        return;
    }
    // Named so a later open can find it in whatever tree the shell has built.
    SetElementName(panel, kPanelName);

    double internalLevel = 0, externalLevel = 0;
    if (g_stockSlider) {
        IRangeBaseABI* range = nullptr;
        if (SUCCEEDED(reinterpret_cast<IInspectableABI*>(g_stockSlider)->QueryInterface(
                kIRangeBase, reinterpret_cast<void**>(&range))) && range) {
            range->get_Value(&internalLevel);
            range->Release();
        }
    }
    externalLevel = internalLevel;  // they are mirrored until one is moved alone

    g_panelHandlers[0].monitor = 0;
    g_panelHandlers[1].monitor = 1;

    if (IInspectableABI* label = MakeLabel(L"This display")) {
        AppendChild(panel, label);
        label->Release();
    }
    if (IInspectableABI* slider = MakeSlider(internalLevel, 0)) {
        AppendChild(panel, slider);
        g_panelSliders[0] = slider;
        slider->AddRef();
        slider->Release();
    }
    if (IInspectableABI* label = MakeLabel(L"Dell P2423D")) {
        AppendChild(panel, label);
        label->Release();
    }
    if (IInspectableABI* slider = MakeSlider(externalLevel, 1)) {
        AppendChild(panel, slider);
        g_panelSliders[1] = slider;
        slider->AddRef();
        slider->Release();
    }

    AddPanelRow(panel);
    g_monitorPanel = panel;

    // Collapsed until the chevron is pressed, which is the whole point of
    // putting it behind a button.
    // Built hidden so the panel is not in the flyout until asked for, then
    // shown immediately: this call came from the chevron.
    IDependencyObjectABI* panelObject = nullptr;
    if (SUCCEEDED(panel->QueryInterface(kIDependencyObject, reinterpret_cast<void**>(&panelObject))) && panelObject) {
        HRESULT hrShow = SetVisibility(panelObject, 0);
        Log(L"panel: built (visible) hr=0x%08X", hrShow);
        panelObject->Release();
    }
    g_panelVisible = true;
}


static int Hook_OnGotFocus(void* self, void* args)
{
    // Nothing XAML-touching here: capture the view, post the work, get out.
    Log(L"hook entered: this=0x%p", self);
    RememberView(self);
    if (!g_monitorPanel) {
        static bool inspected = false;
        if (!inspected) {
            inspected = true;
            g_pendingAction = 0;
            QueueDeferredInspect(self);
        }
    } else {
        // The panel already exists, so this open only has to hide it again.
        g_pendingAction = 1;
        QueueDeferredInspect(self);
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

    InitSync();

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

    // 6, not 5: the prologue is `40 53` (push rbx, two bytes - 0x40 is a REX
    // prefix) followed by `48 83 EC 40` (sub rsp,0x40, four bytes). Stealing 5
    // splits the sub in half, the trampoline runs it with the wrong immediate,
    // and the stack comes back 8 bytes out - ShellHost then dies with
    // 0xc000001d. The E9 patch is still 5 bytes; the sixth is padded with NOP.
    void* trampoline = InstallDetour(target, 6, reinterpret_cast<const void*>(&Hook_OnGotFocus));
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

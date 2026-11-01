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
};

static const IID kIActivationFactory = { 0x00000035, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const IID kIVisualTreeHelperStatics = { 0xe75758c4, 0xd25d, 0x4b1d, { 0x97, 0x1f, 0x59, 0x6f, 0x17, 0xf1, 0x2b, 0xaa } };
static const IID kIFrameworkElement = { 0xa391d09b, 0x4a99, 0x4b7c, { 0x9d, 0x8d, 0x6f, 0xa5, 0xd0, 0x1f, 0x6f, 0xbf } };
static const IID kIDependencyObject = { 0x5c526665, 0xf60e, 0x4912, { 0xaf, 0x59, 0x5f, 0xe0, 0x68, 0x0f, 0x08, 0x9d } };


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

// Appends one real XAML element to the sliders list, to prove insertion works
// before building the actual brightness row.
static void InsertTestSlider()
{
    if (!g_itemsPanel) {
        Log(L"insert: no ItemsStackPanel captured");
        return;
    }

    IPanelABI* panel = nullptr;
    HRESULT hr = reinterpret_cast<IInspectableABI*>(g_itemsPanel)->QueryInterface(
        kIPanel, reinterpret_cast<void**>(&panel));
    Log(L"insert: QI IPanel 0x%08X", hr);
    if (FAILED(hr) || !panel)
        return;

    IInspectableABI* childrenRaw = nullptr;
    hr = panel->get_Children(&childrenRaw);
    Log(L"insert: get_Children 0x%08X", hr);
    if (FAILED(hr) || !childrenRaw) {
        panel->Release();
        return;
    }

    // get_Children hands back IVector<UIElement> already, so cast it rather than
    // QI for IInspectable - that would give a different vtable.
    IVectorABI* children = reinterpret_cast<IVectorABI*>(childrenRaw);

    unsigned before = 0;
    children->get_Size(&before);

    const wchar_t* className = L"Windows.UI.Xaml.Controls.Slider";
    HSTRING name = nullptr;
    g_createString(className, static_cast<UINT32>(wcslen(className)), &name);
    void* instance = nullptr;
    hr = g_roActivateInstance(name, &instance);
    g_deleteString(name);
    Log(L"insert: RoActivateInstance 0x%08X", hr);
    if (FAILED(hr) || !instance) {
        childrenRaw->Release();
        panel->Release();
        return;
    }

    hr = children->Append(reinterpret_cast<IInspectableABI*>(instance));
    Log(L"insert: Append 0x%08X", hr);

    unsigned after = 0;
    children->get_Size(&after);
    Log(L"insert: children %u -> %u", before, after);

    reinterpret_cast<IInspectableABI*>(instance)->Release();
    childrenRaw->Release();
    panel->Release();
}

// Depth-first search for Slider elements, logging the chain that leads to them.
static void SearchSliders(IVisualTreeHelperStaticsABI* helper, IDependencyObjectABI* node,
                          int depth, int* hits)
{
    if (depth > 22 || *hits > 24)
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

        if (wcsstr(className, L"ItemsStackPanel") && !g_itemsPanel) {
            child->AddRef();
            g_itemsPanel = child;
            Log(L"captured ItemsStackPanel: %.0fx%.0f", width, height);
        }

        if (wcscmp(elementName, L"SlidersGroup") == 0) {
            Log(L"FOUND SlidersGroup at depth %d: %s %.0fx%.0f", depth, className, width, height);
            Log(L"--- SlidersGroup subtree ---");
            WalkTree(helper, child, 0);
            Log(L"--- end subtree ---");
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

    InsertTestSlider();

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
static const IID kIAgileObject = { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };

static void* g_pendingView = nullptr;

static void DoDeferredInspect()
{
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

static int Hook_OnGotFocus(void* self, void* args)
{
    // Nothing XAML-touching here: capture the view, post the work, get out.
    static bool queued = false;
    if (!queued) {
        queued = true;
        Log(L"hook entered: this=0x%p", self);
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

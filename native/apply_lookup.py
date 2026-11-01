p = "src/bcdll.cpp"
s = open(p, encoding="utf-8").read()

helper = '''// The shell rebuilds the flyout's content, so a cached panel pointer goes stale
// and hiding it silently does nothing. Everything therefore goes through the
// current tree, finding the panel by name.
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

'''
s = s.replace("// Test hook: while this file exists", helper + "// Test hook: while this file exists", 1)

# collapse: look the panel up in the live tree instead of trusting the cache
s = s.replace('''        if (g_monitorPanel) {
            HRESULT hrVis = reinterpret_cast<IUIElementVisibilityABI*>(g_monitorPanel)->put_Visibility(1);
            g_panelVisible = false;''',
'''        IVisualTreeHelperStaticsABI* finder = GetVisualTreeHelper();
        IDependencyObjectABI* viewObject = nullptr;
        IDependencyObjectABI* live = nullptr;
        if (finder && g_pendingView &&
            SUCCEEDED(reinterpret_cast<IInspectableABI*>(g_pendingView)->QueryInterface(
                kIDependencyObject, reinterpret_cast<void**>(&viewObject))) && viewObject) {
            live = FindByName(finder, viewObject, kPanelName, 0);
        }

        if (live) {
            HRESULT hrVis = reinterpret_cast<IUIElementVisibilityABI*>(live)->put_Visibility(1);
            g_panelVisible = false;
            live->Release();
            Log(L"panel: collapsed on open hr=0x%08X", hrVis);
        } else if (g_monitorPanel) {
            // Nothing in this tree, so the shell rebuilt the content and our
            // panel went with it; the cached pointer is stale from here on.
            Log(L"panel: not in the live tree - dropping the stale reference");
            g_monitorPanel->Release();
            g_monitorPanel = nullptr;
            g_panelVisible = false;
        }
        if (viewObject)
            viewObject->Release();
        if (finder)
            finder->Release();

        if (false) {
            HRESULT hrVis = 0;
            g_panelVisible = false;''')

open(p, "w", encoding="utf-8").write(s)
print("tree lookup wired")

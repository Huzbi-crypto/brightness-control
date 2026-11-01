p = "src/bcdll.cpp"
s = open(p, encoding="utf-8").read()

# collapse every match, not just the first: the shell can hold more than one
# copy of the view, and each copy carries its own panel
s = s.replace('''static IDependencyObjectABI* FindByName(''',
'''static int CollapseAllByName(IVisualTreeHelperStaticsABI* helper, IDependencyObjectABI* node,
                             const wchar_t* name, int depth)
{
    if (depth > 24 || !node)
        return 0;

    int collapsed = 0;

    wchar_t elementName[128] = {};
    GetElementName(node, elementName, 128);
    if (wcscmp(elementName, name) == 0) {
        reinterpret_cast<IUIElementVisibilityABI*>(node)->put_Visibility(1);
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

static IDependencyObjectABI* FindByName(''')

# use it on every open, walking from the view that just took focus
s = s.replace('''        if (live) {
            HRESULT hrVis = reinterpret_cast<IUIElementVisibilityABI*>(live)->put_Visibility(1);
            Log(L"panel: collapsed on open hr=0x%08X", hrVis);
            live->Release();
            g_panelVisible = false;
        } else if (g_monitorPanel) {''',
'''        int collapsed = 0;
        if (viewObject)
            collapsed = CollapseAllByName(finder, viewObject, kPanelName, 0);

        if (collapsed > 0) {
            Log(L"panel: collapsed %d instance(s) on open", collapsed);
            g_panelVisible = false;
        } else if (g_monitorPanel) {''')

open(p, "w", encoding="utf-8").write(s)
print("collapse all instances")

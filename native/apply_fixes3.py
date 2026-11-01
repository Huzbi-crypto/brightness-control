p = "src/bcdll.cpp"
s = open(p, encoding="utf-8").read()

# a reusable Grid.Row setter, so the panel and the footer share one path
helper = '''// Grid.Row through the dependency property: the static setter faults.
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

'''
s = s.replace("// Puts the panel in its own row of L1Grid", helper + "// Puts the panel in its own row of L1Grid", 1)

# after the panel is appended, move the footer into the row we made room in
s = s.replace('''        AppendChild(reinterpret_cast<IInspectableABI*>(g_l1Grid), content);
        Log(L"panel: added to L1Grid as row %u", count);''',
'''        AppendChild(reinterpret_cast<IInspectableABI*>(g_l1Grid), content);
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
        }''')

open(p, "w", encoding="utf-8").write(s)
print("footer shift via helper")

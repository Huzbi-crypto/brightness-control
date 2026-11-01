p = "src/bcdll.cpp"
s = open(p, encoding="utf-8").read()

# log the row's column count and place the button in the last column
s = s.replace('''    IButtonBaseABI* buttonBase = nullptr;''',
'''    // The row is a Grid of icon | slider, so a child appended without a column
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

    IButtonBaseABI* buttonBase = nullptr;''')

open(p, "w", encoding="utf-8").write(s)
print("column placement")

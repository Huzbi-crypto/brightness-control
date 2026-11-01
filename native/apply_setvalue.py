p = "src/bcdll.cpp"
s = open(p, encoding="utf-8").read()

# IGridStatics needs get_RowProperty at slot 6
s = s.replace('''struct IGridStaticsABI : IInspectableABI
{
    virtual void* unused0();
    virtual void* unused1();
    virtual HRESULT STDMETHODCALLTYPE SetRow(IInspectableABI* element, int value) = 0;
};''',
'''struct IGridStaticsABI : IInspectableABI
{
    virtual HRESULT STDMETHODCALLTYPE get_RowProperty(IDependencyObjectABI** value) = 0;
    virtual void* unused1();                                       // slot 7 GetRow
    virtual HRESULT STDMETHODCALLTYPE SetRow(IInspectableABI* element, int value) = 0;
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

static const IID kIPropertyValueStatics = { 0x629bdbc8, 0xd932, 0x4ff4, { 0x96, 0xb9, 0x8d, 0x96, 0xc5, 0xc1, 0xe8, 0x58 } };''')

# replace the failing SetRow call with the dependency-property route
start = s.index("    // SetRow is called after the append")
end = s.index("    panel->Release();", start)

new = '''    // Grid.SetRow faults when called through the static interface, so set the
    // same attached property directly: Grid.RowProperty + a boxed Int32.
    HSTRING gridName = nullptr;
    const wchar_t* gridClass = L"Windows.UI.Xaml.Controls.Grid";
    g_createString(gridClass, static_cast<UINT32>(wcslen(gridClass)), &gridName);
    void* gridFactory = nullptr;
    HRESULT hrFactory = g_roGetActivationFactory(gridName, kIActivationFactory, &gridFactory);
    g_deleteString(gridName);

    if (SUCCEEDED(hrFactory) && gridFactory) {
        IGridStaticsABI* statics = nullptr;
        HRESULT hrStatics = reinterpret_cast<IInspectableABI*>(gridFactory)->QueryInterface(
            kIGridStatics, reinterpret_cast<void**>(&statics));
        if (SUCCEEDED(hrStatics) && statics) {
            IDependencyObjectABI* rowProperty = nullptr;
            HRESULT hrProp = statics->get_RowProperty(&rowProperty);
            Log(L"insert: get_RowProperty 0x%08X", hrProp);

            HSTRING pvName = nullptr;
            const wchar_t* pvClass = L"Windows.Foundation.PropertyValue";
            g_createString(pvClass, static_cast<UINT32>(wcslen(pvClass)), &pvName);
            void* pvFactory = nullptr;
            HRESULT hrPv = g_roGetActivationFactory(pvName, kIActivationFactory, &pvFactory);
            g_deleteString(pvName);

            if (SUCCEEDED(hrPv) && pvFactory) {
                IPropertyValueStaticsABI* pv = nullptr;
                HRESULT hrPvQI = reinterpret_cast<IInspectableABI*>(pvFactory)->QueryInterface(
                    kIPropertyValueStatics, reinterpret_cast<void**>(&pv));
                if (SUCCEEDED(hrPvQI) && pv) {
                    IInspectableABI* boxed = nullptr;
                    HRESULT hrBox = pv->CreateInt32(static_cast<int>(newRow), &boxed);
                    Log(L"insert: CreateInt32(%u) 0x%08X", newRow, hrBox);
                    if (SUCCEEDED(hrBox) && boxed) {
                        HRESULT hrSet = reinterpret_cast<IDependencyObjectSetValueABI*>(panel)
                                            ->SetValue(rowProperty, boxed);
                        Log(L"insert: SetValue(Grid.Row, %u) 0x%08X", newRow, hrSet);
                        boxed->Release();
                    }
                    pv->Release();
                }
                reinterpret_cast<IInspectableABI*>(pvFactory)->Release();
            }
            if (rowProperty)
                rowProperty->Release();
            statics->Release();
        }
        reinterpret_cast<IInspectableABI*>(gridFactory)->Release();
    }

'''
s = s[:start] + new + s[end:]
open(p, "w", encoding="utf-8").write(s)
print("SetValue route wired")

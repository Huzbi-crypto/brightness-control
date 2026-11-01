import re

p = "src/bcdll.cpp"
s = open(p, encoding="utf-8").read()

# --- new ABI pieces ---
anchor = "static const IID kIAgileObject"
abi = '''// fd104460-2e15-4ba3-8b8f-fa693a4161e9 - only the two definition vectors.
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
    virtual void* unused0();
    virtual void* unused1();
    virtual HRESULT STDMETHODCALLTYPE SetRow(IInspectableABI* element, int value) = 0;
};

static const IID kIGrid = { 0xfd104460, 0x2e15, 0x4ba3, { 0x8b, 0x8f, 0xfa, 0x69, 0x3a, 0x41, 0x61, 0xe9 } };
static const IID kIRowDefinition = { 0x4abae829, 0xd80c, 0x4a5e, { 0xa4, 0x8c, 0xf8, 0xb3, 0xd3, 0xb6, 0x53, 0x3d } };
static const IID kIGridStatics = { 0x64fe2e9f, 0xf951, 0x42b6, { 0xa9, 0xce, 0xbb, 0x17, 0x9a, 0xf1, 0x15, 0x95 } };

'''
s = s.replace(anchor, abi + anchor, 1)

s = s.replace("static IDependencyObjectABI* g_slidersGroup = nullptr;",
              "static IDependencyObjectABI* g_slidersGroup = nullptr;\n"
              "static IDependencyObjectABI* g_l1Grid = nullptr;")

s = s.replace('        if (wcsstr(className, L"ItemsStackPanel") && !g_itemsPanel) {',
              '        if (wcscmp(elementName, L"L1Grid") == 0 && !g_l1Grid) {\n'
              '            child->AddRef();\n'
              '            g_l1Grid = child;\n'
              '            Log(L"captured L1Grid: %s %.0fx%.0f", className, width, height);\n'
              '        }\n\n'
              '        if (wcsstr(className, L"ItemsStackPanel") && !g_itemsPanel) {')

start = s.index("// Wraps the shell's GridView in a StackPanel")
end = s.index("// Depth-first search for Slider elements", start)

new = open("insert_block.txt", encoding="utf-8").read()

s = s[:start] + new + s[end:]
s = s.replace("    InsertRowWrapped();", "    InsertPanelIntoGrid();")
open(p, "w", encoding="utf-8").write(s)
print("L1Grid insertion wired")

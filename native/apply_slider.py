p = "src/bcdll.cpp"
s = open(p, encoding="utf-8").read()

block = open("slider_block.txt", encoding="utf-8").read()

# kIAgileObject is declared in the dispatcher section, which comes later; hoist it.
agile_line = ('// TryEnqueue refuses a handler that is not agile: 0x800001C, "The object must\n'
              '// support the IAgileObject interface".\n')
import re
m = re.search(r'static const IID kIAgileObject = \{[^}]*\};\n', s)
if m:
    agile_def = m.group(0)
    s = s.replace(agile_def, "")
    s = s.replace("static const IID kIGrid = {", agile_def + "static const IID kIGrid = {", 1)

# insert the DDC + slider block before GetVisualTreeHelper
anchor = "static IVisualTreeHelperStaticsABI* GetVisualTreeHelper()"
s = s.replace(anchor, block + anchor, 1)

# capture the first Slider in the sliders subtree as the stock brightness slider
s = s.replace('''        if (wcsstr(className, L"Slider") || wcsstr(className, L"Brightness")) {''',
'''        if (wcsstr(className, L"Slider") && !g_stockSlider) {
            child->AddRef();
            g_stockSlider = child;
            Log(L"captured stock slider: %s %.0fx%.0f", className, width, height);
        }

        if (wcsstr(className, L"Slider") || wcsstr(className, L"Brightness")) {''')

# attach after inserting the row
s = s.replace("    InsertPanelIntoGrid();", "    InsertPanelIntoGrid();\n    AttachStockSlider();")

# start the DDC worker in InitThread
s = s.replace("    ResolveHStringHelpers();",
'''    ResolveHStringHelpers();

    g_ddcEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_ddcEvent) {
        HANDLE thread = CreateThread(nullptr, 0, DdcWorker, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }''')

open(p, "w", encoding="utf-8").write(s)
print("slider sync wired")

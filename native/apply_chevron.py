p = "src/bcdll.cpp"
s = open(p, encoding="utf-8").read()

block = open("chevron_block.txt", encoding="utf-8").read()

# Activate() and DescribeNode() must exist before this block, so park it right
# after InsertPanelIntoGrid's helper definitions.
anchor = "// Adds a row to L1Grid and puts the panel in it"
s = s.replace(anchor, block + anchor, 1)

# stub for now - the panel comes next
s = s.replace("static void ShowMonitorPanel();  // built later",
              "static void ShowMonitorPanel();  // built later")

s = s.replace('''    QueueExternalBrightness(value);''',
'''    QueueExternalBrightness(value);
    BuildChevron();''')

# provide the stub implementation before Hook_OnGotFocus
s = s.replace("static int Hook_OnGotFocus(void* self, void* args)",
'''static void ShowMonitorPanel()
{
    Log(L"panel: not built yet");
}

static int Hook_OnGotFocus(void* self, void* args)''')

open(p, "w", encoding="utf-8").write(s)
print("chevron wired")

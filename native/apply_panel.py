p = "src/bcdll.cpp"
s = open(p, encoding="utf-8").read()

block = open("panel_block.txt", encoding="utf-8").read()

# g_suspendMirror is read by the mirror handler, which is defined earlier
block = block.replace('''// Set while the panel drives one panel, so the mirror path does not fight it.
static bool g_suspendMirror = false;
''', "")

s = s.replace("static OnGotFocusFn g_onGotFocus = nullptr;",
              "static OnGotFocusFn g_onGotFocus = nullptr;\n\n"
              "// Set while the per-monitor panel drives a single panel, so the mirror\n"
              "// path does not immediately copy it to the other one.\n"
              "static bool g_suspendMirror = false;")

# the mirror handler must respect the flag
s = s.replace('''            if (SUCCEEDED(range->get_Value(&value)))
                QueueExternalBrightness(value);''',
'''            if (SUCCEEDED(range->get_Value(&value)) && !g_suspendMirror)
                QueueExternalBrightness(value);''')

# swap the stub for the real panel
s = s.replace('''static void ShowMonitorPanel()
{
    Log(L"panel: not built yet");
}''', block)

open(p, "w", encoding="utf-8").write(s)
print("panel wired")

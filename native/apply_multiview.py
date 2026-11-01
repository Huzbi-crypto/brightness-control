p = "src/bcdll.cpp"
s = open(p, encoding="utf-8").read()

# remember every view that has taken focus: the shell keeps more than one, and a
# panel can live in a view that is not the one firing OnGotFocus
s = s.replace("static void* g_pendingView = nullptr;",
              "static void* g_pendingView = nullptr;\n"
              "static void* g_views[8] = {};\n"
              "static int g_viewCount = 0;\n"
              "static void RememberView(void* view)\n"
              "{\n"
              "    if (!view)\n"
              "        return;\n"
              "    for (int i = 0; i < g_viewCount; ++i)\n"
              "        if (g_views[i] == view)\n"
              "            return;\n"
              "    if (g_viewCount < 8)\n"
              "        g_views[g_viewCount++] = view;\n"
              "}")

# collapse in every known view, not just the firing one
s = s.replace('''        int collapsed = 0;
        if (viewObject)
            collapsed = CollapseAllByName(finder, viewObject, kPanelName, 0);''',
'''        int collapsed = 0;
        for (int v = 0; v < g_viewCount; ++v) {
            IDependencyObjectABI* root = nullptr;
            if (SUCCEEDED(reinterpret_cast<IInspectableABI*>(g_views[v])->QueryInterface(
                    kIDependencyObject, reinterpret_cast<void**>(&root))) && root) {
                collapsed += CollapseAllByName(finder, root, kPanelName, 0);
                root->Release();
            }
        }''')

# record the view on every focus event
s = s.replace('''    // Nothing XAML-touching here: capture the view, post the work, get out.
    Log(L"hook entered: this=0x%p", self);''',
'''    // Nothing XAML-touching here: capture the view, post the work, get out.
    Log(L"hook entered: this=0x%p", self);
    RememberView(self);''')

open(p, "w", encoding="utf-8").write(s)
print("multi-view collapse")

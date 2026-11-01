p = "src/bcdll.cpp"
s = open(p, encoding="utf-8").read()

# a deferred action: 0 = inspect/build, 1 = collapse the panel
s = s.replace("static void* g_pendingView = nullptr;",
              "static void* g_pendingView = nullptr;\n"
              "static int g_pendingAction = 0;  // 0 = inspect, 1 = collapse panel")

s = s.replace('''static void DoDeferredInspect()
{
    void* view = g_pendingView;
    Log(L"deferred callback: view=0x%p", view);
    if (view)
        InspectView(view);
    Log(L"deferred callback: done");
}''',
'''static void DoDeferredInspect()
{
    if (g_pendingAction == 1) {
        // Every time the flyout opens the panel starts collapsed. Without this
        // it stays visible for the life of the process after the first chevron
        // press, which is exactly how it looked on reopen.
        if (g_monitorPanel) {
            reinterpret_cast<IUIElementVisibilityABI*>(g_monitorPanel)->put_Visibility(1);
            g_panelVisible = false;
            Log(L"panel: collapsed on open");
        }
        return;
    }

    void* view = g_pendingView;
    Log(L"deferred callback: view=0x%p", view);
    if (view)
        InspectView(view);
    Log(L"deferred callback: done");
}''')

# the hook now enqueues on every fire, not just the first
s = s.replace('''    // Nothing XAML-touching here: capture the view, post the work, get out.
    static bool queued = false;
    if (!queued) {
        queued = true;
        Log(L"hook entered: this=0x%p", self);
        QueueDeferredInspect(self);
    }
    return g_onGotFocus(self, args);''',
'''    // Nothing XAML-touching here: capture the view, post the work, get out.
    Log(L"hook entered: this=0x%p", self);
    if (!g_monitorPanel) {
        static bool inspected = false;
        if (!inspected) {
            inspected = true;
            g_pendingAction = 0;
            QueueDeferredInspect(self);
        }
    } else {
        // The panel already exists, so this open only has to hide it again.
        g_pendingAction = 1;
        QueueDeferredInspect(self);
    }
    return g_onGotFocus(self, args);''')

open(p, "w", encoding="utf-8").write(s)
print("collapse on every open")

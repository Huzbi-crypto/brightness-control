p = "src/bcdll.cpp"
s = open(p, encoding="utf-8").read()

# --- 1. panel starts hidden, and sits right under the sliders row ---

# track the panel sliders so the stock slider can push its value into them
s = s.replace("static IInspectableABI* g_monitorPanel = nullptr;",
              "static IInspectableABI* g_monitorPanel = nullptr;\n"
              "static IInspectableABI* g_panelSliders[2] = { nullptr, nullptr };")

# panel slider handlers must not react while we are syncing them
s = s.replace('''    virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectableABI* sender, void*)
    {
        IRangeBaseABI* range = nullptr;
        if (!sender || FAILED(sender->QueryInterface(kIRangeBase, reinterpret_cast<void**>(&range))) || !range)
            return S_OK;''',
'''    virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectableABI* sender, void*)
    {
        if (g_suspendMirror)
            return S_OK;  // we are writing this value in, not reacting to the user

        IRangeBaseABI* range = nullptr;
        if (!sender || FAILED(sender->QueryInterface(kIRangeBase, reinterpret_cast<void**>(&range))) || !range)
            return S_OK;''')

# keep the panel sliders in step with the shell slider
s = s.replace('''            if (SUCCEEDED(range->get_Value(&value)) && !g_suspendMirror)
                QueueExternalBrightness(value);''',
'''            if (SUCCEEDED(range->get_Value(&value)) && !g_suspendMirror) {
                QueueExternalBrightness(value);

                // The stock slider moves both panels, so the per-monitor sliders
                // have to show the new level instead of drifting out of date.
                g_suspendMirror = true;
                for (int i = 0; i < 2; ++i) {
                    IRangeBaseABI* panelRange = nullptr;
                    if (g_panelSliders[i] &&
                        SUCCEEDED(g_panelSliders[i]->QueryInterface(kIRangeBase, reinterpret_cast<void**>(&panelRange))) &&
                        panelRange) {
                        panelRange->put_Value(value);
                        panelRange->Release();
                    }
                }
                g_suspendMirror = false;
            }''')

# remember the sliders when the panel is built
s = s.replace('''    if (IInspectableABI* slider = MakeSlider(internalLevel, 0)) {
        AppendChild(panel, slider);
        slider->Release();
    }''',
'''    if (IInspectableABI* slider = MakeSlider(internalLevel, 0)) {
        AppendChild(panel, slider);
        g_panelSliders[0] = slider;
        slider->AddRef();
        slider->Release();
    }''')
s = s.replace('''    if (IInspectableABI* slider = MakeSlider(externalLevel, 1)) {
        AppendChild(panel, slider);
        slider->Release();
    }''',
'''    if (IInspectableABI* slider = MakeSlider(externalLevel, 1)) {
        AppendChild(panel, slider);
        g_panelSliders[1] = slider;
        slider->AddRef();
        slider->Release();
    }''')

# start collapsed, so opening Quick Settings does not show it until asked
s = s.replace('''    AddPanelRow(panel);
    g_monitorPanel = panel;
    g_panelVisible = true;
    Log(L"panel: built");''',
'''    AddPanelRow(panel);
    g_monitorPanel = panel;

    // Collapsed until the chevron is pressed, which is the whole point of
    // putting it behind a button.
    reinterpret_cast<IUIElementVisibilityABI*>(panel)->put_Visibility(1);
    g_panelVisible = false;
    Log(L"panel: built (collapsed)");''')

open(p, "w", encoding="utf-8").write(s)
print("panel behaviour + sync")

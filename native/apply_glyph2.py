p = "src/bcdll.cpp"
s = open(p, encoding="utf-8").read()

start = s.index("    // A glyph, because an empty Button has no visual content.")
end = s.index("    IInspectableABI* button = Activate(L\"Windows.UI.Xaml.Controls.Button\");", start)

new = '''    // A display glyph next to a chevron, the way the volume row pairs its
    // speaker icon with one, instead of a lone arrow in a box.
    IInspectableABI* text = Activate(L"Windows.UI.Xaml.Controls.StackPanel");
    if (text) {
        IStackPanelABI* stack = nullptr;
        if (SUCCEEDED(text->QueryInterface(kIStackPanel, reinterpret_cast<void**>(&stack))) && stack) {
            stack->put_Orientation(1);  // Horizontal
            stack->Release();
        }
        if (IInspectableABI* monitor = MakeGlyph(L"\\xE7F4", 14, 255)) {
            AppendChild(text, monitor);
            monitor->Release();
        }
        if (IInspectableABI* chevron = MakeGlyph(L"\\xE76C", 10, 200)) {
            AppendChild(text, chevron);
            chevron->Release();
        }
        Log(L"chevron: icon built");
    }

'''
s = s[:start] + new + s[end:]

# make the button itself transparent so only the glyphs show
s = s.replace('''    if (SUCCEEDED(button->QueryInterface(kIFrameworkElement, reinterpret_cast<void**>(&element))) && element) {''',
'''    IInspectableABI* transparent = Activate(L"Windows.UI.Xaml.Media.SolidColorBrush");
    if (transparent) {
        ISolidColorBrushABI* solid = nullptr;
        if (SUCCEEDED(transparent->QueryInterface(kISolidColorBrush, reinterpret_cast<void**>(&solid))) && solid) {
            ColorABI clear = { 0, 0, 0, 0 };
            solid->put_Color(clear);
            solid->Release();
        }
        IControlABI* control = nullptr;
        if (SUCCEEDED(button->QueryInterface(kIControl, reinterpret_cast<void**>(&control))) && control) {
            control->put_Background(transparent);
            control->put_BorderBrush(transparent);
            control->Release();
        }
        transparent->Release();
    }

    if (SUCCEEDED(button->QueryInterface(kIFrameworkElement, reinterpret_cast<void**>(&element))) && element) {''')

open(p, "w", encoding="utf-8").write(s)
print("composite icon + transparent button")

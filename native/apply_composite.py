p = "src/bcdll.cpp"
s = open(p, encoding="utf-8").read()

# reusable FontIcon builder, mirroring the reference mod's MakeContrastIcon
s = s.replace('''static void BuildChevron()''',
'''// A FontIcon the way the reference mod builds them: the glyph, the icon font,
// and a size, with no TextBlock in between.
static IInspectableABI* MakeFontIcon(const wchar_t* glyph, double size, BYTE alpha)
{
    IInspectableABI* icon = Activate(L"Windows.UI.Xaml.Controls.FontIcon");
    if (!icon)
        return nullptr;

    IFontIconABI* fontIcon = nullptr;
    if (FAILED(icon->QueryInterface(kIFontIcon, reinterpret_cast<void**>(&fontIcon))) || !fontIcon) {
        icon->Release();
        return nullptr;
    }

    HSTRING value = nullptr;
    g_createString(glyph, 1, &value);
    fontIcon->put_Glyph(value);
    g_deleteString(value);
    fontIcon->put_FontSize(size);

    IInspectableABI* family = MakeFontFamily(L"Segoe Fluent Icons");
    if (family) {
        fontIcon->put_FontFamily(family);
        family->Release();
    }

    IInspectableABI* brush = Activate(L"Windows.UI.Xaml.Media.SolidColorBrush");
    if (brush) {
        ISolidColorBrushABI* solid = nullptr;
        if (SUCCEEDED(brush->QueryInterface(kISolidColorBrush, reinterpret_cast<void**>(&solid))) && solid) {
            ColorABI colour = { alpha, 255, 255, 255 };
            solid->put_Color(colour);
            solid->Release();
        }
        IIconElementABI* iconElement = nullptr;
        if (SUCCEEDED(icon->QueryInterface(kIIconElement, reinterpret_cast<void**>(&iconElement))) && iconElement) {
            iconElement->put_Foreground(brush);
            iconElement->Release();
        }
        brush->Release();
    }

    IFrameworkElementABI* element = nullptr;
    if (SUCCEEDED(icon->QueryInterface(kIFrameworkElement, reinterpret_cast<void**>(&element))) && element) {
        element->put_Width(size);
        element->put_Height(size);
        element->Release();
    }

    fontIcon->Release();
    return icon;
}

static void BuildChevron()''')

# composite content: brightness glyph + chevron, like the volume row's pairing
start = s.index("    // A FontIcon rather than a TextBlock: it is the element XAML uses for glyphs")
end = s.index("    IInspectableABI* button = Activate(L\"Windows.UI.Xaml.Controls.Button\");", start)

new = '''    // Two glyphs, the way the volume row pairs its speaker icon with a chevron:
    // the brightness mark then a chevron, in a transparent container so only the
    // marks show and it does not read as a lone arrow in a box.
    IInspectableABI* text = Activate(L"Windows.UI.Xaml.Controls.StackPanel");
    if (text) {
        IStackPanelABI* stack = nullptr;
        if (SUCCEEDED(text->QueryInterface(kIStackPanel, reinterpret_cast<void**>(&stack))) && stack) {
            stack->put_Orientation(1);  // Horizontal
            stack->Release();
        }
        if (IInspectableABI* brightness = MakeFontIcon(L"\\xE706", 16, 255)) {
            AppendChild(text, brightness);
            brightness->Release();
        }
        if (IInspectableABI* chevron = MakeFontIcon(L"\\xE76C", 10, 200)) {
            AppendChild(text, chevron);
            chevron->Release();
        }
        Log(L"chevron: composite icon built");
    }

'''
s = s[:start] + new + s[end:]

# transparent button chrome, through IControl
s = s.replace('''    if (SUCCEEDED(button->QueryInterface(kIFrameworkElement, reinterpret_cast<void**>(&element))) && element) {''',
'''    IInspectableABI* clearBrush = Activate(L"Windows.UI.Xaml.Media.SolidColorBrush");
    if (clearBrush) {
        ISolidColorBrushABI* solid = nullptr;
        if (SUCCEEDED(clearBrush->QueryInterface(kISolidColorBrush, reinterpret_cast<void**>(&solid))) && solid) {
            ColorABI clear = { 0, 0, 0, 0 };
            solid->put_Color(clear);
            solid->Release();
        }
        IControlABI* control = nullptr;
        if (SUCCEEDED(button->QueryInterface(kIControl, reinterpret_cast<void**>(&control))) && control) {
            control->put_Background(clearBrush);
            control->put_BorderBrush(clearBrush);
            control->Release();
        }
        clearBrush->Release();
    }

    if (SUCCEEDED(button->QueryInterface(kIFrameworkElement, reinterpret_cast<void**>(&element))) && element) {''')

open(p, "w", encoding="utf-8").write(s)
print("composite FontIcon content")

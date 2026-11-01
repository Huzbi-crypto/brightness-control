p = "src/bcdll.cpp"
s = open(p, encoding="utf-8").read()

# 1. drop the composite icon block, restore the inline glyph that worked
start = s.index("    // A display glyph next to a chevron")
end = s.index("    IInspectableABI* button = Activate(L\"Windows.UI.Xaml.Controls.Button\");", start)
restored = '''    // A glyph, because an empty Button has no visual content.
    IInspectableABI* text = Activate(L"Windows.UI.Xaml.Controls.TextBlock");
    if (text) {
        ITextBlockABI* textBlock = nullptr;
        if (SUCCEEDED(text->QueryInterface(kITextBlock, reinterpret_cast<void**>(&textBlock))) && textBlock) {
            HSTRING glyph = nullptr;
            const wchar_t* chevron = L"\\xE76C";  // Segoe Fluent Icons: ChevronRight
            g_createString(chevron, 1, &glyph);
            HRESULT hrText = textBlock->put_Text(glyph);
            Log(L"chevron: put_Text 0x%08X", hrText);
            g_deleteString(glyph);
            textBlock->put_FontSize(14);

            IInspectableABI* fontFamily = MakeFontFamily(L"Segoe Fluent Icons");
            if (fontFamily) {
                HRESULT hrFont = textBlock->put_FontFamily(fontFamily);
                Log(L"chevron: put_FontFamily 0x%08X", hrFont);
                fontFamily->Release();
            }

            IInspectableABI* brush = Activate(L"Windows.UI.Xaml.Media.SolidColorBrush");
            if (brush) {
                ISolidColorBrushABI* solid = nullptr;
                if (SUCCEEDED(brush->QueryInterface(kISolidColorBrush, reinterpret_cast<void**>(&solid))) && solid) {
                    ColorABI white = { 255, 255, 255, 255 };
                    HRESULT hrColor = solid->put_Color(white);
                    HRESULT hrFg = textBlock->put_Foreground(brush);
                    Log(L"chevron: put_Color 0x%08X put_Foreground 0x%08X", hrColor, hrFg);
                    solid->Release();
                }
                brush->Release();
            }
            textBlock->Release();
        }
    }

'''
s = s[:start] + restored + s[end:]

# 2. remove the now-unused MakeGlyph
mstart = s.index("// A glyph in the shell's icon font, sized and coloured for the panel.")
mend = s.index("static void BuildChevron()\n{", mstart)
s = s[:mstart] + s[mend:]

# 3. drop the transparent-background block (it followed the composite icon)
tstart = s.index("    IInspectableABI* transparent = Activate(L\"Windows.UI.Xaml.Media.SolidColorBrush\");")
tend = s.index("    if (SUCCEEDED(button->QueryInterface(kIFrameworkElement, reinterpret_cast<void**>(&element))) && element) {", tstart)
s = s[:tstart] + s[tend:]

open(p, "w", encoding="utf-8").write(s)
print("reverted to the working glyph")

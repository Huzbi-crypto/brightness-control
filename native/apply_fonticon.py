p = "src/bcdll.cpp"
s = open(p, encoding="utf-8").read()

# ABI for FontIcon, which is what the reference mod uses for its glyphs
s = s.replace('''// IControl: put_Background is method 31 (slot 37)''',
'''// d4d61391-39ac-4950-9166-3606c264418b - put_Glyph 7, put_FontSize 9, put_FontFamily 11.
struct IFontIconABI : IInspectableABI
{
    virtual void* i0();                                          // slot 6  get_Glyph
    virtual HRESULT STDMETHODCALLTYPE put_Glyph(HSTRING value) = 0;      // slot 7
    virtual void* i2();                                          // slot 8  get_FontFamily
    virtual HRESULT STDMETHODCALLTYPE put_FontSize(double value) = 0;    // slot 9
    virtual void* i4();                                          // slot 10
    virtual HRESULT STDMETHODCALLTYPE put_FontFamily(IInspectableABI* value) = 0;  // slot 11
};

// 9af0803b-d04c-467a-bbd5-9b81f02d9a56 - put_Foreground is method 1.
struct IIconElementABI : IInspectableABI
{
    virtual void* e0();                                          // slot 6
    virtual HRESULT STDMETHODCALLTYPE put_Foreground(IInspectableABI* value) = 0;  // slot 7
};

static const IID kIFontIcon = { 0xd4d61391, 0x39ac, 0x4950, { 0x91, 0x66, 0x36, 0x06, 0xc2, 0x64, 0x41, 0x8b } };
static const IID kIIconElement = { 0x9af0803b, 0xd04c, 0x467a, { 0xbb, 0xd5, 0x9b, 0x81, 0xf0, 0x2d, 0x9a, 0x56 } };

// IControl: put_Background is method 31 (slot 37)''')

start = s.index("    // A glyph, because an empty Button has no visual content.")
end = s.index("    IInspectableABI* button = Activate(L\"Windows.UI.Xaml.Controls.Button\");", start)

new = '''    // A FontIcon rather than a TextBlock: it is the element XAML uses for glyphs
    // and it carries the icon font itself, which is how the reference mod draws
    // every one of its icons.
    IInspectableABI* text = Activate(L"Windows.UI.Xaml.Controls.FontIcon");
    if (text) {
        IFontIconABI* fontIcon = nullptr;
        if (SUCCEEDED(text->QueryInterface(kIFontIcon, reinterpret_cast<void**>(&fontIcon))) && fontIcon) {
            HSTRING glyph = nullptr;
            const wchar_t* chevron = L"\\xE76C";  // Segoe Fluent Icons: ChevronRight
            g_createString(chevron, 1, &glyph);
            HRESULT hrGlyph = fontIcon->put_Glyph(glyph);
            g_deleteString(glyph);
            fontIcon->put_FontSize(14);

            IInspectableABI* family = MakeFontFamily(L"Segoe Fluent Icons");
            if (family) {
                fontIcon->put_FontFamily(family);
                family->Release();
            }

            IInspectableABI* brush = Activate(L"Windows.UI.Xaml.Media.SolidColorBrush");
            if (brush) {
                ISolidColorBrushABI* solid = nullptr;
                if (SUCCEEDED(brush->QueryInterface(kISolidColorBrush, reinterpret_cast<void**>(&solid))) && solid) {
                    ColorABI white = { 255, 255, 255, 255 };
                    solid->put_Color(white);
                    solid->Release();
                }
                IIconElementABI* iconElement = nullptr;
                if (SUCCEEDED(text->QueryInterface(kIIconElement, reinterpret_cast<void**>(&iconElement))) && iconElement) {
                    iconElement->put_Foreground(brush);
                    iconElement->Release();
                }
                brush->Release();
            }

            Log(L"chevron: FontIcon glyph 0x%08X", hrGlyph);
            fontIcon->Release();
        }
    }

'''
s = s[:start] + new + s[end:]
open(p, "w", encoding="utf-8").write(s)
print("FontIcon wired")

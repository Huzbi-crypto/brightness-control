p = "src/bcdll.cpp"
s = open(p, encoding="utf-8").read()

# --- ABI for a transparent button and a horizontal stack ---
s = s.replace('''static const IID kITextBlock =''',
'''// IControl: put_Background is method 31 (slot 37), put_BorderBrush 32 (slot 38).
struct IControlABI : IInspectableABI
{
    virtual void* c0(); virtual void* c1(); virtual void* c2(); virtual void* c3();
    virtual void* c4(); virtual void* c5(); virtual void* c6(); virtual void* c7();
    virtual void* c8(); virtual void* c9(); virtual void* c10(); virtual void* c11();
    virtual void* c12(); virtual void* c13(); virtual void* c14(); virtual void* c15();
    virtual void* c16(); virtual void* c17(); virtual void* c18(); virtual void* c19();
    virtual void* c20(); virtual void* c21(); virtual void* c22(); virtual void* c23();
    virtual void* c24(); virtual void* c25(); virtual void* c26(); virtual void* c27();
    virtual void* c28(); virtual void* c29(); virtual void* c30();
    virtual HRESULT STDMETHODCALLTYPE put_Background(IInspectableABI* value) = 0;   // slot 37
    virtual HRESULT STDMETHODCALLTYPE put_BorderBrush(IInspectableABI* value) = 0;  // slot 38
};

// IStackPanel: put_Orientation is method 3 (slot 9).
struct IStackPanelABI : IInspectableABI
{
    virtual void* s0(); virtual void* s1(); virtual void* s2();
    virtual HRESULT STDMETHODCALLTYPE put_Orientation(int value) = 0;  // slot 9
};

static const IID kIControl = { 0xa8912263, 0x2951, 0x4f58, { 0xa9, 0xc5, 0x5a, 0x13, 0x4e, 0xaa, 0x7f, 0x07 } };
static const IID kIStackPanel = { 0xb8ae8fe2, 0xd641, 0x4fd7, { 0x80, 0xb4, 0x74, 0x39, 0x20, 0x7d, 0x27, 0x98 } };

static const IID kITextBlock =''')

# --- build one icon TextBlock, reused for both glyphs ---
s = s.replace('''static void BuildChevron()''',
'''// A glyph in the shell's icon font, sized and coloured for the panel.
static IInspectableABI* MakeGlyph(const wchar_t* codepoint, double size, BYTE alpha)
{
    IInspectableABI* text = Activate(L"Windows.UI.Xaml.Controls.TextBlock");
    if (!text)
        return nullptr;

    ITextBlockABI* block = nullptr;
    if (FAILED(text->QueryInterface(kITextBlock, reinterpret_cast<void**>(&block))) || !block) {
        text->Release();
        return nullptr;
    }

    HSTRING glyph = nullptr;
    g_createString(codepoint, 1, &glyph);
    block->put_Text(glyph);
    g_deleteString(glyph);
    block->put_FontSize(size);

    IInspectableABI* fontFamily = MakeFontFamily(L"Segoe Fluent Icons");
    if (fontFamily) {
        block->put_FontFamily(fontFamily);
        fontFamily->Release();
    }

    IInspectableABI* brush = Activate(L"Windows.UI.Xaml.Media.SolidColorBrush");
    if (brush) {
        ISolidColorBrushABI* solid = nullptr;
        if (SUCCEEDED(brush->QueryInterface(kISolidColorBrush, reinterpret_cast<void**>(&solid))) && solid) {
            ColorABI colour = { alpha, 255, 255, 255 };
            solid->put_Color(colour);
            block->put_Foreground(brush);
            solid->Release();
        }
        brush->Release();
    }

    block->Release();
    return text;
}

static void BuildChevron()''')

open(p, "w", encoding="utf-8").write(s)
print("icon ABI + MakeGlyph")

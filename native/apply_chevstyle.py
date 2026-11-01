p = "src/bcdll.cpp"
s = open(p, encoding="utf-8").read()

# ITextBlock needs put_Foreground (slot 19) alongside put_Text (slot 27)
old_text_block = '''struct ITextBlockABI : IInspectableABI
{
    virtual void* u0(); virtual void* u1(); virtual void* u2(); virtual void* u3();
    virtual void* u4(); virtual void* u5(); virtual void* u6(); virtual void* u7();
    virtual void* u8(); virtual void* u9(); virtual void* u10(); virtual void* u11();
    virtual void* u12(); virtual void* u13(); virtual void* u14(); virtual void* u15();
    virtual void* u16(); virtual void* u17(); virtual void* u18(); virtual void* u19();
    virtual void* u20();
    virtual HRESULT STDMETHODCALLTYPE put_Text(HSTRING value) = 0;  // slot 27
};'''
new_text_block = '''struct ITextBlockABI : IInspectableABI
{
    virtual void* u0(); virtual void* u1(); virtual void* u2(); virtual void* u3();
    virtual void* u4(); virtual void* u5(); virtual void* u6(); virtual void* u7();
    virtual void* u8(); virtual void* u9(); virtual void* u10(); virtual void* u11();
    virtual void* u12();
    virtual HRESULT STDMETHODCALLTYPE put_Foreground(IInspectableABI* value) = 0;  // slot 19
    virtual void* u14(); virtual void* u15(); virtual void* u16(); virtual void* u17();
    virtual void* u18(); virtual void* u19(); virtual void* u20();
    virtual HRESULT STDMETHODCALLTYPE put_Text(HSTRING value) = 0;  // slot 27
};

// 9d850850-66f3-48df-9a8f-824bd5e070af - put_Color is method 1.
struct ColorABI
{
    BYTE a, r, g, b;
};

struct ISolidColorBrushABI : IInspectableABI
{
    virtual void* unusedGetColor();                                 // slot 6
    virtual HRESULT STDMETHODCALLTYPE put_Color(ColorABI value) = 0;  // slot 7
};

static const IID kISolidColorBrush = { 0x9d850850, 0x66f3, 0x48df, { 0x9a, 0x8f, 0x82, 0x4b, 0xd5, 0xe0, 0x70, 0xaf } };'''
s = s.replace(old_text_block, new_text_block)

# IFrameworkElement needs put_HorizontalAlignment (slot 28)
s = s.replace('''    virtual void* unused8();                                        // slot 17 get_Height
    virtual HRESULT STDMETHODCALLTYPE put_Height(double value) = 0;  // slot 18
};''',
'''    virtual void* unused8();                                        // slot 17 get_Height
    virtual HRESULT STDMETHODCALLTYPE put_Height(double value) = 0;  // slot 18
    virtual void* a0(); virtual void* a1(); virtual void* a2();      // slots 19..21
    virtual void* a3(); virtual void* a4(); virtual void* a5();      // slots 22..24
    virtual void* a6(); virtual void* a7(); virtual void* a8();      // slots 25..27
    virtual HRESULT STDMETHODCALLTYPE put_HorizontalAlignment(int value) = 0;  // slot 28
};''')

# white glyph
s = s.replace('''            HRESULT hrText = textBlock->put_Text(glyph);
            Log(L"chevron: put_Text 0x%08X", hrText);
            g_deleteString(glyph);
            textBlock->Release();''',
'''            HRESULT hrText = textBlock->put_Text(glyph);
            Log(L"chevron: put_Text 0x%08X", hrText);
            g_deleteString(glyph);

            // A TextBlock defaults to black, which is invisible on the panel.
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
            textBlock->Release();''')

# align right so it sits at the end of the row
s = s.replace('''        element->put_Width(36);
        element->put_Height(32);
        element->Release();''',
'''        element->put_Width(36);
        element->put_Height(32);
        element->put_HorizontalAlignment(2);  // Right
        element->Release();''')

open(p, "w", encoding="utf-8").write(s)
print("chevron styling")

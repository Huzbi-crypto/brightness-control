p = "src/bcdll.cpp"
s = open(p, encoding="utf-8").read()

# put_FontFamily is slot 9
s = s.replace('''    virtual void* u2(); virtual void* u3();''',
'''    virtual void* u2();
    virtual HRESULT STDMETHODCALLTYPE put_FontFamily(IInspectableABI* value) = 0;  // slot 9
    virtual void* u4(); virtual void* u5(); virtual void* u6();''')

# FontFamily factory + helper
s = s.replace('''static const IID kISolidColorBrush''',
'''// d5603377-3dae-4dcd-af09-f9498e9ec659 - CreateInstanceWithName is method 0.
struct IFontFamilyFactoryABI : IInspectableABI
{
    virtual HRESULT STDMETHODCALLTYPE CreateInstanceWithName(HSTRING familyName,
                                                             IInspectableABI* baseUri,
                                                             IInspectableABI** result) = 0;
};

static const IID kIFontFamilyFactory = { 0xd5603377, 0x3dae, 0x4dcd, { 0xaf, 0x09, 0xf9, 0x49, 0x8e, 0x9e, 0xc6, 0x59 } };

static IInspectableABI* MakeFontFamily(const wchar_t* family)
{
    HSTRING className = nullptr;
    const wchar_t* cls = L"Windows.UI.Xaml.Media.FontFamily";
    g_createString(cls, static_cast<UINT32>(wcslen(cls)), &className);
    void* factory = nullptr;
    HRESULT hr = g_roGetActivationFactory(className, kIActivationFactory, &factory);
    g_deleteString(className);
    if (FAILED(hr) || !factory) {
        Log(L"fontfamily: factory 0x%08X", hr);
        return nullptr;
    }

    IFontFamilyFactoryABI* fontFactory = nullptr;
    hr = reinterpret_cast<IInspectableABI*>(factory)->QueryInterface(
        kIFontFamilyFactory, reinterpret_cast<void**>(&fontFactory));
    if (FAILED(hr) || !fontFactory) {
        Log(L"fontfamily: QI 0x%08X", hr);
        reinterpret_cast<IInspectableABI*>(factory)->Release();
        return nullptr;
    }

    HSTRING name = nullptr;
    g_createString(family, static_cast<UINT32>(wcslen(family)), &name);
    IInspectableABI* result = nullptr;
    hr = fontFactory->CreateInstanceWithName(name, nullptr, &result);
    g_deleteString(name);
    fontFactory->Release();
    reinterpret_cast<IInspectableABI*>(factory)->Release();
    Log(L"fontfamily: '%s' 0x%08X", family, hr);
    return SUCCEEDED(hr) ? result : nullptr;
}

static const IID kISolidColorBrush''')

# use the icon font and the real chevron
s = s.replace('''            // A plain chevron rather than E76C: the Segoe Fluent Icons glyphs need
            // that font, which a default TextBlock does not use, and the box it
            // falls back to looks like a placeholder.
            const wchar_t* chevron = L"\\x203A";  // single right angle quotation mark
            g_createString(chevron, 1, &glyph);
            textBlock->put_FontSize(20);''',
'''            // Segoe Fluent Icons ships with Windows 11, so E76C draws a real
            // chevron - but only once the TextBlock is told to use that family.
            // Without it the codepoint falls back to a box.
            const wchar_t* chevron = L"\\xE76C";  // ChevronRight
            g_createString(chevron, 1, &glyph);
            textBlock->put_FontSize(16);
            IInspectableABI* fontFamily = MakeFontFamily(L"Segoe Fluent Icons");
            if (fontFamily) {
                HRESULT hrFont = textBlock->put_FontFamily(fontFamily);
                Log(L"chevron: put_FontFamily 0x%08X", hrFont);
                fontFamily->Release();
            }''')

open(p, "w", encoding="utf-8").write(s)
print("icon font wired")

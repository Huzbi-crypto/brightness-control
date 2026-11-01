p = "src/bcdll.cpp"
s = open(p, encoding="utf-8").read()

# --- 2. put the panel row right after the sliders, not at the very bottom ---
s = s.replace('''        IInspectableABI* rowDefinition = Activate(L"Windows.UI.Xaml.Controls.RowDefinition");
        if (rowDefinition) {
            IRowDefinitionABI* row = nullptr;
            if (SUCCEEDED(rowDefinition->QueryInterface(kIRowDefinition, reinterpret_cast<void**>(&row))) && row) {
                GridLengthABI autoHeight = { 0.0, 0, 0 };
                row->put_Height(autoHeight);
                row->Release();
            }
            rows->Append(rowDefinition);
            rowDefinition->Release();
        }
        rowsRaw->Release();''',
'''        // Row 2 is the footer, so insert at 2 and push the footer down to 3.
        // Appending instead would put the panel below the battery row.
        const unsigned insertAt = 2;
        IInspectableABI* rowDefinition = Activate(L"Windows.UI.Xaml.Controls.RowDefinition");
        if (rowDefinition) {
            IRowDefinitionABI* row = nullptr;
            if (SUCCEEDED(rowDefinition->QueryInterface(kIRowDefinition, reinterpret_cast<void**>(&row))) && row) {
                GridLengthABI autoHeight = { 0.0, 0, 0 };
                row->put_Height(autoHeight);
                row->Release();
            }
            if (count > insertAt)
                rows->InsertAt(insertAt, rowDefinition);
            else
                rows->Append(rowDefinition);
            rowDefinition->Release();
        }
        rowsRaw->Release();
        count = insertAt;''')

# --- 3. a lighter chevron: smaller button, smaller glyph ---
s = s.replace('''        element->put_Width(36);
        element->put_Height(32);
        element->put_HorizontalAlignment(2);  // Right''',
'''        // Match the volume row's trailing chevron rather than a big boxed arrow.
        element->put_Width(28);
        element->put_Height(28);
        element->put_HorizontalAlignment(2);  // Right''')

s = s.replace('''            textBlock->put_FontSize(16);''', '''            textBlock->put_FontSize(12);''')

open(p, "w", encoding="utf-8").write(s)
print("position + chevron size")

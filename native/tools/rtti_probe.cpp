// rtti_probe.exe — static, read-only RTTI walk over ControlCenter.dll.
//
// Finds the type descriptor for a named class, then the Complete Object
// Locator that points at it, then the vtable that locates through it. That
// gives us the vtable address of ControlCenterView without needing a symbol
// server, which is what the hook targets live behind.
//
//   rtti_probe.exe C:\Windows\System32\ControlCenter.dll ControlCenterView

#include <windows.h>
#include <stdio.h>
#include <vector>
#include <string>

struct Mapped {
    std::vector<BYTE> bytes;
    const IMAGE_NT_HEADERS64* nt = nullptr;
    const IMAGE_SECTION_HEADER* sections = nullptr;
    ULONGLONG imageBase = 0;

    bool Load(const wchar_t* path)
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"rb") != 0 || !f) return false;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        bytes.resize(size);
        size_t got = fread(bytes.data(), 1, size, f);
        fclose(f);
        if (got != (size_t)size) return false;

        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes.data() + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        sections = IMAGE_FIRST_SECTION(nt);
        imageBase = nt->OptionalHeader.ImageBase;
        return true;
    }

    // file offset -> RVA
    DWORD OffsetToRva(DWORD offset) const
    {
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const auto& s = sections[i];
            if (offset >= s.PointerToRawData && offset < s.PointerToRawData + s.SizeOfRawData)
                return s.VirtualAddress + (offset - s.PointerToRawData);
        }
        return 0;
    }

    // RVA -> file offset, or -1
    long RvaToOffset(DWORD rva) const
    {
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const auto& s = sections[i];
            if (rva >= s.VirtualAddress && rva < s.VirtualAddress + s.SizeOfRawData)
                return (long)(s.PointerToRawData + (rva - s.VirtualAddress));
        }
        return -1;
    }

    DWORD Size() const { return nt->OptionalHeader.SizeOfImage; }
    DWORD Read32(size_t off) const { return *reinterpret_cast<const DWORD*>(bytes.data() + off); }
    ULONGLONG Read64(size_t off) const { return *reinterpret_cast<const ULONGLONG*>(bytes.data() + off); }
};

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) {
        wprintf(L"usage: %s <dll> <class-name-fragment>\n", argv[0]);
        return 2;
    }

    Mapped m;
    if (!m.Load(argv[1])) { wprintf(L"could not map %s\n", argv[1]); return 1; }
    wprintf(L"image base 0x%llX, size 0x%X\n", m.imageBase, m.Size());

    // The type descriptor's name is a NUL-terminated char string; the struct is
    // { vfptr, spare, name[] } so it starts 16 bytes before the text on x64.
    // MSVC stores the decorated name, e.g. ".?AVControlCenterView@@", so anchor
    // on the prefix: matching the bare class name hits mid-string and puts the
    // descriptor 4 bytes off.
    char needle[160] = ".?AV";
    char frag[128] = {};
    WideCharToMultiByte(CP_ACP, 0, argv[2], -1, frag, sizeof(frag) - 1, nullptr, nullptr);
    strncat_s(needle, frag, sizeof(needle) - strlen(needle) - 1);

    std::string hay(reinterpret_cast<const char*>(m.bytes.data()), m.bytes.size());
    std::vector<DWORD> typeDescriptorRvas;

    for (size_t pos = hay.find(needle); pos != std::string::npos; pos = hay.find(needle, pos + 1)) {
        if (pos < 16) continue;
        DWORD rva = m.OffsetToRva((DWORD)(pos - 16));
        if (rva) typeDescriptorRvas.push_back(rva);
    }

    wprintf(L"type descriptor candidates for \"%S\": %zu\n", needle, typeDescriptorRvas.size());
    for (DWORD td : typeDescriptorRvas) wprintf(L"  TD rva 0x%X\n", td);

    // CompleteObjectLocator (x64): signature=1, offset, cdOffset,
    // pTypeDescriptor (RVA), pClassDescriptor (RVA), pSelf (RVA)
    struct Col { DWORD rva; };
    std::vector<Col> cols;
    for (size_t off = 0; off + 24 < m.bytes.size(); off += 4) {
        if (m.Read32(off) != 1) continue;
        DWORD tdRef = m.Read32(off + 12);
        for (DWORD td : typeDescriptorRvas) {
            if (tdRef == td) {
                DWORD rva = m.OffsetToRva((DWORD)off);
                if (rva) cols.push_back({ rva });
                break;
            }
        }
    }

    wprintf(L"complete object locators: %zu\n", cols.size());

    // vtable[-1] holds the locator; scan for that pointer to find the table.
    for (const auto& col : cols) {
        ULONGLONG wanted = m.imageBase + col.rva;
        wprintf(L"  COL rva 0x%X (expect vtable[-1] == 0x%llX)\n", col.rva, wanted);

        int found = 0;
        for (size_t off = 0; off + 8 < m.bytes.size() && found < 4; off += 8) {
            if (m.Read64(off) != wanted) continue;

            DWORD slotRva = m.OffsetToRva((DWORD)off);
            if (!slotRva) continue;
            DWORD vtableRva = slotRva + 8;
            wprintf(L"    vtable at rva 0x%X (va 0x%llX)\n", vtableRva, m.imageBase + vtableRva);

            long voff = m.RvaToOffset(vtableRva);
            if (voff < 0) continue;
            for (int i = 0; i < 12; ++i) {
                size_t so = (size_t)voff + i * 8;
                if (so + 8 > m.bytes.size()) break;
                ULONGLONG fn = m.Read64(so);
                DWORD fnRva = (DWORD)(fn - m.imageBase);
                wprintf(L"      slot[%2d] 0x%llX  (rva 0x%X)\n", i, fn, fnRva);
            }
            found++;
        }
        if (!found) wprintf(L"    no vtable referenced this locator\n");
    }
    return 0;
}

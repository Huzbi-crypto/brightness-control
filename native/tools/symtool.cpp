// symtool.exe — get symbol RVAs for a Windows component without a symbol server client.
//
//   symtool.exe <dll> <cachedir> [substring ...]
//
// 1. reads the PE debug directory for the CodeView record (PDB GUID + age + name)
// 2. downloads that PDB from msdl.microsoft.com if it is not already cached
// 3. enumerates the PDB's symbols and prints the RVA of every name containing
//    all the given substrings
//
// Used to find the two non-exported virtuals on ControlCenterView that the
// XAML hook needs. DbgHelp reads the PDB straight from the cache directory, so
// symsrv.dll is not required.

#include <windows.h>
#include <dbghelp.h>
#include <urlmon.h>
#include <stdio.h>
#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "urlmon.lib")

static std::vector<BYTE> ReadFileBytes(const wchar_t* path)
{
    std::vector<BYTE> data;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return data;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data.resize(size);
    fread(data.data(), 1, size, f);
    fclose(f);
    return data;
}

struct CodeView {
    bool valid = false;
    GUID guid = {};
    DWORD age = 0;
    std::wstring pdbName;
};

static CodeView ReadCodeView(const std::vector<BYTE>& img)
{
    CodeView cv;
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(img.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return cv;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(img.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return cv;

    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!dir.Size) return cv;

    auto sec = IMAGE_FIRST_SECTION(nt);
    auto rvaToOff = [&](DWORD rva) -> long {
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            if (rva >= sec[i].VirtualAddress && rva < sec[i].VirtualAddress + sec[i].SizeOfRawData)
                return (long)(sec[i].PointerToRawData + (rva - sec[i].VirtualAddress));
        return -1;
    };

    long off = rvaToOff(dir.VirtualAddress);
    if (off < 0) return cv;

    auto entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(img.data() + off);
    DWORD count = dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    for (DWORD i = 0; i < count; ++i) {
        if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
        long cvOff = (long)entries[i].PointerToRawData;
        if (cvOff <= 0) continue;
        auto base = img.data() + cvOff;
        if (*(DWORD*)base != 0x53445352) continue; // 'RSDS'
        auto p = base + 4;
        memcpy(&cv.guid, p, sizeof(GUID));
        p += sizeof(GUID);
        cv.age = *(DWORD*)p;
        p += 4;
        const char* name = reinterpret_cast<const char*>(p);
        std::wstring wide(name, name + strlen(name));
        cv.pdbName = wide;
        cv.valid = true;
        return cv;
    }
    return cv;
}

struct EnumCtx {
    std::vector<std::wstring> needles;
    DWORD base;
};

static BOOL CALLBACK EnumCallback(PSYMBOL_INFOW sym, ULONG, PVOID ctx)
{
    auto c = reinterpret_cast<EnumCtx*>(ctx);
    std::wstring name = sym->Name;
    for (auto& n : c->needles)
        if (name.find(n) == std::wstring::npos) return TRUE;

    wprintf(L"  0x%08X  %s\n", sym->Address - c->base, sym->Name);
    return TRUE;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 4) {
        wprintf(L"usage: %s <dll> <cachedir> <substring> [substring ...]\n", argv[0]);
        return 2;
    }

    auto img = ReadFileBytes(argv[1]);
    if (img.empty()) { wprintf(L"cannot read %s\n", argv[1]); return 1; }

    CodeView cv = ReadCodeView(img);
    if (!cv.valid) { wprintf(L"no CodeView record\n"); return 1; }

    wchar_t guidHex[64];
    swprintf_s(guidHex, L"%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%X",
               cv.guid.Data1, cv.guid.Data2, cv.guid.Data3,
               cv.guid.Data4[0], cv.guid.Data4[1], cv.guid.Data4[2], cv.guid.Data4[3],
               cv.guid.Data4[4], cv.guid.Data4[5], cv.guid.Data4[6], cv.guid.Data4[7], cv.age);

    wprintf(L"PDB   : %s\nGUID  : %s (age %lu)\n", cv.pdbName.c_str(), guidHex, cv.age);

    std::wstring cache = argv[2];
    CreateDirectoryW(cache.c_str(), nullptr);
    std::wstring local = cache + L"\\" + cv.pdbName;

    if (GetFileAttributesW(local.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring url = L"https://msdl.microsoft.com/download/symbols/" + cv.pdbName +
                           L"/" + guidHex + L"/" + cv.pdbName;
        wprintf(L"fetch : %s\n", url.c_str());
        HRESULT hr = URLDownloadToFileW(nullptr, url.c_str(), local.c_str(), 0, nullptr);
        if (FAILED(hr)) {
            wprintf(L"download failed 0x%08X (PDB not published?)\n", hr);
            return 1;
        }
    } else {
        wprintf(L"cached: %s\n", local.c_str());
    }

    wprintf(L"\nresolving...\n");

    auto base = SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_ANYTHING);
    (void)base;
    HANDLE proc = GetCurrentProcess();
    if (!SymInitializeW(proc, cache.c_str(), FALSE)) {
        wprintf(L"SymInitialize failed %lu\n", GetLastError());
        return 1;
    }

    DWORD64 modBase = SymLoadModuleExW(proc, nullptr, argv[1], nullptr, 0, 0, nullptr, 0);
    if (!modBase) {
        wprintf(L"SymLoadModuleEx failed %lu\n", GetLastError());
        SymCleanup(proc);
        return 1;
    }
    wprintf(L"module base 0x%llX\n\n", modBase);

    EnumCtx ctx;
    ctx.base = (DWORD)modBase;
    for (int i = 3; i < argc; ++i) ctx.needles.push_back(argv[i]);

    SymEnumSymbolsW(proc, modBase, L"*", EnumCallback, &ctx);

    SymCleanup(proc);
    return 0;
}

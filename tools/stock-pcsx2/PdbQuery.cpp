// Read-only DIA queries. This program never loads or executes the target image.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dia2.h>
#include <diacreate.h>
#include <atlbase.h>
#include <atlcomcli.h>
#include <algorithm>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

static void Check(HRESULT hr, const char* operation)
{
    if (FAILED(hr))
    {
        std::ostringstream message;
        message << operation << " failed: HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
        throw std::runtime_error(message.str());
    }
}

static std::string Utf8(const wchar_t* text)
{
    if (!text) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

static std::string Json(const std::string& text)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : text)
    {
        if (c == '\\' || c == '"') { result += '\\'; result += c; }
        else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += c;
    }
    return result + '"';
}

static std::string Name(IDiaSymbol* symbol)
{
    CComBSTR name;
    symbol->get_name(&name);
    return Utf8(name);
}

static std::vector<CComPtr<IDiaSymbol>> Children(IDiaSymbol* parent, enum SymTagEnum tag,
    const wchar_t* name = nullptr)
{
    CComPtr<IDiaEnumSymbols> enumeration;
    Check(parent->findChildren(tag, name, nsCaseSensitive, &enumeration), "findChildren");
    std::vector<CComPtr<IDiaSymbol>> result;
    if (!enumeration) return result;
    for (;;)
    {
        CComPtr<IDiaSymbol> child;
        ULONG count = 0;
        Check(enumeration->Next(1, &child, &count), "Next symbol");
        if (!count) break;
        result.push_back(child);
    }
    return result;
}

// Preserve ABI facts rather than DIA's build-specific type IDs. Pointer targets
// are shallow, preventing cycles in class graphs; records are queried separately.
static std::string Type(IDiaSymbol* symbol, unsigned depth = 0)
{
    if (!symbol || depth > 8) return "null";
    DWORD tag = 0;
    ULONGLONG length = 0;
    symbol->get_symTag(&tag);
    symbol->get_length(&length);
    BOOL isConst = FALSE, isVolatile = FALSE;
    symbol->get_constType(&isConst);
    symbol->get_volatileType(&isVolatile);
    std::ostringstream out;
    out << "{\"tag\":" << tag << ",\"name\":" << Json(Name(symbol)) << ",\"size\":" << length
        << ",\"const\":" << (isConst ? "true" : "false")
        << ",\"volatile\":" << (isVolatile ? "true" : "false");
    if (tag == SymTagBaseType)
    {
        DWORD base = 0;
        symbol->get_baseType(&base);
        out << ",\"base_type\":" << base;
    }
    else if (tag == SymTagPointerType || tag == SymTagArrayType || tag == SymTagTypedef || tag == SymTagFunctionArgType)
    {
        CComPtr<IDiaSymbol> target;
        symbol->get_type(&target);
        if (tag == SymTagPointerType)
        {
            BOOL reference = FALSE;
            symbol->get_reference(&reference);
            out << ",\"reference\":" << (reference ? "true" : "false");
        }
        if (tag == SymTagArrayType)
        {
            DWORD count = 0;
            symbol->get_count(&count);
            out << ",\"count\":" << count;
        }
        if (target) out << ",\"target\":" << Type(target, depth + 1);
    }
    else if (tag == SymTagFunctionType)
    {
        DWORD convention = 0;
        symbol->get_callingConvention(&convention);
        CComPtr<IDiaSymbol> returnType;
        symbol->get_type(&returnType);
        out << ",\"calling_convention\":" << convention << ",\"return\":" << Type(returnType, depth + 1);
        out << ",\"arguments\":[";
        bool first = true;
        for (const auto& argument : Children(symbol, SymTagFunctionArgType))
        {
            if (!first) out << ',';
            first = false;
            out << Type(argument, depth + 1);
        }
        out << ']';
    }
    else if (tag == SymTagEnum)
    {
        CComPtr<IDiaSymbol> underlying;
        symbol->get_type(&underlying);
        out << ",\"underlying\":" << Type(underlying, depth + 1) << ",\"values\":[";
        bool first = true;
        for (const auto& constant : Children(symbol, SymTagData))
        {
            CComVariant value;
            if (constant->get_value(&value) != S_OK) continue;
            Check(value.ChangeType(VT_BSTR), "format enum constant");
            if (!first) out << ',';
            first = false;
            out << "{\"name\":" << Json(Name(constant)) << ",\"value\":" << Json(Utf8(value.bstrVal)) << '}';
        }
        out << ']';
    }
    out << '}';
    return out.str();
}

static std::string Describe(IDiaSymbol* symbol, bool record)
{
    DWORD tag = 0, rva = 0, location = 0;
    ULONGLONG length = 0;
    symbol->get_symTag(&tag);
    const bool hasRva = symbol->get_relativeVirtualAddress(&rva) == S_OK && rva != 0;
    symbol->get_length(&length);
    symbol->get_locationType(&location);
    std::ostringstream out;
    out << "{\"name\":" << Json(Name(symbol)) << ",\"tag\":" << tag
        << ",\"rva\":" << (hasRva ? std::to_string(rva) : "null")
        << ",\"size\":" << length << ",\"location\":" << location;
    CComPtr<IDiaSymbol> type;
    symbol->get_type(&type);
    out << ",\"type\":" << Type(record ? symbol : type.p);
    if (record)
    {
        out << ",\"members\":[";
        bool first = true;
        for (const auto& member : Children(symbol, SymTagData))
        {
            DWORD kind = 0;
            member->get_dataKind(&kind);
            if (kind != DataIsMember) continue;
            if (!first) out << ',';
            first = false;
            LONG offset = 0;
            DWORD bit = 0, memberLocation = 0;
            ULONGLONG bitLength = 0;
            member->get_offset(&offset);
            member->get_bitPosition(&bit);
            member->get_length(&bitLength);
            member->get_locationType(&memberLocation);
            CComPtr<IDiaSymbol> memberType;
            member->get_type(&memberType);
            out << "{\"name\":" << Json(Name(member)) << ",\"offset\":" << offset
                << ",\"type\":" << Type(memberType);
            if (memberLocation == LocIsBitField)
                out << ",\"bit_position\":" << bit << ",\"bit_size\":" << bitLength;
            out << '}';
        }
        out << ']';
    }
    out << '}';
    return out.str();
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 7 || (argc - 5) % 2)
    {
        std::cerr << "Usage: PdbQuery DIA_DLL PDB GUID AGE {symbol|type NAME}...\n";
        return 2;
    }
    try
    {
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM initialization");
        GUID guid{};
        Check(CLSIDFromString(argv[3], &guid), "parse expected GUID");
        const DWORD age = static_cast<DWORD>(std::stoul(argv[4]));
        CComPtr<IDiaDataSource> source;
        Check(NoRegCoCreate(argv[1], __uuidof(DiaSource), __uuidof(IDiaDataSource),
            reinterpret_cast<void**>(&source.p)), "create DIA source");
        Check(source->loadAndValidateDataFromPdb(argv[2], &guid, 0, age), "validate PDB GUID/age");
        CComPtr<IDiaSession> session;
        Check(source->openSession(&session), "open DIA session");
        CComPtr<IDiaSymbol> global;
        Check(session->get_globalScope(&global), "get global scope");
        const auto compilands = Children(global, SymTagCompiland);
        DWORD machine = 0;
        Check(global->get_machineType(&machine), "get machine type");
        std::cout << "{\"machine\":" << machine << ",\"queries\":[";
        for (int i = 5; i < argc; i += 2)
        {
            if (i != 5) std::cout << ',';
            const bool record = std::wstring(argv[i]) == L"type";
            if (!record && std::wstring(argv[i]) != L"symbol") throw std::runtime_error("Unknown query kind");
            std::vector<std::string> matches;
            std::set<DWORD> ids;
            const auto scan = [&](IDiaSymbol* scope)
            {
                for (const auto& symbol : Children(scope, record ? SymTagUDT : SymTagNull, argv[i + 1]))
                {
                    DWORD tag = 0, id = 0;
                    symbol->get_symTag(&tag);
                    symbol->get_symIndexId(&id);
                    if (!record && tag != SymTagFunction && tag != SymTagData) continue;
                    if (ids.insert(id).second) matches.push_back(Describe(symbol, record));
                }
            };
            scan(global);
            if (!record) for (const auto& compiland : compilands) scan(compiland);
            // Same record may be emitted by many compilation units. Coalesce
            // identical facts, retaining conflicting layouts for manual review.
            std::sort(matches.begin(), matches.end());
            matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
            std::cout << "{\"kind\":" << Json(Utf8(argv[i])) << ",\"name\":"
                << Json(Utf8(argv[i + 1])) << ",\"matches\":[";
            for (size_t j = 0; j < matches.size(); ++j)
            {
                if (j) std::cout << ',';
                std::cout << matches[j];
            }
            std::cout << "]}";
        }
        std::cout << "]}\n";
        // CComPtr instances release before process teardown; no COM objects or
        // provider modules are shared with the emulator.
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

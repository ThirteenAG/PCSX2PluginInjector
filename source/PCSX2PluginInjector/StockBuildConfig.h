#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include "IniReader.h"
#include "StockAbiReference.h"
#pragma comment(lib, "bcrypt.lib")

namespace StockPCSX2
{
inline constexpr const char* Roles[] = {"boot_patches", "set_error", "render_osd", "imgui_begin", "imgui_end", "imgui_text",
    "elf_init", "elf_loading", "shutdown", "reset", "ei", "syscall", "rec_ei", "rec_syscall", "clear_cpu_caches",
    "clear_guest_code", "reset_block_tracking", "throttle", "save_state", "load_state", "finish_vu0", "ee_memory", "exposed_ram", "registers", "vm_state",
    "disc_serial", "disc_elf", "disc_version", "title", "disc_crc", "current_crc", "elf_entry", "elf_path", "elf_executed", "config", "gs_device", "emu_thread", "vu_registers"};
inline constexpr size_t FunctionRoleCount = 21;
struct Symbol
{
    uint32_t rva = 0, extent = 0;
    std::vector<uint8_t> bytes, mask;
};
struct Configuration
{
    std::map<std::string, Symbol> symbols;
    std::string version, activation, source, abi, sha, guid;
    uint32_t image_size = 0, age = 0;
};
inline uint32_t Number(const std::string& text)
{
    uint32_t number = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::runtime_error("Invalid/missing stock configuration number");
    return number;
}
inline std::vector<uint8_t> Hex(const std::string& value)
{
    if (value.empty() || value.size() % 2) throw std::runtime_error("Invalid hexadecimal configuration value");
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < value.size(); i += 2)
    {
        unsigned number = 0;
        auto parsed = std::from_chars(value.data() + i, value.data() + i + 2, number, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + i + 2 || number > 255)
            throw std::runtime_error("Invalid hexadecimal configuration value");
        bytes.push_back(static_cast<uint8_t>(number));
    }
    return bytes;
}
inline Configuration ReadConfiguration(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path)) throw std::runtime_error("Missing PCSX2PluginInjector.stock.ini beside the injector");
    CIniReader ini(path);
    auto identity = [&](const char* name) { return ini.ReadString("Identity", name, ""); };
    if (identity("Schema") != "1" || identity("Adapter") != "upstream-win64-v1")
        throw std::runtime_error("Unknown stock adapter schema/profile");
    Configuration c;
    c.version = identity("Version"); c.activation = identity("Activation"); c.source = identity("SourceCommit");
    c.abi = identity("AdapterABI"); c.sha = identity("ExecutableSHA256"); c.guid = identity("PdbGUID");
    c.image_size = Number(identity("ImageSize")); c.age = Number(identity("PdbAge"));
    if (c.source != StockABI::SourceCommit || c.abi != StockABI::Fingerprint)
        throw std::runtime_error("Stock build source/ABI differs from this injector; update both together");
    if (c.activation != "disabled" && c.activation != "test-checkpoint" && c.activation != "source-matched")
        throw std::runtime_error("Unknown stock profile activation state");
    if (Hex(c.sha).size() != 32 || c.image_size > 0x40000000 || !c.image_size)
        throw std::runtime_error("Invalid stock image identity");
    for (size_t i = 0; i < std::size(Roles); ++i)
    {
        const std::string role(Roles[i]);
        Symbol symbol;
        symbol.rva = Number(ini.ReadString("Symbols", role, ""));
        symbol.extent = Number(ini.ReadString("Symbols", role + ".extent", ""));
        if (!symbol.rva || !symbol.extent || static_cast<uint64_t>(symbol.rva) + symbol.extent > c.image_size)
            throw std::runtime_error("Out-of-image stock symbol: " + role);
        if (i < FunctionRoleCount)
        {
            symbol.bytes = Hex(ini.ReadString("Symbols", role + ".bytes", ""));
            symbol.mask = Hex(ini.ReadString("Symbols", role + ".mask", ""));
            if (symbol.bytes.size() != std::min<size_t>(32, symbol.extent) || symbol.mask.size() != symbol.bytes.size())
                throw std::runtime_error("Invalid function validation bytes: " + role);
            for (uint8_t mask : symbol.mask) if (mask != 0 && mask != 255)
                throw std::runtime_error("Invalid function byte mask");
            if (std::count(symbol.mask.begin(), symbol.mask.end(), 255) < 5)
                throw std::runtime_error("Insufficient function validation bytes: " + role);
        }
        c.symbols.emplace(role, std::move(symbol));
    }
    const std::pair<const char*, uint32_t> extents[] = {{"ee_memory", 8}, {"exposed_ram", 4},
        {"registers", StockABI::RegisterPackSize}, {"vm_state", 4}, {"disc_serial", 32}, {"disc_elf", 32},
        {"disc_version", 32}, {"title", 32}, {"disc_crc", 4}, {"current_crc", 4}, {"elf_entry", 4},
        {"elf_path", 32}, {"elf_executed", 1}, {"config", StockABI::ConfigSize}, {"gs_device", 8}, {"emu_thread", 8}, {"vu_registers", StockABI::VURegisterSize * 2}};
    for (auto [name, extent] : extents)
        if (c.symbols.at(name).extent != extent) throw std::runtime_error("Stock data ABI extent mismatch");
    return c;
}
inline std::vector<uint8_t> ReadImageFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = file ? file.tellg() : std::streampos(-1);
    if (size <= 0 || size > 128 * 1024 * 1024) throw std::runtime_error("Cannot read bounded stock executable image");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) throw std::runtime_error("Cannot read stock executable image");
    return bytes;
}
inline std::string SHA256(std::span<const uint8_t> bytes)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<uint8_t, 32> result{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("SHA-256 provider unavailable");
    const auto created = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    NTSTATUS status = created;
    if (status >= 0) status = BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0);
    if (status >= 0) status = BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0);
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("Stock executable hashing failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string text;
    for (uint8_t byte : result) { text += hex[byte >> 4]; text += hex[byte & 15]; }
    return text;
}
inline bool Readable(const uint8_t* pointer, size_t size)
{
    const uintptr_t begin = reinterpret_cast<uintptr_t>(pointer);
    if (!begin || !size || begin + size < begin) return false;
    uintptr_t current = begin;
    while (current < begin + size)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(current), &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (end <= current) return false;
        current = end;
    }
    return true;
}
inline void ValidateImage(const Configuration& c, std::span<const uint8_t> file, const uint8_t* mapped)
{
    if (SHA256(file) != c.sha) throw std::runtime_error("Unsupported PCSX2 executable SHA-256; no hooks installed");
    if (!Readable(mapped, sizeof(IMAGE_DOS_HEADER))) throw std::runtime_error("Unreadable stock image header");
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mapped);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 4096 ||
        !Readable(mapped + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS64))) throw std::runtime_error("Invalid stock DOS/PE header");
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(mapped + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt->OptionalHeader.SizeOfImage != c.image_size)
        throw std::runtime_error("Stock PE machine/image identity mismatch");
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!directory.Size || directory.Size % sizeof(IMAGE_DEBUG_DIRECTORY) ||
        static_cast<uint64_t>(directory.VirtualAddress) + directory.Size > c.image_size ||
        !Readable(mapped + directory.VirtualAddress, directory.Size)) throw std::runtime_error("Invalid CodeView debug directory");
    const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(mapped + directory.VirtualAddress);
    bool identity_found = false;
    for (size_t i = 0; i < directory.Size / sizeof(*entries); ++i)
    {
        const auto& entry = entries[i];
        if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
        if (entry.SizeOfData < 24 || static_cast<uint64_t>(entry.AddressOfRawData) + entry.SizeOfData > c.image_size ||
            !Readable(mapped + entry.AddressOfRawData, entry.SizeOfData)) throw std::runtime_error("Invalid CodeView identity");
        const auto* record = mapped + entry.AddressOfRawData;
        if (std::memcmp(record, "RSDS", 4)) throw std::runtime_error("Unsupported CodeView identity");
        GUID guid{};
        uint32_t age;
        std::memcpy(&guid, record + 4, 16); std::memcpy(&age, record + 20, 4);
        char formatted[37];
        sprintf_s(formatted, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", guid.Data1, guid.Data2, guid.Data3,
            guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
        if (c.guid != formatted || c.age != age) throw std::runtime_error("Stock PDB GUID/age mismatch");
        if (identity_found) throw std::runtime_error("Ambiguous stock CodeView identities");
        identity_found = true;
    }
    if (!identity_found) throw std::runtime_error("Missing stock CodeView identity");
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    if (!Readable(reinterpret_cast<const uint8_t*>(sections), nt->FileHeader.NumberOfSections * sizeof(*sections)))
        throw std::runtime_error("Invalid stock section table");
    for (size_t role = 0; role < std::size(Roles); ++role)
    {
        const auto& symbol = c.symbols.at(Roles[role]);
        const IMAGE_SECTION_HEADER* owner = nullptr;
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            if (symbol.rva >= sections[i].VirtualAddress && static_cast<uint64_t>(symbol.rva) + symbol.extent <=
                static_cast<uint64_t>(sections[i].VirtualAddress) + std::max(sections[i].Misc.VirtualSize, sections[i].SizeOfRawData))
            {
                if (owner) throw std::runtime_error("Ambiguous stock symbol section");
                owner = &sections[i];
            }
        const bool function = role < FunctionRoleCount;
        if (!owner || !(owner->Characteristics & IMAGE_SCN_MEM_READ) ||
            function != static_cast<bool>(owner->Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
            (!function && !(owner->Characteristics & IMAGE_SCN_MEM_WRITE)) ||
            !Readable(mapped + symbol.rva, symbol.extent)) throw std::runtime_error("Stock symbol permissions/bounds mismatch");
        for (size_t i = 0; i < symbol.bytes.size(); ++i)
            if ((mapped[symbol.rva + i] & symbol.mask[i]) != (symbol.bytes[i] & symbol.mask[i]))
                throw std::runtime_error(std::string("Stock function already changed or mismatched: ") + Roles[role]);
    }
}
}

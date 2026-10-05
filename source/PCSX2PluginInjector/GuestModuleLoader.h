#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "../API/pcsx2f_api.h"
#include "../API/guest_module.h"

namespace GuestModule
{
    static_assert(sizeof(PCSX2FModuleDescriptor) == 32);
    static_assert(sizeof(PCSX2FModuleContext) == 32);
    constexpr uint32_t MaxFileSize = 128 * 1024 * 1024;
    constexpr uint32_t ArenaBegin = 0x02001000; // first page belongs to the fork
    constexpr uint32_t ArenaEnd = 0x08000000;
    struct Symbol { uint32_t address, size, flags; };
    struct Section { uint32_t name, type, flags, address, offset, size, link, info, alignment, entrySize, placed = 0; };
    struct Image
    {
        PluginInfo info{}; // adapter for existing host input/display consumers
        std::vector<char> bytes;
        std::unordered_map<std::string, Symbol> symbols;
        std::vector<Section> sections;
        uint32_t stackTop = 0, context = 0, heapBegin = 0, heapEnd = 0;

        const Symbol* find(std::string_view name) const
        {
            auto it = symbols.find(std::string(name));
            return it == symbols.end() ? nullptr : &it->second;
        }
        bool contains(uint32_t address, uint32_t size, uint32_t flags = 0) const
        {
            for (const auto& section : sections)
                if ((section.flags & 2) && (section.flags & flags) == flags && size &&
                    address >= section.placed && uint64_t(address) + size <= uint64_t(section.placed) + section.size)
                    return true;
            return false;
        }
        uint32_t word(uint32_t address) const
        {
            uint32_t value;
            std::memcpy(&value, bytes.data() + address - info.Base, 4);
            return value;
        }
        bool matchesCRC(uint32_t address, uint32_t size, uint32_t crc) const
        {
            if (!size || size % 4 || !contains(address, size)) return false;
            for (uint32_t offset = 0; offset < size; offset += 4)
                if (word(address + offset) == crc) return true;
            return false;
        }
    };

    namespace Detail
    {
        inline uint16_t U16(const std::vector<char>& b, size_t at)
        { return uint16_t(uint8_t(b[at])) | uint16_t(uint16_t(uint8_t(b[at + 1])) << 8); }
        inline uint32_t U32(const std::vector<char>& b, size_t at)
        { return uint32_t(U16(b, at)) | (uint32_t(U16(b, at + 2)) << 16); }
        inline void Put(std::vector<char>& b, size_t at, uint32_t value)
        { for (unsigned i = 0; i < 4; ++i) b[at + i] = char(value >> (i * 8)); }
        inline bool InFile(const std::vector<char>& b, uint64_t at, uint64_t size)
        { return at <= b.size() && size <= b.size() - at; }
        inline bool PowerOfTwo(uint32_t n) { return n == 0 || (n & (n - 1)) == 0; }
        inline uint64_t Align(uint64_t n, uint32_t alignment)
        { return alignment <= 1 ? n : (n + alignment - 1) & ~uint64_t(alignment - 1); }
        inline bool String(const std::vector<char>& b, const Section& table, uint32_t at, std::string& name)
        {
            if (at >= table.size) return false;
            auto* begin = b.data() + table.offset + at;
            auto* end = static_cast<const char*>(std::memchr(begin, 0, table.size - at));
            if (!end) return false;
            name.assign(begin, end);
            return true;
        }
    }

    // Transactional: no guest memory writes, and output stays unchanged on error.
    inline bool Load(const std::vector<char>& file, uint32_t base, uint32_t arenaEnd, Image& output, std::string& error)
    {
        using namespace Detail;
        auto fail = [&](std::string reason) { error = std::move(reason); return false; };
        if (file.size() < 52 || file.size() > MaxFileSize) return fail("invalid module file size");
        if (std::memcmp(file.data(), "\x7f" "ELF", 4) || file[4] != 1 || file[5] != 1 || file[6] != 1)
            return fail("expected ELF32 little-endian");
        if (U16(file, 16) == 2)
            return fail("fixed-address plugin is obsolete; update/rebuild this plugin for the relocatable module ABI");
        if (U16(file, 16) != 1 || U16(file, 18) != 8 || U32(file, 20) != 1 || U16(file, 40) != 52 ||
            U16(file, 44) != 0 || U32(file, 24) != 0 || (U32(file, 36) & (2 | 0x06000000)))
            return fail("expected non-PIC MIPS ET_REL (-G0 -mno-abicalls -fno-pic)");
        // EF_MIPS_CPIC alone is also emitted by non-PIC SDK archive objects.
        // Their absolute relocations are supported; GOT/GP-relative/PIC code
        // still fails the explicit relocation whitelist below.
        if (base < ArenaBegin || arenaEnd > ArenaEnd || base % 128 || base >= arenaEnd)
            return fail("invalid module arena");
        const uint32_t shOffset = U32(file, 32);
        const uint16_t count = U16(file, 48), strings = U16(file, 50);
        if (!count || !strings || strings >= count || U16(file, 46) != 40 || shOffset < 52 ||
            !InFile(file, shOffset, uint64_t(count) * 40)) return fail("invalid section table");
        Image image;
        image.info.Base = base;
        uint64_t cursor = base;
        uint32_t symbolTable = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const size_t at = shOffset + size_t(i) * 40;
            Section s{U32(file, at), U32(file, at + 4), U32(file, at + 8), U32(file, at + 12),
                U32(file, at + 16), U32(file, at + 20), U32(file, at + 24), U32(file, at + 28), U32(file, at + 32), U32(file, at + 36)};
            if (!PowerOfTwo(s.alignment) || s.alignment > ArenaEnd ||
                (s.type != 0 && s.type != 8 && !InFile(file, s.offset, s.size))) return fail("invalid section range/alignment");
            if (s.flags & 2)
            {
                if ((s.flags & 0x400) || (s.type != 1 && s.type != 8 && s.type != 14 && s.type != 15 && s.type != 0x70000006 && s.type != 0x7000002a))
                    return fail("unsupported allocated section (TLS/dynamic runtime is not enabled)");
                cursor = Align(cursor, s.alignment);
                if (cursor + s.size > arenaEnd) return fail("module exceeds guest arena");
                s.placed = static_cast<uint32_t>(cursor);
                cursor += s.size;
            }
            if (s.type == 2)
            {
                if (symbolTable) return fail("multiple symbol tables");
                symbolTable = i;
            }
            image.sections.push_back(s);
        }
        if (image.sections[strings].type != 3 || !symbolTable) return fail("missing strings/symbols");
        for (const auto& s : image.sections)
        {
            std::string name;
            if (!String(file, image.sections[strings], s.name, name)) return fail("invalid section name");
            if ((s.flags & 2) && s.size && (name == ".ctors" || name == ".dtors"))
                return fail("legacy constructor arrays must be rebuilt with the module runtime");
        }
        image.bytes.resize(static_cast<size_t>(cursor - base), 0);
        for (const auto& s : image.sections)
            if ((s.flags & 2) && s.type != 8 && s.size)
                std::memcpy(image.bytes.data() + s.placed - base, file.data() + s.offset, s.size);
        const auto& symtab = image.sections[symbolTable];
        if (symtab.entrySize != 16 || symtab.size % 16 || symtab.link >= count ||
            image.sections[symtab.link].type != 3 || symtab.info > symtab.size / 16) return fail("invalid symbol table");
        struct Resolved { uint32_t value; bool defined; bool isSection; };
        std::vector<Resolved> resolved;
        for (uint32_t i = 0; i < symtab.size / 16; ++i)
        {
            const size_t at = symtab.offset + size_t(i) * 16;
            std::string name;
            if (!String(file, image.sections[symtab.link], U32(file, at), name)) return fail("invalid symbol name");
            const uint16_t index = U16(file, at + 14);
            const uint32_t value = U32(file, at + 4), size = U32(file, at + 8);
            const uint8_t binding = uint8_t(file[at + 12]) >> 4;
            uint32_t address = 0, flags = 0;
            bool defined = true;
            if (!index)
            {
                defined = i == 0 || binding == 2; // undefined weak resolves to zero
                if (!defined) return fail("unresolved runtime symbol: " + name);
            }
            else if (index == 0xfff1) address = value; // SHN_ABS
            else if (index >= count) return fail("unsupported common/reserved symbol; compile with -fno-common");
            else
            {
                const auto& owner = image.sections[index];
                if (uint64_t(value) + size > owner.size) return fail("symbol outside section: " + name);
                if (!(owner.flags & 2)) defined = false;
                else { address = owner.placed + value; flags = owner.flags; }
            }
            resolved.push_back({address, defined, (uint8_t(file[at + 12]) & 15) == 3});
            // Only externally visible symbols are part of the module lookup contract.
            if (binding != 0 && defined && flags && !name.empty())
            {
                if (!image.symbols.emplace(name, Symbol{address, size, flags}).second)
                    return fail("duplicate exported symbol: " + name);
            }
        }
        for (const auto& reloc : image.sections)
        {
            if (reloc.type != 9 && reloc.type != 4) continue;
            if (reloc.info >= count) return fail("invalid relocation target");
            const auto& target = image.sections[reloc.info];
            if (!(target.flags & 2)) continue; // debug relocations remain on disk
            const bool explicitAddend = reloc.type == 4;
            const uint32_t recordSize = explicitAddend ? 12 : 8;
            if (reloc.entrySize != recordSize || reloc.size % recordSize || reloc.link != symbolTable)
                return fail("unsupported/invalid relocation table");
            struct High { size_t at; uint32_t symbol, instruction; };
            std::vector<High> pending;
            for (uint32_t i = 0; i < reloc.size / recordSize; ++i)
            {
                const size_t at = reloc.offset + size_t(i) * recordSize;
                const uint32_t offset = U32(file, at), info = U32(file, at + 4), symbol = info >> 8, type = info & 255;
                if (symbol >= resolved.size()) return fail("invalid relocation symbol index");
                if (!type) continue; // R_MIPS_NONE
                // ELF metadata (including SDK .eh_frame) can contain packed
                // unaligned data pointers. Instruction relocations stay aligned.
                if ((offset % 4 && (type != 2 || (target.flags & 4))) ||
                    uint64_t(offset) + 4 > target.size || !resolved[symbol].defined)
                    return fail("invalid relocation location/symbol");
                const size_t patch = target.placed - base + size_t(offset);
                const uint32_t instruction = U32(image.bytes, patch), value = resolved[symbol].value;
                const int32_t addend = explicitAddend ? static_cast<int32_t>(U32(file, at + 8)) : 0;
                if (type == 2) Put(image.bytes, patch, value + (explicitAddend ? uint32_t(addend) : instruction)); // R_MIPS_32
                else if (type == 4) // R_MIPS_26
                {
                    const uint32_t encoded = (instruction & 0x03ffffff) << 2;
                    // MIPS REL function-symbol addends are signed 28-bit; section
                    // symbol addends are unsigned offsets (GNU BFD semantics).
                    const int64_t jumpAddend = explicitAddend ? int64_t(addend) :
                        (!resolved[symbol].isSection && (encoded & 0x08000000) ? int64_t(encoded) - 0x10000000 : int64_t(encoded));
                    const int64_t destination = int64_t(value) + jumpAddend;
                    const uint32_t place = target.placed + offset;
                    if ((instruction >> 26 != 2 && instruction >> 26 != 3) || destination < 0 || destination > UINT32_MAX ||
                        (destination & 3) || ((place + 4) & 0xf0000000) != (destination & 0xf0000000))
                        return fail("unreachable/malformed R_MIPS_26 jump");
                    Put(image.bytes, patch, (instruction & 0xfc000000) | (uint32_t(destination) >> 2 & 0x03ffffff));
                }
                else if (type == 5)
                {
                    if (explicitAddend)
                        Put(image.bytes, patch, (instruction & 0xffff0000) | ((uint64_t(uint32_t(value + addend)) + 0x8000) >> 16 & 0xffff));
                    else pending.push_back({patch, symbol, instruction}); // R_MIPS_HI16
                }
                else if (type == 6) // One LO16 can resolve several preceding HI16s for this symbol.
                {
                    const int32_t low = explicitAddend ? addend : int16_t(instruction & 0xffff);
                    for (auto it = pending.begin(); it != pending.end();)
                    {
                        if (it->symbol != symbol) { ++it; continue; }
                        const uint32_t full = value + ((it->instruction & 0xffff) << 16) + low;
                        Put(image.bytes, it->at, (it->instruction & 0xffff0000) | ((uint64_t(full) + 0x8000) >> 16 & 0xffff));
                        it = pending.erase(it);
                    }
                    Put(image.bytes, patch, (instruction & 0xffff0000) | ((value + low) & 0xffff));
                }
                else return fail("unsupported MIPS relocation: " + std::to_string(type));
            }
            if (!pending.empty()) return fail("R_MIPS_HI16 has no matching LO16");
        }
        for (const char* prefix : {"__init_array_", "__fini_array_"})
        {
            const auto* begin = image.find(std::string(prefix) + "start");
            const auto* end = image.find(std::string(prefix) + "end");
            if (!begin && !end) continue;
            if (!begin || !end || end->address < begin->address || begin->address % 4 || (end->address - begin->address) % 4 ||
                (end->address != begin->address && !image.contains(begin->address, end->address - begin->address)))
                return fail("invalid runtime constructor/destructor range");
            for (uint32_t at = begin->address; at < end->address; at += 4)
                if (image.word(at) % 4 || !image.contains(image.word(at), 4, 4))
                    return fail("invalid runtime constructor/destructor target");
        }
        const auto* descriptor = image.find("PCSX2FModule");
        if (!descriptor || descriptor->size != 32 || descriptor->address % 4 || !image.contains(descriptor->address, 32))
            return fail("missing versioned PCSX2FModule descriptor; update/rebuild this plugin");
        const uint32_t at = descriptor->address;
        if (image.word(at) != PCSX2F_MODULE_MAGIC || image.word(at + 4) != PCSX2F_MODULE_VERSION ||
            image.word(at + 8) != 32 || image.word(at + 12) || image.word(at + 28)) return fail("unsupported module ABI/flags");
        image.info.EntryPoint = image.word(at + 16);
        if (image.info.EntryPoint % 4 || !image.contains(image.info.EntryPoint, 4, 4)) return fail("invalid module entry point");
        const uint32_t stackSize = image.word(at + 20), heapSize = image.word(at + 24);
        if (stackSize < 16384 || stackSize > 1024 * 1024 || stackSize % 16 || heapSize > 16 * 1024 * 1024 || heapSize % 16)
            return fail("invalid module stack/heap request");
        cursor = Align(cursor, 16);
        image.context = static_cast<uint32_t>(cursor);
        cursor += 32;
        image.heapBegin = static_cast<uint32_t>(cursor);
        cursor += heapSize;
        image.heapEnd = static_cast<uint32_t>(cursor);
        cursor += stackSize;
        if (cursor > arenaEnd) return fail("module stack/heap exceed guest arena");
        image.stackTop = static_cast<uint32_t>(cursor);
        image.bytes.resize(static_cast<size_t>(cursor - base), 0);
        image.info.Size = static_cast<uint32_t>(image.bytes.size());
        Put(image.bytes, image.context - base, 32);
        Put(image.bytes, image.context - base + 4, PCSX2F_MODULE_VERSION);
        Put(image.bytes, image.context - base + 12, base);
        Put(image.bytes, image.context - base + 16, image.heapBegin);
        Put(image.bytes, image.context - base + 20, image.heapEnd);
        struct Buffer { const char* name; uint32_t PluginInfo::*addr; uint32_t PluginInfo::*size; uint32_t min, multiple, align; bool writable; };
        const Buffer buffers[] = {
            {"PluginData", &PluginInfo::PluginDataAddr, &PluginInfo::PluginDataSize, 4, 1, 4, true},
            {"PCSX2Data", &PluginInfo::PCSX2DataAddr, &PluginInfo::PCSX2DataSize, 4, 4, 4, true},
            {"CompatibleCRCList", &PluginInfo::CompatibleCRCListAddr, &PluginInfo::CompatibleCRCListSize, 4, 4, 4, false},
            {"CompatibleElfCRCList", &PluginInfo::CompatibleElfCRCListAddr, &PluginInfo::CompatibleElfCRCListSize, 4, 4, 4, false},
            {"KeyboardState", &PluginInfo::KeyboardStateAddr, &PluginInfo::KeyboardStateSize, 512, 1, 1, true},
            {"MouseState", &PluginInfo::MouseStateAddr, &PluginInfo::MouseStateSize, 40, 1, 4, true},
            {"CheatString", &PluginInfo::CheatStringAddr, &PluginInfo::CheatStringSize, 2, 1, 1, true},
            {"OSDText", &PluginInfo::OSDTextAddr, &PluginInfo::OSDTextSize, 255, 255, 1, true},
            {"FrameLimitUnthrottle", &PluginInfo::FrameLimitUnthrottleAddr, &PluginInfo::FrameLimitUnthrottleSize, 1, 1, 1, true},
            {"CLEOScripts", &PluginInfo::CLEOScriptsAddr, &PluginInfo::CLEOScriptsSize, 1, 1, 1, true}};
        for (const auto& buffer : buffers)
        {
            const auto* symbol = image.find(buffer.name);
            if (!symbol) continue;
            if (symbol->size < buffer.min || symbol->size % buffer.multiple || symbol->address % buffer.align ||
                !image.contains(symbol->address, symbol->size, buffer.writable ? 1 : 0))
                return fail(std::string("invalid exported buffer: ") + buffer.name);
            image.info.*buffer.addr = symbol->address;
            image.info.*buffer.size = symbol->size;
        }
        if (!image.info.CompatibleCRCListAddr)
            return fail("missing CompatibleCRCList game restriction");
        error.clear();
        output = std::move(image);
        return true;
    }
}

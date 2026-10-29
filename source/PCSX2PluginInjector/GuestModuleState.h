#pragma once
#include <bcrypt.h>
#include <filesystem>
#include <span>
#include <stdexcept>
#pragma comment(lib, "bcrypt.lib")

namespace GuestPluginState
{
    constexpr uint32_t Version = 2, MaxBytes = 128 * 1024 * 1024, MaxElfBytes = 32 * 1024 * 1024;
    struct Record
    {
        std::string name, hash;
        uint32_t base = 0, size = 0, entry = 0;
        std::vector<char> file;
    };
    struct Manifest
    {
        uint32_t disc = 0, elf = 0;
        std::vector<Record> modules;
        uint32_t version = Version;
    };
    inline std::string Hash(const std::vector<char>& bytes)
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        std::array<uint8_t, 32> result{};
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            throw std::runtime_error("Plugin SHA-256 provider unavailable");
        NTSTATUS status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
        if (status >= 0) status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0);
        if (status >= 0) status = BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0);
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        if (status < 0) throw std::runtime_error("Plugin SHA-256 failed");
        static constexpr char hex[] = "0123456789abcdef";
        std::string text;
        for (uint8_t byte : result) { text += hex[byte >> 4]; text += hex[byte & 15]; }
        return text;
    }
    class Archive
    {
        std::span<const uint8_t> input;
        size_t position = 0;
    public:
        std::vector<uint8_t> output;
        explicit Archive(std::span<const uint8_t> bytes = {}) : input(bytes) {}
        uint32_t Read()
        {
            if (input.size() - position < 4) throw std::runtime_error("Truncated plugin manifest");
            uint32_t value = 0;
            for (unsigned i = 0; i < 4; ++i) value |= uint32_t(input[position++]) << (8 * i);
            return value;
        }
        std::string ReadString(uint32_t limit)
        {
            const uint32_t size = Read();
            if (!size || size > limit || size > input.size() - position) throw std::runtime_error("Invalid plugin manifest string");
            std::string value(reinterpret_cast<const char*>(input.data() + position), size);
            position += size;
            if (value.find('\0') != std::string::npos) throw std::runtime_error("Invalid plugin name");
            return value;
        }
        void Write(uint32_t value) { for (unsigned i = 0; i < 4; ++i) output.push_back(uint8_t(value >> (8 * i))); }
        std::vector<char> ReadFile()
        {
            const uint32_t size = Read();
            if (!size || size > MaxElfBytes || size > input.size() - position)
                throw std::runtime_error("Invalid saved plugin ELF size");
            std::vector<char> bytes(input.begin() + position, input.begin() + position + size);
            position += size;
            return bytes;
        }
        void Write(const std::string& value)
        {
            Write(static_cast<uint32_t>(value.size()));
            output.insert(output.end(), value.begin(), value.end());
        }
        bool End() const { return position == input.size(); }
    };
    inline std::vector<uint8_t> Encode(const Manifest& manifest)
    {
        Archive ar;
        ar.Write(manifest.version); ar.Write(manifest.disc); ar.Write(manifest.elf);
        ar.Write(static_cast<uint32_t>(manifest.modules.size()));
        for (const auto& module : manifest.modules)
        {
            ar.Write(module.name); ar.Write(module.hash);
            ar.Write(module.base); ar.Write(module.size); ar.Write(module.entry);
            if (manifest.version >= 2)
            {
                if (module.file.empty() || module.file.size() > MaxElfBytes ||
                    ar.output.size() + 4 + module.file.size() > MaxBytes)
                    throw std::runtime_error("Saved plugin ELF exceeds state bounds");
                ar.Write(static_cast<uint32_t>(module.file.size()));
                ar.output.insert(ar.output.end(), module.file.begin(), module.file.end());
            }
        }
        return std::move(ar.output);
    }
    inline Manifest Decode(std::span<const uint8_t> bytes)
    {
        if (bytes.size() > MaxBytes) throw std::runtime_error("Plugin manifest too large");
        Archive ar(bytes);
        Manifest manifest;
        manifest.version = ar.Read();
        if (manifest.version != 1 && manifest.version != Version) throw std::runtime_error("Unsupported plugin manifest version");
        manifest.disc = ar.Read(); manifest.elf = ar.Read();
        const uint32_t count = ar.Read();
        if (!count || count > 256) throw std::runtime_error("Invalid plugin count");
        for (uint32_t i = 0; i < count; ++i)
        {
            Record record;
            record.name = ar.ReadString(4096); record.hash = ar.ReadString(64);
            record.base = ar.Read(); record.size = ar.Read(); record.entry = ar.Read();
            // Names are sidecar paths only. Never allow a state to escape PLUGINS.
            const auto path = std::filesystem::path(std::u8string(record.name.begin(), record.name.end()));
            if (path.has_root_path() || record.name.find(':') != std::string::npos ||
                record.name.find('\\') != std::string::npos ||
                std::any_of(path.begin(), path.end(), [](const auto& part) { return part == ".." || part == "."; }))
                throw std::runtime_error("Invalid saved plugin path");
            if (manifest.version >= 2)
            {
                record.file = ar.ReadFile();
                if (Hash(record.file) != record.hash) throw std::runtime_error("Saved plugin ELF hash mismatch: " + record.name);
            }
            if (record.hash.size() != 64 || record.base < GuestModule::ArenaBegin ||
                !record.size || uint64_t(record.base) + record.size > GuestModule::ArenaEnd)
                throw std::runtime_error("Invalid plugin manifest range: " + record.name);
            for (const auto& other : manifest.modules)
                if (other.name == record.name || (record.base < uint64_t(other.base) + other.size && other.base < uint64_t(record.base) + record.size))
                    throw std::runtime_error("Duplicate or overlapping plugin: " + record.name);
            manifest.modules.push_back(std::move(record));
        }
        if (!ar.End()) throw std::runtime_error("Trailing plugin manifest data");
        return manifest;
    }
}

#pragma once
#include "../API/plugin_settings.h"
#include "../API/plugin_settings_batch.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace PluginSettings
{
inline std::string_view Trim(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r");
    if (first == value.npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}
inline bool Equal(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a'-'A') : c; };
        return lower(x) == lower(y);
    });
}
inline bool Valid(const char (&value)[64], bool name)
{
    const char* end = static_cast<const char*>(std::memchr(value, 0, sizeof(value)));
    if (!end || (name && (end == value || Trim({value, size_t(end-value)}).size() != size_t(end-value)))) return false;
    for (auto p = value; p != end; ++p)
        if (static_cast<unsigned char>(*p) < 32 || (name && std::strchr("[]=;#", *p))) return false;
    return true;
}
// Update existing keys in place, retaining comments, unrelated keys and ordering.
// Duplicate keys are updated together so first/last-key INI readers agree.
inline bool Update(std::string& text, const PCSX2FIniEntry& entry, bool write, std::string& value)
{
    bool inSection = false, seenSection = false, found = false;
    size_t insertion = text.size();
    for (size_t at = 0; at < text.size();)
    {
        const size_t newline = text.find('\n', at);
        const size_t end = newline == text.npos ? text.size() : newline;
        auto line = Trim(std::string_view(text).substr(at, end-at));
        if (at == 0 && line.substr(0,3) == "\xEF\xBB\xBF") line.remove_prefix(3);
        const size_t next = newline == text.npos ? text.size() : newline+1;
        if (line.size() >= 2 && line.front() == '[' && line.back() == ']')
        {
            inSection = Equal(Trim(line.substr(1,line.size()-2)), entry.section);
            if (inSection) { seenSection = true; insertion = next; }
        }
        else if (inSection && !line.empty())
            insertion = next; // New keys follow the section's last non-blank line.
        if (inSection && !line.empty() && line.front() != '[' && line.front() != ';' && line.front() != '#')
        {
            const auto equals = line.find('=');
            if (equals != line.npos && Equal(Trim(line.substr(0,equals)), entry.key))
            {
                if (!write) { value = Trim(line.substr(equals+1)); return true; }
                size_t start = size_t(line.data() - text.data()) + equals + 1;
                while (start < end && (text[start] == ' ' || text[start] == '\t')) ++start;
                size_t valueEnd = end;
                if (valueEnd > start && text[valueEnd-1] == '\r') --valueEnd;
                const size_t length = std::strlen(entry.value), oldLength = valueEnd-start;
                text.replace(start, oldLength, entry.value);
                at = end + length - oldLength + (newline == text.npos ? 0 : 1);
                found = true;
                continue;
            }
        }
        at = next;
    }
    if (!write || found) return found;
    if (!seenSection)
    {
        if (!text.empty() && text.back() != '\n') text += "\r\n";
        text += "\r\n["; text += entry.section; text += "]\r\n";
        insertion = text.size();
    }
    const std::string separator = insertion && text[insertion-1] != '\n' ? "\r\n" : "";
    text.insert(insertion, separator + entry.key + " = " + entry.value + "\r\n");
    return true;
}
inline bool ReadText(HANDLE file, std::string& text)
{
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 1024 * 1024)
        return false;
    text.resize(size_t(size.QuadPart));
    DWORD read = 0;
    return text.empty() || (ReadFile(file, text.data(), DWORD(text.size()), &read, nullptr) && read == text.size());
}

inline bool ReadText(const std::filesystem::path& path, std::string& text, bool& exists)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    exists = file != INVALID_HANDLE_VALUE;
    if (!exists)
        return GetLastError() == ERROR_FILE_NOT_FOUND;

    const std::unique_ptr<void, decltype(&CloseHandle)> close(file, CloseHandle);
    return ReadText(file, text);
}

inline uint32_t CreateText(const std::filesystem::path& path, const std::string& text)
{
    static uint64_t sequence = 0;
    auto temporary = path;
    temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++sequence);
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return PCSX2F_SETTINGS_IO_ERROR;

    DWORD written = 0;
    const bool saved = WriteFile(file, text.data(), DWORD(text.size()), &written, nullptr) &&
                       written == text.size() && FlushFileBuffers(file);
    const bool closed = CloseHandle(file) != FALSE;
    if (saved && closed && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH))
        return PCSX2F_SETTINGS_OK;
    DeleteFileW(temporary.c_str());
    return PCSX2F_SETTINGS_IO_ERROR;
}

inline bool WriteText(HANDLE file, const std::string& text)
{
    LARGE_INTEGER zero{};
    DWORD written = 0;
    return SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) &&
        WriteFile(file, text.data(), DWORD(text.size()), &written, nullptr) &&
        written == text.size() && SetEndOfFile(file) && FlushFileBuffers(file);
}

inline uint32_t WriteEntries(const std::filesystem::path& path, const PCSX2FIniRequest& request)
{
    // Keep the same file open from read through commit. A concurrent writer or
    // replacement must finish before this open or wait until after the commit.
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool exists = file != INVALID_HANDLE_VALUE;
    if (!exists && GetLastError() != ERROR_FILE_NOT_FOUND)
        return PCSX2F_SETTINGS_IO_ERROR;
    const std::unique_ptr<void, decltype(&CloseHandle)> close(exists ? file : nullptr, CloseHandle);
    std::string original;
    if (exists && !ReadText(file, original))
        return PCSX2F_SETTINGS_IO_ERROR;
    auto edited = original;
    for (uint32_t i = 0; i < request.count; ++i)
    {
        std::string ignored;
        Update(edited, request.entries[i], true, ignored);
    }
    if (!exists)
        return CreateText(path, edited);
    if (edited == original || WriteText(file, edited))
        return PCSX2F_SETTINGS_OK;
    WriteText(file, original);
    return PCSX2F_SETTINGS_IO_ERROR;
}

inline bool ReadValue(std::string& text, PCSX2FIniEntry& entry, bool& present)
{
    std::string value;
    present = Update(text, entry, false, value);
    if (value.size() >= sizeof(entry.value))
        return false;
    std::memset(entry.value, 0, sizeof(entry.value));
    std::memcpy(entry.value, value.data(), value.size());
    return true;
}

inline uint32_t Process(const std::filesystem::path& path, PCSX2FIniRequest& request)
{
    if (request.size != sizeof(request) || request.version != 1 || request.operation > PCSX2F_SETTINGS_WRITE ||
        !request.count || request.count > PCSX2F_SETTINGS_MAX_ENTRIES) return PCSX2F_SETTINGS_INVALID;
    const bool writing = request.operation == PCSX2F_SETTINGS_WRITE;
    for (uint32_t i=0; i<request.count; ++i)
        if (!Valid(request.entries[i].section,true) || !Valid(request.entries[i].key,true) ||
            (writing && !Valid(request.entries[i].value,false))) return PCSX2F_SETTINGS_INVALID;

    if (writing)
        return WriteEntries(path, request);

    bool exists = false;
    std::string text;
    if (!ReadText(path, text, exists))
        return PCSX2F_SETTINGS_IO_ERROR;
    for (uint32_t i = 0; i < request.count; ++i)
    {
        bool present = false;
        if (!ReadValue(text, request.entries[i], present))
            return PCSX2F_SETTINGS_INVALID;
        if (!present)
            return PCSX2F_SETTINGS_NOT_FOUND;
    }
    return PCSX2F_SETTINGS_OK;
}

inline uint32_t Process(const std::filesystem::path& path, PCSX2FSettingsBatch::Request& request)
{
    namespace Batch = PCSX2FSettingsBatch;
    if (request.size != sizeof(request) || request.version != Batch::Version ||
        request.operation > Batch::Seed || request.count > Batch::MaxEntries)
        return PCSX2F_SETTINGS_INVALID;

    const bool seeding = request.operation == Batch::Seed;
    if (!request.count)
        return seeding ? PCSX2F_SETTINGS_INVALID : PCSX2F_SETTINGS_OK;
    for (uint32_t i = 0; i < request.count; ++i)
    {
        const auto& entry = request.entries[i];
        if (!Valid(entry.section, true) || !Valid(entry.key, true) || (seeding && !Valid(entry.value, false)))
            return PCSX2F_SETTINGS_INVALID;
    }

    std::string text;
    bool exists = false;
    if (!ReadText(path, text, exists))
        return PCSX2F_SETTINGS_IO_ERROR;
    if (seeding)
    {
        // An external editor can replace the file independently, so startup
        // creates only a new CFG. Explicit values in an existing file win.
        if (exists)
            return Batch::SeedSkipped;
        for (uint32_t i = 0; i < request.count; ++i)
        {
            std::string ignored;
            Update(text, request.entries[i], true, ignored);
        }
        return CreateText(path, text);
    }

    request.present[0] = request.present[1] = 0;
    for (uint32_t i = 0; i < request.count; ++i)
    {
        bool present = false;
        if (!ReadValue(text, request.entries[i], present))
            return PCSX2F_SETTINGS_INVALID;
        if (present)
            request.mark(i);
    }
    return PCSX2F_SETTINGS_OK;
}

}

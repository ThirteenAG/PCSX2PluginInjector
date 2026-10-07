#pragma once
#include "../API/plugin_settings.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
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
inline uint32_t Process(const std::filesystem::path& path, PCSX2FIniRequest& request)
{
    if (request.size != sizeof(request) || request.version != 1 || request.operation > PCSX2F_SETTINGS_WRITE ||
        !request.count || request.count > PCSX2F_SETTINGS_MAX_ENTRIES) return PCSX2F_SETTINGS_INVALID;
    for (uint32_t i=0; i<request.count; ++i)
        if (!Valid(request.entries[i].section,true) || !Valid(request.entries[i].key,true) ||
            (request.operation == PCSX2F_SETTINGS_WRITE && !Valid(request.entries[i].value,false))) return PCSX2F_SETTINGS_INVALID;
    std::error_code error;
    const bool exists = std::filesystem::exists(path,error);
    if (error) return PCSX2F_SETTINGS_IO_ERROR;
    std::string text;
    if (exists)
    {
        const auto size = std::filesystem::file_size(path,error);
        if (error || size > 1024*1024) return PCSX2F_SETTINGS_IO_ERROR;
        std::ifstream file(path,std::ios::binary);
        if (!file) return PCSX2F_SETTINGS_IO_ERROR;
        text.resize(size_t(size));
        if (!text.empty() && !file.read(text.data(),std::streamsize(text.size()))) return PCSX2F_SETTINGS_IO_ERROR;
    }
    for (uint32_t i=0; i<request.count; ++i)
    {
        auto& entry = request.entries[i];
        std::string value;
        if (!Update(text,entry,request.operation == PCSX2F_SETTINGS_WRITE,value)) return PCSX2F_SETTINGS_NOT_FOUND;
        if (request.operation == PCSX2F_SETTINGS_READ)
        {
            if (value.size() >= sizeof(entry.value)) return PCSX2F_SETTINGS_INVALID;
            std::memset(entry.value,0,sizeof(entry.value));
            std::memcpy(entry.value,value.data(),value.size());
        }
    }
    if (request.operation == PCSX2F_SETTINGS_READ) return PCSX2F_SETTINGS_OK;
    // Stage the complete batch before replacing the original. CREATE_NEW prevents
    // taking over an existing temporary file; failures never truncate the INI.
    static uint64_t sequence = 0;
    auto temporary = path;
    temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++sequence);
    HANDLE file = CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file == INVALID_HANDLE_VALUE) return PCSX2F_SETTINGS_IO_ERROR;
    DWORD written = 0;
    const bool saved = WriteFile(file,text.data(),DWORD(text.size()),&written,nullptr) && written == text.size() && FlushFileBuffers(file);
    const bool closed = CloseHandle(file) != FALSE;
    if (saved && closed && MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return PCSX2F_SETTINGS_OK;
    DeleteFileW(temporary.c_str());
    return PCSX2F_SETTINGS_IO_ERROR;
}
}

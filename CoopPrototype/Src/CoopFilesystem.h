#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

// The CRT narrow environment and filesystem conversions use the active code
// page on Windows. That loses usernames which cannot be represented by it.
// Keep paths native while constructing them, and use UTF-8 only when a path
// has to cross an existing std::string/game API boundary.
namespace CoopFilesystem
{
inline std::wstring Utf8ToWide(std::string_view value)
{
#ifdef _WIN32
    if (value.empty())
        return {};

    const int inputLength = static_cast<int>(value.size());
    int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), inputLength, nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (required <= 0)
    {
        // Paths received from the game may still be encoded with the active
        // Windows code page. Accept that legacy boundary while all generated
        // paths use UTF-8 below.
        codePage = CP_ACP;
        flags = 0;
        required = MultiByteToWideChar(codePage, flags, value.data(), inputLength, nullptr, 0);
    }
    if (required <= 0)
        return {};

    std::wstring result(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(codePage, flags, value.data(), inputLength, result.data(), required) <= 0)
        return {};
    return result;
#else
    return std::wstring(value.begin(), value.end());
#endif
}

inline std::string WideToUtf8(std::wstring_view value)
{
#ifdef _WIN32
    if (value.empty())
        return {};

    const int inputLength = static_cast<int>(value.size());
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        inputLength,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0)
        return {};

    std::string result(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            inputLength,
            result.data(),
            required,
            nullptr,
            nullptr) <= 0)
    {
        return {};
    }
    return result;
#else
    return std::string(value.begin(), value.end());
#endif
}

inline std::filesystem::path FromUtf8(std::string_view value)
{
#ifdef _WIN32
    return std::filesystem::path(Utf8ToWide(value));
#else
    return std::filesystem::path(std::string(value));
#endif
}

inline std::string ToUtf8(const std::filesystem::path& value)
{
#ifdef _WIN32
    return WideToUtf8(value.native());
#else
    return value.string();
#endif
}

inline std::filesystem::path EnvironmentPath(const char* name)
{
    if (!name || !name[0])
        return {};

#ifdef _WIN32
    std::wstring wideName;
    for (const unsigned char* cursor = reinterpret_cast<const unsigned char*>(name); *cursor; ++cursor)
        wideName.push_back(static_cast<wchar_t>(*cursor));

    DWORD capacity = 256;
    for (;;)
    {
        std::wstring value(static_cast<size_t>(capacity), L'\0');
        const DWORD length = GetEnvironmentVariableW(wideName.c_str(), value.data(), capacity);
        if (length == 0)
            break;
        // A successful query returns the character count excluding the null
        // terminator; a truncated query returns the required size instead.
        if (length < capacity)
        {
            value.resize(length);
            return std::filesystem::path(value);
        }
        capacity = length + 1;
    }
#endif

    const char* value = std::getenv(name);
    return value && value[0] ? FromUtf8(value) : std::filesystem::path();
}
}

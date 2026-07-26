#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace
{
struct WindowInfo
{
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::string title;
    std::string className;
    LONG area = 0;
    bool visible = false;
};

std::string ReadWindowText(HWND hwnd)
{
    char buffer[512] = {};
    GetWindowTextA(hwnd, buffer, static_cast<int>(sizeof(buffer)));
    return buffer;
}

std::string ReadClassName(HWND hwnd)
{
    char buffer[256] = {};
    GetClassNameA(hwnd, buffer, static_cast<int>(sizeof(buffer)));
    return buffer;
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
    {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool LooksLikePreyWindow(const WindowInfo& info)
{
    const std::string title = LowerAscii(info.title);
    const std::string className = LowerAscii(info.className);
    return title.find("prey") != std::string::npos ||
        title.find("prey.exe") != std::string::npos ||
        className.find("prey") != std::string::npos ||
        className.find("steam_app_480490") != std::string::npos;
}

int ScorePreyGameWindow(const WindowInfo& info)
{
    if (!LooksLikePreyWindow(info))
        return 0;

    const std::string title = LowerAscii(info.title);
    const std::string className = LowerAscii(info.className);

    int score = 100;
    if (title == "prey")
        score += 10000;
    if (className.find("steam_app_480490") != std::string::npos)
        score += 8000;
    if (className.find("prey") != std::string::npos)
        score += 6000;

    // The Chairloader console often has a long path ending in Prey.exe. It is
    // useful to defocus, but it should not win activation over the game window.
    if (title.find("prey.exe") != std::string::npos || title.find("\\") != std::string::npos || title.find("/") != std::string::npos)
        score -= 3000;

    score += static_cast<int>(std::min<LONG>(info.area / 100000, 200));
    return score;
}

WindowInfo DescribeWindow(HWND hwnd)
{
    WindowInfo info;
    info.hwnd = hwnd;
    info.visible = IsWindow(hwnd) && IsWindowVisible(hwnd);
    GetWindowThreadProcessId(hwnd, &info.pid);
    info.title = ReadWindowText(hwnd);
    info.className = ReadClassName(hwnd);

    RECT rect = {};
    if (GetWindowRect(hwnd, &rect))
    {
        const LONG width = std::max<LONG>(0, rect.right - rect.left);
        const LONG height = std::max<LONG>(0, rect.bottom - rect.top);
        info.area = width * height;
    }
    return info;
}

std::vector<WindowInfo> EnumerateWindows()
{
    std::vector<WindowInfo> windows;
    EnumWindows(
        [](HWND hwnd, LPARAM raw) -> BOOL
        {
            auto* out = reinterpret_cast<std::vector<WindowInfo>*>(raw);
            WindowInfo info = DescribeWindow(hwnd);
            if (info.visible && info.area > 100000)
                out->push_back(info);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows));
    return windows;
}

HWND FindBestPreyWindow()
{
    std::vector<WindowInfo> windows = EnumerateWindows();
    HWND best = nullptr;
    int bestScore = 0;
    for (const WindowInfo& info : windows)
    {
        const int score = ScorePreyGameWindow(info);
        if (score > bestScore)
        {
            bestScore = score;
            best = info.hwnd;
        }
    }
    return best;
}

void PrintWindow(const char* prefix, HWND hwnd)
{
    WindowInfo info = DescribeWindow(hwnd);
    std::printf(
        "%s hwnd=0x%p pid=%lu visible=%d area=%ld prey=%d score=%d class=\"%s\" title=\"%s\"\n",
        prefix,
        hwnd,
        static_cast<unsigned long>(info.pid),
        info.visible ? 1 : 0,
        static_cast<long>(info.area),
        LooksLikePreyWindow(info) ? 1 : 0,
        ScorePreyGameWindow(info),
        info.className.c_str(),
        info.title.c_str());
}

bool WaitForegroundDifferent(HWND oldForeground, DWORD timeoutMs)
{
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs)
    {
        HWND foreground = GetForegroundWindow();
        if (!oldForeground || foreground != oldForeground)
            return true;
        Sleep(50);
    }
    return GetForegroundWindow() != oldForeground;
}

bool WaitForegroundEquals(HWND target, DWORD timeoutMs)
{
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs)
    {
        if (GetForegroundWindow() == target)
            return true;
        Sleep(50);
    }
    return GetForegroundWindow() == target;
}

bool WaitForegroundNotPrey(DWORD timeoutMs)
{
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs)
    {
        WindowInfo info = DescribeWindow(GetForegroundWindow());
        if (!LooksLikePreyWindow(info))
            return true;
        Sleep(50);
    }

    WindowInfo info = DescribeWindow(GetForegroundWindow());
    return !LooksLikePreyWindow(info);
}

DWORD ParseTimeout(const char* value, DWORD fallback)
{
    if (!value || !value[0])
        return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value)
        return fallback;
    return static_cast<DWORD>(std::clamp<unsigned long>(parsed, 50, 30000));
}

bool ActivatePreyWindow(HWND prey, DWORD timeoutMs)
{
    if (!prey)
        return false;

    ShowWindow(prey, SW_RESTORE);
    SetForegroundWindow(prey);
    SetFocus(prey);
    return WaitForegroundEquals(prey, timeoutMs);
}

bool ResolveVirtualKey(const std::string& token, WORD& virtualKey)
{
    const std::string key = LowerAscii(token);
    if (key == "shift") virtualKey = VK_SHIFT;
    else if (key == "ctrl" || key == "control") virtualKey = VK_CONTROL;
    else if (key == "alt") virtualKey = VK_MENU;
    else if (key == "space") virtualKey = VK_SPACE;
    else if (key == "esc" || key == "escape") virtualKey = VK_ESCAPE;
    else if (key == "enter" || key == "return") virtualKey = VK_RETURN;
    else if (key == "tab") virtualKey = VK_TAB;
    else if (key == "up") virtualKey = VK_UP;
    else if (key == "down") virtualKey = VK_DOWN;
    else if (key == "left") virtualKey = VK_LEFT;
    else if (key == "right") virtualKey = VK_RIGHT;
    else if (key.size() == 1 && std::isalnum(static_cast<unsigned char>(key[0])))
        virtualKey = static_cast<WORD>(std::toupper(static_cast<unsigned char>(key[0])));
    else
        return false;
    return true;
}

bool ParseKeySpec(const char* value, std::vector<WORD>& virtualKeys)
{
    if (!value || !value[0])
        return false;

    std::stringstream stream(value);
    std::string token;
    while (std::getline(stream, token, '+'))
    {
        WORD virtualKey = 0;
        if (!ResolveVirtualKey(token, virtualKey))
            return false;
        virtualKeys.push_back(virtualKey);
    }
    return !virtualKeys.empty();
}

bool SendPreyKeys(HWND prey, const char* spec, DWORD holdMs)
{
    std::vector<WORD> virtualKeys;
    if (!ParseKeySpec(spec, virtualKeys) || !ActivatePreyWindow(prey, 2500))
        return false;

    std::vector<INPUT> inputs;
    inputs.reserve(virtualKeys.size());
    for (WORD virtualKey : virtualKeys)
    {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = virtualKey;
        inputs.push_back(input);
    }
    if (SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) != inputs.size())
        return false;

    Sleep(holdMs);
    inputs.clear();
    for (auto it = virtualKeys.rbegin(); it != virtualKeys.rend(); ++it)
    {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = *it;
        input.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(input);
    }
    return SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) == inputs.size();
}

bool CapturePreyWindow(HWND prey, const char* path, LONG& width, LONG& height)
{
    if (!prey || !path || !path[0])
        return false;

    RECT clientRect = {};
    if (!GetClientRect(prey, &clientRect))
        return false;
    width = clientRect.right - clientRect.left;
    height = clientRect.bottom - clientRect.top;
    if (width <= 0 || height <= 0)
        return false;

    HDC source = GetDC(prey);
    HDC destination = source ? CreateCompatibleDC(source) : nullptr;
    HBITMAP bitmap = source ? CreateCompatibleBitmap(source, width, height) : nullptr;
    HGDIOBJ previous = destination && bitmap ? SelectObject(destination, bitmap) : nullptr;
    bool copied = destination && bitmap &&
        BitBlt(destination, 0, 0, width, height, source, 0, 0, SRCCOPY | CAPTUREBLT);

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const size_t pixelBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    std::vector<uint8_t> pixels(pixelBytes);
    copied = copied && GetDIBits(
        destination,
        bitmap,
        0,
        static_cast<UINT>(height),
        pixels.data(),
        &info,
        DIB_RGB_COLORS) == static_cast<int>(height);

    BITMAPFILEHEADER fileHeader = {};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(pixelBytes);

    bool written = false;
    if (copied)
    {
        if (FILE* file = std::fopen(path, "wb"))
        {
            written =
                std::fwrite(&fileHeader, sizeof(fileHeader), 1, file) == 1 &&
                std::fwrite(&info.bmiHeader, sizeof(info.bmiHeader), 1, file) == 1 &&
                std::fwrite(pixels.data(), pixels.size(), 1, file) == 1;
            std::fclose(file);
        }
    }

    if (previous)
        SelectObject(destination, previous);
    if (bitmap)
        DeleteObject(bitmap);
    if (destination)
        DeleteDC(destination);
    if (source)
        ReleaseDC(prey, source);
    return written;
}

}

int main(int argc, char** argv)
{
    const char* command = argc >= 2 ? argv[1] : "foreground";
    const DWORD timeoutMs = argc >= 3 ? ParseTimeout(argv[2], 2500) : 2500;

    if (std::strcmp(command, "foreground") == 0)
    {
        PrintWindow("foreground", GetForegroundWindow());
        return 0;
    }

    if (std::strcmp(command, "list") == 0)
    {
        std::vector<WindowInfo> windows = EnumerateWindows();
        for (const WindowInfo& info : windows)
            PrintWindow("window", info.hwnd);
        return 0;
    }

    if (std::strcmp(command, "is-prey-foreground") == 0)
    {
        HWND foreground = GetForegroundWindow();
        WindowInfo info = DescribeWindow(foreground);
        PrintWindow("foreground", foreground);
        return LooksLikePreyWindow(info) ? 0 : 1;
    }

    if (std::strcmp(command, "minimize-foreground-prey") == 0)
    {
        HWND foreground = GetForegroundWindow();
        WindowInfo info = DescribeWindow(foreground);
        PrintWindow("before", foreground);
        if (!foreground || !LooksLikePreyWindow(info))
            return 0;

        ShowWindow(foreground, SW_MINIMIZE);
        const bool defocused = WaitForegroundDifferent(foreground, timeoutMs);
        PrintWindow("after", GetForegroundWindow());
        return defocused ? 0 : 2;
    }

    if (std::strcmp(command, "wait-not-prey-foreground") == 0)
    {
        PrintWindow("before", GetForegroundWindow());
        const bool defocused = WaitForegroundNotPrey(timeoutMs);
        PrintWindow("after", GetForegroundWindow());
        return defocused ? 0 : 2;
    }

    if (std::strcmp(command, "activate-prey") == 0)
    {
        HWND prey = FindBestPreyWindow();
        PrintWindow("target", prey);
        if (!prey)
            return 1;

        const bool focused = ActivatePreyWindow(prey, timeoutMs);
        PrintWindow("after", GetForegroundWindow());
        return focused ? 0 : 2;
    }

    if (std::strcmp(command, "keys-prey") == 0)
    {
        HWND prey = FindBestPreyWindow();
        PrintWindow("target", prey);
        if (!prey || argc < 3)
            return 1;
        const DWORD holdMs = argc >= 4 ? ParseTimeout(argv[3], 150) : 150;
        const bool sent = SendPreyKeys(prey, argv[2], holdMs);
        std::printf("keys spec=\"%s\" holdMs=%lu sent=%d\n", argv[2], static_cast<unsigned long>(holdMs), sent ? 1 : 0);
        return sent ? 0 : 2;
    }

    if (std::strcmp(command, "capture-prey") == 0)
    {
        HWND prey = FindBestPreyWindow();
        PrintWindow("target", prey);
        if (!prey || argc < 3)
            return 1;
        LONG width = 0;
        LONG height = 0;
        const bool captured = CapturePreyWindow(prey, argv[2], width, height);
        std::printf("capture path=\"%s\" width=%ld height=%ld captured=%d\n", argv[2], width, height, captured ? 1 : 0);
        return captured ? 0 : 2;
    }

    std::fprintf(stderr, "usage: CoopWineFocusCtl.exe [foreground|list|is-prey-foreground|minimize-foreground-prey [ms]|wait-not-prey-foreground [ms]|activate-prey [ms]|keys-prey <KEY[+KEY...]> [hold-ms]|capture-prey <path.bmp>]\n");
    return 64;
}

#pragma once
#include <windows.h>
#include <string_view>

namespace bone_eater::render::window_exit_protocol {
// Scalar-only x64 protocol. This cookie identifies a particular attachment;
// it is not an authorization boundary against another process on the desktop.
inline constexpr wchar_t property[] = L"BoneEater.MainExit.v1.Cookie.55E0D171";
inline constexpr wchar_t message[] = L"BoneEater.MainExit.v1.Request.55E0D171";
inline constexpr wchar_t windowClass[] = L"AskaWnd";
inline constexpr LPARAM version = 1;
inline constexpr LRESULT accepted = 0x42454331;
static_assert(sizeof(UINT_PTR) == 8, "The game and companion must both be x64");

inline bool mainTitle(std::wstring_view title) noexcept {
    if (title == L"ASKA") return true;
    // Native string RVA 0x6FA528: ASKA (x%4.2f : <localized double-click hint>).
    // The cookie is the attachment identity; the suffix is not an auth token.
    if (!title.starts_with(L"ASKA (x") || !title.ends_with(L")") || title.size() > 255) return false;
    const auto separator = title.find(L" : ", 7);
    if (separator == std::wstring_view::npos || separator + 3 >= title.size() - 1) return false;
    bool digit = false, dot = false;
    for (const auto ch : title.substr(7, separator - 7)) {
        if (ch == L' ' && !digit && !dot) continue;
        if (ch >= L'0' && ch <= L'9') { digit = true; continue; }
        if (ch == L'.' && !dot) { dot = true; continue; }
        return false;
    }
    for (const auto ch : title.substr(separator + 3, title.size() - separator - 4))
        if (ch < 32) return false;
    return digit;
}
} // namespace bone_eater::render::window_exit_protocol

#pragma once

#include "external/rapidjson/document.h"

#include <cstddef>
#include <cstring>
#include <string_view>

namespace bone_eater::input {

enum class SelectedGunApiFields { Buttons, Analogs };
enum class SelectedGunApiCheck { Allowed, GunOwned, Malformed };

inline bool selectedGunApiField(SelectedGunApiFields fields, std::string_view name) noexcept {
    if (fields == SelectedGunApiFields::Buttons)
        return name == "Gun Pressed" || name == "Scope Right" || name == "Scope Left";
    if (fields == SelectedGunApiFields::Analogs) return name == "Gun X" || name == "Gun Y";
    return false;
}

// Call only when the immutable launch mode exclusively owns the selected gun,
// and BEFORE the existing API request's first override mutation. Matching this
// API's numeric acceptance preserves unrelated control behavior. Embedded NULs
// are rejected: upstream later converts GetString() to std::string and would
// otherwise turn "Gun X\0suffix" into a gun-field write after a length-aware check.
// No partial request is applied by this pure preflight; only Allowed may enter
// the upstream mutation loop. Reset is deliberately a separate cleanup path.
inline SelectedGunApiCheck preflightSelectedGunWrite(const rapidjson::Value& params,
        SelectedGunApiFields fields) noexcept {
    if ((fields != SelectedGunApiFields::Buttons && fields != SelectedGunApiFields::Analogs) ||
            !params.IsArray()) return SelectedGunApiCheck::Malformed;
    bool owned = false;
    for (const auto& param : params.GetArray()) {
        if (!param.IsArray() || param.Size() < 2 || !param[0].IsString()) return SelectedGunApiCheck::Malformed;
        const auto& name = param[0];
        if (std::memchr(name.GetString(), 0, name.GetStringLength())) return SelectedGunApiCheck::Malformed;
        if (fields == SelectedGunApiFields::Buttons) {
            if (!param[1].IsBool() && !param[1].IsFloat() && !param[1].IsInt()) return SelectedGunApiCheck::Malformed;
        } else if (!param[1].IsFloat() && !param[1].IsInt()) return SelectedGunApiCheck::Malformed;
        owned |= selectedGunApiField(fields, std::string_view(name.GetString(), name.GetStringLength()));
    }
    return owned ? SelectedGunApiCheck::GunOwned : SelectedGunApiCheck::Allowed;
}

} // namespace bone_eater::input

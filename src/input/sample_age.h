#pragma once

#include "input/selected_hid_config.h"
#include <limits>
#include <type_traits>

namespace bone_eater::input {

// Nonnegative clock age without signed duration subtraction. The supplied
// maximum is explicit and bounded for conversion into the supported clock rep.
// Both source receipt aging and cabinet-publication aging use the same rule.
inline bool sampleAgeWithin(GunSourceClock::time_point receipt, GunSourceClock::time_point now,
        std::chrono::milliseconds maximum) noexcept {
    using Rep = GunSourceClock::duration::rep;
    static_assert(std::is_integral_v<Rep>);
    static_assert(std::numeric_limits<Rep>::digits >=
        std::numeric_limits<SelectedHidAgeComparison::rep>::digits);
    if (maximum.count() <= 0 || maximum > selectedHidMaximumReportAge || receipt > now) return false;
    using Unsigned = std::make_unsigned_t<Rep>;
    const auto age = static_cast<Unsigned>(now.time_since_epoch().count()) -
        static_cast<Unsigned>(receipt.time_since_epoch().count());
    const auto limit = std::chrono::duration_cast<GunSourceClock::duration>(maximum).count();
    return age <= static_cast<Unsigned>(limit);
}

} // namespace bone_eater::input

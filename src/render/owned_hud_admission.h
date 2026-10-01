#pragma once
#include "render/owned_hud_quad_selection.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace bone_eater::render {
// Observation policy for GuiObjectManager::update (1E6140). Membership is
// provisional at its first sort (return1E62E5), committed only after update
// returns. This does not install hooks or authorize replacement rendering.
// Integration must serialize all calls, cancel on exceptions, and revoke any
// earlier certificate if another manager pass starts in the same frame.
class OwnedHudAdmission {
public:
    static_assert(ownedHudNodeCount<=64);
    static constexpr std::size_t maxEntries=4998; //Native reports error at4999.
    struct Record {
        std::uintptr_t key=0, gui=0;
        bool operator==(const Record&) const = default;
    };
    struct Stamp {
        std::uint64_t frame=0, imageEpoch=0, fontEpoch=0;
        bool operator==(const Stamp&) const = default;
    };
    using Selected=std::array<std::uintptr_t,ownedHudNodeCount>;
    unsigned captureFailure() const noexcept { return captureFailure_; }
    struct Certificate {
        Stamp stamp;
        Selected selected {};
        std::uint64_t omitted=0;
        bool valid=false;
    };

    // One manager invocation per material frame. Reentry or a repeated pass
    // invalidates the existing provisional proof, even when its epoch matches.
    // Registry and identity reads are the integration's synchronous snapshots.
    std::uint64_t begin(Stamp stamp,std::span<const Record> registry,const Selected& selected) {
        if (pending_ || !stamp.frame || !stamp.imageEpoch || !stamp.fontEpoch ||
                stamp.frame<=lastFrame_) { rejected_=true; return 0; }
        lastFrame_=stamp.frame;
        rejected_=true;
        if (!validRegistry(registry) || !validSelected(registry,selected)) return 0;
        registry_.assign(registry.begin(),registry.end());
        selected_=selected; stamp_=stamp;
        submitted_=omitted_=0; captured_=false;
        if (serial_==std::numeric_limits<std::uint64_t>::max()) return 0;
        ++serial_; pending_=true; rejected_=false;
        return serial_;
    }
    // Supply GUI identities copied through the first-sort list's GUI** cells.
    // Never retain borrowed stack-list or heap-cell addresses in this object.
    bool capture(std::uint64_t token,std::span<const std::uintptr_t> admitted) {
        captureFailure_=1;
        if (!pending_ || rejected_ || token!=serial_ || captured_) return reject();
        captureFailure_=2;
        if (admitted.empty() || admitted.size()>registry_.size() || admitted.size()>maxEntries) return reject();
        std::vector<std::uintptr_t> sorted(admitted.begin(),admitted.end());
        std::sort(sorted.begin(),sorted.end());
        captureFailure_=3;
        if (!sorted.front() || std::adjacent_find(sorted.begin(),sorted.end())!=sorted.end()) return reject();
        auto known=identities(registry_);
        captureFailure_=4;
        if (!std::includes(known.begin(),known.end(),sorted.begin(),sorted.end())) return reject();
        for (std::size_t i=0;i<selected_.size();++i)
            if (!std::binary_search(sorted.begin(),sorted.end(),selected_[i])) omitted_|=std::uint64_t{1}<<i;
        captured_=true;
        captureFailure_=5;
        if (omitted_&submitted_) return reject();
        captureFailure_=0;
        return true;
    }
    void submitted(std::uintptr_t gui) noexcept {
        if (!pending_) return;
        for (std::size_t i=0;i<selected_.size();++i)
            if (gui==selected_[i]) submitted_|=std::uint64_t{1}<<i;
        if (captured_ && (omitted_&submitted_)) rejected_=true;
    }
    Certificate finish(std::uint64_t token,Stamp stamp,std::span<const Record> registry,
                       const Selected& selected,bool identitiesStillCurrent) noexcept {
        const bool valid=pending_ && !rejected_ && captured_ && token==serial_ && stamp==stamp_ &&
            identitiesStillCurrent && selected==selected_ && registry.size()==registry_.size() &&
            std::equal(registry.begin(),registry.end(),registry_.begin()) && !(omitted_&submitted_);
        pending_=false; rejected_=true;
        return valid ? Certificate{stamp_,selected_,omitted_,true} : Certificate{};
    }
    void cancel() noexcept { pending_=false; rejected_=true; }
private:
    static std::vector<std::uintptr_t> identities(std::span<const Record> registry) {
        std::vector<std::uintptr_t> ids; ids.reserve(registry.size());
        for (const auto& record:registry) ids.push_back(record.gui);
        std::sort(ids.begin(),ids.end()); return ids;
    }
    static bool validRegistry(std::span<const Record> registry) {
        if (registry.empty() || registry.size()>maxEntries) return false;
        const auto ids=identities(registry);
        return ids.front() && std::adjacent_find(ids.begin(),ids.end())==ids.end();
    }
    static bool validSelected(std::span<const Record> registry,const Selected& selected) {
        auto ids=selected; std::sort(ids.begin(),ids.end());
        if (!ids.front() || std::adjacent_find(ids.begin(),ids.end())!=ids.end()) return false;
        const auto known=identities(registry);
        return std::includes(known.begin(),known.end(),ids.begin(),ids.end());
    }
    bool reject() noexcept { rejected_=true; return false; }
    std::vector<Record> registry_;
    Selected selected_ {};
    Stamp stamp_;
    std::uint64_t lastFrame_=0,serial_=0,submitted_=0,omitted_=0;
    bool pending_=false,rejected_=true,captured_=false;
    unsigned captureFailure_=0;
};

// Exact pinned module layout. Reader must return false for unreadable memory.
// incomingThis is complete singleton+8 (secondary vtable10CE6A8), not the
// complete object with primary vtable10CE680. No native memory is changed.
template<class Reader>
bool readOwnedHudRegistry(std::uintptr_t base,std::uintptr_t incomingThis,Reader&& reader,
                          std::vector<OwnedHudAdmission::Record>& out) {
    out.clear();
    const auto read=[&](std::uintptr_t p,std::size_t offset,auto& value) {
        const auto max=std::numeric_limits<std::uintptr_t>::max();
        return p && offset<=max-p && sizeof(value)<=max-(p+offset) && reader(p+offset,&value,sizeof(value));
    };
    if (!base || base>std::numeric_limits<std::uintptr_t>::max()-0x13DCFF0) return false;
    std::uintptr_t singleton=0,primary=0,secondary=0;
    std::int32_t count=0,repeated=0;
    if (!read(base,0x13DCFE8,singleton) || !singleton || singleton>std::numeric_limits<std::uintptr_t>::max()-8 ||
            incomingThis!=singleton+8 || !read(singleton,0,primary) || primary!=base+0x10CE680 ||
            !read(incomingThis,0,secondary) || secondary!=base+0x10CE6A8 ||
            !read(incomingThis,0x138B8,count) || count<=0 || count>static_cast<int>(OwnedHudAdmission::maxEntries)) return false;
    std::vector<OwnedHudAdmission::Record> current(static_cast<std::size_t>(count));
    static_assert(sizeof(OwnedHudAdmission::Record)==16);
    for (std::size_t i=0;i<current.size();++i)
        if (!read(incomingThis,0x28+i*0x10,current[i])) return false;
    if (!read(incomingThis,0x138B8,repeated) || repeated!=count) return false;
    out=std::move(current); return true;
}

template<class Reader>
bool readOwnedHudAdmitted(std::uintptr_t list,int count,Reader&& reader,
                         std::vector<std::uintptr_t>& out) {
    out.clear();
    if (!list || count<=0 || count>static_cast<int>(OwnedHudAdmission::maxEntries)) return false;
    const auto max=std::numeric_limits<std::uintptr_t>::max();
    if (list>max-static_cast<std::size_t>(count)*sizeof(std::uintptr_t)) return false;
    std::vector<std::uintptr_t> current(static_cast<std::size_t>(count));
    for (std::size_t i=0;i<current.size();++i) {
        std::uintptr_t cell=0;
        if (!reader(list+i*sizeof(cell),&cell,sizeof(cell)) || !cell || cell>max-sizeof(cell) ||
                !reader(cell,&current[i],sizeof(current[i])) || !current[i]) return false;
    }
    out=std::move(current); return true;
}
} //namespace bone_eater::render

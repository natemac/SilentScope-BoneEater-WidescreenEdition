#pragma once
#include <cstdint>
#include <string>
#include <array>
#include <algorithm>
#include <chrono>
#include <random>

namespace bone_eater::render {
inline constexpr unsigned titleArtPairCount=12;
inline std::wstring titleArtSuffix(unsigned index) {
    return (index<10?L"0":L"")+std::to_wstring(index)+L".png";
}
// Feed only verified title owners. Unknown reads do not constitute a loop.
class TitleArtRotation {
    std::uintptr_t owner_=0;
    bool active_=false,started_=false;
    unsigned selected_=0,displayed_=0;
    std::array<unsigned,titleArtPairCount> bag_ {};
    unsigned next_=titleArtPairCount;
    std::mt19937 random_;
public:
    explicit TitleArtRotation(std::uint32_t seed=static_cast<std::uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count())) noexcept : random_(seed) {}
    void leave() noexcept { active_=false; }
    void observe(std::uintptr_t owner,unsigned phase) noexcept {
        if(!owner)return;
        if(phase==6||phase==7) {
            if(!active_||owner_!=owner) {
                if(next_==titleArtPairCount) {
                    for(unsigned i=0;i<titleArtPairCount;++i)bag_[i]=i;
                    std::shuffle(bag_.begin(),bag_.end(),random_);
                    if(started_&&bag_[0]==selected_)std::swap(bag_[0],bag_[1]);
                    next_=0;
                }
                selected_=bag_[next_++];
                started_=true;active_=true;owner_=owner;
            }
        } else if(phase!=8&&phase!=15&&phase!=16) leave();
    }
    unsigned title() const noexcept { return selected_; }
    unsigned menu() const noexcept { return displayed_; }
    void presented() noexcept { displayed_=selected_; }
};
}

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <algorithm>

namespace bone_eater::render {
enum class OwnedHudGroup : std::uint8_t { Left, Right, Center, Count };
struct OwnedHudNodeSpec {
    const char* name;
    const char* parent;
    OwnedHudGroup group;
    bool font;
};
// Exact authored selection. Roots are included because native zero-size Image
// nodes can still submit quads. No root_upper wildcard, rectangle or atlas test.
inline constexpr OwnedHudNodeSpec ownedHudNodes[] {
    {"root_upperLeft","root_upper",OwnedHudGroup::Left,false},
    {"root_UpLeft_Circle","root_upperLeft",OwnedHudGroup::Left,false},
    {"root_UpLeft_Circle_St","root_upperLeft",OwnedHudGroup::Left,false},
    {"root_UpLeft_Circle_St1","root_upperLeft",OwnedHudGroup::Left,false},
    {"lcd_bt_bulletnum_0","root_upperLeft",OwnedHudGroup::Left,false},
    {"lcd_bt_bulletnum_1","root_upperLeft",OwnedHudGroup::Left,false},
    {"lcd_bt_bulletnum_2","root_upperLeft",OwnedHudGroup::Left,false},
    {"lcd_bt_bulletnum_0F","root_upperLeft",OwnedHudGroup::Left,false},
    {"lcd_bt_bulletnum_1F","root_upperLeft",OwnedHudGroup::Left,false},
    {"lcd_bt_bulletnum_2F","root_upperLeft",OwnedHudGroup::Left,false},
    {"lcd_bt_heart_num","root_upperLeft",OwnedHudGroup::Left,false},
    {"lcd_bt_heart_0","root_upperLeft",OwnedHudGroup::Left,false},
    {"lcd_bt_heart_1","root_upperLeft",OwnedHudGroup::Left,false},
    {"lcd_bt_heart_2","root_upperLeft",OwnedHudGroup::Left,false},
    {"lcd_bt_heart_3","root_upperLeft",OwnedHudGroup::Left,false},
    {"lcd_bt_heart_4","root_upperLeft",OwnedHudGroup::Left,false},
    {"lcd_bt_BulletTypeFont","root_upperLeft",OwnedHudGroup::Left,false},
    {"lcd_bt_LeftCircle_satellie_0","root_UpLeft_Circle_St",OwnedHudGroup::Left,false},
    {"lcd_bt_LeftCircle_satellie_1","root_UpLeft_Circle_St",OwnedHudGroup::Left,false},
    {"lcd_bt_bullet_unlimited","root_upper",OwnedHudGroup::Left,false},
    {"lcd_bt_LeftCircle_0","root_upper",OwnedHudGroup::Left,false},
    {"lcd_bt_LeftBack0","root_upper",OwnedHudGroup::Left,false},
    {"lcd_bt_LeftBack1","root_upper",OwnedHudGroup::Left,false},
    {"root_upperRight","root_upper",OwnedHudGroup::Right,false},
    {"root_UpRight_Circle","root_upperRight",OwnedHudGroup::Right,false},
    {"root_UpRight_Circle_St","root_upperRight",OwnedHudGroup::Right,false},
    {"root_UpRight_Circle_St1","root_upperRight",OwnedHudGroup::Right,false},
    {"lcd_bt_accRateNum_0","root_upperRight",OwnedHudGroup::Right,false},
    {"lcd_bt_accRateNum_1","root_upperRight",OwnedHudGroup::Right,false},
    {"lcd_bt_accRateNum_2","root_upperRight",OwnedHudGroup::Right,false},
    {"lcd_bt_accRateNum_5","root_upperRight",OwnedHudGroup::Right,false},
    {"lcd_bt_font_Accuracy","root_upperRight",OwnedHudGroup::Right,false},
    {"lcd_bt_RightCircle_satellie_0","root_UpRight_Circle_St",OwnedHudGroup::Right,false},
    {"lcd_bt_RightCircle_satellie_1","root_UpRight_Circle_St1",OwnedHudGroup::Right,false},
    {"lcd_bt_RightCircle_0","root_upper",OwnedHudGroup::Right,false},
    {"lcd_bt_RightBack0","root_upper",OwnedHudGroup::Right,false},
    {"lcd_bt_RightBack1","root_upper",OwnedHudGroup::Right,false},
    {"lcd_bt_font_score","root_upper",OwnedHudGroup::Center,false},
    {"lcd_bt_scorenum_0","root_upper",OwnedHudGroup::Center,false},
    {"lcd_bt_scorenum_1","root_upper",OwnedHudGroup::Center,false},
    {"lcd_bt_scorenum_2","root_upper",OwnedHudGroup::Center,false},
    {"lcd_bt_scorenum_3","root_upper",OwnedHudGroup::Center,false},
    {"lcd_bt_scorenum_4","root_upper",OwnedHudGroup::Center,false},
    {"lcd_bt_scorenum_5","root_upper",OwnedHudGroup::Center,false},
    {"lcd_bt_scorenum_6","root_upper",OwnedHudGroup::Center,false},
    {"lcd_bt_scorenum_7","root_upper",OwnedHudGroup::Center,false},
    {"lcd_bt_PlayerName","root_upper",OwnedHudGroup::Center,true},
    {"root_ShootingRangeUpper","root_upper",OwnedHudGroup::Center,false},
    {"lcd_bt_font_BULLET","root_ShootingRangeUpper",OwnedHudGroup::Left,false},
    {"lcd_bt_timenum_3","root_ShootingRangeUpper",OwnedHudGroup::Center,false},
    {"lcd_bt_timenum_2","root_ShootingRangeUpper",OwnedHudGroup::Center,false},
    {"lcd_bt_timenum_dot","root_ShootingRangeUpper",OwnedHudGroup::Center,false},
    {"lcd_bt_timenum_1","root_ShootingRangeUpper",OwnedHudGroup::Center,false},
    {"lcd_bt_timenum_0","root_ShootingRangeUpper",OwnedHudGroup::Center,false},
    {"lcd_bt_roundScore","root_ShootingRangeUpper",OwnedHudGroup::Center,false}
};
inline constexpr std::size_t ownedHudNodeCount=std::size(ownedHudNodes);
static_assert(ownedHudNodeCount==55);

// Opaque IDs are compared only. They are not dereferenceable retained GUI
// pointers. The integration must assign epoch on every material reset329310,
// and frame on the actual submission frame, not from an arbitrary clear.
struct OwnedHudBatchKey {
    std::uint64_t material=0, epoch=0, vertexBuffer=0;
    bool operator==(const OwnedHudBatchKey&) const = default;
    bool valid() const noexcept { return material && epoch && vertexBuffer; }
};
struct OwnedHudQuadSpan {
    OwnedHudBatchKey batch;
    std::uint32_t first=0, count=0;
    OwnedHudGroup group=OwnedHudGroup::Left;
};
struct OwnedHudDrawRange {
    std::uint32_t firstIndex=0, indexCount=0;
    // Count denotes original front; a semantic group denotes the wider target.
    OwnedHudGroup destination=OwnedHudGroup::Count;
};
struct OwnedHudDrawPlan {
    std::uint64_t frame=0;
    OwnedHudBatchKey batch;
    std::uint32_t totalQuads=0;
    std::array<OwnedHudDrawRange,ownedHudNodeCount*2+1> ranges {};
    std::size_t count=0;
    bool replacement=false;
};

class OwnedHudQuadSelection {
public:
    enum Reject : std::uint32_t { Guard=1,Duplicate=2,Range=4,Missing=8,Backing=16,Font=32,Overlap=64,Batch=128,Seal=256,Frame=512,LateMutation=1024,VertexUnavailable=2048,CounterState=4096,Identity=8192,AppendDelta=16384,EpochChanged=32768 };
    struct Diagnostics {
        std::array<unsigned,3> seen {},missing {},pending {};
        std::array<std::uint32_t,3> reasons {};
        std::uint64_t missingMask=0;
    };
    Diagnostics diagnostics() const noexcept {
        Diagnostics d; d.reasons=reasons_;
        for (std::size_t i=0;i<ownedHudNodeCount;++i) {
            const auto g=static_cast<std::size_t>(ownedHudNodes[i].group);
            if (seen_[i]) ++d.seen[g];
            else { ++d.missing[g]; d.missingMask|=std::uint64_t{1}<<i; }
            if (seen_[i] && spans_[i].count && !validated_[i]) ++d.pending[g];
        }
        return d;
    }
    // This conservative bound stays below the native16-bit shared quad index
    // buffer's vertex limit. No arithmetic can wrap or exceed 65535 vertices.
    static constexpr std::uint32_t maxQuads=16383;
    void begin(std::uint64_t frame) noexcept {
        *this={}; frame_=frame;
    }
    void invalidate(OwnedHudGroup group,std::uint32_t reason=Guard) noexcept {
        if (validGroup(group)) { rejected_[static_cast<std::size_t>(group)]=true; reasons_[static_cast<std::size_t>(group)]|=reason; }
    }
    // Caller must validate the complete live name/type/parent/owner identity.
    // Even an invisible leaf needs an explicit observation. Alpha0 is not
    // evidence that a quad was not appended.
    bool observe(std::uint64_t frame, std::size_t node, const OwnedHudBatchKey& batch,
                 std::uint32_t first, std::uint32_t count,
                 bool certifiedNoSubmission=false) noexcept {
        if (!frame_ || frame!=frame_ || sealed_ || node>=ownedHudNodeCount) { rejectAll(Frame); return false; }
        const auto group=ownedHudNodes[node].group;
        if (seen_[node] || (!count && !certifiedNoSubmission) ||
                (count && (certifiedNoSubmission || !batch.valid() || first>=maxQuads || count>maxQuads-first)) ||
                (!ownedHudNodes[node].font && count>1)) { invalidate(group,seen_[node] ? Duplicate : Range); return false; }
        // A zero-length observation certifies the node emitted no primitive;
        // it must not contribute a stale material identity to a draw plan.
        spans_[node]={count ? batch : OwnedHudBatchKey{},first,count,group};
        seen_[node]=true;
        return true;
    }
    // Identity revalidation, wider GPU resource readiness, and coherent rear
    // illumination are frame-level prerequisites. Font route proof is separate.
    void seal(std::uint64_t frame, bool identitiesRevalidated, bool targetReady,
              const std::array<bool,3>& backingReady, bool fontRouteCertified) noexcept {
        if (sealed_ || !frame_ || frame!=frame_ || !identitiesRevalidated || !targetReady) rejectAll(Seal);
        for (std::size_t node=0;node<ownedHudNodeCount;++node)
            if (!seen_[node]) invalidate(ownedHudNodes[node].group,Missing);
        for (std::size_t g=0;g<3;++g) if (!backingReady[g]) invalidate(static_cast<OwnedHudGroup>(g),Backing);
        if (!fontRouteCertified) invalidate(OwnedHudGroup::Center,Font);
        // Two selected nodes cannot own the same index. Reject both semantic
        // groups even if one was already rejected: the alias is ambiguous.
        for (std::size_t i=0;i<ownedHudNodeCount;++i)
            for (std::size_t j=0;j<i;++j) {
                const auto& a=spans_[i]; const auto& b=spans_[j];
                if (seen_[i] && seen_[j] && a.count && b.count && a.batch==b.batch &&
                        a.first<b.first+b.count && b.first<a.first+a.count) {
                    invalidate(a.group,Overlap); invalidate(b.group,Overlap);
                }
            }
        sealed_=true;
    }
    bool ready(OwnedHudGroup group) const noexcept {
        if (!sealed_ || !validGroup(group) || rejected_[static_cast<std::size_t>(group)]) return false;
        for (std::size_t i=0;i<ownedHudNodeCount;++i)
            if (spans_[i].group==group && spans_[i].count && !validated_[i]) return false;
        return true;
    }
    // Must be called for every batch before any selected group is replaced.
    // A missing or reset batch invalidates that entire group, including other
    // material batches. Integration performs this preflight before its first
    // replacement draw; late failures cannot undo already omitted pixels.
    void validateBatch(const OwnedHudBatchKey& expected, const OwnedHudBatchKey& current,
                       std::uint32_t totalQuads, bool nativeQuadIndexBuffer) noexcept {
        for (std::size_t i=0;i<ownedHudNodeCount;++i) {
            const auto& span=spans_[i];
            if (!span.count || !(span.batch==expected)) continue;
            if (!(expected==current) || !nativeQuadIndexBuffer || !totalQuads || totalQuads>maxQuads ||
                    span.first>=totalQuads || span.count>totalQuads-span.first) invalidate(span.group,Batch);
            else validated_[i]=true;
        }
    }
    OwnedHudDrawPlan plan(std::uint64_t frame, const OwnedHudBatchKey& batch,
                          std::uint32_t totalQuads) const noexcept {
        OwnedHudDrawPlan out;
        if (!sealed_ || frame!=frame_ || !batch.valid() || !totalQuads || totalQuads>maxQuads) return out;
        std::array<OwnedHudQuadSpan,ownedHudNodeCount> selected {};
        std::size_t n=0;
        for (const auto& span:spans_) if (span.count && span.batch==batch && ready(span.group)) {
            if (span.first>=totalQuads || span.count>totalQuads-span.first) return {};
            selected[n++]=span;
        }
        if (!n) return out; // Empty means call the unchanged original draw.
        std::sort(selected.begin(),selected.begin()+n,[](const auto& a,const auto& b){return a.first<b.first;});
        std::uint32_t next=0;
        for (std::size_t i=0;i<n;++i) {
            const auto& span=selected[i];
            if (span.first<next) return {};
            if (span.first>next) out.ranges[out.count++]={next*6,(span.first-next)*6,OwnedHudGroup::Count};
            out.ranges[out.count++]={span.first*6,span.count*6,span.group};
            next=span.first+span.count;
        }
        if (next<totalQuads) out.ranges[out.count++]={next*6,(totalQuads-next)*6,OwnedHudGroup::Count};
        out.replacement=true;
        out.frame=frame_;
        out.batch=batch;
        out.totalQuads=totalQuads;
        return out;
    }
private:
    static bool validGroup(OwnedHudGroup g) noexcept { return static_cast<unsigned>(g)<3; }
    void rejectAll(std::uint32_t reason) noexcept { rejected_.fill(true); for(auto& r:reasons_) r|=reason; }
    std::uint64_t frame_=0;
    bool sealed_=false;
    std::array<bool,3> rejected_ {};
    std::array<std::uint32_t,3> reasons_ {};
    std::array<bool,ownedHudNodeCount> seen_ {};
    std::array<bool,ownedHudNodeCount> validated_ {};
    std::array<OwnedHudQuadSpan,ownedHudNodeCount> spans_ {};
};
} // namespace bone_eater::render

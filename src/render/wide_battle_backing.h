#pragma once
#include "render/rear_illumination_math.h"
#include "render/owned_hud_quad_selection.h"
#include <array>
#include <cmath>
#include <cstdint>

namespace bone_eater::render {
// This intentionally stretches ONLY the two white/variable-alpha rear upper
// illumination quads horizontally. Front glyphs use an independent uniform
// transform. All relocated front groups must share this vertical transform.
struct WideBattleBackingLayout {
    unsigned width=1920, height=1080;
    float left=0, right=1920, frontScale=1, frontTop=0;
    bool operator==(const WideBattleBackingLayout&) const = default;
};
struct WideBattleBackingPlan {
    WideBattleBackingLayout layout;
    std::array<RearIlluminationPose,2> poses {};
    bool valid=false;
};
namespace wide_backing_detail {
inline bool near(float a,float b) noexcept { return std::isfinite(a) && std::isfinite(b) && std::fabs(a-b)<=.001f; }
inline bool samePose(const RearIlluminationPose& a,const RearIlluminationPose& b) noexcept {
    return near(a.position[0],b.position[0]) && near(a.position[1],b.position[1]) &&
        near(a.scale[0],b.scale[0]) && near(a.scale[1],b.scale[1]);
}
}

// Only call with fresh pre-adaptation cached poses from the already certified
// native two-leaf chain. Unit source scales and the 400-pixel sibling relation
// reject accidental application to an already fitted/expanded backing.
// No input is modified; failure publishes no partial output.
inline bool planWideBattleBacking(const WideBattleBackingLayout& layout,
        const std::array<RearIlluminationPose,2>& source, WideBattleBackingPlan& output) noexcept {
    using wide_backing_detail::near;
    if (layout.width!=1920 || layout.height!=1080 || !std::isfinite(layout.left) ||
        !std::isfinite(layout.right) || layout.left<0 || layout.right>1920 ||
        layout.right-layout.left<800 || !std::isfinite(layout.frontScale) ||
        layout.frontScale<.5f || layout.frontScale>1.5f || !std::isfinite(layout.frontTop) ||
        std::fabs(layout.frontTop)>1080) return false;
    for (const auto& pose:source) {
        if (!std::isfinite(pose.position[0]) || !std::isfinite(pose.position[1]) ||
            std::fabs(pose.position[0])>16384 || std::fabs(pose.position[1])>16384 ||
            !near(pose.scale[0],1) || !near(pose.scale[1],1)) return false;
    }
    if (!near(source[1].position[0]-source[0].position[0],400) ||
        !near(source[0].position[1],source[1].position[1])) return false;
    WideBattleBackingPlan result;
    result.layout=layout;
    const double xScale=(static_cast<double>(layout.right)-layout.left)/800.;
    // Original front presentation quad covers [-.5,1366.5]. Convert rear
    // submitted Y to front source pixels, then use the new uniform HUD scale.
    const double yScale=static_cast<double>(layout.frontScale)*1366./1367.;
    for (unsigned i=0;i<2;++i) {
        auto& pose=result.poses[i];
        // A native GUI commit subtracts .1 once. The settled original cached X
        // values 0,400 therefore become exact submitted X=left,midpoint.
        pose.position[0]=static_cast<float>(layout.left+xScale*source[i].position[0]+.1);
        pose.position[1]=static_cast<float>(layout.frontTop+yScale*(source[i].position[1]+.4)+.1);
        pose.scale={static_cast<float>(xScale),static_cast<float>(yScale)};
    }
    result.valid=true;
    output=result;
    return true;
}

// Readiness is earned from BOTH exact native source leaves in one frame, after
// successful synchronous commits/restoration AND consumption by their original
// native draw. This object performs no reads, writes or rendering. Runtime must
// establish the known BattleUIMain/type/name/tree/UV/blend/camera ownership and
// use authoritative render-frame/epoch IDs before calling these methods.
class WideBattleBackingFrame {
public:
    void begin(std::uint64_t frame,std::uint64_t layoutRevision,
               const WideBattleBackingPlan& plan,bool exactPairCertified) noexcept {
        *this={}; frame_=frame; revision_=layoutRevision; plan_=plan;
        rejected_=!frame || !layoutRevision || !plan.valid || !exactPairCertified;
    }
    // committedPose is the adapted cached pose that the native commit actually
    // consumed, NOT the restored pose. Caller has verified exactly one append
    // (+1 quad,+0x70 bytes) and repeated full identity around that native call.
    bool submitted(std::uint64_t frame,unsigned leaf,const OwnedHudBatchKey& batch,
                   std::uint32_t ordinal,const RearIlluminationPose& committedPose,
                   bool oneQuadAndIdentityRevalidated,bool restored) noexcept {
        if (frame!=frame_ || leaf>=2 || rejected_ || !batch.valid() ||
            ordinal>=OwnedHudQuadSelection::maxQuads || seen_[leaf] ||
            !oneQuadAndIdentityRevalidated || !restored ||
            !wide_backing_detail::samePose(committedPose,plan_.poses[leaf])) {
            rejected_=true; return false;
        }
        for (unsigned i=0;i<2;++i) if (seen_[i] && batches_[i]==batch && ordinals_[i]==ordinal) {
            rejected_=true; return false;
        }
        batches_[leaf]=batch; ordinals_[leaf]=ordinal; seen_[leaf]=true;
        return true;
    }
    // Call AFTER the original verified rear draw has completed successfully;
    // preflight alone cannot certify illumination that was never rendered.
    void consumed(std::uint64_t frame,const OwnedHudBatchKey& expected,
                  const OwnedHudBatchKey& current,std::uint32_t totalQuads,
                  bool originalRearDrawCompleted) noexcept {
        if (frame!=frame_ || !originalRearDrawCompleted) { rejected_=true; return; }
        for (unsigned i=0;i<2;++i) if (seen_[i] && batches_[i]==expected) {
            if (!(expected==current) || !totalQuads || totalQuads>OwnedHudQuadSelection::maxQuads ||
                ordinals_[i]>=totalQuads) { rejected_=true; return; }
            consumed_[i]=true;
        }
    }
    void invalidate() noexcept { rejected_=true; }
    bool ready(std::uint64_t frame,std::uint64_t revision,
               const WideBattleBackingLayout& layout) const noexcept {
        return !rejected_ && frame && frame==frame_ && revision && revision==revision_ &&
            layout==plan_.layout && seen_[0] && seen_[1] && consumed_[0] && consumed_[1];
    }
    std::array<bool,3> groups(std::uint64_t frame,std::uint64_t revision,
                              const WideBattleBackingLayout& layout) const noexcept {
        const bool complete=ready(frame,revision,layout);
        return {complete,complete,complete};
    }
private:
    std::uint64_t frame_=0,revision_=0;
    WideBattleBackingPlan plan_;
    bool rejected_=true;
    std::array<bool,2> seen_ {},consumed_ {};
    std::array<OwnedHudBatchKey,2> batches_ {};
    std::array<std::uint32_t,2> ordinals_ {};
};
}

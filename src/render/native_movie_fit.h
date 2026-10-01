#pragma once

namespace bone_eater::render {

// Default off. BONE_EATER_NATIVE_MOVIE_FIT=1 installs an exact-build movie
// initialization hook before game entry. Requires a verified native-wide
// allocation and the live native -2Display helper. Only new display-0 movie
// sprites are fitted; scope/LCD movies, UVs, blend, playback and audio remain
// native. Restart without the option to restore the original initialization.
void installNativeMovieFit(void* module) noexcept;

} // namespace bone_eater::render

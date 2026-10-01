#pragma once
#include <cstdint>
struct IDXGISwapChain;
namespace bone_eater::render {
bool captureLobbyImpact(std::uintptr_t base, std::uintptr_t adapter) noexcept;
void composeLobbyImpact(IDXGISwapChain* chain, void* mainWindow, unsigned flags) noexcept;
}

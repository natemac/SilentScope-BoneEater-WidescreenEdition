#pragma once
struct IDXGISwapChain;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
#include <cstdint>
namespace bone_eater::render {
// Certified native main-presentation UI draw, before its first lobby batch.
void captureLobbyBackdrop(ID3D11DeviceContext*,std::uintptr_t material, bool frontConsumer = false) noexcept;
bool lobbyBackdropRequested() noexcept;
bool lobbyReticleRequested() noexcept;
bool resultsReticleRequested() noexcept;
bool captureScoreRoundScene(ID3D11DeviceContext*) noexcept;
bool extendScoreRoundSides(ID3D11DeviceContext*,ID3D11ShaderResourceView*,float left,float width) noexcept;
void composeTitleD3D11Present(IDXGISwapChain*, void* mainWindow, unsigned flags) noexcept;
}

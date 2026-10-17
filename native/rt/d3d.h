#pragma once

// D3D11 helpers shared by both mods: swap-chain vtable discovery and a game-window test.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace lie::d3d {

// IDXGISwapChain::Present (vtable 8) and ResizeBuffers (vtable 13) from a throw-away device.
bool swapchain_entries(void*& present, void*& resize_buffers);

// True for this process' UnrealWindow
bool is_game_window(HWND hwnd);
HWND find_game_window();

template <class T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

} // namespace lie::d3d

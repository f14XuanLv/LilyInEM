#include "rt/d3d.h"

#include <d3d11.h>
#include <dxgi.h>

#include <cwchar>

namespace lie::d3d {

bool swapchain_entries(void*& present, void*& resize_buffers) {
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"LilyInEM_Probe", nullptr};
    RegisterClassExW(&wc);
    HWND wnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 32, 32, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = wnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* swap = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    const bool ok = SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &swap, &dev,
                                                            nullptr, &ctx)) &&
                    swap;
    if (ok) {
        void** vt = *reinterpret_cast<void***>(swap);
        present = vt[8];
        resize_buffers = vt[13];
    }
    release(swap);
    release(ctx);
    release(dev);
    DestroyWindow(wnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

bool is_game_window(HWND hwnd) {
    wchar_t cls[32]{};
    if (!hwnd || !GetClassNameW(hwnd, cls, 32)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId() && std::wcscmp(cls, L"UnrealWindow") == 0;
}

HWND find_game_window() {
    HWND found = nullptr;
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            if (!is_game_window(h)) return TRUE;
            *reinterpret_cast<HWND*>(lp) = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

} // namespace lie::d3d

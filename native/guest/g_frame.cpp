// Render-thread side of the Guest: every EL Present copies the back buffer into a DXGI shared texture
// (zero-copy for the Host) and publishes FrameMeta with the projection staged by the game thread for
// that frame. While the Host routes input to an EL menu, menu keys are forwarded to the EL window.

#include "guest/guest.h"

#include "rt/clock.h"
#include "rt/d3d.h"
#include "rt/hook.h"
#include "rt/log.h"
#include "rt/shm.h"

#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace lie::guest {

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using CreateTexFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
PresentFn g_present = nullptr;
ResizeFn g_resize = nullptr;
CreateTexFn g_create_tex = nullptr;

struct Export {
    std::mutex m;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* shared = nullptr;
    HANDLE handle = nullptr;
    UINT w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint64_t seq = 0;

    // SceneCapture2D alpha channel: the capture RT's native texture is grabbed by the CreateTexture2D
    // detour (capture_expect_rt arms the match, g_capture creates the RT), then copied each Present
    // into shared_chars for the Host.
    ID3D11Texture2D* shared_chars = nullptr;
    HANDLE chars_handle = nullptr;
    ID3D11Texture2D* capture_rt = nullptr;
    D3D11_TEXTURE2D_DESC want{};        // desc the detour should grab (w/h/fmt/bind)
    bool want_active = false;
    bool rt_checked = false;            // one-shot staging verification done
    std::uint32_t rt_check_frames = 0;  // world-kind frames seen since the grab
} g_x;

struct Staged {
    std::mutex m;
    bool valid = false;
    FrameKind kind = FrameKind::World;
    float split[4]{};
    float menu_alpha = 1.0f;
    float foot_u = 0.5f, foot_v = 0.5f, view_w = 0.0f, view_h = 0.0f;
} g_staged;

std::atomic<HWND> g_hwnd{nullptr};

void release_textures() {
    d3d::release(g_x.shared);
    d3d::release(g_x.shared_chars);
    g_x.handle = nullptr;
    g_x.chars_handle = nullptr;
    g_x.w = g_x.h = 0;
}

// The capture RT is the only texture EL creates with this desc (a full-res FloatRGBA target carries
// TexCreate_UAV; ours is plain RT|SRV), so a want-flagged match is enough. A one-shot staging readback
// in on_present still proves it (the capture contains inverse-opacity alpha: some a < 0.99 once Lily
// is on screen); on failure the match resumes.
HRESULT STDMETHODCALLTYPE on_create_tex(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    const HRESULT hr = g_create_tex(dev, d, init, out);
    if (SUCCEEDED(hr) && d && out && *out && g_x.want_active && !g_x.capture_rt && d->Width == g_x.want.Width && d->Height == g_x.want.Height &&
        d->Format == g_x.want.Format && (d->BindFlags & g_x.want.BindFlags) == g_x.want.BindFlags && d->Usage == D3D11_USAGE_DEFAULT) {
        (*out)->AddRef();
        g_x.capture_rt = *out;
        g_x.want_active = false;
        g_x.rt_checked = false;
        g_x.rt_check_frames = 0;
        log::info(L"capture RT texture grabbed (format %d)", static_cast<int>(d->Format));
    }
    return hr;
}

void arm_create_tex() {
    if (!g_x.device || hook::attached("D3D11.CreateTexture2D")) return;
    void** vt = *reinterpret_cast<void***>(g_x.device);
    hook::attach_at("D3D11.CreateTexture2D", vt[5], reinterpret_cast<void*>(&on_create_tex), reinterpret_cast<void**>(&g_create_tex));
}

ID3D11Texture2D* ensure_shared(const D3D11_TEXTURE2D_DESC& src, ID3D11Texture2D*& tex, HANDLE& handle) {
    D3D11_TEXTURE2D_DESC d = src;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    d.CPUAccessFlags = 0;
    if (!tex) {
        if (FAILED(g_x.device->CreateTexture2D(&d, nullptr, &tex)) || !tex) return nullptr;
        IDXGIResource* res = nullptr;
        if (SUCCEEDED(tex->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&res))) && res) {
            res->GetSharedHandle(&handle);
            res->Release();
        }
        if (!handle) {
            d3d::release(tex);
            return nullptr;
        }
    }
    return tex;
}

bool ensure_texture(IDXGISwapChain* swap, const D3D11_TEXTURE2D_DESC& back) {
    if (!g_x.device) {
        if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_x.device))) || !g_x.device) return false;
        g_x.device->GetImmediateContext(&g_x.context);
        arm_create_tex();
    }
    if (g_x.shared && g_x.w == back.Width && g_x.h == back.Height && g_x.format == back.Format) return true;
    release_textures();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = back.Width;
    d.Height = back.Height;
    d.MipLevels = d.ArraySize = 1;
    d.Format = back.Format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    if (FAILED(g_x.device->CreateTexture2D(&d, nullptr, &g_x.shared)) || !g_x.shared) return false;
    IDXGIResource* res = nullptr;
    if (SUCCEEDED(g_x.shared->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&res))) && res) {
        res->GetSharedHandle(&g_x.handle);
        res->Release();
    }
    g_x.w = back.Width;
    g_x.h = back.Height;
    g_x.format = back.Format;
    log::info(L"shared frame texture %ux%u handle=%p", g_x.w, g_x.h, g_x.handle);
    return g_x.handle != nullptr;
}

// One-shot staging readback: a live capture contains inverse-opacity alpha (some a < 1) whenever the
// world layer is on screen. A wrong grab stays uniformly 1 -> drop it and keep matching.
bool verify_capture_rt() {
    if (!g_x.capture_rt) return false;
    D3D11_TEXTURE2D_DESC d{};
    g_x.capture_rt->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_x.device->CreateTexture2D(&d, nullptr, &staging)) || !staging) return true; // cannot verify: keep it
    g_x.context->CopyResource(staging, g_x.capture_rt);
    D3D11_MAPPED_SUBRESOURCE m{};
    bool covered = false;
    if (SUCCEEDED(g_x.context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        const auto* p = static_cast<const std::uint8_t*>(m.pData);
        const bool fp16 = d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        const UINT stride = fp16 ? 8 : 4, aoff = fp16 ? 6 : 3;
        for (UINT y = 0; y < d.Height && !covered; y += 8) {
            const auto* row = p + static_cast<std::size_t>(y) * m.RowPitch;
            for (UINT x = 0; x < d.Width && !covered; x += 8) {
                if (fp16) covered = *reinterpret_cast<const std::uint16_t*>(row + x * stride + aoff) < 0x3C00; // half 1.0f
                else covered = row[x * stride + aoff] < 0xFD;
            }
        }
        g_x.context->Unmap(staging, 0);
    }
    staging->Release();
    return covered;
}

// EL UIInputSettingsData: VALIDATION=Enter, CANCEL=Escape, CONTEXT=C, PREVIOUS=A, NEXT=D [CUE4P SaveSettings]
constexpr UINT kMenuKeys[9] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_ESCAPE, 'C', 'A', 'D'};

void forward_menu_keys(HWND hwnd) {
    static std::uint32_t prev = 0;
    static bool activated = false;
    Control* c = shm::control();
    InputFrame in{};
    const bool menu = c && host_ctx() == Ctx::LilyMenu && c->input.read(in);
    std::uint32_t cur = 0;
    if (menu) {
        if (!activated) {
            SendMessageW(hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
            activated = true;
        }
        auto held = [&](int k) { return (in.raw & (1u << k)) != 0; };
        if (in.y > 0.5f) cur |= 1u << 0;
        if (in.y < -0.5f) cur |= 1u << 1;
        if (in.x < -0.5f) cur |= 1u << 2;
        if (in.x > 0.5f) cur |= 1u << 3;
        if (held(cmd::Jump)) cur |= 1u << 4;
        if (held(cmd::Dodge) || held(cmd::Sub2)) cur |= 1u << 5;
        if (held(cmd::Attack) || held(cmd::Sub1)) cur |= 1u << 6;
        if (held(cmd::Heal)) cur |= 1u << 7;
        if (held(cmd::SummonSwitch)) cur |= 1u << 8;
    } else {
        activated = false;
    }
    const std::uint32_t pressed = cur & ~prev, released = prev & ~cur;
    prev = cur;
    for (int k = 0; k < 9; ++k) {
        if (pressed & (1u << k)) PostMessageW(hwnd, WM_KEYDOWN, kMenuKeys[k], 0);
        if (released & (1u << k)) PostMessageW(hwnd, WM_KEYUP, kMenuKeys[k], (1u << 30) | (1u << 31));
    }
}

HRESULT STDMETHODCALLTYPE on_present(IDXGISwapChain* swap, UINT sync, UINT flags) {
    {
        std::scoped_lock lk(g_x.m);
        DXGI_SWAP_CHAIN_DESC desc{};
        ID3D11Texture2D* back = nullptr;
        Control* c = shm::control();
        if (c && SUCCEEDED(swap->GetDesc(&desc)) && d3d::is_game_window(desc.OutputWindow) &&
            SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) && back) {
            g_hwnd.store(desc.OutputWindow);
            D3D11_TEXTURE2D_DESC bd{};
            back->GetDesc(&bd);
            if (ensure_texture(swap, bd)) {
                g_x.context->CopyResource(g_x.shared, back);
                std::uintptr_t chars_handle = 0;
                {
                    FrameKind kind;
                    {
                        std::scoped_lock sl(g_staged.m);
                        kind = g_staged.kind;
                    }
                    if (g_x.capture_rt && !g_x.rt_checked && kind != FrameKind::Menu && ++g_x.rt_check_frames > 30) {
                        if (!verify_capture_rt()) {
                            log::error(L"capture RT grab failed verification (alpha uniformly 1): dropping it");
                            d3d::release(g_x.capture_rt);
                            g_x.want_active = true; // keep matching the real one if it is created later
                        }
                        g_x.rt_checked = true;
                    }
                    if (g_x.capture_rt) {
                        D3D11_TEXTURE2D_DESC cd{};
                        g_x.capture_rt->GetDesc(&cd);
                        if (g_x.shared_chars) {
                            D3D11_TEXTURE2D_DESC sd{};
                            g_x.shared_chars->GetDesc(&sd);
                            if (sd.Width != cd.Width || sd.Height != cd.Height || sd.Format != cd.Format) {
                                d3d::release(g_x.shared_chars);
                                g_x.chars_handle = nullptr;
                            }
                        }
                        if (ensure_shared(cd, g_x.shared_chars, g_x.chars_handle)) {
                            g_x.context->CopyResource(g_x.shared_chars, g_x.capture_rt);
                            chars_handle = reinterpret_cast<std::uintptr_t>(g_x.chars_handle);
                        }
                    }
                }
                g_x.context->Flush();
                FrameMeta meta{};
                meta.shared_handle = reinterpret_cast<std::uintptr_t>(g_x.handle);
                meta.chars_handle = chars_handle;
                meta.width = g_x.w;
                meta.height = g_x.h;
                {
                    std::scoped_lock sl(g_staged.m);
                    meta.menu_frame = static_cast<std::uint32_t>(g_staged.kind);
                    std::memcpy(meta.split, g_staged.split, sizeof(meta.split));
                    meta.menu_alpha = g_staged.menu_alpha;
                    if (g_staged.valid) {
                        meta.foot_u = g_staged.foot_u;
                        meta.foot_v = g_staged.foot_v;
                        meta.view_w_uu = g_staged.view_w;
                        meta.view_h_uu = g_staged.view_h;
                    }
                }
                meta.frame_ms = clock::now_ms();
                meta.frame_seq = ++g_x.seq;
                c->frame.store(meta);
            }
            forward_menu_keys(desc.OutputWindow);
        }
        d3d::release(back);
    }
    return g_present(swap, sync, flags);
}

HRESULT STDMETHODCALLTYPE on_resize(IDXGISwapChain* swap, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
    {
        std::scoped_lock lk(g_x.m);
        release_textures();
        d3d::release(g_x.capture_rt);
        g_x.rt_checked = false;
        g_x.rt_check_frames = 0;
    }
    return g_resize(swap, n, w, h, f, flags);
}

} // namespace

void arm_present() {
    if (hook::attached("DXGI.Present")) return;
    void* present = nullptr;
    void* resize = nullptr;
    if (!d3d::swapchain_entries(present, resize)) {
        log::error(L"cannot probe the DXGI swap chain (launch with -dx11)");
        return;
    }
    hook::attach_at("DXGI.Present", present, reinterpret_cast<void*>(&on_present), reinterpret_cast<void**>(&g_present));
    hook::attach_at("DXGI.ResizeBuffers", resize, reinterpret_cast<void*>(&on_resize), reinterpret_cast<void**>(&g_resize));
}

void stage_frame(float foot_u, float foot_v, float view_w_uu, float view_h_uu) {
    std::scoped_lock lk(g_staged.m);
    g_staged.valid = true;
    g_staged.foot_u = foot_u;
    g_staged.foot_v = foot_v;
    g_staged.view_w = view_w_uu;
    g_staged.view_h = view_h_uu;
}

void stage_menu(FrameKind kind, const float split[4], float menu_alpha) {
    std::scoped_lock lk(g_staged.m);
    g_staged.kind = kind;
    std::memcpy(g_staged.split, split, sizeof(g_staged.split));
    g_staged.menu_alpha = menu_alpha;
}

// ---- SceneCapture alpha channel (called from the game thread by g_capture) ----

bool frame_dims(UINT& w, UINT& h) {
    std::scoped_lock lk(g_x.m);
    w = g_x.w;
    h = g_x.h;
    return w != 0 && h != 0;
}

void capture_expect_rt(UINT w, UINT h) {
    std::scoped_lock lk(g_x.m);
    g_x.want = {};
    g_x.want.Width = w;
    g_x.want.Height = h;
    g_x.want.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // UTextureRenderTarget2D RTF_RGBA16f
    g_x.want.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    g_x.want_active = true;
    d3d::release(g_x.capture_rt);
    g_x.rt_checked = false;
    g_x.rt_check_frames = 0;
}

bool capture_rt_ready() {
    std::scoped_lock lk(g_x.m);
    return g_x.capture_rt != nullptr;
}

void* window() { return g_hwnd.load(); }

void shutdown_frame() {
    std::scoped_lock lk(g_x.m);
    release_textures();
    d3d::release(g_x.capture_rt);
    d3d::release(g_x.context);
    d3d::release(g_x.device);
}

} // namespace lie::guest

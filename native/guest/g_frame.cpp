// Render-thread side of the Guest: every EL Present copies the back buffer into a DXGI shared texture
// (zero-copy for the Host) and publishes FrameMeta with the projection staged by the game thread for
// that frame. While the Host routes input to an EL menu, menu keys are forwarded to the EL window.

#include "guest/guest.h"

#include "core/input_map.h"
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

// A UTextureRenderTarget2D we created on the game thread: its native texture is grabbed by the CreateTexture2D
// detour (*_expect_rt arms the match right before the RT is created), then copied each Present into a shared
// texture for the Host.
struct Grab {
    const wchar_t* name;
    bool (*proof)(const D3D11_TEXTURE2D_DESC&, const std::uint8_t* row, UINT x); // true = proves the grab
    ID3D11Texture2D* rt = nullptr;
    ID3D11Texture2D* shared = nullptr;
    HANDLE handle = nullptr;
    D3D11_TEXTURE2D_DESC want{};        // desc the detour should grab (w/h/fmt/bind)
    bool want_active = false;
    bool checked = false;               // one-shot staging verification done
    std::uint32_t check_frames = 0;     // world-kind frames seen since the grab
};

std::uint16_t alpha16(const std::uint8_t* row, UINT x) { return *reinterpret_cast<const std::uint16_t*>(row + x * 8 + 6); }

struct Export {
    std::mutex m;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* shared = nullptr;
    HANDLE handle = nullptr;
    UINT w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint64_t seq = 0;

    // SceneCapture2D alpha channel: a live capture contains inverse-opacity alpha (some a < 1) once Lily is on
    // screen; a wrong grab stays uniformly 1
    Grab chars{L"capture", [](const D3D11_TEXTURE2D_DESC&, const std::uint8_t* row, UINT x) { return alpha16(row, x) < 0x3C00; /* half 1.0 */ }};
    // EL HUD channel: the widget RT is cleared to transparent, so most of it is exactly a = 0; an opaque target never is
    Grab hud{L"HUD", [](const D3D11_TEXTURE2D_DESC&, const std::uint8_t* row, UINT x) { return alpha16(row, x) == 0; }};
    std::atomic<bool> hud_live{false};  // game thread: the component holds EL's PlayerUI and draws into the RT
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
    d3d::release(g_x.chars.shared);
    g_x.handle = nullptr;
    g_x.chars.handle = nullptr;
    g_x.w = g_x.h = 0;
}

void release_grab(Grab& g) {
    d3d::release(g.rt);
    d3d::release(g.shared);
    g.handle = nullptr;
    g.checked = false;
    g.check_frames = 0;
}

bool try_grab(Grab& g, const D3D11_TEXTURE2D_DESC* d, ID3D11Texture2D* tex) {
    if (!g.want_active || g.rt || d->Width != g.want.Width || d->Height != g.want.Height || d->Format != g.want.Format ||
        (d->BindFlags & g.want.BindFlags) != g.want.BindFlags || d->Usage != D3D11_USAGE_DEFAULT)
        return false;
    tex->AddRef();
    g.rt = tex;
    g.want_active = false;
    g.checked = false;
    g.check_frames = 0;
    log::info(L"%s RT texture grabbed (format %d)", g.name, static_cast<int>(d->Format));
    return true;
}

// Our RTs are the only textures EL creates with these descs (full-res FloatRGBA targets of the renderer
// carry TexCreate_UAV; ours are plain RT|SRV), so a want-flagged match is enough. A one-shot staging
// readback in on_present still proves each grab; on failure the match resumes.
HRESULT STDMETHODCALLTYPE on_create_tex(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    const HRESULT hr = g_create_tex(dev, d, init, out);
    if (SUCCEEDED(hr) && d && out && *out && !try_grab(g_x.chars, d, *out)) try_grab(g_x.hud, d, *out);
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

// One-shot staging readback (both RTs are R16G16B16A16_FLOAT): some sampled pixel must satisfy the grab's proof
bool verify_grab(const Grab& g) {
    if (!g.rt) return false;
    D3D11_TEXTURE2D_DESC d{};
    g.rt->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) return false;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_x.device->CreateTexture2D(&d, nullptr, &staging)) || !staging) return true; // cannot verify: keep it
    g_x.context->CopyResource(staging, g.rt);
    D3D11_MAPPED_SUBRESOURCE m{};
    bool ok = false;
    if (SUCCEEDED(g_x.context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        const auto* p = static_cast<const std::uint8_t*>(m.pData);
        for (UINT y = 0; y < d.Height && !ok; y += 8) {
            const auto* row = p + static_cast<std::size_t>(y) * m.RowPitch;
            for (UINT x = 0; x < d.Width && !ok; x += 8) ok = g.proof(d, row, x);
        }
        g_x.context->Unmap(staging, 0);
    }
    staging->Release();
    return ok;
}

// Per Present: verify once, then copy the grabbed RT into its shared texture. Returns the shared handle (0 = none).
std::uintptr_t export_grab(Grab& g, bool verify_now) {
    if (g.rt && !g.checked && verify_now && ++g.check_frames > 30) {
        if (!verify_grab(g)) {
            log::error(L"%s RT grab failed verification: dropping it", g.name);
            release_grab(g);
            g.want_active = true; // keep matching the real one if it is created later
        }
        g.checked = true;
    }
    if (!g.rt) return 0;
    D3D11_TEXTURE2D_DESC cd{};
    g.rt->GetDesc(&cd);
    if (g.shared) {
        D3D11_TEXTURE2D_DESC sd{};
        g.shared->GetDesc(&sd);
        if (sd.Width != cd.Width || sd.Height != cd.Height || sd.Format != cd.Format) {
            d3d::release(g.shared);
            g.handle = nullptr;
        }
    }
    if (!ensure_shared(cd, g.shared, g.handle)) return 0;
    g_x.context->CopyResource(g.shared, g.rt);
    return reinterpret_cast<std::uintptr_t>(g.handle);
}

void post_key(HWND hwnd, WPARAM key, bool down) {
    PostMessageW(hwnd, down ? WM_KEYDOWN : WM_KEYUP, key, down ? 0 : (1u << 30) | (1u << 31));
}

// Host menu intents (EM's UI keys) -> the same key pressed into EL (Control::ui_layout; the Guest binds EL's UI to
// those keys, so prompts match). While EL's key binding page waits for a key, the Host sends every held key
// instead (InputFrame::keys) and they go in as they are. The pad goes through the XInput detour (g_input.cpp).
void forward_menu_keys(HWND hwnd) {
    static std::uint32_t prev = 0;
    static std::uint64_t prev_keys[4]{};
    static bool activated = false;
    Control* c = shm::control();
    InputFrame in{};
    const bool menu = c && host_ctx() == Ctx::LilyMenu && c->input.read(in);
    std::uint32_t cur = 0;
    std::uint64_t keys[4]{};
    if (menu) {
        if (!activated) {
            SendMessageW(hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
            activated = true;
        }
        cur = in.menu;
        std::memcpy(keys, in.keys, sizeof(keys));
    } else {
        activated = false;
    }
    for (int w = 0; w < 4; ++w) {
        const std::uint64_t diff = keys[w] ^ prev_keys[w];
        for (int b = 0; diff && b < 64; ++b) {
            const int vk = w * 64 + b;
            if ((diff >> b & 1u) && vk >= 0x08) post_key(hwnd, static_cast<WPARAM>(vk), (keys[w] >> b & 1u) != 0);
        }
        prev_keys[w] = keys[w];
    }
    const std::uint32_t pressed = cur & ~prev, released = prev & ~cur;
    prev = cur;
    if (!pressed && !released) return;
    UiLayout l{};
    const bool live = c && c->ui_layout.read(l) && l.valid;
    for (int k = 0; k < menu::Count; ++k) {
        const auto key = static_cast<WPARAM>(live && l.vk[k] ? l.vk[k] : input::kElMenuVkDefault[k]);
        if (pressed & (1u << k)) post_key(hwnd, key, true);
        if (released & (1u << k)) post_key(hwnd, key, false);
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
                FrameKind kind;
                {
                    std::scoped_lock sl(g_staged.m);
                    kind = g_staged.kind;
                }
                const std::uintptr_t chars_handle = export_grab(g_x.chars, kind != FrameKind::Menu);
                const bool hud_live = g_x.hud_live.load();
                const std::uintptr_t hud_handle = hud_live ? export_grab(g_x.hud, true) : 0;
                g_x.context->Flush();
                FrameMeta meta{};
                meta.shared_handle = reinterpret_cast<std::uintptr_t>(g_x.handle);
                meta.chars_handle = chars_handle;
                meta.hud_handle = hud_handle;
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
        release_grab(g_x.chars); // the capture RT follows the frame size; the HUD RT is design-sized and stays
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

static void expect_rt(Grab& g, UINT w, UINT h) {
    std::scoped_lock lk(g_x.m);
    g.want = {};
    g.want.Width = w;
    g.want.Height = h;
    g.want.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // UTextureRenderTarget2D RTF_RGBA16f
    g.want.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    g.want_active = true;
    release_grab(g);
}

void capture_expect_rt(UINT w, UINT h) { expect_rt(g_x.chars, w, h); }
void hud_expect_rt(UINT w, UINT h) { expect_rt(g_x.hud, w, h); }
void hud_set_live(bool live) { g_x.hud_live.store(live); }

bool capture_rt_ready() {
    std::scoped_lock lk(g_x.m);
    return g_x.chars.rt != nullptr;
}

void* window() { return g_hwnd.load(); }

// r.setres / GameUserSettings clamp windowed resolutions to the logical desktop (1707x1067 on a
// 2560x1600/150% display). So does UE4's WM_GETMINMAXINFO handler (max track size = work area).
// The window lives off-screen anyway: subclass it to lift that clamp, then WM_SIZE resizes the swap chain.
WNDPROC g_el_wndproc = nullptr;
LRESULT CALLBACK on_el_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_GETMINMAXINFO) {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMaxSize = {32000, 32000};
        mm->ptMaxTrackSize = {32000, 32000};
        return 0;
    }
    return CallWindowProcW(g_el_wndproc, h, msg, wp, lp);
}

void request_frame_size(std::uint32_t w, std::uint32_t h) {
    HWND hwnd = g_hwnd.load();
    if (!hwnd) return;
    if (!g_el_wndproc) g_el_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&on_el_wndproc)));
    RECT wr{}, cr{};
    if (!GetWindowRect(hwnd, &wr) || !GetClientRect(hwnd, &cr)) return;
    const int chrome_w = (wr.right - wr.left) - (cr.right - cr.left);
    const int chrome_h = (wr.bottom - wr.top) - (cr.bottom - cr.top);
    SetWindowPos(hwnd, nullptr, 0, 0, static_cast<int>(w) + chrome_w, static_cast<int>(h) + chrome_h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOOWNERZORDER);
}

void shutdown_frame() {
    std::scoped_lock lk(g_x.m);
    release_textures();
    release_grab(g_x.chars);
    release_grab(g_x.hud);
    d3d::release(g_x.context);
    d3d::release(g_x.device);
}

} // namespace lie::guest

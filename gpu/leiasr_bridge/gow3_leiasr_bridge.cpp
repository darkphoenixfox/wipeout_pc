// SPDX-License-Identifier: MIT
// gow3_leiasr.dll: the LeiaSR SDK's Direct3D 11 weaver behind a C interface (gow3_leiasr_bridge.h).

#define GOW3_LEIASR_BRIDGE_BUILD
#include "gow3_leiasr_bridge.h"

// TryGetDisplayManagerInstance() binds the display manager at run time.
#define SRDISPLAY_LAZYBINDING

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

#include "sr/management/srcontext.h"
#include "sr/sense/display/switchablehint.h"
#include "sr/utility/exception.h"
#include "sr/weaver/dx11weaver.h"
#include "sr/world/display/display.h"

namespace {

template <typename T>
void Release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

struct State {
    Gow3LeiaLogFn log = nullptr;
    SR::SRContext* context = nullptr;
    SR::IDX11Weaver1* weaver = nullptr;
    SR::SwitchableLensHint* lens = nullptr;
    int lens_on = -1;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* immediate = nullptr;
    ID3D11Query* done = nullptr;
    ID3D11Texture2D* views = nullptr;
    ID3D11ShaderResourceView* views_srv = nullptr;
    HANDLE views_handle = nullptr;
    ID3D11Texture2D* output = nullptr;
    ID3D11RenderTargetView* output_rtv = nullptr;
    HANDLE output_handle = nullptr;
    UINT output_width = 0, output_height = 0;
};
State state;

void Log(int warning, const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (state.log) {
        state.log(warning, text);
    }
}

void ReleaseTextures() {
    Release(state.views_srv);
    Release(state.views);
    Release(state.output_rtv);
    Release(state.output);
    if (state.views_handle) CloseHandle(state.views_handle);
    if (state.output_handle) CloseHandle(state.output_handle);
    state.views_handle = state.output_handle = nullptr;
}

bool CreateDevice(const uint8_t luid[8]) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        Log(1, "no DXGI factory");
        return false;
    }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (std::memcmp(&desc.AdapterLuid, luid, 8) == 0) {
            break;
        }
        Release(adapter);
    }
    Release(factory);
    if (!adapter) {
        Log(1, "the Vulkan GPU has no Direct3D adapter (LUID not found)");
        return false;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                         D3D11_SDK_VERSION, &state.device, nullptr,
                                         &state.immediate);
    Release(adapter);
    if (FAILED(hr)) {
        Log(1, "D3D11CreateDevice failed (0x%08lx)", hr);
        return false;
    }
    const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
    state.device->CreateQuery(&query, &state.done);
    return true;
}

bool CreateContext(uint32_t wait_ms) {
    const auto start = std::chrono::steady_clock::now();
    while (!state.context) {
        try {
            state.context = SR::SRContext::create(true);
        } catch (SR::ServerNotAvailableException&) {
        } catch (...) {
            Log(1, "SRContext::create failed");
            return false;
        }
        if (state.context) break;
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(wait_ms)) {
            Log(1, "the SR service is not available (is the SR Platform installed and the "
                   "display on?)");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

HANDLE Share(ID3D11Texture2D* texture) {
    IDXGIResource1* resource = nullptr;
    HANDLE handle = nullptr;
    if (SUCCEEDED(texture->QueryInterface(__uuidof(IDXGIResource1),
                                          reinterpret_cast<void**>(&resource)))) {
        resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                     nullptr, &handle);
        resource->Release();
    }
    return handle;
}

ID3D11Texture2D* SharedTexture(UINT width, UINT height) {
    const D3D11_TEXTURE2D_DESC desc{
        .Width = width,
        .Height = height,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .SampleDesc = {1, 0},
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
        .CPUAccessFlags = 0,
        .MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE,
    };
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(state.device->CreateTexture2D(&desc, nullptr, &texture))) {
        Log(1, "cannot create a %ux%u shared texture", width, height);
    }
    return texture;
}

} // namespace

extern "C" int gow3_leiasr_abi(void) {
    return GOW3_LEIASR_BRIDGE_ABI;
}

extern "C" int gow3_leiasr_create(const uint8_t luid[8], void* hwnd, uint32_t wait_ms,
                                  Gow3LeiaLogFn log, Gow3LeiaDisplay* display) {
    gow3_leiasr_destroy();
    state.log = log;
    if (!CreateDevice(luid) || !CreateContext(wait_ms)) {
        gow3_leiasr_destroy();
        return 1;
    }
    if (display) {
        *display = {};
        try {
            if (SR::IDisplayManager* manager = SR::TryGetDisplayManagerInstance(*state.context)) {
                if (SR::IDisplay* sr_display = manager->getPrimaryActiveSRDisplay();
                    sr_display && sr_display->isValid()) {
                    display->view_width = UINT(sr_display->getRecommendedViewsTextureWidth());
                    display->view_height = UINT(sr_display->getRecommendedViewsTextureHeight());
                    const SR_recti location = sr_display->getLocation();
                    display->left = int32_t(location.left);
                    display->top = int32_t(location.top);
                    display->right = int32_t(location.right);
                    display->bottom = int32_t(location.bottom);
                }
            }
        } catch (...) {
            Log(1, "the SR display could not be queried");
        }
    }
    try {
        const WeaverErrorCode result = SR::CreateDX11Weaver(
            state.context, state.immediate, static_cast<HWND>(hwnd), &state.weaver);
        if (result != WeaverErrorCode::WeaverSuccess || !state.weaver) {
            Log(1, "CreateDX11Weaver failed (%d)", int(result));
            gow3_leiasr_destroy();
            return 1;
        }
        // The textures hold sRGB-encoded values in UNORM: the weaver decodes and encodes.
        state.weaver->setShaderSRGBConversion(true, true);
        state.lens = SR::SwitchableLensHint::create(*state.context);
        state.context->initialize();
    } catch (...) {
        Log(1, "the weaver could not be created");
        gow3_leiasr_destroy();
        return 1;
    }
    gow3_leiasr_lens(1);
    return 0;
}

extern "C" int gow3_leiasr_resize(uint32_t views_width, uint32_t views_height,
                                  uint32_t output_width, uint32_t output_height,
                                  Gow3LeiaTextures* textures) {
    if (!state.weaver || !textures) return 1;
    ReleaseTextures();
    state.views = SharedTexture(views_width, views_height);
    state.output = SharedTexture(output_width, output_height);
    if (!state.views || !state.output) {
        ReleaseTextures();
        return 1;
    }
    state.device->CreateShaderResourceView(state.views, nullptr, &state.views_srv);
    state.device->CreateRenderTargetView(state.output, nullptr, &state.output_rtv);
    state.views_handle = Share(state.views);
    state.output_handle = Share(state.output);
    if (!state.views_srv || !state.output_rtv || !state.views_handle || !state.output_handle) {
        Log(1, "the shared textures could not be shared");
        ReleaseTextures();
        return 1;
    }
    state.output_width = output_width;
    state.output_height = output_height;
    try {
        // The width of one view: both are side by side.
        state.weaver->setInputViewTexture(state.views_srv, int(views_width / 2), int(views_height),
                                          DXGI_FORMAT_B8G8R8A8_UNORM);
    } catch (...) {
        Log(1, "setInputViewTexture failed");
        ReleaseTextures();
        return 1;
    }
    textures->views_handle = state.views_handle;
    textures->output_handle = state.output_handle;
    return 0;
}

extern "C" int gow3_leiasr_weave(void) {
    if (!state.weaver || !state.output_rtv) return 1;
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, FLOAT(state.output_width),
                                  FLOAT(state.output_height), 0.0f, 1.0f};
    state.immediate->OMSetRenderTargets(1, &state.output_rtv, nullptr);
    state.immediate->RSSetViewports(1, &viewport);
    try {
        state.weaver->weave();
    } catch (...) {
        Log(1, "weave failed");
        return 1;
    }
    state.immediate->End(state.done);
    state.immediate->Flush();
    while (state.immediate->GetData(state.done, nullptr, 0, 0) == S_FALSE) {
        std::this_thread::yield();
    }
    return 0;
}

extern "C" void gow3_leiasr_lens(int on) {
    if (!state.lens || state.lens_on == on) return;
    state.lens_on = on;
    try {
        on ? state.lens->enable() : state.lens->disable();
    } catch (...) {
        Log(1, "the lens hint failed");
    }
}

extern "C" void gow3_leiasr_destroy(void) {
    ReleaseTextures();
    if (state.weaver) {
        state.weaver->destroy();
        state.weaver = nullptr;
    }
    state.lens = nullptr; // owned by the context
    state.lens_on = -1;
    if (state.context) {
        SR::SRContext::deleteSRContext(state.context);
        state.context = nullptr;
    }
    Release(state.done);
    Release(state.immediate);
    Release(state.device);
}

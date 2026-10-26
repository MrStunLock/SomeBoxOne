#include "d3d11_presenter_win32.hpp"

#ifdef _WIN32

#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <iterator>
#include <initializer_list>
#include <utility>
#include <sstream>

namespace someboxone::graphics {
namespace {

struct Vertex {
    float x;
    float y;
    float u;
    float v;
};

constexpr std::array<Vertex, 4> kFullscreenQuad{{
    {-1.0F,  1.0F, 0.0F, 0.0F},
    { 1.0F,  1.0F, 1.0F, 0.0F},
    {-1.0F, -1.0F, 0.0F, 1.0F},
    { 1.0F, -1.0F, 1.0F, 1.0F},
}};

constexpr char kVertexShaderSource[] = R"hlsl(
struct VSInput { float2 position : POSITION; float2 uv : TEXCOORD0; };
struct VSOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
VSOutput main(VSInput input) {
    VSOutput output;
    output.position = float4(input.position, 0.0f, 1.0f);
    output.uv = input.uv;
    return output;
}
)hlsl";

constexpr char kPixelShaderSource[] = R"hlsl(
Texture2D frameTexture : register(t0);
SamplerState frameSampler : register(s0);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return frameTexture.Sample(frameSampler, uv);
}
)hlsl";

std::wstring hr_text(HRESULT hr) {
    std::wostringstream stream;
    stream << L"HRESULT 0x" << std::hex << std::uppercase
           << static_cast<unsigned long>(hr);
    return stream.str();
}

} // namespace

D3D11Presenter::~D3D11Presenter() {
    shutdown();
}

void D3D11Presenter::set_error_text(std::wstring message) {
    last_error_ = std::move(message);
}

void D3D11Presenter::set_error(const wchar_t* operation, HRESULT result) {
    last_error_ = operation;
    last_error_ += L" failed (";
    last_error_ += hr_text(result);
    last_error_ += L")";
}

void D3D11Presenter::clear_device_objects() noexcept {
    frame_view_.Reset();
    frame_texture_.Reset();
    sampler_.Reset();
    vertex_buffer_.Reset();
    input_layout_.Reset();
    pixel_shader_.Reset();
    vertex_shader_.Reset();
    render_target_.Reset();
    swap_chain_.Reset();
    if (context_) context_->ClearState();
    context_.Reset();
    device_.Reset();
    frame_width_ = 0;
    frame_height_ = 0;
    back_buffer_width_ = 0;
    back_buffer_height_ = 0;
    using_warp_ = false;
}

void D3D11Presenter::shutdown() noexcept {
    clear_device_objects();
    window_ = nullptr;
}

bool D3D11Presenter::create_device(D3D_DRIVER_TYPE driver_type) {
    constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    constexpr D3D_FEATURE_LEVEL levels_with_11_1[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    constexpr D3D_FEATURE_LEVEL levels_legacy[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selected{};
    HRESULT hr = D3D11CreateDevice(nullptr, driver_type, nullptr, flags,
        levels_with_11_1, static_cast<UINT>(std::size(levels_with_11_1)),
        D3D11_SDK_VERSION, &device_, &selected, &context_);
    // Windows 7-era D3D runtimes reject the 11_1 feature-level token with
    // E_INVALIDARG. Retry without that token instead of treating it as fatal.
    if (hr == E_INVALIDARG) {
        device_.Reset();
        context_.Reset();
        hr = D3D11CreateDevice(nullptr, driver_type, nullptr, flags,
            levels_legacy, static_cast<UINT>(std::size(levels_legacy)),
            D3D11_SDK_VERSION, &device_, &selected, &context_);
    }
    if (FAILED(hr)) {
        set_error(L"D3D11CreateDevice", hr);
        device_.Reset();
        context_.Reset();
        return false;
    }
    return true;
}

bool D3D11Presenter::create_swap_chain() {
    if (!device_ || !window_) return false;

    RECT client{};
    if (!GetClientRect(window_, &client)) {
        set_error(L"GetClientRect", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    const auto client_width = static_cast<std::uint32_t>((std::max<LONG>)(client.right - client.left, 1));
    const auto client_height = static_cast<std::uint32_t>((std::max<LONG>)(client.bottom - client.top, 1));

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    HRESULT hr = device_.As(&dxgi_device);
    if (FAILED(hr)) { set_error(L"QueryInterface(IDXGIDevice)", hr); return false; }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(&adapter);
    if (FAILED(hr)) { set_error(L"IDXGIDevice::GetAdapter", hr); return false; }

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory2;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory2));
    if (SUCCEEDED(hr)) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = client_width;
        desc.Height = client_height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Stereo = FALSE;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags = 0;

        Microsoft::WRL::ComPtr<IDXGISwapChain1> chain1;
        hr = factory2->CreateSwapChainForHwnd(device_.Get(), window_, &desc,
            nullptr, nullptr, &chain1);
        if (FAILED(hr)) {
            // Flip sequential is broadly supported on systems that lack flip-discard.
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            hr = factory2->CreateSwapChainForHwnd(device_.Get(), window_, &desc,
                nullptr, nullptr, &chain1);
        }
        if (SUCCEEDED(hr)) {
            hr = chain1.As(&swap_chain_);
            if (FAILED(hr)) { set_error(L"QueryInterface(IDXGISwapChain)", hr); return false; }
        }
    }

    if (!swap_chain_) {
        // Last-resort legacy DXGI swap chain for older runtimes/drivers.
        Microsoft::WRL::ComPtr<IDXGIFactory> factory;
        hr = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) { set_error(L"IDXGIAdapter::GetParent(IDXGIFactory)", hr); return false; }
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = client_width;
        desc.BufferDesc.Height = client_height;
        desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 1;
        desc.OutputWindow = window_;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = factory->CreateSwapChain(device_.Get(), &desc, &swap_chain_);
        if (FAILED(hr)) { set_error(L"IDXGIFactory::CreateSwapChain", hr); return false; }
        (void)factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);
    } else {
        Microsoft::WRL::ComPtr<IDXGIFactory> factory;
        if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))))
            (void)factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);
    }

    back_buffer_width_ = client_width;
    back_buffer_height_ = client_height;
    return true;
}

bool D3D11Presenter::create_pipeline() {
    if (!device_) return false;

    Microsoft::WRL::ComPtr<ID3DBlob> vertex_bytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(kVertexShaderSource, sizeof(kVertexShaderSource) - 1,
        nullptr, nullptr, nullptr, "main", "vs_4_0", D3DCOMPILE_ENABLE_STRICTNESS,
        0, &vertex_bytecode, &errors);
    if (FAILED(hr)) {
        set_error_text(L"D3DCompile(vertex shader) failed");
        return false;
    }

    hr = device_->CreateVertexShader(vertex_bytecode->GetBufferPointer(),
        vertex_bytecode->GetBufferSize(), nullptr, &vertex_shader_);
    if (FAILED(hr)) { set_error(L"ID3D11Device::CreateVertexShader", hr); return false; }

    constexpr D3D11_INPUT_ELEMENT_DESC input_elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = device_->CreateInputLayout(input_elements,
        static_cast<UINT>(std::size(input_elements)), vertex_bytecode->GetBufferPointer(),
        vertex_bytecode->GetBufferSize(), &input_layout_);
    if (FAILED(hr)) { set_error(L"ID3D11Device::CreateInputLayout", hr); return false; }

    Microsoft::WRL::ComPtr<ID3DBlob> pixel_bytecode;
    errors.Reset();
    hr = D3DCompile(kPixelShaderSource, sizeof(kPixelShaderSource) - 1,
        nullptr, nullptr, nullptr, "main", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS,
        0, &pixel_bytecode, &errors);
    if (FAILED(hr)) {
        set_error_text(L"D3DCompile(pixel shader) failed");
        return false;
    }
    hr = device_->CreatePixelShader(pixel_bytecode->GetBufferPointer(),
        pixel_bytecode->GetBufferSize(), nullptr, &pixel_shader_);
    if (FAILED(hr)) { set_error(L"ID3D11Device::CreatePixelShader", hr); return false; }

    D3D11_BUFFER_DESC vertex_desc{};
    vertex_desc.ByteWidth = static_cast<UINT>(sizeof(kFullscreenQuad));
    vertex_desc.Usage = D3D11_USAGE_IMMUTABLE;
    vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vertex_data{};
    vertex_data.pSysMem = kFullscreenQuad.data();
    hr = device_->CreateBuffer(&vertex_desc, &vertex_data, &vertex_buffer_);
    if (FAILED(hr)) { set_error(L"ID3D11Device::CreateBuffer(vertex buffer)", hr); return false; }

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device_->CreateSamplerState(&sampler_desc, &sampler_);
    if (FAILED(hr)) { set_error(L"ID3D11Device::CreateSamplerState", hr); return false; }
    return true;
}

bool D3D11Presenter::create_render_target() {
    if (!device_ || !swap_chain_) return false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    HRESULT hr = swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (FAILED(hr)) { set_error(L"IDXGISwapChain::GetBuffer", hr); return false; }
    hr = device_->CreateRenderTargetView(back_buffer.Get(), nullptr, &render_target_);
    if (FAILED(hr)) { set_error(L"ID3D11Device::CreateRenderTargetView", hr); return false; }
    D3D11_TEXTURE2D_DESC desc{};
    back_buffer->GetDesc(&desc);
    back_buffer_width_ = desc.Width;
    back_buffer_height_ = desc.Height;
    return true;
}

bool D3D11Presenter::initialize(HWND window) {
    shutdown();
    if (!window || !IsWindow(window)) {
        set_error_text(L"Cannot initialize D3D11/DXGI without a valid HWND");
        return false;
    }
    window_ = window;

    bool created = false;
    for (const D3D_DRIVER_TYPE driver : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        clear_device_objects();
        if (!create_device(driver)) continue;
        if (!create_swap_chain() || !create_pipeline() || !create_render_target()) {
            clear_device_objects();
            continue;
        }
        using_warp_ = driver == D3D_DRIVER_TYPE_WARP;
        created = true;
        break;
    }
    if (!created) {
        // Preserve the most recent underlying HRESULT/error for the GDI fallback UI.
        window_ = window;
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizer_desc{};
    rasterizer_desc.FillMode = D3D11_FILL_SOLID;
    rasterizer_desc.CullMode = D3D11_CULL_NONE;
    rasterizer_desc.DepthClipEnable = TRUE;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer;
    const HRESULT rasterizer_hr = device_->CreateRasterizerState(&rasterizer_desc, &rasterizer);
    if (SUCCEEDED(rasterizer_hr)) context_->RSSetState(rasterizer.Get());
    last_error_.clear();
    return true;
}

bool D3D11Presenter::resize(std::uint32_t width, std::uint32_t height) {
    if (!ready()) return false;
    if (width == 0 || height == 0) return true; // minimized; retry on restore
    if (width > 16384U || height > 16384U) {
        set_error_text(L"Requested swap-chain size exceeds D3D11 texture limits");
        return false;
    }
    if (width == back_buffer_width_ && height == back_buffer_height_) return true;

    // The immediate context retains bound RTV references; unbind before ResizeBuffers.
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    render_target_.Reset();
    HRESULT hr = swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) { set_error(L"IDXGISwapChain::ResizeBuffers", hr); return false; }
    return create_render_target();
}

bool D3D11Presenter::ensure_frame_texture(std::uint32_t width, std::uint32_t height) {
    if (frame_texture_ && frame_width_ == width && frame_height_ == height && frame_view_) return true;
    frame_view_.Reset();
    frame_texture_.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &frame_texture_);
    if (FAILED(hr)) { set_error(L"ID3D11Device::CreateTexture2D(guest frame)", hr); return false; }
    hr = device_->CreateShaderResourceView(frame_texture_.Get(), nullptr, &frame_view_);
    if (FAILED(hr)) { set_error(L"ID3D11Device::CreateShaderResourceView(guest frame)", hr); frame_texture_.Reset(); return false; }
    frame_width_ = width;
    frame_height_ = height;
    return true;
}

bool D3D11Presenter::upload_frame(const FrameBuffer& frame) {
    if (!ready()) return false;
    const std::uint64_t expected = static_cast<std::uint64_t>(frame.width()) * frame.height();
    if (frame.empty() || frame.width() == 0 || frame.height() == 0 ||
        frame.width() > FrameBuffer::kMaxDimension || frame.height() > FrameBuffer::kMaxDimension ||
        expected != frame.pixels().size() || expected > FrameBuffer::kMaxPixels) {
        set_error_text(L"Rejected invalid or oversized guest framebuffer");
        return false;
    }
    if (!ensure_frame_texture(frame.width(), frame.height())) return false;
    const auto row_pitch64 = static_cast<std::uint64_t>(frame.width()) * sizeof(std::uint32_t);
    if (row_pitch64 > std::numeric_limits<UINT>::max()) {
        set_error_text(L"Guest framebuffer row pitch exceeds D3D11 limits");
        return false;
    }
    context_->UpdateSubresource(frame_texture_.Get(), 0, nullptr, frame.pixels().data(),
        static_cast<UINT>(row_pitch64), 0);
    return true;
}

bool D3D11Presenter::render() {
    if (!ready()) return false;
    RECT client{};
    if (!GetClientRect(window_, &client)) {
        set_error(L"GetClientRect", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    const LONG client_width = client.right - client.left;
    const LONG client_height = client.bottom - client.top;
    if (client_width <= 0 || client_height <= 0) return true;
    if (!resize(static_cast<std::uint32_t>(client_width), static_cast<std::uint32_t>(client_height))) return false;

    constexpr FLOAT clear_color[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    context_->OMSetRenderTargets(1, render_target_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(render_target_.Get(), clear_color);

    if (frame_view_) {
        // Preserve the guest's aspect ratio and center it in the client area. This
        // avoids stretching 16:9 console frames into arbitrary window dimensions.
        const float frame_aspect = static_cast<float>(frame_width_) / static_cast<float>(frame_height_);
        const float client_aspect = static_cast<float>(client_width) / static_cast<float>(client_height);
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<FLOAT>(client_width);
        viewport.Height = static_cast<FLOAT>(client_height);
        if (client_aspect > frame_aspect) {
            viewport.Width = viewport.Height * frame_aspect;
            viewport.TopLeftX = (static_cast<FLOAT>(client_width) - viewport.Width) * 0.5F;
        } else {
            viewport.Height = viewport.Width / frame_aspect;
            viewport.TopLeftY = (static_cast<FLOAT>(client_height) - viewport.Height) * 0.5F;
        }
        viewport.MinDepth = 0.0F;
        viewport.MaxDepth = 1.0F;
        context_->RSSetViewports(1, &viewport);
        constexpr UINT stride = static_cast<UINT>(sizeof(Vertex));
        constexpr UINT offset = 0;
        ID3D11Buffer* vertex_buffer = vertex_buffer_.Get();
        context_->IASetInputLayout(input_layout_.Get());
        context_->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
        context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* frame_view = frame_view_.Get();
        context_->PSSetShaderResources(0, 1, &frame_view);
        ID3D11SamplerState* sampler = sampler_.Get();
        context_->PSSetSamplers(0, 1, &sampler);
        context_->Draw(static_cast<UINT>(kFullscreenQuad.size()), 0);
        ID3D11ShaderResourceView* null_view = nullptr;
        context_->PSSetShaderResources(0, 1, &null_view);
    }

    const HRESULT hr = swap_chain_->Present(1, 0);
    if (FAILED(hr)) { set_error(L"IDXGISwapChain::Present", hr); return false; }
    return true;
}

} // namespace someboxone::graphics

#endif // _WIN32

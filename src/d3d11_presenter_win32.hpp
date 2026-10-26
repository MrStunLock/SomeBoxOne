#pragma once

#ifdef _WIN32

#include "framebuffer.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace someboxone::graphics {

// Host-side D3D11/DXGI presenter for complete CPU frames submitted through the
// shared framebuffer transport. This is deliberately not a guest D3D API emulator.
class D3D11Presenter {
public:
    D3D11Presenter() = default;
    D3D11Presenter(const D3D11Presenter&) = delete;
    D3D11Presenter& operator=(const D3D11Presenter&) = delete;
    ~D3D11Presenter();

    bool initialize(HWND window);
    void shutdown() noexcept;
    bool resize(std::uint32_t width, std::uint32_t height);
    bool upload_frame(const FrameBuffer& frame);
    bool render();

    [[nodiscard]] bool ready() const noexcept { return device_ && swap_chain_ && render_target_; }
    [[nodiscard]] bool using_warp() const noexcept { return using_warp_; }
    [[nodiscard]] const std::wstring& last_error() const noexcept { return last_error_; }

private:
    bool create_device(D3D_DRIVER_TYPE driver_type);
    bool create_swap_chain();
    bool create_pipeline();
    bool create_render_target();
    bool ensure_frame_texture(std::uint32_t width, std::uint32_t height);
    void set_error(const wchar_t* operation, HRESULT result);
    void set_error_text(std::wstring message);
    void clear_device_objects() noexcept;

    HWND window_{};
    std::uint32_t back_buffer_width_{};
    std::uint32_t back_buffer_height_{};
    std::uint32_t frame_width_{};
    std::uint32_t frame_height_{};
    bool using_warp_{};
    std::wstring last_error_;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> input_layout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> frame_texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> frame_view_;
};

} // namespace someboxone::graphics

#endif // _WIN32

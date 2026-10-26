#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "d3d11_presenter_win32.hpp"
#include "framebuffer.hpp"
#include "framebuffer_ipc.hpp"
#include "runtime_config.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <sstream>
#include <utility>

namespace {
constexpr wchar_t kWindowClass[] = L"SomeBoxOneGameDisplayWindow";
constexpr wchar_t kWindowTitle[] = L"SomeBoxOne - Guest Display";
someboxone::graphics::FrameBuffer g_frame(1280, 720);
someboxone::graphics::SharedFrameMapping g_shared_frames;
someboxone::graphics::D3D11Presenter g_presenter;
std::uint64_t g_last_frame_id = 0;
bool g_transport_open = false;
bool g_have_guest_frame = false;
bool g_fullscreen = false;
bool g_d3d_ready = false;
bool g_test_pattern_mode = false;
std::string g_guest_status;
std::wstring g_current_game_title;
std::uint32_t g_game_info_generation = 0;
std::wstring g_renderer_status = L"D3D11/DXGI presenter has not initialized yet.";

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return L"Game";
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                            result.data(), count) != count) return L"Game";
    return result;
}

std::wstring game_window_title() {
    return L"SomeBoxOne - " + (g_current_game_title.empty() ? std::wstring(L"Guest Display") : g_current_game_title);
}

bool load_splash_frame(const std::wstring& path, someboxone::graphics::FrameBuffer& destination) {
    if (path.empty()) return false;
    const HRESULT init_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(init_result);
    if (FAILED(init_result) && init_result != RPC_E_CHANGED_MODE) return false;

    bool loaded = false;
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf()));
    if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                                WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf());
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, frame.GetAddressOf());
    UINT width = 0, height = 0;
    if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (SUCCEEDED(hr) && (width == 0 || height == 0 || width > someboxone::graphics::FrameBuffer::kMaxDimension ||
        height > someboxone::graphics::FrameBuffer::kMaxDimension || pixels > someboxone::graphics::FrameBuffer::kMaxPixels)) {
        hr = E_INVALIDARG;
    }
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(converter.GetAddressOf());
    if (SUCCEEDED(hr)) hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr) && destination.resize(width, height)) {
        const UINT stride = width * 4U;
        const UINT bytes = stride * height;
        hr = converter->CopyPixels(nullptr, stride, bytes,
            reinterpret_cast<BYTE*>(destination.writable_pixels().data()));
        loaded = SUCCEEDED(hr);
    }
    // Release WIC interfaces while the COM apartment is still initialized.
    converter.Reset();
    frame.Reset();
    decoder.Reset();
    factory.Reset();
    if (!loaded) destination = someboxone::graphics::FrameBuffer();
    if (uninitialize) CoUninitialize();
    return loaded;
}

void draw_overlay(HDC dc, const RECT& client) {
    const int panel_height = 92;
    RECT panel{18, 18, (std::min<LONG>)(client.right - 18, static_cast<LONG>(760)), 18 + panel_height};
    HBRUSH brush = CreateSolidBrush(RGB(17, 24, 39));
    if (brush) {
        FillRect(dc, &panel, brush);
        DeleteObject(brush);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    HFONT title_font = CreateFontW(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ old_font = title_font ? SelectObject(dc, title_font) : nullptr;
    const wchar_t* title = L"SOMEBOXONE  /  D3D11 + DXGI HOST PRESENTER";
    TextOutW(dc, 32, 30, title, lstrlenW(title));
    if (old_font) SelectObject(dc, old_font);
    if (title_font) DeleteObject(title_font);
    SetTextColor(dc, RGB(210, 222, 239));

    std::wstring detail;
    if (g_test_pattern_mode) {
        detail = L"Local diagnostic pattern enabled (not game output).";
    } else if (g_have_guest_frame) {
        detail = L"Guest frames received: " + std::to_wstring(g_last_frame_id);
    } else if (!g_guest_status.empty()) {
        detail = utf8_to_wide(g_guest_status);
    } else if (g_transport_open) {
        detail = L"Framebuffer transport ready; waiting for guest renderer to submit its first frame.";
    } else {
        detail = L"Framebuffer transport unavailable; no guest frame can be presented.";
    }
    TextOutW(dc, 32, 61, detail.c_str(), static_cast<int>(detail.size()));
    SetTextColor(dc, RGB(255, 205, 125));
    const std::wstring status = g_renderer_status;
    TextOutW(dc, 32, 82, status.c_str(), static_cast<int>(status.size()));
}

void use_gdi_fallback(HWND hwnd, const std::wstring& reason) {
    g_d3d_ready = false;
    g_renderer_status = L"GDI fallback: " + reason;
    g_presenter.shutdown();
    SetWindowTextW(hwnd, game_window_title().c_str());
}

void update_guest_status(HWND hwnd) {
    if (g_test_pattern_mode) return;
    std::string status;
    if (!g_shared_frames.read_status(status) || status == g_guest_status) return;
    g_guest_status = std::move(status);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void update_game_info(HWND hwnd) {
    if (g_test_pattern_mode) return;
    std::string display_name;
    std::string splash_path;
    std::uint32_t generation = 0;
    if (!g_shared_frames.read_game_info(display_name, splash_path, generation) || generation == g_game_info_generation) return;

    g_game_info_generation = generation;
    g_current_game_title = utf8_to_wide(display_name);
    if (g_current_game_title.empty()) g_current_game_title = L"Guest Display";
    SetWindowTextW(hwnd, game_window_title().c_str());

    // A new title invalidates the old game's last frame. This lets the splash stay
    // visible until the new guest actually submits its first frame.
    g_last_frame_id = g_shared_frames.current_frame_id();
    g_have_guest_frame = false;
    g_guest_status.clear();

    someboxone::graphics::FrameBuffer splash;
    if (!splash_path.empty()) {
        const std::wstring wide_path = utf8_to_wide(splash_path);
        if (load_splash_frame(wide_path, splash)) {
            g_frame = std::move(splash);
        } else {
            g_frame.clear(0);
        }
    } else {
        g_frame.clear(0);
    }
    if (g_d3d_ready && !g_frame.empty() && !g_presenter.upload_frame(g_frame)) {
        const std::wstring reason = g_presenter.last_error();
        use_gdi_fallback(hwnd, reason.empty() ? L"splash/frame upload failed" : reason);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void paint_gdi_frame(HDC dc, const RECT& client) {
    // Paint the letterbox bars black before scaling the fallback image.
    HBRUSH background = CreateSolidBrush(RGB(0, 0, 0));
    if (background) {
        FillRect(dc, &client, background);
        DeleteObject(background);
    }
    if (!g_frame.empty()) {
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = static_cast<LONG>(g_frame.width());
        bitmap.bmiHeader.biHeight = -static_cast<LONG>(g_frame.height()); // top-down rows
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        const auto pixels = g_frame.pixels();
        const double frame_aspect = static_cast<double>(g_frame.width()) / static_cast<double>(g_frame.height());
        const double client_aspect = client.bottom > 0
            ? static_cast<double>(client.right) / static_cast<double>(client.bottom) : frame_aspect;
        int draw_width = client.right;
        int draw_height = client.bottom;
        if (client_aspect > frame_aspect) {
            draw_width = static_cast<int>(static_cast<double>(draw_height) * frame_aspect);
        } else if (client_aspect < frame_aspect) {
            draw_height = static_cast<int>(static_cast<double>(draw_width) / frame_aspect);
        }
        const int draw_x = (client.right - draw_width) / 2;
        const int draw_y = (client.bottom - draw_height) / 2;
        (void)StretchDIBits(dc, draw_x, draw_y, draw_width, draw_height,
            0, 0, static_cast<int>(g_frame.width()), static_cast<int>(g_frame.height()),
            pixels.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
    }
    draw_overlay(dc, client);
}

LRESULT CALLBACK display_window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_transport_open = g_shared_frames.open();
        g_d3d_ready = g_presenter.initialize(hwnd);
        if (g_d3d_ready) {
            g_renderer_status = g_presenter.using_warp()
                ? L"D3D11/DXGI scan-out ready using WARP software rendering."
                : L"D3D11/DXGI scan-out ready using the hardware adapter.";
            SetWindowTextW(hwnd, game_window_title().c_str());
        } else {
            g_renderer_status = L"D3D11/DXGI unavailable: " + g_presenter.last_error();
            g_renderer_status += L"; using GDI fallback.";
            SetWindowTextW(hwnd, game_window_title().c_str());
        }
        if (g_test_pattern_mode) {
            // Explicit host-only smoke test for the D3D11/DXGI scan-out path. This is
            // never enabled during normal game launches and is not presented as game output.
            g_frame.draw_test_pattern();
            g_have_guest_frame = true;
            if (g_d3d_ready && !g_presenter.upload_frame(g_frame)) {
                const std::wstring reason = g_presenter.last_error();
                use_gdi_fallback(hwnd, reason.empty() ? L"test-pattern upload failed" : reason);
            }
            SetWindowTextW(hwnd, g_d3d_ready
                ? L"SomeBoxOne — D3D11/DXGI TEST PATTERN (not game output)"
                : L"SomeBoxOne — GDI TEST PATTERN (not game output)");
        }
        SetTimer(hwnd, 1, 16, nullptr); // poll the shared frame queue at approximately 60 Hz
        return 0;
    case WM_TIMER:
        if (wParam == 1) update_game_info(hwnd);
        if (wParam == 1) update_guest_status(hwnd);
        if (wParam == 1 && !g_test_pattern_mode && g_shared_frames.read_latest(g_frame, g_last_frame_id)) {
            g_have_guest_frame = true;
            if (g_d3d_ready && !g_presenter.upload_frame(g_frame)) {
                const std::wstring reason = g_presenter.last_error();
                use_gdi_fallback(hwnd, reason.empty() ? L"frame upload failed" : reason);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_SIZE:
        if (g_d3d_ready && wParam != SIZE_MINIMIZED) {
            const auto width = static_cast<std::uint32_t>(LOWORD(lParam));
            const auto height = static_cast<std::uint32_t>(HIWORD(lParam));
            if (width != 0 && height != 0 && !g_presenter.resize(width, height)) {
                const std::wstring reason = g_presenter.last_error();
                use_gdi_fallback(hwnd, reason.empty() ? L"swap-chain resize failed" : reason);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);

        if (g_d3d_ready) {
            // Before the first guest frame, present a black DXGI back buffer and keep
            // status in the title bar. GDI overlays are used only by the fallback path.
            if (!g_presenter.render()) {
                const std::wstring reason = g_presenter.last_error();
                use_gdi_fallback(hwnd, reason.empty() ? L"DXGI present failed" : reason);
                paint_gdi_frame(dc, client);
            }
        } else {
            paint_gdi_frame(dc, client);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        g_presenter.shutdown();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show_command) {
    auto config = someboxone::load_runtime_config();
    std::uint32_t width = config.graphics_width;
    std::uint32_t height = config.graphics_height;
    g_fullscreen = config.graphics_fullscreen;
    // Parse window options without depending on shell command-line helpers.
    std::wistringstream arguments(command_line ? command_line : L"");
    std::wstring argument;
    while (arguments >> argument) {
        if (argument.rfind(L"--width=", 0) == 0) {
            try { width = static_cast<std::uint32_t>(std::stoul(argument.substr(8))); } catch (...) {}
        } else if (argument.rfind(L"--height=", 0) == 0) {
            try { height = static_cast<std::uint32_t>(std::stoul(argument.substr(9))); } catch (...) {}
        } else if (argument == L"--fullscreen") {
            g_fullscreen = true;
        } else if (argument == L"--windowed") {
            g_fullscreen = false;
        } else if (argument == L"--test-pattern") {
            g_test_pattern_mode = true;
        }
    }
    if (width < 320 || width > 8192 || height < 240 || height > 8192 ||
        static_cast<std::uint64_t>(width) * height > 16ULL * 1024ULL * 1024ULL || !g_frame.resize(width, height)) {
        width = 1280;
        height = 720;
        g_fullscreen = false;
        (void)g_frame.resize(width, height);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = display_window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 1;

    const DWORD style = g_fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    RECT desired{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    if (!g_fullscreen && !AdjustWindowRect(&desired, style, FALSE)) return 1;
    const int x = g_fullscreen ? 0 : CW_USEDEFAULT;
    const int y = g_fullscreen ? 0 : CW_USEDEFAULT;
    const int window_width = g_fullscreen ? GetSystemMetrics(SM_CXSCREEN) : desired.right - desired.left;
    const int window_height = g_fullscreen ? GetSystemMetrics(SM_CYSCREEN) : desired.bottom - desired.top;
    HWND hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle, style,
        x, y, window_width, window_height, nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show_command);
    UpdateWindow(hwnd);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

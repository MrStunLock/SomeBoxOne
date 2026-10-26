#pragma once
// SomeBoxOne v0.23.0 - software-capture Direct3D 11 (D3D11.x) device model.
//
// This is NOT a GPU emulator. It provides guest-visible ID3D11Device / ID3D11DeviceContext /
// resource objects with the documented vtable layouts so a title can initialise and run its
// render loop. Resource memory lives in guest memory, so a title that renders in software and
// uploads a framebuffer texture (as RSDKv5 does) can be captured at Unmap/UpdateSubresource/Present.
// Draw calls are counted but not rasterised.

namespace someboxone {

inline unsigned dxgi_bytes_per_pixel(std::uint32_t f) {
    switch (f) {
    case 2: case 3: case 4: case 5: return 16;                          // R32G32B32A32
    case 10: case 11: case 12: case 13: return 8;                       // R16G16B16A16
    case 24: case 26: case 28: case 29: case 30: case 31: case 87: case 88: case 91: case 93: return 4;
    case 40: case 41: case 42: case 43: return 4;                       // D32/R32
    case 54: case 55: case 56: case 57: case 58: case 59: return 2;     // R16 / D16
    case 61: case 62: case 63: case 64: case 65: return 1;              // R8 / A8
    case 85: case 86: case 115: return 2;                               // B5G6R5, B5G5R5A1, B4G4R4A4
    default: return 4;
    }
}

inline std::uint64_t XboxTitleRuntime::d3d_make_child(const std::string& cls, const std::string& kind, D3dResource res) {
    ComMethods m; // ID3D11DeviceChild (3..6) + ID3D11Resource (7..9) + Desc getter (10)
    m.emplace_back("GetDevice", [this](GuestCpu& c) { mem_.write<std::uint64_t>(c.arg(1), d3d_device_); c.r[0] = 0; });
    m.emplace_back("GetPrivateData", [](GuestCpu& c) { c.r[0] = 0; });
    m.emplace_back("SetPrivateData", [](GuestCpu& c) { c.r[0] = 0; });
    m.emplace_back("SetPrivateDataInterface", [](GuestCpu& c) { c.r[0] = 0; });
    if (xdk2012_layout() && kind == "view") {
        // VS2012-era XDK: ID3D11DeviceChild carries one extra method, so View::GetResource(ID3D11Resource**) is slot 8 (not 7).
        m.emplace_back("XdkChildExtra1", [](GuestCpu& c) { c.r[0] = 0; });                                                  // 7
        m.emplace_back("GetResource", [this, res](GuestCpu& c) {                                                            // 8
            if (c.arg(1) && mem_.is_mapped(c.arg(1), 8)) { mem_.write<std::uint64_t>(c.arg(1), res.resource); if (res.resource) mem_.write<std::uint64_t>(res.resource + 8, mem_.read<std::uint64_t>(res.resource + 8) + 1); }
            c.r[0] = 0; });
        m.emplace_back("GetDesc", [](GuestCpu& c) { c.r[0] = 0; });                                                         // 9 (view desc: left as the caller initialised it)
    } else {
        m.emplace_back("GetType", [this](GuestCpu& c) { mem_.write<std::uint32_t>(c.arg(1), 3); }); // D3D11_RESOURCE_DIMENSION_TEXTURE2D
        m.emplace_back("SetEvictionPriority", [](GuestCpu&) {});
        m.emplace_back("GetEvictionPriority", [](GuestCpu& c) { c.r[0] = 0; });
        // VS2012-era XDK: resources carry two extra methods before GetDesc (the title calls Texture2D::GetDesc at slot 12, not 10).
        if (xdk2012_layout()) { m.insert(m.begin() + 4, std::make_pair(std::string("XdkChildExtra1"), Handler([](GuestCpu& c) { c.r[0] = 0; })));
                                m.insert(m.begin() + 5, std::make_pair(std::string("XdkChildExtra2"), Handler([](GuestCpu& c) { c.r[0] = 0; }))); }
        // Texture2D::GetDesc(D3D11_TEXTURE2D_DESC*) - 44 bytes: width, height, mips, array, format, sample(count, quality), usage, bind, cpu, misc.
        m.emplace_back("GetDesc", [this, res](GuestCpu& c) {
            const std::uint64_t d = c.arg(1); if (!d || !mem_.is_mapped(d, 44)) return;
            const std::uint32_t v[11] = {res.width, res.height, 1, 1, res.format, 1, 0, res.usage, res.bind_flags, 0, 0};
            for (int k = 0; k < 11; ++k) mem_.write<std::uint32_t>(d + static_cast<std::uint64_t>(k) * 4, v[k]); });
    }
    const std::uint64_t obj = make_com_object(cls, m, {"*"}, false, 28);
    res.kind = kind;
    d3d_res_[obj] = res;
    return obj;
}

inline void XboxTitleRuntime::d3d_log_once(const std::string& key, const std::string& msg) {
    if (com_log_.insert("d3d:" + key).second) milestone(msg);
}

inline const D3dResource* XboxTitleRuntime::d3d_best_texture() {
    // The framebuffer texture is the CPU-backed texture the title has written most.
    const D3dResource* best = nullptr;
    for (const auto& kv : d3d_res_) { const auto& r = kv.second; if (r.kind == "texture2d" && r.data && r.width >= 64 && r.height >= 64 && r.writes > 0 && (!best || r.writes >= best->writes)) best = &r; }
    return best;
}

inline void XboxTitleRuntime::d3d_capture_frame(const char* why) {
    ++d3d_present_count_;
    if (const auto* r = d3d_best_texture())
        d3d_last_frame_ = "texture " + std::to_string(r->width) + "x" + std::to_string(r->height) + " DXGI format " + std::to_string(r->format) + " (" + why + ")";
}

// Converts the top-left w x h region of a CPU-backed texture to tightly packed RGB or BGRA for host presentation.
inline void XboxTitleRuntime::d3d_rgb_into(const D3dResource& r, unsigned w, unsigned h, bool bgra,
                                                std::vector<std::uint8_t>& out,
                                                std::vector<std::uint8_t>& row) {
    w = (std::min)(w, r.width); h = (std::min)(h, r.height);
    const unsigned bpp = dxgi_bytes_per_pixel(r.format), out_bpp = bgra ? 4 : 3;
    const std::uint32_t pitch = r.pitch ? r.pitch : r.width * bpp;
    out.resize(static_cast<std::size_t>(w) * h * out_bpp);
    row.resize(pitch);
    for (std::uint32_t y = 0; y < h; ++y) {
        mem_.read_bytes(r.data + static_cast<std::uint64_t>(y) * pitch, row.data(), pitch);
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint8_t* p = &row[static_cast<std::size_t>(x) * bpp];
            std::uint8_t R = 0, G = 0, B = 0;
            if (r.format == 85 && bpp == 2) { const unsigned v = p[0] | (p[1] << 8); R = static_cast<std::uint8_t>(((v >> 11) & 31) * 255 / 31); G = static_cast<std::uint8_t>(((v >> 5) & 63) * 255 / 63); B = static_cast<std::uint8_t>((v & 31) * 255 / 31); }
            else if (bpp == 4) { if (r.format == 87 || r.format == 88 || r.format == 91) { B = p[0]; G = p[1]; R = p[2]; } else { R = p[0]; G = p[1]; B = p[2]; } }
            else if (bpp == 2) { R = G = B = p[1]; }
            else if (bpp == 1) { R = G = B = p[0]; }
            std::uint8_t* o = &out[(static_cast<std::size_t>(y) * w + x) * out_bpp];
            if (bgra) { o[0] = B; o[1] = G; o[2] = R; o[3] = 255; } else { o[0] = R; o[1] = G; o[2] = B; }
        }
    }
}

inline std::vector<std::uint8_t> XboxTitleRuntime::d3d_rgb(const D3dResource& r, unsigned w, unsigned h, bool bgra) {
    std::vector<std::uint8_t> out, row;
    d3d_rgb_into(r, w, h, bgra, out, row);
    return out;
}

inline void XboxTitleRuntime::d3d_write_png(const D3dResource& r, const std::filesystem::path& path, unsigned scale) {
    const unsigned w = (std::min)(opt_.view_width, r.width), h = (std::min)(opt_.view_height, r.height);
    write_png_rgb(path, d3d_rgb(r, w, h, false), w, h, scale);
}

// Called once per main-loop iteration (CoreDispatcher.ProcessEvents): periodic PNGs and the live-frame sink.
// The live sink reuses retained pixel/row buffers; callbacks must consume the data synchronously before returning.
inline void XboxTitleRuntime::d3d_frame_outputs() {
    const D3dResource* r = d3d_best_texture();
    if (!r) return;
    if (opt_.save_every && frames_ % opt_.save_every == 0) {
        char name[48]; std::snprintf(name, sizeof name, "frame_%05llu.png", static_cast<unsigned long long>(frames_));
        const auto path = opt_.save_root / name;
        d3d_write_png(*r, path, 2);
        ++d3d_frames_saved_;
        if (d3d_frames_saved_ <= 40) milestone("FRAME SAVED: main-loop iteration " + std::to_string(frames_) + " -> " + PeImage::path_utf8(path));
    }
    if (opt_.frame_sink && opt_.sink_every && frames_ % opt_.sink_every == 0) {
        const unsigned w = (std::min)(opt_.view_width, r->width), h = (std::min)(opt_.view_height, r->height);
        d3d_rgb_into(*r, w, h, true, d3d_live_bgra_, d3d_live_row_);
        if (opt_.show_fps) draw_fps_overlay(d3d_live_bgra_, w, h, pacer_.fps(), opt_.fps_limit);
        opt_.frame_sink(d3d_live_bgra_, w, h, frames_);
    }
}

inline void XboxTitleRuntime::d3d_save_current_png() {
    if (const auto* r = d3d_best_texture()) d3d_write_png(*r, opt_.save_root / "current.png", 2);
}

inline void XboxTitleRuntime::d3d_save_final_frame() {
    if (const auto* r = d3d_best_texture()) {
        const auto path = opt_.save_root / "frame_final.png";
        d3d_write_png(*r, path, 2);
        milestone("FINAL FRAME saved (" + std::to_string(opt_.view_width) + "x" + std::to_string(opt_.view_height) + " active area, 2x) -> " + PeImage::path_utf8(path));
    }
}

inline std::uint64_t XboxTitleRuntime::d3d_create_context() {
    static const char* names[] = {
        "", "", "", "GetDevice", "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface",
        "VSSetConstantBuffers", "PSSetShaderResources", "PSSetShader", "PSSetSamplers", "VSSetShader", "DrawIndexed", "Draw", "Map", "Unmap",
        "PSSetConstantBuffers", "IASetInputLayout", "IASetVertexBuffers", "IASetIndexBuffer", "DrawIndexedInstanced", "DrawInstanced",
        "GSSetConstantBuffers", "GSSetShader", "IASetPrimitiveTopology", "VSSetShaderResources", "VSSetSamplers", "Begin", "End", "GetData",
        "SetPredication", "GSSetShaderResources", "GSSetSamplers", "OMSetRenderTargets", "OMSetRenderTargetsAndUnorderedAccessViews",
        "OMSetBlendState", "OMSetDepthStencilState", "SOSetTargets", "DrawAuto", "DrawIndexedInstancedIndirect", "DrawInstancedIndirect",
        "Dispatch", "DispatchIndirect", "RSSetState", "RSSetViewports", "RSSetScissorRects", "CopySubresourceRegion", "CopyResource",
        "UpdateSubresource", "CopyStructureCount", "ClearRenderTargetView", "ClearUnorderedAccessViewUint", "ClearUnorderedAccessViewFloat",
        "ClearDepthStencilView", "GenerateMips", "SetResourceMinLOD", "GetResourceMinLOD", "ResolveSubresource", "ExecuteCommandList",
        "HSSetShaderResources", "HSSetShader", "HSSetSamplers", "HSSetConstantBuffers", "DSSetShaderResources", "DSSetShader", "DSSetSamplers",
        "DSSetConstantBuffers", "CSSetShaderResources", "CSSetUnorderedAccessViews", "CSSetShader", "CSSetSamplers", "CSSetConstantBuffers",
        "VSGetConstantBuffers", "PSGetShaderResources", "PSGetShader", "PSGetSamplers", "VSGetShader", "PSGetConstantBuffers", "IAGetInputLayout",
        "IAGetVertexBuffers", "IAGetIndexBuffer", "GSGetConstantBuffers", "GSGetShader", "IAGetPrimitiveTopology", "VSGetShaderResources",
        "VSGetSamplers", "VSGetConstantBuffers1", "GetPredication", "GSGetShaderResources", "GSGetSamplers", "OMGetRenderTargets",
        "OMGetRenderTargetsAndUnorderedAccessViews", "OMGetBlendState", "OMGetDepthStencilState", "SOGetTargets", "RSGetState", "RSGetViewports",
        "RSGetScissorRects", "HSGetShaderResources", "HSGetShader", "HSGetSamplers", "HSGetConstantBuffers", "DSGetShaderResources", "DSGetShader",
        "DSGetSamplers", "DSGetConstantBuffers", "CSGetShaderResources", "CSGetUnorderedAccessViews", "CSGetShader", "CSGetSamplers",
        "CSGetConstantBuffers", "ClearState", "Flush", "GetType", "GetContextFlags", "FinishCommandList"};
    const unsigned n = sizeof names / sizeof names[0];
    ComMethods m;
    for (unsigned slot = 3; slot < n; ++slot) {
        const std::string nm = names[slot];
        Handler h = [this, nm](GuestCpu& c) { ++d3d_calls_[nm]; c.r[0] = 0; }; // void/no-op by default (counted)
        if (nm == "GetDevice") h = [this](GuestCpu& c) { mem_.write<std::uint64_t>(c.arg(1), d3d_device_); };
        else if (nm == "Draw" || nm == "DrawIndexed" || nm == "DrawIndexedInstanced" || nm == "DrawInstanced")
            h = [this, nm](GuestCpu&) { ++d3d_calls_[nm]; ++d3d_draws_; };
        else if (nm == "Map") h = [this](GuestCpu& c) { // (res, subresource, maptype, flags, D3D11_MAPPED_SUBRESOURCE*)
            ++d3d_calls_["Map"];
            const auto it = d3d_res_.find(c.arg(1));
            const std::uint64_t out = c.arg(5);
            if (it == d3d_res_.end() || !it->second.data) { d3d_log_once("mapbad", "D3D11 Map of an unknown resource -> E_INVALIDARG"); c.r[0] = 0x80070057ULL; return; }
            mem_.write<std::uint64_t>(out, it->second.data);
            const auto pitch = it->second.pitch ? it->second.pitch : it->second.width * dxgi_bytes_per_pixel(it->second.format);
            mem_.write<std::uint32_t>(out + 8, pitch); mem_.write<std::uint32_t>(out + 12, pitch * it->second.height);
            c.r[0] = 0; };
        else if (nm == "Unmap") h = [this](GuestCpu& c) { ++d3d_calls_["Unmap"]; const auto it = d3d_res_.find(c.arg(1)); if (it != d3d_res_.end()) ++it->second.writes; };
        else if (nm == "UpdateSubresource") h = [this](GuestCpu& c) { // (dst, sub, box*, src, rowPitch, depthPitch)
            ++d3d_calls_["UpdateSubresource"];
            const auto it = d3d_res_.find(c.arg(1));
            if (it == d3d_res_.end() || !it->second.data || !c.arg(4)) return;
            auto& r = it->second;
            const std::uint32_t rp = static_cast<std::uint32_t>(c.arg(5)), bpp = dxgi_bytes_per_pixel(r.format);
            const std::uint32_t dst_pitch = r.pitch ? r.pitch : r.width * bpp;
            if (r.kind == "texture2d" && r.height) { const std::uint32_t row = (std::min)(rp ? rp : dst_pitch, dst_pitch); for (std::uint32_t y = 0; y < r.height; ++y) copy_guest(r.data + static_cast<std::uint64_t>(y) * dst_pitch, c.arg(4) + static_cast<std::uint64_t>(y) * (rp ? rp : dst_pitch), row); }
            else copy_guest(r.data, c.arg(4), (std::min)(r.size, static_cast<std::uint64_t>(1) << 24));
            ++r.writes; };
        else if (nm == "CopyResource") h = [this](GuestCpu& c) { ++d3d_calls_["CopyResource"]; const auto d = d3d_res_.find(c.arg(1)), s = d3d_res_.find(c.arg(2)); if (d != d3d_res_.end() && s != d3d_res_.end() && d->second.data && s->second.data) { copy_guest(d->second.data, s->second.data, (std::min)(d->second.size, s->second.size)); ++d->second.writes; } };
        else if (nm == "GetData") h = [](GuestCpu& c) { c.r[0] = 1; }; // S_FALSE: query not ready is never reported as an error
        else if (nm == "Flush" || nm == "ClearState") h = [this, nm](GuestCpu&) { ++d3d_calls_[nm]; };
        else if (nm == "GetType") h = [](GuestCpu& c) { c.r[0] = 0; };
        m.emplace_back(nm, h);
    }
    const std::uint64_t obj = make_com_object("ID3D11DeviceContextX", m, {"*"}, false, 160, 0x1800);
    d3d_fill_fast_table(obj);
    return obj;
}

// D3D11.x "fast semantics" immediate context: hot-path calls go through function pointers stored inside the context
// object (the title does `mov rax,[ctx]` / `call [rax+0x4e0]` with no vtable load). No public header gives the offsets, so
// every qword in the object becomes a logged trap and calls are classified by argument shape (Map/Unmap/UpdateSubresource on
// known resources are served; everything else is a counted no-op). The per-offset call counts appear in the report.
inline void XboxTitleRuntime::d3d_fill_fast_table(std::uint64_t obj) {
    constexpr std::uint64_t kSize = 0x1800; // already reserved by make_com_object(object_bytes)
    for (std::uint64_t off = 0x10; off < kSize; off += 8) {
        const std::string name = "CtxFast+0x" + GuestCpu::hex(off);
        handlers_[name] = [this, off](GuestCpu& c) {
            ++d3d_fast_calls_[off];
            const std::uint64_t a1 = c.arg(1), a2 = c.arg(2), a3 = c.arg(3);
            if (std::getenv("SOMEBOXONE_TRACE_CTX") && d3d_trace_samples_[static_cast<std::uint32_t>(off)]++ < 4)
                std::fprintf(stderr, "[ctx] +0x%x(%s, %s, %s, %s) from %s\n", static_cast<unsigned>(off), d3d_describe_arg(a1).c_str(), d3d_describe_arg(a2).c_str(), d3d_describe_arg(a3).c_str(), d3d_describe_arg(c.arg(4)).c_str(), locate(mem_.read<std::uint64_t>(c.r[4])).c_str());
            if (off == 0x698 && xdk2012_layout() && a1 == 1 && a2 && d3d_last_rtv_ && mem_.is_mapped(a2, 8)) {
                // OMGetRenderTargets(1, ID3D11RenderTargetView** rtv, ID3D11DepthStencilView** dsv): hand back the views the title created
                ++d3d_calls_["OMGetRenderTargets(fast)"];
                mem_.write<std::uint64_t>(a2, d3d_last_rtv_); mem_.write<std::uint64_t>(d3d_last_rtv_ + 8, mem_.read<std::uint64_t>(d3d_last_rtv_ + 8) + 1);
                if (a3 && d3d_last_dsv_ && mem_.is_mapped(a3, 8)) { mem_.write<std::uint64_t>(a3, d3d_last_dsv_); mem_.write<std::uint64_t>(d3d_last_dsv_ + 8, mem_.read<std::uint64_t>(d3d_last_dsv_ + 8) + 1); }
                d3d_log_once("fastomget", "D3D11.x fast-semantics OMGetRenderTargets identified at context+0x698 (VS2012 XDK layout)");
                c.r[0] = 0; return;
            }
            const auto res = d3d_res_.find(a1);
            const bool is_mem_res = res != d3d_res_.end() && (res->second.kind == "texture2d" || res->second.kind == "buffer") && res->second.data;
            const bool out_on_stack = c.arg(5) >= xrt::kStackTop - xrt::kStackSize && c.arg(5) < xrt::kStackTop;
            if (is_mem_res && a2 < 16 && a3 >= 1 && a3 <= 5 && out_on_stack) { // Map(res, sub, type, flags, D3D11_MAPPED_SUBRESOURCE*)
                auto& r = res->second;
                const std::uint32_t pitch = r.pitch ? r.pitch : static_cast<std::uint32_t>(r.size);
                mem_.write<std::uint64_t>(c.arg(5), r.data); mem_.write<std::uint32_t>(c.arg(5) + 8, pitch); mem_.write<std::uint32_t>(c.arg(5) + 12, pitch * (r.height ? r.height : 1));
                d3d_fast_mapped_ = a1; ++d3d_calls_["Map(fast)"];
                d3d_log_once("fastmap" + GuestCpu::hex(off), "D3D11.x fast-semantics Map identified at context+0x" + GuestCpu::hex(off) + " (resource " + std::to_string(r.width) + "x" + std::to_string(r.height) + " fmt " + std::to_string(r.format) + ")");
                c.r[0] = 0; return;
            }
            if (is_mem_res && a1 == d3d_fast_mapped_) { // Unmap(res, sub) directly after its Map
                ++res->second.writes; d3d_fast_mapped_ = 0; ++d3d_calls_["Unmap(fast)"];
                d3d_log_once("fastunmap" + GuestCpu::hex(off), "D3D11.x fast-semantics Unmap identified at context+0x" + GuestCpu::hex(off));
                c.r[0] = 0; return;
            }
            if (d3d_fast_seen_.insert(off).second && d3d_fast_seen_.size() <= 60)
                milestone("D3D11.x fast-context call at +0x" + GuestCpu::hex(off) + " (args 0x" + GuestCpu::hex(a1) + ", 0x" + GuestCpu::hex(a2) + ", 0x" + GuestCpu::hex(a3) + ") treated as a no-op");
            c.r[0] = 0;
        };
        mem_.write<std::uint64_t>(obj + off, trap_for_name(name));
    }
}

inline std::uint64_t XboxTitleRuntime::d3d_create_device() {
    ComMethods m; // slots 3..42 = ID3D11Device; Xbox extras follow (discovery)
    auto create_out = [this](const char* what, const char* kind, unsigned out_arg) {
        return [this, what, kind, out_arg](GuestCpu& c) {
            D3dResource r; r.kind = kind;
            const std::string w = what;
            const std::uint64_t desc = c.arg(1);
            if (w == "CreateBuffer" && desc) { r.size = mem_.read<std::uint32_t>(desc); r.bind_flags = mem_.read<std::uint32_t>(desc + 8); r.data = r.size ? heap_alloc(r.size, true) : 0; if (r.data && c.arg(2) && mem_.read<std::uint64_t>(c.arg(2))) copy_guest(r.data, mem_.read<std::uint64_t>(c.arg(2)), r.size); }
            if (std::string(kind) == "shader" && w != "CreateGeometryShaderWithStreamOutput" && c.arg(1) && c.arg(2) && c.arg(2) < (64u << 20) && mem_.is_mapped(c.arg(1), 4)) {
                // Shader bytecode: log its container format (host Direct3D accepts DXBC; Xbox GPU microcode would need a translator). SOMEBOXONE_DUMP_SHADERS=<dir> also writes each blob.
                const std::uint64_t len = c.arg(2); char magic[5] = {0}; for (int k = 0; k < 4; ++k) { const auto ch = mem_.read<std::uint8_t>(c.arg(1) + static_cast<std::uint64_t>(k)); magic[k] = (ch >= 32 && ch < 127) ? static_cast<char>(ch) : '.'; }
                std::string hex; for (int k = 0; k < 16 && static_cast<std::uint64_t>(k) < len; ++k) { char t[4]; std::snprintf(t, sizeof t, "%02x", mem_.read<std::uint8_t>(c.arg(1) + static_cast<std::uint64_t>(k))); hex += t; hex += ' '; }
                ++d3d_shader_formats_[magic];
                d3d_log_once(std::string("shader-") + magic, w + ": bytecode container '" + magic + "', " + std::to_string(len) + " bytes, first bytes " + hex);
                if (const char* dir = std::getenv("SOMEBOXONE_DUMP_SHADERS")) {
                    std::vector<std::uint8_t> blob(static_cast<std::size_t>(len)); mem_.read_bytes(c.arg(1), blob.data(), blob.size());
                    const std::string path = std::string(dir) + "/shader_" + std::to_string(d3d_shader_dump_count_++) + "_" + w + ".bin";
                    if (std::FILE* f = std::fopen(path.c_str(), "wb")) { std::fwrite(blob.data(), 1, blob.size(), f); std::fclose(f); }
                }
            }
            if (w == "CreateTexture2D" && desc) {
                r.width = mem_.read<std::uint32_t>(desc); r.height = mem_.read<std::uint32_t>(desc + 4); r.format = mem_.read<std::uint32_t>(desc + 16);
                r.usage = mem_.read<std::uint32_t>(desc + 28); r.bind_flags = mem_.read<std::uint32_t>(desc + 32);
                r.pitch = r.width * dxgi_bytes_per_pixel(r.format); r.size = static_cast<std::uint64_t>(r.pitch) * r.height;
                if (r.size && r.size < (1ULL << 28)) r.data = heap_alloc(r.size, true);
                if (r.data && c.arg(2)) { const auto sub = c.arg(2); const auto src = mem_.read<std::uint64_t>(sub); const auto sp = mem_.read<std::uint32_t>(sub + 8); if (src) for (std::uint32_t y = 0; y < r.height; ++y) copy_guest(r.data + static_cast<std::uint64_t>(y) * r.pitch, src + static_cast<std::uint64_t>(y) * (sp ? sp : r.pitch), r.pitch); }
                d3d_log_once(std::string("tex") + std::to_string(r.width) + "x" + std::to_string(r.height) + "f" + std::to_string(r.format),
                    "D3D11 CreateTexture2D " + std::to_string(r.width) + "x" + std::to_string(r.height) + " DXGI format " + std::to_string(r.format) + " usage " + std::to_string(r.usage) + " bind 0x" + GuestCpu::hex(r.bind_flags));
            }
            r.made_by = w;
            if (w == "CreateShaderResourceView" || w == "CreateRenderTargetView" || w == "CreateUnorderedAccessView" || w == "CreateDepthStencilView") r.resource = c.arg(1);
            ++d3d_calls_[w];
            const std::uint64_t obj = d3d_make_child(std::string("D3D11.") + w.substr(6), kind, r);
            if (c.arg(out_arg)) mem_.write<std::uint64_t>(c.arg(out_arg), obj);
            if (w == "CreateRenderTargetView") d3d_last_rtv_ = obj; else if (w == "CreateDepthStencilView") d3d_last_dsv_ = obj;
            c.r[0] = 0;
        };
    };
    struct Spec { const char* name; const char* kind; unsigned out; };
    const Spec specs[] = {{"CreateBuffer", "buffer", 3}, {"CreateTexture1D", "texture1d", 3}, {"CreateTexture2D", "texture2d", 3}, {"CreateTexture3D", "texture3d", 3},
        {"CreateShaderResourceView", "view", 3}, {"CreateUnorderedAccessView", "view", 3}, {"CreateRenderTargetView", "view", 3}, {"CreateDepthStencilView", "view", 3},
        {"CreateInputLayout", "layout", 5}, {"CreateVertexShader", "shader", 4}, {"CreateGeometryShader", "shader", 4}, {"CreateGeometryShaderWithStreamOutput", "shader", 9},
        {"CreatePixelShader", "shader", 4}, {"CreateHullShader", "shader", 4}, {"CreateDomainShader", "shader", 4}, {"CreateComputeShader", "shader", 4},
        {"CreateClassLinkage", "other", 1}, {"CreateBlendState", "state", 2}, {"CreateDepthStencilState", "state", 2}, {"CreateRasterizerState", "state", 2},
        {"CreateSamplerState", "state", 2}, {"CreateQuery", "other", 2}, {"CreatePredicate", "other", 2}, {"CreateCounter", "other", 2}};
    for (const auto& s : specs) m.emplace_back(s.name, create_out(s.name, s.kind, s.out));
    m.emplace_back("CreateDeferredContext", [this](GuestCpu& c) { d3d_log_once("deferred", "D3D11 CreateDeferredContext requested (returned the immediate context)"); mem_.write<std::uint64_t>(c.arg(2), d3d_context_); c.r[0] = 0; }); // 27
    m.emplace_back("OpenSharedResource", [](GuestCpu& c) { c.r[0] = 0x80004001ULL; });                                      // 28
    m.emplace_back("CheckFormatSupport", [this](GuestCpu& c) { mem_.write<std::uint32_t>(c.arg(2), 0x000FFFFF); c.r[0] = 0; });// 29
    m.emplace_back("CheckMultisampleQualityLevels", [this](GuestCpu& c) { mem_.write<std::uint32_t>(c.arg(3), 0); c.r[0] = 0; }); // 30
    m.emplace_back("CheckCounterInfo", [](GuestCpu&) {});                                                                   // 31
    m.emplace_back("CheckCounter", [](GuestCpu& c) { c.r[0] = 0x80004001ULL; });                                            // 32
    m.emplace_back("CheckFeatureSupport", [this](GuestCpu& c) { if (c.arg(2) && c.arg(3) < 4096) zero_fill(c.arg(2), c.arg(3)); c.r[0] = 0; }); // 33
    m.emplace_back("GetPrivateData", [](GuestCpu& c) { c.r[0] = 0; }); m.emplace_back("SetPrivateData", [](GuestCpu& c) { c.r[0] = 0; });
    m.emplace_back("SetPrivateDataInterface", [](GuestCpu& c) { c.r[0] = 0; });                                             // 34-36
    m.emplace_back("GetFeatureLevel", [](GuestCpu& c) { c.r[0] = 0xB100; });                                                // 37
    m.emplace_back("GetCreationFlags", [](GuestCpu& c) { c.r[0] = 0; });                                                    // 38
    m.emplace_back("GetDeviceRemovedReason", [](GuestCpu& c) { c.r[0] = 0; });                                              // 39
    // VS2012-era XDK: one extra method sits just before GetImmediateContext (the title calls it at slot 41, passing the out pointer).
    if (xdk2012_layout()) m.emplace_back("XdkDeviceExtra", [](GuestCpu& c) { c.r[0] = 0; });                                // 40 (VS2012 layout only)
    m.emplace_back("GetImmediateContext", [this](GuestCpu& c) { c.r[0] = 0; mem_.write<std::uint64_t>(c.arg(1), d3d_context_); mem_.write<std::uint64_t>(d3d_context_ + 8, mem_.read<std::uint64_t>(d3d_context_ + 8) + 1); }); // 40
    m.emplace_back("SetExceptionMode", [](GuestCpu& c) { c.r[0] = 0; }); m.emplace_back("GetExceptionMode", [](GuestCpu& c) { c.r[0] = 0; }); // 41-42
    // 43..84: Xbox-specific D3D11.x device extras (unknown layout) -> discovery; 85 = hardware-info query seen at .text+0x211cbb
    for (unsigned slot = 43; slot <= 85; ++slot) {
        if (slot == 85) m.emplace_back("XboxHardwareInfo", [this](GuestCpu& c) { // (this, struct* out{ptr,ptr/u32,level}) - level >= 2 selects the 4K config
            d3d_log_once("hwinfo", "D3D11.x hardware-info query (DeviceX slot 85) -> baseline Xbox One (level 1): the title selects its 1080p configuration");
            if (c.arg(1)) { mem_.write<std::uint64_t>(c.arg(1), 0); mem_.write<std::uint32_t>(c.arg(1) + 8, 1); } c.r[0] = 0; });
        else m.emplace_back("xbox_slot" + std::to_string(slot), [this, slot](GuestCpu& c) {
            last_discovery_ = "ID3D11DeviceX method slot " + std::to_string(slot);
            milestone("D3D DISCOVERY: ID3D11DeviceX method slot " + std::to_string(slot) + " called but not implemented, called from " + locate(mem_.read<std::uint64_t>(c.r[4])) + " -> E_NOTIMPL");
            c.r[0] = 0x80004001ULL; });
    }
    return make_com_object("ID3D11DeviceX", m, {"*"}, false, 160);
}

// DXGI chain used to create the swap chain: IDXGIDevice1 -> IDXGIAdapter -> IDXGIFactory2 -> IDXGISwapChain1.
inline void XboxTitleRuntime::d3d_create_dxgi() {
    auto ok = [](GuestCpu& c) { c.r[0] = 0; };
    auto notimpl = [this](const std::string& cls, unsigned slot) { return [this, cls, slot](GuestCpu& c) {
        last_discovery_ = cls + " method slot " + std::to_string(slot);
        if (!com_log_.insert("disc:" + cls + ":" + std::to_string(slot)).second) { c.r[0] = 0x80004001ULL; return; }
        milestone("D3D DISCOVERY: " + cls + " method slot " + std::to_string(slot) + " called but not implemented, called from " + locate(mem_.read<std::uint64_t>(c.r[4])) + " -> E_NOTIMPL");
        c.r[0] = 0x80004001ULL; }; };
    // Older Xbox One XDK builds (Visual Studio 2012 titles such as Minecraft 1.2) expose one extra IDXGIObject method at
    // slot 6, so GetParent is slot 7, IDXGIDevice1::GetAdapter is slot 8 and so on. Every DXGI object is shifted by one.
    const bool xdk2012_layout = this->xdk2012_layout();
    if (xdk2012_layout) milestone("DXGI: VS2012-era Xbox XDK layout detected (IDXGIObject has one extra method; DXGI vtables shifted by one slot)");
    auto shift_layout = [xdk2012_layout, ok](ComMethods& m) { if (xdk2012_layout) m.insert(m.begin() + 3, std::make_pair(std::string("XdkObjectExtra"), Handler(ok))); };
    // ---- swap chain (IDXGISwapChain1) ----
    {
        ComMethods m;
        m.emplace_back("SetPrivateData", ok); m.emplace_back("SetPrivateDataInterface", ok); m.emplace_back("GetPrivateData", ok);   // 3-5
        m.emplace_back("GetParent", [notimpl](GuestCpu& c) { notimpl("IDXGISwapChain1", 6)(c); });                                   // 6
        m.emplace_back("GetDevice", [this](GuestCpu& c) { mem_.write<std::uint64_t>(c.arg(2), d3d_device_); c.r[0] = 0; });          // 7
        m.emplace_back("Present", [this](GuestCpu& c) { d3d_capture_frame("Present"); c.r[0] = 0; });                                // 8
        m.emplace_back("GetBuffer", [this](GuestCpu& c) { mem_.write<std::uint64_t>(c.arg(3), d3d_backbuffer_); mem_.write<std::uint64_t>(d3d_backbuffer_ + 8, mem_.read<std::uint64_t>(d3d_backbuffer_ + 8) + 1); c.r[0] = 0; }); // 9
        m.emplace_back("SetFullscreenState", ok);                                                                                      // 10
        m.emplace_back("GetFullscreenState", [this](GuestCpu& c) { if (c.arg(1)) mem_.write<std::uint32_t>(c.arg(1), 1); if (c.arg(2)) mem_.write<std::uint64_t>(c.arg(2), 0); c.r[0] = 0; }); // 11
        m.emplace_back("GetDesc", notimpl("IDXGISwapChain1", 12));
        m.emplace_back("ResizeBuffers", ok); m.emplace_back("ResizeTarget", ok);                                                       // 13-14
        for (unsigned slot = 15; slot <= 21; ++slot) m.emplace_back("slot" + std::to_string(slot), notimpl("IDXGISwapChain1", slot));     // 15-21
        m.emplace_back("Present1", [this](GuestCpu& c) { ++d3d_calls_["Present1"]; d3d_capture_frame("Present1"); c.r[0] = 0; });       // 22
        shift_layout(m);
        d3d_swapchain_ = make_com_object("IDXGISwapChain1", m, {"*"}, false, 44);
    }
    // ---- factory (IDXGIFactory2) ----
    std::uint64_t factory;
    {
        ComMethods m;
        m.emplace_back("SetPrivateData", ok); m.emplace_back("SetPrivateDataInterface", ok); m.emplace_back("GetPrivateData", ok);
        m.emplace_back("GetParent", notimpl("IDXGIFactory2", 6));
        for (unsigned slot = 7; slot <= 15; ++slot) m.emplace_back("slot" + std::to_string(slot), slot == 8 ? Handler(ok) : Handler(notimpl("IDXGIFactory2", slot)));
        m.emplace_back("CreateSwapChainForCoreWindow", [this](GuestCpu& c) { // (device, window, DXGI_SWAP_CHAIN_DESC1*, restrict, IDXGISwapChain1**)  slot 16
            std::uint32_t w = 1920, h = 1080, fmt = 87;
            if (c.arg(3)) { const auto dw = mem_.read<std::uint32_t>(c.arg(3)), dh = mem_.read<std::uint32_t>(c.arg(3) + 4), df = mem_.read<std::uint32_t>(c.arg(3) + 8); if (dw) w = dw; if (dh) h = dh; if (df) fmt = df; }
            D3dResource bb; bb.width = w; bb.height = h; bb.format = fmt; bb.pitch = w * dxgi_bytes_per_pixel(fmt); bb.size = static_cast<std::uint64_t>(bb.pitch) * h;
            bb.bind_flags = 0x20; bb.data = 0; // the GPU-rendered back buffer is not materialised (draws are not rasterised)
            d3d_backbuffer_ = d3d_make_child("IDXGISurface.BackBuffer", "texture2d", bb);
            milestone("DXGI CreateSwapChainForCoreWindow -> " + std::to_string(w) + "x" + std::to_string(h) + " format " + std::to_string(fmt) + " swap chain created");
            mem_.write<std::uint64_t>(c.arg(5), d3d_swapchain_); mem_.write<std::uint64_t>(d3d_swapchain_ + 8, mem_.read<std::uint64_t>(d3d_swapchain_ + 8) + 1);
            c.r[0] = 0; });
        for (unsigned slot = 17; slot <= 24; ++slot) m.emplace_back("slot" + std::to_string(slot), notimpl("IDXGIFactory2", slot));
        shift_layout(m);
        factory = make_com_object("IDXGIFactory2", m, {"*"}, false, 44);
    }
    // ---- adapter (IDXGIAdapter) ----
    std::uint64_t adapter;
    {
        ComMethods m;
        m.emplace_back("SetPrivateData", ok); m.emplace_back("SetPrivateDataInterface", ok); m.emplace_back("GetPrivateData", ok);
        m.emplace_back("GetParent", [this, factory](GuestCpu& c) { mem_.write<std::uint64_t>(c.arg(2), factory); mem_.write<std::uint64_t>(factory + 8, mem_.read<std::uint64_t>(factory + 8) + 1); c.r[0] = 0; }); // 6
        // IDXGIOutput for the single 1920x1080@60 display (EnumOutputs index 0; further indices: DXGI_ERROR_NOT_FOUND)
        ComMethods om;
        om.emplace_back("SetPrivateData", ok); om.emplace_back("SetPrivateDataInterface", ok); om.emplace_back("GetPrivateData", ok);
        om.emplace_back("GetParent", [this](GuestCpu& c) { if (c.arg(2)) mem_.write<std::uint64_t>(c.arg(2), 0); c.r[0] = 0x80004002ULL; });   // E_NOINTERFACE
        om.emplace_back("GetDesc", [this](GuestCpu& c) { // DXGI_OUTPUT_DESC: WCHAR DeviceName[32]; RECT; BOOL AttachedToDesktop; UINT Rotation; HMONITOR
            const std::uint64_t d = c.arg(1); if (!d || !mem_.is_mapped(d, 0x60)) { c.r[0] = 0x80070057ULL; return; }
            zero_fill(d, 0x60); write_wstr(d, u"\\\\.\\DISPLAY1");
            mem_.write<std::int32_t>(d + 64 + 8, 1920); mem_.write<std::int32_t>(d + 64 + 12, 1080); mem_.write<std::uint32_t>(d + 80, 1); mem_.write<std::uint32_t>(d + 84, 1); mem_.write<std::uint64_t>(d + 88, 1);
            c.r[0] = 0; });
        om.emplace_back("GetDisplayModeList", [this](GuestCpu& c) { // (format, flags, UINT* num, DXGI_MODE_DESC* modes)
            const std::uint64_t n = c.arg(3), modes = c.arg(4);
            if (!n) { c.r[0] = 0x80070057ULL; return; }
            const std::uint32_t have = mem_.read<std::uint32_t>(n);
            if (modes && have >= 1) { zero_fill(modes, 28); mem_.write<std::uint32_t>(modes, 1920); mem_.write<std::uint32_t>(modes + 4, 1080); mem_.write<std::uint32_t>(modes + 8, 60); mem_.write<std::uint32_t>(modes + 12, 1); mem_.write<std::uint32_t>(modes + 16, c.arg(1) ? static_cast<std::uint32_t>(c.arg(1)) : 28); }
            mem_.write<std::uint32_t>(n, 1); c.r[0] = 0; });
        om.emplace_back("FindClosestMatchingMode", ok); om.emplace_back("WaitForVBlank", ok); om.emplace_back("TakeOwnership", ok); om.emplace_back("ReleaseOwnership", ok);
        om.emplace_back("GetGammaControlCapabilities", ok); om.emplace_back("SetGammaControl", ok); om.emplace_back("GetGammaControl", ok);
        om.emplace_back("SetDisplaySurface", ok); om.emplace_back("GetDisplaySurfaceData", ok); om.emplace_back("GetFrameStatistics", ok);
        shift_layout(om);
        const std::uint64_t output = make_com_object("IDXGIOutput", om, {"*"}, false, 28);
        m.emplace_back("EnumOutputs", [this, output](GuestCpu& c) { // (index, IDXGIOutput**)
            if ((c.arg(1) & 0xFFFFFFFFULL) != 0) { if (c.arg(2)) mem_.write<std::uint64_t>(c.arg(2), 0); c.r[0] = 0x887A0002ULL; return; }   // DXGI_ERROR_NOT_FOUND
            mem_.write<std::uint64_t>(c.arg(2), output); mem_.write<std::uint64_t>(output + 8, mem_.read<std::uint64_t>(output + 8) + 1); c.r[0] = 0; });
        m.emplace_back("GetDesc", [this](GuestCpu& c) { // DXGI_ADAPTER_DESC
            const std::uint64_t d = c.arg(1); if (!d || !mem_.is_mapped(d, 0x130)) { c.r[0] = 0x80070057ULL; return; }
            zero_fill(d, 0x130); write_wstr(d, u"SomeBoxOne Virtual Xbox One GPU");
            mem_.write<std::uint32_t>(d + 0x100, 0x1002); mem_.write<std::uint32_t>(d + 0x104, 0x1234); mem_.write<std::uint64_t>(d + 0x110, 8ULL << 30);
            c.r[0] = 0; });
        m.emplace_back("CheckInterfaceSupport", ok);
        shift_layout(m);
        adapter = make_com_object("IDXGIAdapter", m, {"*"}, false, 28);
    }
    // ---- device (IDXGIDevice1) ----
    {
        ComMethods m;
        m.emplace_back("SetPrivateData", ok); m.emplace_back("SetPrivateDataInterface", ok); m.emplace_back("GetPrivateData", ok);
        m.emplace_back("GetParent", notimpl("IDXGIDevice1", 6));
        m.emplace_back("GetAdapter", [this, adapter](GuestCpu& c) { mem_.write<std::uint64_t>(c.arg(1), adapter); mem_.write<std::uint64_t>(adapter + 8, mem_.read<std::uint64_t>(adapter + 8) + 1); c.r[0] = 0; }); // 7
        m.emplace_back("CreateSurface", notimpl("IDXGIDevice1", 8)); m.emplace_back("QueryResourceResidency", notimpl("IDXGIDevice1", 9));
        m.emplace_back("SetGPUThreadPriority", ok); m.emplace_back("GetGPUThreadPriority", [this](GuestCpu& c) { mem_.write<std::uint32_t>(c.arg(1), 0); c.r[0] = 0; }); // 10-11
        m.emplace_back("SetMaximumFrameLatency", ok); m.emplace_back("GetMaximumFrameLatency", [this](GuestCpu& c) { mem_.write<std::uint32_t>(c.arg(1), 1); c.r[0] = 0; }); // 12-13
        shift_layout(m);
        d3d_dxgi_device_ = make_com_object("IDXGIDevice1", m, {"*"}, false, 28);
    }
    for (const char* iid : {"54ec77fa-1377-44e6-8c32-88fd5f44c84c", "77db970f-6276-48ba-ba28-070143b4392c", "05008617-fbfd-4051-a790-144884b4f6a9"})
        iid_objects_[iid] = d3d_dxgi_device_;
}


// Debug aid: classify a call argument as a known D3D object, a pointer to an array of them, or a plain value.
inline std::string XboxTitleRuntime::d3d_describe_arg(std::uint64_t v) {
    auto name = [this](std::uint64_t p) -> std::string {
        const auto it = d3d_res_.find(p); if (it == d3d_res_.end()) return {};
        return it->second.kind + ":" + it->second.made_by + (it->second.width ? "(" + std::to_string(it->second.width) + "x" + std::to_string(it->second.height) + ")" : std::string());
    };
    if (const auto n = name(v); !n.empty()) return n;
    if (v < 0x10000) return "#" + std::to_string(v);
    if (mem_.is_mapped(v, 8)) {
        std::string out; unsigned shown = 0;
        for (unsigned i = 0; i < 4 && mem_.is_mapped(v + i * 8ULL, 8); ++i) { const auto q = mem_.read<std::uint64_t>(v + i * 8ULL); const auto n = name(q); if (n.empty()) { if (i == 0) out = "ptr"; break; } out += (shown++ ? "," : "[") + n; }
        if (shown) return out + "]";
        return out.empty() ? "ptr" : out;
    }
    return "0x" + GuestCpu::hex(v);
}

} // namespace someboxone

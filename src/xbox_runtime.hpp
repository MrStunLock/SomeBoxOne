#pragma once

// SomeBoxOne v0.22.0 - user-mode Xbox One title runtime (HLE).
//
// Loads an x64 title PE, builds a Windows-style process image (TEB/PEB,
// KUSER_SHARED_DATA, TLS, stack, heap), binds every import to a host-side
// handler, then runs the title's entry point on the GuestCpu interpreter.
//
// This is high-level emulation of the *title-facing* API surface only (CRT,
// kernel32-style calls, a sandboxed view of the package files). It does NOT
// emulate the Xbox One hypervisor, GPU or system OS. Imports with no handler are
// "unimplemented stubs": they return 0, are logged, and are listed in the report
// so nothing silently pretends to work.

#include "guest_cpu.hpp"
#include "gamepad_input.hpp"
#include "guest_mem.hpp"
#include "pe_image.hpp"
#include "png_writer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <deque>
#include <functional>
#include <map>
#include <limits>
#include <memory>
#include <chrono>
#include "audio_mixer.hpp"
#include "audio_sink.hpp"
#include "frame_pacer.hpp"
#include "fps_overlay.hpp"
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace someboxone {

namespace xrt {
constexpr std::uint64_t kStackTop = 0x00007FF800200000ULL;
constexpr std::uint64_t kStackSize = 0x00200000ULL;
constexpr std::uint64_t kTebBase = 0x00007FF700000000ULL;   // TEB page(s)
constexpr std::uint64_t kPebBase = 0x00007FF700010000ULL;
constexpr std::uint64_t kKusd = 0x000000007FFE0000ULL;       // KUSER_SHARED_DATA
constexpr std::uint64_t kTrapBase = 0x00007FF900000000ULL;
constexpr std::uint64_t kTrapSlots = 8192;
constexpr std::uint64_t kSentinel = 0x00007FF9F0000000ULL;   // never mapped; used as stop_at
constexpr std::uint64_t kHeapBase = 0x0000000200000000ULL;
constexpr std::uint64_t kHeapMax = 0x0000000400000000ULL;
constexpr std::uint64_t kStaticBase = 0x00007FF600000000ULL; // CRT scratch (errno, iob, ctype, strings)
constexpr std::uint64_t kVirtBase = 0x0000010000000000ULL;   // VirtualAlloc arena
constexpr std::uint64_t kVirtMax = 0x0000020000000000ULL;
}

struct TitleLaunchOptions {
    std::filesystem::path package_root; // directory holding AppxManifest.xml (the "Mount" folder)
    std::filesystem::path save_root;    // writable sandbox; defaults to <package_root>/../SomeBoxOneSave
    std::filesystem::path system_root;  // package redistributable DLLs (EmbeddedXvd/Windows/System32); auto-detected
    std::vector<std::string> real_dlls{"msvcp140.dll", "concrt140.dll", "vccorlib140.dll", "msvcp120.dll", "concrt120.dll", "vccorlib120.dll",
                                       "msvcp110.dll", "concrt110.dll", "vccorlib110.dll", "msvcp100.dll"}; // C++ runtime DLLs loaded for real (only if the title imports them and the package ships them)
    std::uint64_t max_instructions{20000000000ULL};
    std::uint64_t max_frames{300};
    std::uint64_t save_every{60};       // save a PNG every N main-loop iterations (0 = only the final frame)
    unsigned view_width{424}, view_height{240};   // RSDKv5 Sonic Mania's active screen inside its 512x256 texture
    // Optional live-frame sink (called on the runner's thread): BGRA pixels, width, height, frame number.
    std::function<void(const std::vector<std::uint8_t>&, unsigned, unsigned, std::uint64_t)> frame_sink;
    // Live textual output for the GUI console; called synchronously on the guest-runner thread.
    std::function<void(const std::string&)> progress_sink;
    // Called whenever the guest polls Gamepad.GetCurrentReading; host keyboard/controller state is sampled live.
    std::function<HostGamepadState()> input_provider;
    std::uint64_t sink_every{1};        // hand every frame to the live display (the old default of 4 made a 60 FPS game look like 15 FPS)
    unsigned fps_limit{0};              // frame-rate lock applied at the main-loop boundary: 0 = unlimited, otherwise 30 or 60 (the GUI default is 60)
    bool show_fps{false};               // draw the FPS counter into the live frame
    bool audio_enabled{true};           // false: the mixer still runs (title callbacks keep firing) but nothing is sent to the sound device
    float audio_volume{1.0f};           // 0..1 master volume applied on top of the title's own mastering-voice volume
    std::string audio_wav_path;         // non-empty: write the mixed audio to this .wav file instead of the sound device (diagnostics)
    // Scripted virtual-gamepad input: comma-separated "frame:buttons:duration" steps, e.g. "850:menu+a:6,1000:a:6".
    // Buttons: menu(start) view a b x y up down left right lb rb ls rs. `frame` counts main-loop iterations.
    std::string input_script;
    // Called when max_frames is reached. May append more scripted input and returns the next absolute frame limit (<= current = stop).
    // Used by `launch-title --serve DIR` so a controller of the process can play interactively one segment at a time.
    std::function<std::uint64_t(std::uint64_t frames, std::string& more_input)> pause_hook;
    bool virtual_gamepad{true};         // present one virtual Xbox gamepad to the title (buttons are scripted, see docs)
    const std::atomic<bool>* cancel{nullptr};   // set by a UI thread to stop the run at the next main-loop iteration      // stop after this many main-loop iterations (CoreDispatcher.ProcessEvents calls)
    bool trace_hle{false};
    // Unimplemented methods on Windows.Xbox.* service classes (multiplayer, social, Live) succeed with an empty/zero result instead of
    // raising E_NOTIMPL (which C++/CX turns into a fatal exception). Each one is still logged once. `--strict-winrt` turns this off.
    bool permissive_xbox_services{true};
    bool trace_instructions{false};
    std::uint64_t trace_limit{2000};
    std::uint64_t memory_budget_mb{1280};
    std::string exe_name;               // executable file name (for GetModuleFileName)
    std::string command_line{"\"Sonic Mania.exe\""};
};

struct TitleLaunchReport {
    bool image_loaded{};
    std::string load_error;
    std::string stop_kind;              // exit | fault | unsupported | limit | host
    std::string stop_detail;
    std::uint64_t instructions{};
    std::uint64_t stop_rip{};
    std::string stop_location;
    bool has_fault_address{};
    std::uint64_t fault_address{};
    std::string stop_instruction_bytes;
    int exit_code{-1};
    bool exited{};
    std::vector<std::string> milestones;
    std::vector<std::string> stubbed_imports_called;     // imports that had no real handler
    std::vector<std::pair<std::string, std::uint64_t>> hle_calls;   // name -> count
    std::vector<std::string> last_calls;
    std::vector<std::string> files_opened;
    std::vector<std::string> backtrace;
    std::vector<std::string> graphics;      // D3D/audio/frame summary lines
    std::uint64_t frames{};
    std::uint32_t frames_saved{};
    std::vector<std::string> registers;
    std::uint64_t heap_bytes{};
    std::uint64_t guest_memory_bytes{};
    std::size_t imports_total{}, imports_implemented{}, real_dll_imports{};
    std::string text;                   // rendered report
};

struct D3dResource {
    std::string kind;            // buffer | texture2d | view | state | shader | layout | other
    std::string made_by;         // the Create* method that produced it (CreatePixelShader, CreateRenderTargetView, ...)
    std::uint32_t width{}, height{}, format{}, pitch{}, bind_flags{}, usage{};
    std::uint64_t data{};        // guest address of backing storage (textures/buffers)
    std::uint64_t size{};
    std::uint64_t resource{};    // for views: the underlying resource object
    std::uint64_t writes{};
};

class XboxTitleRuntime {
public:
    using Handler = std::function<void(GuestCpu&)>;

    XboxTitleRuntime(const PeImage& image, TitleLaunchOptions options)
        : image_(image), opt_(std::move(options)),
          mem_(opt_.memory_budget_mb * 1024ULL * 1024ULL), cpu_(mem_) {
        register_handlers();
    }

    TitleLaunchReport run() {
        TitleLaunchReport rep;
        rep.image_loaded = false;
        std::string err;
        if (!setup(&err)) { rep.load_error = err; rep.text = render(rep); return rep; }
        rep.image_loaded = true;
        milestone("image mapped at 0x" + GuestCpu::hex(image_.image_base) + ", " + std::to_string(image_.imports.size()) + " imports bound");

        cpu_.trap_lo = xrt::kTrapBase;
        cpu_.trap_hi = xrt::kTrapBase + xrt::kTrapSlots * 16;
        cpu_.trap_handler = [this](GuestCpu& c) { dispatch_trap(c); };
        if (opt_.trace_instructions) {
            cpu_.trace_hook = [this](GuestCpu& c) {
                if (trace_count_++ < opt_.trace_limit) {
                    std::fprintf(stderr, "  %s  rax=%llx rcx=%llx rdx=%llx rsp=%llx\n", locate(c.rip).c_str(),
                        (unsigned long long)c.r[0], (unsigned long long)c.r[1], (unsigned long long)c.r[2], (unsigned long long)c.r[4]);
                }
            };
        }

        // DllMain(DLL_PROCESS_ATTACH) for each real DLL, in load (dependency) order.
        for (const auto& m : modules_) {
            if (!m->img.entry_rva) continue;
            milestone("DllMain(" + m->name + ", PROCESS_ATTACH)");
            const std::uint64_t ok = call_guest(cpu_, m->base + m->img.entry_rva, {m->base, 1, 0});
            if (cpu_.stop != GuestCpu::Stop::None) break;
            if ((ok & 0xFF) == 0) milestone("DllMain(" + m->name + ") returned FALSE");
        }
        // TLS callbacks (DLL_PROCESS_ATTACH), then the entry point.
        if (cpu_.stop == GuestCpu::Stop::None && image_.tls.present && image_.tls.callbacks_va) {
            for (unsigned i = 0; i < 16; ++i) {
                std::uint64_t cb = 0;
                try { cb = mem_.read<std::uint64_t>(image_.tls.callbacks_va + i * 8); } catch (...) { break; }
                if (!cb) break;
                milestone("running TLS callback #" + std::to_string(i) + " at " + locate(cb));
                call_guest(cpu_, cb, {image_.image_base, 1, 0});
                if (cpu_.stop != GuestCpu::Stop::None) break;
            }
        }
        if (cpu_.stop == GuestCpu::Stop::None) {
            milestone("entering title entry point " + locate(image_.image_base + image_.entry_rva));
            const std::uint64_t rv = call_guest(cpu_, image_.image_base + image_.entry_rva, {xrt::kPebBase});
            if (cpu_.stop == GuestCpu::Stop::None) {
                exited_ = true; exit_code_ = static_cast<int>(rv);
                milestone("entry point returned normally, exit code " + std::to_string(exit_code_));
            }
        }
        finish(rep);
        return rep;
    }

    // Test/diagnostic hooks: set up the process image without running, and invoke one HLE handler with Win64 arguments.
    bool debug_setup(std::string* err) {
        if (!setup(err)) return false;
        cpu_.trap_lo = xrt::kTrapBase; cpu_.trap_hi = xrt::kTrapBase + xrt::kTrapSlots * 16;
        cpu_.trap_handler = [this](GuestCpu& c) { dispatch_trap(c); };
        return true;
    }
    std::uint64_t debug_call(const std::string& name, std::initializer_list<std::uint64_t> args) {
        const auto it = handlers_.find(name);
        if (it == handlers_.end()) return ~0ULL;
        std::uint64_t sp = ((xrt::kStackTop - 0x4000) & ~0xFULL) - 0x80;
        const std::vector<std::uint64_t> a(args);
        const std::uint64_t regs[4] = {1, 2, 8, 9};
        for (std::size_t i = 0; i < a.size(); ++i) { if (i < 4) cpu_.r[regs[i]] = a[i]; else mem_.write<std::uint64_t>(sp + 8 + 32 + (i - 4) * 8, a[i]); }
        cpu_.r[4] = sp; mem_.write<std::uint64_t>(sp, 0);
        it->second(cpu_);
        return cpu_.r[0];
    }
    // More test hooks: build an async operation / a delegate and call COM methods through a guest vtable slot.
    std::uint64_t debug_make_async(const std::string& label, bool has_result, std::uint64_t result) { return make_async_operation(label, has_result, result); }
    std::uint64_t debug_make_delegate(Handler h) {
        const std::string name = "dbg.delegate." + std::to_string(handlers_.size());
        handlers_[name] = std::move(h);
        const std::uint64_t vt = static_alloc(32), obj = static_alloc(16);
        mem_.write<std::uint64_t>(vt + 24, trap_for_name(name)); mem_.write<std::uint64_t>(obj, vt); mem_.write<std::uint64_t>(obj + 8, 1);
        return obj;
    }
    std::uint64_t debug_vcall(std::uint64_t obj, unsigned slot, std::initializer_list<std::uint64_t> args) {
        const std::uint64_t trap = mem_.read<std::uint64_t>(mem_.read<std::uint64_t>(obj) + slot * 8ULL);
        const std::uint64_t idx = (trap - xrt::kTrapBase) / 16;
        if (idx >= bindings_.size() || !bindings_[idx].handler) return ~0ULL;
        std::uint64_t sp = ((xrt::kStackTop - 0x4000) & ~0xFULL) - 0x80;
        const std::vector<std::uint64_t> a(args);
        cpu_.r[1] = obj; const std::uint64_t regs[3] = {2, 8, 9};
        for (std::size_t i = 0; i < a.size() && i < 3; ++i) cpu_.r[regs[i]] = a[i];
        cpu_.r[4] = sp; mem_.write<std::uint64_t>(sp, 0);
        bindings_[idx].handler(cpu_);
        return cpu_.r[0];
    }
    GuestMemory& memory() { return mem_; }
    GuestCpu& cpu() { return cpu_; }

private:
    const PeImage& image_;
    TitleLaunchOptions opt_;
    GuestMemory mem_;
    GuestCpu cpu_;

    struct ImportBinding { std::string dll, name; std::uint64_t trap{}; Handler handler; bool stub{}; std::uint64_t calls{}; };
    std::deque<ImportBinding> bindings_;   // a deque: references stay valid while a handler binds more traps (trap_for_name)
    std::unordered_map<std::string, Handler> handlers_;     // by function name
    struct Module { std::string name; PeImage img; std::uint64_t base{}; std::uint32_t tls_index{}; std::vector<std::pair<std::uint32_t, std::string>> sorted_exports; };
    std::vector<std::unique_ptr<Module>> modules_;
    std::uint64_t next_module_base_{0x00007FF500000000ULL};
    std::map<std::string, std::uint64_t> call_counts_;
    struct RingEntry { const std::string* name{}; std::uint64_t a[4]{}; };   // last HLE calls, formatted lazily (only the report needs the text)
    std::array<RingEntry, 48> ring_{}; std::uint64_t ring_pos_{};
    std::vector<std::string> ring_lines() const {
        std::vector<std::string> out;
        const std::uint64_t n = ring_pos_ < ring_.size() ? ring_pos_ : ring_.size();
        for (std::uint64_t i = ring_pos_ - n; i < ring_pos_; ++i) {
            const RingEntry& e = ring_[i % ring_.size()]; char line[200];
            std::snprintf(line, sizeof line, "%s(0x%llx, 0x%llx, 0x%llx, 0x%llx)", e.name ? e.name->c_str() : "?", (unsigned long long)e.a[0], (unsigned long long)e.a[1], (unsigned long long)e.a[2], (unsigned long long)e.a[3]);
            out.push_back(line);
        }
        return out;
    }
    std::set<std::string> stubs_hit_;
    std::vector<std::string> milestones_;
    std::vector<std::string> files_opened_;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> runtime_functions_; // begin,end RVAs (sorted)

    std::uint64_t heap_next_{xrt::kHeapBase};
    std::uint64_t heap_mapped_end_{xrt::kHeapBase};
    std::uint64_t heap_live_{};
    std::map<std::uint64_t, std::vector<std::uint64_t>> free_lists_; // size class -> blocks
    std::uint64_t static_next_{xrt::kStaticBase};
    std::uint64_t trace_count_{};

    bool exited_{};
    int exit_code_{-1};
    std::uint32_t last_error_{};
    std::uint64_t qpc_{1000000};
    std::uint64_t filetime_{0x01DC000000000000ULL};
    const std::chrono::steady_clock::time_point start_time_{std::chrono::steady_clock::now()};
    std::uint64_t rand_state_{1};
    std::uint32_t next_tls_index_{};
    std::vector<std::uint32_t> free_tls_indexes_;
    std::set<std::uint32_t> allocated_tls_indexes_;
    std::unordered_map<std::uint32_t, std::uint64_t> tls_values_;
    std::uint64_t next_fake_handle_{0xDEAD1000ULL};
    struct SemaphoreState { std::uint32_t count{}, maximum{}; };
    std::map<std::uint64_t, SemaphoreState> semaphores_;
    std::set<std::uint64_t> mutex_handles_;
    std::uint32_t threads_created_{};
    // ---- cooperative guest threads (xbox_threads.hpp) ----
    // Every CreateThread gets its own GuestCpu (registers, TEB, TLS) over the shared guest memory. Threads are scheduled
    // cooperatively: they run when the main thread reaches a frame boundary or blocks, and they yield when they block.
    struct GuestThread {
        std::unique_ptr<GuestCpu> cpu; std::uint32_t tid{}; std::uint64_t handle{};
        bool finished{}, suspended{}; std::uint32_t exit_code{259};
        std::vector<std::uint64_t> tls, fls; std::uint64_t teb{}, start{};
        bool waiting{}; std::uint64_t wait_rsp{}, deadline{}, aux{}, aux2{}; int phase{}; bool timed_out{};   // state of the blocking HLE call being retried
        std::uint64_t slices{};
    };
    std::vector<std::unique_ptr<GuestThread>> threads_;
    GuestThread* cur_thread_{nullptr};                 // nullptr = the main guest thread is running
    bool hle_retry_{false}, pending_yield_{false};
    std::uint32_t next_tid_{0x1000};
    std::uint64_t thread_exit_trap_{};
    struct TlsTemplate { std::uint32_t index; std::uint64_t start_va, len, zero_fill; };
    std::vector<TlsTemplate> tls_templates_;
    std::map<std::uint64_t, std::uint64_t> cv_gen_;    // condition variable address -> wake generation
    struct SrwState { std::uint32_t excl{}, shared{}; };
    std::map<std::uint64_t, SrwState> srw_;
    std::uint64_t thread_slice_{2'000'000};            // max guest instructions per thread per scheduling pass
    bool thread_deadlock_logged_{false}; unsigned thread_deadlocks_logged_{0};
    enum class WaitResult { Ready, Timeout, Retry };
    std::uint32_t current_tid() const { return cur_thread_ ? cur_thread_->tid : 0x1000; }
    void register_thread_handlers();
    void deliver_launch_activation(GuestCpu& c);
    void permissive_result(GuestCpu& c, const std::string& cls);
    std::uint64_t make_permissive_object(const std::string& cls);
    std::map<std::uint64_t, unsigned> xmem_contexts_;   // XMem context handle -> LZX window size
    std::uint64_t xmem_calls_{};
    std::uint64_t d3d_last_rtv_{}, d3d_last_dsv_{};
    std::string caller_watch_ = [] { const char* e = std::getenv("SOMEBOXONE_CALLERS"); return e ? std::string(e) : std::string(); }();
    std::map<std::uint64_t, std::uint64_t> caller_hist_;
    std::string d3d_describe_arg(std::uint64_t v);
    std::map<std::uint32_t, unsigned> d3d_trace_samples_;
    std::map<std::string, std::uint64_t> d3d_shader_formats_; std::uint64_t d3d_shader_dump_count_{};   // shader bytecode container magic -> count
    bool activation_delivered_{false};
    void run_threads(std::uint64_t slice);
    WaitResult wait_for(GuestCpu& c, const std::function<bool()>& ready, std::uint64_t timeout_ms);
    bool wait_try_handle(std::uint64_t handle);
    std::uint64_t create_guest_thread(GuestCpu& c, std::uint64_t stack, std::uint64_t start, std::uint64_t param, std::uint64_t flags, std::uint64_t tid_out);
    std::vector<std::uint64_t> atexit_fns_;
    std::uint64_t errno_ptr_{}, iob_base_{}, ctype_ptr_{}, commode_ptr_{}, empty_cmdline_{}, c_locale_str_{};
    bool wide_crt_state_ready_{};
    std::uint64_t wide_arg0_{}, wide_argv_{}, wide_env_{}, wide_cmdline_{};
    std::uint64_t wide_argc_slot_{}, wide_argv_slot_{}, wide_env_slot_{}, wide_cmdline_slot_{};
    unsigned call_depth_{};

    struct HostFile { std::FILE* fp{}; std::string path; };
    std::map<std::uint64_t, HostFile> files_;
    std::uint64_t next_file_slot_{};

    // ----------------------------------------------------------- helpers
    void milestone(const std::string& s) {
        if (milestones_.size() < 200) milestones_.push_back(s);
        if (opt_.progress_sink) opt_.progress_sink(s);
    }

    std::string locate(std::uint64_t va) const {
        if (va >= image_.image_base && va < image_.image_base + image_.size_of_image) {
            const std::uint32_t rva = static_cast<std::uint32_t>(va - image_.image_base);
            std::string s = "0x" + GuestCpu::hex(va);
            if (const auto* sec = image_.section_for_rva(rva)) s += " (" + sec->name + "+0x" + GuestCpu::hex(rva - sec->virtual_address);
            else s += " (hdr";
            const auto it = std::upper_bound(runtime_functions_.begin(), runtime_functions_.end(), std::make_pair<std::uint64_t, std::uint64_t>(rva, ~0ULL));
            if (it != runtime_functions_.begin()) {
                const auto& f = *(it - 1);
                if (rva >= f.first && rva < f.second) s += ", func sub_" + GuestCpu::hex(f.first) + "+0x" + GuestCpu::hex(rva - f.first);
            }
            return s + ")";
        }
        for (const auto& m : modules_) {
            if (va >= m->base && va < m->base + m->img.size_of_image) {
                const std::uint32_t rva = static_cast<std::uint32_t>(va - m->base);
                std::string s = "0x" + GuestCpu::hex(va) + " (" + m->name + "+0x" + GuestCpu::hex(rva);
                const auto it = std::upper_bound(m->sorted_exports.begin(), m->sorted_exports.end(), std::make_pair(rva, std::string("\xff")));
                if (it != m->sorted_exports.begin()) { const auto& e = *(it - 1); if (rva - e.first < 0x2000) s += ", near export " + e.second + "+0x" + GuestCpu::hex(rva - e.first); }
                return s + ")";
            }
        }
        if (va >= xrt::kTrapBase && va < xrt::kTrapBase + xrt::kTrapSlots * 16) {
            const std::uint64_t idx = (va - xrt::kTrapBase) / 16;
            if (idx < bindings_.size()) return "HLE " + bindings_[idx].dll + "!" + bindings_[idx].name;
            return "HLE trap";
        }
        if (va >= xrt::kStackTop - xrt::kStackSize && va < xrt::kStackTop) return "0x" + GuestCpu::hex(va) + " (stack)";
        if (va >= xrt::kHeapBase && va < xrt::kHeapMax) return "0x" + GuestCpu::hex(va) + " (heap)";
        return "0x" + GuestCpu::hex(va);
    }

    std::uint64_t static_alloc(std::uint64_t size, std::uint64_t align = 16) {
        static_next_ = (static_next_ + align - 1) & ~(align - 1);
        const std::uint64_t a = static_next_;
        static_next_ += size;
        mem_.map(a, size, "crt-static");
        return a;
    }
    std::uint64_t static_cstr(const std::string& s) {
        const std::uint64_t a = static_alloc(s.size() + 1, 2);
        mem_.write_bytes(a, s.c_str(), s.size() + 1);
        return a;
    }

    // Heap: bump allocator with exact-size-class free lists. 16-byte header holds the size.
    std::uint64_t heap_alloc(std::uint64_t size, bool zero = true) {
        if (size == 0) size = 1;
        if (size > 0x40000000ULL) return 0;
        const std::uint64_t cls = (size + 15) & ~15ULL;
        auto& fl = free_lists_[cls];
        std::uint64_t block;
        if (!fl.empty()) { block = fl.back(); fl.pop_back(); if (zero) zero_fill(block, cls); }
        else {
            const std::uint64_t need = cls + 16;
            if (heap_next_ + need > xrt::kHeapMax) return 0;
            while (heap_mapped_end_ < heap_next_ + need) {
                const std::uint64_t chunk = (std::max<std::uint64_t>)(0x100000, (heap_next_ + need - heap_mapped_end_ + 0xFFFFF) & ~0xFFFFFULL);
                if (!mem_.map(heap_mapped_end_, chunk, "heap")) return 0;
                heap_mapped_end_ += chunk;
            }
            block = heap_next_ + 16;
            heap_next_ += need;
        }
        mem_.write<std::uint64_t>(block - 16, cls);
        heap_live_ += cls;
        return block;
    }
    void zero_fill(std::uint64_t a, std::uint64_t n) {
        static const std::uint8_t zeros[4096] = {};
        while (n) { const auto c = (std::min<std::uint64_t>)(n, sizeof zeros); mem_.write_bytes(a, zeros, static_cast<std::size_t>(c)); a += c; n -= c; }
    }
    std::uint64_t heap_size_of(std::uint64_t p) { return p ? mem_.read<std::uint64_t>(p - 16) : 0; }
    void heap_free(std::uint64_t p) {
        if (!p || p < xrt::kHeapBase + 16 || p >= heap_next_) return;
        const std::uint64_t cls = mem_.read<std::uint64_t>(p - 16);
        if (cls == 0 || (cls & 15)) return;
        heap_live_ -= (std::min)(heap_live_, cls);
        free_lists_[cls].push_back(p);
    }
    std::uint64_t heap_realloc(std::uint64_t p, std::uint64_t n) {
        if (!p) return heap_alloc(n, false);
        if (n == 0) { heap_free(p); return 0; }
        const std::uint64_t old = heap_size_of(p);
        if (n <= old) return p;
        const std::uint64_t q = heap_alloc(n, false);
        if (!q) return 0;
        copy_guest(q, p, old);
        heap_free(p);
        return q;
    }
    void copy_guest(std::uint64_t dst, std::uint64_t src, std::uint64_t n) {
        if (n == 0) return;
        if (n > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()) ||
            dst > (std::numeric_limits<std::uint64_t>::max)() - n ||
            src > (std::numeric_limits<std::uint64_t>::max)() - n)
            throw std::runtime_error("guest memory copy size/address overflow");
        std::vector<std::uint8_t> tmp(static_cast<std::size_t>((std::min<std::uint64_t>)(n, 1ULL << 20)));
        if (dst > src && dst < src + n) { // overlapping, copy backwards in chunks
            std::uint64_t left = n;
            while (left) {
                const std::uint64_t c = (std::min<std::uint64_t>)(left, tmp.size());
                mem_.read_bytes(src + left - c, tmp.data(), static_cast<std::size_t>(c));
                mem_.write_bytes(dst + left - c, tmp.data(), static_cast<std::size_t>(c));
                left -= c;
            }
            return;
        }
        std::uint64_t done = 0;
        while (done < n) {
            const std::uint64_t c = (std::min<std::uint64_t>)(n - done, tmp.size());
            mem_.read_bytes(src + done, tmp.data(), static_cast<std::size_t>(c));
            mem_.write_bytes(dst + done, tmp.data(), static_cast<std::size_t>(c));
            done += c;
        }
    }

    // ----------------------------------------------------------- strings
    static std::string utf16_to_utf8(const std::u16string& w) {
        std::string s;
        for (std::size_t i = 0; i < w.size(); ++i) {
            std::uint32_t c = w[i];
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < w.size() && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00); ++i; }
            if (c < 0x80) s.push_back(static_cast<char>(c));
            else if (c < 0x800) { s.push_back(static_cast<char>(0xC0 | (c >> 6))); s.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
            else if (c < 0x10000) { s.push_back(static_cast<char>(0xE0 | (c >> 12))); s.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F))); s.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
            else { s.push_back(static_cast<char>(0xF0 | (c >> 18))); s.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F))); s.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F))); s.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
        }
        return s;
    }
    static std::u16string utf8_to_utf16(const std::string& s) {
        std::u16string w;
        for (std::size_t i = 0; i < s.size();) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            std::uint32_t cp; std::size_t n;
            if (c < 0x80) { cp = c; n = 1; } else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; } else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; } else { cp = c & 0x07; n = 4; }
            for (std::size_t k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
            i += n;
            if (cp >= 0x10000) { cp -= 0x10000; w.push_back(static_cast<char16_t>(0xD800 + (cp >> 10))); w.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF))); }
            else w.push_back(static_cast<char16_t>(cp));
        }
        return w;
    }
    void write_wstr(std::uint64_t dst, const std::u16string& w) {
        for (std::size_t i = 0; i < w.size(); ++i) mem_.write<std::uint16_t>(dst + i * 2, w[i]);
        mem_.write<std::uint16_t>(dst + w.size() * 2, 0);
    }
    void ensure_wide_crt_state() {
        if (wide_crt_state_ready_) return;
        const auto arg0 = utf8_to_utf16("SomeBoxOneTitle.exe");
        wide_arg0_ = static_alloc((arg0.size() + 1) * 2, 2);
        write_wstr(wide_arg0_, arg0);
        wide_argv_ = static_alloc(16, 8);
        mem_.write<std::uint64_t>(wide_argv_, wide_arg0_);
        mem_.write<std::uint64_t>(wide_argv_ + 8, 0);
        wide_env_ = static_alloc(8, 8);
        mem_.write<std::uint64_t>(wide_env_, 0);
        const auto cmd = utf8_to_utf16(opt_.command_line);
        wide_cmdline_ = static_alloc((cmd.size() + 1) * 2, 2);
        write_wstr(wide_cmdline_, cmd);
        wide_argc_slot_ = static_alloc(4, 4);
        mem_.write<std::int32_t>(wide_argc_slot_, 1);
        wide_argv_slot_ = static_alloc(8, 8);
        mem_.write<std::uint64_t>(wide_argv_slot_, wide_argv_);
        wide_env_slot_ = static_alloc(8, 8);
        mem_.write<std::uint64_t>(wide_env_slot_, wide_env_);
        wide_cmdline_slot_ = static_alloc(8, 8);
        mem_.write<std::uint64_t>(wide_cmdline_slot_, wide_cmdline_);
        wide_crt_state_ready_ = true;
        milestone("CRT wide argv/environment initialized with one synthetic executable argument and an empty environment");
    }

    // ----------------------------------------------------------- printf engine
    struct VaList {
        GuestMemory& m; std::uint64_t p;
        std::uint64_t next() { const auto v = m.read<std::uint64_t>(p); p += 8; return v; }
    };
    std::string guest_format(const std::string& fmt, std::uint64_t va, bool wide) {
        VaList v{mem_, va};
        std::string out;
        for (std::size_t i = 0; i < fmt.size(); ++i) {
            if (fmt[i] != '%') { out.push_back(fmt[i]); continue; }
            if (++i >= fmt.size()) break;
            if (fmt[i] == '%') { out.push_back('%'); continue; }
            std::string flags; int width = -1, prec = -1;
            while (i < fmt.size() && std::string("-+ #0").find(fmt[i]) != std::string::npos) flags.push_back(fmt[i++]);
            if (i < fmt.size() && fmt[i] == '*') { width = static_cast<int>(static_cast<std::int32_t>(v.next())); if (width < 0) { flags.push_back('-'); width = -width; } ++i; }
            else { std::string w; while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) w.push_back(fmt[i++]); if (!w.empty()) width = std::atoi(w.c_str()); }
            if (i < fmt.size() && fmt[i] == '.') {
                ++i; prec = 0;
                if (i < fmt.size() && fmt[i] == '*') { prec = static_cast<int>(static_cast<std::int32_t>(v.next())); ++i; }
                else { std::string p; while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) p.push_back(fmt[i++]); prec = std::atoi(p.c_str()); }
            }
            std::string len;
            while (i < fmt.size() && std::string("hlLzjtIw").find(fmt[i]) != std::string::npos) {
                if (fmt[i] == 'I') { // I, I32, I64
                    len = "I"; ++i;
                    if (fmt.compare(i, 2, "64") == 0) { len = "ll"; i += 2; } else if (fmt.compare(i, 2, "32") == 0) { len = ""; i += 2; } else len = "z";
                    break;
                }
                len.push_back(fmt[i++]);
            }
            if (i >= fmt.size()) break;
            const char conv = fmt[i];
            auto pad = [&](std::string s) {
                if (width > 0 && static_cast<int>(s.size()) < width) {
                    if (flags.find('-') != std::string::npos) s.append(static_cast<std::size_t>(width) - s.size(), ' ');
                    else s.insert(0, static_cast<std::size_t>(width) - s.size(), (flags.find('0') != std::string::npos && conv != 's' && conv != 'c') ? '0' : ' ');
                }
                return s;
            };
            char buf[512];
            std::string spec = "%" + flags;
            if (width >= 0) spec += std::to_string(width);
            if (prec >= 0) spec += "." + std::to_string(prec);
            switch (conv) {
            case 'd': case 'i': {
                const std::uint64_t raw = v.next();
                long long val = (len == "ll" || len == "z" || len == "j" || len == "t" || len == "L") ? static_cast<long long>(raw) : (len == "h" ? static_cast<short>(raw) : len == "hh" ? static_cast<signed char>(raw) : static_cast<std::int32_t>(raw));
                std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), val); out += buf; break;
            }
            case 'u': case 'x': case 'X': case 'o': {
                const std::uint64_t raw = v.next();
                unsigned long long val = (len == "ll" || len == "z" || len == "j" || len == "t" || len == "L") ? raw : (len == "h" ? static_cast<std::uint16_t>(raw) : len == "hh" ? static_cast<std::uint8_t>(raw) : static_cast<std::uint32_t>(raw));
                std::snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), val); out += buf; break;
            }
            case 'p': { std::snprintf(buf, sizeof buf, "%016llX", static_cast<unsigned long long>(v.next())); out += pad(buf); break; }
            case 'c': { const auto ch = static_cast<char16_t>(v.next() & 0xFFFF); std::string s = ch < 0x80 ? std::string(1, static_cast<char>(ch)) : utf16_to_utf8(std::u16string(1, ch)); out += pad(s); break; }
            case 's': case 'S': case 'Z': {
                const bool is_wide = (len == "l" || len == "w") ? true : (conv == 'S' ? !wide : wide);
                const std::uint64_t ptr = v.next();
                std::string s;
                if (!ptr) s = "(null)";
                else if (is_wide) s = utf16_to_utf8(mem_.read_wstring(ptr));
                else s = mem_.read_cstring(ptr);
                if (prec >= 0 && static_cast<int>(s.size()) > prec) s.resize(static_cast<std::size_t>(prec));
                out += pad(s); break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                double d; const std::uint64_t raw = v.next(); std::memcpy(&d, &raw, 8);
                std::snprintf(buf, sizeof buf, (spec + conv).c_str(), d); out += buf; break;
            }
            case 'n': v.next(); break;
            default: out.push_back('%'); out.push_back(conv); break;
            }
        }
        return out;
    }

    // ----------------------------------------------------------- file system sandbox
    std::filesystem::path resolve_path(std::string p, bool for_write) const {
        std::replace(p.begin(), p.end(), '\\', '/');
        if (p.size() >= 2 && p[1] == ':') p = p.substr(2);
        while (!p.empty() && p[0] == '/') p.erase(0, 1);
        std::filesystem::path rel;
        std::stringstream ss(p); std::string part;
        while (std::getline(ss, part, '/')) { if (part.empty() || part == ".") continue; if (part == "..") continue; rel /= part; }
        const std::filesystem::path& root = opt_.package_root; const std::filesystem::path& save = opt_.save_root;
        if (!for_write) { const auto cand = root / rel; if (std::filesystem::exists(cand)) return cand; const auto sc = save / rel; if (std::filesystem::exists(sc)) return sc; return cand; }
        return save / rel;
    }

    // ----------------------------------------------------------- calls into guest code
    std::uint64_t call_guest(GuestCpu& c, std::uint64_t fn, std::initializer_list<std::uint64_t> args) {
        if (call_depth_ > 24) { c.request_stop(GuestCpu::Stop::HostRequest, "HLE->guest call nesting too deep"); return 0; }
        const std::uint64_t saved_rsp = c.r[4], saved_rip = c.rip;
        const auto saved_flags = c.rflags;
        std::uint64_t sp = (c.r[4] - 0x400) & ~0xFULL;
        const std::vector<std::uint64_t> a(args);
        const std::size_t extra = a.size() > 4 ? a.size() - 4 : 0;
        sp -= 0x20 + ((extra * 8 + 15) & ~15ULL);
        for (std::size_t i = 4; i < a.size(); ++i) mem_.write<std::uint64_t>(sp + 0x20 + (i - 4) * 8, a[i]);
        sp -= 8; mem_.write<std::uint64_t>(sp, xrt::kSentinel);
        const std::uint64_t regs[4] = {1, 2, 8, 9};
        for (std::size_t i = 0; i < a.size() && i < 4; ++i) c.r[regs[i]] = a[i];
        c.r[4] = sp; c.rip = fn;
        ++call_depth_;
        const std::uint64_t used = c.instructions;
        if (opt_.max_instructions == 0) {
            constexpr std::uint64_t unlimited_chunk = 1'000'000;
            while (c.stop == GuestCpu::Stop::None) {
                if (opt_.cancel && opt_.cancel->load()) {
                    c.request_stop(GuestCpu::Stop::HostRequest, "stopped by the user during guest execution");
                    break;
                }
                c.run(unlimited_chunk, xrt::kSentinel);
                if (c.stop == GuestCpu::Stop::StepLimit) {
                    c.stop = GuestCpu::Stop::None;
                    c.stop_detail.clear();
                    continue;
                }
                break;
            }
        } else {
            const std::uint64_t budget = opt_.max_instructions > used ? opt_.max_instructions - used : 0;
            c.run(budget, xrt::kSentinel);
        }
        --call_depth_;
        const std::uint64_t rv = c.r[0];
        if (c.stop == GuestCpu::Stop::None) { c.r[4] = saved_rsp; c.rip = saved_rip; c.rflags = saved_flags; }
        return rv;
    }

    // ----------------------------------------------------------- setup
    bool setup(std::string* err) {
        std::error_code ec;
        if (opt_.save_root.empty()) opt_.save_root = opt_.package_root.parent_path() / "SomeBoxOneSave";
        if (!image_.map_into(mem_, image_.image_base, err)) return false;

        // function table for symbolization
        if (image_.exception_rva && image_.exception_size) {
            const std::uint32_t n = image_.exception_size / 12;
            for (std::uint32_t i = 0; i < n && i < 1000000; ++i) {
                const std::uint64_t a = image_.image_base + image_.exception_rva + i * 12ULL;
                runtime_functions_.emplace_back(mem_.read<std::uint32_t>(a), mem_.read<std::uint32_t>(a + 4));
            }
            std::sort(runtime_functions_.begin(), runtime_functions_.end());
        }

        if (!mem_.map(xrt::kStackTop - xrt::kStackSize, xrt::kStackSize, "stack") ||
            !mem_.map(xrt::kTebBase, 0x20000, "teb/peb") || !mem_.map(xrt::kKusd, 0x1000, "kuser_shared_data") ||
            !mem_.map(xrt::kTrapBase, xrt::kTrapSlots * 16, "hle-traps")) { *err = "cannot map process structures"; return false; }

        // CRT scratch: errno, ctype table, stdio FILE objects, constant strings
        errno_ptr_ = static_alloc(16);
        commode_ptr_ = static_alloc(16);
        iob_base_ = static_alloc(0x100 * 3);
        next_file_slot_ = static_alloc(0x100 * 256);
        ctype_ptr_ = static_alloc(257 * 2 + 16);
        for (int ch = -1; ch < 256; ++ch) { // table is indexed from -1; pointer returned points at entry 0
            std::uint16_t f = 0;
            if (ch >= 0 && ch < 128) {
                if (ch >= 'A' && ch <= 'Z') f |= 0x1 | 0x100;
                if (ch >= 'a' && ch <= 'z') f |= 0x2 | 0x100;
                if (ch >= '0' && ch <= '9') f |= 0x4 | 0x80;
                if (ch == ' ' || (ch >= 9 && ch <= 13)) f |= 0x8;
                if ((ch >= 33 && ch <= 47) || (ch >= 58 && ch <= 64) || (ch >= 91 && ch <= 96) || (ch >= 123 && ch <= 126)) f |= 0x10;
                if (ch < 32 || ch == 127) f |= 0x20;
                if (ch == ' ') f |= 0x40;
                if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')) f |= 0x80;
            }
            mem_.write<std::uint16_t>(ctype_ptr_ + static_cast<std::uint64_t>(ch + 1) * 2, f);
        }
        ctype_ptr_ += 2;
        empty_cmdline_ = static_cstr("");
        c_locale_str_ = static_cstr("C");

        // KUSER_SHARED_DATA
        mem_.write<std::uint32_t>(xrt::kKusd + 0x26C, 10);        // NtMajorVersion
        mem_.write<std::uint64_t>(xrt::kKusd + 0x300, 10000000);  // QpcFrequency
        mem_.write<std::uint8_t>(xrt::kKusd + 0x274 + 10, 1);     // PF_XMMI64 (SSE2)
        mem_.write<std::uint8_t>(xrt::kKusd + 0x274 + 6, 1);      // PF_XMMI (SSE)
        mem_.write<std::uint32_t>(xrt::kKusd + 0x2D4, 0);         // KdDebuggerEnabled = off
        mem_.write<std::uint8_t>(xrt::kKusd + 0x2B4, 4);          // NumberOfPhysicalPages-ish padding
        mem_.write<std::uint32_t>(xrt::kKusd + 0x3C0, 4);         // ActiveProcessorCount
        mem_.write<std::uint32_t>(xrt::kKusd + 0x2E0, 0);

        // Stack, TEB, PEB
        const std::uint64_t rsp0 = xrt::kStackTop - 0x1000;
        cpu_.r[4] = rsp0 & ~0xFULL;
        cpu_.gs_base = xrt::kTebBase;
        mem_.write<std::uint64_t>(xrt::kTebBase + 0x08, xrt::kStackTop);
        mem_.write<std::uint64_t>(xrt::kTebBase + 0x10, xrt::kStackTop - xrt::kStackSize);
        mem_.write<std::uint64_t>(xrt::kTebBase + 0x30, xrt::kTebBase);
        mem_.write<std::uint64_t>(xrt::kTebBase + 0x40, 0x400);   // PID
        mem_.write<std::uint64_t>(xrt::kTebBase + 0x48, 0x1000);  // TID
        mem_.write<std::uint64_t>(xrt::kTebBase + 0x60, xrt::kPebBase);
        mem_.write<std::uint64_t>(xrt::kPebBase + 0x10, image_.image_base);
        mem_.write<std::uint32_t>(xrt::kPebBase + 0x118, 10);

        // TLS: one pointer-array slot per module, referenced via TEB.ThreadLocalStoragePointer
        tls_array_ = static_alloc(8 * 64);
        mem_.write<std::uint64_t>(xrt::kTebBase + 0x58, tls_array_);

        // Load real redistributable DLLs the title depends on (from the package's own EmbeddedXvd).
        if (opt_.system_root.empty()) {
            const auto cand = opt_.package_root.parent_path() / "EmbeddedXvd" / "Windows" / "System32";
            if (std::filesystem::exists(cand)) opt_.system_root = cand;
        }
        std::set<std::string> wanted;
        for (const auto& d : image_.import_dlls) wanted.insert(lower(d));
        for (const auto& want : opt_.real_dlls) if (wanted.count(lower(want)) && !load_module(want, err)) return false;

        std::size_t implemented = 0;
        add_tls(image_, image_.image_base, 0);
        bind_imports(image_, image_.image_base, implemented);
        for (auto& m : modules_) bind_imports(m->img, m->base, implemented);
        imports_implemented_ = implemented;
        return true;
    }
    std::size_t imports_implemented_{};
    std::uint64_t tls_array_{};
    std::size_t real_bound_{};

    // True for titles built against the older (Visual Studio 2012) Xbox One XDK, whose DXGI/D3D11 object vtables carry extra methods.
    bool xdk2012_layout() const { for (const auto& imp : image_.imports) if (lower(imp.dll) == "msvcr110") return true; return false; }

    static std::string lower(std::string v) {
        for (auto& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (v.size() > 4 && v.compare(v.size() - 4, 4, ".dll") == 0) v.resize(v.size() - 4);
        return v;
    }
    void add_tls(const PeImage& img, std::uint64_t base, std::uint32_t index) {
        (void)base;
        if (!img.tls.present) return;
        const std::uint64_t len = img.tls.raw_end_va - img.tls.raw_start_va;
        const std::uint64_t block = static_alloc(len + img.tls.zero_fill + 64);
        if (len) copy_guest(block, img.tls.raw_start_va, len);
        mem_.write<std::uint64_t>(tls_array_ + index * 8ULL, block);
        tls_templates_.push_back({index, img.tls.raw_start_va, len, img.tls.zero_fill});
        mem_.write<std::uint32_t>(img.tls.index_va, index);
    }
    bool is_free(std::uint64_t base, std::uint64_t size) const {
        if (base < image_.image_base + image_.size_of_image && base + size > image_.image_base) return false;
        for (const auto& m : modules_) if (base < m->base + m->img.size_of_image && base + size > m->base) return false;
        return true;
    }
    bool load_module(const std::string& file, std::string* err) {
        const std::string key = lower(file);
        for (const auto& m : modules_) if (lower(m->name) == key) return true;
        std::filesystem::path found;
        if (!opt_.system_root.empty()) for (const auto& e : std::filesystem::directory_iterator(opt_.system_root))
            if (lower(e.path().filename().string()) == key) { found = e.path(); break; }
        if (found.empty()) { milestone("DLL " + file + " not found in package system folder; using HLE for its imports"); return true; }
        std::string perr;
        auto img = PeImage::load_file(found, &perr);
        if (!img) { *err = "failed to parse " + file + ": " + perr; return false; }
        auto m = std::make_unique<Module>();
        m->name = file; m->img = std::move(*img);
        for (const auto& d : m->img.import_dlls) { // dependencies first so their exports exist when we bind
            bool want = false; for (const auto& w : opt_.real_dlls) if (lower(w) == lower(d)) want = true;
            if (want && lower(d) != key && !load_module(d, err)) return false;
        }
        std::uint64_t base = m->img.image_base;
        const bool clash = !is_free(base, m->img.size_of_image);
        if (clash) { base = next_module_base_; next_module_base_ += (m->img.size_of_image + 0xFFFFFULL) & ~0xFFFFFULL; }
        if (!m->img.map_into(mem_, base, err)) return false;
        m->base = base;
        m->tls_index = static_cast<std::uint32_t>(modules_.size() + 1);
        for (const auto& e : m->img.exports_by_name) m->sorted_exports.emplace_back(e.second, e.first);
        std::sort(m->sorted_exports.begin(), m->sorted_exports.end());
        add_tls(m->img, base, m->tls_index);
        milestone("loaded real DLL " + file + " at 0x" + GuestCpu::hex(base) + (clash ? " (relocated)" : "") + ", " + std::to_string(m->img.exports_by_name.size()) + " named exports");
        modules_.push_back(std::move(m));
        return true;
    }
    void bind_imports(const PeImage& img, std::uint64_t base, std::size_t& implemented) {
        for (const auto& imp : img.imports) {
            const std::string nm = imp.by_ordinal ? ("#" + std::to_string(imp.ordinal)) : imp.name;
            std::uint64_t real = 0;
            for (const auto& m : modules_) if (lower(m->name) == lower(imp.dll)) {
                if (imp.by_ordinal) { const auto it = m->img.exports_by_ordinal.find(imp.ordinal); if (it != m->img.exports_by_ordinal.end()) real = m->base + it->second; }
                else { const auto it = m->img.exports_by_name.find(imp.name); if (it != m->img.exports_by_name.end()) real = m->base + it->second; }
            }
            if (real) { mem_.write<std::uint64_t>(base + imp.iat_rva, real); ++real_bound_; continue; }
            if (nm == "_fmode" || nm == "_commode" || nm == "__initenv") { // data imports: the IAT slot must hold the variable's address, not a code trap
                auto& cell = crt_data_imports_[nm];
                if (!cell) cell = static_alloc(16, 16);
                mem_.write<std::uint64_t>(base + imp.iat_rva, cell); continue;
            }
            if (bindings_.size() + 1 >= xrt::kTrapSlots) continue;
            ImportBinding b; b.dll = imp.dll; b.name = nm; b.trap = xrt::kTrapBase + bindings_.size() * 16;
            const auto it = handlers_.find(nm);
            if (it != handlers_.end()) { b.handler = it->second; ++implemented; } else b.stub = true;
            if (b.stub && std::getenv("SOMEBOXONE_LIST_STUBS")) std::fprintf(stderr, "[stub] %s!%s\n", imp.dll.c_str(), nm.c_str());
            mem_.write<std::uint64_t>(base + imp.iat_rva, b.trap);
            bindings_.push_back(std::move(b));
        }
    }

    // ----------------------------------------------------------- trap dispatch
    void dispatch_trap(GuestCpu& c) {
        const std::uint64_t idx = (c.rip - xrt::kTrapBase) / 16;
        if (idx >= bindings_.size()) { c.request_stop(GuestCpu::Stop::HostRequest, "jump into unbound HLE trap slot " + std::to_string(idx)); return; }
        ImportBinding* const bind = &bindings_[idx];
        const std::string& name = bind->name;     // no per-call copies of the name or the std::function: this runs for every import call
        const Handler& h = bind->handler;
        const bool retrying = cur_thread_ && cur_thread_->waiting && cur_thread_->wait_rsp == c.r[4];   // a blocked thread re-executing its wait: don't spam the counters/ring
        if (!retrying) {
            ++bind->calls;
            RingEntry& e = ring_[ring_pos_++ % ring_.size()];
            e.name = &name; e.a[0] = c.r[1]; e.a[1] = c.r[2]; e.a[2] = c.r[8]; e.a[3] = c.r[9];
            if (opt_.trace_hle) std::fprintf(stderr, "[hle] %s(0x%llx, 0x%llx, 0x%llx, 0x%llx)\n", name.c_str(), (unsigned long long)c.r[1], (unsigned long long)c.r[2], (unsigned long long)c.r[8], (unsigned long long)c.r[9]);
        }

        const std::uint64_t ret_addr = mem_.read<std::uint64_t>(c.r[4]);
        const std::uint64_t entry_rsp = c.r[4];
        if (!caller_watch_.empty() && name == caller_watch_) ++caller_hist_[ret_addr];   // SOMEBOXONE_CALLERS=<import>: who calls it?
        try {
            if (h) h(c);
            else {
                stubs_hit_.insert((bind ? bind->dll + "!" : std::string()) + name);
                if (stubs_hit_.size() <= 400 && bind->calls == 1) milestone("UNIMPLEMENTED import first called: " + (bind ? bind->dll + "!" : std::string()) + name + " (returned 0)");
                c.r[0] = 0;
            }
        } catch (const GuestFault& f) {
            c.last_fault = f; c.fault_rip = c.rip;
            c.request_stop(GuestCpu::Stop::MemoryFault, "HLE handler " + name + " touched unmapped guest address 0x" + GuestCpu::hex(f.address));
        }
        if (c.stop != GuestCpu::Stop::None) { hle_retry_ = pending_yield_ = false; return; }
        if (hle_retry_) {   // the call could not complete yet: stay on the trap (RIP/RSP/args untouched) and let other threads run
            hle_retry_ = false;
            if (pending_yield_) { pending_yield_ = false; c.request_stop(GuestCpu::Stop::StepLimit, "yield"); }
            return;
        }
        if (c.r[4] == entry_rsp) { c.r[4] += 8; c.rip = ret_addr; }  // normal return; handlers that redirected RIP/RSP keep their own
        if (pending_yield_) { pending_yield_ = false; c.request_stop(GuestCpu::Stop::StepLimit, "yield"); }
    }

    std::uint64_t trap_for_name(const std::string& n) {
        for (const auto& b : bindings_) if (b.name == n && b.dll == "(GetProcAddress)") return b.trap;
        if (bindings_.size() + 1 >= xrt::kTrapSlots) return 0;
        ImportBinding b; b.dll = "(GetProcAddress)"; b.name = n; b.trap = xrt::kTrapBase + bindings_.size() * 16;
        const auto it = handlers_.find(n); if (it != handlers_.end()) b.handler = it->second; else b.stub = true;
        bindings_.push_back(b);
        return b.trap;
    }

    // ----------------------------------------------------------- handler registry
    void H(const char* name, Handler h) { handlers_[name] = std::move(h); }

    static double getd(GuestCpu& c, int i = 0) { return c.xmm[static_cast<std::size_t>(i)].df[0]; }
    static float getf(GuestCpu& c, int i = 0) { return c.xmm[static_cast<std::size_t>(i)].f[0]; }
    static void setd(GuestCpu& c, double v) { c.xmm[0].df[0] = v; }
    static void setf(GuestCpu& c, float v) { c.xmm[0].f[0] = v; }

    static std::FILE* open_host_file(const std::filesystem::path& p, const std::string& mode) {
#if defined(_WIN32)
        return _wfopen(p.c_str(), std::wstring(mode.begin(), mode.end()).c_str());
#else
        return std::fopen(p.string().c_str(), mode.c_str());
#endif
    }
    std::uint64_t make_file(std::FILE* fp, const std::string& path) {
        const std::uint64_t slot = next_file_slot_;
        next_file_slot_ += 0x100;
        files_[slot] = {fp, path};
        return slot;
    }
    std::FILE* file_of(std::uint64_t p) {
        if (p == iob_base_ + 0x00) return stdin;
        if (p == iob_base_ + 0x100) return nullptr; // stdout handled by log capture
        const auto it = files_.find(p);
        return it == files_.end() ? nullptr : it->second.fp;
    }
    std::vector<std::string> stdout_log_;

    void register_handlers();
    void register_crt_handlers();
    void register_kernel_handlers();
    void register_cpp_handlers();
    void register_extra_handlers();
    void register_kernel_baseline();
    void register_crt_vs2012_handlers();
    // ---- VS2012 CRT state (xbox_crt_vs2012.hpp) ----
    std::uint64_t crt_iob_{}, crt_strerror_{}, crt_va_scratch_{}, crt_exc_vtable_{}, crt_unknown_exc_{}, crt_tm_{};
    std::map<std::string, std::uint64_t> crt_data_imports_;   // data (not function) imports: _fmode, _commode, __initenv
    // ---- kernel baseline state (xbox_kernel_baseline.hpp) ----
    std::vector<std::uint64_t> tls_slots_, fls_slots_;
    struct VirtRegion { std::uint64_t size{}; };
    std::map<std::uint64_t, VirtRegion> virt_regions_;
    std::uint64_t virt_next_{xrt::kVirtBase};
    std::uint64_t heap_handles_{};
    std::uint64_t cmdline_w_{}, cmdline_a_{}, env_block_{};
    struct WinHandle {
        enum Kind { File, Event, Semaphore, Mutex, Find, Thread } kind{File};
        std::size_t thread_index{}; std::uint32_t owner{};   // Thread: index into threads_; Mutex: owning guest thread id (count = recursion)
        std::FILE* fp{}; std::string path; bool writable{};
        bool manual{}, signaled{}; std::int64_t count{};
        std::vector<std::string> entries; std::filesystem::path dir; std::size_t index{};
    };
    std::map<std::uint64_t, WinHandle> win_handles_;
    std::uint64_t win_handle_counter_{0x1000};
    std::uint64_t next_win_handle_() { win_handle_counter_ += 4; return win_handle_counter_; }
    static std::uint64_t kInvalidHandleValue() { return ~0ULL; }
    std::uint64_t virtual_alloc(std::uint64_t addr, std::uint64_t size, std::uint32_t type);
    void fill_system_info(std::uint64_t out);
    std::uint64_t tick_ms();
    std::uint64_t win_create_file(const std::string& path, std::uint64_t access, std::uint32_t disposition);
    void fill_find_data(std::uint64_t out, const WinHandle& h);
    std::uint64_t locale_names_{}, mb_cur_max_{}, lconv_{}, cur_exc_{}, proc_throw_{};
    std::set<std::string> winrt_requests_;
    // ---- module handles for GetModuleHandle*/LoadLibrary*/GetProcAddress ----
    // Real loaded DLLs resolve to their true image base (exports come from the PE). Known OS DLL names resolve to a synthetic
    // handle whose exports are the HLE handlers; unknown names fail with ERROR_MOD_NOT_FOUND (126) / ERROR_PROC_NOT_FOUND (127).
    std::map<std::string, std::uint64_t> os_modules_;
    std::uint64_t next_os_module_{0x00007FF4B0000000ULL};
    static bool is_system_dll_stem(const std::string& stem) {
        static const char* const names[] = {"kernel32", "kernelbase", "kernelx", "ntdll", "user32", "gdi32", "advapi32", "ole32", "oleaut32",
            "combase", "ucrtbase", "bcrypt", "d3d11", "d3d11_x", "dxgi", "dxgi_x", "xaudio2_9", "shell32", "shcore", "version", "winmm", "ws2_32",
            "rpcrt4", "sechost", "msvcrt", "normaliz", "iphlpapi", "crypt32", "wininet", "winhttp", "windowscodecs", "twinapi.appcore", "coremessaging",
            "xgameruntime", "xinput1_4", "xinput9_1_0", "mmdevapi", "audioses", "avrt", "userenv", "dbghelp", "psapi", "setupapi", "cfgmgr32", "ntmarta"};
        for (const char* n : names) if (stem == n) return true;
        return stem.rfind("api-ms-win-", 0) == 0 || stem.rfind("ext-ms-win-", 0) == 0;
    }
    std::uint64_t module_handle_for(const std::string& name_utf8) {
        std::string n = name_utf8;
        std::replace(n.begin(), n.end(), '/', '\\');
        const auto slash = n.find_last_of('\\'); if (slash != std::string::npos) n = n.substr(slash + 1);
        const std::string stem = lower(n);
        for (const auto& m : modules_) if (lower(m->name) == stem) return m->base;
        if (stem == lower(image_.module_name.empty() ? std::string("\x01") : image_.module_name)) return image_.image_base;
        if (stem.find("xaudio2_9") != std::string::npos) return 0x00007FF4A0D10000ULL;
        if (!is_system_dll_stem(stem)) return 0;
        auto it = os_modules_.find(stem);
        if (it == os_modules_.end()) { it = os_modules_.emplace(stem, next_os_module_).first; next_os_module_ += 0x10000; }
        return it->second;
    }
    bool is_os_module_handle(std::uint64_t h) const { for (const auto& kv : os_modules_) if (kv.second == h) return true; return false; }

    // ---- guest-visible COM objects ----
    // An object is {vtable*, refcount, ...slots}. Each vtable entry is an HLE trap bound to a host
    // handler, so the title (or a real DLL such as vccorlib140) can call methods through normal
    // COM vtable dispatch. IUnknown (QueryInterface/AddRef/Release) is provided for every object.
    using ComMethods = std::vector<std::pair<std::string, Handler>>;
    // `winrt` adds the three IInspectable slots (GetIids/GetRuntimeClassName/GetTrustLevel). The vtable is padded
    // to `total_slots`; any padding slot that gets called is logged by index and returns E_NOTIMPL, which is the
    // "discovery mode" that tells us exactly which method the title needs next.
    std::uint64_t make_com_object(const std::string& cls, const ComMethods& methods, std::set<std::string> extra_iids = {},
                                  bool winrt = false, unsigned total_slots = 0, std::uint64_t object_bytes = 64) {
        const unsigned base = winrt ? 6u : 3u;
        const unsigned slots = (std::max)(total_slots, base + static_cast<unsigned>(methods.size()));
        const std::uint64_t vt = static_alloc(slots * 8ULL);
        auto bind = [&](unsigned slot, const std::string& name, Handler h) {
            const std::string full = "COM:" + cls + "::" + name;
            handlers_[full] = std::move(h);
            mem_.write<std::uint64_t>(vt + slot * 8ULL, trap_for_name(full));
        };
        const std::string cls_copy = cls;
        bind(0, "QueryInterface", [this, cls_copy, extra_iids, winrt](GuestCpu& c) { // (this, riid, ppv)
            const std::uint64_t riid = c.arg(1), ppv = c.arg(2);
            const std::string iid = guid_text(riid);
            { // per-object interface routing (e.g. IAsyncAction <-> IAsyncInfo are different vtables on one logical object)
                const auto po = obj_qi_.find(c.arg(0));
                if (po != obj_qi_.end()) {
                    auto it = po->second.find(iid);
                    if (it == po->second.end()) it = po->second.find("*");
                    if (it != po->second.end()) { mem_.write<std::uint64_t>(ppv, it->second); mem_.write<std::uint64_t>(it->second + 8, mem_.read<std::uint64_t>(it->second + 8) + 1); c.r[0] = 0; return; }
                }
            }
            { const auto ov = iid_objects_.find(iid); if (ov != iid_objects_.end()) { mem_.write<std::uint64_t>(ppv, ov->second); mem_.write<std::uint64_t>(ov->second + 8, mem_.read<std::uint64_t>(ov->second + 8) + 1); c.r[0] = 0; return; } }
            const bool ok = extra_iids.count("*") || iid == "00000000-0000-0000-c000-000000000046" || (winrt && iid == "af86e2e0-b12d-4c6a-9c5a-d7aa65101e90") ||
                            iid == "94ea2b94-e9cc-49e0-c0ff-ee64ca8f5b90" || extra_iids.count(iid);
            if (ok) { mem_.write<std::uint64_t>(ppv, c.arg(0)); mem_.write<std::uint64_t>(c.arg(0) + 8, mem_.read<std::uint64_t>(c.arg(0) + 8) + 1); c.r[0] = 0; return; }
            if (com_log_.insert(cls_copy + " QI " + iid).second) milestone("COM " + cls_copy + "::QueryInterface(" + iid + ") -> E_NOINTERFACE");
            mem_.write<std::uint64_t>(ppv, 0); c.r[0] = 0x80004002ULL;
        });
        bind(1, "AddRef", [this](GuestCpu& c) { const auto n = mem_.read<std::uint64_t>(c.arg(0) + 8) + 1; mem_.write<std::uint64_t>(c.arg(0) + 8, n); c.r[0] = n; });
        bind(2, "Release", [this](GuestCpu& c) { auto n = mem_.read<std::uint64_t>(c.arg(0) + 8); if (n) --n; mem_.write<std::uint64_t>(c.arg(0) + 8, n); c.r[0] = n; });
        if (winrt) {
            bind(3, "GetIids", [this](GuestCpu& c) { mem_.write<std::uint32_t>(c.arg(1), 0); mem_.write<std::uint64_t>(c.arg(2), 0); c.r[0] = 0; });
            bind(4, "GetRuntimeClassName", [this, cls_copy](GuestCpu& c) { mem_.write<std::uint64_t>(c.arg(1), make_hstring(utf8_to_utf16(cls_copy))); c.r[0] = 0; });
            bind(5, "GetTrustLevel", [this](GuestCpu& c) { mem_.write<std::uint32_t>(c.arg(1), 0); c.r[0] = 0; });
        }
        for (std::size_t i = 0; i < methods.size(); ++i) bind(base + static_cast<unsigned>(i), methods[i].first, methods[i].second);
        for (unsigned slot = base + static_cast<unsigned>(methods.size()); slot < slots; ++slot) {
            bind(slot, "slot" + std::to_string(slot), [this, cls_copy, slot](GuestCpu& c) {
                // Canonical WinRT event shapes are recognised and served: add_X(handler: guest heap object, token*: stack) and
                // remove_X(token: one we issued). Anything else is a real method and stops with a precise report.
                const bool handler_like = c.arg(1) >= xrt::kHeapBase && c.arg(1) < xrt::kHeapMax;
                const bool out_on_stack = c.arg(2) >= xrt::kStackTop - xrt::kStackSize && c.arg(2) < xrt::kStackTop;
                if (handler_like && out_on_stack) {
                    const std::uint64_t tok = next_event_token_++;
                    event_subs_.push_back({cls_copy + ".slot" + std::to_string(slot), c.arg(1), tok});
                    guest_vcall(c, c.arg(1), 1, {});   // the event source keeps the delegate alive (AddRef)
                    if (com_log_.insert(cls_copy + " ev" + std::to_string(slot)).second)
                        milestone("WinRT event registered (shape-inferred add_X): " + cls_copy + " slot " + std::to_string(slot) + ", handler 0x" + GuestCpu::hex(c.arg(1)) + ", Invoke at " + handler_invoke_location(c.arg(1)));
                    mem_.write<std::uint64_t>(c.arg(2), tok); c.r[0] = 0; return;
                }
                if (c.arg(1) >= 0x7000 && c.arg(1) < next_event_token_) { c.r[0] = 0; return; }
                if (opt_.permissive_xbox_services && (cls_copy.rfind("Windows.Xbox.", 0) == 0 || cls_copy.rfind("Microsoft.Xbox.", 0) == 0)) {
                    if (com_log_.insert("permissive:" + cls_copy + ":" + std::to_string(slot)).second)
                        milestone("WinRT " + cls_copy + " method slot " + std::to_string(slot) + " served as an offline no-op (S_OK, empty result; called from " + locate(mem_.read<std::uint64_t>(c.r[4])) + ")");
                    permissive_result(c, cls_copy); c.r[0] = 0; return;
                }
                last_discovery_ = cls_copy + " method slot " + std::to_string(slot);
                if (!com_log_.insert("disc:" + cls_copy + ":" + std::to_string(slot)).second) { c.r[0] = 0x80004001ULL; return; }
                milestone("WinRT DISCOVERY: " + cls_copy + " method slot " + std::to_string(slot) + " this=0x" + GuestCpu::hex(c.arg(0)) + " called but not implemented (args rdx=0x" + GuestCpu::hex(c.arg(1)) +
                          " r8=0x" + GuestCpu::hex(c.arg(2)) + " r9=0x" + GuestCpu::hex(c.arg(3)) + "), called from " + locate(mem_.read<std::uint64_t>(c.r[4])) + " -> E_NOTIMPL");
                c.r[0] = 0x80004001ULL;
            });
        }
        const std::uint64_t obj = static_alloc(object_bytes);
        mem_.write<std::uint64_t>(obj, vt); mem_.write<std::uint64_t>(obj + 8, 1);
        return obj;
    }
    // ---- Windows Runtime asynchronous operations ----
    // A WinRT async object is two COM interfaces over one state: the typed interface (IAsyncAction or IAsyncOperation<T>:
    // put_Completed=6, get_Completed=7, GetResults=8) and IAsyncInfo (get_Id=6, get_Status=7, get_ErrorCode=8, Cancel=9,
    // Close=10). They have DIFFERENT vtables, so a single flat vtable mis-dispatches (put_Completed would run get_Id and
    // overwrite the title's delegate). QueryInterface routes between the two objects by IID. All operations are created
    // already Completed; put_Completed invokes the title's delegate immediately (AsyncOperationCompletedHandler::Invoke
    // is vtable slot 3), which is what Windows does when a handler is attached to a finished operation.
    struct AsyncOp { std::uint64_t typed{}, info{}, handler{}, result{}; bool has_result{}; std::uint32_t id{}, status{1}, error{}; std::string label; };
    std::map<std::uint64_t, AsyncOp> async_ops_;   // keyed by the typed object's address
    std::map<std::uint64_t, std::map<std::string, std::uint64_t>> obj_qi_;
    std::uint32_t next_async_id_{1};
    std::uint64_t make_async_operation(const std::string& label, bool has_result, std::uint64_t result = 0, std::uint32_t status = 1, std::uint32_t error = 0) {
        ComMethods tm, im;
        tm.emplace_back("put_Completed", [this](GuestCpu& c) { // (this, handler)
            auto it = async_ops_.find(c.arg(0)); if (it == async_ops_.end()) { c.r[0] = 0x80004003ULL; return; }
            AsyncOp& op = it->second; op.handler = c.arg(1);
            if (op.handler && op.status != 0) {
                if (com_log_.insert("async-complete:" + op.label).second) milestone("WinRT async \"" + op.label + "\": put_Completed -> invoking the title's completion delegate immediately (status " + std::to_string(op.status) + ")");
                const std::uint64_t typed = op.typed, status = op.status, handler = op.handler;
                guest_vcall(c, handler, 3, {typed, status});
            }
            c.r[0] = 0; });
        tm.emplace_back("get_Completed", [this](GuestCpu& c) { const auto it = async_ops_.find(c.arg(0)); if (c.arg(1)) mem_.write<std::uint64_t>(c.arg(1), it == async_ops_.end() ? 0 : it->second.handler); c.r[0] = 0; });
        tm.emplace_back("GetResults", [this](GuestCpu& c) { // IAsyncOperation<T>::GetResults(T*) ; IAsyncAction::GetResults() has no out parameter
            const auto it = async_ops_.find(c.arg(0));
            if (it != async_ops_.end() && it->second.has_result && c.arg(1)) {
                mem_.write<std::uint64_t>(c.arg(1), it->second.result);
                if (it->second.result) mem_.write<std::uint64_t>(it->second.result + 8, mem_.read<std::uint64_t>(it->second.result + 8) + 1);
            }
            c.r[0] = it != async_ops_.end() && it->second.status == 3 ? it->second.error : 0; });
        const std::uint64_t typed = make_com_object("SomeBoxOne.Async." + label, tm, {}, true, 12);
        auto find_by_info = [this](std::uint64_t info) -> AsyncOp* { for (auto& kv : async_ops_) if (kv.second.info == info) return &kv.second; return nullptr; };
        im.emplace_back("get_Id", [this, find_by_info](GuestCpu& c) { const auto* o = find_by_info(c.arg(0)); if (c.arg(1)) mem_.write<std::uint32_t>(c.arg(1), o ? o->id : 1); c.r[0] = 0; });
        im.emplace_back("get_Status", [this, find_by_info](GuestCpu& c) { const auto* o = find_by_info(c.arg(0)); if (c.arg(1)) mem_.write<std::uint32_t>(c.arg(1), o ? o->status : 1); c.r[0] = 0; });
        im.emplace_back("get_ErrorCode", [this, find_by_info](GuestCpu& c) { const auto* o = find_by_info(c.arg(0)); if (c.arg(1)) mem_.write<std::uint32_t>(c.arg(1), o ? o->error : 0); c.r[0] = 0; });
        im.emplace_back("Cancel", [](GuestCpu& c) { c.r[0] = 0; });
        im.emplace_back("Close", [](GuestCpu& c) { c.r[0] = 0; });
        const std::uint64_t info = make_com_object("SomeBoxOne.AsyncInfo." + label, im, {}, true, 12);
        AsyncOp op; op.typed = typed; op.info = info; op.has_result = has_result; op.result = result; op.status = status; op.error = error;
        op.id = next_async_id_++; op.label = label;
        async_ops_[typed] = op;
        const std::string kUnk = "00000000-0000-0000-c000-000000000046", kInsp = "af86e2e0-b12d-4c6a-9c5a-d7aa65101e90", kInfo = "00000036-0000-0000-c000-000000000046";
        obj_qi_[typed][kInfo] = info;                                   // typed -> IAsyncInfo
        for (const auto& k : {kUnk, kInsp, kInfo}) obj_qi_[info][k] = info; // IAsyncInfo stays itself for the base interfaces
        obj_qi_[info]["*"] = typed;                                     // any other IID (IAsyncAction / IAsyncOperation<T>) -> typed object
        return typed;
    }
    // HSTRING (simplified layout used consistently by every combase HLE handler): {u32 len, u32 flags, u16 data[]}
    std::uint64_t make_hstring(const std::u16string& w) {
        if (w.empty()) return 0;
        const std::uint64_t h = heap_alloc(8 + (w.size() + 1) * 2, true);
        mem_.write<std::uint32_t>(h, static_cast<std::uint32_t>(w.size()));
        for (std::size_t i = 0; i < w.size(); ++i) mem_.write<std::uint16_t>(h + 8 + i * 2, w[i]);
        return h;
    }
    // WinRT class registry: runtime class name -> factory object creator (see xbox_winrt.hpp).
    std::map<std::string, std::function<std::uint64_t()>> winrt_classes_;
    std::map<std::string, std::uint64_t> winrt_objects_;
    void register_winrt_classes();
    std::uint64_t make_empty_vector_view();
    std::uint64_t make_account_picker_result();
    std::uint64_t make_xbox_user();
    std::uint64_t xbox_user_{}, picker_result_{};
    bool user_signed_in_{};
    std::uint64_t make_vector_view(const std::vector<std::uint64_t>& items);
    std::uint64_t gamepad_object_{};
    struct InputStep { std::uint64_t from{}, to{}; std::uint32_t buttons{}; };
    std::vector<InputStep> input_steps_;
    bool input_parsed_{};
    void append_input_script(const std::string& script) {
        static const std::map<std::string, std::uint32_t> names = {{"menu", 1}, {"start", 1}, {"view", 2}, {"back", 2}, {"a", 4}, {"b", 8}, {"x", 0x10}, {"y", 0x20},
            {"up", 0x40}, {"down", 0x80}, {"left", 0x100}, {"right", 0x200}, {"lb", 0x400}, {"rb", 0x800}, {"ls", 0x1000}, {"rs", 0x2000}};
        std::stringstream ss(script); std::string step;
        while (std::getline(ss, step, ',')) {
            const auto c1 = step.find(':'), c2 = step.find(':', c1 == std::string::npos ? 0 : c1 + 1);
            if (c1 == std::string::npos || c2 == std::string::npos) continue;
            InputStep st; st.from = std::strtoull(step.substr(0, c1).c_str(), nullptr, 10);
            st.to = st.from + std::strtoull(step.substr(c2 + 1).c_str(), nullptr, 10);
            std::stringstream bs(step.substr(c1 + 1, c2 - c1 - 1)); std::string b;
            while (std::getline(bs, b, '+')) { const auto it = names.find(b); if (it != names.end()) st.buttons |= it->second; }
            input_steps_.push_back(st);
        }
    }
    std::uint32_t scripted_buttons() {
        if (!input_parsed_) {
            input_parsed_ = true;
            append_input_script(opt_.input_script);
            if (!input_steps_.empty()) milestone("scripted gamepad input loaded: " + std::to_string(input_steps_.size()) + " step(s)");
        }
        std::uint32_t b = 0;
        for (const auto& st : input_steps_) if (frames_ >= st.from && frames_ < st.to) b |= st.buttons;
        return b;
    }
    HostGamepadState current_gamepad_state() {
        HostGamepadState state{};
        state.buttons = scripted_buttons();
        if (opt_.input_provider) {
            const HostGamepadState host = opt_.input_provider();
            state.buttons |= host.buttons;
            state.left_trigger = host.left_trigger;
            state.right_trigger = host.right_trigger;
            state.left_x = host.left_x;
            state.left_y = host.left_y;
            state.right_x = host.right_x;
            state.right_y = host.right_y;
        }
        return state;
    }
    std::uint64_t make_gamepad();
    bool opt_gamepad() const { return opt_.virtual_gamepad; }
    // ---- HLE XAudio2 (xbox_audio.hpp) ----
    std::uint64_t audio_engine_{}, audio_callback_{};
    std::uint64_t audio_start_calls_{}, audio_buffers_submitted_{}, audio_bytes_submitted_{}, audio_voices_destroyed_{};
    std::uint64_t make_raw_object(const std::string& cls, const std::vector<Handler>& methods, unsigned total_slots);
    std::uint64_t audio_make_voice(const std::string& cls, bool source, bool master, std::uint32_t mix_id);
    void audio_start_service();
    void audio_pump(GuestCpu& c);
    void frame_services(GuestCpu& c);
    AudioMixer audio_mixer_;
    std::unique_ptr<AudioSink> audio_sink_;
    std::map<std::uint32_t, std::uint64_t> audio_callbacks_;   // mixer voice -> guest IXAudio2VoiceCallback*
    std::vector<float> audio_scratch_;
    std::chrono::steady_clock::time_point audio_last_{};
    double audio_carry_{0.0}; float audio_title_master_{1.0f}; bool audio_started_{false}; std::uint64_t audio_frames_mixed_{0};
    FramePacer pacer_; bool pacer_configured_{false};
    std::uint64_t audio_make_engine();
    void audio_log_once(const std::string& key, const std::string& msg);
    void register_audio_handlers();
    // ---- software-capture Direct3D 11 (xbox_d3d11.hpp) ----
    std::map<std::uint64_t, D3dResource> d3d_res_;
    std::map<std::string, std::uint64_t> d3d_calls_;
    std::uint64_t d3d_device_{}, d3d_context_{}, d3d_draws_{}, d3d_present_count_{};
    std::uint32_t d3d_frames_saved_{};
    std::string d3d_last_frame_;
    // Retained live-capture buffers avoid per-frame vector allocations once the
    // dimensions settle. The callback consumes them synchronously before reuse.
    std::vector<std::uint8_t> d3d_live_bgra_;
    std::vector<std::uint8_t> d3d_live_row_;
    std::uint64_t d3d_make_child(const std::string& cls, const std::string& kind, D3dResource res);
    std::uint64_t d3d_create_device();
    std::map<std::string, std::uint64_t> iid_objects_;   // IID -> object served by QueryInterface on any object
    std::uint64_t d3d_dxgi_device_{}, d3d_swapchain_{}, d3d_backbuffer_{};
    void d3d_create_dxgi();
    void d3d_fill_fast_table(std::uint64_t obj);
    std::map<std::uint64_t, std::uint64_t> d3d_fast_calls_;
    std::set<std::uint64_t> d3d_fast_seen_;
    std::uint64_t d3d_fast_mapped_{};
    std::uint64_t d3d_create_context();
    void d3d_log_once(const std::string& key, const std::string& msg);
    void d3d_capture_frame(const char* why);
    std::uint64_t core_window_{}, core_view_{}, core_dispatcher_{};
    std::uint64_t frames_{}; bool frame_limit_hit_{};
    std::uint64_t d3d_captured_writes_{};
    std::uint64_t make_core_dispatcher();
    void d3d_write_png(const D3dResource& r, const std::filesystem::path& path, unsigned scale);
    std::vector<std::uint8_t> d3d_rgb(const D3dResource& r, unsigned w, unsigned h, bool bgra);
    void d3d_rgb_into(const D3dResource& r, unsigned w, unsigned h, bool bgra,
                      std::vector<std::uint8_t>& out, std::vector<std::uint8_t>& row);
    const D3dResource* d3d_best_texture();
    void d3d_frame_outputs();
    void d3d_save_final_frame();
    void d3d_save_current_png();
    std::uint64_t make_core_window();
    std::uint64_t make_core_application_view();
    void winrt_core_application_run(GuestCpu& c, std::uint64_t source);
    std::uint64_t guest_vcall(GuestCpu& c, std::uint64_t obj, unsigned slot, std::initializer_list<std::uint64_t> args);
    // WinRT event plumbing: add_X(handler, EventRegistrationToken*) / remove_X(token). Handlers are recorded so a later
    // phase can invoke them (e.g. controller added); registrations themselves never fail.
    struct EventSub { std::string source; std::uint64_t handler; std::uint64_t token; };
    std::vector<EventSub> event_subs_;
    std::uint64_t next_event_token_{0x7000};
    Handler event_add_handler(const std::string& source) {
        return [this, source](GuestCpu& c) {
            const std::uint64_t tok = next_event_token_++;
            event_subs_.push_back({source, c.arg(1), tok});
            if (c.arg(1) >= xrt::kHeapBase && c.arg(1) < xrt::kHeapMax) guest_vcall(c, c.arg(1), 1, {});   // the event source keeps the delegate alive (AddRef)
            milestone("WinRT event registered: " + source + " (handler object 0x" + GuestCpu::hex(c.arg(1)) + ", Invoke at " + handler_invoke_location(c.arg(1)) + ")");
            if (c.arg(2)) mem_.write<std::uint64_t>(c.arg(2), tok);
            c.r[0] = 0;
        };
    }
    std::string handler_invoke_location(std::uint64_t obj) {   // where the title's delegate::Invoke (vtable slot 3) lives
        try { const auto vt = mem_.read<std::uint64_t>(obj); return locate(mem_.read<std::uint64_t>(vt + 24)); } catch (...) { return "?"; } }
    static Handler event_remove_handler() { return [](GuestCpu& c) { c.r[0] = 0; }; }
    std::uint64_t make_discovery_object(const std::string& cls, unsigned slots = 48) { return make_com_object(cls, {}, {"*"}, true, slots); }
    std::string guid_text(std::uint64_t p) {
        char b[64];
        std::snprintf(b, sizeof b, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", mem_.read<std::uint32_t>(p), mem_.read<std::uint16_t>(p + 4), mem_.read<std::uint16_t>(p + 6),
            mem_.read<std::uint8_t>(p + 8), mem_.read<std::uint8_t>(p + 9), mem_.read<std::uint8_t>(p + 10), mem_.read<std::uint8_t>(p + 11),
            mem_.read<std::uint8_t>(p + 12), mem_.read<std::uint8_t>(p + 13), mem_.read<std::uint8_t>(p + 14), mem_.read<std::uint8_t>(p + 15));
        return b;
    }
    std::set<std::string> com_log_;
    std::uint64_t ftm_{}, dlc_instance_{};
    std::string last_winrt_class_, last_discovery_; std::uint64_t last_winrt_hr_{};
    std::uint64_t hstring_len(std::uint64_t h) {
        if (!h) return 0;
        const bool is_ref = mem_.read<std::uint32_t>(h) == 1 && h >= xrt::kStackTop - xrt::kStackSize && h < xrt::kStackTop;
        return is_ref ? mem_.read<std::uint32_t>(h + 4) : mem_.read<std::uint32_t>(h);
    }
    std::u16string hstring_text(std::uint64_t h) {
        if (!h) return {};
        const bool is_ref = mem_.read<std::uint32_t>(h) == 1 && h >= xrt::kStackTop - xrt::kStackSize && h < xrt::kStackTop;
        if (is_ref) { std::u16string s; const auto n = mem_.read<std::uint32_t>(h + 4); const auto p = mem_.read<std::uint64_t>(h + 8); for (std::uint32_t i = 0; i < n; ++i) s.push_back(static_cast<char16_t>(mem_.read<std::uint16_t>(p + i * 2))); return s; }
        return mem_.read_wstring(h + 8, mem_.read<std::uint32_t>(h));
    }
    void finish(TitleLaunchReport& rep);
    std::string render(const TitleLaunchReport& rep) const;
};

} // namespace someboxone

#include "xbox_runtime_hle.hpp"
#include "xbox_runtime_hle2.hpp"
#include "xbox_winrt.hpp"
#include "xbox_d3d11.hpp"
#include "xbox_audio.hpp"
#include "lzx.hpp"
#include "xbox_crt_vs2012.hpp"
#include "xbox_threads.hpp"
#include "xbox_kernel_baseline.hpp"

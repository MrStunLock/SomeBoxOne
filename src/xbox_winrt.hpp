#pragma once
// SomeBoxOne v0.23.0 - Windows Runtime (WinRT) class implementations for Xbox One titles.
//
// Each class registers a factory object under its runtime class name. Interface layouts
// (IIDs, method slot order) are recovered from the title's own call sites, not from public
// headers - see docs/TITLE_LAUNCH_v0.23.0.md for how each one was derived.

namespace someboxone {

// Empty WinRT IVectorView<T>: GetAt(6) E_BOUNDS, get_Size(7) 0, IndexOf(8) not found, GetMany(9) 0 items.
inline std::uint64_t XboxTitleRuntime::make_empty_vector_view() {
    ComMethods m;
    m.emplace_back("GetAt", [](GuestCpu& c) { c.r[0] = 0x8000000BULL; });
    m.emplace_back("get_Size", [this](GuestCpu& c) { mem_.write<std::uint32_t>(c.arg(1), 0); c.r[0] = 0; });
    m.emplace_back("IndexOf", [this](GuestCpu& c) { mem_.write<std::uint32_t>(c.arg(2), 0); mem_.write<std::uint8_t>(c.arg(3), 0); c.r[0] = 0; });
    m.emplace_back("GetMany", [this](GuestCpu& c) { mem_.write<std::uint32_t>(c.arg(4), 0); c.r[0] = 0; });
    return make_com_object("EmptyVectorView", m, {"*"}, true, 12);
}

// ----- Windows.ApplicationModel.Core.CoreApplication and the objects its lifecycle hands to the title -----
inline std::uint64_t XboxTitleRuntime::guest_vcall(GuestCpu& c, std::uint64_t obj, unsigned slot, std::initializer_list<std::uint64_t> args) {
    const std::uint64_t fn = mem_.read<std::uint64_t>(mem_.read<std::uint64_t>(obj) + slot * 8ULL);
    std::vector<std::uint64_t> a{obj}; a.insert(a.end(), args.begin(), args.end());
    switch (a.size()) {
    case 1: return call_guest(c, fn, {a[0]});
    case 2: return call_guest(c, fn, {a[0], a[1]});
    case 3: return call_guest(c, fn, {a[0], a[1], a[2]});
    default: return call_guest(c, fn, {a[0], a[1], a[2], a[3]});
    }
}

inline std::uint64_t XboxTitleRuntime::make_core_dispatcher() {
    if (core_dispatcher_) return core_dispatcher_;
    // ICoreDispatcher: 6 get_HasThreadAccess, 7 ProcessEvents(CoreProcessEventsOption), 8 RunAsync, 9 RunIdleAsync, ...
    ComMethods m;
    m.emplace_back("get_HasThreadAccess", [this](GuestCpu& c) { mem_.write<std::uint8_t>(c.arg(1), 1); c.r[0] = 0; });
    m.emplace_back("ProcessEvents", [this](GuestCpu& c) { // called once per iteration of the title's main loop = our frame boundary
        ++frames_;
        if (frames_ == 1) deliver_launch_activation(c);   // the OS raises CoreApplicationView.Activated(Launch) as soon as the app's message loop starts
        if (c.stop != GuestCpu::Stop::None) return;
        if (!threads_.empty()) { qpc_ += 166667; run_threads(thread_slice_); }   // the title's worker threads run once per main-loop iteration
        if (frames_ == 1) milestone("MAIN LOOP REACHED: the title's Run() loop is executing (CoreDispatcher.ProcessEvents option " + std::to_string(c.arg(1) & 0xFFFFFFFFULL) + ")");
        d3d_frame_outputs();
        if (opt_.cancel && opt_.cancel->load()) {
            milestone("run stopped by the user after " + std::to_string(frames_) + " main-loop iterations");
            c.request_stop(GuestCpu::Stop::HostRequest, "stopped by the user after " + std::to_string(frames_) + " main-loop iterations");
            frame_limit_hit_ = true;
        } else if (opt_.max_frames != 0 && frames_ >= opt_.max_frames) {
            std::uint64_t next = 0;
            if (opt_.pause_hook) { // serve mode: publish a screenshot, then wait for the controller's next command
                d3d_save_current_png();
                std::string more;
                next = opt_.pause_hook(frames_, more);
                if (!more.empty()) append_input_script(more);
            }
            if (next > frames_) {
                opt_.max_frames = next;
            } else {
                milestone("frame limit reached (" + std::to_string(frames_) + " main-loop iterations)");
                c.request_stop(GuestCpu::Stop::HostRequest, "frame limit reached after " + std::to_string(frames_) + " main-loop iterations");
                frame_limit_hit_ = true;
            }
        }
        if (c.stop == GuestCpu::Stop::None) frame_services(c);
        c.r[0] = 0; });
    core_dispatcher_ = make_com_object("Windows.UI.Core.CoreDispatcher", m, {"*"}, true, 24);
    return core_dispatcher_;
}

inline std::uint64_t XboxTitleRuntime::make_core_window() {
    if (core_window_) return core_window_;
    // ICoreWindow as laid out by this Xbox build: slot 7 = get_Dispatcher(out) (confirmed at .text+0x249466); slot 19 = first
    // event add (confirmed); the other slots are served by discovery (+ shape-inferred events).
    ComMethods m;
    m.emplace_back("get_Bounds", [this](GuestCpu& c) { // slot 6: `HRESULT get_Bounds(Rect* out)` (x, y, width, height as floats; the caller copies all 16 bytes - Minecraft 1.2)
        const float rect[4] = {0.0f, 0.0f, 1920.0f, 1080.0f}; // Xbox One titles are laid out for a 1920x1080 view
        if (c.arg(1) && mem_.is_mapped(c.arg(1), 16)) for (int k = 0; k < 4; ++k) mem_.write<float>(c.arg(1) + static_cast<std::uint64_t>(k) * 4, rect[k]);
        if (com_log_.insert("offline:CoreWindow:bounds").second) milestone("WinRT Windows.UI.Core.CoreWindow slot 6 get_Bounds -> 0,0,1920,1080");
        c.r[0] = 0; });
    m.emplace_back("get_Dispatcher", [this](GuestCpu& c) {
        const auto d = make_core_dispatcher();
        mem_.write<std::uint64_t>(c.arg(1), d); mem_.write<std::uint64_t>(d + 8, mem_.read<std::uint64_t>(d + 8) + 1); c.r[0] = 0; });
    // slots 8..10 are not yet seen; slot 11 = Activate() (no arguments; Minecraft 1.2 calls it from its Activated handler)
    for (unsigned slot = 8; slot <= 10; ++slot) m.emplace_back("slot" + std::to_string(slot), [this, slot](GuestCpu& c) {
        last_discovery_ = "Windows.UI.Core.CoreWindow method slot " + std::to_string(slot);
        if (com_log_.insert("disc:CoreWindow:" + std::to_string(slot)).second) milestone("WinRT DISCOVERY: Windows.UI.Core.CoreWindow method slot " + std::to_string(slot) + " called but not implemented -> E_NOTIMPL");
        c.r[0] = 0x80004001ULL; });
    m.emplace_back("Activate", [this](GuestCpu& c) { if (com_log_.insert("offline:CoreWindow:activate").second) milestone("WinRT Windows.UI.Core.CoreWindow slot 11 Activate() -> S_OK"); c.r[0] = 0; });
    core_window_ = make_com_object("Windows.UI.Core.CoreWindow", m, {"*"}, true, 64);
    return core_window_;
}

inline std::uint64_t XboxTitleRuntime::make_core_application_view() {
    if (core_view_) return core_view_;
    ComMethods m; // ICoreApplicationView: 6 CoreWindow, 7/8 Activated add/remove, 9 IsMain, 10 IsHosted
    m.emplace_back("get_CoreWindow", [this](GuestCpu& c) { mem_.write<std::uint64_t>(c.arg(1), make_core_window()); mem_.write<std::uint64_t>(core_window_ + 8, mem_.read<std::uint64_t>(core_window_ + 8) + 1); c.r[0] = 0; });
    m.emplace_back("add_Activated", event_add_handler("CoreApplicationView.Activated"));
    m.emplace_back("remove_Activated", event_remove_handler());
    m.emplace_back("get_IsMain", [this](GuestCpu& c) { mem_.write<std::uint8_t>(c.arg(1), 1); c.r[0] = 0; });
    m.emplace_back("get_IsHosted", [this](GuestCpu& c) { mem_.write<std::uint8_t>(c.arg(1), 0); c.r[0] = 0; });
    core_view_ = make_com_object("Windows.ApplicationModel.Core.CoreApplicationView", m, {"*"}, true, 32);
    return core_view_;
}

inline void XboxTitleRuntime::winrt_core_application_run(GuestCpu& c, std::uint64_t source) {
    // IFrameworkViewSource::CreateView (slot 6) -> IFrameworkView; then Initialize(7? no: IFrameworkView slots 6..10).
    const std::uint64_t scratch = static_alloc(64);
    milestone("CoreApplication.Run(IFrameworkViewSource 0x" + GuestCpu::hex(source) + "): creating the title's view");
    mem_.write<std::uint64_t>(scratch, 0);
    std::uint64_t hr = guest_vcall(c, source, 6, {scratch});
    if (c.stop != GuestCpu::Stop::None) return;
    const std::uint64_t view = mem_.read<std::uint64_t>(scratch);
    if ((hr & 0x80000000ULL) || !view) { milestone("IFrameworkViewSource::CreateView failed (hr=0x" + GuestCpu::hex(hr & 0xFFFFFFFFULL) + ")"); return; }
    struct Step { const char* name; unsigned slot; std::uint64_t arg; bool has_arg; };
    const std::uint64_t entry = make_hstring(u"");
    const Step steps[] = {{"Initialize(CoreApplicationView)", 6, make_core_application_view(), true},
                          {"SetWindow(CoreWindow)", 7, make_core_window(), true},
                          {"Load(entryPoint)", 8, entry, true},
                          {"Run()", 9, 0, false},
                          {"Uninitialize()", 10, 0, false}};
    for (const auto& st : steps) {
        milestone(std::string("IFrameworkView::") + st.name);
        hr = st.has_arg ? guest_vcall(c, view, st.slot, {st.arg}) : guest_vcall(c, view, st.slot, {});
        if (c.stop != GuestCpu::Stop::None) return;
        if (hr & 0x80000000ULL) { milestone(std::string("IFrameworkView::") + st.name + " returned failure hr=0x" + GuestCpu::hex(hr & 0xFFFFFFFFULL)); return; }
    }
    milestone("IFrameworkView lifecycle completed: the title's Run() loop returned");
}

// WinRT IVectorView<T> over a fixed list of interface pointers: GetAt(6), get_Size(7), IndexOf(8), GetMany(9).
inline std::uint64_t XboxTitleRuntime::make_vector_view(const std::vector<std::uint64_t>& items) {
    if (items.empty()) return make_empty_vector_view();
    ComMethods m;
    m.emplace_back("GetAt", [this, items](GuestCpu& c) {
        const std::uint32_t i = static_cast<std::uint32_t>(c.arg(1));
        if (i >= items.size()) { c.r[0] = 0x8000000BULL; return; }
        mem_.write<std::uint64_t>(c.arg(2), items[i]); mem_.write<std::uint64_t>(items[i] + 8, mem_.read<std::uint64_t>(items[i] + 8) + 1); c.r[0] = 0; });
    m.emplace_back("get_Size", [this, items](GuestCpu& c) { mem_.write<std::uint32_t>(c.arg(1), static_cast<std::uint32_t>(items.size())); c.r[0] = 0; });
    m.emplace_back("IndexOf", [this, items](GuestCpu& c) {
        std::uint32_t idx = 0; std::uint8_t found = 0;
        for (std::uint32_t i = 0; i < items.size(); ++i) if (items[i] == c.arg(1)) { idx = i; found = 1; }
        mem_.write<std::uint32_t>(c.arg(2), idx); mem_.write<std::uint8_t>(c.arg(3), found); c.r[0] = 0; });
    m.emplace_back("GetMany", [this, items](GuestCpu& c) { // (startIndex, capacity, T* items, uint32* actual): copy [start, start+capacity) and AddRef each element
        const std::uint64_t start = c.arg(1) & 0xFFFFFFFFULL, cap = c.arg(2) & 0xFFFFFFFFULL, out = c.arg(3);
        std::uint32_t n = 0;
        if (out) for (std::uint64_t i = start; i < items.size() && n < cap; ++i, ++n) {
            mem_.write<std::uint64_t>(out + static_cast<std::uint64_t>(n) * 8, items[i]);
            mem_.write<std::uint64_t>(items[i] + 8, mem_.read<std::uint64_t>(items[i] + 8) + 1); }
        if (c.arg(4)) mem_.write<std::uint32_t>(c.arg(4), n);
        c.r[0] = 0; });
    return make_com_object("VectorView", m, {"*"}, true, 12);
}

// One virtual Xbox gamepad (IController/IGamepad). Method layout is learned from the title's calls (discovery mode).
inline std::uint64_t XboxTitleRuntime::make_gamepad() {
    if (gamepad_object_) return gamepad_object_;
    ComMethods m; // IController: slot 6 = get_Id(UINT64*) (call site .text+0x255c64; the title hashes the 64-bit id)
    m.emplace_back("get_Id", [this](GuestCpu& c) { mem_.write<std::uint64_t>(c.arg(1), 0x0123456789ABCDEFULL); c.r[0] = 0; });
    m.emplace_back("slot7", [this](GuestCpu& c) { last_discovery_ = "Gamepad.Instance method slot 7"; milestone("WinRT DISCOVERY: Gamepad.Instance method slot 7 not implemented, called from " + locate(mem_.read<std::uint64_t>(c.r[4])) + " -> E_NOTIMPL"); c.r[0] = 0x80004001ULL; });
    // slot 8 = GetCurrentReading(GamepadReading*): {u64 timestamp; u32 buttons (+pad); float LT, RT, LX, LY, RX, RY} = 40 bytes
    m.emplace_back("GetCurrentReading", [this](GuestCpu& c) {
        const std::uint64_t o = c.arg(1);
        zero_fill(o, 40);
        mem_.write<std::uint64_t>(o, 10000000ULL + frames_ * 166667ULL);   // 100 ns ticks, ~60 Hz
        const HostGamepadState reading = current_gamepad_state();
        mem_.write<std::uint32_t>(o + 8, reading.buttons);
        // GamepadReading (40 bytes): u64 timestamp @0, u32 buttons @8, then LT @0xC, RT @0x10, LX @0x14, LY @0x18, RX @0x1C, RY @0x20.
        // Verified against Sonic Mania's polling code (.text+0x253efc): it reads [buf+0xC] and [buf+0x10] as the triggers and
        // [buf+0x14]/[buf+0x18] and [buf+0x1C]/[buf+0x20] as the two sticks. (Earlier builds wrote everything 4 bytes too late.)
        mem_.write<float>(o + 0xC, reading.left_trigger);
        mem_.write<float>(o + 0x10, reading.right_trigger);
        mem_.write<float>(o + 0x14, reading.left_x);
        mem_.write<float>(o + 0x18, reading.left_y);
        mem_.write<float>(o + 0x1C, reading.right_x);
        mem_.write<float>(o + 0x20, reading.right_y);
        if (reading.buttons && com_log_.insert("pad:" + std::to_string(reading.buttons)).second)
            milestone("virtual gamepad: buttons 0x" + GuestCpu::hex(reading.buttons) + " pressed at main-loop iteration " + std::to_string(frames_));
        c.r[0] = 0; });
    gamepad_object_ = make_com_object("Windows.Xbox.Input.Gamepad.Instance", m, {"*"}, true, 64);
    // Windows.Xbox.Input.IController (IID 9776ff56-9bfd-4581-ad45-b645bba526d6) is a separate interface on the same object:
    // 6 get_Id(UINT64*), 7 get_Type(HSTRING*), 8 get_User(IUser**). Minecraft 1.2 queries it and calls slot 8 expecting a user object
    // (answering with GetCurrentReading wrote 40 bytes over the caller's stack).
    {
        ComMethods cm;
        cm.emplace_back("get_Id", [this](GuestCpu& c) { mem_.write<std::uint64_t>(c.arg(1), 0x0123456789ABCDEFULL); c.r[0] = 0; });
        cm.emplace_back("get_Type", [this](GuestCpu& c) { if (c.arg(1) && mem_.is_mapped(c.arg(1), 8)) mem_.write<std::uint64_t>(c.arg(1), 0); c.r[0] = 0; });   // empty HSTRING
        cm.emplace_back("get_User", [this](GuestCpu& c) {
            const std::uint64_t user = make_xbox_user();
            if (c.arg(1) && mem_.is_mapped(c.arg(1), 8)) { mem_.write<std::uint64_t>(c.arg(1), user); mem_.write<std::uint64_t>(user + 8, mem_.read<std::uint64_t>(user + 8) + 1); }
            c.r[0] = 0; });
        for (unsigned slot = 9; slot <= 14; ++slot) cm.emplace_back("slot" + std::to_string(slot), [this](GuestCpu& c) { if (c.arg(1) && c.arg(1) >= xrt::kStackTop - 0x400000ULL && c.arg(1) < xrt::kStackTop && mem_.is_mapped(c.arg(1), 4)) mem_.write<std::uint32_t>(c.arg(1), 0); c.r[0] = 0; });
        const std::uint64_t controller = make_com_object("Windows.Xbox.Input.IController", cm, {"*"}, true, 16);
        obj_qi_[gamepad_object_]["9776ff56-9bfd-4581-ad45-b645bba526d6"] = controller;
    }
    return gamepad_object_;
}

// The single offline signed-in Xbox user (IUser). Its method layout is unknown, so it is a discovery object: the first call to
// each unimplemented slot is logged with the caller address so the layout can be recovered from the title's own code.
inline std::uint64_t XboxTitleRuntime::make_xbox_user() {
    if (xbox_user_) return xbox_user_;
    ComMethods m; // IUser: slot 6 = get_Id(UINT32*) (call site .text+0x265ae8, which then looks the user up in User.Users by id)
    m.emplace_back("get_Id", [this](GuestCpu& c) { mem_.write<std::uint32_t>(c.arg(1), 1); c.r[0] = 0; });
    xbox_user_ = make_com_object("Windows.Xbox.System.User.Instance", m, {"*"}, true, 64);
    return xbox_user_;
}

// AccountPickerResult returned (via IAsyncOperation) by SystemUI.ShowAccountPickerAsync. Slot 6 is assumed to be get_User.
inline std::uint64_t XboxTitleRuntime::make_account_picker_result() {
    if (picker_result_) return picker_result_;
    ComMethods m;
    m.emplace_back("get_User", [this](GuestCpu& c) {
        milestone("AccountPickerResult.User read -> the offline signed-in user");
        user_signed_in_ = true;
        const auto u = make_xbox_user();
        mem_.write<std::uint64_t>(c.arg(1), u); mem_.write<std::uint64_t>(u + 8, mem_.read<std::uint64_t>(u + 8) + 1); c.r[0] = 0; });
    picker_result_ = make_com_object("Windows.Xbox.UI.AccountPickerResult", m, {"*"}, true, 24);
    return picker_result_;
}

inline void XboxTitleRuntime::register_winrt_classes() {
    // Windows.UI.Core.CoreWindow statics (ICoreWindowStatic, IID 4d239005-3c2a-41b1-9022-536bb9cf93b1; call site .text+0x249f50):
    // slot 6 = GetForCurrentThread(CoreWindow**).
    winrt_classes_["Windows.UI.Core.CoreWindow"] = [this]() {
        ComMethods m;
        m.emplace_back("GetForCurrentThread", [this](GuestCpu& c) {
            const auto w = make_core_window();
            mem_.write<std::uint64_t>(mem_.read<std::uint64_t>(c.r[4]) ? c.arg(1) : c.arg(1), w);
            mem_.write<std::uint64_t>(w + 8, mem_.read<std::uint64_t>(w + 8) + 1); c.r[0] = 0; });
        return make_com_object("Windows.UI.Core.CoreWindowStatics", m, {"*"}, true, 24);
    };
    // Windows.Xbox.Input.Gamepad statics (call site .text+0x255ff0): slot 6 = get_Gamepads(IVectorView<IGamepad>**).
    winrt_classes_["Windows.Xbox.Input.Gamepad"] = [this]() {
        ComMethods m;
        m.emplace_back("get_Gamepads", [this](GuestCpu& c) {
            milestone(opt_gamepad() ? "Gamepad.Gamepads read -> 1 virtual gamepad" : "Gamepad.Gamepads read -> empty list (no gamepad connected)");
            mem_.write<std::uint64_t>(c.arg(1), opt_gamepad() ? make_vector_view({make_gamepad()}) : make_empty_vector_view()); c.r[0] = 0; });
        return make_com_object("Windows.Xbox.Input.Gamepad", m, {"*"}, true, 48);
    };
    // Windows.Xbox.Input.Controller statics (call site .text+0x254df0): slot 6 = get_Controllers(IVectorView<IController>**).
    // No controller is connected yet; add/remove events (slots 7+) are served by the shape-inference in discovery objects.
    winrt_classes_["Windows.Xbox.Input.Controller"] = [this]() {
        ComMethods m;
        m.emplace_back("get_Controllers", [this](GuestCpu& c) {
            milestone(opt_gamepad() ? "Controller.Controllers read -> 1 virtual gamepad" : "Controller.Controllers read -> empty list (no controller connected)");
            mem_.write<std::uint64_t>(c.arg(1), opt_gamepad() ? make_vector_view({make_gamepad()}) : make_empty_vector_view()); c.r[0] = 0; });
        return make_com_object("Windows.Xbox.Input.Controller", m, {"*"}, true, 48);
    };
    // Windows.ApplicationModel.Core.CoreApplication statics (ICoreApplication): 6 Id, 7/8 Suspending, 9/10 Resuming,
    // 11 Properties, 12 GetCurrentView, 13 Run(IFrameworkViewSource) (confirmed: call site .text+0x249ad5 passes the view source).
    winrt_classes_["Windows.ApplicationModel.Core.CoreApplication"] = [this]() {
        ComMethods m;
        m.emplace_back("get_Id", [this](GuestCpu& c) { mem_.write<std::uint64_t>(c.arg(1), make_hstring(u"App")); c.r[0] = 0; });
        m.emplace_back("add_Suspending", event_add_handler("CoreApplication.Suspending")); m.emplace_back("remove_Suspending", event_remove_handler());
        m.emplace_back("add_Resuming", event_add_handler("CoreApplication.Resuming")); m.emplace_back("remove_Resuming", event_remove_handler());
        m.emplace_back("get_Properties", [this](GuestCpu& c) { last_discovery_ = "CoreApplication.get_Properties"; milestone("WinRT DISCOVERY: CoreApplication.Properties not implemented"); c.r[0] = 0x80004001ULL; });
        m.emplace_back("GetCurrentView", [this](GuestCpu& c) { mem_.write<std::uint64_t>(c.arg(1), make_core_application_view()); c.r[0] = 0; });
        m.emplace_back("Run", [this](GuestCpu& c) { winrt_core_application_run(c, c.arg(1)); c.r[0] = 0; });
        return make_com_object("Windows.ApplicationModel.Core.CoreApplication", m, {"*"}, true, 32);
    };

    // Windows.Xbox.Management.Deployment.DownloadableContentPackageManager statics: slot 6 = get_Current(Manager**)
    // (call site .text+0x2516d0). The returned instance is a discovery object until its first use is analysed.
    winrt_classes_["Windows.Xbox.Management.Deployment.DownloadableContentPackageManager"] = [this]() {
        ComMethods m;
        m.emplace_back("get_Current", [this](GuestCpu& c) {
            milestone("DownloadableContentPackageManager.Current read -> manager instance (no DLC installed)");
            if (!dlc_instance_) {
                ComMethods im; // instance: slot 6 = GetPackages(mode, IVectorView**), slot 7 = add_PackageInstallCompleted(handler, token*) (call site .text+0x2505f6), 8 = remove
                im.emplace_back("GetPackages", [this](GuestCpu& c) { // (int mode, IVectorView<Package>** out) - call site .text+0x24ffc7, mode=3
                    milestone("DownloadableContentPackageManager.GetPackages(mode=" + std::to_string(c.arg(1) & 0xFFFFFFFFULL) + ") -> empty list (no DLC installed)");
                    mem_.write<std::uint64_t>(c.arg(2), make_empty_vector_view()); c.r[0] = 0; });
                im.emplace_back("add_PackageInstallCompleted", event_add_handler("DownloadableContentPackageManager.PackageInstallCompleted"));
                im.emplace_back("remove_PackageInstallCompleted", event_remove_handler());
                dlc_instance_ = make_com_object("DownloadableContentPackageManager.Instance", im, {"*"}, true, 48);
            }
            mem_.write<std::uint64_t>(c.arg(1), dlc_instance_); c.r[0] = 0; });
        return make_com_object("DownloadableContentPackageManager", m, {"*"}, true, 32);
    };

    // Offline Xbox achievements are intentionally not synchronized with any account.
    // Minecraft LCE's first startup failure is slot 7 on this activation factory;
    // returning E_NOTIMPL makes C++/CX throw before the title can continue initializing.
    winrt_classes_["Windows.Xbox.Achievements.AchievementNotifier"] = [this]() {
        const std::string cls = "Windows.Xbox.Achievements.AchievementNotifier";
        auto discovery = [this, cls](unsigned slot) { return [this, cls, slot](GuestCpu& c) {
            const std::string key = "disc:" + cls + ":" + std::to_string(slot);
            last_discovery_ = cls + " method slot " + std::to_string(slot);
            if (com_log_.insert(key).second)
                milestone("WinRT DISCOVERY: " + cls + " method slot " + std::to_string(slot) + " called but not implemented -> E_NOTIMPL");
            c.r[0] = 0x80004001ULL;
        }; };
        ComMethods m;
        m.emplace_back("slot6", discovery(6));
        m.emplace_back("slot7_offline", [this, cls](GuestCpu& c) {
            // The observed call passes two adjacent stack slots followed by a guest
            // object. Treat the first slot as an optional async-result output and the
            // second as an optional secondary output/token. This is a conservative
            // offline stub: no achievement events/progress are generated.
            const std::uint64_t first = c.arg(1), second = c.arg(2);
            const std::uint64_t stack_begin = xrt::kStackTop - xrt::kStackSize;
            if (first >= stack_begin && first < xrt::kStackTop && mem_.is_mapped(first, 8)) {
                const auto async = make_async_operation("AchievementNotifier.slot7", false);
                mem_.write<std::uint64_t>(first, async);
                mem_.write<std::uint64_t>(async + 8, mem_.read<std::uint64_t>(async + 8) + 1);
            }
            if (second >= stack_begin && second < xrt::kStackTop && mem_.is_mapped(second, 8))
                mem_.write<std::uint64_t>(second, 0);
            if (com_log_.insert("offline:" + cls + ":7").second)
                milestone("WinRT " + cls + " slot 7 served by offline placeholder (no achievement service/account available)");
            c.r[0] = 0; // S_OK: don't turn unsupported achievements into a fatal C++/CX exception.
        });
        return make_com_object(cls, m, {"*"}, true, 64);
    };

    // Sonic Mania requests SystemUI slot 10 after its main loop is underway. Return a
    // completed async placeholder to avoid throwing E_NOTIMPL for an OS-owned overlay.
    // The actual Xbox system UI is not displayed by SomeBoxOne.
    winrt_classes_["Windows.Xbox.UI.SystemUI"] = [this]() {
        const std::string cls = "Windows.Xbox.UI.SystemUI";
        auto discovery = [this, cls](unsigned slot) { return [this, cls, slot](GuestCpu& c) {
            const std::string key = "disc:" + cls + ":" + std::to_string(slot);
            last_discovery_ = cls + " method slot " + std::to_string(slot);
            if (com_log_.insert(key).second)
                milestone("WinRT DISCOVERY: " + cls + " method slot " + std::to_string(slot) + " called but not implemented -> E_NOTIMPL");
            c.r[0] = 0x80004001ULL;
        }; };
        ComMethods m;
        m.emplace_back("slot6", discovery(6));
        m.emplace_back("slot7", discovery(7));
        m.emplace_back("slot8", discovery(8));
        m.emplace_back("slot9", discovery(9));
        m.emplace_back("slot10_offline_async", [this, cls](GuestCpu& c) {
            const std::uint64_t out = c.arg(3); // observed third explicit arg is a stack output slot
            if (out && mem_.is_mapped(out, 8)) {
                const auto async = make_async_operation("SystemUI.ShowAccountPickerAsync", true, make_account_picker_result());
                mem_.write<std::uint64_t>(out, async);
                mem_.write<std::uint64_t>(async + 8, mem_.read<std::uint64_t>(async + 8) + 1);
            }
            if (com_log_.insert("offline:" + cls + ":10").second)
                milestone("WinRT " + cls + " slot 10 served by completed async placeholder (no system overlay displayed)");
            c.r[0] = 0; // S_OK
        });
        return make_com_object(cls, m, {"*"}, true, 64);
    };

    // Windows.Xbox.System.User statics (IID b4f23a93-7fc6-4a50-b153-950016389e50). Layout recovered from the title's
    // wrappers at .text+0x26a9a0.. and 0x2dc800..: slot 9 = get_Users(IVectorView<User>**), slots 10/12 = add_<event>(handler, token*) (UserAdded/UserRemoved) with removals at 11/13,
    // slots 15/17 = user operations that take inputs + an out IAsyncOperation (not reachable with no signed-in user).
    winrt_classes_["Windows.Xbox.System.User"] = [this]() {
        ComMethods m;
        auto discovery = [this](unsigned slot) { return [this, slot](GuestCpu& c) {
            last_discovery_ = "Windows.Xbox.System.User method slot " + std::to_string(slot);
            milestone("WinRT DISCOVERY: Windows.Xbox.System.User method slot " + std::to_string(slot) + " called but not implemented -> E_NOTIMPL");
            c.r[0] = 0x80004001ULL; }; };
        for (unsigned slot = 6; slot <= 32; ++slot) {
            if (slot == 9) m.emplace_back("get_Users", [this](GuestCpu& c) {
                milestone(user_signed_in_ ? "User.Users read -> 1 signed-in offline user" : "User.Users read -> empty list (nobody signed in yet)");
                mem_.write<std::uint64_t>(c.arg(1), user_signed_in_ ? make_vector_view({make_xbox_user()}) : make_empty_vector_view());
                c.r[0] = 0; });
            else if (slot == 7 || slot == 10 || slot == 12 || slot == 18 || slot == 20 || slot == 22 || slot == 26) m.emplace_back("add_event" + std::to_string(slot), [this, slot](GuestCpu& c) { // (handler, EventRegistrationToken*)
                milestone("User event registration (slot " + std::to_string(slot) + ") accepted; it will never fire (no user sign-in events are generated)");
                mem_.write<std::uint64_t>(c.arg(2), 0x5000 + slot); c.r[0] = 0; });
            else if (slot == 8 || slot == 11 || slot == 13 || slot == 19 || slot == 21 || slot == 23 || slot == 27) m.emplace_back("remove_event" + std::to_string(slot), [](GuestCpu& c) { c.r[0] = 0; });
            else if (slot == 6) m.emplace_back("get_slot6_u32", [this](GuestCpu& c) { // Minecraft 1.2 (older SDK layout): `HRESULT f(uint32_t* out)`; the neighbouring 8 bytes hold a live pointer, so write exactly 4
                if (com_log_.insert("offline:User:6").second) milestone("WinRT Windows.Xbox.System.User slot 6 served by offline placeholder (32-bit result = " + std::string(user_signed_in_ ? "1" : "0") + ")");
                if (c.arg(1) && mem_.is_mapped(c.arg(1), 4)) mem_.write<std::uint32_t>(c.arg(1), user_signed_in_ ? 1U : 0U);
                c.r[0] = 0; });
            else m.emplace_back("slot" + std::to_string(slot), discovery(slot));
        }
        return make_com_object("Windows.Xbox.System.User", m, {"b4f23a93-7fc6-4a50-b153-950016389e50"}, true, 40);
    };

    // Windows.Xbox.Services.XboxLiveConfiguration (statics, IID 00193376-6a84-4c67-84a5-3b8a8aaea712)
    // Sonic Mania calls slots 6, 8 and 16 (vtable offsets 0x30/0x40/0x80); each is a property getter
    // `HRESULT get_X(HSTRING* value)`. The values are placeholders for an offline, signed-out title;
    // slot meaning is inferred (identity strings), which is why they are logged when read.
    winrt_classes_["Windows.Xbox.Services.XboxLiveConfiguration"] = [this]() {
        auto str_getter = [this](const char* what, const std::string& value) {
            const std::string w = what;
            return [this, w, value](GuestCpu& c) {
                milestone("XboxLiveConfiguration." + w + " read -> \"" + value + "\" (offline placeholder)");
                mem_.write<std::uint64_t>(c.arg(1), make_hstring(utf8_to_utf16(value)));
                c.r[0] = 0;
            };
        };
        ComMethods m;
        // Slots 6..16 are all `HRESULT get_X(HSTRING*)` string properties (call sites at .text+0x30bd80/0x30bf10/0x30c030/0x30c0a0
        // use offsets 0x30, 0x38, 0x40, 0x80 identically). Unused ones return an empty string (a null HSTRING).
        for (unsigned slot = 6; slot <= 16; ++slot) {
            std::string value, what = "slot" + std::to_string(slot);
            if (slot == 6) { value = "00000000-0000-0000-0000-000034ba1de0"; what += " (service config id?)"; }
            else if (slot == 7 || slot == 8) { value = "34BA1DE0"; what += " (title id?)"; }
            else if (slot == 16) { value = "RETAIL"; what += " (sandbox id?)"; }
            m.emplace_back("get_slot" + std::to_string(slot), str_getter(what.c_str(), value));
        }
        return make_com_object("XboxLiveConfiguration", m, {"00193376-6a84-4c67-84a5-3b8a8aaea712"}, true, 32);
    };
}

} // namespace someboxone

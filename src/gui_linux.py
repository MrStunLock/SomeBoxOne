#!/usr/bin/env python3
"""Native Linux Tk GUI launcher for SomeBoxOne (stdlib only)."""
from __future__ import annotations
import os, pathlib, queue, subprocess, threading, time, tkinter as tk
from tkinter import ttk, filedialog, messagebox

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
BIN = BUILD / "SomeBoxOne"
CONFIG = pathlib.Path(os.environ.get("XDG_CONFIG_HOME", pathlib.Path.home()/".config")) / "SomeBoxOne" / "SomeBoxOne.ini"
LOG = pathlib.Path(os.environ.get("XDG_STATE_HOME", pathlib.Path.home()/".local/state")) / "SomeBoxOne/logs/SomeBoxOne.log"
DEFAULTS = {
    "MasterVolumePercent":"100", "GameVolumePercent":"100", "KeepLogs":"true",
    "GraphicsBackend":"d3d11", "GraphicsWidth":"1280", "GraphicsHeight":"720",
    "OpenGraphicsWindowOnLaunch":"true", "GraphicsFullscreen":"false",
    "XboxLiveMode":"offline-stub", "XboxLiveSandbox":"RETAIL", "XboxLiveTitleId":"0",
    "XboxLivePrimaryServiceConfigId":""
}

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("SomeBoxOne — Xbox One Emulator")
        self.geometry("1040x720")
        self.minsize(820, 570)
        try:
            self._app_icon = tk.PhotoImage(file=str(ROOT / "assets" / "SomeBoxOne.png"))
            self.iconphoto(True, self._app_icon)
        except (tk.TclError, OSError):
            self._app_icon = None
        self.proc = None
        self.reader_thread = None
        self.lines = queue.Queue()
        self.settings = self.load_settings()
        self.selected_path = tk.StringVar()
        self.status = tk.StringVar(value="Ready")
        self._build_ui()
        self.after(100, self._drain_output)
        self.protocol("WM_DELETE_WINDOW", self._close)

    def load_settings(self):
        values = DEFAULTS.copy()
        try:
            for raw in CONFIG.read_text(encoding="utf-8").splitlines():
                line=raw.strip()
                if not line or line.startswith(("#",";","[")) or "=" not in line: continue
                k,v=line.split("=",1); values[k.strip()]=v.strip()
        except OSError: pass
        return values

    def save_settings(self):
        CONFIG.parent.mkdir(parents=True, exist_ok=True)
        lines=["# SomeBoxOne runtime configuration"]
        for k in DEFAULTS:
            lines.append(f"{k}={self.settings.get(k, DEFAULTS[k])}")
        CONFIG.write_text("\n".join(lines)+"\n", encoding="utf-8")
        self.status.set(f"Settings saved to {CONFIG}")
        self._log("INFO", "SETTINGS_SAVED", str(CONFIG))

    def _log(self, level, event, detail):
        if self.settings.get("KeepLogs","true").lower() not in ("true","1","yes","on"): return
        try:
            LOG.parent.mkdir(parents=True, exist_ok=True)
            with LOG.open("a", encoding="utf-8") as f:
                f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} [{level}] {event}: {detail}\n")
        except OSError: pass

    def _build_ui(self):
        header=ttk.Frame(self, padding=(14,10)); header.pack(fill="x")
        ttk.Label(header,text="SomeBoxOne",font=("TkDefaultFont",18,"bold")).pack(side="left")
        ttk.Label(header,text="Xbox One Emulator",foreground="#666").pack(side="left",padx=14,pady=(5,0))
        self.tabs=ttk.Notebook(self); self.tabs.pack(fill="both",expand=True,padx=12,pady=(0,8))
        self.games=ttk.Frame(self.tabs,padding=12); self.firmware=ttk.Frame(self.tabs,padding=12)
        self.debug=ttk.Frame(self.tabs,padding=12); self.settings_tab=ttk.Frame(self.tabs,padding=12)
        self.tabs.add(self.games,text="Games"); self.tabs.add(self.firmware,text="Firmware")
        self.tabs.add(self.debug,text="Debug / Scan"); self.tabs.add(self.settings_tab,text="Settings")
        self._build_games(); self._build_firmware(); self._build_debug(); self._build_settings()
        foot=ttk.Frame(self,padding=(12,3,12,8)); foot.pack(fill="x")
        ttk.Label(foot,textvariable=self.status).pack(side="left",fill="x",expand=True)
        ttk.Label(foot,text="Linux GUI · Python Tkinter",foreground="#777").pack(side="right")

    def _build_games(self):
        ttk.Label(self.games,text="Launch an unpacked title",font=("TkDefaultFont",13,"bold")).pack(anchor="w")
        ttk.Label(self.games,text="Select a folder containing AppxManifest.xml (or a package folder containing Mount/AppxManifest.xml). Encrypted .xvc packages are not launched.",wraplength=900).pack(anchor="w",pady=(5,10))
        pick=ttk.Frame(self.games); pick.pack(fill="x")
        ttk.Entry(pick,textvariable=self.selected_path).pack(side="left",fill="x",expand=True)
        ttk.Button(pick,text="Browse…",command=self._browse_title).pack(side="left",padx=(8,0))
        actions=ttk.Frame(self.games); actions.pack(fill="x",pady=8)
        self.launch_btn=ttk.Button(actions,text="Launch title",command=self._launch); self.launch_btn.pack(side="left")
        self.stop_btn=ttk.Button(actions,text="Stop run",command=self._stop,state="disabled"); self.stop_btn.pack(side="left",padx=6)
        ttk.Button(actions,text="Save report…",command=self._save_report).pack(side="left",padx=6)
        ttk.Button(actions,text="Clear output",command=self._clear_output).pack(side="left",padx=6)
        ttk.Label(self.games,text="Live game output and final report",font=("TkDefaultFont",11,"bold")).pack(anchor="w",pady=(6,4))
        box=ttk.Frame(self.games); box.pack(fill="both",expand=True)
        self.output=tk.Text(box,wrap="word",height=20,font=("TkFixedFont",9),state="disabled")
        scroll=ttk.Scrollbar(box,orient="vertical",command=self.output.yview); self.output.configure(yscrollcommand=scroll.set)
        self.output.pack(side="left",fill="both",expand=True); scroll.pack(side="right",fill="y")
        self.output.insert("end","Ready. Choose an unpacked title folder and click Launch title.\n")
        self.last_report=""

    def _build_firmware(self):
        ttk.Label(self.firmware,text="Firmware preflight",font=("TkDefaultFont",13,"bold")).pack(anchor="w")
        ttk.Label(self.firmware,text="This build can inspect Xbox system images and folders, but it cannot boot Xbox One firmware.",wraplength=900).pack(anchor="w",pady=8)
        row=ttk.Frame(self.firmware); row.pack(fill="x",pady=6)
        self.firmware_path=tk.StringVar()
        ttk.Entry(row,textvariable=self.firmware_path).pack(side="left",fill="x",expand=True)
        ttk.Button(row,text="Choose image…",command=lambda:self._choose_into(self.firmware_path, files=[("System images","*.xvd *.bin *.img"),("All files","*.*")])).pack(side="left",padx=6)
        ttk.Button(row,text="Inspect image",command=self._inspect_firmware).pack(side="left")
        ttk.Label(self.firmware,text="Result appears in the Games output panel.",foreground="#666").pack(anchor="w",pady=8)

    def _build_debug(self):
        ttk.Label(self.debug,text="Debug / Scan",font=("TkDefaultFont",13,"bold")).pack(anchor="w")
        ttk.Label(self.debug,text="Run the portable runtime diagnostics or scan an OSU/restore folder. These are compatibility diagnostics, not a firmware boot.",wraplength=900).pack(anchor="w",pady=8)
        row=ttk.Frame(self.debug); row.pack(fill="x",pady=4)
        for label,cmd in [("Long-mode test",["long-mode-test"]),("Page-fault test",["page-fault-test"]),("IDT test",["idt-test"]),("CPU → IDT flow",["cpu-idt-flow-test"]),("#UD flow",["cpu-ud-flow-test"]),("#BP flow",["cpu-bp-flow-test"]),("#GP / LIDT flow",["cpu-gp-lidt-flow-test"])]:
            ttk.Button(row,text=label,command=lambda c=cmd:self._run_diagnostic(c)).pack(side="left",padx=(0,5),pady=4)
        row2=ttk.Frame(self.debug); row2.pack(fill="x",pady=8)
        self.scan_path=tk.StringVar()
        ttk.Entry(row2,textvariable=self.scan_path).pack(side="left",fill="x",expand=True)
        ttk.Button(row2,text="Choose folder…",command=lambda:self.scan_path.set(filedialog.askdirectory(title="Choose OSU / restore folder") or self.scan_path.get())).pack(side="left",padx=6)
        ttk.Button(row2,text="Scan folder",command=self._scan).pack(side="left")
        ttk.Button(self.debug,text="Show CLI help",command=lambda:self._run_diagnostic(["--help"])).pack(anchor="w",pady=5)

    def _build_settings(self):
        ttk.Label(self.settings_tab,text="Settings",font=("TkDefaultFont",13,"bold")).pack(anchor="w")
        ttk.Label(self.settings_tab,text="Preferences are shared with the runtime configuration. Audio controls are saved, but the current audio HLE is silent.",wraplength=900).pack(anchor="w",pady=(5,14))
        self.master_vol=tk.IntVar(value=int(self.settings.get("MasterVolumePercent","100")))
        self.game_vol=tk.IntVar(value=int(self.settings.get("GameVolumePercent","100")))
        self.keep_logs=tk.BooleanVar(value=self.settings.get("KeepLogs","true").lower() in ("true","1","yes","on"))
        self.backend=tk.StringVar(value=self.settings.get("GraphicsBackend","d3d11").lower())
        self._volume_row(self.settings_tab,"Master volume",self.master_vol)
        self._volume_row(self.settings_tab,"Game volume",self.game_vol)
        ttk.Checkbutton(self.settings_tab,text="Keep persistent logs",variable=self.keep_logs).pack(anchor="w",pady=8)
        back=ttk.LabelFrame(self.settings_tab,text="Graphics backend",padding=10); back.pack(fill="x",pady=8)
        ttk.Radiobutton(back,text="Direct3D 11 / DXGI (Windows; Linux GUI does not use this presenter)",value="d3d11",variable=self.backend).pack(anchor="w")
        ttk.Radiobutton(back,text="Vulkan (not implemented yet; currently a saved preference only)",value="vulkan",variable=self.backend).pack(anchor="w")
        ttk.Label(self.settings_tab,text="On Linux, this launcher currently shows captured runtime output but does not yet provide a separate live graphics window.",wraplength=900,foreground="#666").pack(anchor="w",pady=8)
        buttons=ttk.Frame(self.settings_tab); buttons.pack(anchor="w",pady=8)
        ttk.Button(buttons,text="Save settings",command=self._save_settings_from_ui).pack(side="left")
        ttk.Button(buttons,text="Open config folder",command=self._open_config_folder).pack(side="left",padx=6)

    def _volume_row(self,parent,label,var):
        row=ttk.Frame(parent); row.pack(fill="x",pady=4)
        ttk.Label(row,text=label,width=18).pack(side="left")
        ttk.Scale(row,from_=0,to=100,variable=var,orient="horizontal").pack(side="left",fill="x",expand=True,padx=8)
        ttk.Label(row,textvariable=var,width=4).pack(side="right")

    def _browse_title(self):
        path=filedialog.askdirectory(title="Choose unpacked Xbox title folder")
        if path: self.selected_path.set(path)

    def _choose_into(self,var,files):
        path=filedialog.askopenfilename(title="Choose Xbox image",filetypes=files)
        if path: var.set(path)

    def _append(self,text):
        self.output.configure(state="normal"); self.output.insert("end",text)
        self.output.see("end"); self.output.configure(state="disabled")

    def _clear_output(self):
        self.output.configure(state="normal"); self.output.delete("1.0","end"); self.output.configure(state="disabled")
        self.last_report=""

    def _save_settings_from_ui(self):
        self.settings["MasterVolumePercent"]=str(max(0,min(100,int(self.master_vol.get()))))
        self.settings["GameVolumePercent"]=str(max(0,min(100,int(self.game_vol.get()))))
        self.settings["KeepLogs"]="true" if self.keep_logs.get() else "false"
        self.settings["GraphicsBackend"]=self.backend.get()
        self.save_settings()

    def _open_config_folder(self):
        CONFIG.parent.mkdir(parents=True,exist_ok=True)
        try: subprocess.Popen(["xdg-open",str(CONFIG.parent)])
        except OSError as e: messagebox.showerror("Open config folder",str(e))

    def _start_command(self,args,game=False):
        if self.proc and self.proc.poll() is None:
            messagebox.showwarning("Already running","Stop the current command before starting another."); return
        if not BIN.exists():
            messagebox.showerror("Runtime missing",f"Could not find {BIN}. Run run_linux_gui.sh to build the project first."); return
        self.tabs.select(self.games)
        self._append("\n$ " + " ".join(map(str,args)) + "\n" + "="*70 + "\n")
        self.status.set("Running: " + str(args[0]))
        if game:
            self.launch_btn.configure(state="disabled"); self.stop_btn.configure(state="normal")
        def worker():
            try:
                self.proc=subprocess.Popen([str(BIN),*args],cwd=str(ROOT),stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,bufsize=1,errors="replace")
                for line in self.proc.stdout:
                    self.lines.put((line,game))
                code=self.proc.wait(); self.lines.put((f"\n[process exited with code {code}]\n",game))
            except Exception as exc: self.lines.put((f"Launcher error: {exc}\n",game))
            finally: self.lines.put((None,game))
        self.reader_thread=threading.Thread(target=worker,daemon=True); self.reader_thread.start()

    def _drain_output(self):
        try:
            while True:
                item=self.lines.get_nowait()
                if item[0] is None:
                    self.proc=None; self.launch_btn.configure(state="normal"); self.stop_btn.configure(state="disabled")
                    self.status.set("Finished")
                    continue
                line,game=item; self._append(line)
                if game: self.last_report += line
                self._log("INFO","LIVE_OUTPUT",line.rstrip())
        except queue.Empty: pass
        self.after(100,self._drain_output)

    def _launch(self):
        path=self.selected_path.get().strip()
        if not path: messagebox.showinfo("Choose a title","Choose an unpacked title folder first."); return
        if not pathlib.Path(path).exists(): messagebox.showerror("Invalid path","That path does not exist."); return
        self._save_settings_from_ui()
        self._start_command(["launch-title",path],game=True)

    def _stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate(); self.status.set("Stop requested")

    def _save_report(self):
        text=self.last_report or self.output.get("1.0","end-1c")
        path=filedialog.asksaveasfilename(title="Save game report",defaultextension=".txt",filetypes=[("Text report","*.txt"),("All files","*.*")])
        if path:
            pathlib.Path(path).write_text(text,encoding="utf-8"); self.status.set("Report saved: "+path)

    def _run_diagnostic(self,cmd): self._start_command(cmd,game=False)
    def _inspect_firmware(self):
        p=self.firmware_path.get().strip()
        if p: self._start_command(["inspect",p])
        else: messagebox.showinfo("Choose image","Choose an image first.")
    def _scan(self):
        p=self.scan_path.get().strip()
        if p: self._start_command(["scan",p])
        else: messagebox.showinfo("Choose folder","Choose a folder first.")

    def _close(self):
        if self.proc and self.proc.poll() is None:
            if not messagebox.askyesno("Quit","A command is still running. Stop it and quit?"): return
            self.proc.terminate()
        self.destroy()

if __name__ == "__main__":
    App().mainloop()

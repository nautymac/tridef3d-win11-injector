"""Window front end for play3d - no console.

One window: the games registered in TriDef 3D Ignition, a launch button, and a
log pane. Everything play3d/injector print() while launching is captured into
the pane (and into launcher.log next to the exe), since a windowed build has
no console for it to go to.
"""
import os
import sys
import queue
import threading
import traceback
import tkinter as tk
from tkinter import ttk

import play3d


class _QueueWriter:
    """stdout/stderr replacement: hands text to the UI thread through a queue."""
    def __init__(self, q, logfile):
        self.q = q
        self.logfile = logfile

    def write(self, s):
        if not s:
            return
        self.q.put(('log', s))
        try:
            self.logfile.write(s)
            self.logfile.flush()
        except Exception:
            pass

    def flush(self):
        pass

    def isatty(self):
        return False


def _resource(*parts):
    base = getattr(sys, '_MEIPASS', os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, *parts)


STRINGS = {
    'ko': {
        'title': 'TriDef 3D Play', 'title_sr': 'TriDef 3D Play SR',
        'header': 'TriDef 3D로 실행할 게임',
        'sub': 'TriDef 3D를 주입해서 입체로 실행합니다',
        'sub_sr': 'TriDef 3D 주입 후 SR 패널용으로 위빙합니다',
        'launch': '3D로 실행', 'refresh': '목록 새로 고침', 'close': '닫기',
        'count': '등록된 게임 {n}개. 맨 위가 최근 실행한 게임입니다.',
        'none': 'TriDef 3D Ignition에 등록된 게임이 없습니다. Ignition에서 게임을 먼저 추가하세요.',
        'pick': '실행할 게임을 목록에서 고르세요.',
        'running': '{g} 실행 중... 게임이 뜨면 TriDef를 주입합니다.',
        'done': '{g}: TriDef 3D 주입 완료. 게임 창으로 전환하세요.',
        'fail': '{g}: 실패. 아래 로그를 확인하세요.',
        'method': '실행 방법...', 'cancel': '취소',
        'waiting': '{g}: 게임이 뜨기를 기다리는 중... {r}초 남음',
        'cancelled': '{g}: 기다리기를 취소했습니다.',
        'm_auto': '자동', 'm_launcher': '런처로 실행', 'm_wait': '기다리기만',
        'sel_method': '실행 방법: {m}',
        'dlg_title': '실행 방법 - {g}',
        'dlg_head': '{exe}을(를) 어떻게 시작할까요?',
        'dlg_auto': '자동 - Steam 게임은 Steam으로, 그 밖의 게임은 실행 파일로 직접 시작',
        'dlg_launcher': '런처로 실행 - 아래 런처를 실행하고 게임이 뜨기를 기다렸다가 주입',
        'dlg_wait': '기다리기만 - 아무것도 실행하지 않음. 런처나 바로 가기로 직접 시작하면 주입',
        'dlg_note': '게임 실행 파일 자체는 이 도구가 실행하지 않습니다. 런처가 실행하면 그 순간 주입합니다.\n'
                    'PlayGTAV.exe나 CSM처럼 런처를 거쳐야만 실행되는 게임에 쓰세요.',
        'dlg_launcher_lbl': '런처 (실행 파일, 바로 가기 또는 steam:// 같은 주소)',
        'dlg_args_lbl': '런처 인수 (선택)',
        'browse': '찾아보기...', 'save': '저장',
        'need_launcher': '런처를 지정하세요.',
        'saved': '{g}: 실행 방법을 저장했습니다 ({m}).',
    },
    'en': {
        'title': 'TriDef 3D Play', 'title_sr': 'TriDef 3D Play SR',
        'header': 'Game to launch with TriDef 3D',
        'sub': 'Injects TriDef 3D so the game renders in stereo',
        'sub_sr': 'Injects TriDef 3D, then weaves the output for an SR panel',
        'launch': 'Launch in 3D', 'refresh': 'Refresh list', 'close': 'Close',
        'count': '{n} game(s) registered. The most recently launched one is at the top.',
        'none': 'No games are registered in TriDef 3D Ignition. Add the game there first.',
        'pick': 'Pick a game from the list.',
        'running': 'Launching {g}... TriDef is injected as soon as the game starts.',
        'done': '{g}: TriDef 3D injected. Switch to the game window.',
        'fail': '{g}: failed. See the log below.',
        'method': 'Launch method...', 'cancel': 'Cancel',
        'waiting': '{g}: waiting for the game to start... {r}s left',
        'cancelled': '{g}: stopped waiting.',
        'm_auto': 'Automatic', 'm_launcher': 'Via launcher', 'm_wait': 'Wait only',
        'sel_method': 'Launch method: {m}',
        'dlg_title': 'Launch method - {g}',
        'dlg_head': 'How should {exe} be started?',
        'dlg_auto': 'Automatic - Steam games through Steam, any other game from its exe',
        'dlg_launcher': 'Via launcher - start the launcher below, wait for the game, then inject',
        'dlg_wait': 'Wait only - start nothing; inject when you start it from its launcher or a shortcut',
        'dlg_note': 'This tool never starts the game exe itself here: it injects the moment the launcher starts it.\n'
                    'Use this for games that only run when their launcher starts them (PlayGTAV.exe, CSM).',
        'dlg_launcher_lbl': 'Launcher (an exe, a shortcut, or an address such as steam://...)',
        'dlg_args_lbl': 'Launcher arguments (optional)',
        'browse': 'Browse...', 'save': 'Save',
        'need_launcher': 'Choose a launcher.',
        'saved': '{g}: launch method saved ({m}).',
    },
}


def detect_lang():
    """Korean when the Windows display language is Korean, English otherwise."""
    try:
        import ctypes
        if (ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3ff) == 0x12:
            return 'ko'
    except Exception:
        pass
    return 'en'


class App:
    def __init__(self, sr, game, dry_run, lang=None, mode=None, via=None, timeout_s=None):
        self.sr = sr
        self.dry_run = dry_run
        # command-line overrides, for the game given on the command line only
        self.cli_game, self.cli_mode, self.cli_via, self.cli_timeout = game, mode, via, timeout_s
        self.t = STRINGS.get(lang or detect_lang(), STRINGS['en'])
        self.busy = False
        self.cancel = None
        self.q = queue.Queue()

        # Per-monitor DPI awareness: without it Windows bitmap-scales the window
        # (blurry, and 2.25x on a 4K panel pushes it off the screen).
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass
        self.root = tk.Tk()
        self.root.title(self.t['title_sr'] if sr else self.t['title'])
        self.scale = max(1.0, self.root.winfo_fpixels('1i') / 96.0)   # 1.0 at 100%, 2.25 at 225%
        w, h = int(560 * self.scale), int(620 * self.scale)
        x = (self.root.winfo_screenwidth() - w) // 2
        y = max(0, (self.root.winfo_screenheight() - h) // 2 - int(40 * self.scale))
        self.root.geometry(f'{w}x{h}+{x}+{y}')
        self.root.minsize(int(460 * self.scale), int(460 * self.scale))
        self._dpi_note = (f"dpi: fpixels/in={self.root.winfo_fpixels('1i'):.0f} scale={self.scale:.2f} "
                          f"screen={self.root.winfo_screenwidth()}x{self.root.winfo_screenheight()} geometry={w}x{h}+{x}+{y}")
        try:
            self.root.iconbitmap(_resource('res', 'Tridef3D_Play_SR.ico' if sr else 'Tridef3D_Play.ico'))
        except Exception:
            pass
        try:
            ttk.Style().theme_use('vista')
        except Exception:
            pass
        self.root.option_add('*Font', ('Segoe UI', 10))

        px = lambda v: int(v * self.scale)
        pad = {'padx': px(16)}
        head = ttk.Frame(self.root)
        head.pack(fill='x', pady=(px(14), px(4)), **pad)
        ttk.Label(head, text=self.t['header'], font=('Segoe UI', 14, 'bold')).pack(anchor='w')
        sub = self.t['sub_sr'] if sr else self.t['sub']
        ttk.Label(head, text=sub, foreground='#555').pack(anchor='w')

        body = ttk.Frame(self.root)
        body.pack(fill='both', expand=True, pady=(px(8), 0), **pad)
        self.listbox = tk.Listbox(body, font=('Segoe UI', 11), activestyle='none', height=8,
                                  selectbackground='#2f6fd6', selectforeground='white',
                                  borderwidth=1, relief='solid', highlightthickness=0)
        sb = ttk.Scrollbar(body, orient='vertical', command=self.listbox.yview)
        self.listbox.configure(yscrollcommand=sb.set)
        self.listbox.pack(side='left', fill='both', expand=True)
        sb.pack(side='right', fill='y')
        self.listbox.bind('<Double-1>', lambda e: self.launch())
        self.listbox.bind('<Return>', lambda e: self.launch())
        self.listbox.bind('<<ListboxSelect>>', lambda e: self._show_method())

        btns = ttk.Frame(self.root)
        btns.pack(fill='x', pady=px(10), **pad)
        self.launch_btn = ttk.Button(btns, text=self.t['launch'], command=self.launch)
        self.launch_btn.pack(side='left')
        self.method_btn = ttk.Button(btns, text=self.t['method'], command=self.edit_method)
        self.method_btn.pack(side='left', padx=(px(8), 0))
        self.refresh_btn = ttk.Button(btns, text=self.t['refresh'], command=self.populate)
        self.refresh_btn.pack(side='left', padx=(px(8), 0))
        ttk.Button(btns, text=self.t['close'], command=self.close).pack(side='right')

        self.method_lbl = ttk.Label(self.root, text='', foreground='#555')
        self.method_lbl.pack(fill='x', **pad)

        self.status = ttk.Label(self.root, text='', foreground='#333')
        self.status.pack(fill='x', **pad)

        logf = ttk.Frame(self.root)
        logf.pack(fill='both', expand=True, pady=(px(6), px(14)), **pad)
        self.log = tk.Text(logf, height=9, font=('Consolas', 9), wrap='word', state='disabled',
                           background='#1e1e1e', foreground='#d8d8d8', insertbackground='white',
                           borderwidth=0, highlightthickness=0, padx=px(8), pady=px(6))
        lsb = ttk.Scrollbar(logf, orient='vertical', command=self.log.yview)
        self.log.configure(yscrollcommand=lsb.set)
        self.log.pack(side='left', fill='both', expand=True)
        lsb.pack(side='right', fill='y')

        # capture everything the launcher prints
        try:
            self.logfile = open(os.path.join(play3d.get_app_dir(), 'launcher.log'), 'w', encoding='utf-8')
        except OSError:
            self.logfile = open(os.devnull, 'w')
        sys.stdout = sys.stderr = _QueueWriter(self.q, self.logfile)

        self.populate()
        self.root.after(100, self._pump)
        self.root.update_idletasks()
        try:   # diagnostics go to launcher.log only, not the pane
            self.logfile.write(self._dpi_note + f" actual={self.root.winfo_width()}x{self.root.winfo_height()}"
                               f"+{self.root.winfo_x()}+{self.root.winfo_y()}\n")
            self.logfile.flush()
        except Exception:
            pass
        if game:
            self._select(game)
            self.root.after(300, self.launch)
        self.root.bind('<Escape>', lambda e: None if self.busy else self.root.destroy())
        self.root.protocol('WM_DELETE_WINDOW', self.close)

    def close(self):
        if self.cancel is not None:
            self.cancel.set()      # a wait in progress stops; nothing is left watching
        self.root.destroy()

    # ---- launch method ----------------------------------------------------
    def _method_text(self, name):
        s = play3d.get_launch_setting(name)
        m = self.t['m_' + s['mode']]
        if s['mode'] == 'launcher':
            m += f" ({os.path.basename(s['launcher'].rstrip('/')) or s['launcher']})"
        return self.t['sel_method'].format(m=m)

    def _show_method(self):
        name = self.selected()
        self.method_lbl.configure(text=self._method_text(name) if name else '')

    def edit_method(self):
        name = self.selected()
        if self.busy or not name:
            if not name:
                self.status.configure(text=self.t['pick'])
            return
        MethodDialog(self, name)

    # ---- list -------------------------------------------------------------
    def populate(self):
        games = play3d.list_registered_games()
        default = play3d.get_ignition_last_game_name() or play3d.load_last_used()
        if default in games:
            games.remove(default)
            games.insert(0, default)
        self.listbox.delete(0, 'end')
        for g in games:
            self.listbox.insert('end', g)
        if games:
            self.listbox.selection_set(0)
            self.listbox.activate(0)
            self.listbox.focus_set()
            self.status.configure(text=self.t['count'].format(n=len(games)))
        else:
            self.status.configure(text=self.t['none'])
        self._show_method()

    def _select(self, name):
        items = self.listbox.get(0, 'end')
        if name not in items:
            self.listbox.insert('end', name)
            items = self.listbox.get(0, 'end')
        idx = items.index(name)
        self.listbox.selection_clear(0, 'end')
        self.listbox.selection_set(idx)
        self.listbox.activate(idx)
        self.listbox.see(idx)
        self._show_method()

    def selected(self):
        sel = self.listbox.curselection()
        return self.listbox.get(sel[0]) if sel else None

    # ---- launch -------------------------------------------------------------
    def launch(self):
        if self.busy:
            if self.cancel is not None:      # the button is "Cancel" while waiting
                self.cancel.set()
            return
        name = self.selected()
        if not name:
            self.status.configure(text=self.t['pick'])
            return
        self.busy = True
        self.cancel = threading.Event()
        self.launch_btn.configure(text=self.t['cancel'])
        self.method_btn.configure(state='disabled')
        self.refresh_btn.configure(state='disabled')
        self.listbox.configure(state='disabled')
        self._clear_log()
        self.status.configure(text=self.t['running'].format(g=name))
        threading.Thread(target=self._run, args=(name, self.cancel), daemon=True).start()

    def _run(self, name, cancel):
        ok = False
        kw = {}
        if name == self.cli_game:
            kw = {'mode': self.cli_mode, 'via': self.cli_via, 'timeout_s': self.cli_timeout}
        try:
            ok = play3d.play3d(name, dry_run=self.dry_run, sr=self.sr, cancel=cancel,
                               progress=lambda e, t: self.q.put(('progress', (name, t - e))), **kw)
            if ok and not self.dry_run:
                play3d.save_last_used(name)
        except Exception:
            print('\n' + traceback.format_exc())
        self.q.put(('done', (name, ok, cancel.is_set())))

    def _pump(self):
        try:
            while True:
                kind, payload = self.q.get_nowait()
                if kind == 'log':
                    self._append(payload)
                elif kind == 'progress':
                    name, left = payload
                    if self.busy:
                        self.status.configure(text=self.t['waiting'].format(g=name, r=max(0, int(left))))
                elif kind == 'done':
                    name, ok, cancelled = payload
                    self.busy = False
                    self.cancel = None
                    self.launch_btn.configure(text=self.t['launch'])
                    self.method_btn.configure(state='normal')
                    self.refresh_btn.configure(state='normal')
                    self.listbox.configure(state='normal')
                    if ok:
                        self.status.configure(text=self.t['done'].format(g=name))
                    elif cancelled:
                        self.status.configure(text=self.t['cancelled'].format(g=name))
                    else:
                        self.status.configure(text=self.t['fail'].format(g=name))
        except queue.Empty:
            pass
        self.root.after(100, self._pump)

    # ---- log --------------------------------------------------------------
    def _append(self, s):
        self.log.configure(state='normal')
        self.log.insert('end', s)
        self.log.see('end')
        self.log.configure(state='disabled')

    def _clear_log(self):
        self.log.configure(state='normal')
        self.log.delete('1.0', 'end')
        self.log.configure(state='disabled')

    def run(self):
        self.root.mainloop()


class MethodDialog:
    """Per-game launch method: automatic / via a launcher / wait only. Saved in
    the game's Ignition key (play3d.set_launch_setting), so Geo Gaming Hub and
    the --console mode see the same setting."""
    def __init__(self, app, name):
        self.app, self.name, t = app, name, app.t
        px = lambda v: int(v * app.scale)
        s = play3d.get_launch_setting(name)
        exe = play3d.get_registered_exe(name)
        appid = None
        try:
            appid = play3d.get_tridef_game_appid(name)
        except Exception:
            pass
        if not exe and appid:
            try:
                exe = play3d.find_steam_game_exe(name, appid)
            except Exception:
                exe = None
        suggestion = s['launcher'] or play3d.suggest_launcher(exe, appid)

        self.win = w = tk.Toplevel(app.root)
        w.title(t['dlg_title'].format(g=name))
        w.transient(app.root)
        w.resizable(True, False)
        f = ttk.Frame(w, padding=px(16))
        f.pack(fill='both', expand=True)
        ttk.Label(f, text=t['dlg_head'].format(exe=os.path.basename(exe) if exe else name),
                  font=('Segoe UI', 11, 'bold')).pack(anchor='w')
        self.mode = tk.StringVar(value=s['mode'])
        for m in play3d.MODES:
            ttk.Radiobutton(f, text=t['dlg_' + m], value=m, variable=self.mode,
                            command=self._sync).pack(anchor='w', pady=(px(6), 0))
        ttk.Label(f, text=t['dlg_note'], foreground='#555', justify='left',
                  wraplength=px(520)).pack(anchor='w', pady=(px(10), px(8)))

        ttk.Label(f, text=t['dlg_launcher_lbl']).pack(anchor='w')
        row = ttk.Frame(f)
        row.pack(fill='x')
        self.launcher = tk.StringVar(value=suggestion)
        self.launcher_entry = ttk.Entry(row, textvariable=self.launcher, width=60)
        self.launcher_entry.pack(side='left', fill='x', expand=True)
        self.browse_btn = ttk.Button(row, text=t['browse'], command=lambda: self._browse(exe))
        self.browse_btn.pack(side='left', padx=(px(6), 0))
        ttk.Label(f, text=t['dlg_args_lbl']).pack(anchor='w', pady=(px(6), 0))
        self.args = tk.StringVar(value=s['args'])
        self.args_entry = ttk.Entry(f, textvariable=self.args)
        self.args_entry.pack(fill='x')

        self.err = ttk.Label(f, text='', foreground='#b00020')
        self.err.pack(anchor='w', pady=(px(6), 0))
        btns = ttk.Frame(f)
        btns.pack(fill='x', pady=(px(8), 0))
        ttk.Button(btns, text=t['close'], command=w.destroy).pack(side='right')
        ttk.Button(btns, text=t['save'], command=self._save).pack(side='right', padx=(0, px(6)))
        self._sync()
        w.bind('<Escape>', lambda e: w.destroy())
        w.grab_set()

    def _sync(self):
        state = 'normal' if self.mode.get() == 'launcher' else 'disabled'
        for wdg in (self.launcher_entry, self.browse_btn, self.args_entry):
            wdg.configure(state=state)

    def _browse(self, exe):
        from tkinter import filedialog
        cur = self.launcher.get().strip()
        start = os.path.dirname(cur) if cur and os.path.isdir(os.path.dirname(cur)) else (os.path.dirname(exe) if exe else None)
        p = filedialog.askopenfilename(parent=self.win, initialdir=start,
                                       filetypes=[('Programs / shortcuts', '*.exe *.lnk *.url *.bat *.cmd'),
                                                  ('All files', '*.*')])
        if p:
            self.launcher.set(os.path.normpath(p))

    def _save(self):
        mode = self.mode.get()
        launcher = self.launcher.get().strip().strip('"')
        if mode == 'launcher' and not launcher:
            self.err.configure(text=self.app.t['need_launcher'])
            return
        try:
            play3d.set_launch_setting(self.name, mode, launcher if mode == 'launcher' else '',
                                      self.args.get().strip() if mode == 'launcher' else '')
        except OSError as e:
            self.err.configure(text=str(e))
            return
        self.app.status.configure(text=self.app.t['saved'].format(g=self.name, m=self.app.t['m_' + mode]))
        self.app._show_method()
        self.win.destroy()


def run(sr=False, game=None, dry_run=False, lang=None, mode=None, via=None, timeout_s=None):
    App(sr, game, dry_run, lang, mode, via, timeout_s).run()

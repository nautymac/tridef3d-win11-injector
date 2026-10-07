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
        'none': 'TriDef 3D Ignition에 등록된 Steam 게임이 없습니다. Ignition에서 게임을 먼저 추가하세요.',
        'pick': '실행할 게임을 목록에서 고르세요.',
        'running': '{g} 실행 중... Steam이 게임을 띄우면 TriDef를 주입합니다.',
        'done': '{g}: TriDef 3D 주입 완료. 게임 창으로 전환하세요.',
        'fail': '{g}: 실패. 아래 로그를 확인하세요.',
    },
    'en': {
        'title': 'TriDef 3D Play', 'title_sr': 'TriDef 3D Play SR',
        'header': 'Game to launch with TriDef 3D',
        'sub': 'Injects TriDef 3D so the game renders in stereo',
        'sub_sr': 'Injects TriDef 3D, then weaves the output for an SR panel',
        'launch': 'Launch in 3D', 'refresh': 'Refresh list', 'close': 'Close',
        'count': '{n} game(s) registered. The most recently launched one is at the top.',
        'none': 'No Steam games are registered in TriDef 3D Ignition. Add the game there first.',
        'pick': 'Pick a game from the list.',
        'running': 'Launching {g}... TriDef is injected as soon as Steam starts the game.',
        'done': '{g}: TriDef 3D injected. Switch to the game window.',
        'fail': '{g}: failed. See the log below.',
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
    def __init__(self, sr, game, dry_run, lang=None):
        self.sr = sr
        self.dry_run = dry_run
        self.t = STRINGS.get(lang or detect_lang(), STRINGS['en'])
        self.busy = False
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

        btns = ttk.Frame(self.root)
        btns.pack(fill='x', pady=px(10), **pad)
        self.launch_btn = ttk.Button(btns, text=self.t['launch'], command=self.launch)
        self.launch_btn.pack(side='left')
        self.refresh_btn = ttk.Button(btns, text=self.t['refresh'], command=self.populate)
        self.refresh_btn.pack(side='left', padx=(px(8), 0))
        ttk.Button(btns, text=self.t['close'], command=self.root.destroy).pack(side='right')

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

    def selected(self):
        sel = self.listbox.curselection()
        return self.listbox.get(sel[0]) if sel else None

    # ---- launch -------------------------------------------------------------
    def launch(self):
        if self.busy:
            return
        name = self.selected()
        if not name:
            self.status.configure(text=self.t['pick'])
            return
        self.busy = True
        self.launch_btn.configure(state='disabled')
        self.refresh_btn.configure(state='disabled')
        self.listbox.configure(state='disabled')
        self._clear_log()
        self.status.configure(text=self.t['running'].format(g=name))
        threading.Thread(target=self._run, args=(name,), daemon=True).start()

    def _run(self, name):
        ok = False
        try:
            ok = play3d.play3d(name, dry_run=self.dry_run, sr=self.sr)
            if ok and not self.dry_run:
                play3d.save_last_used(name)
        except Exception:
            print('\n' + traceback.format_exc())
        self.q.put(('done', (name, ok)))

    def _pump(self):
        try:
            while True:
                kind, payload = self.q.get_nowait()
                if kind == 'log':
                    self._append(payload)
                elif kind == 'done':
                    name, ok = payload
                    self.busy = False
                    self.launch_btn.configure(state='normal')
                    self.refresh_btn.configure(state='normal')
                    self.listbox.configure(state='normal')
                    if ok:
                        self.status.configure(text=self.t['done'].format(g=name))
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


def run(sr=False, game=None, dry_run=False, lang=None):
    App(sr, game, dry_run, lang).run()

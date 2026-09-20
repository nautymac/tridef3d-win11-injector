"""
Custom TriDef DLL injector - replaces TriDefInjector64.exe's fragile manual-PE-mapping
technique with the standard, battle-tested LoadLibraryW + CreateRemoteThread method.

Usage:
    python injector.py <PID> <dll_path> [<dll_path2> ...]

Why this exists:
    TriDefInjector64.exe manually constructs a fake PE image in the target process's
    memory and searches for free space by walking *upward only* from the target
    module's base address. Modern games load far more DLLs (~100) than games did
    circa 2016 when this tool was built, so that linear search can run out of room
    and silently fail (exit code 4). This injector sidesteps all of that by using
    the same mechanism the OS loader itself uses.
"""
import ctypes
import ctypes.wintypes as wt
import sys
import os

kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)

# ---- Proper prototypes (critical on x64: default ctypes assumes 32-bit int
# return values, which truncates pointers/handles and silently corrupts results) ----

kernel32.OpenProcess.restype = wt.HANDLE
kernel32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]

kernel32.VirtualAllocEx.restype = wt.LPVOID
kernel32.VirtualAllocEx.argtypes = [wt.HANDLE, wt.LPVOID, ctypes.c_size_t, wt.DWORD, wt.DWORD]

kernel32.WriteProcessMemory.restype = wt.BOOL
kernel32.WriteProcessMemory.argtypes = [wt.HANDLE, wt.LPVOID, wt.LPCVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]

kernel32.GetModuleHandleW.restype = wt.HMODULE
kernel32.GetModuleHandleW.argtypes = [wt.LPCWSTR]

kernel32.GetProcAddress.restype = wt.LPVOID
kernel32.GetProcAddress.argtypes = [wt.HMODULE, wt.LPCSTR]

kernel32.CreateRemoteThread.restype = wt.HANDLE
kernel32.CreateRemoteThread.argtypes = [wt.HANDLE, wt.LPVOID, ctypes.c_size_t, wt.LPVOID, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD)]

kernel32.WaitForSingleObject.restype = wt.DWORD
kernel32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]

kernel32.GetExitCodeThread.restype = wt.BOOL
kernel32.GetExitCodeThread.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]

kernel32.VirtualFreeEx.restype = wt.BOOL
kernel32.VirtualFreeEx.argtypes = [wt.HANDLE, wt.LPVOID, ctypes.c_size_t, wt.DWORD]

kernel32.CloseHandle.restype = wt.BOOL
kernel32.CloseHandle.argtypes = [wt.HANDLE]

kernel32.ResumeThread.restype = wt.DWORD
kernel32.ResumeThread.argtypes = [wt.HANDLE]


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [
        ("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR),
        ("lpTitle", wt.LPWSTR), ("dwX", wt.DWORD), ("dwY", wt.DWORD),
        ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD), ("dwXCountChars", wt.DWORD),
        ("dwYCountChars", wt.DWORD), ("dwFillAttribute", wt.DWORD), ("dwFlags", wt.DWORD),
        ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD), ("lpReserved2", ctypes.c_void_p),
        ("hStdInput", wt.HANDLE), ("hStdOutput", wt.HANDLE), ("hStdError", wt.HANDLE),
    ]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE),
                ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]


kernel32.CreateProcessW.restype = wt.BOOL
kernel32.CreateProcessW.argtypes = [
    wt.LPCWSTR, wt.LPWSTR, wt.LPVOID, wt.LPVOID, wt.BOOL, wt.DWORD,
    wt.LPVOID, wt.LPCWSTR, ctypes.POINTER(STARTUPINFOW), ctypes.POINTER(PROCESS_INFORMATION)
]

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_OPERATION = 0x0008
PROCESS_VM_WRITE = 0x0020
PROCESS_VM_READ = 0x0010
PROCESS_CREATE_THREAD = 0x0002
MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_RELEASE = 0x8000
PAGE_READWRITE = 0x04
WAIT_TIMEOUT = 0x102
CREATE_SUSPENDED = 0x00000004


def inject_one(hProcess, dll_path):
    print(f"  -> injecting: {dll_path}")
    path_bytes = (dll_path + "\x00").encode('utf-16-le')
    size = len(path_bytes)

    remote_mem = kernel32.VirtualAllocEx(hProcess, None, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
    if not remote_mem:
        print(f"     VirtualAllocEx failed, err={ctypes.get_last_error()}")
        return False

    written = ctypes.c_size_t(0)
    ok = kernel32.WriteProcessMemory(hProcess, remote_mem, path_bytes, size, ctypes.byref(written))
    if not ok or written.value != size:
        print(f"     WriteProcessMemory failed, err={ctypes.get_last_error()}")
        kernel32.VirtualFreeEx(hProcess, remote_mem, 0, MEM_RELEASE)
        return False

    hK32 = kernel32.GetModuleHandleW("kernel32.dll")
    loadlib_addr = kernel32.GetProcAddress(hK32, b"LoadLibraryW")
    if not loadlib_addr:
        print("     GetProcAddress(LoadLibraryW) failed")
        return False

    thread_id = wt.DWORD(0)
    hThread = kernel32.CreateRemoteThread(hProcess, None, 0, loadlib_addr, remote_mem, 0, ctypes.byref(thread_id))
    if not hThread:
        print(f"     CreateRemoteThread failed, err={ctypes.get_last_error()}")
        kernel32.VirtualFreeEx(hProcess, remote_mem, 0, MEM_RELEASE)
        return False

    result = kernel32.WaitForSingleObject(hThread, 15000)
    if result == WAIT_TIMEOUT:
        print("     remote thread timed out after 15s")
        kernel32.CloseHandle(hThread)
        return False

    exit_code = wt.DWORD(0)
    kernel32.GetExitCodeThread(hThread, ctypes.byref(exit_code))
    kernel32.CloseHandle(hThread)
    kernel32.VirtualFreeEx(hProcess, remote_mem, 0, MEM_RELEASE)

    if exit_code.value == 0:
        print(f"     LoadLibraryW returned NULL -> load FAILED (check DllMain / dependencies)")
        return False
    print(f"     loaded OK, remote module handle = 0x{exit_code.value:x}")
    return True


def launch_suspended_and_inject(exe_path, dll_paths):
    """Launch exe_path CREATE_SUSPENDED, inject dll_paths before any of the
    game's own code (including D3D device/resource creation) can run, then
    resume the main thread. Returns the PID, or None on failure."""
    si = STARTUPINFOW()
    si.cb = ctypes.sizeof(STARTUPINFOW)
    pi = PROCESS_INFORMATION()

    import os
    workdir = os.path.dirname(exe_path)
    cmdline = ctypes.create_unicode_buffer(f'"{exe_path}"')

    ok = kernel32.CreateProcessW(
        exe_path, cmdline, None, None, False, CREATE_SUSPENDED,
        None, workdir, ctypes.byref(si), ctypes.byref(pi)
    )
    if not ok:
        print(f"CreateProcessW failed, err={ctypes.get_last_error()}")
        return None

    print(f"launched suspended: PID={pi.dwProcessId}")

    all_ok = True
    for dll in dll_paths:
        ok = inject_one(pi.hProcess, dll)
        all_ok = all_ok and ok

    kernel32.ResumeThread(pi.hThread)
    print(f"resumed main thread, PID={pi.dwProcessId} now running")

    kernel32.CloseHandle(pi.hThread)
    kernel32.CloseHandle(pi.hProcess)
    return pi.dwProcessId if all_ok else None


DEBUG_PROCESS = 0x00000001
CREATE_PROCESS_DEBUG_EVENT = 3
EXIT_PROCESS_DEBUG_EVENT = 5
DBG_CONTINUE = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001
INFINITE = 0xFFFFFFFF

kernel32.DebugSetProcessKillOnExit.restype = wt.BOOL
kernel32.DebugSetProcessKillOnExit.argtypes = [wt.BOOL]
kernel32.DebugActiveProcessStop.restype = wt.BOOL
kernel32.DebugActiveProcessStop.argtypes = [wt.DWORD]
kernel32.ContinueDebugEvent.restype = wt.BOOL
kernel32.ContinueDebugEvent.argtypes = [wt.DWORD, wt.DWORD, wt.DWORD]

# DEBUG_EVENT: 3 DWORDs (code, pid, tid) + a union big enough for the
# largest variant (CREATE_PROCESS_DEBUG_INFO etc). We only decode the
# fields we need (hProcess / dwExitCode) by raw byte offset instead of
# fully typing every union member.
DEBUG_EVENT_SIZE = 12 + 160


class DEBUG_EVENT_RAW(ctypes.Structure):
    _fields_ = [
        ("dwDebugEventCode", wt.DWORD),
        ("dwProcessId", wt.DWORD),
        ("dwThreadId", wt.DWORD),
        ("_pad", wt.DWORD),  # compiler-inserted padding: the union's HANDLE
                             # members need 8-byte alignment on x64, so it
                             # starts at offset 16, not 12.
        ("union_bytes", ctypes.c_ubyte * 160),
    ]


kernel32.WaitForDebugEvent.restype = wt.BOOL
kernel32.WaitForDebugEvent.argtypes = [ctypes.POINTER(DEBUG_EVENT_RAW), wt.DWORD]

kernel32.DebugActiveProcess.restype = wt.BOOL
kernel32.DebugActiveProcess.argtypes = [wt.DWORD]

kernel32.QueryFullProcessImageNameW.restype = wt.BOOL
kernel32.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD)]


def get_process_image_path(hProcess):
    buf = ctypes.create_unicode_buffer(1024)
    size = wt.DWORD(1024)
    if kernel32.QueryFullProcessImageNameW(hProcess, 0, buf, ctypes.byref(size)):
        return buf.value
    return None


def find_pid_by_name(image_name):
    import subprocess
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {image_name}", "/FO", "CSV", "/NH"],
                          capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.strip('"').split('","')
        if len(parts) >= 2 and parts[0].lower() == image_name.lower():
            return int(parts[1])
    return None


def debug_attach_steam_and_launch(steam_exe_path, launch_uri, dll_paths,
                                   path_filter="steamapps\\common", idle_timeout_ms=20000):
    """Attach as debugger to the already-running steam.exe, then trigger the
    real game launch via its steam:// URI. Steam itself spawns the actual
    game as ITS OWN child process (not a child of whatever briefly runs the
    URI), so a plain CreateProcess(DEBUG_PROCESS) on a freshly-launched
    process tree never sees it - only debug-attaching to the long-running
    Steam client itself catches that spawn. We filter matching children by
    install path (steamapps\\common) so we don't inject into Steam's own
    helper processes (steamwebhelper, service, overlay, etc)."""
    import subprocess

    steam_pid = find_pid_by_name("steam.exe")
    if not steam_pid:
        print("steam.exe not found running")
        return None
    print(f"found running steam.exe pid={steam_pid}")

    if not kernel32.DebugActiveProcess(steam_pid):
        print(f"DebugActiveProcess({steam_pid}) failed, err={ctypes.get_last_error()}")
        return None
    kernel32.DebugSetProcessKillOnExit(False)
    print(f"attached as debugger to steam.exe (pid={steam_pid})")

    subprocess.Popen(["cmd", "/c", "start", "", launch_uri], shell=False)
    print(f"triggered launch: {launch_uri}")

    injected_pids = []
    ev = DEBUG_EVENT_RAW()

    while True:
        if not kernel32.WaitForDebugEvent(ctypes.byref(ev), idle_timeout_ms):
            print(f"no more relevant debug events after {idle_timeout_ms}ms, detaching")
            break

        code = ev.dwDebugEventCode
        pid = ev.dwProcessId
        tid = ev.dwThreadId

        if code == CREATE_PROCESS_DEBUG_EVENT:
            hProcess = int.from_bytes(bytes(ev.union_bytes[8:16]), 'little')
            path = get_process_image_path(hProcess)
            print(f"CREATE_PROCESS_DEBUG_EVENT: pid={pid} path={path}")
            if path and path_filter.lower() in path.lower():
                print(f"  -> matches filter, injecting")
                for dll in dll_paths:
                    inject_one_async(hProcess, dll)
                injected_pids.append(pid)
            else:
                print(f"  -> does not match filter, skipping (Steam internal process)")
        elif code == EXIT_PROCESS_DEBUG_EVENT:
            print(f"EXIT_PROCESS_DEBUG_EVENT: pid={pid}")
            if pid in injected_pids:
                injected_pids.remove(pid)
                print(f"  (that was one of our injected targets - it exited, still watching)")

        kernel32.ContinueDebugEvent(pid, tid, DBG_CONTINUE)

        if injected_pids and code == CREATE_PROCESS_DEBUG_EVENT:
            # give the newly injected process a moment to either settle or
            # relaunch-and-exit again before we consider detaching early
            pass

    kernel32.DebugActiveProcessStop(steam_pid)
    print(f"detached from steam.exe. successfully-injected pids still alive: {injected_pids}")
    return injected_pids[-1] if injected_pids else None


def debug_launch_and_inject(exe_path, dll_paths, max_processes=12, idle_timeout_ms=15000):
    """Launch exe_path as a debuggee (DEBUG_PROCESS): this pauses every
    process in the resulting tree (including any self-relaunched child,
    e.g. via SteamAPI_RestartAppIfNecessary or a Unity bootstrap stub)
    right at creation, before a single instruction of its own code -
    including D3D device/resource creation - has run. We inject into
    every process we see created, then continue it. After the tree goes
    idle we detach so the game keeps running without us attached."""
    si = STARTUPINFOW()
    si.cb = ctypes.sizeof(STARTUPINFOW)
    pi = PROCESS_INFORMATION()

    import os
    workdir = os.path.dirname(exe_path)
    cmdline = ctypes.create_unicode_buffer(f'"{exe_path}"')

    ok = kernel32.CreateProcessW(
        exe_path, cmdline, None, None, False, DEBUG_PROCESS,
        None, workdir, ctypes.byref(si), ctypes.byref(pi)
    )
    if not ok:
        print(f"CreateProcessW failed, err={ctypes.get_last_error()}")
        return None

    root_pid = pi.dwProcessId
    kernel32.CloseHandle(pi.hThread)
    kernel32.CloseHandle(pi.hProcess)
    print(f"launched as debuggee: root PID={root_pid}")

    kernel32.DebugSetProcessKillOnExit(False)

    injected_pids = []
    ev = DEBUG_EVENT_RAW()
    last_pid_seen = None

    while True:
        if not kernel32.WaitForDebugEvent(ctypes.byref(ev), idle_timeout_ms):
            print(f"no more debug events after {idle_timeout_ms}ms, detaching")
            break

        code = ev.dwDebugEventCode
        pid = ev.dwProcessId
        tid = ev.dwThreadId

        if code == CREATE_PROCESS_DEBUG_EVENT:
            hProcess = int.from_bytes(bytes(ev.union_bytes[8:16]), 'little')  # CREATE_PROCESS_DEBUG_INFO.hProcess
            print(f"CREATE_PROCESS_DEBUG_EVENT: pid={pid} hProcess=0x{hProcess:x}")
            for dll in dll_paths:
                inject_one_async(hProcess, dll)
            injected_pids.append(pid)
            last_pid_seen = pid
            if len(injected_pids) >= max_processes:
                kernel32.ContinueDebugEvent(pid, tid, DBG_CONTINUE)
                print(f"reached max_processes={max_processes}, will detach after this continue")
                break
        elif code == EXIT_PROCESS_DEBUG_EVENT:
            print(f"EXIT_PROCESS_DEBUG_EVENT: pid={pid}")

        kernel32.ContinueDebugEvent(pid, tid, DBG_CONTINUE)

    # Detach from everything we're still attached to so the game runs free.
    for p in injected_pids:
        kernel32.DebugActiveProcessStop(p)
    kernel32.DebugActiveProcessStop(root_pid)
    print(f"detached. injected into pids: {injected_pids}")
    return injected_pids[-1] if injected_pids else None


def inject_one_async(hProcess, dll_path):
    """Like inject_one, but does not wait for the remote thread to finish.
    Required while the target is paused on a debug event: the whole process
    (including any thread we just created) stays frozen until we call
    ContinueDebugEvent, so blocking here would always time out."""
    print(f"  -> injecting (async): {dll_path}")
    path_bytes = (dll_path + "\x00").encode('utf-16-le')
    size = len(path_bytes)

    remote_mem = kernel32.VirtualAllocEx(hProcess, None, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
    if not remote_mem:
        print(f"     VirtualAllocEx failed, err={ctypes.get_last_error()}")
        return False

    written = ctypes.c_size_t(0)
    ok = kernel32.WriteProcessMemory(hProcess, remote_mem, path_bytes, size, ctypes.byref(written))
    if not ok or written.value != size:
        print(f"     WriteProcessMemory failed, err={ctypes.get_last_error()}")
        return False

    hK32 = kernel32.GetModuleHandleW("kernel32.dll")
    loadlib_addr = kernel32.GetProcAddress(hK32, b"LoadLibraryW")
    if not loadlib_addr:
        print("     GetProcAddress(LoadLibraryW) failed")
        return False

    thread_id = wt.DWORD(0)
    hThread = kernel32.CreateRemoteThread(hProcess, None, 0, loadlib_addr, remote_mem, 0, ctypes.byref(thread_id))
    if not hThread:
        print(f"     CreateRemoteThread failed, err={ctypes.get_last_error()}")
        return False
    print(f"     remote thread created (tid={thread_id.value}), will run once the debug event is continued")
    kernel32.CloseHandle(hThread)
    return True


TH32CS_SNAPPROCESS = 0x00000002
TH32CS_SNAPTHREAD = 0x00000004
THREAD_SUSPEND_RESUME = 0x0002


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)), ("th32ModuleID", wt.DWORD),
        ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wt.DWORD), ("szExeFile", ctypes.c_char * 260),
    ]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
        ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", ctypes.c_long),
        ("tpDeltaPri", ctypes.c_long), ("dwFlags", wt.DWORD),
    ]


kernel32.CreateToolhelp32Snapshot.restype = wt.HANDLE
kernel32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
kernel32.Process32First.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32)]
kernel32.Process32Next.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32)]
kernel32.Thread32First.argtypes = [wt.HANDLE, ctypes.POINTER(THREADENTRY32)]
kernel32.Thread32Next.argtypes = [wt.HANDLE, ctypes.POINTER(THREADENTRY32)]
kernel32.OpenThread.restype = wt.HANDLE
kernel32.OpenThread.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
kernel32.SuspendThread.restype = wt.DWORD
kernel32.SuspendThread.argtypes = [wt.HANDLE]
kernel32.ResumeThread.argtypes = [wt.HANDLE]


def get_pids_by_name(image_name):
    pids = set()
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == wt.HANDLE(-1).value or not snap:
        return pids
    entry = PROCESSENTRY32()
    entry.dwSize = ctypes.sizeof(PROCESSENTRY32)
    if kernel32.Process32First(snap, ctypes.byref(entry)):
        while True:
            name = entry.szExeFile.decode(errors='ignore')
            if name.lower() == image_name.lower():
                pids.add(entry.th32ProcessID)
            if not kernel32.Process32Next(snap, ctypes.byref(entry)):
                break
    kernel32.CloseHandle(snap)
    return pids


def suspend_all_threads(pid):
    handles = []
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    if not snap:
        return handles
    entry = THREADENTRY32()
    entry.dwSize = ctypes.sizeof(THREADENTRY32)
    if kernel32.Thread32First(snap, ctypes.byref(entry)):
        while True:
            if entry.th32OwnerProcessID == pid:
                h = kernel32.OpenThread(THREAD_SUSPEND_RESUME, False, entry.th32ThreadID)
                if h:
                    kernel32.SuspendThread(h)
                    handles.append(h)
            if not kernel32.Thread32Next(snap, ctypes.byref(entry)):
                break
    kernel32.CloseHandle(snap)
    return handles


def resume_threads(handles):
    for h in handles:
        kernel32.ResumeThread(h)
        kernel32.CloseHandle(h)


def inject_via_external_python(helper_exe, pid, dll_paths):
    """Delegate the actual LoadLibraryW/CreateRemoteThread injection to a
    separate, bitness-matched executable (either a compiled helper like
    Inject32.exe, or 'python.exe injector.py' when run from source).
    Needed for 32-bit (WOW64) targets: a 64-bit process's
    kernel32.dll!LoadLibraryW address is meaningless inside a 32-bit
    target's address space, so the injecting process's bitness has to
    match the target's."""
    import subprocess
    if helper_exe.lower().endswith(".py"):
        cmd = [sys.executable, helper_exe, str(pid)] + dll_paths
    else:
        cmd = [helper_exe, str(pid)] + dll_paths
    print(f"  delegating injection to {cmd[0]} (bitness-matched helper)")
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    print(result.stdout.strip())
    if result.stderr.strip():
        print(result.stderr.strip())
    return result.returncode == 0


def poll_launch_and_inject(image_name, launch_uri_or_none, dll_paths, timeout_s=20, poll_s=0.001,
                            suspend_first=False, injector_python=None, launch_cmd=None):
    """Poll for a NEW process named image_name appearing and inject into it
    as fast as possible, racing to beat its own D3D device/resource
    creation. If launch_uri_or_none is given, it is triggered via
    'cmd /c start' right after we start polling. If launch_cmd is given
    instead (a full argv list), it's run directly via subprocess.Popen -
    used to launch steam.exe itself with extra flags (see play3d.py) rather
    than going through the shell's steam:// URI handler, which can't pass
    flags through to steam.exe.

    suspend_first=True freezes every thread before injecting, which sounds
    safer but usually deadlocks: LoadLibraryW needs the process's loader
    lock, and a brand-new process's only thread is very likely to be
    holding that exact lock (doing its own static-import setup) at the
    moment we freeze it - so the injection thread waits forever. Default
    is to inject immediately without suspending anything."""
    import time, subprocess

    baseline = get_pids_by_name(image_name)
    print(f"baseline {image_name} pids (ignored): {baseline}")

    if launch_cmd:
        subprocess.Popen(launch_cmd, shell=False)
        print(f"triggered launch: {launch_cmd}")
    elif launch_uri_or_none:
        subprocess.Popen(["cmd", "/c", "start", "", launch_uri_or_none], shell=False)
        print(f"triggered launch: {launch_uri_or_none}")

    start = time.time()
    while time.time() - start < timeout_s:
        current = get_pids_by_name(image_name)
        new_pids = current - baseline
        if new_pids:
            pid = sorted(new_pids)[0]
            t_found = time.time()
            print(f"NEW PID {pid} detected after {t_found-start:.4f}s")

            handles = suspend_all_threads(pid) if suspend_first else []
            if suspend_first:
                print(f"  suspended {len(handles)} thread(s)")

            if injector_python:
                inject_via_external_python(injector_python, pid, dll_paths)
            else:
                access = (PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ)
                hProcess = kernel32.OpenProcess(access, False, pid)
                if hProcess:
                    for dll in dll_paths:
                        inject_one(hProcess, dll)
                    kernel32.CloseHandle(hProcess)
                else:
                    print(f"OpenProcess({pid}) failed err={ctypes.get_last_error()}")

            if suspend_first:
                resume_threads(handles)
            print(f"done, {time.time()-t_found:.4f}s after detection")
            return pid
        time.sleep(poll_s)

    print(f"no new {image_name} process seen within {timeout_s}s")
    return None


def main():
    if len(sys.argv) < 3:
        print("Usage:")
        print("  python injector.py <PID> <dll_path> [<dll_path2> ...]")
        print("  python injector.py --launch <exe_path> <dll_path> [<dll_path2> ...]")
        print("  python injector.py --debug-launch <exe_path> <dll_path> [<dll_path2> ...]")
        print("  python injector.py --poll-launch <image_name> <steam_uri> [--injector-python <helper.exe>] <dll_path> [<dll_path2> ...]")
        print()
        print("(most users want play3d.py instead - this is the lower-level primitive it's built on)")
        sys.exit(1)

    if sys.argv[1] == '--debug-launch':
        exe_path = sys.argv[2]
        dll_paths = sys.argv[3:]
        pid = debug_launch_and_inject(exe_path, dll_paths)
        sys.exit(0 if pid else 3)

    if sys.argv[1] == '--steam-launch':
        steam_exe = sys.argv[2]
        launch_uri = sys.argv[3]
        dll_paths = sys.argv[4:]
        pid = debug_attach_steam_and_launch(steam_exe, launch_uri, dll_paths)
        sys.exit(0 if pid else 3)

    if sys.argv[1] == '--poll-launch':
        image_name = sys.argv[2]
        launch_uri = sys.argv[3]
        rest = sys.argv[4:]
        injector_python = None
        if '--injector-python' in rest:
            i = rest.index('--injector-python')
            injector_python = rest[i + 1]
            rest = rest[:i] + rest[i + 2:]
        dll_paths = rest
        pid = poll_launch_and_inject(image_name, launch_uri, dll_paths, injector_python=injector_python)
        sys.exit(0 if pid else 3)

    if sys.argv[1] == '--launch':
        exe_path = sys.argv[2]
        dll_paths = sys.argv[3:]
        pid = launch_suspended_and_inject(exe_path, dll_paths)
        sys.exit(0 if pid else 3)

    pid = int(sys.argv[1])
    dll_paths = sys.argv[2:]

    access = (PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
              PROCESS_VM_READ | PROCESS_CREATE_THREAD)
    hProcess = kernel32.OpenProcess(access, False, pid)
    if not hProcess:
        print(f"OpenProcess({pid}) failed, err={ctypes.get_last_error()}")
        sys.exit(2)
    print(f"OpenProcess({pid}) OK")

    # Strictly in the order given, waiting for each DllMain to finish before
    # starting the next. An earlier version fired all CreateRemoteThreads at
    # once to save time; that made the TriDefIgnition.dll / TriDefD3D9.dll
    # initialisation order random, and TriDefD3D9.dll loaded before its
    # Ignition counterpart sits inert - the DX9 side worked about one launch
    # in three. Sequential loading is what the 64-bit in-process path
    # always did, which is why DX11 never had this problem.
    all_ok = True
    for dll in dll_paths:
        ok = inject_one(hProcess, dll)
        all_ok = all_ok and ok

    kernel32.CloseHandle(hProcess)
    sys.exit(0 if all_ok else 3)


if __name__ == '__main__':
    main()

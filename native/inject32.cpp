// Inject32.exe - tiny native 32-bit LoadLibraryW injector.
//
//   Inject32.exe <pid> <dll> [dll ...]
//
// Replaces the earlier PyInstaller-packed helper. That one spent ~1.5 s
// unpacking itself on every run, which landed the injection *after* fast
// starting DirectX 9 games (Source engine) had already created their
// device - TriDef then had nothing left to hook. This does the same job
// in a few milliseconds.
//
// Deliberately header-free and CRT-free (only kernel32 + shell32 imports)
// so it builds with a bare MSVC toolchain and no Windows SDK headers.
// Output lines mirror what the launcher printed before.

typedef unsigned long DWORD;
typedef int BOOL;
typedef void *HANDLE;
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef unsigned long SIZE_T;   // 32-bit build only
typedef unsigned long ULONG_PTR;
typedef void *FARPROC;
typedef DWORD (__stdcall *LPTHREAD_START_ROUTINE)(LPVOID);

#define WINAPI __stdcall
#define DLLIMPORT extern "C" __declspec(dllimport)

DLLIMPORT HANDLE WINAPI OpenProcess(DWORD, BOOL, DWORD);
DLLIMPORT DWORD  WINAPI GetLastError(void);
DLLIMPORT HANDLE WINAPI GetModuleHandleW(const wchar_t *);
DLLIMPORT FARPROC WINAPI GetProcAddress(HANDLE, const char *);
DLLIMPORT LPVOID WINAPI VirtualAllocEx(HANDLE, LPVOID, SIZE_T, DWORD, DWORD);
DLLIMPORT BOOL   WINAPI VirtualFreeEx(HANDLE, LPVOID, SIZE_T, DWORD);
DLLIMPORT BOOL   WINAPI WriteProcessMemory(HANDLE, LPVOID, LPCVOID, SIZE_T, SIZE_T *);
DLLIMPORT HANDLE WINAPI CreateRemoteThread(HANDLE, LPVOID, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, DWORD *);
DLLIMPORT DWORD  WINAPI WaitForSingleObject(HANDLE, DWORD);
DLLIMPORT BOOL   WINAPI GetExitCodeThread(HANDLE, DWORD *);
DLLIMPORT BOOL   WINAPI CloseHandle(HANDLE);
DLLIMPORT const wchar_t *WINAPI GetCommandLineW(void);
DLLIMPORT wchar_t **WINAPI CommandLineToArgvW(const wchar_t *, int *);
DLLIMPORT HANDLE WINAPI GetStdHandle(DWORD);
DLLIMPORT BOOL   WINAPI WriteFile(HANDLE, LPCVOID, DWORD, DWORD *, LPVOID);
DLLIMPORT void   WINAPI ExitProcess(DWORD);

#define PROCESS_CREATE_THREAD     0x0002
#define PROCESS_VM_OPERATION      0x0008
#define PROCESS_VM_READ           0x0010
#define PROCESS_VM_WRITE          0x0020
#define PROCESS_QUERY_INFORMATION 0x0400
#define MEM_COMMIT  0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define PAGE_READWRITE 0x04
#define WAIT_OBJECT_0 0
#define STD_OUTPUT_HANDLE ((DWORD)-11)

static HANDLE g_out;

static void out(const char *s)
{
    DWORD n = 0, w = 0;
    while (s[n]) n++;
    WriteFile(g_out, s, n, &w, 0);
}

static void outw(const wchar_t *s)
{
    // Paths here are plain ASCII; anything else is replaced with '?'.
    char buf[64];
    DWORD i = 0, w = 0;
    while (*s) {
        buf[i++] = (*s < 128) ? (char)*s : '?';
        s++;
        if (i == sizeof buf) { WriteFile(g_out, buf, i, &w, 0); i = 0; }
    }
    if (i) WriteFile(g_out, buf, i, &w, 0);
}

static void outnum(DWORD v)
{
    char buf[12];
    int i = 11;
    buf[i] = 0;
    do { buf[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    out(buf + i);
}

static void outhex(DWORD v)
{
    char buf[11];
    buf[0] = '0'; buf[1] = 'x'; buf[10] = 0;
    for (int i = 9; i >= 2; i--) { int d = v & 15; buf[i] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
    out(buf);
}

static DWORD parse_pid(const wchar_t *s)
{
    DWORD v = 0;
    while (*s >= L'0' && *s <= L'9') { v = v * 10 + (DWORD)(*s - L'0'); s++; }
    return v;
}

extern "C" int WINAPI EntryMain(void)
{
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 3) {
        out("usage: Inject32.exe <pid> <dll> [dll ...]\n");
        ExitProcess(2);
    }
    DWORD pid = parse_pid(argv[1]);
    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                           PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, 0, pid);
    if (!h) {
        out("OpenProcess("); outnum(pid); out(") failed, err="); outnum(GetLastError()); out("\n");
        ExitProcess(3);
    }
    out("OpenProcess("); outnum(pid); out(") OK\n");

    // kernel32.dll is mapped at the same base in every 32-bit process of a
    // session, so our own LoadLibraryW address is valid inside the target.
    FARPROC loadlib = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");

    int fails = 0;
    for (int i = 2; i < argc; i++) {
        const wchar_t *dll = argv[i];
        SIZE_T len = 0;
        while (dll[len]) len++;
        SIZE_T bytes = (len + 1) * sizeof(wchar_t);
        out("  -> injecting: "); outw(dll); out("\n");

        LPVOID mem = VirtualAllocEx(h, 0, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!mem || !WriteProcessMemory(h, mem, dll, bytes, 0)) {
            out("     "); outw(dll); out(": alloc/write failed, err="); outnum(GetLastError()); out("\n");
            fails++;
            continue;
        }
        HANDLE t = CreateRemoteThread(h, 0, 0, (LPTHREAD_START_ROUTINE)loadlib, mem, 0, 0);
        if (!t) {
            out("     "); outw(dll); out(": CreateRemoteThread failed, err="); outnum(GetLastError()); out("\n");
            fails++;
            continue;
        }
        DWORD w = WaitForSingleObject(t, 15000);
        DWORD code = 0;
        GetExitCodeThread(t, &code);
        CloseHandle(t);
        VirtualFreeEx(h, mem, 0, MEM_RELEASE);
        if (w != WAIT_OBJECT_0) {
            out("     "); outw(dll); out(": timed out waiting for LoadLibraryW\n");
            fails++;
        } else if (code == 0) {
            out("     "); outw(dll); out(": LoadLibraryW returned NULL\n");
            fails++;
        } else {
            out("     "); outw(dll); out(": loaded OK, handle="); outhex(code); out("\n");
        }
    }
    CloseHandle(h);
    ExitProcess(fails ? 1 : 0);
    return 0;
}

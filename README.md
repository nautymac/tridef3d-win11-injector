# SRWeave — DX9/DX11 게임 안에서 SR SDK로 직접 위빙

TriDef 3D가 만든 좌우(SBS) 화면을 **게임 프로세스 안에서, `Present` 직전에** SR 런타임으로 렌티큘러 위빙한다.
SRCapture3D(캡처 → 위빙)보다 한 프레임 빠르고, 전체화면 독점 모드도 그대로 된다.

| 파일 | 대상 | 후킹 지점 |
|---|---|---|
| `SRWeaveDX9.dll` (x86/x64) | DirectX 9 | `IDirect3DDevice9::Present` / `Reset`, `IDirect3DSwapChain9::Present` |
| `SRWeaveDX11.dll` (x86/x64) | DirectX 11 | `IDXGISwapChain::Present` / `Present1` / `ResizeBuffers` |

산출물은 `bin\weave\x64\`, `bin\weave\x86\`.

## 쓰는 법

**A. 주입 (기본)** — [tridef3d-win11-injector](https://github.com/nautymac/tridef3d-win11-injector)의
`Tridef3D_Play_SR.exe`(또는 `Tridef3D_Play.exe --sr`)가 TriDef DLL 다음에 이 DLL을 순서대로 주입한다.
런처 폴더에 `sr\x86\SRWeaveDX9.dll`, `sr\x64\SRWeaveDX11.dll` 등을 둔다. 32비트 게임 → DX9 DLL, 64비트 → DX11 DLL.

**B. 프록시 (파일 복사)** — 게임 폴더에 `SRWeaveDX9.dll`을 `d3d9.dll`로, `SRWeaveDX11.dll`을 `dxgi.dll`(또는 `d3d11.dll`)로
복사한다. 게임이 로드할 때 System32 원본으로 넘겨주면서 훅을 건다. TriDef 주입은 평소대로.

## 설정·로그

DLL 옆의 `SRWeave.ini` (`[SRWeave]` 섹션), 로그는 같은 폴더의 `SRWeave.log`.

| 키 | 기본 | 뜻 |
|---|---|---|
| `weave` | 1 | 위빙 자체 켜고 끄기 |
| `swap_eyes` | 0 | TriDef가 오른눈\|왼눈 순서로 낼 때 1 |
| `srgb` | 0 | 백버퍼를 sRGB로 취급 |
| `test` | 0 | 게임 화면 대신 왼쪽 빨강 / 오른쪽 파랑 (위빙·눈 순서 확인용) |
| `lens` | 1 | 위빙 중 렌티큘러 렌즈 켜기 |
| `latency_frames` | 1 | `setLatencyInFrames` |
| `hook_delay_ms` | 0 | 주입 모드에서 훅을 거는 시점을 늦춘다 |

단축키(게임 창 포커스): Ctrl+Alt+W 위빙, Ctrl+Alt+S 좌우 바꾸기, Ctrl+Alt+T 테스트 패턴, Ctrl+Alt+L 렌즈.

## TriDef 밑에 들어가는 방법 (핵심)

TriDef는 `d3d9.dll` 내부의 디바이스 생성까지 후킹해서 **모든 디바이스를 완전한 래퍼 객체**로 돌려준다(vtable 전체가
`TriDefD3D9.dll`). 그리고 **진짜 디바이스 객체의 vtable 포인터는 힙 복사본**으로 바꿔 `Present`/`Reset`만 자기 것으로
패치한다. 그래서:

1. 임시 디바이스(래퍼)를 만들고, 그 스왑체인 래퍼의 멤버에서 vtable이 `d3d9.dll` 안에 있는 **진짜 스왑체인**을 찾는다.
2. 진짜 스왑체인의 `GetDevice()`로 **진짜 디바이스**를 얻는다(vtable은 힙 복사본).
3. 그 복사본에서 패치되지 않은 항목들을 지문 삼아 디스크의 `d3d9.dll` 원본 이미지에서 원래 vtable을 찾고,
   거기서 원본 `Present`/`Reset` 주소를 얻는다 (`CleanVtableEntryByFingerprint`).
4. 그 **진짜 함수를 MinHook 인라인 후킹**한다. TriDef 래퍼가 SBS를 다 그리고 원본을 부르면 그 안에서 우리가 위빙한다.

임시 객체는 해제하지 않는다 — TriDef 래퍼를 해제하면 TriDef가 흔들려 게임이 죽었다.
`IDirect3DDevice9Ex::PresentEx`는 아직 안 건다(Ex 디바이스를 만들면 역시 래핑되고 불안정).

위빙 절차(DX9): 백버퍼 → `StretchRect` → SBS 텍스처, `setInputViewTexture(tex, 폭/2, 높이, 포맷)`,
상태 블록 캡처 → RT=백버퍼, 깊이 없음, Z 끔 → `BeginScene`/`weave()`/`EndScene` → 상태 복원 → 원래 `Present`.
`Reset` 전후로 `invalidateDeviceObjects`/`restoreDeviceObjects`.

## 빌드

```
powershell -ExecutionPolicy Bypass -File setup-deps.ps1 -SrSdkInstaller <win64.exe> -SrSdk32Installer <win32.exe>
powershell -ExecutionPolicy Bypass -File weave\build-weave.ps1
```

MSVC(x86·x64 크로스 툴)와 Windows 10 SDK만 있으면 된다. `build-weave.ps1`은 vswhere 없이 MSVC와 SDK를 직접 찾아
Ninja로 두 아키텍처를 빌드한다. CRT는 정적 링크, SR 런타임 DLL은 지연 로드(SpatialLabs 설치 폴더에서 찾는다),
MinHook은 `weave\third_party\minhook`에 포함.

## 검증 (2026-09-23)

Left 4 Dead (DX9, 32비트) + TriDef 3D: 진짜 `d3d9.dll` 함수에 훅, SR 컨텍스트·위버 생성, `first weaved frame`,
게임 내 해상도 변경(`Reset`) 두 번 통과, 게임 생존.

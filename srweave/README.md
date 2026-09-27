# SRWeave — DX9/DX11 게임 안에서 SR SDK로 직접 위빙

이미 좌우(SBS)로 만들어진 게임 화면을 **게임 프로세스 안에서, `Present` 직전에** SR(Simulated Reality / Acer SpatialLabs)
런타임으로 렌티큘러 위빙한다. SBS를 누가 만들었는지는 상관없다 — TriDef 3D, Geo-11/3DMigoto, ReShade SuperDepth3D 모두 된다.
화면을 캡처해서 위빙하는 방식([SRCapture3D](https://github.com/nautymac/SRCapture3D))보다 한 프레임 빠르고,
전체화면 독점 모드도 그대로 된다.

필요한 것: SR/SpatialLabs 런타임이 설치되어 SR Service가 돌고 있는 렌티큘러 3D 패널.

| 파일 | 대상 | 후킹 지점 |
|---|---|---|
| `SRWeaveDX9.dll` (x86/x64) | DirectX 9 | `IDirect3DDevice9::Present` / `Reset`, `IDirect3DSwapChain9::Present` |
| `SRWeaveDX11.dll` (x86/x64) | DirectX 11 | `IDXGISwapChain::Present` / `Present1` / `ResizeBuffers` |

산출물은 `bin\x64\`, `bin\x86\`. 게임이 32비트면 x86, 64비트면 x64 DLL을 쓴다.

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
powershell -ExecutionPolicy Bypass -File build.ps1
```

MSVC(x86·x64 크로스 툴)와 Windows 10 SDK만 있으면 된다. `build.ps1`은 MSVC와 SDK를 직접 찾아 Ninja로 두 아키텍처를
빌드한다(찾지 못하면 `$env:SRWEAVE_MSVC`, `$env:SRWEAVE_SDK`로 지정). CRT는 정적 링크, SR 런타임 DLL은 지연
로드(SpatialLabs 설치 폴더에서 찾는다), MinHook(BSD-2)은 `third_party\minhook`에 포함.

SR SDK는 재배포하지 않는다 — LeiaSR/SpatialLabs 런타임 설치본에서 `setup-deps.ps1`로 풀어 쓴다. x86 DLL을 빌드하려면
win32 판 SDK도 필요하고, 그것은 win64 설치 파일을 풀면 나오는 `temp\` 폴더 안에 들어 있다.

## 검증 (2026-09-23)

Left 4 Dead (DX9, 32비트) + TriDef 3D: 진짜 `d3d9.dll` 함수에 훅, SR 컨텍스트·위버 생성, `first weaved frame`,
게임 내 해상도 변경(`Reset`) 두 번 통과, 게임 생존.

Binary Domain (DX9, 32비트) + TriDef 3D: 임시 디바이스 래퍼의 `GetSwapChain(0)`이 `0x8876086c`(D3DERR_INVALIDCALL)로
실패해서 처음엔 훅이 하나도 안 걸렸다. 대체 경로 추가 — 스왑체인 대신 **디바이스 래퍼의 멤버에서 진짜 디바이스를 직접 찾는다**
(`FindInnerBySlots`: vtable 119칸 중 80칸 이상이 `d3d9.dll`을 가리키는 객체, `QueryInterface(IDirect3DDevice9)`로 확인).
그 힙 vtable로 지문을 떠 원본 `Present`/`Reset`을 찾는다. 결과: 훅 → SR 컨텍스트 → 위버 → `first weaved frame: 3840x2160`,
패널에서 육안으로 위빙 확인 (2026-09-23). 관리자 권한 없이 `Tridef3D_Play_SR.exe`로 주입해도 된다(게임을 Steam이 일반 권한으로 띄우므로).

Gone Home (DX11, 64비트) + TriDef 3D: TriDef의 DXGI 계층(`TriDefDXGI64.dll`)도 스왑체인을 완전 래퍼로 돌려줘 처음엔 훅이
0개였다. DX9 디바이스와 같은 대체 경로(`FindInnerBySlots`로 래퍼의 1번 필드에서 dxgi.dll vtable을 가진 진짜 스왑체인을 찾고
`QueryInterface(IDXGISwapChain)`으로 확인)로 `Present`/`Present1`/`ResizeBuffers`를 후킹. 결과: 위버 생성 →
`first weaved frame: 3840x2160`, `ResizeBuffers` 두 번 통과, 게임 생존 (2026-09-23).
Left 4 Dead는 같은 빌드로 회귀 확인(스왑체인 경로 그대로 동작).

## 라이선스

MIT — 저장소 루트의 `LICENSE`. 포함된 [MinHook](https://github.com/TsudaKageyu/minhook)은 BSD-2
(`third_party\minhook\LICENSE.txt`). SR SDK와 런타임은 포함하지 않는다 — Leia/Acer 것이다.

## 이 폴더의 위치

원래 [SRCapture3D](https://github.com/nautymac/SRCapture3D) 의 `weave\` 였고, 2026-09-27에 이 저장소
(`tridef3d-win11-injector`)의 `srweave\` 로 옮겨 왔다 — `--sr` 로 이 DLL을 주입하는 런처와 늘 같이 쓰이기 때문이다.
히스토리는 그대로 남아 있다.

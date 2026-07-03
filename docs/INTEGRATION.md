# baroCCTVSimulator 통합 가이드 — baroQuantum 세우기 & baro_unreal 마이그레이션

이 문서는 `baroCCTVSimulator` 플러그인을 소비하는 두 프로젝트를 세우는 실행 절차다.

## 목차

- [단일 소스 구조](#단일-소스-구조)
- [Phase 1 — baroQuantum 세우기 (심플 프레임워크)](#phase-1--baroquantum-세우기-심플-프레임워크)
  - [1. 빈 C++ 프로젝트 생성](#1-빈-c-프로젝트-생성)
  - [2. baroCCTVSimulator submodule 부착](#2-barocctvsimulator-submodule-부착)
  - [3. .uproject 에 플러그인 등록](#3-uproject-에-플러그인-등록)
  - [4. Config 설정](#4-config-설정)
  - [5. 빌드](#5-빌드)
  - [6. 그레이박스 데모 레벨 제작](#6-그레이박스-데모-레벨-제작)
  - [7. 검증](#7-검증)
- [Phase 2 — baro_unreal 마이그레이션 (실사 개발용)](#phase-2--baro_unreal-마이그레이션-실사-개발용)
- [트러블슈팅](#트러블슈팅)

## 단일 소스 구조

```
C:\works\ue_prjs\
├── baroCCTVSimulator\  ← CCTV 시뮬 C++ 의 유일한 진실 소스 (이 저장소, 독립 git)
├── baroQuantum\        ← 심플 프레임워크/시연용. Plugins/baroCCTVSimulator = submodule
└── baro_unreal\        ← 실사 개발용(30GB). Plugins/baroCCTVSimulator = submodule (Phase 2 완료)
```

버그픽스/기능은 `baroCCTVSimulator` 에서만 하고, 두 프로젝트는 submodule 포인터만 갱신한다.

> **GitHub 원격 권장**: 팀 공유를 하려면 `baroCCTVSimulator` 을 먼저 GitHub(private)로 push 하고,
> 그 URL 로 submodule 을 건다. 원격 없이 로컬에서 먼저 검증하려면 submodule URL 자리에
> 로컬 경로(`C:/works/ue_prjs/baroCCTVSimulator`)를 써도 된다(나중에 `git submodule set-url` 로 교체).

---

## Phase 1 — baroQuantum 세우기 (심플 프레임워크)

### 1. 빈 C++ 프로젝트 생성

Epic Launcher → **UE 5.8** → New Project → **Games → Blank** →
- **C++** 탭 선택 (BP 아님 — 코드 플러그인 모듈이 컴파일되려면 C++ 프로젝트여야 함)
- Starter Content: **없음** (그레이박스는 엔진 기본 셰이프만 사용)
- 이름 `baroQuantum`, 경로 `C:\works\ue_prjs`

에디터가 열리면 **일단 닫는다** (아래 submodule/config 후 재빌드).

### 2. baroCCTVSimulator submodule 부착

```bash
cd C:/works/ue_prjs/baroQuantum
git init            # 런처가 git 을 안 만들었다면
git submodule add C:/works/ue_prjs/baroCCTVSimulator Plugins/baroCCTVSimulator
#  또는 원격이 있으면:  git submodule add <baroCCTVSimulator-repo-url> Plugins/baroCCTVSimulator
git submodule update --init --recursive
```

`baroQuantum/Plugins/baroCCTVSimulator/baroCCTVSimulator.uplugin` 이 보이면 성공.

### 3. .uproject 에 플러그인 등록

`baroQuantum.uproject` 의 `"Plugins"` 배열에 추가:

```json
"Plugins": [
    { "Name": "baroCCTVSimulator", "Enabled": true }
]
```

> 게임 모듈 `Build.cs` 에는 `"baroCCTVSimulator"` 의존성을 **추가하지 않아도 된다** — baroQuantum
> 게임 코드는 baroCCTVSimulator 타입을 직접 참조하지 않고, GameMode 는 config 문자열
> `/Script/baroCCTVSimulator.BaroSimGameMode` 로 런타임 해석되기 때문. (나중에 게임 모듈 C++ 에서
> baroCCTVSimulator 클래스를 직접 쓰게 되면 그때 추가.)

### 4. Config 설정

`Config/DefaultEngine.ini`:

```ini
[/Script/EngineSettings.GameMapsSettings]
GameDefaultMap=/Game/simulator/LV_Quantum_sim_01
GlobalDefaultGameMode=/Script/baroCCTVSimulator.BaroSimGameMode

[/Script/Engine.RendererSettings]
r.DynamicGlobalIlluminationMethod=1
r.ReflectionMethod=1
r.GenerateMeshDistanceFields=True
```

`Config/DefaultGame.ini`:

```ini
[/Script/baroCCTVSimulator.HucomsServerSubsystem]
BaseHttpPort=8081
BaseMjpegPort=8091
StreamFps=30
StreamWidth=1280
StreamHeight=720
CaptureContrast=1.2
CaptureExposureBias=-0.7
```

> baroQuantum 은 **신규 프로젝트라 CoreRedirects 가 필요 없다** — 처음부터 `/Script/baroCCTVSimulator.*`
> 를 참조한다. (기존 콘텐츠를 가진 baro_unreal 만 Phase 2 에서 redirect 가 필요.)

### 5. 빌드

`baroQuantum.uproject` 우클릭 → **Generate Visual Studio project files** → 그다음:

```powershell
& "C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" `
  baroQuantumEditor Win64 Development -Project="C:\works\ue_prjs\baroQuantum\baroQuantum.uproject" -WaitMutex
```

빌드가 성공하면 baroCCTVSimulator 모듈(19파일)이 baroQuantum 컨텍스트에서 **최초로 실제 컴파일**된다.
(플러그인 단독으로는 컴파일 불가 — 반드시 호스트 프로젝트 안에서 빌드된다.)

### 6. 그레이박스 데모 레벨 제작

에디터에서 새 레벨(Empty Level) 만들고 `/Game/simulator/LV_Quantum_sim_01` 로 저장.

**바닥 · 조명**
- `/Engine/BasicShapes/Plane` 1개, Location (0,0,0), Scale (100,100,1) → 100m×100m 바닥.
- Directional Light + Sky Atmosphere + SkyLight (Environment Light Mixer 로 한 번에 추가 가능).

**"차" (그레이박스 박스)**
- `/Engine/BasicShapes/Cube` 를 차 치수로: Scale **(4.5, 1.8, 1.5)**, Z=**75** (바닥에 얹힘).
- 2열 × 4대 예시 (X=열, Y=칸, 간격 300):

| 이름 | Location (X, Y, Z) |
|---|---|
| Car_R1_1..4 | ( 400, -450/-150/150/450, 75) |
| Car_R2_1..4 | (-400, -450/-150/150/450, 75) |

**PTZ 카메라 4대** — 클래스 팔레트에서 **APTZCamera** 를 직접 배치 (BP 불필요):

| 카메라 | Location (X,Y,Z) | Rotation Yaw | 포트(자동) |
|---|---|---|---|
| PTZ_Cam_01 | (-1500,-1500, 500) |  45 | http 8081 / mjpeg 8091 |
| PTZ_Cam_02 | ( 1500,-1500, 500) | 135 | http 8082 / mjpeg 8092 |
| PTZ_Cam_03 | ( 1500, 1500, 500) | 225 | http 8083 / mjpeg 8093 |
| PTZ_Cam_04 | (-1500, 1500, 500) | 315 | http 8084 / mjpeg 8094 |

각 카메라 디테일 패널:
- `bServeHucoms = true`
- `HucomsHttpPort = 0`, `HucomsMjpegPort = 0` (0=자동 부여, 위 표대로 인덱스 가산)
- 초기 조준: `TargetTilt` 를 -15~-25 정도로 (아래를 보게). 필요하면 `BodyMesh`/`HeadMesh` 에
  Cube 를 물려 카메라를 눈에 보이게 할 수 있으나 시뮬 기능엔 불필요.

> **주의**: `ABaroSimGameMode` 는 standalone(-game) 에서 **월드 렌더를 끈다**(헤드리스 카메라 서버 목적).
> 그레이박스를 눈으로 확인/편집할 때는 **PIE**(에디터 Play)를 쓰면 월드가 보인다. 스트림/스냅샷은
> SceneCapture 자체 렌더라 월드 렌더 on/off 와 무관하게 정상 동작한다.

레벨을 `GameDefaultMap` 으로 저장했으면 끝.

### 7. 검증

standalone 실행:

```powershell
& "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe" `
  "C:\works\ue_prjs\baroQuantum\baroQuantum.uproject" -game -windowed -resx=1280 -resy=720
```

다른 터미널에서:

```powershell
# PTZ 위치 라운드트립
curl "http://127.0.0.1:8081/cgi-bin/control/ptzf_status.cgi?action=getptzfpos"
# 실렌더 스냅샷
curl "http://127.0.0.1:8081/cgi-bin/image/jpeg.cgi" -o snap01.jpg
# 2번 카메라
curl "http://127.0.0.1:8082/cgi-bin/image/jpeg.cgi" -o snap02.jpg
```

`snap01.jpg` 에 그레이박스 주차장(바닥+박스)이 찍히면 성공.
연속 MJPEG 는 `:8091`(MediaMTX runOnDemand 입력)로 확인.

---

## Phase 2 — baro_unreal 마이그레이션 (실사 개발용)

> ✅ **완료(2026-07-03)** — baro_unreal 도 이 플러그인을 submodule 로 소비하도록 이관 후 리빌드 성공.
> 아래는 그 절차 기록(재현/역참조용). baro_unreal 쪽 변경은 아직 **미커밋**(사용자 리뷰용).
>
> Live Coding 으로는 불가(WorldSubsystem/모듈 구조 변경) — 에디터 닫고 CLI 풀 리빌드 필요.

1. **에디터 닫기** (baro_unreal + MCP 서버 종료).

2. **submodule 부착**
   ```bash
   cd C:/works/ue_prjs/baro_unreal
   git submodule add C:/works/ue_prjs/baroCCTVSimulator Plugins/baroCCTVSimulator
   ```
   > ⚠️ **gotcha**: baro_unreal 은 `.gitignore` 가 `Plugins/`(상용 RYU 2.6GB) 통째 제외라 submodule add 가 막힌다.
   > `.gitignore` 의 `Plugins/` 를 `Plugins/*` + `!Plugins/baroCCTVSimulator` 로 바꿔 **자체 플러그인만 예외처리**한 뒤 add 한다.

3. **게임 모듈에서 CCTV 19파일 삭제** — `Source/baro_unreal/` 에서 아래만 남기고 삭제:
   - 남김: `baro_unreal.cpp`, `baro_unreal.h`, `baro_unreal.Build.cs` (+ `../*.Target.cs`)
   - 삭제: `HucomsServerSubsystem.*`, `HucomsProtocol.h`, `PTZCamera.*`, `PTZCaptureComponent.*`,
     `PTZPlayerController.*`, `MjpegStreamServer.*`, `CenteringClientComponent.*`,
     `BaroSimGameMode.*`, `BaroSimHUD.*`, `BaroSimPlayerController.*`

4. **`baro_unreal.uproject`** — `"Plugins"` 에 `{ "Name": "baroCCTVSimulator", "Enabled": true }` 추가.

5. **`baro_unreal.Build.cs`** — CCTV 전용이던 Private 의존성 제거(플러그인이 자체 보유):
   ```csharp
   PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput" });
   // HTTP/Json/HTTPServer/Sockets/Networking 줄 삭제 (baroCCTVSimulator 플러그인으로 이동됨)
   ```

6. **Config 경로 갱신** — 모듈명이 바뀌었으므로:
   - `Config/DefaultEngine.ini`:
     `GlobalDefaultGameMode=/Script/baro_unreal.BaroSimGameMode` → `=/Script/baroCCTVSimulator.BaroSimGameMode`
   - `Config/DefaultGame.ini`:
     `[/Script/baro_unreal.HucomsServerSubsystem]` → `[/Script/baroCCTVSimulator.HucomsServerSubsystem]`

7. **CoreRedirects (필수)** — 기존 `.umap`/BP 들이 네이티브 클래스를 `/Script/baro_unreal.*`
   로 참조하므로, 모듈 이동 후 깨지지 않게 `Config/DefaultEngine.ini` 에 추가:
   ```ini
   [CoreRedirects]
   +ClassRedirects=(OldName="/Script/baro_unreal.PTZCamera",NewName="/Script/baroCCTVSimulator.PTZCamera")
   +ClassRedirects=(OldName="/Script/baro_unreal.PTZCaptureComponent",NewName="/Script/baroCCTVSimulator.PTZCaptureComponent")
   +ClassRedirects=(OldName="/Script/baro_unreal.PTZPlayerController",NewName="/Script/baroCCTVSimulator.PTZPlayerController")
   +ClassRedirects=(OldName="/Script/baro_unreal.CenteringClientComponent",NewName="/Script/baroCCTVSimulator.CenteringClientComponent")
   +ClassRedirects=(OldName="/Script/baro_unreal.HucomsServerSubsystem",NewName="/Script/baroCCTVSimulator.HucomsServerSubsystem")
   +ClassRedirects=(OldName="/Script/baro_unreal.BaroSimGameMode",NewName="/Script/baroCCTVSimulator.BaroSimGameMode")
   +ClassRedirects=(OldName="/Script/baro_unreal.BaroSimHUD",NewName="/Script/baroCCTVSimulator.BaroSimHUD")
   +ClassRedirects=(OldName="/Script/baro_unreal.BaroSimPlayerController",NewName="/Script/baroCCTVSimulator.BaroSimPlayerController")
   +StructRedirects=(OldName="/Script/baro_unreal.CenteringPlate",NewName="/Script/baroCCTVSimulator.CenteringPlate")
   +EnumRedirects=(OldName="/Script/baro_unreal.ECenteringState",NewName="/Script/baroCCTVSimulator.ECenteringState")
   ```
   > 기존 `[CoreRedirects]` 의 `BP_PTZCamera -> PTZCamera` 매핑이 `/Script/baro_unreal.PTZCamera`
   > 를 NewName 으로 쓰고 있으면 `/Script/baroCCTVSimulator.PTZCamera` 로 함께 수정.

8. **프로젝트 파일 재생성 + 풀 리빌드**
   ```powershell
   & "C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" `
     baro_unrealEditor Win64 Development -Project="C:\works\ue_prjs\baro_unreal\baro_unreal.uproject" -WaitMutex
   ```

9. **검증** — 에디터 열어 `LV_Park_01`(PTZ 4대) 배치가 깨지지 않았는지 확인 + Phase 1 의 curl
   검증 반복. 정상이면 커밋(submodule 포인터 + config + 소스삭제).

---

## 트러블슈팅

- **`Cannot find module 'baroCCTVSimulator'`**: `.uproject` 플러그인 등록 누락 또는 submodule 이 비어 있음
  (`git submodule update --init`).
- **BP 가 클래스 참조를 잃음(baro_unreal)**: CoreRedirects(7번) 누락. ini 저장 후 에디터 재시작.
- **Shared 빌드환경 불일치 컴파일 에러**: `*.Target.cs` 가 V7/Unreal5_8 인지 확인(코드 문제 아님).
- **캡처가 뿌옇다**: TAA-off/FXAA 문제 — 선명도는 `PTZCaptureComponent` 의 TAA-on+Lumen override
  경로가 담당(플러그인 내장). 라플라시안 분산으로 실측 비교.
- **standalone 에서 화면이 검다**: 정상 — `ABaroSimGameMode` 가 월드 렌더를 끈다(헤드리스 서버).
  씬 확인은 PIE 로.

# baroCCTVSimulator — CCTV PTZ 시뮬레이터 플러그인 (UE 5.8)

주차장 CCTV PTZ 카메라를 언리얼 안에서 "실기(Hucoms)처럼" 행세시키는 재사용 가능한 런타임 플러그인.
`baro_calory`(Node/Python AI 에이전트)가 실기 IP 대신 이 시뮬레이터에 붙어 동일 프로토콜로 동작하도록 한다.

## 목차

- [무엇인가](#무엇인가)
- [모듈 구성](#모듈-구성)
- [의존성](#의존성)
- [프로젝트에 추가하기 (submodule)](#프로젝트에-추가하기-submodule)
- [설정 키 (config)](#설정-키-config)
- [포트 규약](#포트-규약)
- [게임모드와 실행 모드 (PIE vs standalone)](#게임모드와-실행-모드-pie-vs-standalone)
- [검증](#검증)
- [단일 소스 원칙](#단일-소스-원칙)

## 무엇인가

- **인엔진 Hucoms HTTP CGI 서버**: `UHucomsServerSubsystem` 이 레벨의 각 `APTZCamera` 를 자기 포트에
  독립 서버(채널)로 노출. `ptzf_status.cgi` / `ptz_centering.cgi` / `capabilityptz.cgi` / `jpeg.cgi` / `mjpeg.cgi`.
- **연속 MJPEG 스트리밍**: `FMjpegStreamServer` (TCP, multipart/x-mixed-replace). MediaMTX/ffmpeg 브리지가 RTSP 로 재송출.
- **PTZ 짐벌 + 캡처**: `APTZCamera`(Pan/Tilt/Zoom) + `UPTZCaptureComponent`(SceneCapture→JPEG).
- **VLA 관측 클라이언트**: `UCenteringClientComponent` — baro_vla 추론 서버로 프레임 전송, 번호판 위치 수신.
- **standalone 게임 프레임워크**: `ABaroSimGameMode` / `ABaroSimHUD` / `ABaroSimPlayerController`
  (SpectatorPawn + 월드렌더 OFF + ESC 종료 — "카메라 서버" 목적의 헤드리스풍 실행).
- **순수 프로토콜 헬퍼**: `HucomsProtocol.h` (단위/범위/좌표 변환, UObject 비의존).

## 모듈 구성

- 모듈명: `baroCCTVSimulator` (Runtime, LoadingPhase Default)
- `Source/baroCCTVSimulator/Public/` — 공개 헤더 10
- `Source/baroCCTVSimulator/Private/` — 구현 9 + 모듈 부트(`baroCCTVSimulatorModule.cpp`)
- export 매크로: `BAROCCTVSIMULATOR_API`

## 의존성

- Public: `Core CoreUObject Engine InputCore HTTP HTTPServer`
- Private: `Json Sockets Networking ImageCore`

## 프로젝트에 추가하기 (submodule)

플러그인은 **단일 소스**로 두고 각 프로젝트가 submodule 로 참조한다.

```bash
# 프로젝트 루트에서
git submodule add <baroCCTVSimulator-repo-url> Plugins/baroCCTVSimulator
git submodule update --init --recursive
```

그다음:

1. `<Project>.uproject` 의 `"Plugins"` 배열에 `{ "Name": "baroCCTVSimulator", "Enabled": true }` 추가.
2. 게임 모듈 `*.Build.cs` 의 의존성에 `"baroCCTVSimulator"` 추가.
3. 프로젝트 파일 재생성 후 에디터 타깃 빌드.

> 로컬에서 원격 없이 먼저 붙일 때는 URL 자리에 로컬 경로(`C:/works/ue_prjs/baroCCTVSimulator`)를 써도 된다.
> 이후 GitHub 원격을 만들면 `git submodule set-url` 로 교체.

## 설정 키 (config)

`Config/DefaultGame.ini` 에 아래 섹션으로 오버라이드 (모듈명이 섹션 경로에 들어감):

```ini
[/Script/baroCCTVSimulator.HucomsServerSubsystem]
BaseHttpPort=8081      ; 자동 포트 시작값(카메라 인덱스 가산)
BaseMjpegPort=8091
StreamFps=30           ; 연속 MJPEG 목표 fps
StreamWidth=1280
StreamHeight=720
CaptureContrast=1.2    ; 캡처 대비(하이키 클리핑 회피값)
CaptureExposureBias=-0.7
```

게임모드 지정은 `Config/DefaultEngine.ini`:

```ini
[/Script/EngineSettings.GameMapsSettings]
GlobalDefaultGameMode=/Script/baroCCTVSimulator.BaroSimGameMode
```

## 포트 규약

- 카메라의 `HucomsHttpPort`/`HucomsMjpegPort` 가 >0 이면 그 값을, 0 이면
  `BaseHttpPort`/`BaseMjpegPort + 카메라 인덱스` 로 자동 부여.
- `baro_calory` 의 `devices.list[].{host,port}` 와 카메라를 1:1 로 맞출 것.

## 게임모드와 실행 모드 (PIE vs standalone)

시뮬 게임모드 `ABaroSimGameMode` 는 "CCTV 카메라 서버"가 목적인 미니멀 게임모드다:
`ASpectatorPawn`(비가시) + `ABaroSimHUD`(검은 화면 위 타이틀/서버상태/종료안내) + `ABaroSimPlayerController`.

### 어떻게 적용되나 — 레벨이 아니라 **프로젝트 전역 config**

레벨 World Settings 에 아무것도 지정하지 않아도 적용된다. 게임모드는 프로젝트 전역 config 로 붙는다:

```ini
; Config/DefaultEngine.ini
[/Script/EngineSettings.GameMapsSettings]
GlobalDefaultGameMode=/Script/baroCCTVSimulator.BaroSimGameMode
```

GameMode 결정 **우선순위**: `레벨 World Settings Override` > `프로젝트 GlobalDefaultGameMode` > `커맨드라인 ?game=`.
레벨에 Override 가 없으면 전역 기본값이 자동 적용된다.
- 특정 레벨만 다른 게임모드를 쓰려면 그 레벨 **World Settings → GameMode Override** 지정(전역을 덮어씀).
- GUI 확인: **Project Settings → Maps & Modes → Default GameMode**.

### PIE 는 정상 화면, standalone 만 검은 화면 — **의도된 설계**

`ABaroSimGameMode::BeginPlay()` 는 `WorldType == EWorldType::Game`(= standalone `-game`)일 때만
메인 뷰포트의 `bDisableWorldRendering = true` 로 월드 렌더를 끈다:

| 실행 | WorldType | 메인 창 |
|---|---|---|
| **PIE** (에디터 ▶ Play) | `PIE` | early return → **정상 렌더** (에디터 작업 화면 유지) |
| **standalone** (`-game`) | `Game` | **검은 화면** (헤드리스 카메라 서버) |

- **검은 것은 "메인 창"뿐이다.** CCTV 프레임(`jpeg.cgi`/`mjpeg.cgi`)은 `UPTZCaptureComponent` 의
  `SceneCapture` 가 **자체적으로 따로 렌더**하므로 검은 화면과 무관하게 정상 출력된다
  (검은 창인 standalone 에서도 curl 로 실렌더 JPEG 이 나온다).
- **부수효과 — 캡처 화질**: standalone 은 메인 월드 렌더가 꺼져 있어 **Lumen GI 가 SceneCapture 에
  안 들어올 수 있다**(면이 어둡게 캡처됨). PIE 는 정상. 헤드리스 캡처 조명이 중요하면 라이팅(정적/베이크)
  또는 캡처 설정을 별도 튜닝한다.
- **standalone 에서도 씬을 보이게** 하려면 `BaroSimGameMode.cpp` 의 `bDisableWorldRendering = true` 를
  config 플래그(예: `bHeadless`)로 토글화하면 된다.

## 검증

- standalone(-game) 실행 후:
  - `curl http://127.0.0.1:8081/cgi-bin/control/ptzf_status.cgi?action=getptzfpos` → PTZ 위치 응답
  - `curl http://127.0.0.1:8081/cgi-bin/image/jpeg.cgi -o snap.jpg` → 실렌더 JPEG
  - MJPEG 연속 스트림은 `:8091` (MediaMTX runOnDemand 입력).

## 단일 소스 원칙

이 저장소가 CCTV 시뮬 C++ 의 **유일한 진실 소스**다.
`baro_unreal`(실사 개발용)과 `baroQuantum`(심플 프레임워크/시연용)은 둘 다 이 플러그인을 submodule 로 소비한다.
버그픽스/기능은 여기서만 수정하고, 각 프로젝트는 submodule 포인터를 갱신한다.

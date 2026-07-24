# Memo

## 목차

- [제품 기준선](#제품-기준선)
- [기본 설정값](#기본-설정값)
- [런타임 구조 메모](#런타임-구조-메모)
- [동작 규칙](#동작-규칙)
- [반복 금지](#반복-금지)

## 제품 기준선

- Unreal Engine 5.8 Runtime 플러그인.
- 현재 플러그인 버전: **0.1.9** (`baroCCTVSimulator.uplugin` `VersionName` — **단일 출처**).
  이 문서에 버전을 적을 때는 반드시 `.uplugin`을 확인하고 쓴다. 0.1.2~0.1.5 동안 이 줄이 0.1.1로
  방치돼 `/scene/catalog.pluginVersion`(실제값)과 어긋나 있었다.
- 검증 플랫폼: **Windows / Win64 전용**(2026-07-22 기준). 코드 자체는 크로스플랫폼 UE API만 쓰지만,
  Linux(Vulkan 오프스크린)에서는 VT 피드백이 동작하지 않아 주차면 라인 데칼이 렌더되지 않는 **화질 손상**이
  있어 소비 프로젝트가 Windows 전용으로 간다. Linux 지원 재개 전에는 비-VT 데칼 교체가 선행돼야 한다.
- 소비 프로젝트: `baro_unreal`, `baroQuantum` 등에서 `Plugins/baroCCTVSimulator` submodule로 사용.

## 기본 설정값

- Hucoms 제어 포트는 소비 프로젝트 `Config/DefaultGame.ini`의 `[/Script/baroCCTVSimulator.HucomsServerSubsystem]`에서 설정.
- 씬 제어 API 포트는 `[/Script/baroCCTVSimulator.SceneControlSubsystem] ScenePort`(기본 8095).

## 런타임 구조 메모

- `UHucomsServerSubsystem`: 카메라별 Hucoms CGI/MJPEG 서버.
- `USceneControlSubsystem`: `/scene/*` 런타임 씬 제어 API. 차량 스폰/편집/삭제, 주차면 조회, 카메라 파라미터, 투영 오라클을 제공.
- `BaroSimHUD`: `.uplugin` VersionName을 읽어 런타임 HUD에 표시.

## 동작 규칙

- **렌더 자원은 "쓰는 카메라만"(v0.1.9~).** 캡처/PTZ 이동이 수요 신호이고, 상태 폴링(`getptzfpos`·`capabilityptz`)은 **수요가 아니다**. 새 카메라를 쓰면 `MaxActiveCameras`(기본 1)를 넘긴 다른 카메라가, 마지막 사용 후 `IdleReleaseSeconds`(기본 30)가 지나면 그 카메라도 꺼진다. 단 **MJPEG 클라이언트가 붙은 채널과 `MinWarmSeconds`(기본 5) 이내에 쓴 채널은 축출 금지** — 이 유예가 없으면 여러 카메라를 번갈아 쓰는 소비자에서 껐다 켜는 churn(실측 173회/분)이 나 오히려 더 무겁다.
- `/scene/slots`: `id=GetName()`은 안정적인 런타임 식별자, `label=GetActorLabel()`은 사람이 보는 에디터 표시명이다. 웹 UI는 label을 표시하되 RPC는 id를 사용한다.
- 플러그인 수정 시 `VersionName` patch를 올리고, 소비 프로젝트에서 풀 빌드해 WorldSubsystem 변경을 반영한다.

## 반복 금지

- **"클라이언트 없음" HUD 표시를 유휴의 근거로 삼지 않는다.** 그 카운터는 MJPEG 클라이언트만 센다 — jpeg.cgi 폴링(검출기 등)은 안 잡힌다. 2026-07-24 에 DGX uvicorn 이 6대를 상시 폴링하는데도 "클라이언트 없음, 캡처 0"으로 보여 시뮬 성능 문제로 오인했다. v0.1.9 HUD 의 켜짐/꺼짐 표시를 볼 것.
- **HTTP keep-alive 연결은 원격 포트가 안 바뀐다** — "원격 포트 불변 = 요청 없음"으로 판단하지 말 것(같은 진단에서 오판했다). 실제 사용 여부는 GPU 사용률이나 서버 측 유휴 해제 로그로 확인한다.
- 프론트에서 `BP_ParkingSlot_C_*` 같은 런타임 이름을 하드코딩하거나 표시명으로 추정하지 않는다.
- `UWorldSubsystem` 변경은 Live Coding에 기대지 않는다. 에디터 종료 후 `Build.bat <Project>Editor Win64 Development` 풀 빌드로 확인한다.

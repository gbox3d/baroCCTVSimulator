# Inventory

## 목차

- [Repository](#repository)
- [Top-level structure](#top-level-structure)
- [Entrypoints and key modules](#entrypoints-and-key-modules)
- [Build and validation commands](#build-and-validation-commands)
- [Tests](#tests)
- [Notes](#notes)

## Repository

- Name: `baroCCTVSimulator`
- Path: `C:\works\ue_prjs\baroCCTVSimulator`
- Summary: Unreal Engine 5.8 Runtime 플러그인. PTZ 카메라 짐벌, Hucoms 호환 HTTP/MJPEG 서버, 시뮬레이터 HUD, `/scene/*` 런타임 씬 제어 API를 제공한다. 현재 버전 **0.1.1**.

## Top-level structure

- `baroCCTVSimulator.uplugin` — 플러그인 메타데이터, `VersionName` 단일 출처.
- `Source/baroCCTVSimulator/Public/` — 공개 헤더.
- `Source/baroCCTVSimulator/Private/` — 구현.
- `docs/` — 통합/개발자 문서.

## Entrypoints and key modules

- `HucomsServerSubsystem.*` — Hucoms CGI/MJPEG 서버.
- `SceneControlSubsystem.*` — `/scene/catalog`, `/scene/slots`, `/scene/cameras`, `/scene/project`, `/scene/cars`, `/scene/reset`.
- `PTZCamera.*`, `PTZCaptureComponent.*` — PTZ 짐벌과 SceneCapture JPEG.
- `BaroSimHUD.*` — 시뮬레이터 HUD와 플러그인 버전 표시.

## Build and validation commands

- 소비 프로젝트에서 에디터를 닫고 풀 빌드:
  ```powershell
  & "C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" `
    baro_unrealEditor Win64 Development -Project="C:\works\ue_prjs\baro_unreal\baro_unreal.uproject" -WaitMutex -NoHotReload
  ```

## Tests

- 2026-07-08: `baro_unrealEditor Win64 Development` 풀 빌드 성공.

## Notes

- `SceneControlSubsystem`은 `IPluginManager`를 통해 `.uplugin` `VersionName`을 `/scene/catalog.pluginVersion`에 노출한다.
- `/scene/slots`는 `id=GetName()`, `label=GetActorLabel()`을 함께 제공한다.

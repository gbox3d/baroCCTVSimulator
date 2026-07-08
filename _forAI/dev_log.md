# Dev Log

## 목차

- [Entries](#entries)

## Entries

- 2026-07-08: **Scene Control 슬롯 라벨 계약 + v0.1.1.**
  - `/scene/slots`가 주차면 액터의 안정 ID(`GetName()`)와 에디터 표시명(`GetActorLabel()`)을 함께 반환하도록 정리. `id`는 RPC/점유 추적용, `label`은 웹 UI 표시용이다.
  - `baroCCTVSimulator.uplugin` `VersionName`을 **0.1.1**로 올림. 이 값은 `/scene/catalog.pluginVersion`과 `BaroSimHUD` 제목줄에 그대로 노출된다.
  - `baro_unrealEditor Win64 Development` 풀 빌드로 WorldSubsystem/플러그인 변경 반영 확인.

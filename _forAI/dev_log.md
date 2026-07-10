# Dev Log

## 목차

- [Entries](#entries)

## Entries

- 2026-07-10: **v0.1.3 — 캡처 VT 페이지 스로틀 해제(주차면 라인 데칼 미렌더 해결).**
  - `PTZCaptureComponent::EnsureSetup`의 캡처 컴포넌트에 `bOverrideVirtualTextureThrottle=true`. 이 플러그인의 표준 구조(메인 뷰포트 렌더 OFF + SceneCapture 전용)에서는 SVT 텍스처의 VT 피드백이 스로틀에 막혀 VT 샘플링 머티리얼(Megascans _VT 데칼 등)이 쿡 빌드에서 부팅 복불복/오프스크린 상시로 투명 렌더된다. Windows D3D12 완치(클린부팅 6/6). Vulkan 오프스크린은 VT 피드백 자체가 무동작이라 이 플래그로도 미해결(호스트 프로젝트 dev_log 2026-07-10 플랜 B 참조).
  - v0.1.2(같은 날 오전): `HandleCatalog`가 BP_Car.Mesh_List를 CDO 리플렉션으로 읽어 차종 `cars[]{index,name,asset}` 반환. carType 클램프도 배열 길이 기반(`ClampCarType`).
- 2026-07-08: **Scene Control 슬롯 라벨 계약 + v0.1.1.**
  - `/scene/slots`가 주차면 액터의 안정 ID(`GetName()`)와 에디터 표시명(`GetActorLabel()`)을 함께 반환하도록 정리. `id`는 RPC/점유 추적용, `label`은 웹 UI 표시용이다.
  - `baroCCTVSimulator.uplugin` `VersionName`을 **0.1.1**로 올림. 이 값은 `/scene/catalog.pluginVersion`과 `BaroSimHUD` 제목줄에 그대로 노출된다.
  - `baro_unrealEditor Win64 Development` 풀 빌드로 WorldSubsystem/플러그인 변경 반영 확인.

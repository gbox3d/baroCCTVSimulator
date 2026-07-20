# Dev Log

## 목차

- [Entries](#entries)

## Entries

- 2026-07-20: **v0.1.6 — persistent ViewState 를 HWRT Lumen 가용 시에만 허용(UE5.8 SW Lumen 캡처 누수 근본 대응).**
  - UE 5.8 엔진 결함: 소프트웨어 Lumen(SDF 트레이싱) + persistent ViewState 를 가진 SceneCapture 조합에서 캡처 프레임마다 CPU 할당이 회수되지 않는다(LLM 태그 `DistanceFields`, 실측 +1.9~2.1MB/s @30fps 1280x720 — 35시간 가동 시 가상 72GiB OOM, 2026-07-16 현장). 라디언스캐시·스크린프로브 템포럴·서피스캐시 피드백·GDF 재캐시 억제·브릭 아틀라스 확대 cvar 전부 무효(A/B 10회 실측). Lumen GI off 또는 ViewState 제거 시에만 소멸.
  - v0.1.5(persist 무조건 off)는 누수는 잡지만 ViewState 가 없으면 캡처에서 Lumen 자체가 비활성이라 암부가 뭉개진다(clipLo 15.2%→18.4% 실측) — 화질 회귀로 대체.
  - 수정: `baro.Capture.PersistRenderingState` cvar(기본 1) + 가드 — `GRHISupportsRayTracing && r.Lumen.HardwareRayTracing` 일 때만 `bAlwaysPersistRenderingState=true`(HWRT 경로는 누수 원천인 SDF/GDF 프레임 갱신을 쓰지 않음). `bUseRayTracingIfEnabled=true` 필수(`r.RayTracing.SceneCaptures` 기본 -1 은 컴포넌트 설정을 따름). Build.cs 에 `RHI` Private 의존 추가.
  - 실측(호스트 baro_unreal Development 패키지, RTX): HWRT+persist **+0.053MB/s**(누수 소멸) + 암부 clipLo 14.8%(구 SW persist 15.2% 대비 개선, 선명도 동등). SW 폴백(가드 발동, persist auto-off) **-0.343MB/s**. 소비 호스트는 `DefaultEngine.ini` 에 `r.Lumen.HardwareRayTracing=True` 를 설정해야 화질 모드가 켜진다 — RT 미지원 GPU 는 자동으로 안전 모드(v0.1.5 동작)로 강등.
- 2026-07-20: **v0.1.5 — 캡처 persistent ViewState 비활성(누수 1차 대응 — v0.1.6 으로 대체).** `bAlwaysPersistRenderingState=false`. 누수는 제거되나 캡처 Lumen 소실로 암부 화질이 저하되어 같은 날 v0.1.6 가드 방식으로 대체됐다.
- 2026-07-14: **v0.1.4 — setcenter 조준을 tan+구면 기하로 정정.** (소급 기록. 실기 텔레메트리 역산으로 줌별 초점거리 배율 정합 — 호스트 baro_calory 센터링 오차 실측 종결 작업의 sim 측 반영.)
- 2026-07-10: **v0.1.3 — 캡처 VT 페이지 스로틀 해제(주차면 라인 데칼 미렌더 해결).**
  - `PTZCaptureComponent::EnsureSetup`의 캡처 컴포넌트에 `bOverrideVirtualTextureThrottle=true`. 이 플러그인의 표준 구조(메인 뷰포트 렌더 OFF + SceneCapture 전용)에서는 SVT 텍스처의 VT 피드백이 스로틀에 막혀 VT 샘플링 머티리얼(Megascans _VT 데칼 등)이 쿡 빌드에서 부팅 복불복/오프스크린 상시로 투명 렌더된다. Windows D3D12 완치(클린부팅 6/6). Vulkan 오프스크린은 VT 피드백 자체가 무동작이라 이 플래그로도 미해결(호스트 프로젝트 dev_log 2026-07-10 플랜 B 참조).
  - v0.1.2(같은 날 오전): `HandleCatalog`가 BP_Car.Mesh_List를 CDO 리플렉션으로 읽어 차종 `cars[]{index,name,asset}` 반환. carType 클램프도 배열 길이 기반(`ClampCarType`).
- 2026-07-08: **Scene Control 슬롯 라벨 계약 + v0.1.1.**
  - `/scene/slots`가 주차면 액터의 안정 ID(`GetName()`)와 에디터 표시명(`GetActorLabel()`)을 함께 반환하도록 정리. `id`는 RPC/점유 추적용, `label`은 웹 UI 표시용이다.
  - `baroCCTVSimulator.uplugin` `VersionName`을 **0.1.1**로 올림. 이 값은 `/scene/catalog.pluginVersion`과 `BaroSimHUD` 제목줄에 그대로 노출된다.
  - `baro_unrealEditor Win64 Development` 풀 빌드로 WorldSubsystem/플러그인 변경 반영 확인.

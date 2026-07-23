// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "IHttpRouter.h"          // IHttpRouter, FHttpRequestHandler, FHttpRouteHandle
#include "HttpResultCallback.h"   // FHttpResultCallback
#include "HucomsServerSubsystem.generated.h"

class APTZCamera;
class UPTZCaptureComponent;
class FMjpegStreamServer;
struct FHttpServerRequest;

/**
 * FHucomsChannel — 한 APTZCamera = 한 Hucoms 서버 채널.
 *
 * 각 카메라를 자기 고유 포트에 독립 Hucoms CGI 서버(HTTP) + 연속 MJPEG 스트림(TCP)으로 노출한다.
 * 채널은 자기만의 "정준 PTZ 상태(raw Hucoms 단위)"를 소유하고 그 카메라에만 미러링한다.
 * baro_calory 의 devices.list[].{host,port} 와 채널이 1:1 이라, 클라이언트가 카메라별로 개별 접속한다.
 */
struct FHucomsChannel
{
	TWeakObjectPtr<APTZCamera> Camera;
	int32 HttpPort = 0;
	int32 MjpegPort = 0;

	// --- HTTP CGI 라우터 ---
	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> RouteHandles;

	// --- 정준 PTZ 상태 (raw Hucoms 단위) = 이 카메라의 와이어 진실 소스 ---
	int32 CurPan = 0, CurTilt = 0, CurZoom = 0, CurFocus = 0;
	int32 TgtPan = 0, TgtTilt = 0, TgtZoom = 0, TgtFocus = 0;
	bool bPtEnable = true, bZfEnable = true;

	// 고정형 카메라(APTZCamera::bFixedMode 복사). true 면 이 채널은 goptzfpos/setcenter 명령과
	// 모터 슬루를 무시하고 설치 자세로 고정한다(스트림/스냅샷은 정상). capabilityptz 는 PTZ 미지원 광고.
	bool bFixed = false;

	// --- 연속 MJPEG 스트림 서버(RTSP 브리지 입력) ---
	FMjpegStreamServer* Stream = nullptr;
	float StreamAccum = 0.f;

	// --- 스트림 송신 fps 실측 (1초 창) — HUD 표시용 ---
	float FpsWindowAccum = 0.f;
	int32 FpsWindowFrames = 0;
	float MeasuredStreamFps = 0.f;
};

/**
 * UHucomsServerSubsystem — 인엔진 "Hucoms PTZ CCTV 행세" 서버 (멀티 카메라)
 *
 * 목표: baro_calory(Node) 가 sim 의 IP 로 접속해도 실기와 동일하게 동작하도록, Hucoms HTTP CGI 표면을
 * UE 안에서 그대로 응답한다. **레벨의 각 APTZCamera 마다 자기 포트에 독립 서버(채널)를 띄운다.**
 *   - GET /cgi-bin/control/ptzf_status.cgi   (getptzfpos / goptzfpos / getptzstatus / setptzstatus / lensreset)
 *   - GET /cgi-bin/control/ptz_centering.cgi (action=setcenter, type=point|box) - 조준의 핵심
 *   - GET /cgi-bin/control/capabilityptz.cgi (action=getPTZ)
 *   - GET /cgi-bin/image/jpeg.cgi            (활성 카메라 실렌더 JPEG, 실패 시 4바이트 스텁)
 *   - GET /cgi-bin/image/mjpeg.cgi           (단일 프레임 멀티파트 / 연속 스트림은 별도 TCP 포트)
 *
 * 포트 부여: 카메라의 HucomsHttpPort / HucomsMjpegPort 가 >0 이면 그 값을, 0 이면
 *   BaseHttpPort/BaseMjpegPort + (카메라 인덱스) 로 자동 부여한다. baro_calory devices[].port 와 맞출 것.
 *
 * 설계 핵심 (fidelity): 채널이 정준 PTZ 상태를 소유(Hucoms 정수 단위) → getptzfpos 라운드트립 정확.
 *   모터 슬루를 Tick 에서 시뮬, APTZCamera 는 그 current 를 미러링(SnapToTarget).
 *   setcenter 는 TAN 핀홀 역투영 + 현재 틸트를 포함한 구면 짐벌 기하를 사용.
 *
 * 수명: 게임/PIE 월드의 BeginPlay 에 채널 서버들 시작, Deinitialize 에 정지.
 */
UCLASS(config = Game)
class BAROCCTVSIMULATOR_API UHucomsServerSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	//==================================================================================
	// Config (DefaultGame.ini 의 [/Script/baroCCTVSimulator.HucomsServerSubsystem] 로 오버라이드 가능)
	//==================================================================================

	/** 자동 포트 부여 시작값(HTTP CGI). 카메라 HucomsHttpPort=0 이면 BaseHttpPort + 카메라 인덱스. */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Server")
	int32 BaseHttpPort = 8081;

	/** 자동 포트 부여 시작값(연속 MJPEG). 카메라 HucomsMjpegPort=0 이면 BaseMjpegPort + 카메라 인덱스. */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Server")
	int32 BaseMjpegPort = 8091;

	/**
	 * wide 프리셋(zoompos 0)에서의 수평 FOV(deg). 실측값 57.14 (cam-001, 2026-07-14).
	 * 옛 값 69.88 은 "픽셀↔각도는 선형"이라는 틀린 가정으로 역산된 유물이라 22% 넓었다.
	 * 세로 FOV 는 별도 상수를 두지 않는다 — rectilinear 광학에서 종횡비로 유도되는 값이고,
	 * 옛 WideVFovDeg(30.48) 역시 같은 선형 모델의 유물이었다(그래서 세로가 30%나 빗나갔다).
	 */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Optics")
	float WideHFovDeg = 57.14f;

	/**
	 * setcenter 가 쓰는 초점거리 배율. 1.0 = 기하학적으로 정확한 카메라(기본).
	 *
	 * 실기 펌웨어는 기하는 맞게 풀지만 자기가 믿는 초점거리가 실제 렌즈와 어긋나서 조준이
	 * 빗나간다(줌에 따라 0.99~1.11, 망원 포화 구간에선 0.75 — 즉 과회전). 그 결함을 시뮬에서
	 * 일부러 재현하고 싶을 때(예: 클릭 보정 파이프라인을 실카메라 없이 검증) 이 값을 바꾼다.
	 * 시뮬을 '정확한 카메라'로 쓰는 평상시에는 1.0 으로 둔다.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Optics")
	float SetCenterFocalGain = 1.f;

	/** Pan 모터 슬루 속도 (centi-degree/sec). 9000 = 90deg/s. */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Motor")
	float PanSlewCdPerSec = 9000.f;

	/** Tilt 모터 슬루 속도 (centi-degree/sec). 6000 = 60deg/s. */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Motor")
	float TiltSlewCdPerSec = 6000.f;

	/** Zoom 슬루 속도 (raw tick/sec). */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Motor")
	float ZoomSlewPerSec = 30000.f;

	/** Hucoms pan(centi-deg) -> UE Yaw 부호 (화면 방향 보정용, 라운드트립과 무관). */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Mapping")
	float PanToYawSign = 1.f;

	/**
	 * Hucoms tilt(centi-deg) -> UE Pitch 부호.
	 * 실기 규약(fov-convert.mjs, cam-001 필드검증): higher tiltpos = 카메라가 '아래'를 봄.
	 * UE 는 +Pitch = '위'. 따라서 tiltpos↑ 를 UE 에서 아래로 만들려면 -1 로 부호를 뒤집는다.
	 * 이 값은 '실기와 같은 절대 방향으로 렌더'하기 위한 것 — 함부로 +1 로 바꾸면 sim 이 실기와
	 * 상하 반대로 렌더되어 절대 tiltpos 명령/프리셋/호밍이 전부 뒤집힌다. 조작 방향은 setcenter/UI 에서 처리.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Mapping")
	float TiltToPitchSign = -1.f;

	/** jpeg.cgi 스냅샷 가로 해상도. QHD(2560x1440) — 굶은 4K 보다 TSR 로 수렴된 QHD 가 더 선명하고
	 *  VRAM 여유가 있다(현재 RT 지오메트리 예산 초과 경고 있음). 센터링 논리프레임(1920x1080)과 무관 —
	 *  클릭 좌표는 클라이언트가 naturalWidth 기준으로 보내고 서버에서 1920 로 스케일된다.
	 *  (DefaultGame.ini [/Script/baroCCTVSimulator.HucomsServerSubsystem] 로 오버라이드 가능. VRAM 해결 후 4K 상향 가능.) */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Capture", meta = (ClampMin = "64"))
	int32 SnapshotWidth = 2560;

	/** jpeg.cgi 스냅샷 세로 해상도 (QHD = 1440). */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Capture", meta = (ClampMin = "64"))
	int32 SnapshotHeight = 1440;

	/** JPEG 품질 (1~100). */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Capture", meta = (ClampMin = "1", ClampMax = "100"))
	int32 JpegQuality = 92;

	/** 스냅샷 캡처 시 TSR 히스토리 워밍업 프레임 수. **실측 결과 0(단발)이 가장 선명**하고 워밍업은
	 *  오히려 소프트닝(연속 CaptureScene 이 정지프레임을 재블렌딩) + GPU 낭비라 0 권장. (라플라시안 분산
	 *  N0=1358 > N8=1174.) 선명도 핵심은 워밍업이 아니라 TAA-on + Lumen(PTZCaptureComponent). */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Capture", meta = (ClampMin = "0", ClampMax = "32"))
	int32 SnapshotWarmupFrames = 0;

	/** 캡처 노출 보정(EV). SceneCapture 자동노출이 뷰포트보다 밝게 잡혀 "희게 뜨는" 것을 음수로 낮춘다.
	 *  뷰포트 톤 실측 정합 결과 -0.7 이 최적(캡처 mean 159→126, 목표 122). DefaultGame.ini 로 무리빌드 튜닝 가능. */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Capture")
	float CaptureExposureBias = -0.7f;

	/** 캡처 컬러 대비(1=기본). 캡처 대비(std)가 뷰포트보다 낮아 밋밋 → 상향. 실측 정합 결과 1.6 (std 47→61, 목표 69). */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Capture", meta = (ClampMin = "0.5", ClampMax = "3.0"))
	float CaptureContrast = 1.6f;

	/** 레벨에 APTZCamera 가 하나도 없을 때 기본 카메라를 자동 생성할지(자율 검증용). */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Camera")
	bool bAutoSpawnCameraIfNone = true;

	/** 연속 MJPEG 스트림 서버(RTSP 브리지 입력) 활성화. 채널마다 자기 MJPEG 포트에 하나씩. */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Stream")
	bool bEnableMjpegStream = true;

	/** 스트림 프레임레이트(캡처/송신 상한). */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Stream", meta = (ClampMin = "1", ClampMax = "60"))
	int32 StreamFps = 15;

	/** 스트림 가로 해상도(스냅샷과 별개, FOV 동일 유지하며 경량화). */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Stream", meta = (ClampMin = "64"))
	int32 StreamWidth = 1280;

	/** 스트림 세로 해상도. */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Stream", meta = (ClampMin = "64"))
	int32 StreamHeight = 720;

	/** 스트림 JPEG 품질. */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Stream", meta = (ClampMin = "1", ClampMax = "100"))
	int32 StreamJpegQuality = 80;

	//==================================================================================
	// USubsystem / UWorldSubsystem / FTickableGameObject
	//==================================================================================
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	//==================================================================================
	// 진단/테스트용 (BlueprintCallable)
	//==================================================================================

	/** 현재 채널(카메라/포트/PTZ) 요약 문자열. */
	UFUNCTION(BlueprintCallable, Category = "Hucoms")
	FString DebugStateString() const;

	/** 활성 채널 수(=서빙 중인 카메라 수). */
	UFUNCTION(BlueprintPure, Category = "Hucoms")
	int32 GetChannelCount() const { return Channels.Num(); }

	/** 채널별 상태 한 줄씩(카메라·포트·실측 스트림 fps·클라이언트 수) — sim HUD 표시용. */
	TArray<FString> GetChannelStatusLines() const;

	/**
	 * 이 카메라에 실제로 바인딩된 Hucoms 포트(HTTP/MJPEG)를 반환. 채널이 바인딩한 실효 포트가
	 * 진실의 출처라, SceneControl 의 /scene/cameras 가 이 값으로 baro_calory device.port 와 조인한다
	 * (자동 포트 부여 규칙을 다른 서브시스템이 중복 계산하지 않도록). 채널 없으면 false.
	 */
	bool GetCameraPorts(const APTZCamera* Cam, int32& OutHttpPort, int32& OutMjpegPort) const;

private:
	bool bServersStarted = false;
	bool bAutoSpawnAttempted = false;

	// 카메라별 채널 (TSharedPtr 로 안정된 포인터 → 라우트 핸들러 람다가 캡처).
	TArray<TSharedPtr<FHucomsChannel>> Channels;

	void StartServers();
	void StopServers();

	/** 레벨의 APTZCamera(bServeHucoms) 들을 열거해 채널을 만든다(포트 부여 + 카메라 sim 설정). */
	void BuildChannels();

	void ConfigureCameraForSim(APTZCamera* Cam);
	void MirrorChannel(FHucomsChannel& Ch);

	/** 카메라의 캡처 컴포넌트를 찾거나 생성. 없으면 nullptr. */
	UPTZCaptureComponent* ResolveCapture(APTZCamera* Cam);

	/** 채널의 카메라를 렌더해 JPEG 바이트를 채운다. 실패 시 false(호출부가 스텁 폴백). */
	bool RenderSnapshotJpeg(FHucomsChannel& Ch, TArray<uint8>& OutBytes);

	// --- CGI 핸들러 (채널별) ---
	bool HandlePtzfStatus(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandlePtzCentering(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleCapabilityPtz(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleJpeg(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleMjpeg(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);

	/**
	 * 캡처 튜닝(노출/대비/JPEG품질/워밍업/해상도) 조회·설정 — Hucoms 실기 프로토콜에는 없는
	 * 시뮬레이터 전용 디버그 API. 쿼리 파라미터로 준 항목만 갱신하고, 항상 현재 전체 상태를
	 * JSON으로 반환한다. 값은 전 채널(카메라) 공유 — 어느 포트로 호출해도 동일하게 적용된다
	 * (RenderSnapshotJpeg 가 이 subsystem 멤버를 매 캡처마다 읽으므로 재빌드 불필요, 즉시 반영).
	 * UI는 이 서브시스템에 두지 않는다 — webUI(baro_calory) 쪽 책임, 여긴 이미지 생성만.
	 */
	bool HandleTuning(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);

	// --- 명령 적용 (채널별) ---
	void ApplyGoPtz(FHucomsChannel& Ch, const FHttpServerRequest& Req);
	void ApplySetCenter(FHucomsChannel& Ch, const FHttpServerRequest& Req);
	FString BuildPtzPosBody(const FHucomsChannel& Ch) const;
};

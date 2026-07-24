// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PTZCaptureComponent.generated.h"

class APTZCamera;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;

/**
 * UPTZCaptureComponent — APTZCamera 시점을 온디맨드로 렌더해 JPEG 로 인코딩.
 *
 * Hucoms 시뮬레이터의 jpeg.cgi / mjpeg.cgi 가 실제 프레임을 반환하도록 쓰인다.
 * 기존 CenteringClientComponent 의 검증된 캡처 경로(SceneCapture2D + RenderTarget,
 * SCS_FinalColorLDR, FImageUtils)를 재사용 가능한 형태로 추출한 것.
 *
 * 소유자는 APTZCamera 여야 한다(CameraComp 에 캡처를 부착).
 * CaptureJpeg() 는 동기 렌더+리드백(렌더 명령 플러시 = 한 프레임 히치)이며 게임스레드에서 호출.
 */
UCLASS(ClassGroup = (PTZ), meta = (BlueprintSpawnableComponent))
class BAROCCTVSIMULATOR_API UPTZCaptureComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPTZCaptureComponent();

	/**
	 * 현재 카메라 시점을 Width x Height 로 렌더해 JPEG 로 인코딩한다(동기).
	 * 캡처 FOV 는 소유 카메라의 현재 FieldOfView 를 따라간다(줌 반영).
	 * @return 성공 시 true, OutJpeg 에 JPEG 바이트. 카메라/렌더 실패 시 false.
	 */
	bool CaptureJpeg(int32 Width, int32 Height, int32 Quality, TArray64<uint8>& OutJpeg, int32 WarmupFrames = 0, float ExposureBias = 0.f, float Contrast = 1.f);

	/**
	 * 캡처 뷰를 준비한다 — SceneCapture/RT 확보(크기별) + FOV/LOD/노출/대비 세팅 + TextureTarget 바인딩.
	 * 동기(CaptureJpeg)와 비동기 스트림 제출이 같은 세팅을 쓰게 하는 공용 진입점(품질 동일성 보장).
	 * @return 이 캡처가 그릴 렌더 타겟. 실패 시 nullptr.
	 */
	UTextureRenderTarget2D* PrepareCapture(int32 Width, int32 Height, float ExposureBias, float Contrast);

	/** 준비된 뷰로 씬 1회 렌더 큐잉(CaptureScene). PrepareCapture 성공 후 호출. */
	void RenderOnce();

	/**
	 * 렌더 자원(SceneCapture2D + RenderTarget)을 즉시 반납한다 — "카메라를 끈다".
	 *
	 * 이게 없으면 한 번이라도 캡처된 카메라는 EndPlay 까지 persistent ViewState(Lumen/TSR/노출
	 * 히스토리)와 RT 상주를 영구히 물고 있어, 아무도 안 보는 카메라가 GPU 를 계속 태운다.
	 * DestroyComponent 는 OnUnregister 를 태워 ViewState 를 즉시 파괴하는 엔진 공식 경로다.
	 * 다음 캡처 시 PrepareCapture 가 전부 null 가드라 자동으로 재생성된다(콜드 시작).
	 */
	void ReleaseCaptureResources();

	/** 렌더 자원이 현재 살아있는가(= 켜져 있는가). 콜드 재생성 판정용. */
	bool HasCaptureResources() const { return CaptureComp != nullptr; }

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** 소유 카메라 (APTZCamera). */
	UPROPERTY()
	TObjectPtr<APTZCamera> OwnerCam;

	/** 런타임 생성 씬 캡처 (CameraComp 에 부착). */
	UPROPERTY()
	TObjectPtr<USceneCaptureComponent2D> CaptureComp;

	/**
	 * 크기별 렌더 타겟(실질 최대 2개: 스트림 1280x720 / 스냅샷 2560x1440).
	 * v0.1.9 까지는 단일 RT 를 리사이즈로 공유해 스트리밍 중 스냅샷 1회마다 720p↔1440p
	 * 재할당이 왕복했고("Gamethread hitch waiting for resource cleanup" 경고 실측),
	 * 비동기 in-flight 리드백의 크기 메타데이터도 오염 위험이 있어 크기별로 분리했다.
	 * 비용은 warm 카메라당 +3.7MB 로 무시 가능.
	 */
	UPROPERTY()
	TArray<TObjectPtr<UTextureRenderTarget2D>> RenderTargets;

	/** SceneCapture 컴포넌트를 (필요 시) 생성. 성공 시 true. */
	bool EnsureCaptureComp();

	/** 크기가 일치하는 RT 를 찾거나 생성. */
	UTextureRenderTarget2D* FindOrCreateRT(int32 Width, int32 Height);
};

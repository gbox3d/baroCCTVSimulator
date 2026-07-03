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
class BAROSIM_API UPTZCaptureComponent : public UActorComponent
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

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** 소유 카메라 (APTZCamera). */
	UPROPERTY()
	TObjectPtr<APTZCamera> OwnerCam;

	/** 런타임 생성 씬 캡처 (CameraComp 에 부착). */
	UPROPERTY()
	TObjectPtr<USceneCaptureComponent2D> CaptureComp;

	/** 캡처 대상 렌더 타겟. */
	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> RenderTarget;

	/** 현재 RT 해상도 (변경 시 재할당). */
	int32 RtWidth = 0;
	int32 RtHeight = 0;

	/** SceneCapture/RenderTarget 을 (필요 시) 생성/리사이즈. 성공 시 true. */
	bool EnsureSetup(int32 Width, int32 Height);
};

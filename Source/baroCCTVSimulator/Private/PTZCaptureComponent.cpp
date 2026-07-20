// Fill out your copyright notice in the Description page of Project Settings.

#include "PTZCaptureComponent.h"

#include "PTZCamera.h"

#include "Camera/CameraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Scene.h"   // FPostProcessSettings, EDynamicGlobalIlluminationMethod, EReflectionMethod
#include "ImageUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogPTZCapture, Log, All);

UPTZCaptureComponent::UPTZCaptureComponent()
{
	// 캡처는 요청 시에만 수행(온디맨드) — Tick 불필요.
	PrimaryComponentTick.bCanEverTick = false;
}

bool UPTZCaptureComponent::EnsureSetup(int32 Width, int32 Height)
{
	if (!OwnerCam)
	{
		OwnerCam = Cast<APTZCamera>(GetOwner());
	}
	if (!OwnerCam || !OwnerCam->CameraComp)
	{
		UE_LOG(LogPTZCapture, Error, TEXT("[PTZCapture] 소유자가 CameraComp 를 가진 APTZCamera 가 아님: %s"),
			*GetNameSafe(GetOwner()));
		return false;
	}

	// 렌더 타겟: 8비트 BGRA + sRGB (bForceLinearGamma=false). SCS_FinalColorLDR 와 짝.
	if (!RenderTarget || RtWidth != Width || RtHeight != Height)
	{
		if (!RenderTarget)
		{
			RenderTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("HucomsCaptureRT"));
			RenderTarget->ClearColor = FLinearColor::Black;
		}
		RenderTarget->InitCustomFormat(Width, Height, PF_B8G8R8A8, /*bInForceLinearGamma=*/false);
		RtWidth = Width;
		RtHeight = Height;
	}

	// 씬 캡처: 런타임 생성이므로 NewObject -> RegisterComponent -> AttachToComponent.
	if (!CaptureComp)
	{
		CaptureComp = NewObject<USceneCaptureComponent2D>(OwnerCam, TEXT("HucomsCapture"));
		CaptureComp->RegisterComponent();
		CaptureComp->AttachToComponent(OwnerCam->CameraComp,
			FAttachmentTransformRules::SnapToTargetNotIncludingScale);
		CaptureComp->SetRelativeLocationAndRotation(FVector::ZeroVector, FRotator::ZeroRotator);

		CaptureComp->ProjectionType = ECameraProjectionMode::Perspective;
		// FinalColorLDR = 포스트프로세스+톤맵 거친 8비트 sRGB = 실제 CCTV 인코더가 보는 그림.
		CaptureComp->CaptureSource = SCS_FinalColorLDR;
		CaptureComp->bCaptureEveryFrame = false;   // 요청 시에만
		CaptureComp->bCaptureOnMovement = false;
		// UE 5.8에서 Lumen SceneCapture의 persistent ViewState를 켜면 캡처 중 프로세스
		// 물리 메모리가 1.2~1.9 MB/s로 계속 증가한다. 캡처/JPEG/소켓을 분리한 A/B 검증에서
		// 이 플래그를 끈 경우만 정상 기울기(-0.3 MB/s)로 복귀했으므로 재발 방지를 위해 끈다.
		CaptureComp->bAlwaysPersistRenderingState = false;
		// 버추얼 텍스처(SVT) 페이지 스트리밍은 렌더 픽셀의 피드백으로 굴러가는데, 이 sim 은
		// bDisableWorldRendering 으로 메인 뷰포트를 끄고 SceneCapture 만 돌린다. 캡처의 VT
		// 피드백은 스로틀에 막혀 페이지가 안 올라오고(부팅별 복불복·-RenderOffscreen 은 상시),
		// VT 데칼(주차면 라인 MI_Decal_Line_Road_White_02 등)만 투명해진다(2026-07-09 실측:
		// r.VirtualTextures=0 강제 시 동일 증상 재현). 엔진이 캡처 전용 시나리오용으로 제공하는
		// 스로틀 해제 플래그가 정답이다.
		CaptureComp->bOverrideVirtualTextureThrottle = true;
		// 정지 사진에 팬 모션블러가 섞이면 가독성만 해친다.
		CaptureComp->ShowFlags.SetMotionBlur(false);
		// 선명도(적대적 검증 결론): 단발 SceneCapture 는 뷰포트의 TSR 시간축 누적(초해상도+샤픈)이
		// 없어 "프레임 0"처럼 소프트하다. 예전엔 TemporalAA 를 껐는데, 그건 TSR→FXAA 블러 경로로
		// 강제해 오히려 더 뿌옇게 만든 오답이었다. AA 플래그는 유지하되, persistent ViewState는
		// 위 메모리 누수 때문에 사용하지 않는다.
		CaptureComp->ShowFlags.SetTemporalAA(true);
		CaptureComp->ShowFlags.SetAntiAliasing(true);
		// SceneCapture2D 기본 PostProcess 는 GI/Reflection=None 이라 Lumen fidelity 를 잃는다 → 명시 오버라이드.
		CaptureComp->PostProcessSettings.bOverride_DynamicGlobalIlluminationMethod = true;
		CaptureComp->PostProcessSettings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::Lumen;
		CaptureComp->PostProcessSettings.bOverride_ReflectionMethod = true;
		CaptureComp->PostProcessSettings.ReflectionMethod = EReflectionMethod::Lumen;
	}
	CaptureComp->TextureTarget = RenderTarget;
	return true;
}

bool UPTZCaptureComponent::CaptureJpeg(int32 Width, int32 Height, int32 Quality, TArray64<uint8>& OutJpeg, int32 WarmupFrames, float ExposureBias, float Contrast)
{
	if (!EnsureSetup(Width, Height))
	{
		return false;
	}

	// 광학 줌 동기화 — 씬캡처 FOV 는 카메라를 자동으로 따라가지 않는다. 매 캡처 직전 필수.
	// (Hucoms 서버가 MirrorToCamera 에서 카메라 FOV 를 HFOV(zoompos) 로 맞춰둔다.)
	CaptureComp->FOVAngle = OwnerCam->CameraComp->FieldOfView;

	// 줌 보정 LOD: 메시 LOD 는 화면 크기 기반이라 줌(좁은 FOV)을 반영하지만, 폴리지/인스턴스
	// 컬링·페이드는 순수 거리 기반이라 줌을 모른다. 캡처 뷰의 LOD 거리 계산을 줌 배율만큼
	// 당겨(팩터 < 1) 원거리 소품이 저LOD/컬링으로 뭉개지는 것을 막는다.
	CaptureComp->LODDistanceFactor = FMath::Clamp(CaptureComp->FOVAngle / FMath::Max(1.f, OwnerCam->BaseFOV), 0.05f, 1.f);

	// 톤 정합: SceneCapture 자동노출/대비가 뷰포트와 달라 "희게 뜨는"(과노출+저대비) 것을 보정한다.
	// 뷰포트 실측(mean 108, contrast 72) 대비 캡처(159, 47)가 밝고 밋밋 → 노출 낮추고 대비 올림.
	CaptureComp->PostProcessSettings.bOverride_AutoExposureBias = true;
	CaptureComp->PostProcessSettings.AutoExposureBias = ExposureBias;
	CaptureComp->PostProcessSettings.bOverride_ColorContrast = true;
	CaptureComp->PostProcessSettings.ColorContrast = FVector4(Contrast, Contrast, Contrast, 1.0);

	// 캡처 워밍업: 노출/Lumen 리소스가 첫 readback 전에 안정될 시간을 준다. UE 5.8 Lumen
	// ViewState 누수를 막기 위해 요청 간 temporal history는 보존하지 않는다.
	const int32 Frames = FMath::Max(1, WarmupFrames + 1);
	for (int32 i = 0; i < Frames; ++i)
	{
		CaptureComp->CaptureScene();
	}

	// GetRenderTargetImage 는 내부에서 ReadPixels(렌더 명령 플러시 = 한 프레임 히치)를 수행하고
	// RT 의 감마 공간(sRGB)을 FImage 에 태깅 -> CompressImage 까지 감마 처리 자동.
	FImage Image;
	if (!FImageUtils::GetRenderTargetImage(RenderTarget, Image))
	{
		UE_LOG(LogPTZCapture, Error, TEXT("[PTZCapture] GetRenderTargetImage 실패"));
		return false;
	}
	if (!FImageUtils::CompressImage(OutJpeg, TEXT("jpg"), Image, Quality))
	{
		UE_LOG(LogPTZCapture, Error, TEXT("[PTZCapture] JPEG 인코드 실패"));
		return false;
	}
	return true;
}

void UPTZCaptureComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (CaptureComp)
	{
		CaptureComp->DestroyComponent();
		CaptureComp = nullptr;
	}
	RenderTarget = nullptr; // GC 회수
	Super::EndPlay(EndPlayReason);
}

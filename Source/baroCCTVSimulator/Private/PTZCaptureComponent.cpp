// Fill out your copyright notice in the Description page of Project Settings.

#include "PTZCaptureComponent.h"

#include "PTZCamera.h"

#include "Camera/CameraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Scene.h"   // FPostProcessSettings, EDynamicGlobalIlluminationMethod, EReflectionMethod
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "RHIGlobals.h"     // GRHISupportsRayTracing

DEFINE_LOG_CATEGORY_STATIC(LogPTZCapture, Log, All);

static TAutoConsoleVariable<int32> CVarBaroCapturePersistRenderingState(
	TEXT("baro.Capture.PersistRenderingState"), 1,
	TEXT("PTZ 캡처 SceneCapture 의 persistent ViewState 허용 여부.\n")
	TEXT(" 1(기본): HWRT Lumen 이 실제 가용할 때만 켠다 — SW Lumen 은 UE5.8 DistanceFields 누수로 항상 끔\n")
	TEXT(" 0: 항상 끔"),
	ECVF_Default);

namespace
{
	// UE 5.8 소프트웨어 Lumen(SDF 트레이싱)은 persistent ViewState 를 가진 SceneCapture 에서 캡처
	// 프레임마다 CPU 메모리를 회수하지 않는다(LLM 태그 DistanceFields, 실측 ~1.9MB/s @30fps 720p —
	// 라디언스캐시·템포럴·서피스캐시 피드백·브릭 아틀라스 크기 cvar 전부 무관, Lumen GI off 또는
	// ViewState 제거 시에만 소멸. 2026-07-20 A/B 10회 실측). ViewState 를 끄면 누수는 없지만 Lumen
	// 시간축 수렴이 사라져 암부가 뭉개진다(clipLo 15%→18% 실측). HWRT Lumen 경로는 누수 원천인
	// SDF/GDF 프레임 갱신을 쓰지 않으므로, HWRT 가 실제 가용한 경우에만 persist 를 허용한다.
	bool ShouldPersistCaptureRenderingState()
	{
		if (CVarBaroCapturePersistRenderingState.GetValueOnGameThread() == 0)
		{
			return false;
		}
		static const IConsoleVariable* LumenHWRT =
			IConsoleManager::Get().FindConsoleVariable(TEXT("r.Lumen.HardwareRayTracing"));
		return GRHISupportsRayTracing && LumenHWRT && LumenHWRT->GetInt() != 0;
	}
}

UPTZCaptureComponent::UPTZCaptureComponent()
{
	// 캡처는 요청 시에만 수행(온디맨드) — Tick 불필요.
	PrimaryComponentTick.bCanEverTick = false;
}

UTextureRenderTarget2D* UPTZCaptureComponent::FindOrCreateRT(int32 Width, int32 Height)
{
	for (const TObjectPtr<UTextureRenderTarget2D>& RT : RenderTargets)
	{
		if (RT && RT->SizeX == Width && RT->SizeY == Height)
		{
			return RT;
		}
	}
	// 렌더 타겟: 8비트 BGRA + sRGB (bForceLinearGamma=false). SCS_FinalColorLDR 와 짝.
	UTextureRenderTarget2D* NewRT = NewObject<UTextureRenderTarget2D>(this);
	NewRT->ClearColor = FLinearColor::Black;
	NewRT->InitCustomFormat(Width, Height, PF_B8G8R8A8, /*bInForceLinearGamma=*/false);
	RenderTargets.Add(NewRT);
	return NewRT;
}

bool UPTZCaptureComponent::EnsureCaptureComp()
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
		// persist 판정 근거는 상단 ShouldPersistCaptureRenderingState() 주석 참조(UE5.8 SW Lumen 누수).
		CaptureComp->bAlwaysPersistRenderingState = ShouldPersistCaptureRenderingState();
		// r.RayTracing.SceneCaptures 기본값(-1)은 컴포넌트 설정을 따른다 — HWRT Lumen 캡처에 필수.
		CaptureComp->bUseRayTracingIfEnabled = true;
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
	return true;
}

UTextureRenderTarget2D* UPTZCaptureComponent::PrepareCapture(int32 Width, int32 Height, float ExposureBias, float Contrast)
{
	if (!EnsureCaptureComp())
	{
		return nullptr;
	}
	UTextureRenderTarget2D* RT = FindOrCreateRT(Width, Height);
	if (!RT)
	{
		return nullptr;
	}
	CaptureComp->TextureTarget = RT;

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

	return RT;
}

void UPTZCaptureComponent::RenderOnce()
{
	if (CaptureComp)
	{
		CaptureComp->CaptureScene();
	}
}

bool UPTZCaptureComponent::CaptureJpeg(int32 Width, int32 Height, int32 Quality, TArray64<uint8>& OutJpeg, int32 WarmupFrames, float ExposureBias, float Contrast)
{
	UTextureRenderTarget2D* RT = PrepareCapture(Width, Height, ExposureBias, Contrast);
	if (!RT)
	{
		return false;
	}

	// 캡처 워밍업: 노출/Lumen 리소스가 첫 readback 전에 안정될 시간을 준다. UE 5.8 Lumen
	// ViewState 누수를 막기 위해 요청 간 temporal history는 보존하지 않는다.
	const int32 Frames = FMath::Max(1, WarmupFrames + 1);
	for (int32 i = 0; i < Frames; ++i)
	{
		CaptureComp->CaptureScene();
	}

	// GetRenderTargetImage 는 내부에서 ReadPixels(렌더 명령 플러시 = 한 프레임 히치)를 수행하고
	// RT 의 감마 공간(sRGB)을 FImage 에 태깅 -> CompressImage 까지 감마 처리 자동.
	// 참고: 이 동기 경로는 스냅샷(jpeg.cgi) 전용이다. 연속 스트림은 서브시스템의 비동기
	// 파이프라인(FRHIGPUTextureReadback — 게임 스레드 무정지)을 쓴다. ReadPixels 는 내부에서
	// 디바이스 전역 GPU 드레인을 수행하므로(실측: 6대 스트림 중 26.8ms+ 블로킹) 고빈도 호출 금지.
	FImage Image;
	if (!FImageUtils::GetRenderTargetImage(RT, Image))
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

void UPTZCaptureComponent::ReleaseCaptureResources()
{
	if (!CaptureComp && RenderTargets.Num() == 0)
	{
		return; // 이미 꺼져 있음 — 매 틱 호출돼도 무해.
	}

	if (CaptureComp)
	{
		// DestroyComponent -> OnUnregister 가 persistent ViewState(Lumen/TSR/노출 히스토리)를
		// 즉시 파괴한다. 이게 유휴 카메라가 GPU 를 붙잡는 주 원인이었다.
		CaptureComp->DestroyComponent();
		CaptureComp = nullptr;
	}
	for (const TObjectPtr<UTextureRenderTarget2D>& RT : RenderTargets)
	{
		if (RT)
		{
			// GC 를 기다리지 않고 RT 의 GPU 메모리를 즉시 반납.
			RT->ReleaseResource();
		}
	}
	RenderTargets.Reset();

	UE_LOG(LogPTZCapture, Verbose, TEXT("[PTZCapture] 렌더 자원 반납: %s"), *GetNameSafe(GetOwner()));
}

void UPTZCaptureComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ReleaseCaptureResources();
	Super::EndPlay(EndPlayReason);
}

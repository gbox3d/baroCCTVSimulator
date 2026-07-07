// Fill out your copyright notice in the Description page of Project Settings.


#include "PTZCamera.h"

#include "Camera/CameraComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
// FMath 헬퍼는 CoreMinimal.h 로 이미 포함됨(별도 Math 헤더 불필요).

// Sets default values
APTZCamera::APTZCamera()
{
	// 매 프레임 Tick() 호출 - PTZ 보간을 위해 반드시 필요.
	PrimaryActorTick.bCanEverTick = true;

	//------------------------------------------------------------------
	// 컴포넌트 계층 구성 (생성자에서는 SetupAttachment 만 사용. AttachToComponent 금지!)
	// Root -> [BodyMesh] / PanPivot -> [HeadMesh] / TiltPivot -> CameraComp
	//------------------------------------------------------------------

	// 고정 루트
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	// 고정 몸체 메시 (선택 - 메시는 에디터에서 지정). 충돌 비활성으로 비간섭.
	BodyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BodyMesh"));
	BodyMesh->SetupAttachment(Root);
	BodyMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BodyMesh->SetCollisionProfileName(TEXT("NoCollision"));

	// PAN(Yaw) 피벗
	PanPivot = CreateDefaultSubobject<USceneComponent>(TEXT("PanPivot"));
	PanPivot->SetupAttachment(Root);

	// 회전 헤드 메시 (선택 - 메시는 에디터에서 지정). PanPivot 에 부착되어 함께 회전.
	HeadMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HeadMesh"));
	HeadMesh->SetupAttachment(PanPivot);
	HeadMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HeadMesh->SetCollisionProfileName(TEXT("NoCollision"));

	// TILT(Pitch) 피벗 - PanPivot 의 자식이므로 Pan 회전을 따라간다.
	TiltPivot = CreateDefaultSubobject<USceneComponent>(TEXT("TiltPivot"));
	TiltPivot->SetupAttachment(PanPivot);

	// 카메라 (멤버/서브오브젝트 이름은 BP 의 'Camera' 와 겹치지 않도록 CameraComp)
	CameraComp = CreateDefaultSubobject<UCameraComponent>(TEXT("CameraComp"));
	CameraComp->SetupAttachment(TiltPivot);
	// 정적 CCTV: 컨트롤러 회전을 더하지 않는다(PTZ 피벗이 방향을 결정).
	CameraComp->bUsePawnControlRotation = false;
	// 원근 투영이어야 광학 줌(FOV) 이 의미를 가진다.
	CameraComp->SetProjectionMode(ECameraProjectionMode::Perspective);

	// 뷰 타겟일 때 활성 UCameraComponent 를 자동 사용하도록 명시(기본 true 지만 방어적으로).
	bFindCameraComponentWhenViewTarget = true;
}

// Called when the game starts or when spawned
void APTZCamera::BeginPlay()
{
	Super::BeginPlay();

	// 에디터에서 한계 값이 뒤집혀 입력된 경우(Min>Max) 보정.
	// (FMath::Clamp 는 Min>Max 시 항상 Max 를 반환해 범위가 한 점으로 붕괴됨)
	if (PanMin > PanMax) { Swap(PanMin, PanMax); }
	if (TiltMin > TiltMax) { Swap(TiltMin, TiltMax); }
	MinZoomFactor = FMath::Max(1.f, MinZoomFactor);
	if (MinZoomFactor > MaxZoomFactor) { Swap(MinZoomFactor, MaxZoomFactor); }

	// 목표값을 한계 내로 정리하고, 시작 시 현재값을 목표값에 맞춘 뒤 즉시 적용.
	TargetPan = FMath::Clamp(TargetPan, PanMin, PanMax);
	TargetTilt = FMath::Clamp(TargetTilt, TiltMin, TiltMax);
	TargetZoomFactor = FMath::Clamp(TargetZoomFactor, MinZoomFactor, MaxZoomFactor);

	SnapToTarget();
}

// Called every frame
void APTZCamera::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// 고정형 모드: 모터 보간 없이 현재 자세 유지. 설치 자세는 BeginPlay 의 SnapToTarget(또는
	// 서버의 MirrorChannel->SnapToTarget)으로 이미 컴포넌트에 반영돼 있어, 이후 Tick 은 아무것도
	// 하지 않아도 그 자세로 고정된다. (조향 API 로 Target 이 바뀌어도 Current 는 움직이지 않는다.)
	if (bFixedMode)
	{
		return;
	}

	// 이미 목표에 도달해 정지 상태면 매 프레임 갱신을 건너뛴다.
	// (주차장에 다수의 CCTV 가 있을 때 불필요한 회전/렌더 dirty 연산 절감)
	const bool bMoving =
		!FMath::IsNearlyEqual(CurrentPan, TargetPan) ||
		!FMath::IsNearlyEqual(CurrentTilt, TargetTilt) ||
		!FMath::IsNearlyEqual(CurrentZoomFactor, TargetZoomFactor);
	if (!bMoving)
	{
		return;
	}

	// Pan/Tilt: 일정 속도(deg/s) 모터 보간 - 실제 PTZ 모터 느낌.
	CurrentPan = FMath::FInterpConstantTo(CurrentPan, TargetPan, DeltaTime, PanSpeed);
	CurrentTilt = FMath::FInterpConstantTo(CurrentTilt, TargetTilt, DeltaTime, TiltSpeed);

	// Zoom: 부드러운 ease-out 보간(서보 느낌).
	CurrentZoomFactor = FMath::FInterpTo(CurrentZoomFactor, TargetZoomFactor, DeltaTime, ZoomSpeed);

	// 보간된 현재값을 컴포넌트에 적용.
	ApplyToComponents();
}

//======================================================================================
// Control API
//======================================================================================

void APTZCamera::SetPan(float NewPanDegrees)
{
	// 입력(target)만 clamp. current 는 절대 clamp 하지 않음.
	TargetPan = FMath::Clamp(NewPanDegrees, PanMin, PanMax);
}

void APTZCamera::SetTilt(float NewTiltDegrees)
{
	TargetTilt = FMath::Clamp(NewTiltDegrees, TiltMin, TiltMax);
}

void APTZCamera::SetPanTilt(float NewPanDegrees, float NewTiltDegrees)
{
	SetPan(NewPanDegrees);
	SetTilt(NewTiltDegrees);
}

void APTZCamera::SetZoomFactor(float NewZoomFactor)
{
	// 0 나눗셈 방지: MinZoomFactor 는 항상 >= 1.0 (헤더 ClampMin) 이지만 한 번 더 보장.
	const float SafeMin = FMath::Max(1.f, MinZoomFactor);
	TargetZoomFactor = FMath::Clamp(NewZoomFactor, SafeMin, FMath::Max(SafeMin, MaxZoomFactor));
}

void APTZCamera::AddPan(float DeltaDegrees)
{
	SetPan(TargetPan + DeltaDegrees);
}

void APTZCamera::AddTilt(float DeltaDegrees)
{
	SetTilt(TargetTilt + DeltaDegrees);
}

void APTZCamera::AddZoom(float DeltaFactor)
{
	SetZoomFactor(TargetZoomFactor + DeltaFactor);
}

void APTZCamera::SnapToTarget()
{
	// 보간 없이 즉시 목표값으로.
	CurrentPan = TargetPan;
	CurrentTilt = TargetTilt;
	CurrentZoomFactor = TargetZoomFactor;
	ApplyToComponents();
}

//======================================================================================
// Getters
//======================================================================================

float APTZCamera::GetCurrentFOV() const
{
	return ZoomFactorToFOV(CurrentZoomFactor);
}

//======================================================================================
// View
//======================================================================================

void APTZCamera::ActivateView(float BlendTime)
{
	// 로컬 플레이어(인덱스 0)의 PlayerController 를 가져와 이 액터를 뷰 타겟으로 설정.
	APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
	if (PC)
	{
		PC->SetViewTargetWithBlend(
			this,
			BlendTime,
			EViewTargetBlendFunction::VTBlend_Cubic,
			0.f,
			false);
	}
}

//======================================================================================
// Internal helpers
//======================================================================================

float APTZCamera::ZoomFactorToFOV(float ZoomFactor) const
{
	// 광학 줌: 카메라는 이동하지 않고 FOV 만 변경.
	// FOV = 2 * atan( tan(BaseFOV/2) / ZoomFactor )
	const float SafeFactor = FMath::Max(1.f, ZoomFactor); // 0/음수 방어
	const float HalfBaseRad = FMath::DegreesToRadians(BaseFOV) * 0.5f;
	const float NewHalfRad = FMath::Atan(FMath::Tan(HalfBaseRad) / SafeFactor);
	const float FOV = FMath::RadiansToDegrees(2.f * NewHalfRad);
	// 극단적으로 좁은/넓은 FOV 로 인한 투영 정밀도/컬링 이상 방지.
	return FMath::Clamp(FOV, 1.f, 170.f);
}

void APTZCamera::ApplyToComponents()
{
	// PAN = Yaw. 팬은 반드시 '월드 수직축(중력)' 기준으로 돈다 — 실제 팬-틸트 헤드와 동일.
	// PanPivot 의 '월드' 회전을 yaw-only(Pitch=Roll=0)로 강제해, 액터를 기울여(벽면 마운트 등)
	// 설치해도 팬 축이 따라 기울지 않는다. => 어떤 설치 자세에서도 팬 시 지평선이 롤되지 않는다.
	// 액터의 설치 '방향(Yaw)'만 취하고, 액터의 Pitch/Roll 은 광학축에 넣지 않는다(상하 조준은 Tilt 전담).
	if (PanPivot)
	{
		const float BaseYaw = GetActorRotation().Yaw; // 설치 heading 만 사용 (pitch/roll 무시)
		PanPivot->SetWorldRotation(FRotator(0.f, BaseYaw + CurrentPan, 0.f));
	}

	// TILT = Pitch 를 TiltPivot 에. 부모(PanPivot)가 월드-수평이라 상대 pitch 는 항상 수평
	// 우측축 기준으로 돌아 지평선을 유지한다(롤 0).
	if (TiltPivot)
	{
		TiltPivot->SetRelativeRotation(FRotator(CurrentTilt, 0.f, 0.f));
	}

	// 광학 줌: 런타임에는 FieldOfView 를 직접 쓰지 않고 SetFieldOfView() 로 갱신
	// (렌더 상태를 dirty 처리해 즉시 반영).
	if (CameraComp)
	{
		CameraComp->SetFieldOfView(ZoomFactorToFOV(CurrentZoomFactor));
	}
}

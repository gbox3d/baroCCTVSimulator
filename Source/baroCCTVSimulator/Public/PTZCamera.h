// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PTZCamera.generated.h"

// Forward declarations (keep the header light; full includes live in the .cpp)
class USceneComponent;
class UCameraComponent;
class UStaticMeshComponent;
class APlayerController;

/**
 * APTZCamera
 *
 * 주차장 CCTV 시뮬레이터용 PTZ(Pan / Tilt / Zoom) 카메라 액터.
 * A parking-lot CCTV PTZ camera actor.
 *
 * 설계 (Design):
 *  - PAN  = Yaw  (수평 회전) -> PanPivot 에 적용.
 *  - TILT = Pitch(수직 회전) -> TiltPivot 에 적용.
 *  - ZOOM = 광학 줌(Optical). 카메라는 이동하지 않고 FieldOfView 만 변경.
 *           ZoomFactor(1x..MaxZoomFactor) 를 FOV 로 매핑한다.
 *  - 매 Tick 마다 Current 값이 Target 값을 향해 보간(모터처럼)되며,
 *    Pan/Tilt 는 일정 속도(deg/s), Zoom 은 줌 단위/s 로 움직인다.
 *
 * 컴포넌트 계층 (Component hierarchy):
 *   Root(USceneComponent, RootComponent)
 *     -> BodyMesh   (UStaticMeshComponent, 고정 몸체 - 선택)
 *     -> PanPivot   (USceneComponent, Yaw 회전)
 *          -> HeadMesh  (UStaticMeshComponent, 회전 헤드 - 선택)
 *          -> TiltPivot (USceneComponent, Pitch 회전)
 *               -> CameraComp (UCameraComponent)
 *
 * 뷰 타겟 (View target):
 *   AActor 의 기본 CalcCamera 가 활성 UCameraComponent 를 자동으로 사용하므로
 *   (bFindCameraComponentWhenViewTarget == true) 별도 Pawn 이나 CalcCamera 재정의가 필요 없다.
 *   ActivateView() 를 호출하면 로컬 플레이어가 이 카메라를 통해 보게 된다.
 *
 * 입력 경로 (Input path):
 *   MainUI(UMG) 위젯이 BlueprintCallable 함수(SetPan/SetTilt/SetZoomFactor 등)를 호출한다.
 *
 * 이름 변경 메모:
 *   구 이름은 "ABP_PTZCamera"(BP_ 접두사 오기). 현재 정식 이름은 "APTZCamera".
 *   자식 BP(BP_TestCCTV1)가 구 이름을 부모로 참조하므로, DefaultEngine.ini 의
 *   [CoreRedirects] 에 BP_PTZCamera -> PTZCamera 매핑을 두어 깨지지 않게 했다.
 *
 * 주의(이름 충돌 회피):
 *   네이티브 카메라 멤버 이름을 "Camera" 가 아닌 "CameraComp" 로 둔 이유 -
 *   기존 자식 BP(BP_TestCCTV1)가 직접 추가한 "Camera" 컴포넌트와 변수명이 겹치는 것을 막기 위함.
 *   자식 BP 가 직접 추가한 Camera/DefaultSceneRoot/Arrow 는 삭제하고 네이티브 계층만 사용할 것.
 */
UCLASS()
class BAROCCTVSIMULATOR_API APTZCamera : public AActor
{
	GENERATED_BODY()

public:
	// Sets default values for this actor's properties
	APTZCamera();

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	//==================================================================================
	// Components
	//==================================================================================

	/** 고정 루트. 이 액터의 위치/기준. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PTZ|Components")
	TObjectPtr<USceneComponent> Root;

	/** 고정 몸체(벽 마운트 등). 메시는 에디터에서 선택 지정. (선택, 비필수) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PTZ|Components")
	TObjectPtr<UStaticMeshComponent> BodyMesh;

	/** PAN(Yaw) 피벗. 수평 회전을 담당. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PTZ|Components")
	TObjectPtr<USceneComponent> PanPivot;

	/** 회전하는 헤드 메시. PanPivot 에 부착. 메시는 에디터에서 선택 지정. (선택, 비필수) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PTZ|Components")
	TObjectPtr<UStaticMeshComponent> HeadMesh;

	/** TILT(Pitch) 피벗. 수직 회전을 담당. (Pan/Tilt 를 분리해 짐벌 커플링 방지) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PTZ|Components")
	TObjectPtr<USceneComponent> TiltPivot;

	/** 실제 카메라. 뷰 타겟일 때 플레이어가 이 카메라를 통해 본다. (BP 의 'Camera' 와 충돌 회피용 이름) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PTZ|Components")
	TObjectPtr<UCameraComponent> CameraComp;

	//==================================================================================
	// Tunables - Limits (기계적 한계, 입력 set 시 target 을 clamp)
	//==================================================================================

	/** Pan(Yaw) 최소 각도 (deg). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PTZ|Limits")
	float PanMin = -180.f;

	/** Pan(Yaw) 최대 각도 (deg). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PTZ|Limits")
	float PanMax = 180.f;

	/** Tilt(Pitch) 최소 각도 (deg). 보통 아래로 더 많이 내려감. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PTZ|Limits")
	float TiltMin = -90.f;

	/** Tilt(Pitch) 최대 각도 (deg). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PTZ|Limits")
	float TiltMax = 30.f;

	/** 최소 줌 배율. 분모로 쓰이므로 1.0 이상 강제(0 나눗셈 방지). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PTZ|Limits", meta = (ClampMin = "1.0"))
	float MinZoomFactor = 1.f;

	/** 최대 줌 배율 (예: 30x). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PTZ|Limits", meta = (ClampMin = "1.0"))
	float MaxZoomFactor = 30.f;

	//==================================================================================
	// Tunables - Speed (모터 속도)
	//==================================================================================

	/** Pan 모터 속도 (deg/s). 일정 속도 보간(FInterpConstantTo). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PTZ|Speed", meta = (ClampMin = "0.0"))
	float PanSpeed = 60.f;

	/** Tilt 모터 속도 (deg/s). 일정 속도 보간(FInterpConstantTo). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PTZ|Speed", meta = (ClampMin = "0.0"))
	float TiltSpeed = 45.f;

	/** 줌 보간 속도. FInterpTo 의 InterpSpeed(단위 없는 비율) 로 사용 - 부드러운 서보 느낌. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PTZ|Speed", meta = (ClampMin = "0.0"))
	float ZoomSpeed = 4.f;

	//==================================================================================
	// Tunables - Optics (광학)
	//==================================================================================

	/** 1x 일 때(가장 넓은) 기준 수평 FOV (deg). FOV = 2*atan(tan(BaseFOV/2)/factor). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PTZ|Optics", meta = (ClampMin = "1.0", ClampMax = "170.0"))
	float BaseFOV = 90.f;

	//==================================================================================
	// Hucoms 서버 노출 (카메라별 포트) — UHucomsServerSubsystem 이 읽는다.
	//   각 카메라가 자기 포트에 독립 Hucoms CGI 서버로 노출된다. baro_calory 의
	//   devices.list[].{host,port} 와 1:1 로 맞추면 카메라별 개별 접속이 된다.
	//==================================================================================

	/** 이 카메라를 Hucoms 서버로 노출할지. false 면 서버 채널을 만들지 않는다. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PTZ|Hucoms")
	bool bServeHucoms = true;

	/** Hucoms HTTP CGI 포트. 0 = 자동(BaseHttpPort + 카메라 인덱스). baro_calory device.port 와 일치시킬 것. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PTZ|Hucoms", meta = (ClampMin = "0", ClampMax = "65535"))
	int32 HucomsHttpPort = 0;

	/** 연속 MJPEG 스트림 포트(RTSP 브리지 입력). 0 = 자동(BaseMjpegPort + 카메라 인덱스). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PTZ|Hucoms", meta = (ClampMin = "0", ClampMax = "65535"))
	int32 HucomsMjpegPort = 0;

	//==================================================================================
	// State - Targets (UI 가 설정하는 목표 값)
	//   EditAnywhere 로 에디터 초기값은 지정 가능하되 BlueprintReadOnly 로 두어,
	//   UMG 가 raw 프로퍼티를 직접 써서 clamp 를 우회하는 것을 막는다(SetPan/SetTilt/SetZoomFactor 사용).
	//==================================================================================

	/** 목표 Pan(Yaw) 각도 (deg). SetPan 등으로 clamp 후 저장. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PTZ|State")
	float TargetPan = 0.f;

	/** 목표 Tilt(Pitch) 각도 (deg). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PTZ|State")
	float TargetTilt = 0.f;

	/** 목표 줌 배율 (1x..MaxZoomFactor). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "PTZ|State")
	float TargetZoomFactor = 1.f;

	//==================================================================================
	// State - Current (Tick 에서 보간되는 실제 값. Transient - 저장 안 함)
	//==================================================================================

	/** 현재 Pan(Yaw) 각도 (deg). 절대 clamp 하지 않음 - 목표만 clamp. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "PTZ|State")
	float CurrentPan = 0.f;

	/** 현재 Tilt(Pitch) 각도 (deg). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "PTZ|State")
	float CurrentTilt = 0.f;

	/** 현재 줌 배율. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "PTZ|State")
	float CurrentZoomFactor = 1.f;

	//==================================================================================
	// BlueprintCallable API - MainUI(UMG) 위젯이 호출
	//==================================================================================

	/** 목표 Pan 을 절대값으로 설정(clamp). */
	UFUNCTION(BlueprintCallable, Category = "PTZ|Control")
	void SetPan(float NewPanDegrees);

	/** 목표 Tilt 을 절대값으로 설정(clamp). */
	UFUNCTION(BlueprintCallable, Category = "PTZ|Control")
	void SetTilt(float NewTiltDegrees);

	/** 목표 Pan/Tilt 을 한 번에 설정(clamp). */
	UFUNCTION(BlueprintCallable, Category = "PTZ|Control")
	void SetPanTilt(float NewPanDegrees, float NewTiltDegrees);

	/** 목표 줌 배율을 설정(MinZoomFactor..MaxZoomFactor 로 clamp). */
	UFUNCTION(BlueprintCallable, Category = "PTZ|Control")
	void SetZoomFactor(float NewZoomFactor);

	/** 현재 목표 Pan 에 Delta(deg)를 더함(clamp). 조이스틱/누름 버튼용. */
	UFUNCTION(BlueprintCallable, Category = "PTZ|Control")
	void AddPan(float DeltaDegrees);

	/** 현재 목표 Tilt 에 Delta(deg)를 더함(clamp). */
	UFUNCTION(BlueprintCallable, Category = "PTZ|Control")
	void AddTilt(float DeltaDegrees);

	/** 현재 목표 줌 배율에 Delta 를 더함(clamp). */
	UFUNCTION(BlueprintCallable, Category = "PTZ|Control")
	void AddZoom(float DeltaFactor);

	/** 보간 없이 Current 를 Target 으로 즉시 스냅하고 적용. */
	UFUNCTION(BlueprintCallable, Category = "PTZ|Control")
	void SnapToTarget();

	//==================================================================================
	// Getters
	//==================================================================================

	/** 현재 Pan(Yaw) 각도 (deg). */
	UFUNCTION(BlueprintPure, Category = "PTZ|State")
	float GetCurrentPan() const { return CurrentPan; }

	/** 현재 Tilt(Pitch) 각도 (deg). */
	UFUNCTION(BlueprintPure, Category = "PTZ|State")
	float GetCurrentTilt() const { return CurrentTilt; }

	/** 현재 줌 배율. */
	UFUNCTION(BlueprintPure, Category = "PTZ|State")
	float GetCurrentZoomFactor() const { return CurrentZoomFactor; }

	/** 현재 카메라 FOV (deg). */
	UFUNCTION(BlueprintPure, Category = "PTZ|State")
	float GetCurrentFOV() const;

	//==================================================================================
	// View
	//==================================================================================

	/**
	 * 이 액터를 로컬 플레이어(인덱스 0)의 뷰 타겟으로 설정한다.
	 * 호출 후 플레이어는 이 PTZ 카메라를 통해 화면을 본다.
	 * (BP_GM 의 DefaultPawnClass=BP_Pawn 으로 인한 기본 Pawn 시점을 이 호출이 덮어쓴다.)
	 * @param BlendTime  블렌드 시간(초). 0 이면 즉시 전환.
	 */
	UFUNCTION(BlueprintCallable, Category = "PTZ|View")
	void ActivateView(float BlendTime = 0.5f);

private:
	/** ZoomFactor -> FOV(deg) 변환. 광학 줌 공식. */
	float ZoomFactorToFOV(float ZoomFactor) const;

	/** Current 값을 컴포넌트(피벗 회전 + 카메라 FOV)에 적용. */
	void ApplyToComponents();
};

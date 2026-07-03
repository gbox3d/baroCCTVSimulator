// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "PTZPlayerController.generated.h"

class APTZCamera;

/**
 * ActiveCamera 가 바뀔 때 방송되는 델리게이트.
 * UMG 위젯이 바인딩해서 슬라이더/표시 값을 새 카메라 기준으로 재동기화하는 데 사용.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnActiveCameraChanged, APTZCamera*, NewCamera);

/**
 * APTZPlayerController
 *
 * 주차장 CCTV 관제사(Operator) 컨트롤러.
 *
 * 역할:
 *  - "현재 관제 중인 카메라(ActiveCamera)" 의 단일 진실 소스(single source of truth).
 *    화면(ViewTarget)과 PTZ 슬라이더가 항상 같은 카메라를 보도록 보장.
 *  - 카메라 전환은 빙의(Possess)가 아니라 ViewTarget 교체로 처리한다.
 *    Pawn(관제사)은 계속 빙의된 채 입력/UI 를 받고, 화면만 선택한 CCTV 로 바뀐다.
 *
 * 사용(블루프린트):
 *  - BP_Controller 의 부모 클래스를 이 클래스로 reparent.
 *  - WBP_MainUI 의 카메라 선택 동작 -> ViewCamera(Cam) (또는 ViewCameraByIndex).
 *  - WBP_MainUI 의 PTZ 슬라이더 -> ActiveCamera->SetPan/SetTilt/SetZoomFactor.
 *  - OnActiveCameraChanged 에 바인딩해 전환 시 슬라이더 값을 다시 읽어 표시.
 */
UCLASS()
class BAROCCTVSIMULATOR_API APTZPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	APTZPlayerController();

	/** 현재 관제 중(화면에 보이고 PTZ 조종 대상인) 카메라. UI 의 단일 진실 소스. */
	UPROPERTY(BlueprintReadOnly, Category = "CCTV")
	TObjectPtr<APTZCamera> ActiveCamera;

	/** ActiveCamera 가 바뀔 때 방송. UMG 가 바인딩해 슬라이더 값을 재동기화. */
	UPROPERTY(BlueprintAssignable, Category = "CCTV")
	FOnActiveCameraChanged OnActiveCameraChanged;

	/**
	 * 카메라 전환: 이 컨트롤러의 뷰 타겟을 Cam 으로 바꾸고 ActiveCamera 로 기록한 뒤
	 * OnActiveCameraChanged 를 방송한다. Cam 이 null 이면 무시.
	 * @param Cam        전환할 PTZ 카메라.
	 * @param BlendTime  화면 전환 블렌드 시간(초). 0 이면 즉시.
	 */
	UFUNCTION(BlueprintCallable, Category = "CCTV")
	void ViewCamera(APTZCamera* Cam, float BlendTime = 0.5f);

	/**
	 * 레벨에 배치된 모든 PTZCamera 중 Index 번째로 전환한다(없으면 무시).
	 * 빠른 테스트/단일 카메라 선택용. (주의: GetAllActorsOfClass 의 순서는 보장되지 않음)
	 */
	UFUNCTION(BlueprintCallable, Category = "CCTV")
	void ViewCameraByIndex(int32 Index, float BlendTime = 0.5f);
};

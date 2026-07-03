// Fill out your copyright notice in the Description page of Project Settings.


#include "PTZPlayerController.h"

#include "PTZCamera.h"
#include "Kismet/GameplayStatics.h"

APTZPlayerController::APTZPlayerController()
{
	// 관제사는 UI(슬라이더/버튼)를 마우스로 조작하므로 커서를 기본 노출.
	// (필요 없으면 reparent 한 BP 의 Details 에서 끄면 됨)
	bShowMouseCursor = true;
}

void APTZPlayerController::ViewCamera(APTZCamera* Cam, float BlendTime)
{
	if (!Cam)
	{
		return;
	}

	// 1) 단일 진실 소스 갱신.
	ActiveCamera = Cam;

	// 2) 화면(ViewTarget)을 선택 카메라로. 빙의는 그대로 두므로 입력/UI 는 계속 동작.
	SetViewTargetWithBlend(Cam, BlendTime, EViewTargetBlendFunction::VTBlend_Cubic, 0.f, false);

	// 3) UI 가 새 카메라 기준으로 슬라이더 값을 재동기화하도록 방송.
	OnActiveCameraChanged.Broadcast(Cam);
}

void APTZPlayerController::ViewCameraByIndex(int32 Index, float BlendTime)
{
	TArray<AActor*> Found;
	UGameplayStatics::GetAllActorsOfClass(this, APTZCamera::StaticClass(), Found);

	if (Found.IsValidIndex(Index))
	{
		ViewCamera(Cast<APTZCamera>(Found[Index]), BlendTime);
	}
}

// Fill out your copyright notice in the Description page of Project Settings.

#include "BaroSimGameMode.h"

#include "BaroSimHUD.h"
#include "BaroSimPlayerController.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/SpectatorPawn.h"

ABaroSimGameMode::ABaroSimGameMode()
{
	DefaultPawnClass = ASpectatorPawn::StaticClass(); // 비가시 — "플레이어 구체" 제거
	PlayerControllerClass = ABaroSimPlayerController::StaticClass();
	HUDClass = ABaroSimHUD::StaticClass();
}

void ABaroSimGameMode::BeginPlay()
{
	Super::BeginPlay();

	UWorld* World = GetWorld();
	if (!World || World->WorldType != EWorldType::Game)
	{
		return; // PIE: 에디터 뷰포트는 그대로 둔다
	}

	if (UGameViewportClient* Viewport = World->GetGameViewport())
	{
		// 메인 뷰포트 월드 렌더 OFF (HUD/UI 는 계속 그려짐). CCTV 프레임은
		// SceneCapture 가 자체적으로 렌더하므로 스트림/스냅샷에 영향 없다.
		Viewport->bDisableWorldRendering = true;
	}
	if (GEngine)
	{
		GEngine->Exec(World, TEXT("t.MaxFPS 60"));
	}
}

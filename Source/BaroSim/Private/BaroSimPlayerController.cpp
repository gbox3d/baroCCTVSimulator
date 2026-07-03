// Fill out your copyright notice in the Description page of Project Settings.

#include "BaroSimPlayerController.h"

ABaroSimPlayerController::ABaroSimPlayerController()
{
	bShowMouseCursor = true;
}

void ABaroSimPlayerController::BeginPlay()
{
	Super::BeginPlay();

	FInputModeGameAndUI Mode;
	Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	Mode.SetHideCursorDuringCapture(false);
	SetInputMode(Mode);
}

void ABaroSimPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	if (WasInputKeyJustPressed(EKeys::Escape))
	{
		ConsoleCommand(TEXT("quit"));
	}
}

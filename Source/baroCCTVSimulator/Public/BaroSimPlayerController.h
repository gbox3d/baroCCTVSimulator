// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "BaroSimPlayerController.generated.h"

/**
 * ABaroSimPlayerController — CCTV 시뮬레이터 standalone 용 미니멀 컨트롤러.
 *  - 마우스 커서 항상 표시, 뷰포트에 캡처/잠금하지 않음.
 *  - ESC 로 종료(quit). PIE 에서는 에디터가 ESC 를 먼저 소비하므로 영향 없음.
 */
UCLASS()
class BAROCCTVSIMULATOR_API ABaroSimPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	ABaroSimPlayerController();

	virtual void BeginPlay() override;
	virtual void PlayerTick(float DeltaTime) override;
};

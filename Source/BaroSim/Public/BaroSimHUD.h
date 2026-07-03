// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "BaroSimHUD.generated.h"

/**
 * ABaroSimHUD — standalone 검은 화면 위에 타이틀/서버 상태/종료 안내만 그린다.
 * (월드 렌더가 꺼져 있어도 HUD 캔버스는 그려진다.) PIE 에서는 아무것도 그리지 않음.
 */
UCLASS()
class BAROSIM_API ABaroSimHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	/** 게임 틱 fps 지수이동평균(표시 안정화용). */
	float FpsEma = 0.f;
};

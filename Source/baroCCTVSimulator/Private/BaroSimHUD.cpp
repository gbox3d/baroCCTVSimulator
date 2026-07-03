// Fill out your copyright notice in the Description page of Project Settings.

#include "BaroSimHUD.h"

#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "HucomsServerSubsystem.h"

void ABaroSimHUD::DrawHUD()
{
	Super::DrawHUD();

	UWorld* World = GetWorld();
	if (!World || World->WorldType != EWorldType::Game || !GEngine)
	{
		return;
	}

	const UHucomsServerSubsystem* Sub = World->GetSubsystem<UHucomsServerSubsystem>();
	const int32 ChannelCount = Sub ? Sub->GetChannelCount() : 0;
	const int32 HttpBase = Sub ? Sub->BaseHttpPort : 8081;
	const int32 MjpegBase = Sub ? Sub->BaseMjpegPort : 8091;

	const float Dt = FMath::Max(World->GetDeltaSeconds(), 0.0001f);
	FpsEma = (FpsEma <= 0.f) ? (1.f / Dt) : (FpsEma * 0.95f + (1.f / Dt) * 0.05f);

	const float X = 40.f;
	float Y = 40.f;

	DrawText(TEXT("baroCCTVSimulator — CCTV 시뮬레이터"), FLinearColor::White, X, Y, GEngine->GetLargeFont(), 1.6f);
	Y += 48.f;
	DrawText(FString::Printf(TEXT("Hucoms 채널 %d개 서빙 중  ·  HTTP %d~  ·  MJPEG %d~"),
		ChannelCount, HttpBase, MjpegBase), FLinearColor(0.7f, 0.85f, 1.f), X, Y, GEngine->GetMediumFont(), 1.2f);
	Y += 28.f;
	DrawText(FString::Printf(TEXT("게임 틱 %.0f fps  ·  월드 렌더링 OFF — CCTV SceneCapture 만 렌더 중"), FpsEma),
		FLinearColor(0.6f, 0.6f, 0.6f), X, Y, GEngine->GetMediumFont(), 1.2f);
	Y += 36.f;

	if (Sub)
	{
		for (const FString& Line : Sub->GetChannelStatusLines())
		{
			DrawText(Line, FLinearColor(0.55f, 1.f, 0.65f), X, Y, GEngine->GetMediumFont(), 1.2f);
			Y += 26.f;
		}
	}

	Y += 10.f;
	DrawText(TEXT("ESC: 종료"), FLinearColor(1.f, 0.85f, 0.4f), X, Y, GEngine->GetMediumFont(), 1.2f);
}

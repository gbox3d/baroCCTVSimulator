// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "BaroSimGameMode.generated.h"

/**
 * ABaroSimGameMode — CCTV 시뮬레이터 전용 미니멀 게임모드.
 *
 * 이 프로젝트의 standalone(-game) 실행은 "카메라 서버"가 목적이지 플레이가 아니다:
 *  - DefaultPawn(구체 메시) 대신 SpectatorPawn(비가시) 스폰 — 화면에 아무것도 안 보임.
 *  - standalone 에서는 메인 뷰포트의 월드 렌더를 끈다(bDisableWorldRendering).
 *    PTZCaptureComponent 의 SceneCapture 는 자체 씬 렌더라 영향 없음 — 스트림/스냅샷 정상.
 *  - 언캡 게임 틱 방지: t.MaxFPS 60 (스트림 상한 30fps 의 2배 여유 — 틱이 스트림 클록이므로
 *    상한을 너무 바짝 잡으면 accumulator 양자화로 실효 fps 가 떨어진다).
 *  - PIE 에서는 월드 렌더를 끄지 않는다(에디터 작업 화면 유지).
 *
 * DefaultEngine.ini [/Script/EngineSettings.GameMapsSettings] GlobalDefaultGameMode 로 지정.
 * (관제 UI 용 APTZPlayerController/BP_Controller 흐름과는 별개 — 그쪽은 UMG 프로젝트용.)
 */
UCLASS()
class BAROCCTVSIMULATOR_API ABaroSimGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ABaroSimGameMode();

	virtual void BeginPlay() override;
};

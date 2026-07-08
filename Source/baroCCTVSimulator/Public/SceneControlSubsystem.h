// Copyright gbox3d. All Rights Reserved.
//
// USceneControlSubsystem — 시뮬레이터 "씬 편집" 런타임 서버 (월드 전역, 카메라와 별개 축)
//
// baro_calory 의 /api/simulator/* 가 프록시하는 JSON REST 계약(/scene/*)을 UE 안에서 이행한다.
// 배치된 BP_ParkingSlot 액터를 슬롯으로 노출하고, /Game/BP/BP_Car 를 슬롯에 스폰하며,
// 차종/색상/번호판종류/번호판글자를 BP 함수(Change_Car/Change_Color/Change_Plate/Change_Text)를
// ProcessEvent 로 호출해 반영한다(리플렉션 직접설정 대신 BP 로직 재사용).
//
// HucomsServerSubsystem(카메라별 N포트, PTZ, Tick)과 관심사가 다르므로(월드 전역 1포트, CRUD,
// 비틱) 별도 UWorldSubsystem 으로 분리한다. 포트 8095 는 baro_calory config.simulator.port 와 일치.
//
// 계약(엔드포인트):
//   GET    /scene/catalog        차종수·색상·번호판종류·한글목록
//   GET    /scene/slots          주차면(id·label·type·transform·occupied·carId)
//   GET    /scene/cameras        카메라 광학 포즈(CameraComp 월드)+FOV+해상도+PTZ+포트 (오버레이 투영 파라미터)
//   POST   /scene/project        월드점→픽셀 그라운드-트루스(UE 뷰·투영행렬; 웹 오버레이 정합 검증 오라클)
//   GET    /scene/cars           배치된 차량
//   POST   /scene/cars           스폰 {slotId,carType,color,plate}
//   GET|PATCH|DELETE /scene/cars/:id
//   POST   /scene/reset          전체 삭제

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "IHttpRouter.h"          // IHttpRouter, FHttpRequestHandler, FHttpRouteHandle
#include "HttpResultCallback.h"   // FHttpResultCallback
#include "SceneControlSubsystem.generated.h"

struct FHttpServerRequest;

/** 씬에 배치된 차량 1대의 논리 상태 — 응답 재구성 + 스폰 액터 참조. */
struct FSimCarState
{
	FString Id;
	FString SlotId;                 // 빈 문자열 = 자유 좌표 배치(슬롯 없음)
	int32 CarType = 0;              // selected_Car 0..22
	int32 Color = 0;               // selected_Color 0..7
	int32 PlateType = 0;           // selected_Plate 0..2
	FString City, Prefix, Kor, Number;
	FTransform Transform = FTransform::Identity;
	TWeakObjectPtr<AActor> Actor;  // 스폰된 BP_Car (딴 경로로 파괴돼도 dangling 안 됨)
};

UCLASS(config = Game)
class BAROCCTVSIMULATOR_API USceneControlSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	/** 씬 제어 HTTP 리스너 포트. baro_calory config.simulator.port 와 일치시킬 것. */
	UPROPERTY(config, EditAnywhere, Category = "Scene")
	int32 ScenePort = 8095;

	/** 스폰할 차량 블루프린트의 generated class 경로. */
	UPROPERTY(config, EditAnywhere, Category = "Scene")
	FString CarBlueprintPath = TEXT("/Game/BP/BP_Car.BP_Car_C");

	/** 주차면 액터를 식별하는 클래스명 접두(변종 BP_ParkingSlot_5m/6m/BUS/... 공통). */
	UPROPERTY(config, EditAnywhere, Category = "Scene")
	FString ParkingSlotClassPrefix = TEXT("BP_ParkingSlot");

	//==================================================================================
	// UWorldSubsystem
	//==================================================================================
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

private:
	bool bStarted = false;
	int32 CarSeq = 0;

	/** /Game/BP/BP_Car.BP_Car_C 캐시 (UPROPERTY 로 GC 보호). */
	UPROPERTY()
	TSubclassOf<AActor> CarClass;

	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> Routes;

	TMap<FString, FSimCarState> Cars;      // carId  -> 상태
	TMap<FString, FString> SlotOccupancy;  // slotId -> 점유 carId

	void StartServer();
	void StopServer();

	// --- 라우트 핸들러 ---
	bool HandleCatalog(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleSlots(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleCameras(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);   // 카메라 포즈+FOV+포트
	bool HandleProject(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);   // 월드점→픽셀 오라클
	bool HandleCars(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);      // GET list / POST spawn
	bool HandleCarById(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);   // GET / PATCH / DELETE
	bool HandleReset(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);

	// --- 내부 ---
	UClass* ResolveCarClass();
	AActor* SpawnCarActor(const FTransform& Xform);
	void ApplyToActor(AActor* Car, const FSimCarState& S);   // BP setter 호출(Change_Car/Color/Plate/Text)
};

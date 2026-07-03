// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "HttpFwd.h"
#include "CenteringClientComponent.generated.h"

class APTZCamera;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;

/**
 * 추론 결과 1건 (번호판 1개).
 * 좌표계 주의: DPan/DTilt 는 "캡처 시점 자세 기준 델타(deg)" 이며 **데이터셋(실측 CCTV) 좌표계**다.
 * UE 좌표계 적용은 LookAtPlate 가 PanSign/TiltSign 으로 변환해서 수행한다.
 */
USTRUCT(BlueprintType)
struct FCenteringPlate
{
	GENERATED_BODY()

	/** 캡처 자세 기준 pan 델타 (deg, 데이터셋 좌표계). */
	UPROPERTY(BlueprintReadOnly, Category = "Centering")
	float DPan = 0.f;

	/** 캡처 자세 기준 tilt 델타 (deg, 데이터셋 좌표계). */
	UPROPERTY(BlueprintReadOnly, Category = "Centering")
	float DTilt = 0.f;

	/** 카메라→번호판 추정 거리 (m, 절대값). */
	UPROPERTY(BlueprintReadOnly, Category = "Centering")
	float Dist = 0.f;

	/** 목표 줌 (절대값, 데이터셋 범위 ≈ 0~36). */
	UPROPERTY(BlueprintReadOnly, Category = "Centering")
	float Zoom = 0.f;
};

/** 추론 완료. bSuccess=false 면 통신/파싱 실패(Plates 비어 있음). 성공+빈 배열 = 번호판 미검출. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnInferenceResult, bool, bSuccess, const TArray<FCenteringPlate>&, Plates);
/** /health 응답. bReady = (state=="ready"). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnHealthResult, bool, bReady, FString, State);

UENUM(BlueprintType)
enum class ECenteringState : uint8
{
	Idle,
	Capturing,
	Waiting,
};

/**
 * UCenteringClientComponent — VLA centering 관찰 도구 (1차 산출물)
 *
 * 목적: 클로즈드 루프가 아니라 "관찰". PTZ 카메라가 보는 장면을 캡처해
 * baro_vla 추론 서버(POST /centering/lite)로 보내고, 받은 번호판 위치 목록을
 * **저장/표시만** 한다. 카메라 이동은 사용자가 LookAtPlate(Index) 를 호출할 때만,
 * 기존 PTZ 모터 보간(PanSpeed/TiltSpeed)으로 천천히 수행된다.
 * 반복 추론으로 잔차(dpan/dtilt)가 0으로 수렴하는지 관찰하고, 그 데이터를
 * CSV(Saved/Centering/centering_log.csv) 로 누적해 필터 설계의 근거로 삼는다.
 *
 * 소유자는 반드시 APTZCamera (BeginPlay 에서 검증).
 *
 * API 계약 (D:\works\baro_vla\docs\api_reference.md):
 *  - 요청: multipart/form-data — image(JPEG) + current_pan/current_tilt/current_zoom
 *  - 응답: {"datas":[{dpan,dtilt,dist,zoom},...]} — dpan/dtilt 는 캡처 시점 기준 델타,
 *    zoom 은 절대 목표값. 빈 배열 = 미검출/서버측 파싱 실패
 *  - 적용 공식: target = 캡처시점 PTZ + delta (응답 도착 시점의 현재값이 아님!)
 */
UCLASS(ClassGroup = (PTZ), meta = (BlueprintSpawnableComponent))
class BAROCCTVSIMULATOR_API UCenteringClientComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCenteringClientComponent();

	//==================================================================================
	// Config - Server
	//==================================================================================

	/** baro_vla 추론 서버 base URL (끝 슬래시 없이). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Centering|Server")
	FString ServerUrl = TEXT("http://192.168.0.220:8012");

	/** HTTP 타임아웃 (초). 추론은 보통 0.8~2초. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Centering|Server", meta = (ClampMin = "1.0"))
	float TimeoutSec = 30.f;

	//==================================================================================
	// Config - Capture
	//==================================================================================

	/** 캡처 해상도 가로. 서버는 ~1MP 이하 권장(내부 리사이즈). */
	UPROPERTY(EditAnywhere, Category = "Centering|Capture", meta = (ClampMin = "64"))
	int32 CaptureWidth = 1280;

	/** 캡처 해상도 세로. */
	UPROPERTY(EditAnywhere, Category = "Centering|Capture", meta = (ClampMin = "64"))
	int32 CaptureHeight = 720;

	/** JPEG 품질 (1~100). */
	UPROPERTY(EditAnywhere, Category = "Centering|Capture", meta = (ClampMin = "1", ClampMax = "100"))
	int32 JpegQuality = 85;

	//==================================================================================
	// Config - Apply (LookAt)
	//==================================================================================

	/**
	 * Pan 부호 보정: dataset_pan = PanSign * ue_pan.
	 * 전송하는 current 값과 LookAt 적용 delta 양쪽에 대칭 적용된다 (한쪽만 뒤집으면
	 * 프롬프트 컨텍스트와 적용이 서로 다른 좌표계가 됨). PIE 관찰로 경험 보정.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Centering|Apply")
	float PanSign = 1.f;

	/** Tilt 부호 보정. 실측 CCTV 는 아래가 +(추정), UE pitch 는 위가 + 라서 기본 -1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Centering|Apply")
	float TiltSign = -1.f;

	/** LookAt 때 zoom(절대값)도 적용할지. 관찰 1차는 pan/tilt 만 보도록 기본 꺼짐. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Centering|Apply")
	bool bApplyZoomOnLookAt = false;

	//==================================================================================
	// Config - Debug / 관찰 기록
	//==================================================================================

	/** 모든 단계 UE_LOG 출력. */
	UPROPERTY(EditAnywhere, Category = "Centering|Debug")
	bool bDrawDebugLog = true;

	/** 매 캡처를 Saved/Centering/last_capture.jpg 로 저장 (감마/FOV 검증 + curl 재전송용). */
	UPROPERTY(EditAnywhere, Category = "Centering|Debug")
	bool bSaveDebugCapture = false;

	/** 추론/적용 내역을 Saved/Centering/centering_log.csv 로 누적 (관찰 데이터). */
	UPROPERTY(EditAnywhere, Category = "Centering|Debug")
	bool bWriteCsvLog = true;

	//==================================================================================
	// Runtime API
	//==================================================================================

	/**
	 * 캡처 + 전송 + 결과 저장만. 카메라는 움직이지 않는다.
	 * @return busy 거나 초기화 실패 상태면 false.
	 */
	UFUNCTION(BlueprintCallable, Category = "Centering")
	bool RequestInference();

	/**
	 * 마지막 추론 결과의 Index 번 번호판을 천천히 바라보게 한다.
	 * target = 캡처시점 스냅샷 + 부호보정된 delta. 모터 보간은 PTZCamera 가 담당.
	 * @return Index 가 범위 밖이거나 결과가 없으면 false.
	 */
	UFUNCTION(BlueprintCallable, Category = "Centering")
	bool LookAtPlate(int32 Index);

	/** GET /health 비동기 호출. 결과는 OnHealthResult 로 방송. */
	UFUNCTION(BlueprintCallable, Category = "Centering")
	void CheckHealth();

	/** 요청 진행 중 여부. */
	UFUNCTION(BlueprintPure, Category = "Centering")
	bool IsBusy() const { return State != ECenteringState::Idle; }

	/** 마지막 추론 결과 목록 (UI 표시용). */
	UFUNCTION(BlueprintPure, Category = "Centering")
	const TArray<FCenteringPlate>& GetLastPlates() const { return LastPlates; }

	/** 마지막 캡처 시점의 pan (잔차 비교용). */
	UFUNCTION(BlueprintPure, Category = "Centering")
	float GetCapturedPan() const { return CapturedPan; }

	UFUNCTION(BlueprintPure, Category = "Centering")
	float GetCapturedTilt() const { return CapturedTilt; }

	UFUNCTION(BlueprintPure, Category = "Centering")
	float GetCapturedZoom() const { return CapturedZoom; }

	/** 추론 완료 이벤트 (UI 바인딩). */
	UPROPERTY(BlueprintAssignable, Category = "Centering")
	FOnInferenceResult OnInferenceResult;

	/** /health 응답 이벤트. */
	UPROPERTY(BlueprintAssignable, Category = "Centering")
	FOnHealthResult OnHealthResult;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** 소유자 (APTZCamera 만 허용). null 이면 컴포넌트 비활성. */
	UPROPERTY()
	TObjectPtr<APTZCamera> OwnerCam;

	/** 런타임 생성 씬 캡처 (CameraComp 에 부착, FOV 는 매 캡처마다 동기화). */
	UPROPERTY()
	TObjectPtr<USceneCaptureComponent2D> CaptureComp;

	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> RenderTarget;

	ECenteringState State = ECenteringState::Idle;

	/** 캡처 시점 PTZ 스냅샷. LookAt 은 반드시 이 값 기준 (추론 1~2초 동안 카메라가 움직일 수 있음). */
	float CapturedPan = 0.f;
	float CapturedTilt = 0.f;
	float CapturedZoom = 1.f;
	float CapturedFOV = 90.f;

	/** 마지막 추론 결과. */
	TArray<FCenteringPlate> LastPlates;

	/** 세션 내 추론 일련번호 (CSV seq 컬럼). */
	int32 InferenceSeq = 0;

	FHttpRequestPtr PendingRequest;

	// pipeline helpers
	bool CaptureToJpeg(TArray64<uint8>& OutJpeg);
	TArray<uint8> BuildMultipartBody(const FString& Boundary, const TArray64<uint8>& Jpeg,
	                                 float Pan, float Tilt, float Zoom) const;
	void OnInferenceResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully);
	void OnHealthResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully);
	void FinishInference(bool bSuccess);
	void AppendCsvRows(bool bSuccess) const;
	void AppendCsvAppliedRow(int32 Index, const FCenteringPlate& Plate) const;
	FString CsvFilePath() const;
};

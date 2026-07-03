// Fill out your copyright notice in the Description page of Project Settings.


#include "CenteringClientComponent.h"

#include "PTZCamera.h"

#include "Camera/CameraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Dom/JsonObject.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "HttpModule.h"
#include "ImageUtils.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

UCenteringClientComponent::UCenteringClientComponent()
{
	// 이벤트 구동(버튼/콜백)이라 Tick 불필요.
	PrimaryComponentTick.bCanEverTick = false;
}

void UCenteringClientComponent::BeginPlay()
{
	Super::BeginPlay();

	OwnerCam = Cast<APTZCamera>(GetOwner());
	if (!OwnerCam || !OwnerCam->CameraComp)
	{
		UE_LOG(LogTemp, Error,
			TEXT("[Centering] Owner must be APTZCamera with CameraComp (got %s). Component disabled."),
			*GetNameSafe(GetOwner()));
		OwnerCam = nullptr;
		return;
	}

	// 렌더 타겟: 8비트 BGRA + sRGB (bForceLinearGamma=false).
	// SCS_FinalColorLDR(톤맵 후) 출력과 짝을 이뤄 JPEG 인코드까지 감마 처리가 자동으로 맞는다.
	RenderTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("CenteringRT"));
	RenderTarget->ClearColor = FLinearColor::Black;
	RenderTarget->InitCustomFormat(CaptureWidth, CaptureHeight, PF_B8G8R8A8, /*bInForceLinearGamma=*/false);

	// 씬 캡처: 런타임 생성이므로 NewObject -> RegisterComponent -> AttachToComponent 순서
	// (SetupAttachment 는 생성자 전용).
	CaptureComp = NewObject<USceneCaptureComponent2D>(OwnerCam, TEXT("CenteringCapture"));
	CaptureComp->RegisterComponent();
	CaptureComp->AttachToComponent(OwnerCam->CameraComp,
		FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	CaptureComp->SetRelativeLocationAndRotation(FVector::ZeroVector, FRotator::ZeroRotator);

	CaptureComp->ProjectionType = ECameraProjectionMode::Perspective;
	// 기본값은 SCS_SceneColorHDR(톤맵 전 선형) — 그대로 두면 물 빠진/어두운 JPEG 가 나온다.
	// FinalColorLDR = 포스트프로세스+톤맵 거친 8비트 sRGB = 실제 CCTV 인코더가 보는 그림.
	CaptureComp->CaptureSource = SCS_FinalColorLDR;
	CaptureComp->bCaptureEveryFrame = false;   // 요청 시에만 캡처
	CaptureComp->bCaptureOnMovement = false;
	// 단발 캡처 사이에 노출/TAA 히스토리 유지 — 없으면 첫 캡처 노출이 망가진다.
	CaptureComp->bAlwaysPersistRenderingState = true;
	// 정지 사진에 팬 모션블러가 섞이면 번호판 가독성만 해친다.
	CaptureComp->ShowFlags.SetMotionBlur(false);
	CaptureComp->TextureTarget = RenderTarget;

	// 시작 시 서버 준비 상태 1회 확인 (로그 + OnHealthResult 방송).
	CheckHealth();
}

void UCenteringClientComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (PendingRequest.IsValid())
	{
		PendingRequest->CancelRequest();
		PendingRequest.Reset();
	}
	Super::EndPlay(EndPlayReason);
}

//======================================================================================
// Runtime API
//======================================================================================

bool UCenteringClientComponent::RequestInference()
{
	if (!OwnerCam || !CaptureComp || State != ECenteringState::Idle)
	{
		if (bDrawDebugLog)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Centering] RequestInference rejected (%s)"),
				!OwnerCam ? TEXT("uninitialized") : TEXT("busy"));
		}
		return false;
	}
	State = ECenteringState::Capturing;

	// 1) 캡처 시점 PTZ 스냅샷.
	//    추론 1~2초 동안 카메라가 움직일 수 있으므로 LookAt 은 반드시 이 값 기준이다.
	CapturedPan  = OwnerCam->GetCurrentPan();
	CapturedTilt = OwnerCam->GetCurrentTilt();
	CapturedZoom = OwnerCam->GetCurrentZoomFactor();
	CapturedFOV  = OwnerCam->CameraComp->FieldOfView;

	// 2) 광학 줌 동기화 — 씬캡처 FOV 는 카메라를 따라가지 않는다. 매 캡처 직전 필수.
	CaptureComp->FOVAngle = CapturedFOV;

	// 3) 렌더 + 읽기 + JPEG 인코드 (동기 — 한 프레임 히치는 관찰 도구에서 허용).
	TArray64<uint8> Jpeg;
	if (!CaptureToJpeg(Jpeg))
	{
		FinishInference(false);
		return false;
	}

	// 4) 멀티파트 바디. current 값에는 부호 보정을 "전송 측"에도 대칭 적용한다.
	const FString Boundary = TEXT("----baroUE") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TArray<uint8> Body = BuildMultipartBody(Boundary, Jpeg,
		PanSign * CapturedPan, TiltSign * CapturedTilt, CapturedZoom);

	// 5) POST {ServerUrl}/centering/lite
	const FString Url = ServerUrl.TrimEnd().TrimChar(TEXT('/')) + TEXT("/centering/lite");
	FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"),
		FString::Printf(TEXT("multipart/form-data; boundary=%s"), *Boundary));
	Request->SetContent(MoveTemp(Body));
	Request->SetTimeout(TimeoutSec);
	Request->OnProcessRequestComplete().BindUObject(this, &UCenteringClientComponent::OnInferenceResponse);

	PendingRequest = Request;
	State = ECenteringState::Waiting;
	++InferenceSeq;

	if (bDrawDebugLog)
	{
		UE_LOG(LogTemp, Log, TEXT("[Centering] #%d POST %s  (cap pan=%.2f tilt=%.2f zoom=%.2f fov=%.2f, jpeg=%lld bytes)"),
			InferenceSeq, *Url, CapturedPan, CapturedTilt, CapturedZoom, CapturedFOV, Jpeg.Num());
	}

	Request->ProcessRequest();
	return true;
}

bool UCenteringClientComponent::LookAtPlate(int32 Index)
{
	if (!OwnerCam || !LastPlates.IsValidIndex(Index))
	{
		if (bDrawDebugLog)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Centering] LookAtPlate(%d) rejected (plates=%d)"),
				Index, LastPlates.Num());
		}
		return false;
	}

	const FCenteringPlate& P = LastPlates[Index];

	// ---- 필터 삽입 지점 (단일화) -----------------------------------------------
	// 관찰 결과에 따라 데드밴드/스무딩/이상치 제거가 필요해지면 여기 한 곳에만 넣는다.
	// 현재(1차 관찰 단계)는 raw 델타를 그대로 적용한다.
	const float AppliedDPan  = PanSign * P.DPan;
	const float AppliedDTilt = TiltSign * P.DTilt;
	// ---------------------------------------------------------------------------

	// target = 캡처 시점 스냅샷 + 보정된 델타. (현재값/타깃값 기준이 아님!)
	// 실제 회전은 PTZCamera 의 모터 보간(PanSpeed/TiltSpeed deg/s)이 천천히 수행한다.
	OwnerCam->SetPanTilt(CapturedPan + AppliedDPan, CapturedTilt + AppliedDTilt);

	if (bApplyZoomOnLookAt)
	{
		OwnerCam->SetZoomFactor(P.Zoom);   // 절대값 (카메라 Min/MaxZoomFactor 로 clamp 됨)
	}

	if (bDrawDebugLog)
	{
		UE_LOG(LogTemp, Log,
			TEXT("[Centering] LookAt[%d]: dpan=%.2f dtilt=%.2f -> target pan=%.2f tilt=%.2f (dist=%.2f zoom=%.2f%s)"),
			Index, P.DPan, P.DTilt,
			CapturedPan + AppliedDPan, CapturedTilt + AppliedDTilt,
			P.Dist, P.Zoom, bApplyZoomOnLookAt ? TEXT(", applied") : TEXT(", not applied"));
	}

	if (bWriteCsvLog)
	{
		AppendCsvAppliedRow(Index, P);
	}
	return true;
}

void UCenteringClientComponent::CheckHealth()
{
	const FString Url = ServerUrl.TrimEnd().TrimChar(TEXT('/')) + TEXT("/health");
	FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(10.f);
	Request->OnProcessRequestComplete().BindUObject(this, &UCenteringClientComponent::OnHealthResponse);
	Request->ProcessRequest();
}

//======================================================================================
// Pipeline helpers
//======================================================================================

bool UCenteringClientComponent::CaptureToJpeg(TArray64<uint8>& OutJpeg)
{
	CaptureComp->CaptureScene();   // 즉시 단발 렌더

	// GetRenderTargetImage 는 내부에서 ReadPixels(렌더 명령 플러시 = 한 프레임 히치)를 수행하고,
	// RT 의 감마 공간(sRGB)을 FImage 에 태깅한다 -> CompressImage 까지 감마 처리 자동.
	// (UE 5.7 은 IImageWrapper 직접 사용을 deprecated — FImageUtils 경로가 공식.)
	FImage Image;
	if (!FImageUtils::GetRenderTargetImage(RenderTarget, Image))
	{
		UE_LOG(LogTemp, Error, TEXT("[Centering] GetRenderTargetImage failed"));
		return false;
	}
	if (!FImageUtils::CompressImage(OutJpeg, TEXT("jpg"), Image, JpegQuality))
	{
		UE_LOG(LogTemp, Error, TEXT("[Centering] JPEG encode failed"));
		return false;
	}

	if (bSaveDebugCapture)
	{
		const FString Path = FPaths::ProjectSavedDir() / TEXT("Centering/last_capture.jpg");
		FFileHelper::SaveArrayToFile(
			TArrayView<const uint8>(OutJpeg.GetData(), IntCastChecked<int32>(OutJpeg.Num())), *Path);
		if (bDrawDebugLog)
		{
			UE_LOG(LogTemp, Log, TEXT("[Centering] debug capture saved: %s"), *Path);
		}
	}
	return true;
}

namespace
{
	void AppendUtf8(TArray<uint8>& Body, const FString& Str)
	{
		FTCHARToUTF8 Conv(*Str);
		Body.Append(reinterpret_cast<const uint8*>(Conv.Get()), Conv.Length());
	}
}

TArray<uint8> UCenteringClientComponent::BuildMultipartBody(const FString& Boundary,
	const TArray64<uint8>& Jpeg, float Pan, float Tilt, float Zoom) const
{
	// FastAPI 표준 multipart/form-data. CRLF 필수, 종료 구분자는 --{B}-- .
	// 레이아웃은 baro_vla api_test 의 cpp-httplib / Unity 클라이언트와 동일하다.
	TArray<uint8> Body;
	Body.Reserve(static_cast<int32>(Jpeg.Num()) + 1024);

	AppendUtf8(Body, FString::Printf(
		TEXT("--%s\r\nContent-Disposition: form-data; name=\"image\"; filename=\"capture.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n"),
		*Boundary));
	Body.Append(Jpeg.GetData(), IntCastChecked<int32>(Jpeg.Num()));
	AppendUtf8(Body, TEXT("\r\n"));

	const auto AddField = [&Body, &Boundary](const TCHAR* Name, float Value)
	{
		// FString::Printf 의 %f 는 로캘 독립(항상 '.') — Unity 의 InvariantCulture 우려 없음.
		AppendUtf8(Body, FString::Printf(
			TEXT("--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%.3f\r\n"),
			*Boundary, Name, Value));
	};
	AddField(TEXT("current_pan"), Pan);
	AddField(TEXT("current_tilt"), Tilt);
	AddField(TEXT("current_zoom"), Zoom);

	AppendUtf8(Body, FString::Printf(TEXT("--%s--\r\n"), *Boundary));
	return Body;
}

//======================================================================================
// HTTP callbacks (게임스레드에서 호출됨)
//======================================================================================

void UCenteringClientComponent::OnInferenceResponse(FHttpRequestPtr /*Request*/,
	FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
	PendingRequest.Reset();
	LastPlates.Reset();

	if (!bConnectedSuccessfully || !Response.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Centering] #%d connection failed"), InferenceSeq);
		FinishInference(false);
		return;
	}
	if (Response->GetResponseCode() != 200)
	{
		// 503 = 모델 로딩 중, 422 = form 형식 오류
		UE_LOG(LogTemp, Warning, TEXT("[Centering] #%d HTTP %d: %s"),
			InferenceSeq, Response->GetResponseCode(), *Response->GetContentAsString());
		FinishInference(false);
		return;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
	const TArray<TSharedPtr<FJsonValue>>* Datas = nullptr;
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()
		|| !Root->TryGetArrayField(TEXT("datas"), Datas))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Centering] #%d JSON parse failed: %s"),
			InferenceSeq, *Response->GetContentAsString().Left(200));
		FinishInference(false);
		return;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Datas)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj->IsValid())
		{
			continue;
		}
		FCenteringPlate Plate;
		Plate.DPan  = static_cast<float>((*Obj)->GetNumberField(TEXT("dpan")));
		Plate.DTilt = static_cast<float>((*Obj)->GetNumberField(TEXT("dtilt")));
		Plate.Dist  = static_cast<float>((*Obj)->GetNumberField(TEXT("dist")));
		Plate.Zoom  = static_cast<float>((*Obj)->GetNumberField(TEXT("zoom")));
		LastPlates.Add(Plate);
	}

	if (bDrawDebugLog)
	{
		UE_LOG(LogTemp, Log, TEXT("[Centering] #%d OK plates=%d"), InferenceSeq, LastPlates.Num());
		for (int32 i = 0; i < LastPlates.Num(); ++i)
		{
			const FCenteringPlate& P = LastPlates[i];
			UE_LOG(LogTemp, Log, TEXT("    [%d] dpan=%.2f dtilt=%.2f dist=%.2f zoom=%.2f"),
				i, P.DPan, P.DTilt, P.Dist, P.Zoom);
		}
	}

	// 빈 배열은 soft-success: 미검출(또는 서버측 파싱 실패). 카메라는 움직이지 않는다.
	FinishInference(true);
}

void UCenteringClientComponent::OnHealthResponse(FHttpRequestPtr /*Request*/,
	FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
	FString StateStr = TEXT("unreachable");
	if (bConnectedSuccessfully && Response.IsValid() && Response->GetResponseCode() == 200)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
		if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
		{
			StateStr = Root->GetStringField(TEXT("state"));
		}
	}
	const bool bReady = (StateStr == TEXT("ready"));
	UE_LOG(LogTemp, Log, TEXT("[Centering] /health state=%s"), *StateStr);
	OnHealthResult.Broadcast(bReady, StateStr);
}

//======================================================================================
// Finish / CSV
//======================================================================================

void UCenteringClientComponent::FinishInference(bool bSuccess)
{
	State = ECenteringState::Idle;
	if (bWriteCsvLog)
	{
		AppendCsvRows(bSuccess);
	}
	OnInferenceResult.Broadcast(bSuccess, LastPlates);
}

FString UCenteringClientComponent::CsvFilePath() const
{
	return FPaths::ProjectSavedDir() / TEXT("Centering/centering_log.csv");
}

void UCenteringClientComponent::AppendCsvRows(bool bSuccess) const
{
	const FString Path = CsvFilePath();
	FString Out;
	if (!FPaths::FileExists(Path))
	{
		Out += TEXT("utc_time,seq,success,cap_pan,cap_tilt,cap_zoom,cap_fov,plate_idx,plate_count,dpan,dtilt,dist,zoom,applied\r\n");
	}

	const FString Now = FDateTime::UtcNow().ToIso8601();
	if (LastPlates.IsEmpty())
	{
		// 결과 0개(실패 포함)도 한 행 남긴다 — 미검출 빈도 자체가 관찰 대상.
		Out += FString::Printf(TEXT("%s,%d,%d,%.3f,%.3f,%.3f,%.3f,-1,0,,,,,0\r\n"),
			*Now, InferenceSeq, bSuccess ? 1 : 0,
			CapturedPan, CapturedTilt, CapturedZoom, CapturedFOV);
	}
	else
	{
		for (int32 i = 0; i < LastPlates.Num(); ++i)
		{
			const FCenteringPlate& P = LastPlates[i];
			Out += FString::Printf(TEXT("%s,%d,%d,%.3f,%.3f,%.3f,%.3f,%d,%d,%.3f,%.3f,%.3f,%.3f,0\r\n"),
				*Now, InferenceSeq, bSuccess ? 1 : 0,
				CapturedPan, CapturedTilt, CapturedZoom, CapturedFOV,
				i, LastPlates.Num(), P.DPan, P.DTilt, P.Dist, P.Zoom);
		}
	}

	FFileHelper::SaveStringToFile(Out, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
		&IFileManager::Get(), EFileWrite::FILEWRITE_Append);
}

void UCenteringClientComponent::AppendCsvAppliedRow(int32 Index, const FCenteringPlate& Plate) const
{
	const FString Path = CsvFilePath();
	FString Out;
	if (!FPaths::FileExists(Path))
	{
		Out += TEXT("utc_time,seq,success,cap_pan,cap_tilt,cap_zoom,cap_fov,plate_idx,plate_count,dpan,dtilt,dist,zoom,applied\r\n");
	}
	Out += FString::Printf(TEXT("%s,%d,1,%.3f,%.3f,%.3f,%.3f,%d,%d,%.3f,%.3f,%.3f,%.3f,1\r\n"),
		*FDateTime::UtcNow().ToIso8601(), InferenceSeq,
		CapturedPan, CapturedTilt, CapturedZoom, CapturedFOV,
		Index, LastPlates.Num(), Plate.DPan, Plate.DTilt, Plate.Dist, Plate.Zoom);

	FFileHelper::SaveStringToFile(Out, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
		&IFileManager::Get(), EFileWrite::FILEWRITE_Append);
}

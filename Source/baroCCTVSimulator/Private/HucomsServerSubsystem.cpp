// Fill out your copyright notice in the Description page of Project Settings.

#include "HucomsServerSubsystem.h"

#include "HucomsProtocol.h"
#include "PTZCamera.h"
#include "PTZCaptureComponent.h"
#include "MjpegStreamServer.h"

#include "HttpServerModule.h"
#include "HttpServerResponse.h"
#include "HttpServerRequest.h"      // FHttpServerRequest, EHttpServerRequestVerbs
#include "HttpPath.h"
#include "IHttpRouter.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/CameraComponent.h"
#include "ContentStreaming.h"   // IStreamingManager — CCTV 시점을 텍스처 스트리머에 등록

DEFINE_LOG_CATEGORY_STATIC(LogHucomsSim, Log, All);

//======================================================================================
// 로컬 헬퍼 (쿼리 파싱 / 응답 생성)
//======================================================================================
namespace
{
	FString GetQ(const FHttpServerRequest& Req, const TCHAR* Key, const FString& Def = FString())
	{
		if (const FString* V = Req.QueryParams.Find(Key))
		{
			return *V;
		}
		return Def;
	}

	bool HasQ(const FHttpServerRequest& Req, const TCHAR* Key)
	{
		return Req.QueryParams.Contains(Key);
	}

	int32 GetQInt(const FHttpServerRequest& Req, const TCHAR* Key, int32 Def)
	{
		if (const FString* V = Req.QueryParams.Find(Key))
		{
			return FCString::Atoi(**V);
		}
		return Def;
	}

	float GetQFloat(const FHttpServerRequest& Req, const TCHAR* Key, float Def)
	{
		if (const FString* V = Req.QueryParams.Find(Key))
		{
			return FCString::Atof(**V);
		}
		return Def;
	}

	void AppendUtf8(TArray<uint8>& Out, const FString& Str)
	{
		FTCHARToUTF8 Utf8(*Str);
		Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}

	/** text/plain 응답 ('key = value' 본문). 실기와 동일하게 항상 200. */
	TUniquePtr<FHttpServerResponse> MakeText(const FString& Body, const FString& ContentType = TEXT("text/plain"))
	{
		return FHttpServerResponse::Create(Body, ContentType);
	}

	/** 바이너리 응답 (JPEG/멀티파트). content-type 그대로 보존. */
	TUniquePtr<FHttpServerResponse> MakeBytes(TArray<uint8>&& Bytes, const FString& ContentType)
	{
		return FHttpServerResponse::Create(MoveTemp(Bytes), ContentType);
	}

	/** 4바이트 빈 JPEG (SOI+EOI). 렌더 실패 시 스텁 - fake-camera-client 와 동일. */
	TArray<uint8> StubJpeg()
	{
		return TArray<uint8>({ 0xFF, 0xD8, 0xFF, 0xD9 });
	}

	/** Cur 를 Tgt 방향으로 MaxStep 만큼 한 발 이동(선형 슬루). 일반 축용. */
	int32 StepLinear(int32 Cur, int32 Tgt, int32 MaxStep)
	{
		const int32 D = Tgt - Cur;
		if (FMath::Abs(D) <= MaxStep) { return Tgt; }
		return Cur + ((D > 0) ? MaxStep : -MaxStep);
	}
}

//======================================================================================
// Subsystem lifecycle
//======================================================================================
bool UHucomsServerSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// 실제 플레이(게임/PIE)에서만 서버를 띄운다. 에디터 프리뷰/인스펙터 월드는 제외.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UHucomsServerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// 서버 시작은 월드 BeginPlay 이후(액터 스폰 완료)에 한다.
}

void UHucomsServerSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	StartServers();
}

void UHucomsServerSubsystem::Deinitialize()
{
	StopServers();
	Super::Deinitialize();
}

//======================================================================================
// 채널 구성 (레벨의 카메라 열거 -> 포트 부여)
//======================================================================================
void UHucomsServerSubsystem::BuildChannels()
{
	Channels.Reset();

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	TArray<AActor*> Actors;
	UGameplayStatics::GetAllActorsOfClass(World, APTZCamera::StaticClass(), Actors);

	// 카메라가 하나도 없으면 기본 카메라 자동 생성(1회) — 박스아웃 동작 + 자율 검증용.
	if (Actors.Num() == 0 && bAutoSpawnCameraIfNone && !bAutoSpawnAttempted)
	{
		bAutoSpawnAttempted = true;

		FVector Loc(0.f, 0.f, 300.f);
		float Yaw = 0.f;
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			if (AActor* Pawn = PC->GetPawn())
			{
				Loc = Pawn->GetActorLocation() + FVector(0.f, 0.f, 250.f);
				Yaw = Pawn->GetActorRotation().Yaw;
			}
		}
		const FRotator Rot(-15.f, Yaw, 0.f); // 살짝 아래를 보는 CCTV 자세

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		if (APTZCamera* Spawned = World->SpawnActor<APTZCamera>(APTZCamera::StaticClass(), Loc, Rot, Params))
		{
			Actors.Add(Spawned);
			UE_LOG(LogHucomsSim, Log, TEXT("[Hucoms] 레벨에 APTZCamera 없음 -> 자동 생성 @ %s (yaw=%.1f)"), *Loc.ToString(), Yaw);
		}
	}

	int32 Index = 0;
	for (AActor* A : Actors)
	{
		APTZCamera* Cam = Cast<APTZCamera>(A);
		if (!Cam || !Cam->bServeHucoms)
		{
			continue;
		}

		TSharedPtr<FHucomsChannel> Ch = MakeShared<FHucomsChannel>();
		Ch->Camera    = Cam;
		Ch->HttpPort  = (Cam->HucomsHttpPort  > 0) ? Cam->HucomsHttpPort  : (BaseHttpPort  + Index);
		Ch->MjpegPort = (Cam->HucomsMjpegPort > 0) ? Cam->HucomsMjpegPort : (BaseMjpegPort + Index);

		// 홈 포즈 정렬: pan 은 설치 heading(+X)을 그대로 보게 0.
		Ch->CurPan = Ch->TgtPan = 0;
		// 상하 조준 이관: 이제 광학축은 액터 Pitch 를 상속하지 않으므로(롤 방지), 설치 시 액터에
		// 넣어둔 하방 조준(Pitch)을 '틸트'로 옮겨 같은 화각을 롤 없이 재현한다.
		//   MirrorChannel: UE pitch = TiltToPitchSign * (tilt/100)  =>  tilt = pitch / TiltToPitchSign * 100.
		//   (TiltToPitchSign=±1 이므로 pitch*Sign*100 과 동일.)
		const float InstallPitchDeg = Cam->GetActorRotation().Pitch;
		Ch->CurTilt = Ch->TgtTilt = HucomsProtocol::ClampTilt(
			FMath::RoundToInt(InstallPitchDeg * TiltToPitchSign * 100.f));
		Ch->CurZoom = Ch->TgtZoom = 0;
		Ch->CurFocus = Ch->TgtFocus = 0;

		// 고정형: 이 채널은 PTZ 명령/모터 슬루를 무시하고 위 설치 자세(CurTilt=InstallPitch 등)로 고정.
		Ch->bFixed = Cam->bFixedMode;

		ConfigureCameraForSim(Cam);
		Channels.Add(Ch);
		++Index;
	}
}

//======================================================================================
// HTTP server start/stop (채널별)
//======================================================================================
void UHucomsServerSubsystem::StartServers()
{
	if (bServersStarted)
	{
		return;
	}

	BuildChannels();
	if (Channels.Num() == 0)
	{
		UE_LOG(LogHucomsSim, Warning, TEXT("[Hucoms] 서빙할 APTZCamera(bServeHucoms) 가 없음 -> 서버 미기동."));
		return;
	}

	FHttpServerModule& Http = FHttpServerModule::Get();
	int32 StartedCount = 0;

	for (TSharedPtr<FHucomsChannel>& ChPtr : Channels)
	{
		FHucomsChannel& Ch = *ChPtr;

		// bFailOnBindFailure=true: 포트를 즉시 바인드 시도, 실패(중복/점유) 시 nullptr -> 명확한 진단.
		Ch.Router = Http.GetHttpRouter(Ch.HttpPort, /*bFailOnBindFailure=*/true);
		if (!Ch.Router.IsValid())
		{
			UE_LOG(LogHucomsSim, Error, TEXT("[Hucoms] 라우터 획득/바인드 실패 (port %d). 포트 중복/점유 확인 (카메라 %s)."),
				Ch.HttpPort, *GetNameSafe(Ch.Camera.Get()));
			continue;
		}

		// 라우트 바인딩: 핸들러 람다가 채널(TSharedPtr)을 캡처해 그 채널의 상태에 작용.
		auto BindGet = [this, ChPtr](const TCHAR* Path,
			bool (UHucomsServerSubsystem::*Fn)(FHucomsChannel&, const FHttpServerRequest&, const FHttpResultCallback&))
		{
			FHttpRouteHandle H = ChPtr->Router->BindRoute(
				FHttpPath(FString(Path)),
				EHttpServerRequestVerbs::VERB_GET,
				FHttpRequestHandler::CreateLambda(
					[this, ChPtr, Fn](const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
					{
						return (this->*Fn)(*ChPtr, Req, OnComplete);
					}));
			if (H.IsValid())
			{
				ChPtr->RouteHandles.Add(H);
			}
			else
			{
				UE_LOG(LogHucomsSim, Error, TEXT("[Hucoms] 라우트 바인딩 실패: %s (port %d)"), Path, ChPtr->HttpPort);
			}
		};

		BindGet(TEXT("/cgi-bin/control/ptzf_status.cgi"),   &UHucomsServerSubsystem::HandlePtzfStatus);
		BindGet(TEXT("/cgi-bin/control/ptz_centering.cgi"), &UHucomsServerSubsystem::HandlePtzCentering);
		BindGet(TEXT("/cgi-bin/control/capabilityptz.cgi"), &UHucomsServerSubsystem::HandleCapabilityPtz);
		BindGet(TEXT("/cgi-bin/image/jpeg.cgi"),            &UHucomsServerSubsystem::HandleJpeg);
		BindGet(TEXT("/cgi-bin/image/mjpeg.cgi"),           &UHucomsServerSubsystem::HandleMjpeg);
		BindGet(TEXT("/api/tuning"),                        &UHucomsServerSubsystem::HandleTuning);

		// 연속 MJPEG 스트림 서버(채널별 포트).
		if (bEnableMjpegStream)
		{
			Ch.Stream = new FMjpegStreamServer();
			if (!Ch.Stream->StartServer(Ch.MjpegPort, StreamFps))
			{
				delete Ch.Stream;
				Ch.Stream = nullptr;
				UE_LOG(LogHucomsSim, Error, TEXT("[Hucoms] MJPEG 스트림 서버 시작 실패 (port %d)"), Ch.MjpegPort);
			}
		}

		++StartedCount;
		UE_LOG(LogHucomsSim, Log, TEXT("[Hucoms] 채널 기동: 카메라 '%s'  HTTP :%d  MJPEG :%d"),
			*GetNameSafe(Ch.Camera.Get()), Ch.HttpPort, (Ch.Stream ? Ch.MjpegPort : -1));
	}

	Http.StartAllListeners();
	bServersStarted = true;

	UE_LOG(LogHucomsSim, Log, TEXT("[Hucoms] 시뮬레이터 서버 시작 — 채널 %d/%d 개."), StartedCount, Channels.Num());
}

void UHucomsServerSubsystem::StopServers()
{
	for (TSharedPtr<FHucomsChannel>& ChPtr : Channels)
	{
		FHucomsChannel& Ch = *ChPtr;

		if (Ch.Router.IsValid())
		{
			for (const FHttpRouteHandle& H : Ch.RouteHandles)
			{
				if (H.IsValid())
				{
					Ch.Router->UnbindRoute(H);
				}
			}
		}
		Ch.RouteHandles.Reset();
		Ch.Router.Reset();

		if (Ch.Stream)
		{
			Ch.Stream->StopServer();
			delete Ch.Stream;
			Ch.Stream = nullptr;
		}
	}
	Channels.Reset();

	if (bServersStarted)
	{
		// 이 프로젝트만 HTTP 서버를 쓰므로 전역 정지로 충분(모든 채널 리스너 정지).
		FHttpServerModule::Get().StopAllListeners();
		bServersStarted = false;
		UE_LOG(LogHucomsSim, Log, TEXT("[Hucoms] 시뮬레이터 서버 정지(모든 채널)."));
	}
}

//======================================================================================
// Tick - 채널별 모터 슬루 + 카메라 미러 + 스트림
//======================================================================================
void UHucomsServerSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!bServersStarted)
	{
		return;
	}

	const int32 PanStep  = FMath::Max(1, FMath::RoundToInt(PanSlewCdPerSec  * DeltaTime));
	const int32 TiltStep = FMath::Max(1, FMath::RoundToInt(TiltSlewCdPerSec * DeltaTime));
	const int32 ZoomStep = FMath::Max(1, FMath::RoundToInt(ZoomSlewPerSec   * DeltaTime));
	const float Interval = 1.f / FMath::Max(1, StreamFps);

	for (TSharedPtr<FHucomsChannel>& ChPtr : Channels)
	{
		FHucomsChannel& Ch = *ChPtr;

		if (Ch.bFixed)
		{
			// 고정형: 모터 슬루 없이 설치 자세로 고정(Cur=Tgt). 명령이 무시되므로 Tgt 는 초기 설치값 그대로.
			Ch.CurPan   = Ch.TgtPan;
			Ch.CurTilt  = Ch.TgtTilt;
			Ch.CurZoom  = Ch.TgtZoom;
			Ch.CurFocus = Ch.TgtFocus;
		}
		else
		{
			// Pan: 0/35999 이음매를 넘어 최단 호로 이동.
			{
				const int32 D = HucomsProtocol::ShortestPanDiff(Ch.CurPan, Ch.TgtPan);
				if (FMath::Abs(D) <= PanStep)
				{
					Ch.CurPan = HucomsProtocol::WrapPan(Ch.TgtPan);
				}
				else
				{
					Ch.CurPan = HucomsProtocol::WrapPan(Ch.CurPan + ((D > 0) ? PanStep : -PanStep));
				}
			}

			Ch.CurTilt  = StepLinear(Ch.CurTilt,  Ch.TgtTilt,  TiltStep);
			Ch.CurZoom  = StepLinear(Ch.CurZoom,  Ch.TgtZoom,  ZoomStep);
			Ch.CurFocus = Ch.TgtFocus; // 포커스는 즉시
		}

		MirrorChannel(Ch);

		// CCTV 시점을 텍스처 스트리머에 등록 — 스트리머는 플레이어 뷰만 시점으로 쓰고
		// SceneCapture 뷰는 등록하지 않으므로(UE5.8 GameViewportClient.cpp:1913 확인),
		// 줌으로 당긴 원거리 텍스처가 저해상도 mip 으로 뭉개진다. 카메라 위치 + 현재 줌
		// FOV(FOVScreenSize = 폭/tan(HFOV/2))를 매 틱 등록해 mip 이 CCTV 기준으로 올라온다.
		if (const APTZCamera* Cam = Ch.Camera.Get())
		{
			if (Cam->CameraComp)
			{
				const float HalfHFovRad = FMath::DegreesToRadians(FMath::Max(1.f, Cam->CameraComp->FieldOfView)) * 0.5f;
				const float ScreenSize = static_cast<float>(FMath::Max(StreamWidth, SnapshotWidth));
				IStreamingManager::Get().AddViewInformation(
					Cam->CameraComp->GetComponentLocation(), ScreenSize,
					ScreenSize / FMath::Max(FMath::Tan(HalfHFovRad), 0.01f));
			}
		}

		// 연속 MJPEG: 클라이언트가 있을 때만 StreamFps 로 캡처(없으면 렌더 비용 0).
		if (Ch.Stream && Ch.Stream->HasClients())
		{
			Ch.StreamAccum += DeltaTime;
			Ch.FpsWindowAccum += DeltaTime;
			if (Ch.StreamAccum >= Interval)
			{
				// 잔여 시간을 보존해야 실효 fps 가 StreamFps 에 수렴한다(0 리셋은 게임 틱 경계로
				// 양자화되어 항상 목표 미달). 게임 fps < StreamFps 인 구간에서 부채가 무한 누적되어
				// 히치 후 따라잡기 폭주하지 않도록 한 프레임치로 클램프.
				Ch.StreamAccum = FMath::Min(Ch.StreamAccum - Interval, Interval);
				if (UPTZCaptureComponent* Cap = ResolveCapture(Ch.Camera.Get()))
				{
					TArray64<uint8> Jpeg;
					// 스냅샷(jpeg.cgi)과 동일한 노출/대비 보정을 연속 스트림에도 적용 —
					// 안 그러면 튜닝 슬라이더를 움직여도 이 경로를 보는 화면은 안 바뀐다.
					if (Cap->CaptureJpeg(StreamWidth, StreamHeight, StreamJpegQuality, Jpeg, /*WarmupFrames=*/0, CaptureExposureBias, CaptureContrast) && Jpeg.Num() > 0)
					{
						TArray<uint8> Frame;
						Frame.Append(Jpeg.GetData(), IntCastChecked<int32>(Jpeg.Num()));
						Ch.Stream->UpdateFrame(Frame);
						++Ch.FpsWindowFrames;
					}
				}
			}
			// 1초 창으로 실측 송신 fps 갱신 (HUD 표시용)
			if (Ch.FpsWindowAccum >= 1.f)
			{
				Ch.MeasuredStreamFps = Ch.FpsWindowFrames / Ch.FpsWindowAccum;
				Ch.FpsWindowAccum = 0.f;
				Ch.FpsWindowFrames = 0;
			}
		}
		else
		{
			Ch.MeasuredStreamFps = 0.f;
			Ch.FpsWindowAccum = 0.f;
			Ch.FpsWindowFrames = 0;
		}
	}
}

TStatId UHucomsServerSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UHucomsServerSubsystem, STATGROUP_Tickables);
}

TArray<FString> UHucomsServerSubsystem::GetChannelStatusLines() const
{
	TArray<FString> Lines;
	for (const TSharedPtr<FHucomsChannel>& ChPtr : Channels)
	{
		const FHucomsChannel& Ch = *ChPtr;
		const FString Name = Ch.Camera.IsValid() ? Ch.Camera->GetName() : TEXT("(카메라 없음)");
		const int32 Clients = Ch.Stream ? Ch.Stream->GetClientCount() : 0;
		if (Clients > 0)
		{
			Lines.Add(FString::Printf(TEXT("%s  http:%d  mjpeg:%d  ▶ %.1f fps  (클라이언트 %d)"),
				*Name, Ch.HttpPort, Ch.MjpegPort, Ch.MeasuredStreamFps, Clients));
		}
		else
		{
			Lines.Add(FString::Printf(TEXT("%s  http:%d  mjpeg:%d  — 대기 (클라이언트 없음, 캡처 0)"),
				*Name, Ch.HttpPort, Ch.MjpegPort));
		}
	}
	return Lines;
}

bool UHucomsServerSubsystem::GetCameraPorts(const APTZCamera* Cam, int32& OutHttpPort, int32& OutMjpegPort) const
{
	if (!Cam) { return false; }
	for (const TSharedPtr<FHucomsChannel>& ChPtr : Channels)
	{
		if (ChPtr.IsValid() && ChPtr->Camera.Get() == Cam)
		{
			OutHttpPort = ChPtr->HttpPort;
			OutMjpegPort = ChPtr->MjpegPort;
			return true;
		}
	}
	return false;
}

//======================================================================================
// Camera configure / mirror / capture
//======================================================================================
void UHucomsServerSubsystem::ConfigureCameraForSim(APTZCamera* Cam)
{
	if (!Cam)
	{
		return;
	}
	// sim 의 설정된 wide HFOV(현재 기본 57.14)에 1x 를 맞추고, Hucoms 범위를 시각화할 수 있게 한계를 넓힌다.
	Cam->BaseFOV       = WideHFovDeg;
	Cam->PanMin        = FMath::Min(Cam->PanMin, -180.f);
	Cam->PanMax        = FMath::Max(Cam->PanMax,  180.f);
	Cam->TiltMin       = FMath::Min(Cam->TiltMin, HucomsProtocol::TiltPosMin / 100.f); // -20deg
	Cam->TiltMax       = FMath::Max(Cam->TiltMax, HucomsProtocol::TiltPosMax / 100.f); // +90deg
	Cam->MaxZoomFactor = FMath::Max(Cam->MaxZoomFactor, 60.f);

	UE_LOG(LogHucomsSim, Log, TEXT("[Hucoms] 카메라 sim 설정: %s (BaseFOV=%.2f)"), *Cam->GetName(), WideHFovDeg);
}

void UHucomsServerSubsystem::MirrorChannel(FHucomsChannel& Ch)
{
	APTZCamera* Cam = Ch.Camera.Get();
	if (!Cam)
	{
		return;
	}

	const float Yaw   = FMath::UnwindDegrees(PanToYawSign * (Ch.CurPan / 100.f));
	const float Pitch = TiltToPitchSign * (Ch.CurTilt / 100.f);
	const float HFov  = HucomsProtocol::ZoomPosToHFov(Ch.CurZoom, WideHFovDeg);
	const float Zf    = HucomsProtocol::HFovToZoomFactor(HFov, WideHFovDeg);

	// 정준 current 를 카메라에 즉시 반영(채널이 모터, 카메라는 미러).
	Cam->SetPanTilt(Yaw, Pitch);
	Cam->SetZoomFactor(Zf);
	Cam->SnapToTarget();
}

UPTZCaptureComponent* UHucomsServerSubsystem::ResolveCapture(APTZCamera* Cam)
{
	if (!Cam)
	{
		return nullptr;
	}
	UPTZCaptureComponent* Cap = Cam->FindComponentByClass<UPTZCaptureComponent>();
	if (!Cap)
	{
		Cap = NewObject<UPTZCaptureComponent>(Cam);
		Cap->RegisterComponent();
		UE_LOG(LogHucomsSim, Log, TEXT("[Hucoms] 캡처 컴포넌트 생성: %s 에 부착"), *Cam->GetName());
	}
	return Cap;
}

bool UHucomsServerSubsystem::RenderSnapshotJpeg(FHucomsChannel& Ch, TArray<uint8>& OutBytes)
{
	APTZCamera* Cam = Ch.Camera.Get();
	UPTZCaptureComponent* Cap = ResolveCapture(Cam);
	if (!Cap)
	{
		return false;
	}

	// 캡처 직전, 정준 상태를 카메라 FOV 에 한 번 더 반영(요청-틱 사이 정합 보장).
	MirrorChannel(Ch);

	TArray64<uint8> Jpeg;
	// 선명도=TAA-on+Lumen(단발), 톤=노출/대비 보정으로 뷰포트에 정합.
	if (!Cap->CaptureJpeg(SnapshotWidth, SnapshotHeight, JpegQuality, Jpeg, SnapshotWarmupFrames, CaptureExposureBias, CaptureContrast) || Jpeg.Num() == 0)
	{
		return false;
	}

	OutBytes.Reset();
	OutBytes.Append(Jpeg.GetData(), IntCastChecked<int32>(Jpeg.Num()));
	return true;
}

//======================================================================================
// CGI handlers (채널별)
//======================================================================================
bool UHucomsServerSubsystem::HandlePtzfStatus(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	const FString Action = GetQ(Req, TEXT("action"));
	FString Body;

	if (Action == TEXT("getptzfpos"))
	{
		Body = BuildPtzPosBody(Ch);
	}
	else if (Action == TEXT("goptzfpos"))
	{
		ApplyGoPtz(Ch, Req);
		Body.Empty(); // 실기: 성공 시 본문 없음. 클라이언트는 'Error:' 접두만 검사.
	}
	else if (Action == TEXT("getptzstatus"))
	{
		Body = FString::Printf(TEXT("ptstatus = %s\nzfstatus = %s\n"),
			Ch.bPtEnable ? TEXT("enable") : TEXT("disable"),
			Ch.bZfEnable ? TEXT("enable") : TEXT("disable"));
	}
	else if (Action == TEXT("setptzstatus"))
	{
		if (HasQ(Req, TEXT("ptstatus"))) { Ch.bPtEnable = (GetQ(Req, TEXT("ptstatus")) == TEXT("enable")); }
		if (HasQ(Req, TEXT("zfstatus"))) { Ch.bZfEnable = (GetQ(Req, TEXT("zfstatus")) == TEXT("enable")); }
		Body = FString::Printf(TEXT("ptstatus = %s\nzfstatus = %s\n"),
			Ch.bPtEnable ? TEXT("enable") : TEXT("disable"),
			Ch.bZfEnable ? TEXT("enable") : TEXT("disable"));
	}
	else if (Action == TEXT("lensreset"))
	{
		Body.Empty();
	}
	else
	{
		Body = TEXT("Error: invalid parameter\n");
	}

	OnComplete(MakeText(Body));
	return true;
}

bool UHucomsServerSubsystem::HandlePtzCentering(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	const FString Action = GetQ(Req, TEXT("action"));
	if (Action == TEXT("setcenter"))
	{
		ApplySetCenter(Ch, Req);
		OnComplete(MakeText(FString())); // 성공: 빈 본문.
	}
	else
	{
		OnComplete(MakeText(TEXT("Error: invalid parameter\n")));
	}
	return true;
}

bool UHucomsServerSubsystem::HandleCapabilityPtz(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	// getPTZ / getCapabilitiesPTZAll 공통 - 클라이언트 파서는 '[' 헤더 줄을 건너뛴다.
	static const TCHAR* CapsPtz =
		TEXT("[Capabilities PTZ]\n")
		TEXT("PanSupported = Yes\n")
		TEXT("TiltSupported = Yes\n")
		TEXT("ZoomSupported = Yes\n")
		TEXT("FocusSupported = Yes\n")
		TEXT("EndlessPanSupported = No\n")
		TEXT("AutoFocusSupported = Yes\n")
		TEXT("PresetSupported = 128\n")
		TEXT("AutopanSupported = No\n")
		TEXT("AutopancwSupported = No\n")
		TEXT("TourSupported = No\n");
	// 고정형: 실기 고정형 CCTV 와 동일하게 PTZ 미지원으로 광고 — 클라이언트가 PTZ 조작 UI 를 숨길 수 있다.
	static const TCHAR* CapsFixed =
		TEXT("[Capabilities PTZ]\n")
		TEXT("PanSupported = No\n")
		TEXT("TiltSupported = No\n")
		TEXT("ZoomSupported = No\n")
		TEXT("FocusSupported = No\n")
		TEXT("EndlessPanSupported = No\n")
		TEXT("AutoFocusSupported = No\n")
		TEXT("PresetSupported = 0\n")
		TEXT("AutopanSupported = No\n")
		TEXT("AutopancwSupported = No\n")
		TEXT("TourSupported = No\n");
	OnComplete(MakeText(FString(Ch.bFixed ? CapsFixed : CapsPtz)));
	return true;
}

bool UHucomsServerSubsystem::HandleJpeg(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	// 채널 카메라를 실제 렌더해 JPEG 반환. 실패 시 4바이트 스텁 폴백
	// (클라이언트는 content-type 이 image/jpeg 인지를 엄격히 검사하므로 폴백도 image/jpeg).
	TArray<uint8> Jpeg;
	if (RenderSnapshotJpeg(Ch, Jpeg))
	{
		OnComplete(MakeBytes(MoveTemp(Jpeg), TEXT("image/jpeg")));
	}
	else
	{
		UE_LOG(LogHucomsSim, Warning, TEXT("[Hucoms] jpeg.cgi 렌더 실패 -> 스텁 폴백 (port %d)."), Ch.HttpPort);
		OnComplete(MakeBytes(StubJpeg(), TEXT("image/jpeg")));
	}
	return true;
}

bool UHucomsServerSubsystem::HandleMjpeg(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	// 단일 프레임(실제 렌더) 멀티파트. 연속 스트림은 채널의 MJPEG TCP 포트.
	TArray<uint8> Jpeg;
	if (!RenderSnapshotJpeg(Ch, Jpeg))
	{
		Jpeg = StubJpeg();
	}

	const FString Boundary = TEXT("baroworldboundary");
	TArray<uint8> Body;
	AppendUtf8(Body, FString::Printf(
		TEXT("--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n"), *Boundary, Jpeg.Num()));
	Body.Append(Jpeg);
	AppendUtf8(Body, FString::Printf(TEXT("\r\n--%s--\r\n"), *Boundary));

	OnComplete(MakeBytes(MoveTemp(Body),
		FString::Printf(TEXT("multipart/x-mixed-replace;boundary=%s"), *Boundary)));
	return true;
}

bool UHucomsServerSubsystem::HandleTuning(FHucomsChannel& /*Ch*/, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	// 전 채널 공유 값(RenderSnapshotJpeg 가 이 subsystem 멤버를 매 캡처마다 읽음) — 준 항목만 갱신,
	// 나머지는 유지. 클램프는 헤더의 UPROPERTY meta 범위와 동일(에디터 Details 클램프는 런타임엔
	// 미적용이라 여기서 직접 강제 — 외부 HTTP 입력 경계이므로 검증 필요).
	if (HasQ(Req, TEXT("exposureBias")))  { CaptureExposureBias  = FMath::Clamp(GetQFloat(Req, TEXT("exposureBias"), CaptureExposureBias), -4.f, 4.f); }
	if (HasQ(Req, TEXT("contrast")))      { CaptureContrast      = FMath::Clamp(GetQFloat(Req, TEXT("contrast"), CaptureContrast), 0.5f, 3.0f); }
	if (HasQ(Req, TEXT("jpegQuality")))   { JpegQuality           = FMath::Clamp(GetQInt(Req, TEXT("jpegQuality"), JpegQuality), 1, 100); }
	if (HasQ(Req, TEXT("warmupFrames")))  { SnapshotWarmupFrames  = FMath::Clamp(GetQInt(Req, TEXT("warmupFrames"), SnapshotWarmupFrames), 0, 32); }
	if (HasQ(Req, TEXT("width")))         { SnapshotWidth         = FMath::Clamp(GetQInt(Req, TEXT("width"), SnapshotWidth), 64, 7680); }
	if (HasQ(Req, TEXT("height")))        { SnapshotHeight        = FMath::Clamp(GetQInt(Req, TEXT("height"), SnapshotHeight), 64, 4320); }

	const FString Body = FString::Printf(
		TEXT("{\"exposureBias\":%.3f,\"contrast\":%.3f,\"jpegQuality\":%d,\"warmupFrames\":%d,\"width\":%d,\"height\":%d}"),
		CaptureExposureBias, CaptureContrast, JpegQuality, SnapshotWarmupFrames, SnapshotWidth, SnapshotHeight);
	OnComplete(MakeText(Body, TEXT("application/json")));
	return true;
}

//======================================================================================
// Command application (채널별)
//======================================================================================
void UHucomsServerSubsystem::ApplyGoPtz(FHucomsChannel& Ch, const FHttpServerRequest& Req)
{
	// 고정형 카메라는 이동 명령(goptzfpos)을 무시한다 — 설치 자세로 고정.
	// (getptzfpos 는 고정된 Cur 를 그대로 반환하므로 baro_calory 라운드트립은 유지된다.)
	if (Ch.bFixed)
	{
		return;
	}

	// 절대 이동(go-to). 클라이언트는 panpos/tiltpos 항상, zoompos/focuspos 는 선택 전송.
	if (HasQ(Req, TEXT("panpos")))   { Ch.TgtPan   = HucomsProtocol::WrapPan(GetQInt(Req, TEXT("panpos"), Ch.CurPan)); }
	if (HasQ(Req, TEXT("tiltpos")))  { Ch.TgtTilt  = HucomsProtocol::ClampTilt(GetQInt(Req, TEXT("tiltpos"), Ch.CurTilt)); }
	if (HasQ(Req, TEXT("zoompos")))  { Ch.TgtZoom  = HucomsProtocol::ClampZoom(GetQInt(Req, TEXT("zoompos"), Ch.CurZoom)); }
	if (HasQ(Req, TEXT("focuspos"))) { Ch.TgtFocus = HucomsProtocol::ClampFocus(GetQInt(Req, TEXT("focuspos"), Ch.CurFocus)); }

	UE_LOG(LogHucomsSim, Verbose, TEXT("[Hucoms] :%d goptzfpos -> pan=%d tilt=%d zoom=%d"), Ch.HttpPort, Ch.TgtPan, Ch.TgtTilt, Ch.TgtZoom);
}

void UHucomsServerSubsystem::ApplySetCenter(FHucomsChannel& Ch, const FHttpServerRequest& Req)
{
	// 고정형 카메라는 센터링(조준)도 무시한다 — 설치 자세로 고정.
	if (Ch.bFixed)
	{
		return;
	}

	// 픽셀(1920x1080 논리 프레임) -> pan/tilt 델타. TAN 핀홀 + 구면 짐벌 모델로 실기 펌웨어 재현.
	// 기준은 '현재 위치(Cur)' - 실기는 지금 보고 있는 자세에서 센터링한다.
	const FString Type = GetQ(Req, TEXT("type"), TEXT("point"));

	float PixelX, PixelY;
	if (Type == TEXT("box"))
	{
		const float StartX = GetQFloat(Req, TEXT("center.startx"), 0.f);
		const float StartY = GetQFloat(Req, TEXT("center.starty"), 0.f);
		const float EndX   = GetQFloat(Req, TEXT("center.endx"),   HucomsProtocol::FrameW);
		const float EndY   = GetQFloat(Req, TEXT("center.endy"),   HucomsProtocol::FrameH);

		PixelX = (StartX + EndX) * 0.5f;
		PixelY = (StartY + EndY) * 0.5f;

		// 박스 크기 -> 줌인: 작은 박스일수록 더 줌인 (fake-camera 모델과 동일).
		const float BoxArea   = FMath::Abs((EndX - StartX) * (EndY - StartY));
		const float FrameArea = (float)HucomsProtocol::FrameW * (float)HucomsProtocol::FrameH;
		const float Coverage  = (FrameArea > 0.f) ? FMath::Clamp(BoxArea / FrameArea, 0.f, 1.f) : 1.f;
		Ch.TgtZoom = HucomsProtocol::ClampZoom(Ch.CurZoom + FMath::RoundToInt((1.f - Coverage) * 10000.f));
	}
	else // point
	{
		PixelX = GetQFloat(Req, TEXT("center.pointx"), HucomsProtocol::FrameW * 0.5f);
		PixelY = GetQFloat(Req, TEXT("center.pointy"), HucomsProtocol::FrameH * 0.5f);
	}

	// 델타 환산은 '현재 줌의 실효 FOV' 기준 — 광각 상수를 그대로 쓰면 화면상 같은 클릭
	// 오프셋이 줌 배율만큼 과이동한다(예: 10x 줌에서 10배 오버슈트로 엉뚱한 곳을 조준).
	// 세로 FOV 는 인자로 넘기지 않는다: rectilinear 광학은 가로·세로가 같은 초점거리 하나를
	// 공유하고(실기 실측에서 팬/틸트로 역산한 초점거리가 0.1% 이내 일치), 옛 WideVFovDeg 는
	// 선형 모델의 유물이라 그걸 쓰는 순간 세로가 30% 빗나갔다.
	const float CurHFov = HucomsProtocol::ZoomPosToHFov(Ch.CurZoom, WideHFovDeg);

	// 현재 틸트를 함께 넘긴다 — 팬 축이 월드 수직축이라 광축이 기울면 조준 기하가 달라진다
	// (가로 클릭에도 틸트가 조금 딸려 움직인다). 실기가 정확히 그렇게 동작한다.
	int32 PanDeltaCd, TiltDeltaCd;
	HucomsProtocol::PixelToDeltaCentideg(PixelX, PixelY, CurHFov, Ch.CurTilt / 100.f,
		PanDeltaCd, TiltDeltaCd, SetCenterFocalGain);

	Ch.TgtPan  = HucomsProtocol::WrapPan(Ch.CurPan + PanDeltaCd);
	// 실기 setcenter 규약(fov-convert.mjs ptzToWidePixel, cam-001 필드검증): 프레임에서 아래(y+)에
	// 있는 대상을 중앙으로 가져오려면 tiltpos 를 '올린다'(higher tiltpos = 카메라가 아래를 봄).
	// => 델타를 '더한다'. (기존 '-' 는 렌더 없는 fake-camera mock 의 미검증 부호를 답습한 상하반전 버그.)
	Ch.TgtTilt = HucomsProtocol::ClampTilt(Ch.CurTilt + TiltDeltaCd);

	UE_LOG(LogHucomsSim, Verbose, TEXT("[Hucoms] :%d setcenter(%s) px=(%.0f,%.0f) -> dPan=%d dTilt=%d => tgt pan=%d tilt=%d zoom=%d"),
		Ch.HttpPort, *Type, PixelX, PixelY, PanDeltaCd, TiltDeltaCd, Ch.TgtPan, Ch.TgtTilt, Ch.TgtZoom);
}

FString UHucomsServerSubsystem::BuildPtzPosBody(const FHucomsChannel& Ch) const
{
	// 'key = value' 텍스트. settle 판정에 panpos/tiltpos/zoompos 숫자 필수.
	return FString::Printf(TEXT("panpos = %d\ntiltpos = %d\nzoompos = %d\nfocuspos = %d\n"),
		Ch.CurPan, Ch.CurTilt, Ch.CurZoom, Ch.CurFocus);
}

FString UHucomsServerSubsystem::DebugStateString() const
{
	FString Out = FString::Printf(TEXT("Hucoms 채널 %d개:"), Channels.Num());
	for (const TSharedPtr<FHucomsChannel>& ChPtr : Channels)
	{
		const FHucomsChannel& Ch = *ChPtr;
		Out += FString::Printf(TEXT("\n  [:%d] cam=%s cur(pan=%d tilt=%d zoom=%d)"),
			Ch.HttpPort, *GetNameSafe(Ch.Camera.Get()), Ch.CurPan, Ch.CurTilt, Ch.CurZoom);
	}
	return Out;
}

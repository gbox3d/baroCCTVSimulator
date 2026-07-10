// Copyright gbox3d. All Rights Reserved.

#include "SceneControlSubsystem.h"

#include "HttpServerModule.h"
#include "HttpServerResponse.h"
#include "HttpServerRequest.h"    // FHttpServerRequest, EHttpServerRequestVerbs, EHttpServerResponseCodes
#include "HttpPath.h"
#include "IHttpRouter.h"

#include "Engine/World.h"
#include "EngineUtils.h"                     // TActorIterator
#include "Components/ChildActorComponent.h"
#include "UObject/UnrealType.h"              // FIntProperty, FArrayProperty, FindFProperty, FScriptArrayHelper
#include "UObject/SoftObjectPath.h"          // FSoftObjectPath::GetAssetName (Mesh_List 가 soft 참조일 때)

#include "PTZCamera.h"                        // APTZCamera (오버레이 투영 대상 카메라)
#include "HucomsServerSubsystem.h"           // 카메라 실효 포트 조회(GetCameraPorts)
#include "Camera/CameraComponent.h"          // UCameraComponent (광학 포즈)
#include "Camera/CameraTypes.h"              // FMinimalViewInfo, ECameraProjectionMode
#include "Kismet/GameplayStatics.h"          // GetViewProjectionMatrix
#include "Interfaces/IPluginManager.h"       // .uplugin VersionName 런타임 조회(플러그인 버전 노출)

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"

DEFINE_LOG_CATEGORY_STATIC(LogSceneCtrl, Log, All);

//======================================================================================
// 로컬 헬퍼 (카탈로그 상수 / JSON 조립·파싱 / BP 리플렉션 호출)
//======================================================================================
namespace
{
	// BP_Car / BP_Plate 실측 카탈로그(2026-07-07). scene-control-client.mjs SIM_CATALOG 와 동일 캐논.
	const TCHAR* KorList[] = { TEXT("가"), TEXT("나"), TEXT("다"), TEXT("라"), TEXT("마") };
	const TCHAR* PlateTypeNames[] = { TEXT("일반"), TEXT("영업용"), TEXT("전기차") };
	struct FColorDef { const TCHAR* Name; float R, G, B; };
	const FColorDef ColorDefs[] = {
		{ TEXT("화이트"), 0.94f, 0.94f, 0.90f },
		{ TEXT("블랙"),   0.04f, 0.04f, 0.05f },
		{ TEXT("실버"),   0.65f, 0.67f, 0.70f },
		{ TEXT("그레이"), 0.40f, 0.42f, 0.45f },
		{ TEXT("레드"),   0.45f, 0.10f, 0.12f },
		{ TEXT("옐로"),   0.90f, 0.78f, 0.30f },
		{ TEXT("그린"),   0.12f, 0.30f, 0.20f },
		{ TEXT("블루"),   0.15f, 0.22f, 0.60f },
	};

	struct FSlotInfo { FString Id; FString Label; FString Type; FTransform Xform; };

	FString SerializeJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	// application/json 응답 + 상태코드. baro_calory 는 sim 의 HTTP status 를 코드로 되돌린다
	// (scene-control-client.mjs: 404->NOT_FOUND, 409->OCCUPIED, 400->BAD_INPUT).
	TUniquePtr<FHttpServerResponse> JsonResp(const TSharedRef<FJsonObject>& Obj, EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok)
	{
		TUniquePtr<FHttpServerResponse> R = FHttpServerResponse::Create(SerializeJson(Obj), TEXT("application/json"));
		R->Code = Code;
		return R;
	}

	TUniquePtr<FHttpServerResponse> JsonError(EHttpServerResponseCodes Code, const FString& Msg)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("error"), Msg);
		return JsonResp(O, Code);
	}

	FString BodyToString(const FHttpServerRequest& Req)
	{
		if (Req.Body.Num() == 0) { return FString(); }
		FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Req.Body.GetData()), Req.Body.Num());
		return FString(Conv.Length(), Conv.Get());
	}

	// 빈 바디 => 빈 오브젝트({}) (reset 등). 파싱 실패 => nullptr (호출부가 400).
	TSharedPtr<FJsonObject> ParseBody(const FHttpServerRequest& Req)
	{
		const FString Str = BodyToString(Req);
		if (Str.IsEmpty()) { return MakeShared<FJsonObject>(); }
		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Str);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) { return nullptr; }
		return Root;
	}

	TSharedRef<FJsonObject> TransformToJson(const FTransform& X)
	{
		const FVector Loc = X.GetLocation();
		const FRotator Rot = X.Rotator();
		TSharedRef<FJsonObject> L = MakeShared<FJsonObject>();
		L->SetNumberField(TEXT("x"), Loc.X); L->SetNumberField(TEXT("y"), Loc.Y); L->SetNumberField(TEXT("z"), Loc.Z);
		TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetNumberField(TEXT("pitch"), Rot.Pitch); R->SetNumberField(TEXT("yaw"), Rot.Yaw); R->SetNumberField(TEXT("roll"), Rot.Roll);
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetObjectField(TEXT("location"), L);
		O->SetObjectField(TEXT("rotation"), R);
		return O;
	}

	FTransform ParseTransform(const TSharedPtr<FJsonObject>& O)
	{
		FVector Loc = FVector::ZeroVector;
		FRotator Rot = FRotator::ZeroRotator;
		const TSharedPtr<FJsonObject>* L;
		if (O->TryGetObjectField(TEXT("location"), L))
		{
			double x = 0, y = 0, z = 0;
			(*L)->TryGetNumberField(TEXT("x"), x); (*L)->TryGetNumberField(TEXT("y"), y); (*L)->TryGetNumberField(TEXT("z"), z);
			Loc = FVector(x, y, z);
		}
		const TSharedPtr<FJsonObject>* R;
		if (O->TryGetObjectField(TEXT("rotation"), R))
		{
			double p = 0, yw = 0, rl = 0;
			(*R)->TryGetNumberField(TEXT("pitch"), p); (*R)->TryGetNumberField(TEXT("yaw"), yw); (*R)->TryGetNumberField(TEXT("roll"), rl);
			Rot = FRotator(p, yw, rl);
		}
		return FTransform(Rot, Loc);
	}

	TSharedRef<FJsonObject> CarToJson(const FSimCarState& S)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), S.Id);
		if (S.SlotId.IsEmpty()) { O->SetField(TEXT("slotId"), MakeShared<FJsonValueNull>()); }
		else { O->SetStringField(TEXT("slotId"), S.SlotId); }
		O->SetObjectField(TEXT("transform"), TransformToJson(S.Transform));
		O->SetNumberField(TEXT("carType"), S.CarType);
		O->SetNumberField(TEXT("color"), S.Color);
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetNumberField(TEXT("type"), S.PlateType);
		P->SetStringField(TEXT("city"), S.City);
		P->SetStringField(TEXT("prefix"), S.Prefix);
		P->SetStringField(TEXT("kor"), S.Kor);
		P->SetStringField(TEXT("number"), S.Number);
		O->SetObjectField(TEXT("plate"), P);
		return O;
	}

	// "BP_ParkingSlot_5m_C" -> "5m"
	FString SlotTypeFromClass(const FString& ClassName, const FString& Prefix)
	{
		FString S = ClassName;
		S.RemoveFromStart(Prefix);
		S.RemoveFromStart(TEXT("_"));
		S.RemoveFromEnd(TEXT("_C"));
		return S.IsEmpty() ? TEXT("slot") : S;
	}

	bool NaturalLess(const FString& A, const FString& B)
	{
		int32 IA = 0;
		int32 IB = 0;
		while (IA < A.Len() && IB < B.Len())
		{
			const bool bDigitA = FChar::IsDigit(A[IA]);
			const bool bDigitB = FChar::IsDigit(B[IB]);
			if (bDigitA && bDigitB)
			{
				uint64 NA = 0;
				uint64 NB = 0;
				int32 DigitsA = 0;
				int32 DigitsB = 0;
				while (IA < A.Len() && FChar::IsDigit(A[IA]))
				{
					NA = NA * 10 + uint64(A[IA] - TCHAR('0'));
					++IA;
					++DigitsA;
				}
				while (IB < B.Len() && FChar::IsDigit(B[IB]))
				{
					NB = NB * 10 + uint64(B[IB] - TCHAR('0'));
					++IB;
					++DigitsB;
				}
				if (NA != NB) { return NA < NB; }
				if (DigitsA != DigitsB) { return DigitsA < DigitsB; }
				continue;
			}
			if (bDigitA != bDigitB) { return bDigitA; }

			const TCHAR CA = FChar::ToLower(A[IA]);
			const TCHAR CB = FChar::ToLower(B[IB]);
			if (CA != CB) { return CA < CB; }
			++IA;
			++IB;
		}
		return A.Len() < B.Len();
	}

	void CollectSlots(UWorld* World, const FString& Prefix, TArray<FSlotInfo>& Out)
	{
		if (!World) { return; }
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* A = *It;
			if (!A) { continue; }
			const FString CN = A->GetClass()->GetName();
			if (!CN.StartsWith(Prefix)) { continue; }
			FSlotInfo S;
			S.Id = A->GetName();                       // 배치 액터의 오브젝트명 = 안정 id(쿠킹 후 유지).
#if WITH_EDITOR
			S.Label = A->GetActorLabel();              // 에디터 Outliner 표시명. RPC 키로 쓰지 않고 UI 표시용으로만 노출.
#else
			S.Label = S.Id;                            // 패키지/런타임 빌드는 ActorLabel API가 없으므로 안정 id로 폴백.
#endif
			if (S.Label.IsEmpty()) { S.Label = S.Id; }
			S.Type = SlotTypeFromClass(CN, Prefix);
			S.Xform = A->GetActorTransform();
			Out.Add(S);
		}
		Out.Sort([](const FSlotInfo& A, const FSlotInfo& B) { return NaturalLess(A.Label, B.Label); });
	}

	// --- 차종 목록 ---
	// BP_Car 의 Mesh_List(StaticMesh 배열) 원소 순서가 selected_Car 인덱스의 유일한 진실이다
	// (Change_Car = Array_Get(Mesh_List, selected_Car) -> SetStaticMesh). 이름을 C++ 에 베끼면
	// BP 에서 차를 추가·재정렬하는 순간 조용히 어긋나므로, CDO 를 리플렉션으로 읽어 그대로 노출한다.
	const TCHAR* CarMeshListProp = TEXT("Mesh_List");

	// Mesh_List 를 못 읽었을 때만 쓰는 과거 값(2026-07-07 실측). 이름 없이 인덱스만 노출된다.
	constexpr int32 LegacyCarCount = 23;

	// 에셋명 -> 표시명. "현대_쏘나타" -> "현대 쏘나타", "기아_봉고_탑차" -> "기아 봉고 탑차".
	FString CarAssetToDisplayName(const FString& AssetName)
	{
		return AssetName.Replace(TEXT("_"), TEXT(" "));
	}

	// 실패 시 false + Out 비움. 성공 시 Out[i] = 인덱스 i 의 에셋명.
	bool CollectCarAssetNames(UClass* CarCls, TArray<FString>& Out)
	{
		Out.Reset();
		if (!CarCls) { return false; }
		UObject* CDO = CarCls->GetDefaultObject();
		FArrayProperty* Arr = FindFProperty<FArrayProperty>(CarCls, CarMeshListProp);
		if (!CDO || !Arr) { return false; }

		// FSoftObjectProperty 는 FObjectPropertyBase 를 상속하므로 soft 를 먼저 본다.
		FSoftObjectProperty* SoftInner = CastField<FSoftObjectProperty>(Arr->Inner);
		FObjectPropertyBase* HardInner = CastField<FObjectPropertyBase>(Arr->Inner);
		if (!SoftInner && !HardInner) { return false; }

		FScriptArrayHelper Helper(Arr, Arr->ContainerPtrToValuePtr<void>(CDO));
		const int32 N = Helper.Num();
		Out.Reserve(N);
		for (int32 i = 0; i < N; ++i)
		{
			const uint8* Elem = Helper.GetRawPtr(i);
			if (SoftInner)
			{
				// soft 참조면 메시를 로드하지 않고 경로에서 이름만 취한다.
				Out.Add(SoftInner->GetPropertyValue(Elem).ToSoftObjectPath().GetAssetName());
			}
			else
			{
				const UObject* Mesh = HardInner->GetObjectPropertyValue(Elem);
				Out.Add(Mesh ? Mesh->GetName() : FString());
			}
		}
		return N > 0;
	}

	bool SetIntProp(UObject* Obj, const TCHAR* Name, int32 Val)
	{
		if (!Obj) { return false; }
		FIntProperty* P = FindFProperty<FIntProperty>(Obj->GetClass(), Name);
		if (!P) { return false; }
		P->SetPropertyValue_InContainer(Obj, Val);
		return true;
	}

	// 단일 int32 파라미터 BP 함수 호출(Change_Car/Change_Color/Change_Plate). 파라미터 struct 레이아웃이
	// UFunction 시그니처(int32 1개)와 일치해야 한다.
	void CallIntFn(AActor* A, const TCHAR* FnName, int32 Val)
	{
		if (!A) { return; }
		if (UFunction* Fn = A->FindFunction(FName(FnName)))
		{
			struct { int32 V; } Params{ Val };
			A->ProcessEvent(Fn, &Params);
		}
	}

	// 앞/뒤 자식 BP_Plate 의 Change_Text(FString) 호출로 번호판 글자 설정.
	void SetPlateTextOnChildren(AActor* Car, const FString& PlateStr)
	{
		if (!Car) { return; }
		TArray<UChildActorComponent*> Kids;
		Car->GetComponents<UChildActorComponent>(Kids);
		for (UChildActorComponent* K : Kids)
		{
			AActor* Plate = K ? K->GetChildActor() : nullptr;
			if (!Plate) { continue; }
			if (UFunction* Fn = Plate->FindFunction(FName(TEXT("Change_Text"))))
			{
				struct { FString S; } Params{ PlateStr };
				Plate->ProcessEvent(Fn, &Params);
			}
		}
	}

	// 한국 신형 번호판(앞 3자리 + 한글 + 뒤 4자리)으로 자릿수를 정규화한다.
	// BP_Plate::Change_Text 파싱(Num_3=sub(0,3), Kor=sub(3), Num_4=sub(4,4))이 이 자릿수를 전제하므로
	// 앞자리가 2자리면 Num_3 칸에 한글이 섞여 "###" 대신 "##한글"이 되어 유럽식처럼 쪼개진다.
	// 저장값=렌더값 일치를 위해 spawn/patch 시 S 자체를 정규화한다(응답·웹UI 표시도 정규화값).
	void NormalizeKoreanPlate(FSimCarState& S)
	{
		auto Digits = [](const FString& In, int32 Width)
		{
			FString D;
			for (int32 i = 0; i < In.Len(); ++i) { if (FChar::IsDigit(In[i])) { D.AppendChar(In[i]); } }
			if (D.Len() > Width) { D = D.Right(Width); }
			while (D.Len() < Width) { D = FString(TEXT("0")) + D; }  // 부족분 좌측 0패딩(정상 입력이면 미발생)
			return D;
		};
		S.Prefix = Digits(S.Prefix, 3);
		S.Number = Digits(S.Number, 4);
		const FString K = S.Kor.Left(1);
		S.Kor = K.IsEmpty() ? FString(TEXT("가")) : K;
	}

	//----------------------------------------------------------------------------------
	// 오버레이 투영: 카메라 열거 / 카메라→JSON / 월드점→픽셀(그라운드-트루스)
	//----------------------------------------------------------------------------------
	struct FCamEntry { APTZCamera* Cam = nullptr; int32 Http = 0; int32 Mjpeg = 0; };

	// 레벨의 APTZCamera 를 열거하고 각 카메라의 실효 Hucoms 포트를 채워 이름순 정렬.
	void CollectCameras(UWorld* World, TArray<FCamEntry>& Out)
	{
		if (!World) { return; }
		UHucomsServerSubsystem* Hu = World->GetSubsystem<UHucomsServerSubsystem>();
		for (TActorIterator<APTZCamera> It(World); It; ++It)
		{
			APTZCamera* C = *It;
			if (!C) { continue; }
			FCamEntry E; E.Cam = C; E.Http = C->HucomsHttpPort; E.Mjpeg = C->HucomsMjpegPort;
			int32 H = 0, M = 0;
			if (Hu && Hu->GetCameraPorts(C, H, M)) { E.Http = H; E.Mjpeg = M; }  // 채널이 실제 바인딩한 포트 우선
			Out.Add(E);
		}
		Out.Sort([](const FCamEntry& A, const FCamEntry& B) { return A.Cam->GetName() < B.Cam->GetName(); });
	}

	// 광학 포즈 = CameraComp 월드 트랜스폼(pan/tilt 가 이미 구워진, 씬캡처가 실제 렌더하는 시점).
	FTransform CameraOpticalTransform(const APTZCamera* C)
	{
		if (C && C->CameraComp) { return C->CameraComp->GetComponentTransform(); }
		return C ? C->GetActorTransform() : FTransform::Identity;
	}

	// 오버레이 투영에 필요한 것 중 "Hucoms 로 재현 불가능한" 부분만 노출한다 = 외부 파라미터(extrinsic).
	//   mount.location : 광학중심 월드 위치. 피벗이 Root 와 동일위치(레버암 0)라 pan/tilt 에 불변 = 고정.
	//   mount.baseYaw  : 설치 방위(pan=0 기준 yaw). 광학 yaw = baseYaw + CurrentPan 이므로 역산.
	//   wideHFovDeg    : 1x 수평 FOV(hfovFromZoomPos 스케일 기준).
	// FOV·PTZ 는 실카메라와 동일하게 Hucoms(getptzfpos + zoompos)에서 가져온다 → 여기서 중복 노출하지 않음.
	TSharedRef<FJsonObject> CameraToJson(const FCamEntry& E)
	{
		APTZCamera* C = E.Cam;
		const FTransform X = CameraOpticalTransform(C);
		const FVector Loc = X.GetLocation();
		const double BaseYaw = X.Rotator().Yaw - C->GetCurrentPan();

		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), C->GetName());
		O->SetNumberField(TEXT("hucomsPort"), E.Http);
		O->SetNumberField(TEXT("mjpegPort"), E.Mjpeg);
		O->SetBoolField(TEXT("fixed"), C->bFixedMode);

		TSharedRef<FJsonObject> Mount = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> L = MakeShared<FJsonObject>();
		L->SetNumberField(TEXT("x"), Loc.X); L->SetNumberField(TEXT("y"), Loc.Y); L->SetNumberField(TEXT("z"), Loc.Z);
		Mount->SetObjectField(TEXT("location"), L);
		Mount->SetNumberField(TEXT("baseYaw"), BaseYaw);
		O->SetObjectField(TEXT("mount"), Mount);

		O->SetNumberField(TEXT("wideHFovDeg"), C->BaseFOV);
		return O;
	}

	// UE 실제 뷰·투영행렬로 월드점→픽셀(FSceneView::ProjectWorldToScreen 과 동일 매핑).
	TSharedRef<FJsonValue> ProjectPointJson(const FMatrix& VP, const FVector& P, int32 W, int32 H)
	{
		const FVector4 R = VP.TransformFVector4(FVector4(P, 1.f));
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		const bool bBehind = (R.W <= KINDA_SMALL_NUMBER);
		bool bVisible = false;
		double X = 0.0, Y = 0.0;
		if (!bBehind)
		{
			const double Nx = R.X / R.W, Ny = R.Y / R.W;
			X = (Nx * 0.5 + 0.5) * W;
			Y = (0.5 - Ny * 0.5) * H;                       // NDC Y up -> 픽셀 Y down
			bVisible = (X >= 0 && X <= W && Y >= 0 && Y <= H);
		}
		O->SetNumberField(TEXT("x"), X);
		O->SetNumberField(TEXT("y"), Y);
		O->SetBoolField(TEXT("visible"), bVisible);
		O->SetBoolField(TEXT("behind"), bBehind);
		return MakeShared<FJsonValueObject>(O);
	}
}

//======================================================================================
// Subsystem lifecycle
//======================================================================================
bool USceneControlSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void USceneControlSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	StartServer();
}

void USceneControlSubsystem::Deinitialize()
{
	StopServer();
	Super::Deinitialize();
}

//======================================================================================
// HTTP server start/stop (월드 전역 1포트)
//======================================================================================
void USceneControlSubsystem::StartServer()
{
	if (bStarted) { return; }

	ResolveCarClass();   // 실패해도 서버는 뜬다(스폰 시 500 반환).

	FHttpServerModule& Http = FHttpServerModule::Get();
	Router = Http.GetHttpRouter(ScenePort, /*bFailOnBindFailure=*/true);
	if (!Router.IsValid())
	{
		UE_LOG(LogSceneCtrl, Error, TEXT("[Scene] 라우터 바인드 실패 (port %d) — 포트 중복/점유 확인."), ScenePort);
		return;
	}

	auto Bind = [this](const TCHAR* Path, EHttpServerRequestVerbs Verbs,
		bool (USceneControlSubsystem::*Fn)(const FHttpServerRequest&, const FHttpResultCallback&))
	{
		FHttpRouteHandle H = Router->BindRoute(FHttpPath(FString(Path)), Verbs,
			FHttpRequestHandler::CreateLambda(
				[this, Fn](const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
				{
					return (this->*Fn)(Req, OnComplete);
				}));
		if (H.IsValid()) { Routes.Add(H); }
		else { UE_LOG(LogSceneCtrl, Error, TEXT("[Scene] 라우트 바인딩 실패: %s"), Path); }
	};

	using EV = EHttpServerRequestVerbs;
	Bind(TEXT("/scene/catalog"),   EV::VERB_GET, &USceneControlSubsystem::HandleCatalog);
	Bind(TEXT("/scene/slots"),     EV::VERB_GET, &USceneControlSubsystem::HandleSlots);
	Bind(TEXT("/scene/cameras"),   EV::VERB_GET, &USceneControlSubsystem::HandleCameras);
	Bind(TEXT("/scene/project"),   EV::VERB_POST, &USceneControlSubsystem::HandleProject);
	Bind(TEXT("/scene/cars"),      EV::VERB_GET | EV::VERB_POST, &USceneControlSubsystem::HandleCars);
	Bind(TEXT("/scene/cars/:id"),  EV::VERB_GET | EV::VERB_PATCH | EV::VERB_DELETE, &USceneControlSubsystem::HandleCarById);
	Bind(TEXT("/scene/reset"),     EV::VERB_POST, &USceneControlSubsystem::HandleReset);

	Http.StartAllListeners();
	bStarted = true;
	UE_LOG(LogSceneCtrl, Log, TEXT("[Scene] 씬 제어 서버 시작 — http://127.0.0.1:%d/scene/*"), ScenePort);
}

void USceneControlSubsystem::StopServer()
{
	// 스폰한 차량 정리(월드 종료 시).
	for (TPair<FString, FSimCarState>& Pair : Cars)
	{
		if (AActor* A = Pair.Value.Actor.Get()) { A->Destroy(); }
	}
	Cars.Reset();
	SlotOccupancy.Reset();

	if (Router.IsValid())
	{
		for (const FHttpRouteHandle& H : Routes)
		{
			if (H.IsValid()) { Router->UnbindRoute(H); }
		}
	}
	Routes.Reset();
	Router.Reset();

	if (bStarted)
	{
		// StopAllListeners 는 전역(HucomsServerSubsystem 과 공유)이나 idempotent — 월드 종료 시 둘 다 정리된다.
		FHttpServerModule::Get().StopAllListeners();
		bStarted = false;
		UE_LOG(LogSceneCtrl, Log, TEXT("[Scene] 씬 제어 서버 정지."));
	}
}

//======================================================================================
// 스폰 / BP 반영
//======================================================================================
UClass* USceneControlSubsystem::ResolveCarClass()
{
	if (CarClass) { return CarClass; }
	UClass* Loaded = StaticLoadClass(AActor::StaticClass(), nullptr, *CarBlueprintPath);
	if (!Loaded)
	{
		UE_LOG(LogSceneCtrl, Error, TEXT("[Scene] 차량 BP 클래스 로드 실패: %s"), *CarBlueprintPath);
		return nullptr;
	}
	CarClass = Loaded;
	return CarClass;
}

const TArray<FString>& USceneControlSubsystem::GetCarAssetNames()
{
	if (!bCarAssetNamesResolved)
	{
		bCarAssetNamesResolved = true;
		// ResolveCarClass() 가 BP_Car 를 로드한다 — 첫 스폰 히치가 이 시점으로 앞당겨질 뿐이다.
		if (!CollectCarAssetNames(ResolveCarClass(), CarAssetNames))
		{
			UE_LOG(LogSceneCtrl, Warning, TEXT("[Scene] BP_Car.%s 읽기 실패 — 차종명 없이 인덱스만 노출한다."), CarMeshListProp);
		}
	}
	return CarAssetNames;
}

int32 USceneControlSubsystem::ClampCarType(int32 InCarType)
{
	const int32 N = GetCarAssetNames().Num();
	return FMath::Clamp(InCarType, 0, (N > 0 ? N : LegacyCarCount) - 1);
}

AActor* USceneControlSubsystem::SpawnCarActor(const FTransform& Xform)
{
	UWorld* World = GetWorld();
	UClass* Cls = ResolveCarClass();
	if (!World || !Cls) { return nullptr; }
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return World->SpawnActor<AActor>(Cls, Xform.GetLocation(), Xform.Rotator(), Params);
}

void USceneControlSubsystem::ApplyToActor(AActor* Car, const FSimCarState& S)
{
	if (!Car) { return; }
	// selected_Color/selected_Plate 를 먼저 세팅하면 Change_Car(내부에서 Change_Color(selected_Color)+
	// Change_Plate(selected_Plate)+Update_Plate 호출)가 이 값을 반영한다. 이어 명시 호출로 확실히 재적용.
	SetIntProp(Car, TEXT("selected_Color"), S.Color);
	SetIntProp(Car, TEXT("selected_Plate"), S.PlateType);
	CallIntFn(Car, TEXT("Change_Car"), S.CarType);
	CallIntFn(Car, TEXT("Change_Color"), S.Color);
	CallIntFn(Car, TEXT("Change_Plate"), S.PlateType);
	// 번호판 글자. 한국 신형(앞3자리+한글+뒤4자리)으로 정규화된 S 를 "###가####" 로 연결해 넘긴다.
	// (BP_Plate::Change_Text 파싱: Num_3=sub(0,3), Kor=sub(3), Num_4=sub(4,4) — NormalizeKoreanPlate 참조.)
	const FString PlateStr = S.Prefix + S.Kor + S.Number;
	SetPlateTextOnChildren(Car, PlateStr);
}

//======================================================================================
// 라우트 핸들러
//======================================================================================
bool USceneControlSubsystem::HandleCatalog(const FHttpServerRequest& /*Req*/, const FHttpResultCallback& OnComplete)
{
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	FString Level;
	if (UWorld* W = GetWorld()) { Level = W->GetMapName(); Level.RemoveFromStart(W->StreamingLevelsPrefix); }
	O->SetStringField(TEXT("level"), Level);

	// 플러그인 버전 — .uplugin VersionName 을 단일 소스로 읽어 노출(웹 /simulator 에서 확인용).
	FString PluginVer = TEXT("?");
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("baroCCTVSimulator"))) { PluginVer = Plugin->GetDescriptor().VersionName; }
	O->SetStringField(TEXT("pluginVersion"), PluginVer);

	// 차종: BP_Car.Mesh_List 를 읽어 index/name/asset 을 싣는다. carCount 는 그 길이(구 계약 유지).
	const TArray<FString>& CarAssets = GetCarAssetNames();
	O->SetNumberField(TEXT("carCount"), CarAssets.Num() > 0 ? CarAssets.Num() : LegacyCarCount);

	TArray<TSharedPtr<FJsonValue>> CarsJson;
	for (int32 i = 0; i < CarAssets.Num(); ++i)
	{
		TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
		C->SetNumberField(TEXT("index"), i);
		C->SetStringField(TEXT("name"), CarAssetToDisplayName(CarAssets[i]));
		C->SetStringField(TEXT("asset"), CarAssets[i]);
		CarsJson.Add(MakeShared<FJsonValueObject>(C));
	}
	O->SetArrayField(TEXT("cars"), CarsJson);

	TArray<TSharedPtr<FJsonValue>> Colors;
	for (int32 i = 0; i < UE_ARRAY_COUNT(ColorDefs); ++i)
	{
		TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
		C->SetNumberField(TEXT("index"), i);
		C->SetStringField(TEXT("name"), ColorDefs[i].Name);
		TArray<TSharedPtr<FJsonValue>> RGB = {
			MakeShared<FJsonValueNumber>(ColorDefs[i].R),
			MakeShared<FJsonValueNumber>(ColorDefs[i].G),
			MakeShared<FJsonValueNumber>(ColorDefs[i].B) };
		C->SetArrayField(TEXT("rgb"), RGB);
		Colors.Add(MakeShared<FJsonValueObject>(C));
	}
	O->SetArrayField(TEXT("colors"), Colors);

	TArray<TSharedPtr<FJsonValue>> Plates;
	for (int32 i = 0; i < UE_ARRAY_COUNT(PlateTypeNames); ++i)
	{
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetNumberField(TEXT("index"), i);
		P->SetStringField(TEXT("name"), PlateTypeNames[i]);
		Plates.Add(MakeShared<FJsonValueObject>(P));
	}
	O->SetArrayField(TEXT("plateTypes"), Plates);

	TArray<TSharedPtr<FJsonValue>> Kor;
	for (int32 i = 0; i < UE_ARRAY_COUNT(KorList); ++i) { Kor.Add(MakeShared<FJsonValueString>(KorList[i])); }
	O->SetArrayField(TEXT("korList"), Kor);

	OnComplete(JsonResp(O));
	return true;
}

bool USceneControlSubsystem::HandleSlots(const FHttpServerRequest& /*Req*/, const FHttpResultCallback& OnComplete)
{
	TArray<FSlotInfo> Slots;
	CollectSlots(GetWorld(), ParkingSlotClassPrefix, Slots);

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FSlotInfo& S : Slots)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), S.Id);
		O->SetStringField(TEXT("label"), S.Label);
		O->SetStringField(TEXT("type"), S.Type);
		O->SetObjectField(TEXT("transform"), TransformToJson(S.Xform));
		const FString* CarId = SlotOccupancy.Find(S.Id);
		O->SetBoolField(TEXT("occupied"), CarId != nullptr);
		if (CarId) { O->SetStringField(TEXT("carId"), *CarId); }
		else { O->SetField(TEXT("carId"), MakeShared<FJsonValueNull>()); }
		Arr.Add(MakeShared<FJsonValueObject>(O));
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("slots"), Arr);
	OnComplete(JsonResp(Root));
	return true;
}

bool USceneControlSubsystem::HandleCameras(const FHttpServerRequest& /*Req*/, const FHttpResultCallback& OnComplete)
{
	TArray<FCamEntry> Cams;
	CollectCameras(GetWorld(), Cams);

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FCamEntry& E : Cams) { Arr.Add(MakeShared<FJsonValueObject>(CameraToJson(E))); }

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("cameras"), Arr);
	OnComplete(JsonResp(Root));
	return true;
}

bool USceneControlSubsystem::HandleProject(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FJsonObject> Body = ParseBody(Req);
	if (!Body.IsValid()) { OnComplete(JsonError(EHttpServerResponseCodes::BadRequest, TEXT("JSON 파싱 실패"))); return true; }

	// 카메라 선택: cameraId(액터명) 우선, 없으면 hucomsPort. 둘 다 없으면 첫 카메라.
	FString CamId; Body->TryGetStringField(TEXT("cameraId"), CamId);
	int32 WantPort = 0; const bool bHasPort = Body->TryGetNumberField(TEXT("hucomsPort"), WantPort);

	TArray<FCamEntry> Cams;
	CollectCameras(GetWorld(), Cams);
	const FCamEntry* Found = Cams.FindByPredicate([&](const FCamEntry& E)
	{
		if (!CamId.IsEmpty()) { return E.Cam->GetName() == CamId; }
		if (bHasPort) { return E.Http == WantPort; }
		return false;
	});
	if (!Found && Cams.Num() > 0 && CamId.IsEmpty() && !bHasPort) { Found = &Cams[0]; }
	if (!Found) { OnComplete(JsonError(EHttpServerResponseCodes::NotFound, TEXT("카메라를 찾을 수 없습니다(cameraId/hucomsPort)"))); return true; }

	// 해상도(선택) — 없으면 논리 1920x1080. 수평 FOV + 종횡비로 뷰·투영행렬을 구성한다.
	int32 W = 1920, H = 1080;
	const TSharedPtr<FJsonObject>* ResObj;
	if (Body->TryGetObjectField(TEXT("resolution"), ResObj))
	{
		int32 rw = 0, rh = 0;
		if ((*ResObj)->TryGetNumberField(TEXT("width"), rw) && rw > 0) { W = rw; }
		if ((*ResObj)->TryGetNumberField(TEXT("height"), rh) && rh > 0) { H = rh; }
	}

	APTZCamera* Cam = Found->Cam;
	FMinimalViewInfo View;
	const FTransform X = CameraOpticalTransform(Cam);
	View.Location = X.GetLocation();
	View.Rotation = X.Rotator();
	View.FOV = Cam->GetCurrentFOV();                     // 수평 FOV
	View.AspectRatio = (float)W / (float)H;
	View.bConstrainAspectRatio = true;
	View.ProjectionMode = ECameraProjectionMode::Perspective;

	FMatrix ViewM, ProjM, VP;
	UGameplayStatics::GetViewProjectionMatrix(View, ViewM, ProjM, VP);

	TArray<TSharedPtr<FJsonValue>> OutPts;
	const TArray<TSharedPtr<FJsonValue>>* Pts;
	if (Body->TryGetArrayField(TEXT("points"), Pts))
	{
		for (const TSharedPtr<FJsonValue>& V : *Pts)
		{
			const TSharedPtr<FJsonObject> PO = V->AsObject();
			if (!PO.IsValid()) { continue; }
			double x = 0, y = 0, z = 0;
			PO->TryGetNumberField(TEXT("x"), x); PO->TryGetNumberField(TEXT("y"), y); PO->TryGetNumberField(TEXT("z"), z);
			OutPts.Add(ProjectPointJson(VP, FVector(x, y, z), W, H));
		}
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("cameraId"), Cam->GetName());
	Root->SetNumberField(TEXT("fovDeg"), Cam->GetCurrentFOV());
	TSharedRef<FJsonObject> Res = MakeShared<FJsonObject>();
	Res->SetNumberField(TEXT("width"), W); Res->SetNumberField(TEXT("height"), H);
	Root->SetObjectField(TEXT("resolution"), Res);
	Root->SetArrayField(TEXT("points"), OutPts);
	OnComplete(JsonResp(Root));
	return true;
}

bool USceneControlSubsystem::HandleCars(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	// GET = 목록
	if (Req.Verb == EHttpServerRequestVerbs::VERB_GET)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const TPair<FString, FSimCarState>& Pair : Cars) { Arr.Add(MakeShared<FJsonValueObject>(CarToJson(Pair.Value))); }
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetArrayField(TEXT("cars"), Arr);
		OnComplete(JsonResp(Root));
		return true;
	}

	// POST = 스폰
	TSharedPtr<FJsonObject> Body = ParseBody(Req);
	if (!Body.IsValid()) { OnComplete(JsonError(EHttpServerResponseCodes::BadRequest, TEXT("JSON 파싱 실패"))); return true; }

	FSimCarState S;
	int32 CarType = 0, Color = 0;
	Body->TryGetNumberField(TEXT("carType"), CarType);
	Body->TryGetNumberField(TEXT("color"), Color);
	S.CarType = ClampCarType(CarType);
	S.Color = FMath::Clamp(Color, 0, 7);

	const TSharedPtr<FJsonObject>* PlateObj;
	if (Body->TryGetObjectField(TEXT("plate"), PlateObj))
	{
		int32 PT = 0; (*PlateObj)->TryGetNumberField(TEXT("type"), PT); S.PlateType = FMath::Clamp(PT, 0, 2);
		(*PlateObj)->TryGetStringField(TEXT("city"), S.City);
		(*PlateObj)->TryGetStringField(TEXT("prefix"), S.Prefix);
		(*PlateObj)->TryGetStringField(TEXT("kor"), S.Kor);
		(*PlateObj)->TryGetStringField(TEXT("number"), S.Number);
	}

	bool bForce = false; Body->TryGetBoolField(TEXT("force"), bForce);

	// 배치: slotId(슬롯) 우선, 없으면 transform(자유 좌표).
	FString SlotId;
	Body->TryGetStringField(TEXT("slotId"), SlotId);
	FTransform Xform = FTransform::Identity;
	if (!SlotId.IsEmpty())
	{
		TArray<FSlotInfo> Slots; CollectSlots(GetWorld(), ParkingSlotClassPrefix, Slots);
		const FSlotInfo* Found = Slots.FindByPredicate([&](const FSlotInfo& X) { return X.Id == SlotId; });
		if (!Found) { OnComplete(JsonError(EHttpServerResponseCodes::NotFound, FString::Printf(TEXT("주차면 없음: %s"), *SlotId))); return true; }
		if (SlotOccupancy.Contains(SlotId) && !bForce) { OnComplete(JsonError(EHttpServerResponseCodes::Conflict, FString::Printf(TEXT("주차면 점유됨: %s"), *SlotId))); return true; }
		Xform = Found->Xform;
		S.SlotId = SlotId;
	}
	else
	{
		const TSharedPtr<FJsonObject>* TObj;
		if (Body->TryGetObjectField(TEXT("transform"), TObj)) { Xform = ParseTransform(*TObj); }
		else { OnComplete(JsonError(EHttpServerResponseCodes::BadRequest, TEXT("slotId 또는 transform 필요"))); return true; }
	}

	AActor* Car = SpawnCarActor(Xform);
	if (!Car) { OnComplete(JsonError(EHttpServerResponseCodes::ServerError, TEXT("차량 스폰 실패 (BP 로드 확인)"))); return true; }

	S.Id = FString::Printf(TEXT("car-%02d"), ++CarSeq);
	S.Transform = Xform;
	S.Actor = Car;
	NormalizeKoreanPlate(S);
	ApplyToActor(Car, S);

	Cars.Add(S.Id, S);
	if (!S.SlotId.IsEmpty()) { SlotOccupancy.Add(S.SlotId, S.Id); }

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetObjectField(TEXT("car"), CarToJson(S));
	OnComplete(JsonResp(Root));
	return true;
}

bool USceneControlSubsystem::HandleCarById(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	const FString Id = Req.PathParams.FindRef(TEXT("id"));
	FSimCarState* S = Cars.Find(Id);
	if (!S) { OnComplete(JsonError(EHttpServerResponseCodes::NotFound, FString::Printf(TEXT("차량 없음: %s"), *Id))); return true; }

	if (Req.Verb == EHttpServerRequestVerbs::VERB_GET)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetObjectField(TEXT("car"), CarToJson(*S));
		OnComplete(JsonResp(Root));
		return true;
	}

	if (Req.Verb == EHttpServerRequestVerbs::VERB_DELETE)
	{
		if (AActor* A = S->Actor.Get()) { A->Destroy(); }
		if (!S->SlotId.IsEmpty()) { SlotOccupancy.Remove(S->SlotId); }
		Cars.Remove(Id);
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("removed"), Id);
		OnComplete(JsonResp(Root));
		return true;
	}

	// PATCH = 부분 갱신(넘긴 필드만). S 는 TMap 슬롯 포인터 — Remove 를 안 하므로 유효.
	TSharedPtr<FJsonObject> Body = ParseBody(Req);
	if (!Body.IsValid()) { OnComplete(JsonError(EHttpServerResponseCodes::BadRequest, TEXT("JSON 파싱 실패"))); return true; }

	int32 Tmp;
	if (Body->TryGetNumberField(TEXT("carType"), Tmp)) { S->CarType = ClampCarType(Tmp); }
	if (Body->TryGetNumberField(TEXT("color"), Tmp)) { S->Color = FMath::Clamp(Tmp, 0, 7); }
	const TSharedPtr<FJsonObject>* PlateObj;
	if (Body->TryGetObjectField(TEXT("plate"), PlateObj))
	{
		int32 PT; if ((*PlateObj)->TryGetNumberField(TEXT("type"), PT)) { S->PlateType = FMath::Clamp(PT, 0, 2); }
		FString Str;
		if ((*PlateObj)->TryGetStringField(TEXT("city"), Str)) { S->City = Str; }
		if ((*PlateObj)->TryGetStringField(TEXT("prefix"), Str)) { S->Prefix = Str; }
		if ((*PlateObj)->TryGetStringField(TEXT("kor"), Str)) { S->Kor = Str; }
		if ((*PlateObj)->TryGetStringField(TEXT("number"), Str)) { S->Number = Str; }
	}

	// 슬롯 이동
	FString NewSlot;
	if (Body->TryGetStringField(TEXT("slotId"), NewSlot) && NewSlot != S->SlotId)
	{
		bool bForce = false; Body->TryGetBoolField(TEXT("force"), bForce);
		if (!NewSlot.IsEmpty())
		{
			TArray<FSlotInfo> Slots; CollectSlots(GetWorld(), ParkingSlotClassPrefix, Slots);
			const FSlotInfo* Found = Slots.FindByPredicate([&](const FSlotInfo& X) { return X.Id == NewSlot; });
			if (!Found) { OnComplete(JsonError(EHttpServerResponseCodes::NotFound, FString::Printf(TEXT("주차면 없음: %s"), *NewSlot))); return true; }
			if (SlotOccupancy.Contains(NewSlot) && !bForce) { OnComplete(JsonError(EHttpServerResponseCodes::Conflict, FString::Printf(TEXT("주차면 점유됨: %s"), *NewSlot))); return true; }
			if (!S->SlotId.IsEmpty()) { SlotOccupancy.Remove(S->SlotId); }
			S->SlotId = NewSlot;
			S->Transform = Found->Xform;
			SlotOccupancy.Add(NewSlot, Id);
			if (AActor* A = S->Actor.Get()) { A->SetActorTransform(Found->Xform); }
		}
		else
		{
			if (!S->SlotId.IsEmpty()) { SlotOccupancy.Remove(S->SlotId); }
			S->SlotId = TEXT("");
		}
	}

	NormalizeKoreanPlate(*S);
	if (AActor* A = S->Actor.Get()) { ApplyToActor(A, *S); }

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetObjectField(TEXT("car"), CarToJson(*S));
	OnComplete(JsonResp(Root));
	return true;
}

bool USceneControlSubsystem::HandleReset(const FHttpServerRequest& /*Req*/, const FHttpResultCallback& OnComplete)
{
	const int32 N = Cars.Num();
	for (TPair<FString, FSimCarState>& Pair : Cars)
	{
		if (AActor* A = Pair.Value.Actor.Get()) { A->Destroy(); }
	}
	Cars.Reset();
	SlotOccupancy.Reset();

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("cleared"), N);
	OnComplete(JsonResp(Root));
	return true;
}

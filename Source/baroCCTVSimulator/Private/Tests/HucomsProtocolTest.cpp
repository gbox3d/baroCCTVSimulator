// setcenter 조준 기하 테스트.
//
// 이 모듈에는 오랫동안 테스트가 없었고, 그 대가로 "픽셀↔각도는 선형"이라는 **검증된 적 없는
// 가정**이 몇 년간 진실 행세를 했다(mock → sim → 문서). 2026-07-14 실기(cam-001)를 105샘플로
// 실측해 그 가정이 거짓임을 확인했고, 아래 기대값은 **그 실측 텔레메트리 그대로**다.
// 따라서 이 테스트는 "코드가 코드와 일치하는가"가 아니라 "시뮬이 실물과 일치하는가"를 묻는다.
//
// 측정 도구: baro_calrory/tools/centering_calib (measure_center.py / analyze.py)

#include "Misc/AutomationTest.h"
#include "HucomsProtocol.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// 실측 당시 펌웨어가 쓰던 와이드 초점거리(텔레메트리 역산 f=1746.5px)를 HFOV 로 환산한 값.
	// 아래 기대값들은 이 화각·이 틸트에서 실카메라가 실제로 회전한 양이다.
	const float RealWideHFov = FMath::RadiansToDegrees(2.f * FMath::Atan(960.f / 1746.5f));
	constexpr float RealTiltDeg = 16.81f;

	FString Describe(int32 Got, int32 Want)
	{
		return FString::Printf(TEXT("got %d cd, expected %d cd"), Got, Want);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHucomsZoomHfovTable,
	"baroCCTVSimulator.Hucoms.Zoom HFOV table anchors interpolation clamp and scaling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHucomsZoomHfovTable::RunTest(const FString&)
{
	constexpr float CanonicalWide = 57.14f;
	constexpr float AlternateWide = 60.f;
	constexpr float Tolerance = 0.0001f;

	TestEqual(TEXT("공용 zoom-HFOV 앵커 수"), HucomsProtocol::ZoomHfovTableCount, 13);
	for (int32 i = 0; i < HucomsProtocol::ZoomHfovTableCount; ++i)
	{
		const HucomsProtocol::FZoomHfovPoint& Point = HucomsProtocol::ZoomHfovTable[i];
		TestTrue(
			FString::Printf(TEXT("앵커 %d의 canonical HFOV"), i),
			FMath::IsNearlyEqual(
				HucomsProtocol::ZoomPosToHFov(Point.ZoomPos, CanonicalWide),
				Point.HFovDeg,
				Tolerance));

		if (i > 0)
		{
			TestTrue(
				FString::Printf(TEXT("앵커 %d zoomPos 단조 증가"), i),
				Point.ZoomPos > HucomsProtocol::ZoomHfovTable[i - 1].ZoomPos);
			TestTrue(
				FString::Printf(TEXT("앵커 %d HFOV 단조 감소"), i),
				Point.HFovDeg < HucomsProtocol::ZoomHfovTable[i - 1].HFovDeg);
		}
	}

	TestTrue(
		TEXT("2000~3000 중간은 선형 보간"),
		FMath::IsNearlyEqual(
			HucomsProtocol::ZoomPosToHFov(2500, CanonicalWide),
			45.63f,
			Tolerance));
	TestTrue(
		TEXT("최소 zoomPos 아래는 첫 앵커로 clamp"),
		FMath::IsNearlyEqual(
			HucomsProtocol::ZoomPosToHFov(-1, CanonicalWide),
			HucomsProtocol::ZoomHfovTable[0].HFovDeg,
			Tolerance));
	TestTrue(
		TEXT("최대 앵커 위는 마지막 앵커로 clamp"),
		FMath::IsNearlyEqual(
			HucomsProtocol::ZoomPosToHFov(HucomsProtocol::ZoomPosMax, CanonicalWide),
			HucomsProtocol::ZoomHfovTable[HucomsProtocol::ZoomHfovTableCount - 1].HFovDeg,
			Tolerance));

	const float Scale = AlternateWide / CanonicalWide;
	TestTrue(
		TEXT("비기본 BaseFOV에서 첫 앵커는 BaseFOV와 일치"),
		FMath::IsNearlyEqual(
			HucomsProtocol::ZoomPosToHFov(0, AlternateWide),
			AlternateWide,
			Tolerance));
	TestTrue(
		TEXT("비기본 BaseFOV에서 망원 앵커도 한 번만 비례 스케일"),
		FMath::IsNearlyEqual(
			HucomsProtocol::ZoomPosToHFov(16384, AlternateWide),
			2.39f * Scale,
			Tolerance));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHucomsSetCenterMatchesRealCamera,
	"baroCCTVSimulator.Hucoms.SetCenter matches the real camera",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHucomsSetCenterMatchesRealCamera::RunTest(const FString&)
{
	struct FCase { float Px; float Py; int32 WantPan; int32 WantTilt; const TCHAR* What; };
	const FCase Cases[] = {
		// 가로 클릭. 눈여겨볼 것은 WantTilt 가 0 이 아니라는 점 — 팬 축이 월드 수직축이라
		// 광축이 기울어져 있으면 가로로만 돌려도 고도가 따라 바뀐다. 실기가 그렇게 동작한다.
		{ 960 + 120, 540,  411,    -4, TEXT("가로 +120px") },
		{ 960 + 480, 540, 1602,   -62, TEXT("가로 +480px") },
		{ 960 + 720, 540, 2330,  -130, TEXT("가로 +720px") },
		{ 960 + 900, 540, 2829,  -191, TEXT("가로 +900px (여기서 선형 모델은 크게 어긋난다)") },
		// 세로 클릭. 옛 선형 모델은 여기서 30% 나 빗나갔다(VFOV 상수가 선형 유물이었다).
		{ 960, 540 + 300,    0,   975, TEXT("세로 +300px") },
		{ 960, 540 + 450,    0,  1445, TEXT("세로 +450px") },
		{ 960, 540 - 450,    0, -1445, TEXT("세로 -450px") },
	};

	for (const FCase& C : Cases)
	{
		int32 Pan = 0, Tilt = 0;
		HucomsProtocol::PixelToDeltaCentideg(C.Px, C.Py, RealWideHFov, RealTiltDeg, Pan, Tilt);
		TestEqual(FString::Printf(TEXT("%s pan: %s"), C.What, *Describe(Pan, C.WantPan)), Pan, C.WantPan);
		TestEqual(FString::Printf(TEXT("%s tilt: %s"), C.What, *Describe(Tilt, C.WantTilt)), Tilt, C.WantTilt);
	}

	// 정중앙 클릭은 움직일 이유가 없다.
	int32 Pan = 0, Tilt = 0;
	HucomsProtocol::PixelToDeltaCentideg(960.f, 540.f, RealWideHFov, RealTiltDeg, Pan, Tilt);
	TestEqual(TEXT("정중앙 클릭은 pan 을 움직이지 않는다"), Pan, 0);
	TestEqual(TEXT("정중앙 클릭은 tilt 를 움직이지 않는다"), Tilt, 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHucomsSetCenterIsNotLinear,
	"baroCCTVSimulator.Hucoms.SetCenter is tan, not linear",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHucomsSetCenterIsNotLinear::RunTest(const FString&)
{
	// tan 투영에서는 각도가 편심에 비례하지 않는다: 편심을 2배로 늘려도 각도는 2배보다 작다.
	// (옛 선형 모델은 정확히 2배였다 — 이 테스트가 그 회귀를 잡는다.)
	int32 PanNear = 0, PanFar = 0, Tilt = 0;
	HucomsProtocol::PixelToDeltaCentideg(960.f + 240.f, 540.f, 60.f, 0.f, PanNear, Tilt);
	HucomsProtocol::PixelToDeltaCentideg(960.f + 480.f, 540.f, 60.f, 0.f, PanFar, Tilt);

	TestTrue(TEXT("편심 2배의 각도는 2배보다 작아야 한다 (tan 압축)"), PanFar < 2 * PanNear);
	TestTrue(TEXT("그래도 단조 증가는 한다"), PanFar > PanNear);

	// 프레임 가장자리를 클릭하면 정확히 HFOV 의 절반만큼 돈다 — tan 정의상 자명한 앵커.
	int32 PanEdge = 0;
	HucomsProtocol::PixelToDeltaCentideg(1920.f, 540.f, 60.f, 0.f, PanEdge, Tilt);
	TestEqual(TEXT("가장자리 클릭 = HFOV/2"), PanEdge, 3000);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHucomsSetCenterFocalGain,
	"baroCCTVSimulator.Hucoms.SetCenter focal gain reproduces a miscalibrated camera",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHucomsSetCenterFocalGain::RunTest(const FString&)
{
	// FocalGain > 1 = 실제보다 더 줌인했다고 믿는 카메라 => 덜 돈다(실기 cam-001 의 결함).
	int32 PanTrue = 0, PanMiscal = 0, Tilt = 0;
	HucomsProtocol::PixelToDeltaCentideg(960.f + 480.f, 540.f, 60.f, 0.f, PanTrue, Tilt, 1.0f);
	HucomsProtocol::PixelToDeltaCentideg(960.f + 480.f, 540.f, 60.f, 0.f, PanMiscal, Tilt, 1.11f);

	TestTrue(TEXT("게인 1.11 이면 덜 돈다 (언더슈트 재현)"), PanMiscal < PanTrue);

	// 게인 < 1 = 실제보다 넓게 본다고 믿는 카메라 => 지나친다(망원 끝의 실기 결함).
	int32 PanOver = 0;
	HucomsProtocol::PixelToDeltaCentideg(960.f + 480.f, 540.f, 60.f, 0.f, PanOver, Tilt, 0.75f);
	TestTrue(TEXT("게인 0.75 면 지나친다 (오버슈트 재현)"), PanOver > PanTrue);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

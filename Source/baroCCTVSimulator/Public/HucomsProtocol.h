// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * HucomsProtocol
 *
 * Hucoms PTZ CCTV "와이어 프로토콜"의 단위/범위/좌표 변환을 담는 순수 헬퍼.
 * UObject 비의존(테스트 용이) - 시뮬레이터가 실기와 동일한 수치로 동작하도록
 * 한 곳에 모은다. 권위 출처:
 *   baro_calrory/packages/cctv-client/src/{hucoms-camera-client,fov-convert,fake-camera-client}.mjs
 *   baro_calrory/reference/_hucoms_extracted.txt (HTTP_API_Hucoms_V1.22)
 *
 * 핵심 원칙:
 *  - panpos/tiltpos 는 "도(deg) x 100" 정수(centi-degree). zoompos 는 0..65535 raw tick.
 *  - 픽셀<->각도 변환은 **TAN(직선/rectilinear) + 1/cos(tilt) 구면 커플링**.
 *  - 센터링 픽셀 프레임은 항상 논리 1920x1080.
 *
 * [2026-07-14 정정] 이 파일은 오랫동안 "픽셀<->각도는 LINEAR(tan 아님)"라고 못박고 있었고
 * setcenter 도 그렇게 구현돼 있었다. 실기(cam-001, HNR-2036LA)를 105샘플로 실측하니 그 전제가
 * **거짓**이었다: 실제 펌웨어는 정확한 tan 기하를 쓰며, 짐벌의 1/cos(tilt) 커플링까지 이미
 * 적용한다(팬으로 역산한 초점거리와 틸트로 역산한 초점거리가 0.1% 이내로 일치, 그리고 팬
 * 초점거리가 틸트 6°~33° 전 구간에서 불변 — 커플링을 안 하면 불가능한 결과).
 *
 * 옛 "LINEAR" 규약은 실기가 아니라 **실기에 대한 우리의 틀린 믿음**을 재현하고 있었다(mock →
 * sim → 문서로 자기일관되게 전파). 그 탓에 시뮬은 tan 으로 렌더하면서 조준만 선형이라
 * 자기모순이었고, 와이드 클릭에서 가로 47px / 세로 92px(30%)나 빗나갔다. 실측 도구와 근거:
 *   baro_calrory/tools/centering_calib/  (measure_center.py, analyze.py, README.md)
 */
namespace HucomsProtocol
{
	// --- 와이어 범위 (raw Hucoms 단위) ---------------------------------------
	static constexpr int32 PanPosMin = 0;
	static constexpr int32 PanPosMax = 35999;     // 0.01deg, 0.00 .. 359.99
	static constexpr int32 PanWrap   = 36000;     // 한 바퀴(=360.00deg)
	static constexpr int32 TiltPosMin = -2000;    // -20.00deg
	static constexpr int32 TiltPosMax = 9000;     // +90.00deg
	static constexpr int32 ZoomPosMin = 0;
	static constexpr int32 ZoomPosMax = 65535;    // raw tick (closeupZoom 22000 은 단순 프리셋)
	static constexpr int32 FocusPosMin = 0;
	static constexpr int32 FocusPosMax = 65535;

	// --- 센터링 논리 프레임 (와이어는 항상 1920x1080) -----------------------
	static constexpr int32 FrameW = 1920;
	static constexpr int32 FrameH = 1080;

	/** panpos 를 [0, 36000) 로 wrap. */
	inline int32 WrapPan(int32 PanPos)
	{
		int32 P = PanPos % PanWrap;
		if (P < 0) { P += PanWrap; }
		return P;
	}

	/** From->To 의 최단 호(signed) 차이를 raw 단위로 반환. 0/35999 이음매를 넘어 최단 경로. */
	inline int32 ShortestPanDiff(int32 From, int32 To)
	{
		int32 D = WrapPan(To) - WrapPan(From);
		if (D > PanWrap / 2)       { D -= PanWrap; }
		else if (D < -PanWrap / 2) { D += PanWrap; }
		return D;
	}

	inline int32 ClampTilt(int32 TiltPos)  { return FMath::Clamp(TiltPos, TiltPosMin, TiltPosMax); }
	inline int32 ClampZoom(int32 ZoomPos)  { return FMath::Clamp(ZoomPos, ZoomPosMin, ZoomPosMax); }
	inline int32 ClampFocus(int32 FocusPos){ return FMath::Clamp(FocusPos, FocusPosMin, FocusPosMax); }

	/** 실측 zoompos -> 수평 FOV 앵커. 보간 함수와 /scene/cameras 직렬화가 함께 쓰는 단일 소스. */
	struct FZoomHfovPoint
	{
		int32 ZoomPos;
		float HFovDeg;
	};

	inline constexpr FZoomHfovPoint ZoomHfovTable[] = {
		{     0, 57.14f }, {  2000, 47.89f }, {  3000, 43.37f }, {  5129, 34.05f },
		{  8000, 22.59f }, { 10338, 14.68f }, { 12161,  9.77f }, { 14000,  6.29f },
		{ 15000,  4.88f }, { 15400,  4.32f }, { 15800,  3.74f }, { 16100,  3.16f },
		{ 16384,  2.39f },
	};
	inline constexpr int32 ZoomHfovTableCount = UE_ARRAY_COUNT(ZoomHfovTable);

	/**
	 * zoompos -> 수평 FOV(deg). 실측 캘리브레이션 표(cam-001)를 선형보간.
	 * 표의 wide 값을 설정값 WideHFovDeg 에 맞춰 비례 보정한다.
	 *
	 * [2026-07-14 재측정] 옛 표는 4점(69.88 / 35.36 / 23.30 / 1.94)이었고 **와이드가 22% 틀렸다**
	 * (선형 가정으로 역산된 유물). tools/centering_calib 으로 줌 15단계를 실측해 전면 교체:
	 * 실제 광학은 z≈16384 에서 **포화**하며(그 이상 화각 불변 2.39°), 15000~16384 구간에서 화각이
	 * 절반으로 꺾이므로 그 구간에 앵커를 촘촘히 둔다(성기게 두면 보간이 거짓말을 한다).
	 * 이 표는 시뮬레이터 렌더와 /scene/cameras 직렬화가 공유하는 시뮬 쪽 단일 소스다.
	 * 실카메라 표는 baro_calory/packages/web-ui/src/camera-intrinsics.mjs 와 devices[].intrinsics가
	 * 별도로 관리한다. 웹은 sim API 표가 있으면 그것을 우선하고, 없으면 실기/레거시 JS 표를 쓴다.
	 */
	inline float ZoomPosToHFov(int32 ZoomPos, float WideHFovDeg)
	{
		const float Scale = (WideHFovDeg > KINDA_SMALL_NUMBER)
			? (WideHFovDeg / ZoomHfovTable[0].HFovDeg)
			: 1.f;
		const int32 Z = FMath::Clamp(
			ZoomPos,
			ZoomHfovTable[0].ZoomPos,
			ZoomHfovTable[ZoomHfovTableCount - 1].ZoomPos);

		for (int32 i = 0; i < ZoomHfovTableCount - 1; ++i)
		{
			if (Z <= ZoomHfovTable[i + 1].ZoomPos)
			{
				const float T = static_cast<float>(Z - ZoomHfovTable[i].ZoomPos)
					/ static_cast<float>(ZoomHfovTable[i + 1].ZoomPos - ZoomHfovTable[i].ZoomPos);
				return FMath::Lerp(ZoomHfovTable[i].HFovDeg, ZoomHfovTable[i + 1].HFovDeg, T) * Scale;
			}
		}
		return ZoomHfovTable[ZoomHfovTableCount - 1].HFovDeg * Scale;
	}

	/**
	 * 수평 FOV(deg)에 해당하는 광학 줌 배율(ZoomFactor). 기준(1x) = WideHFovDeg.
	 *   ZoomFactor = tan(Wide/2) / tan(HFov/2)   (rectilinear 렌더용)
	 */
	inline float HFovToZoomFactor(float HFovDeg, float WideHFovDeg)
	{
		const float HalfWide = FMath::DegreesToRadians(FMath::Max(1.f, WideHFovDeg)) * 0.5f;
		const float HalfNow  = FMath::DegreesToRadians(FMath::Clamp(HFovDeg, 0.1f, 179.f)) * 0.5f;
		const float TanNow = FMath::Tan(HalfNow);
		return (TanNow > KINDA_SMALL_NUMBER) ? (FMath::Tan(HalfWide) / TanNow) : 1.f;
	}

	/**
	 * 클릭 픽셀(1920x1080 논리 프레임) -> pan/tilt 델타(raw centi-degree).
	 * **TAN 핀홀 + 구면(짐벌) 기하** — 실기 펌웨어 실측 규약(cam-001, 2026-07-14).
	 *
	 * 픽셀을 카메라 좌표계의 광선으로 되돌린 뒤, 그 광선이 새 광축이 되도록 팬/틸트를 푼다.
	 * 팬 축이 월드 수직축이라 광축이 기울어져 있으면 두 가지가 따라온다:
	 *   (1) 같은 가로 편심이라도 더 많이 돌아야 하고(작은각 극한에서 1/cos(tilt)),
	 *   (2) **가로로만 클릭해도 틸트가 조금 움직인다** — 광축이 원뿔을 쓸고 지나가기 때문.
	 * 실측이 (2)까지 그대로 보여줬고(와이드·틸트 16.81°에서 dx=480 클릭에 dtilt=-62cd),
	 * 아래 식이 그 값을 정확히 재현한다. 근사식으로 갈음하지 말 것.
	 *
	 * FocalGain: 1.0 이면 기하학적으로 정확한 카메라(기본). 실기 펌웨어는 자기가 믿는 초점거리가
	 * 실제 렌즈와 어긋나서(줌에 따라 0.99~1.11, 망원 포화 구간에선 0.75) 조준이 빗나가는데,
	 * 그 결함을 시뮬에서 일부러 재현해 보정 파이프라인을 검증하고 싶을 때 쓰는 노브다.
	 *
	 * 부호: 화면 오른쪽(x+) => panpos 증가, 화면 아래(y+) => tiltpos 증가(higher tiltpos = 아래를 봄).
	 * 호출부는 두 델타를 현재값에 **더한다**.
	 */
	inline void PixelToDeltaCentideg(float PixelX, float PixelY, float HFovDeg, float CurTiltDeg,
	                                 int32& OutPanDeltaCd, int32& OutTiltDeltaCd, float FocalGain = 1.f)
	{
		const float HalfH = FMath::DegreesToRadians(FMath::Clamp(HFovDeg, 0.1f, 179.f)) * 0.5f;
		const float Focal = ((FrameW * 0.5f) / FMath::Max(FMath::Tan(HalfH), KINDA_SMALL_NUMBER))
		                  * FMath::Max(FocalGain, 0.01f);

		const float Nx = (PixelX - FrameW * 0.5f) / Focal;   // 광축 기준 정규화 편심
		const float Ny = (PixelY - FrameH * 0.5f) / Focal;

		// 광축의 고도각(elevation). tiltpos 는 '아래로'가 + 라 부호가 뒤집힌다.
		const float E0   = FMath::DegreesToRadians(-CurTiltDeg);
		const float SinE = FMath::Sin(E0);
		const float CosE = FMath::Cos(E0);

		// 목표 광선 = F + R*Nx + U*(-Ny) 를 방위 0 기준으로 편 것.
		const float Rx = CosE + SinE * Ny;
		const float Ry = -Nx;
		const float Rz = SinE - CosE * Ny;

		const float Len = FMath::Sqrt(Rx * Rx + Ry * Ry + Rz * Rz);
		const float A1  = FMath::Atan2(Ry, Rx);                                   // 새 방위
		const float E1  = FMath::Asin(FMath::Clamp(Rz / FMath::Max(Len, KINDA_SMALL_NUMBER), -1.f, 1.f));

		OutPanDeltaCd  = FMath::RoundToInt(FMath::RadiansToDegrees(-A1)      * 100.f);
		OutTiltDeltaCd = FMath::RoundToInt(FMath::RadiansToDegrees(E0 - E1)  * 100.f);
	}
}

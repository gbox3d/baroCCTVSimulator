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
 *  - 픽셀<->각도 변환은 **LINEAR** (tan 아님). HFOV/VFOV 는 wide 프리셋에서 실측된 값으로,
 *    "+480px(1/4 폭) = pan HFOV/4" 가정 하에 측정되었다. tan 투영과 섞으면 안 된다.
 *  - 센터링 픽셀 프레임은 항상 논리 1920x1080.
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

	/**
	 * zoompos -> 수평 FOV(deg). 실측 캘리브레이션 표(cam-001)를 선형보간.
	 *   zoompos    0 -> 69.88   /  5129 -> 35.36  /  8000 -> 23.30  /  16384 -> 1.94 (망원, 이상 clamp)
	 * 표의 wide(69.88)를 설정값 WideHFovDeg 에 맞춰 비례 보정한다.
	 */
	inline float ZoomPosToHFov(int32 ZoomPos, float WideHFovDeg)
	{
		struct FZPt { int32 Z; float H; };
		static const FZPt Tbl[] = { {0, 69.88f}, {5129, 35.36f}, {8000, 23.30f}, {16384, 1.94f} };
		constexpr int32 N = UE_ARRAY_COUNT(Tbl);

		const float Scale = (WideHFovDeg > KINDA_SMALL_NUMBER) ? (WideHFovDeg / Tbl[0].H) : 1.f;
		const int32 Z = FMath::Clamp(ZoomPos, Tbl[0].Z, Tbl[N - 1].Z);

		for (int32 i = 0; i < N - 1; ++i)
		{
			if (Z <= Tbl[i + 1].Z)
			{
				const float T = (float)(Z - Tbl[i].Z) / (float)(Tbl[i + 1].Z - Tbl[i].Z);
				return FMath::Lerp(Tbl[i].H, Tbl[i + 1].H, T) * Scale;
			}
		}
		return Tbl[N - 1].H * Scale;
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
	 * **LINEAR 모델** - 실기 펌웨어 setcenter 재현.
	 *   panDelta  = ((x - W/2)/W) * HFOV * 100
	 *   tiltDelta = ((y - H/2)/H) * VFOV * 100
	 * 주의: 화면 아래(y 큰 값) = 카메라 아래로 => 호출부에서 tiltpos 를 '빼서' 적용한다.
	 */
	inline void PixelToDeltaCentideg(float PixelX, float PixelY, float HFovDeg, float VFovDeg,
	                                 int32& OutPanDeltaCd, int32& OutTiltDeltaCd)
	{
		OutPanDeltaCd  = FMath::RoundToInt(((PixelX - FrameW * 0.5f) / FrameW) * HFovDeg * 100.f);
		OutTiltDeltaCd = FMath::RoundToInt(((PixelY - FrameH * 0.5f) / FrameH) * VFovDeg * 100.f);
	}
}

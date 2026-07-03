// Copyright gbox3d. All Rights Reserved.

using UnrealBuildTool;

public class BaroSim : ModuleRules
{
	public BaroSim(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Public: 공개 헤더가 이 모듈들의 타입을 노출하므로 소비 모듈(게임 모듈)도 include 경로가 필요하다.
		//   - HTTP:       CenteringClientComponent.h 가 HttpFwd.h(FHttpRequestPtr 등)를 노출
		//   - HTTPServer: HucomsServerSubsystem.h 가 IHttpRouter.h / HttpResultCallback.h 를 노출
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput",
			"HTTP", "HTTPServer"
		});

		// Private: 구현부(.cpp)에서만 쓰는 의존성.
		//   - Json:                Hucoms 응답 / baro_vla 추론 JSON 파싱
		//   - Sockets, Networking: FMjpegStreamServer 연속 MJPEG TCP 스트림
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json", "Sockets", "Networking"
		});
	}
}

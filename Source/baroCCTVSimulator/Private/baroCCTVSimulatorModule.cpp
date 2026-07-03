// Copyright gbox3d. All Rights Reserved.

#include "Modules/ModuleManager.h"

// baroCCTVSimulator 은 런타임 로직만 담는 코드 플러그인이라 별도 시작/종료 훅이 필요 없다.
// 기본 모듈 구현으로 등록한다. (두 번째 인자는 반드시 모듈명 "baroCCTVSimulator" 과 일치)
IMPLEMENT_MODULE(FDefaultModuleImpl, baroCCTVSimulator);

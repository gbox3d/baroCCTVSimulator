# baroCCTVSimulator 개발자 매뉴얼

`baroCCTVSimulator` 플러그인을 사용해 **언리얼 안의 PTZ 카메라를 실기(Hucoms) CCTV처럼 제어·스트리밍**하는 방법과, 그 통신 프로토콜(HTTP CGI) 레퍼런스 및 실전 예제를 담는다.

- 대상: 이 시뮬레이터에 붙는 클라이언트(`baro_calory` 등)를 개발하거나, 카메라를 배치/튜닝하는 개발자.
- 기준 구현: 플러그인 v0.1.7 / **Hucoms HTTP API V1.22** 표면 호환.
- 함께 볼 문서: [`README.md`](../README.md)(플러그인 개요·모듈 구성), [`docs/INTEGRATION.md`](INTEGRATION.md)(프로젝트에 submodule로 세우는 절차). 이 문서는 **"세운 뒤 실제로 쓰고 통신하는 법"** 이다.

## 목차

- [1. 아키텍처 한눈에](#1-아키텍처-한눈에)
- [2. 빠른 시작 (배치 → 실행 → 첫 요청)](#2-빠른-시작-배치--실행--첫-요청)
- [3. 카메라 배치와 설정 (APTZCamera)](#3-카메라-배치와-설정-aptzcamera)
- [4. 포트 규약](#4-포트-규약)
- [5. 좌표·단위 규약 (반드시 숙지)](#5-좌표단위-규약-반드시-숙지)
- [6. 통신 프로토콜 레퍼런스 (CGI)](#6-통신-프로토콜-레퍼런스-cgi)
  - [6.1 ptzf_status.cgi — 위치 조회/절대이동/상태](#61-ptzf_statuscgi--위치-조회절대이동상태)
  - [6.2 ptz_centering.cgi — 클릭 센터링](#62-ptz_centeringcgi--클릭-센터링)
  - [6.3 capabilityptz.cgi — 능력 광고](#63-capabilityptzcgi--능력-광고)
  - [6.4 jpeg.cgi — 단일 스냅샷](#64-jpegcgi--단일-스냅샷)
  - [6.5 mjpeg.cgi — 단일 프레임 멀티파트](#65-mjpegcgi--단일-프레임-멀티파트)
  - [6.6 연속 MJPEG 스트림 (전용 TCP 포트)](#66-연속-mjpeg-스트림-전용-tcp-포트)
  - [6.7 /api/tuning — 캡처 튜닝 (시뮬 전용)](#67-apituning--캡처-튜닝-시뮬-전용)
- [7. 통신 시나리오 예제](#7-통신-시나리오-예제)
- [8. 고정형 카메라 모드](#8-고정형-카메라-모드)
- [9. 캡처/스트림 튜닝 (config)](#9-캡처스트림-튜닝-config)
- [10. 트러블슈팅](#10-트러블슈팅)

---

## 1. 아키텍처 한눈에

```
레벨(월드)
 ├─ APTZCamera #0  ─┐
 ├─ APTZCamera #1  ─┤   UHucomsServerSubsystem (월드 서브시스템, 게임/PIE에서 자동 기동)
 └─ APTZCamera #2  ─┘        │
                             ├─ 채널 0  → HTTP :8081  (CGI 라우터)  + MJPEG :8091 (TCP 스트림)
                             ├─ 채널 1  → HTTP :8082                + MJPEG :8092
                             └─ 채널 2  → HTTP :8083                + MJPEG :8093
```

- **카메라 1대 = 채널 1개 = 독립 서버 1개.** 각 채널은 자기 포트에 Hucoms CGI(HTTP) + 연속 MJPEG(TCP)를 노출한다.
- **채널이 "정준 PTZ 상태"(raw Hucoms 정수 단위)를 소유**하고, 매 틱 모터 슬루를 시뮬레이션한 뒤 그 값을 `APTZCamera`에 미러링한다. 그래서 `getptzfpos` 라운드트립이 실기처럼 정확하다.
- **서버 수명**: 월드 `BeginPlay`에 시작, 월드 종료 시 정지. 에디터 프리뷰 월드에서는 안 뜨고 **게임(-game)/PIE**에서만 뜬다.
- `baro_calory`의 `devices.list[].{host,port}`를 채널 포트와 1:1로 맞추면 클라이언트가 카메라별로 개별 접속한다.

---

## 2. 빠른 시작 (배치 → 실행 → 첫 요청)

1. 레벨에 **`APTZCamera`**를 배치한다(클래스 팔레트에서 직접, BP 서브클래스 불필요). 디테일 패널에서 `bServeHucoms = true`(기본값), 포트는 `0`(자동) 그대로 둔다.
2. **standalone(-game)** 으로 실행:
   ```powershell
   & "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe" `
     "C:\works\ue_prjs\<Project>\<Project>.uproject" -game -windowed -resx=1280 -resy=720
   ```
   로그에 `[Hucoms] 시뮬레이터 서버 시작 — 채널 N/N 개.`가 뜨면 성공.
3. 다른 터미널에서 첫 요청:
   ```bash
   # PTZ 현재 위치 조회
   curl "http://127.0.0.1:8081/cgi-bin/control/ptzf_status.cgi?action=getptzfpos"
   # 실렌더 스냅샷 저장
   curl "http://127.0.0.1:8081/cgi-bin/image/jpeg.cgi" -o snap01.jpg
   ```

> ⚠️ standalone에서 **메인 창은 검게** 보인다(헤드리스 카메라 서버 목적 — `ABaroSimGameMode`가 월드 렌더를 끔). 그래도 `jpeg.cgi`/스트림은 SceneCapture 자체 렌더라 **정상 출력**된다. 씬을 눈으로 보려면 PIE로 실행한다.

---

## 3. 카메라 배치와 설정 (APTZCamera)

클래스 팔레트에서 **APTZCamera**를 직접 배치한다. 컴포넌트 계층은 `Root → PanPivot(Yaw) → TiltPivot(Pitch) → CameraComp`이며, 팬은 항상 월드 수직축 기준으로 돌아 어떤 설치각에서도 지평선이 롤되지 않는다.

**설치 자세** = 액터 Transform으로 지정: **Yaw**(설치 방향) + **Pitch**(하방 조준, 보통 −15~−25°). 서버 기동 시 이 설치 Pitch가 초기 `tiltpos`로 이관된다.

주요 디테일 패널 프로퍼티:

| 카테고리 | 프로퍼티 | 기본값 | 의미 |
|---|---|---|---|
| `PTZ\|Mode` | `bFixedMode` | `false` | 고정형 CCTV로 전환(§8) |
| `PTZ\|Hucoms` | `bServeHucoms` | `true` | 이 카메라를 서버 채널로 노출 |
| `PTZ\|Hucoms` | `HucomsHttpPort` | `0` | HTTP 포트(0=자동 `BaseHttpPort+인덱스`) |
| `PTZ\|Hucoms` | `HucomsMjpegPort` | `0` | MJPEG 포트(0=자동 `BaseMjpegPort+인덱스`) |
| `PTZ\|Limits` | `PanMin/Max`, `TiltMin/Max` | ±180 / −90..30 | 기계적 한계(서버가 sim용으로 넓힘) |
| `PTZ\|Optics` | `BaseFOV` | 90 | 에디터 기본값. Hucoms 서버 기동 시 `WideHFovDeg=57.14`로 세팅 |

> **주차장 sim 레벨의 배치 규약**: CCTV는 카메라 폴(`BP_Pole`)의 **자식**으로, 폴 기준 `RelativeLocation (0,0,600)`(암 높이), `Pitch −20`. **폴 개수 = 카메라 개수**.

`APTZCamera`는 BlueprintCallable API(`SetPan`/`SetTilt`/`SetZoomFactor`/`AddPan`/`SnapToTarget`/`ActivateView` 등)도 제공하지만, **Hucoms 서버가 붙은 카메라는 서버가 모터를 소유**하므로(매 틱 `MirrorChannel`이 덮어씀) 직접 호출과 섞지 말 것. 조종은 CGI로 한다.

---

## 4. 포트 규약

- **HTTP CGI 포트** = 카메라 `HucomsHttpPort`가 >0이면 그 값, 아니면 `BaseHttpPort(8081) + 카메라 인덱스`.
- **연속 MJPEG 포트** = `HucomsMjpegPort`가 >0이면 그 값, 아니면 `BaseMjpegPort(8091) + 카메라 인덱스`.
- 인덱스는 레벨에서 열거된 순서(0,1,2,…).

예 — 카메라 2대 레벨:

| 카메라 | HTTP CGI | 연속 MJPEG |
|---|---|---|
| #0 | `:8081` | `:8091` |
| #1 | `:8082` | `:8092` |

> HTTP 제어 포트는 기본적으로 `127.0.0.1`(localhost)에 바인딩된다. 다른 호스트에서 제어하려면 바인딩을 열어야 한다. 연속 MJPEG는 `0.0.0.0`.

---

## 5. 좌표·단위 규약 (반드시 숙지)

모든 와이어 값은 **정수**다. `HucomsProtocol.h`가 유일한 권위 소스.

| 축 | 파라미터 | 범위 | 단위 | 방향 규약 |
|---|---|---|---|---|
| Pan | `panpos` | `0 .. 35999` | centi-degree(0.01°) | **higher = 우측(시계, 위에서 봄)**. 0/35999 이음매를 최단 호로 이동 |
| Tilt | `tiltpos` | `-2000 .. 9000` | centi-degree | **higher = 카메라가 아래를 봄** (−20°..+90°) |
| Zoom | `zoompos` | `0 .. 65535` | raw tick | **higher = 망원(줌인)**. wide(0)=HFOV 57.14°, 광학 화각은 16384부터 2.39°로 포화 |
| Focus | `focuspos` | `0 .. 65535` | raw tick | (시뮬은 즉시 반영) |

- **센터링 픽셀 프레임은 항상 논리 `1920 × 1080`**. 클릭 좌표는 이 좌표계로 보낸다(스냅샷 실제 해상도 QHD와 무관).
- 픽셀→PTZ 델타는 **TAN 핀홀 역투영 + 구면 짐벌 기하**를 사용한다. 논리 픽셀을 현재 HFOV로 카메라 광선에 역투영한 뒤, 현재 틸트를 포함해 그 광선이 새 광축이 되도록 pan/tilt를 푼다. 따라서 틸어진 카메라에서는 가로 클릭만 해도 작은 tilt 보정이 생긴다.
- **모터 슬루**: 명령은 목표(Tgt)만 바꾸고, 현재(Cur)가 매 틱 일정 속도로 목표를 향해 이동한다(Pan 90°/s, Tilt 60°/s 기본). 따라서 이동 명령 직후엔 `getptzfpos`가 아직 목표에 도달 안 한 중간값을 반환한다 → **settle 폴링** 필요(§7).

---

## 6. 통신 프로토콜 레퍼런스 (CGI)

모든 엔드포인트는 **GET**, 응답은 실기와 동일하게 **항상 `200 OK`**. 제어 명령의 성공 응답은 **빈 본문**이며, 클라이언트는 본문이 `Error:`로 시작하는지만 검사한다. 조회 응답은 `text/plain`의 `key = value` 라인들이다.

베이스 URL: `http://<host>:<httpPort>` (예: `http://127.0.0.1:8081`)

### 6.1 ptzf_status.cgi — 위치 조회/절대이동/상태

`GET /cgi-bin/control/ptzf_status.cgi?action=<action>`

| action | 추가 파라미터 | 동작 / 응답 |
|---|---|---|
| `getptzfpos` | — | 현재 위치 조회 (아래 본문) |
| `goptzfpos` | `panpos`,`tiltpos`,`zoompos`,`focuspos` (모두 선택) | 절대 이동(목표 설정). 성공 시 빈 본문 |
| `getptzstatus` | — | `ptstatus`/`zfstatus` (enable/disable) |
| `setptzstatus` | `ptstatus`,`zfstatus` (`enable`/`disable`) | 상태 설정 후 에코 |
| `lensreset` | — | (no-op) 빈 본문 |

`getptzfpos` 응답 본문:
```
panpos = 9000
tiltpos = 1500
zoompos = 5000
focuspos = 0
```

예:
```bash
# 조회
curl "http://127.0.0.1:8081/cgi-bin/control/ptzf_status.cgi?action=getptzfpos"

# 절대 이동: 팬 90.00° / 틸트 15.00°(아래) / 줌 5000
curl "http://127.0.0.1:8081/cgi-bin/control/ptzf_status.cgi?action=goptzfpos&panpos=9000&tiltpos=1500&zoompos=5000"

# 잘못된 action → 본문 "Error: invalid parameter"
```

> `goptzfpos`는 준 파라미터만 갱신한다(예: `zoompos`만 주면 pan/tilt는 유지). 값은 각 범위로 clamp되고 pan은 wrap된다.

### 6.2 ptz_centering.cgi — 클릭 센터링

`GET /cgi-bin/control/ptz_centering.cgi?action=setcenter&type=<point|box>&...`

화면에서 클릭/드래그한 지점을 화면 중앙으로 가져오는 조준 명령. 성공 시 빈 본문. 델타는 **현재 줌의 실효 FOV** 기준으로 환산되어(줌인 상태에서도 정확), 현재 위치(Cur)에 더해진다.

**type=point** — 한 점을 중앙으로:

| 파라미터 | 의미 |
|---|---|
| `center.pointx` | 클릭 X (0..1920) |
| `center.pointy` | 클릭 Y (0..1080) |

```bash
# (1400, 800) 클릭 → 그 지점을 중앙으로 (아래+우측이므로 tiltpos↑, panpos↑)
curl "http://127.0.0.1:8081/cgi-bin/control/ptz_centering.cgi?action=setcenter&type=point&center.pointx=1400&center.pointy=800"
```

**type=box** — 드래그 박스의 중심을 중앙으로 + 박스가 작을수록 줌인:

| 파라미터 | 의미 |
|---|---|
| `center.startx`,`center.starty` | 박스 시작(좌상) |
| `center.endx`,`center.endy` | 박스 끝(우하) |

```bash
# 박스(800,400)-(1120,680) → 중심 조준 + 화면 대비 면적만큼 줌인
curl "http://127.0.0.1:8081/cgi-bin/control/ptz_centering.cgi?action=setcenter&type=box&center.startx=800&center.starty=400&center.endx=1120&center.endy=680"
```

> 방향 규약(field-validated): 화면 **아래(y+)** 지점을 중앙으로 = 카메라가 아래를 봄 = **`tiltpos` 증가**. 우측(x+) = `panpos` 증가.

### 6.3 capabilityptz.cgi — 능력 광고

`GET /cgi-bin/control/capabilityptz.cgi?action=getPTZ`

카메라가 지원하는 PTZ 능력을 광고한다(클라이언트가 UI를 켜고 끄는 데 사용). PTZ 카메라와 고정형(§8)의 응답이 다르다.

```
[Capabilities PTZ]
PanSupported = Yes
TiltSupported = Yes
ZoomSupported = Yes
FocusSupported = Yes
EndlessPanSupported = No
AutoFocusSupported = Yes
PresetSupported = 128
AutopanSupported = No
AutopancwSupported = No
TourSupported = No
```

> 클라이언트 파서는 선두 `[Capabilities PTZ]` 헤더 줄을 건너뛰고 `key = value`만 읽는다.

### 6.4 jpeg.cgi — 단일 스냅샷

`GET /cgi-bin/image/jpeg.cgi`

채널 카메라를 실제 렌더해 **`image/jpeg`** 1장을 반환한다. 해상도는 서버 config `SnapshotWidth×SnapshotHeight`(기본 QHD 2560×1440), 품질 `JpegQuality`(기본 92). 렌더 실패 시 4바이트 스텁 JPEG(`FF D8 FF D9`)을 같은 content-type으로 폴백한다.

```bash
curl "http://127.0.0.1:8081/cgi-bin/image/jpeg.cgi" -o snap.jpg
```

### 6.5 mjpeg.cgi — 단일 프레임 멀티파트

`GET /cgi-bin/image/mjpeg.cgi`

`multipart/x-mixed-replace;boundary=baroworldboundary`로 **단일 프레임** 하나를 감싸 반환한다(HTTP 라우터는 연속 응답이 불가). **연속 스트림은 6.6의 전용 TCP 포트**를 쓴다.

### 6.6 연속 MJPEG 스트림 (전용 TCP 포트)

각 채널의 **MJPEG 포트**(예 `:8091`)에 별도 TCP 서버가 떠서, 접속한 클라이언트에 최신 JPEG 프레임을 `multipart/x-mixed-replace`로 연속 송신한다. 클라이언트가 있을 때만 캡처하므로(없으면 렌더 비용 0), 소비자가 붙는 순간부터 `StreamFps`로 프레임이 흐른다.

```bash
# 원시 mpjpeg 확인
curl "http://127.0.0.1:8091/" --output stream.mjpeg   # Ctrl+C로 중단

# RTSP 재송출 (MediaMTX runOnDemand → ffmpeg가 이 포트를 mpjpeg로 읽어 RTSP/554로)
#   ffmpeg -f mpjpeg -i http://127.0.0.1:8091/ -c:v libx264 -f rtsp rtsp://...
```

> 설계상 **localhost 소비자(MediaMTX/ffmpeg)** 를 가정한 블로킹 송신이다. 느린 원격 브라우저 직결은 대상이 아니다(브리지를 거칠 것).

### 6.7 /api/tuning — 캡처 튜닝 (시뮬 전용)

`GET /api/tuning?<param>=<value>...`

**실기에 없는 시뮬레이터 전용** 디버그 API. 준 항목만 갱신하고 항상 현재 전체 상태를 JSON으로 반환한다. 값은 **전 채널 공유**(어느 포트로 호출해도 동일 적용), 즉시 반영(재빌드 불필요).

| 파라미터 | 범위 | 대상 |
|---|---|---|
| `exposureBias` | −4.0 .. 4.0 | 캡처 노출 보정(EV) |
| `contrast` | 0.5 .. 3.0 | 컬러 대비 |
| `jpegQuality` | 1 .. 100 | 스냅샷 JPEG 품질 |
| `warmupFrames` | 0 .. 32 | TSR 워밍업(0 권장) |
| `width`,`height` | 64.. | 스냅샷 해상도 |

```bash
# 현재 값 조회(파라미터 없이)
curl "http://127.0.0.1:8081/api/tuning"
# 대비만 조정
curl "http://127.0.0.1:8081/api/tuning?contrast=1.3&exposureBias=-0.5"
# → {"exposureBias":-0.500,"contrast":1.300,"jpegQuality":92,"warmupFrames":0,"width":2560,"height":1440}
```

---

## 7. 통신 시나리오 예제

**A. 절대 이동 후 정지 대기(settle)** — 모터 슬루 때문에 이동은 즉시 끝나지 않는다. `getptzfpos`를 폴링해 값이 목표에 수렴하면 완료로 판정:

```bash
BASE="http://127.0.0.1:8081/cgi-bin/control/ptzf_status.cgi"
curl -s "$BASE?action=goptzfpos&panpos=18000&tiltpos=3000&zoompos=8000" >/dev/null
# 목표(pan=18000,tilt=3000,zoom=8000)에 도달할 때까지 폴링
while :; do
  P=$(curl -s "$BASE?action=getptzfpos")
  echo "$P" | grep -q "panpos = 18000" && echo "$P" | grep -q "tiltpos = 3000" && break
  sleep 0.1
done
```

**B. 클릭 센터링 후 스냅샷** — 관심 지점을 중앙으로 조준하고 결과를 확인:

```bash
curl -s "http://127.0.0.1:8081/cgi-bin/control/ptz_centering.cgi?action=setcenter&type=point&center.pointx=1500&center.pointy=900" >/dev/null
sleep 0.5   # 슬루 대기(또는 A처럼 settle 폴링)
curl -s "http://127.0.0.1:8081/cgi-bin/image/jpeg.cgi" -o centered.jpg
```

**C. 줌 인/아웃** — pan/tilt는 그대로 두고 zoom만:

```bash
curl -s "http://127.0.0.1:8081/cgi-bin/control/ptzf_status.cgi?action=goptzfpos&zoompos=16000" >/dev/null  # 망원
curl -s "http://127.0.0.1:8081/cgi-bin/control/ptzf_status.cgi?action=goptzfpos&zoompos=0" >/dev/null      # 광각
```

**D. 다중 카메라 순회 스냅샷** — 포트만 바꿔 카메라별로:

```bash
for port in 8081 8082 8083 8084; do
  curl -s "http://127.0.0.1:$port/cgi-bin/image/jpeg.cgi" -o "cam_$port.jpg"
done
```

---

## 8. 고정형 카메라 모드

고정형 CCTV(팬틸트줌 없는 벽부착형)는 **별도 클래스가 아니라 `APTZCamera::bFixedMode = true`** 로 만든다. 캡처·스트림·서버 인프라를 그대로 공유하되 조향만 죽인 것이다.

- 설치 자세(액터 Yaw+Pitch)와 `TargetZoomFactor`로 화각이 **고정**된다.
- `goptzfpos`/`setcenter` 명령은 **무시**(no-op)된다. 모터 슬루도 없다.
- `capabilityptz.cgi`는 **`PanSupported=No` / `TiltSupported=No` / `ZoomSupported=No`** 로 광고 → 클라이언트가 PTZ 조작 UI를 숨길 수 있다.
- **`getptzfpos`는 고정값을 그대로 반환**하므로 클라이언트 라운드트립은 깨지지 않는다.
- **스트림/스냅샷(`jpeg.cgi`, 연속 MJPEG)은 정상 동작** — 실기 고정형 CCTV와 동일.

에디터에서 체크박스 하나(`bFixedMode`)로 고정↔PTZ를 토글한다.

---

## 9. 캡처/스트림 튜닝 (config)

런타임 값은 `/api/tuning`(§6.7)으로 무리빌드 스윕하고, 확정값은 `Config/DefaultGame.ini`에 굽는다:

```ini
[/Script/baroCCTVSimulator.HucomsServerSubsystem]
BaseHttpPort=8081
BaseMjpegPort=8091
; --- 광학(실측 캘리브레이션, 함부로 바꾸지 말 것) ---
WideHFovDeg=57.14
SetCenterFocalGain=1.0
; --- 캡처(스냅샷) ---
SnapshotWidth=2560
SnapshotHeight=1440
JpegQuality=92
CaptureExposureBias=-0.7    ; SceneCapture가 밝게 뜨는 것 보정
CaptureContrast=1.2         ; 하이키 클리핑 회피값(코드 기본 1.6 → ini로 1.2 권장)
SnapshotWarmupFrames=0      ; 0=단발이 가장 선명(실측)
; --- 연속 스트림 ---
StreamFps=30
StreamWidth=1280
StreamHeight=720
StreamJpegQuality=80
```

> `WideHFovDeg`는 wide 프리셋의 수평 화각이다. 세로 화각은 같은 초점거리와 프레임 종횡비에서 유도하며 별도 `WideVFovDeg` 설정을 두지 않는다. `SetCenterFocalGain=1.0`은 기하학적으로 정확한 센터링이고, 실기 펌웨어의 초점거리 오차를 재현할 때만 조정한다.
>
> 시뮬레이터의 전체 화각표는 `HucomsProtocol::ZoomHfovTable`의 13개 앵커를 단일 소스로 사용한다. `/scene/cameras[].intrinsics.zoomHfov`는 각 카메라의 `WideHFovDeg`가 이미 반영된 실효 표를 `{zoomPos,hfovDeg}` 배열로 노출하므로 소비자는 다시 비례 보정하지 않는다. 실카메라 화각표는 이 표와 별개로 `baro_calory/packages/web-ui/src/camera-intrinsics.mjs` 및 `devices[].intrinsics`에서 JS가 관리한다. 전체 Scene API 계약은 소비 프로젝트의 [`docs/scene-control-api.md`](../../../docs/scene-control-api.md)를 따른다.

---

## 10. 트러블슈팅

| 증상 | 원인 / 해결 |
|---|---|
| 서버가 안 뜸(`서빙할 카메라 없음`) | 레벨에 `bServeHucoms=true`인 `APTZCamera`가 없음. 배치 확인 |
| `라우터 바인드 실패 (port …)` | 포트 중복/점유. 다른 프로세스가 그 포트를 쓰거나 카메라 포트가 겹침 |
| 원격에서 제어 안 됨 | HTTP 제어 포트가 `127.0.0.1` 바인딩. 다른 호스트 제어는 바인딩 개방 필요 |
| `jpeg.cgi`가 4바이트/깨진 이미지 | 렌더 실패 스텁 폴백. 로그의 `jpeg.cgi 렌더 실패` 확인(캡처 컴포넌트/RT) |
| 이동 명령했는데 위치가 안 변함 | 모터 슬루 중. `getptzfpos` settle 폴링(§7-A). 또는 해당 카메라가 `bFixedMode`(§8) |
| standalone 메인 창이 검다 | 정상(헤드리스). 스트림/스냅샷은 나온다. 씬 확인은 PIE |
| 캡처가 뿌옇다 | TAA-off/FXAA 문제. 선명도는 `PTZCaptureComponent`의 TAA-on+Lumen 경로가 담당(플러그인 내장). 라플라시안 분산으로 실측 |

셋업/빌드 관련 문제(모듈 못 찾음, CoreRedirects, Shared 빌드환경)는 [`docs/INTEGRATION.md`](INTEGRATION.md)의 트러블슈팅을 참조.

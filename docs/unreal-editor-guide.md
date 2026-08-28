# Unreal Editor 사용 가이드

이 문서는 저장소에 포함된 Unreal Engine 5.6 프로젝트
`Unreal/uskysim/uskysim.uproject`를 서버와 연결하고, `SkySimSystem` 하나에서 시간·위치,
태양·달, 날씨, 여러 구름층과 광역 구름 표현을 편집하는 방법을 설명합니다.

현재 프로젝트는 개발 중인 기준 구현입니다. 서버의 `CLD2` density를 Unreal의
`Heterogeneous Volume`으로 표시하고, `SKS1` 상태로 조명·대기·안개를 구동합니다. 별도
전용 all-sky ray marcher, temporal reprojection, 강수 입자와 번개 볼트는 아직 구현 범위가
아닙니다. 목표 렌더러 구조는 [Unreal 렌더링 아키텍처와 로드맵](unreal-rendering.md)을
참고하십시오.

## 요구사항

- Unreal Engine 5.6
- Windows에서는 Visual Studio 2022의 Desktop/Game development with C++와 Windows SDK
- 서버 빌드용 CMake 3.16 이상과 C++17 컴파일러
- 검증 도구용 Python 3.10 이상
- DX12, Shader Model 6, Heterogeneous Volumes를 지원하는 GPU/드라이버

프로젝트는 `PythonScriptPlugin`, `SunPosition`, `GeoReferencing`을 활성화합니다. Python은
머티리얼 생성과 자동 검증에만 필요하며 일반 실행 중에는 필요하지 않습니다.

## 빠른 시작

아래 명령은 저장소 루트에서 실행합니다.

### 1. 서버 빌드

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --output-on-failure
```

Visual Studio generator를 사용하면 실행 파일은
`build/Release/cloud_sim_server.exe`에 생성됩니다.

### 2. Unreal C++ 프로젝트 빌드

Unreal Editor가 열려 있다면 먼저 닫습니다. 설치 위치에 맞게 `$ueRoot`만 수정합니다.

```powershell
$ueRoot = "C:\Program Files\Epic Games\UE_5.6"
$project = (Resolve-Path ".\Unreal\uskysim\uskysim.uproject").Path

& "$ueRoot\Engine\Build\BatchFiles\Build.bat" `
  uskysimEditor Win64 Development `
  "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

### 3. 서버 실행

`NewWorld`의 권장 프리뷰는 Natural 날씨, seed 55, 60배 시간과 20km × 20km × 14km
reference domain입니다.

```powershell
& ".\build\Release\cloud_sim_server.exe" `
  --grid 64 --hz 5 --send-hz 5 `
  --protocol 2 --fields density --compression auto `
  --weather natural --weather-seed 55 --time-scale 60 `
  --domain-width-m 20000 --domain-height-m 14000
```

포트 기본값은 다음과 같습니다.

| 용도 | 프로토콜 | 포트 |
|---|---|---:|
| 구름 볼륨 | `CLD2` | 7777 |
| 물체 상호작용 제어 | `CLC2` | 7778 |
| 전역 하늘 상태 | `SKS1` | 7779 |
| 날짜·날씨·구름층 제어 | `SKC1` | 7780 |

### 4. 프로젝트 열기

`Unreal/uskysim/uskysim.uproject`를 열면 `/Game/NewWorld`가 기본 맵으로 로드됩니다.
`SkySimSystem` 액터를 선택합니다. Play를 누르지 않아도 Level Editor viewport에서 서버
프레임을 받습니다.

정상 연결 시 Output Log에서 다음 종류의 메시지를 확인할 수 있습니다.

```text
SkySim: UDP 수신 시작 (CLD2=7777, SKS1=7779)
SkySim: CLD2 완성 frame=... grid=64x64x64 ...
SkySim: SKC1 applied, sequence=...
```

`NewWorld`는 하늘 평가용 최소 맵으로 별도 Landscape나 실제 지형 메시를 포함하지
않습니다. 실제 게임 레벨에서는 `SkySimSystem`을 하나만 배치하고 기존 지형·노출 설정과
함께 검증하십시오.

## 현재 Unreal 처리 경로

현재 구현은 다음 순서로 동작합니다.

1. `SkySimSystem`이 game/editor tick에서 non-blocking UDP 소켓을 읽습니다.
2. `CLD2` chunk를 frame/field별로 조립하고 크기, RLE와 CRC32를 검증합니다.
3. 최신 완성 density를 CPU에서 presentation shaping한 뒤 transient `PF_G16` 3D texture로
   업로드합니다.
4. `UHeterogeneousVolumeComponent`가 `/Game/SkySim/M_SkySimVolume`을 사용해 볼륨을
   렌더링합니다.
5. 머티리얼은 regional coverage mask, 좌표 warp, 비정수 보조 density lookup과 낮은 빈도
   erosion을 적용해 20km reference tile의 규칙적인 반복을 줄입니다.
6. `SKS1`의 UTC, 위치, 태양·달, 시정, 습도와 바람으로 Directional Light, Sky Light,
   Sky Atmosphere, Weather Fog와 material parameter를 갱신합니다.

현재 Unreal 수신기는 density만 렌더링에 사용합니다. 서버가 제공하는 velocity,
temperature, vapor, occupancy의 GPU 보간·가속 경로는 향후 전용 렌더러 항목입니다.

## Editor Preview

`Sky Sim | Editor Preview` 항목은 Play 없이 작업하는 방식을 제어합니다.

| 속성 | 기본값 | 의미 |
|---|---:|---|
| Preview In Editor | 켜짐 | Level Editor viewport에서 UDP 수신과 환경 갱신 |
| Animate Time In Editor | 켜짐 | 서버 상태가 잠시 없을 때도 로컬 프리뷰 시계를 진행 |
| Auto Apply Sky Controls In Editor | 켜짐 | Details 변경을 짧게 debounce한 뒤 `SKC1`로 자동 전송 |
| Sync Authoring Settings On Connect | 켜짐 | 재연결 시 날짜·위치·배속·preset을 저장된 액터 값과 동기화 |
| Apply Advanced Weather On Connect | 꺼짐 | 켜면 Natural을 수동 weather override로 바꾸므로 기본 비활성 |

PIE를 시작하면 editor receiver가 포트를 놓고 Play world가 수신합니다. PIE가 끝나면 editor
preview가 다시 연결됩니다. 맵에 `SkySimSystem`을 둘 이상 배치하면 같은 UDP 포트를 두고
경쟁하므로 하나만 사용하십시오.

## 날짜, 시간과 위치

`Sky Sim | Time and Location`에서 다음 값을 편집합니다.

- `Local Date and Time`: 사용자가 보는 현지 날짜/시간
- `UTC Offset Hours`: 현지 시간에서 UTC로 변환할 offset
- `Latitude`, `Longitude`, `Elevation`: 천체 위치와 대기 계산 기준
- `Time Scale`: `0`은 정지, 양수는 순방향, 음수는 역방향; 서버 허용 범위는
  `-86400..86400`

버튼의 의미는 다음과 같습니다.

- `Apply Date/Time and Location`: 날짜·위치를 즉시 서버에 적용
- `Apply Time Scale`: 입력한 배속 적용
- `Pause Time`: 마지막 0이 아닌 배속을 기억하고 0으로 설정
- `Resume Time`: 기억한 배속으로 복원

기본 클래스 값은 서울, UTC+9, 1배속입니다. 저장된 `NewWorld`는 빠른 변화를 확인하기 위해
Natural seed 55와 60배속을 사용합니다. Natural 날씨는 가속·역방향 달력 이동에도
따라가되 레이어가 한 프레임에 튀지 않도록 응답 속도를 제한합니다.

## 날씨

`Sky Sim | Weather`에는 Natural, Clear, Cumulus, Overcast, Rain, Storm, Snow, Fog가
있습니다.

- `Natural`: UTC, 위치, 계절, 여러 시간대의 deterministic noise와 seed를 이용해 온습도,
  시정, 바람, 강수와 구름층을 계속 변화시킵니다.
- 고정 preset: 지정된 여러 구름층과 환경 값으로 `Weather Transition Seconds` 동안
  전환합니다.
- `Weather Seed`: 날씨 시계열뿐 아니라 cloud-family 중심, 구름 크기와 위성 셀 배치를
  다시 결정합니다.
- `Release Weather To Natural`: Advanced Weather 또는 직접 만든 구름층으로 생긴 수동
  override를 해제합니다.

`Advanced` 하위 항목은 기온, 해면기압, 상대습도, 시정, aerosol, ozone, ENU 바람, 돌풍,
강수, 눈 비율, 지면 젖음, 대류와 번개 활동을 직접 보냅니다. `Apply Custom Weather
Settings`를 누르면 Natural이 아니라 수동 환경이 됩니다.

## 구름층 편집

`Sky Sim | Cloud Authoring`은 최대 네 개의 레이어를 `SKC1` 한 패킷으로 적용합니다.

| 속성 | 의미 |
|---|---|
| Enabled | 해당 슬롯 활성화; 비활성 슬롯은 zero-effect layer로 전송 후 서버에서 제거 |
| Type | Convective, Stratiform, Cirrus, Fog |
| Base/Top Altitude (AGL) | 현재 지표 고도 위의 베이스와 상단, 미터 단위 |
| Coverage | 수평 피복률과 analytic source 수에 함께 영향 |
| Optical Depth | 레이어 전체의 광학 두께 |
| Convective Activity | 발달 높이, 상승류, 수명과 billow 형태에 영향 |
| Liquid Fraction | 액상/빙정 비율 |
| Precipitation | 레이어 강수량 |

Layer 0 기본값은 Convective, 1200~3600m AGL, coverage 0.60, optical depth 7,
convective activity 0.55입니다. 0.60 coverage의 단일 convective layer는 서버 해상도와
전체 예산 적용 전 약 12개 source를 요청합니다. 레이어 종류별 full-coverage 기준은
Convective 20, Stratiform 14, Cirrus 18, Fog 9이며 모든 활성 레이어의 합산 예산은 32입니다.

서버는 source를 균일한 격자에 놓지 않습니다. 세 개의 비등방 cloud family 주위에
군집시키고 일부 위성 구름을 분리합니다. 각 source는 seed와 생애주기에 따라 수평 크기,
높이, 수직 크기, 강도와 billow offset이 달라집니다. 따라서 `Weather Seed`를 바꾸면 같은
coverage에서도 다른 구름 분포를 얻습니다.

레이어를 직접 적용하면 날씨는 수동 override가 됩니다. 자동 시간 변화로 돌아가려면
`Release Weather To Natural`을 사용하십시오.

## 광역 구름과 반복 제거

서버의 물리 reference domain은 기본 20km입니다. `Wide Cloud World`는 이 근거리 해상도를
유지하면서 기본 120km에 확장하고 반복이 눈에 띄지 않도록 두 번째 좌표계와 광역 mask를
사용합니다.

| Details 표시명 | 기본값 | 역할 |
|---|---:|---|
| Enable Wide Cloud World | 켜짐 | 광역 볼륨 활성화 |
| Cloud World Horizontal Extent | 120km | 수평 표시 범위 |
| Auto Horizontal Tile Count | 켜짐 | 서버 domain과 요청 범위에서 tile 수 산출 |
| Wide Cloud Sampling Quality | 1.0 | 광역 bake 수평 해상도 배율 |
| Regional Clear-Sky Strength | 0.13 | 연결된 큰 청천 통로를 파내는 강도 |
| Regional Coverage Scale | (2.15, 1.65, 0.70) | 전체 광역 영역의 저주파 weather-map scale |
| Large-Scale Position Warp | 0.16 | 반복 열을 흐트러뜨리는 위치 왜곡 |
| Secondary Pattern Scale | (0.83, 1.137, 1) | 두 번째 live density lookup의 비정수 배율 |
| Secondary Pattern Offset | (0.37, 0.61, 0) | 보조 lookup의 위상 offset |
| Pattern De-Tiling Blend | 0.72 | 원본과 보조 pattern의 weather-map 기반 혼합량 |

`Regional Clear-Sky Strength`를 지나치게 올리면 구름이 찢어지거나 coverage가 급감합니다.
`Large-Scale Position Warp`를 과도하게 올리면 cloud family가 길게 늘어납니다. 먼저 저장된
기본값에서 `Weather Seed`, `Coverage`, `Regional Clear-Sky Strength` 순서로 조정하고,
반복이 보일 때만 warp와 de-tiling blend를 소폭 변경하는 편이 안정적입니다.

머티리얼 weather map은 서버 UTC와 평균 바람에 따라 이동하므로 editor time lapse에서도
청천 통로가 정지된 스티커처럼 남지 않습니다.

## Cloud Presentation

이 항목은 서버의 원본 통계가 아니라 Unreal에 업로드하는 transient render texture와
머티리얼 표현만 조정합니다.

| 속성 | 기본값 | 주의점 |
|---|---:|---|
| Extinction Scale | 0.10 | 지나치면 빠르게 불투명해짐 |
| Detail Erosion Strength | 0.07 | 큰 값은 도넛·빈 중심과 모아레를 만듦 |
| Detail Noise Tiling | (7.13, 5.77, 4.31) | 낮은 비정수 주기로 반복 줄무늬 완화 |
| Cloud Spread Iterations | 0 | CPU 확산; 구름이 뭉개질 수 있어 기본 비활성 |
| Density Shape Power | 0.92 | 1보다 작으면 중간 밀도를 살림 |
| Density Presentation Gain | 1.20 | 최종 시각 밀도 gain |
| Use Physical Density Scale | 꺼짐 | 현재 solver density는 UE cm⁻¹ extinction으로 보정되지 않음 |

Detail erosion volume은 mip 2로 읽어 탑뷰에서 보이던 대각선 comb/moire를 억제합니다.

## 조명과 대기

`SkySimSystem`은 내부에 다음 컴포넌트를 생성합니다.

- 태양 Directional Light: `SKS1` sun direction/color/lux 사용
- 달 Directional Light: moon direction, phase와 illuminance 사용
- Sky Light: 전체 환경광
- Sky Atmosphere: 대기 배경과 원거리 산란
- SkySimWeatherFog: 시정·습도 기반 Exponential/Volumetric Fog
- Heterogeneous Volume: `CLD2` density 구름

태양과 달의 방향은 날짜, UTC offset, 위도·경도에 따라 자동으로 바뀝니다. 환경 조명을
꺼야 하는 특별한 경우가 아니라면 레벨에 별도의 태양·SkyAtmosphere를 중복 배치하지
마십시오.

### 현재 Weather Fog 동작과 알려진 제한

지면과 수평선의 뿌연 층은 구름이 아니라 `SkySimWeatherFog`입니다. 현재 버전은 Fog
preset 여부와 관계없이 시정·습도 값이 유효하면 Weather Fog를 활성화하며 기본값은 시작
거리 0m, 최대 불투명도 0.95, volumetric fog 켜짐입니다. 지면 가까운 카메라에서는 Natural
날씨도 원거리 지면이 과하게 씻겨 보일 수 있습니다.

맑은 장면을 평가할 때의 임시 방법은 다음 중 하나입니다.

- `Sky Sim | Weather | Fog Rendering | Enable Weather Fog`를 끕니다.
- 또는 `Fog Density Multiplier`를 0으로 둡니다.

이는 구름 레이어의 Base Altitude와 무관합니다. 정상 정책은 실제 저시정/고습 안개 조건과
Fog preset에서만 강한 지표 안개를 적용하는 것이며 후속 수정 항목입니다.

## Spectator 조작

Play를 누르면 `ASkySimSpectatorPawn`을 사용합니다.

| 입력 | 동작 |
|---|---|
| W / S | 전진 / 후진 |
| A / D | 좌 / 우 |
| E / Q | 상승 / 하강 |
| Shift | 4배 이동 boost |
| Mouse | yaw / pitch 시점 회전 |

기본 이동 속도는 100,000cm/s, FOV 95°, 초기 pitch 14°입니다. `Invert Mouse Y`는 기본
꺼짐이며 마우스를 위로 움직이면 위를 봅니다. 충돌을 끈 자유 비행 카메라이므로 지형을
통과할 수 있습니다.

## 상태와 제어 ACK 확인

Details의 `Status`, `Control Status`, `Volume`, `Server State`는 문제를 구분할 때
사용합니다.

- `Is Receiving`: UDP socket이 열렸는지 여부
- `Has Complete Volume Frame`: 완성된 density frame을 받은 적이 있는지 여부
- `Latest Volume Frame Id`, `Density Min/Max/Mean`: 볼륨 갱신과 값 범위
- `Has Sky State`, `Sky State Sequence`: `SKS1` 상태 갱신
- `Control Status`: Idle, Pending, Applied, Rejected, TimedOut
- `Server UTC/Local Date Time`, `Server Time Scale`, `Server Cloud Layer Count`: 서버가 실제로
  적용한 결과

`Pending`이 계속되면 7780 포트와 서버의 sky-control 활성 여부를 확인합니다. `Rejected`는
고도 범위, 날짜, 시정, 풍속 등 `SKC1` 검증 범위를 벗어난 값일 가능성이 큽니다.

## 머티리얼과 기본 맵 재생성

`M_SkySimVolume`은 editor module이 없을 때 생성하고, 다음 스크립트로 그래프를 명시적으로
재생성할 수 있습니다. Editor를 닫고 실행하십시오.

```powershell
$ueRoot = "C:\Program Files\Epic Games\UE_5.6"
$project = (Resolve-Path ".\Unreal\uskysim\uskysim.uproject").Path
$script = (Resolve-Path ".\tools\create_unreal_volume_material.py").Path

& "$ueRoot\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  $project -run=pythonscript "-script=$script" `
  -unattended -nop4 -nosplash
```

다음 스크립트들은 저장된 기본 맵을 재현하거나 읽기 전용으로 점검합니다.

- `tools/configure_unreal_dynamic_sky_defaults.py`: NewWorld 권장 authoring 값 저장
- `tools/ensure_unreal_preview_start.py`: PlayerStart와 Preview Camera 배치
- `tools/inspect_unreal_level.py`: 액터, transform과 SkySim 속성 출력
- `tools/inspect_unreal_material.py`: material domain과 expression 연결 출력
- `tools/verify_unreal_startup.py`: material startup 생성 확인
- `tools/test_unreal_cloud_authoring_control.py`: 맵을 저장하지 않고 두 coverage 제어 왕복

## 검증 절차

### 서버

```powershell
& ".\build\Release\cloud_sim_server.exe" --self-test
ctest --test-dir build --build-config Release --output-on-failure
```

자연스러운 분포 회귀 검사는 수평 활성 면이 거의 균일한 sheet가 아닌지, 서로 다른 고도
평면에 실제 변동이 있는지와 seed 변경 시 field가 달라지는지를 확인합니다.

### Unreal

1. 위의 `Build.bat` 명령으로 `uskysimEditor`를 빌드합니다.
2. commandlet로 `create_unreal_volume_material.py`와 `inspect_unreal_level.py`를 실행합니다.
3. `NewWorld`에서 Map Check가 오류·경고 0인지 확인합니다.
4. 서버와 Editor를 실행하고 Output Log에서 완성 `CLD2` frame, 연속 `SKS1` sequence와
   `SKC1 applied`를 확인합니다.
5. 서버 로그의 `drops[frame=0, packet=0]`과 `sky_tx[..., drops=0]`를 확인합니다.
6. PIE에서 Spectator의 모든 이동축과 기본 Mouse Y 방향을 확인합니다.

`tools/capture_density_projection.py`는 별도 짧은 서버를 띄워 live density의 탑뷰와 활성
면적/변동계수를 저장합니다. Pillow가 필요합니다.

## 문제 해결

### Details에 SkySim 항목이 보이지 않음

- `SkySimSystem` 액터 자체를 선택했는지 확인합니다.
- C++ 변경 후 Hot Reload 상태가 아니라 Editor를 닫고 전체 Editor target을 빌드합니다.
- Output Log에서 `uskysim` runtime/editor module 로드 오류를 확인합니다.

### 로그는 나오지만 구름이 보이지 않음

- `Enable Volume Rendering`과 material `/Game/SkySim/M_SkySimVolume`을 확인합니다.
- `Has Complete Volume Frame`, Density Max와 Latest Frame Id가 변하는지 확인합니다.
- 서버를 `--fields density` 또는 density를 포함한 field 목록으로 실행합니다.
- 카메라가 cloud volume 밖이나 구름층 위에 있지 않은지 확인합니다.

### 화면이 어두움

- `Enable Environment Lighting`, Sun/Sky Light multiplier를 확인합니다.
- 날짜와 위치에서 태양이 수평선 아래인지 Server Local Date Time으로 확인합니다.
- 레벨에 중복된 Directional Light나 Sky Atmosphere가 없는지 확인합니다.

### 지면이 뿌옇게 보임

- 구름이 아니라 Weather Fog인지 먼저 확인합니다.
- 현재 버전에서는 Natural에도 fog가 적용될 수 있으므로 맑은 장면 평가 시 Weather Fog를
  끄거나 Fog Density Multiplier를 0으로 둡니다.
- Preview Camera 기본 높이는 500cm(5m)라 지면 fog가 특히 두드러집니다.

### 구름이 일정한 격자나 대각선 줄로 보임

- 저장된 de-tiling 기본값을 복원합니다.
- Detail Erosion과 Noise Tiling을 과하게 올리지 않습니다.
- `Weather Seed`를 바꿔 서버 cloud-family 배치가 실제로 달라지는지 확인합니다.
- top-down 장면에서는 reference domain 반복이 지상 시점보다 쉽게 보인다는 점을 고려합니다.

### 포트 바인딩 실패

- 같은 프로젝트를 Editor와 PIE에서 동시에 두 번 실행하지 않았는지 확인합니다.
- 다른 서버/수신기가 7777 또는 7779를 점유하지 않았는지 확인합니다.
- Editor preview는 PIE 시작 시 자동으로 소켓을 놓고 종료 시 다시 연결해야 합니다.

### Spectator가 움직이지 않거나 Mouse Y가 반대임

- `NewWorld`의 GameMode가 `uskysimGameModeBase`인지 확인합니다.
- viewport를 한 번 클릭해 game input capture를 얻습니다.
- `Invert Mouse Y`가 의도와 맞는지 확인합니다. 기본은 꺼짐입니다.

## 현재 한계

- 서버 Natural weather는 결정론적 절차 모델이며 실제 관측·수치예보가 아닙니다.
- Unreal은 현재 density만 사용하고 velocity 기반 프레임 보간과 occupancy skip을 하지
  않습니다.
- 20km reference density를 광역 합성하므로 de-tiling을 해도 완전히 독립적인 120km 물리
  simulation은 아닙니다.
- 강수 입자, 번개 위치/볼트, 젖은 지면·적설 material feedback은 미구현입니다.
- Weather Fog의 평상시 활성 조건은 보수적으로 개선할 필요가 있습니다.
- 현재 preview material은 Heterogeneous Volume backend이며 목표 전용 atmosphere/cloud
  ray marcher와 temporal pipeline은 로드맵 항목입니다.

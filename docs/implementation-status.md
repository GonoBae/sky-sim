# 구현 현황 및 검증 기록

기준일: 2026-08-28

이 문서는 서버 기반 SkySim을 Unreal Engine 5.6에서 직접 편집·프리뷰할 수 있도록 확장한
변경 세트를 요약합니다. 프로토콜의 wire 상세는 각 전용 문서, 실제 조작법은
[Unreal Editor 사용 가이드](unreal-editor-guide.md)를 참고하십시오.

## 완료된 서버 변경

### 시간·날씨

- `SKS1` 전역 하늘 상태와 `SKC1` 제어를 날짜·위치·time scale, preset/Natural,
  Advanced Weather와 최대 네 구름층까지 연결했습니다.
- Natural weather가 UTC의 계절·일주 변화와 여러 synoptic/moisture 주기를 이용해 기온,
  습도, 시정, 바람, 강수, 대류와 번개를 연속 갱신합니다.
- 1배속에서는 약 120초 응답으로 부드럽게 변화하고, 가속 시간에서는 최대 48배 응답을
  사용해 time lapse를 따라갑니다. 정지와 역방향 시간도 처리합니다.
- 서로 다른 preset/Natural 레이어를 index로 바로 보간하지 않고 종류와 가까운 중심 고도를
  기준으로 pairing합니다. 새 레이어와 사라지는 레이어는 원래 고도에서 fade합니다.
- Cumulus, Overcast, Rain과 Snow가 단일 slab가 아니라 저층/중층/상층 조합을 갖습니다.
  Natural도 조건에 따라 broken lower layer, main deck, middle deck, cirrus/anvil, radiation
  fog를 함께 만듭니다.

관련 코드:

- `src/weather.hpp`
- `src/environment.hpp`
- `src/main.cpp` self-test
- `tests/test_sky_loopback.py`
- `tests/test_sky_regression_loopback.py`

### 자연스러운 구름 분포

- `Weather Seed`를 `CloudForcing::spatial_seed`에 연결했습니다. seed가 바뀌면 기존 field를
  초기화하고 새로운 cloud-family 공간 배치를 만듭니다.
- 이전의 균일한 additive-recurrence 점 분포를 세 개의 비등방 cloud family와 일부 위성
  cloud 구조로 교체했습니다.
- 단일 레이어 full-coverage emitter 기준을 종류별로 나눴습니다.
  Convective 20, Stratiform 14, Cirrus 18, Fog 9이며 활성 레이어 전체 예산은 32입니다.
- source마다 수평 크기, strength, 중심 높이와 수직 반경을 다르게 생성합니다. Convective
  수평 크기 범위는 대략 0.34~2.38배, 수직 크기는 0.75~1.60배입니다.
- Convective source는 성장/유지/소멸과 재탄생 생애주기를 가지며 생애마다 위치와 크기가
  달라집니다.
- 일곱 billow의 offset, 반경, 높이와 strength를 source/lifecycle seed로 각각 바꿔 동일한
  도장 모양의 반복을 줄였습니다.
- Stratiform/Cirrus/Fog는 서로 다른 aspect, 바람 정렬, 추가 lobe와 edge modulation을
  사용합니다.
- cloud layer metadata는 수직 envelope로만 사용하고 전체 XY plane을 포화시키지 않도록
  background vapor target을 sub-saturated로 유지합니다.

관련 코드:

- `src/simulation.hpp`
- `src/environment.hpp`
- `src/main.cpp`의 density-plane/seed/multilayer 회귀 검사

## 완료된 Unreal 변경

### 프로젝트와 런타임

- Unreal Engine 5.6 C++ 프로젝트를 `Unreal/uskysim`에 포함했습니다.
- `NewWorld`를 editor/game 기본 맵으로 설정했습니다.
- `ASkySimSystem`이 `CLD2`와 `SKS1` UDP를 non-blocking으로 받고 `SKC1`을 송신합니다.
- `CLD2`의 크기, chunk 위치, encoding, RLE, field CRC와 frame 완성을 검증합니다.
- density를 transient `PF_G16` volume texture로 올려
  `UHeterogeneousVolumeComponent`와 `/Game/SkySim/M_SkySimVolume`으로 표시합니다.
- `SKS1`에서 태양·달 Directional Light, Sky Light, Sky Atmosphere와 Weather Fog를
  갱신합니다.

### Editor authoring

- `ShouldTickIfViewportsOnly()`를 사용해 Play 없이 editor viewport에서 프리뷰합니다.
- PIE 진입 시 editor receiver가 UDP 포트를 놓고, PIE 종료 후 자동 재연결합니다.
- 날짜·위치·time scale, preset/seed, Advanced Weather와 네 구름층을 Details에서
  편집합니다.
- Details 변경은 debounce 후 ACK-serialized queue를 통해 전송합니다.
- reconnect sync는 날짜·위치·배속·preset을 복원하되 Advanced Weather는 Natural을 수동
  override하지 않도록 기본 제외합니다.
- Apply/Pause/Resume, Release Weather To Natural, Apply Custom Weather/Cloud Layers와
  Request Keyframe 버튼을 제공합니다.
- 연결, volume frame, density 통계, 서버 시간/위치, control 결과를 Details 상태로
  표시합니다.

관련 코드:

- `Unreal/uskysim/Source/uskysim/SkySimSystem.h`
- `Unreal/uskysim/Source/uskysim/SkySimSystem.cpp`
- `Unreal/uskysim/Source/uskysimEditor/uskysimEditor.cpp`

### 광역 구름 표현

- 20km reference domain을 기본 120km로 확장하며 tile당 density detail을 유지합니다.
- 서버의 cloud-family 군집과 별도로 전체 world 기준 regional coverage mask를 사용해 큰
  청천 통로와 비대칭 front를 만듭니다.
- weather map의 R/G channel로 primary density 좌표를 warp합니다.
- 같은 live density를 비정수 scale/offset의 두 번째 좌표로 다시 읽고 regional mask로
  혼합해 정확한 6×6 복사를 줄입니다.
- weather-map offset은 UTC와 평균 바람을 따라 움직입니다.
- detail erosion을 낮은 비정수 tiling과 mip 2로 바꿔 탑뷰의 대각선 comb/moire를
  완화했습니다.
- CPU presentation은 spread 0, shape power 0.92, gain 1.20을 기본값으로 사용합니다.

저장 기본값:

| 항목 | 값 |
|---|---:|
| Wide world extent | 120km |
| Regional Clear-Sky Strength | 0.13 |
| Regional Coverage Scale | (2.15, 1.65, 0.70) |
| Large-Scale Position Warp | 0.16 |
| Secondary Pattern Scale | (0.83, 1.137, 1.0) |
| Secondary Pattern Offset | (0.37, 0.61, 0.0) |
| Pattern De-Tiling Blend | 0.72 |
| Detail Erosion Strength | 0.07 |
| Detail Noise Tiling | (7.13, 5.77, 4.31) |

관련 코드와 자산:

- `Unreal/uskysim/Source/uskysim/SkySimSystem.cpp`
- `tools/create_unreal_volume_material.py`
- `Unreal/uskysim/Content/SkySim/M_SkySimVolume.uasset`
- `tools/configure_unreal_dynamic_sky_defaults.py`

### Spectator

- `ASkySimSpectatorPawn`을 기본 pawn/spectator class로 사용합니다.
- W/S, A/D, E/Q, Shift boost와 collision 없는 자유 비행을 제공합니다.
- 마우스를 위로 움직이면 위를 보도록 raw Mouse Y 부호를 수정했습니다.
- `Invert Mouse Y`는 선택 옵션이며 기본 꺼짐입니다.
- 기본 FOV 95°, 초기 pitch 14°, 이동 속도 100,000cm/s입니다.

관련 코드:

- `Unreal/uskysim/Source/uskysim/SkySimSpectatorPawn.h`
- `Unreal/uskysim/Source/uskysim/SkySimSpectatorPawn.cpp`
- `Unreal/uskysim/Source/uskysim/uskysimGameModeBase.cpp`

## 재현·진단 도구

다음 도구를 저장소에 포함합니다.

- `tools/create_unreal_volume_material.py`: Heterogeneous Volume material graph 재생성
- `tools/configure_unreal_dynamic_sky_defaults.py`: NewWorld의 권장 기본값 저장
- `tools/ensure_unreal_preview_start.py`: PlayerStart/Preview Camera 구성
- `tools/inspect_unreal_level.py`: 맵 액터, transform과 SkySim 속성 출력
- `tools/inspect_unreal_material.py`: material과 expression 연결 검사
- `tools/verify_unreal_startup.py`: editor startup material 확인
- `tools/test_unreal_cloud_authoring_control.py`: 저장 없이 coverage control 왕복
- `tools/analyze_density_components.py`: raw R16 density 연결 성분 분석
- `tools/capture_density_projection.py`: live density 탑뷰와 분포 통계 생성

Windows 창 클릭·프로세스 제어 스크립트, 실행 로그와 캡처는 로컬 시각 QA용이며 제품
소스와 재현 가능한 테스트가 아니므로 Git에서 제외합니다.

## 검증 결과

이 변경 세트에서 확인할 기준은 다음과 같습니다.

| 검증 | 기대 결과 |
|---|---|
| 서버 `--self-test` | 성공, varied density plane/seed/multilayer 검사 통과 |
| CTest | 9/9 통과 |
| Unreal `uskysimEditor` Development build | 성공 |
| `M_SkySimVolume` commandlet 생성/로드 | 성공 |
| `NewWorld` Map Check | 0 Error, 0 Warning |
| runtime `CLD2`/`SKS1` | frame/packet/sky drop 0 |
| cloud distribution diagnostic | 넓은 clear corridor와 비균일 군집 확인 |
| PIE Spectator | 6축 이동, Shift boost, 정상 Mouse Y 확인 |

검증 명령과 판정 방법은 [Unreal Editor 사용 가이드의 검증 절차](unreal-editor-guide.md#검증-절차)를
따릅니다.

## 알려진 제한과 후속 항목

- `SkySimWeatherFog`는 현재 Fog preset뿐 아니라 유효한 시정/습도 값이 있으면 모든 날씨에서
  켜집니다. 시작 거리 0m라 지면 카메라에서 수평선 연무가 과하게 보일 수 있습니다. 맑은
  장면 평가 시 Weather Fog를 끄거나 density multiplier를 0으로 두십시오.
- Unreal은 현재 `CLD2` density만 사용합니다. velocity temporal interpolation,
  temperature/vapor detail과 occupancy skip은 미구현입니다.
- Wide Cloud World는 독립 120km 물리 solver가 아니라 20km reference field의 비주기 합성
  표현입니다.
- 강수 입자, 번개 bolt/위치, 젖은 지면·적설 feedback은 상태만 있고 renderer는 없습니다.
- `CLC2` 서버와 참조 도구는 구현됐지만 Unreal `SkySimInteractorComponent`는 아직 없습니다.
- Natural weather는 실제 관측/예보 동화가 아닌 deterministic procedural model입니다.

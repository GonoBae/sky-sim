# Unreal 렌더링 구조

현재 구현은 `SkySimSystem`과 `HeterogeneousVolumeComponent`를 사용한다.
서버가 보낸 밀도를 `/Game/SkySim/M_SkySimVolume`으로 표시하고, 하늘 상태로 조명·대기·안개를
갱신한다. 사용법은 [에디터 가이드](unreal-editor-guide.md), 검증 결과는
[구현 현황](implementation-status.md)에 있다.

## 코드 구성

| 파일·클래스 | 담당 |
|---|---|
| `SkySimSystem` | 액터·Details, 소켓, 하늘 상태·제어 명령, 시계와 조명 |
| `SkySimVolumeReceiver` | CLD2 청크 조립, 형식·길이·RLE·CRC 검사 |
| `SkySimDensityFrame` / `SkySimDensityFrameHistory` | 밀도와 메타데이터 보관, 프레임 순서 검사 |
| `SkySimCloudMotion` | 지연 재생 버퍼, 표시할 두 프레임 선택, 바람 이동량 누적 |
| `SkySimDensityPresentation` | 고정 밀도 기준 변환, 확산·형태·배율 처리와 CPU 캐시 |
| `SkySimSystemRendering.cpp` | 두 GPU 슬롯 업로드·재사용, 머티리얼 파라미터 갱신 |
| `uskysimEditor` / `create_unreal_volume_material.py` | 기본 볼륨 머티리얼 생성 |

런타임 코드는 `Unreal/uskysim/Source/uskysim/`에 있다. 수신기·프레임·전처리는
UObject와 GPU를 참조하지 않는다. 액터의 공개 속성은 맵과 Blueprint 설정을 유지한다.

## 수신과 프레임 보관

게임·에디터 Tick에서 non-blocking UDP 소켓을 읽는다. `CLD2`는 선언된 field를 모두
조립·검증한 뒤 density를 넘긴다. 현재 렌더링에 쓰는 field는 density뿐이다.
패킷 규격은 [CLD2](protocol-v2.md), [SKS1](sky-state-protocol.md),
[SKC1](sky-control.md)을 따른다.

완성 프레임은 다음 값을 함께 보관한다.

```text
FrameId, SimulationTime, ReceivePlatformSeconds
GridSize, ValueScale, ValueBias, Bytes
```

밀도는 X가 가장 빠른 순서의 little-endian UNORM16이다. 수신기는 격자 크기와 바이트 수,
유한한 메타데이터를 확인한다. 미완성 프레임은 최대 8개, field는 최대 64MiB이며
1초 이상 갱신되지 않은 조립 상태는 정리한다.

완성 프레임의 ID가 중복·역순이거나 solver 시간이 증가하지 않으면 현재·이전 프레임과
revision을 유지한다. `uint32` ID 순환은 허용한다. 정상적인 새 격자에는 이전 프레임을
짝짓지 않는다. 재연결·서버 재시작은 `BreakContinuity()` 후 새 구간으로 받는다.
액터는 SKS1 재시작을 감지하면 조립 상태도 비우고, density 수신 공백이 2초를 넘는 경우에도
새 보간 구간을 시작한다.

`SKS1.volume_frame_id`는 송신 상태의 힌트다. 해당 CLD2가 도착할 때까지 조명이나
렌더링을 멈추지 않는다.

## 지연 재생과 바람

완성 밀도는 최대 16개 보관한다. 기본 재생 지연은 0.30초이며 수신 시각으로 이전·현재
프레임과 `alpha`를 고른다. 같은 수신 시각에 완성된 묶음은 마지막 프레임을 사용한다.

```text
target = max(now - delay, previousTarget)
alpha  = clamp((target - receipt0) / (receipt1 - receipt0), 0, 1)
solverTime = lerp(simulationTime0, simulationTime1, alpha)
```

범위 밖에서는 첫 프레임 또는 마지막 프레임을 유지한다. 수신 중단 후 마지막 프레임에
도달하면 solver 시간과 누적 이동량도 멈춘다. `StopReceiving()`은 재생 버퍼를 보존해
수신을 끈 상태에서도 밀도 설정을 바꿀 수 있다. 다음 연결의 프레임은 새 구간으로 시작한다.

바람은 SKS1의 평균 수평 풍속을 사용한다. 표시된 solver 시간의 증가량에 이전·현재 풍속의
평균을 곱해 이동량을 누적한다. 한 축의 endpoint 보정은 다음과 같다.

```text
windUv = windMetersPerSecond / domainMeters * (gridSize - 1) / gridSize
dt = simulationTime1 - simulationTime0
previousUv = sampleUv - windUv * alpha * dt
currentUv  = sampleUv + windUv * (1 - alpha) * dt
```

이 부호로 이전 프레임은 앞으로, 현재 프레임은 뒤로 맞춘 뒤 혼합한다. 구름 내부의
voxel별 속도와 고도별 wind shear는 쓰지 않는 평균 바람 근사다.

regional·detail 노이즈도 같은 누적 이동량을 사용한다. 노이즈 타일 배율을 적용한 뒤
최종 좌표 공간에서 위상을 wrap한다. 보조 밀도 좌표에는 `(1 - SecondaryPatternScale)`만큼
이동량을 보정해 원본과 같은 속도로 움직이게 한다.

## 밀도 전처리

기본 설정은 `Stabilize Density Scale = true`, `Density Reference Scale = 8`이다.
프레임별 인코딩 최댓값이 달라져도 같은 물리 밀도는 같은 표시 밀도로 변환한다.

```text
physicalDensity = uint16Value / 65535 * ValueScale + ValueBias
displayDensity  = clamp(physicalDensity / DensityReferenceScale, 0, 1)
```

이후 선택적인 3축 max-filter 확산, `Density Shape Power`, `Density Presentation Gain`을
적용하고 G16으로 변환한다. 기준값은 0.001~100000으로 제한하며 NaN·무한대는 8로 바꾼다.

안정화를 끄면 기존 정규화 값에 형태·배율만 적용한다. 단, `Use Physical Density Scale
For Rendering`이 켜져 있으면 두 프레임을 반드시 공통 기준으로 복원한다. 서로 다른
scale의 밀도와 scale을 따로 보간한 뒤 곱하면 물리 밀도 보간과 달라지기 때문이다.
이 옵션에서는 공통 기준을 머티리얼에서 다시 곱한다. 서버 밀도와 Unreal 소멸 계수의
물리 단위 보정은 아직 별도 과제다.

CPU 결과는 프레임 식별자·설정·필요한 scale/bias로 캐시한다. 새 endpoint나 설정 변경이
없으면 전체 볼륨을 다시 처리하지 않는다. 전처리 작업 버퍼도 재사용한다.

## GPU 슬롯과 머티리얼 계약

서로 다른 이름과 객체를 가진 transient `PF_G16 UVolumeTexture` 두 개를 사용한다.
이전 쌍이 A/B이면 다음 쌍은 B/A로 재사용한다. 동일 endpoint가 양쪽인 경우에는 한
텍스처를 함께 참조한다. 텍스처 배열과 공개 텍스처 참조는 UPROPERTY로 보관한다.

| 머티리얼 파라미터 | 바인딩 |
|---|---|
| `DensityVolume`, `DensityVolumeSecondary` | 현재 프레임 텍스처 |
| `DensityVolumePrevious`, `DensityVolumeSecondaryPrevious` | 이전 프레임 텍스처 |
| `DensityFrameBlend` | 수신 시각에서 구한 alpha |
| `PreviousDensityOffset`, `CurrentDensityOffset` | endpoint별 바람 보정 |
| `WeatherMapOffset`, `DetailPhaseOffset`, `SecondaryMotionOffset` | 누적 노이즈·보조 좌표 위상 |
| `DensityValueScale`, `DensityValueBias` | 현재 표시 방식의 밀도 배율·바이어스 |

각 endpoint에서 원본·보조 좌표를 읽으므로 밀도 샘플은 네 번이다. 두 공간 샘플을 regional
map으로 섞은 뒤 시간 보간하고, detail·regional erosion과 소멸 계수를 적용한다.
밀도 외 노이즈 텍스처 읽기는 별도다.

새 텍스처는 mip 데이터로 초기화하고 기존 텍스처는 render command로 갱신한다.
업로드 명령은 바이트·격자·RHI 참조를 직접 보관한다. 리소스가 준비되지 않으면 업로드
완료 revision을 올리지 않고 다음 Tick에서 재시도한다. 일반 Tick에서는 보간·위상·밀도
uniform만 갱신한다.

기본 머티리얼은 생성 버전 15다. 에디터 모듈과 Python 생성기를 함께 유지해야 하며,
기본 에셋이 없거나 버전이 다르면 에디터 시작 시 그래프를 재생성한다.

## 좌표·표시 범위

액터의 로컬 X/Y/Z는 east/north/up이다. 광원 방향은 액터 회전을 적용하며,
관측자에서 태양·달을 향하는 서버 벡터를 반전해 Directional Light에 넣는다.

볼륨의 로컬 위치를 렌더 해상도로 나눠 `[0,1]` 좌표를 얻는다. X/Y에는 수평 반복 수를
곱하고 연속 wrap sampling한다. Z만 반 texel 안쪽으로 clamp해 위·아래 경계가 섞이지
않게 한다. regional 위치 왜곡과 비정수 보조 좌표로 반복을 완화하며 detail noise는 mip 2를
사용한다.

README 프리뷰 설정의 물리 영역은 20km × 20km × 14km이며 서버 옵션의 기본 높이는 12km다.
기본 표시 폭은 120km다. 수평 렌더 해상도는 반복 수와 품질 설정을 반영하되 축당 1024로
제한한다. 이 구조는 넓어진 만큼 새로운 물리를 계산하지 않으며, 고고도에서는 사각 볼륨의
외곽과 밀도 반복이 보일 수 있다.

## 하늘 시계와 조명

UTC는 SKS1 수신 시각과 배속을 기준으로 패킷 사이를 진행한다. 서버가 끊기면 마지막
기준에서 로컬 시계를 계속 사용한다. 태양 방향은 유효 UTC·위치로 갱신하며 달·조명·안개는
서버 상태와 연결 단절 시 대체 계산을 사용한다.

구름 이동은 이 UTC와 별개인 CLD2 solver 시간으로 계산한다. UTC 변경을 누적 바람 좌표에
직접 곱하지 않는다. `Time Scale = 0`은 UTC·천체 시계를 멈추지만 진행 중인 날씨 전환·완화,
젖음 변화와 유체 계산은 계속될 수 있다.

## 검증과 남은 작업

CPU 테스트는 수신·순서·밀도 변환·재생 시간을 검증한다. 실 D3D12 테스트는 두 텍스처의
독립성·재사용, MID 파라미터, 수신 중단 후 설정 변경과 리소스 재시도를 확인한다.
`SkySim.Cloud.Rendering.Integration`은 NullRHI에서 건너뛴다. 프로세스 종료 코드만으로
성공을 판단하지 말고 Automation 보고서의 실패·미실행 수를 확인한다.

실행 기록은 [구현 현황](implementation-status.md), 작업 순서는
[개발 일정](development-schedule.md)에서 관리한다. 광학 흐름으로 측정한 화면 이동 속도와
잔상 품질은 아직 검증하지 않았다.

남은 렌더링 작업은 반복·고고도 외곽 개선, voxel별 velocity 활용, 점유 영역 건너뛰기,
전용 구름·대기 패스, 강수·번개·지면 효과다. 전용 ray marcher나 clipmap, 품질 프리셋의
구체 수치는 구현·측정 후 정한다. 현재 엔진의 볼륨 렌더링과 혼동하지 않는다.

# Sky Simulation Server

외부 프로세스에서 시간·위치에 따른 하늘 상태와 구름의 큰 형태·운동을 계산하고 Unreal
Engine의 전용 GPU 렌더러로 전달하는 실시간 시뮬레이션 서버입니다. 서버는 카메라와 무관한
날씨·천체·대기 광학·유체 상태를 만들고, Unreal은 하나의 `SkySimSystem`에서 이를 받아
화면 종속 ray march와 합성을 수행하는 구조를 전제로 합니다.

## 현재 구현

- 3차원 반-라그랑주 이류(advection)
- 온도와 수증기에 의한 부력
- 포화 수증기량에 따른 응결과 증발
- 반복 압력 투영을 통한 속도장 안정화
- 장시간 실행에 안전한 `double` 시뮬레이션 시간
- 렌더러용 고정밀 원본 밀도 `R16_UNORM`
- 빠른 물체 후류에서도 약한 바람 정밀도를 보존하는 속도 `RGB16_SNORM`
- 온도와 수증기 보조 볼륨
- 빈 공간 건너뛰기용 coarse occupancy 볼륨
- 멀티필드 UDP 프로토콜 v2
- 명시적 little-endian 직렬화, 필드별 CRC32, 선택적 RLE 압축
- non-blocking UDP 송신과 frame/packet drop 계측
- `CLC2` 동적 물체 제어: moving-solid 압력 경계, 보존적 scalar 밀어내기,
  swept 후류·난류·날개 끝 와류 쌍
- UTC·위도·경도·고도와 time scale에 따른 태양/달 방향, 각반경, 조도와 별 노출
- 기온·기압·습도·시정·바람·돌풍·강수·적설 비율·대류·번개·지면 젖음 상태
- clear, cumulus, overcast, rain, storm, snow, fog preset과 seed 기반 natural weather
- 최대 4개 고도 구름층, 대기 Rayleigh/Mie/ozone 계수와 물리 단위 바람·상승가속도의 구름
  유체 forcing 연동
- `SKS1` 512바이트 전역 하늘 상태와 `SKC1` 실시간 날씨 제어 프로토콜
- seed 기반 cloud-family 군집, 위성 셀, 레이어·수명별 크기/높이/두께 변화
- Cumulus/Overcast/Rain/Snow의 다중 고도 레이어와 가속·역방향 Natural weather 추종
- Unreal Engine 5.6 `SkySimSystem`: Play 없이 editor preview, 태양·달·대기·안개와
  Heterogeneous Volume 구름 렌더링
- 120km Wide Cloud World, 광역 청천 통로, 좌표 warp와 비정수 이중 density sample 기반
  de-tiling
- WASD/QE 자유 비행 Spectator와 정상 Mouse Y/선택적 invert
- 첫 전송부터 구름이 보이도록 하는 기본 2초 사전 시뮬레이션
- 기존 단일 밀도 프로토콜 v1 호환 모드
- macOS, Linux, Windows 소켓 코드

현재 기본 격자는 `64 x 64 x 64`입니다. 서버 밀도는 구름의 큰 형태와 물체 후류를
나타냅니다. 포함된 Unreal 기준 구현은 엔진 erosion volume, regional coverage mask,
좌표 warp와 두 개의 live-density lookup으로 표면과 원거리 반복을 보정합니다. Unreal 기본
`VolumetricCloud`는 사용하지 않습니다. velocity 기반 temporal 보간과 전용 all-sky ray
marcher는 현재 구현이 아니라 다음 렌더러 단계입니다.

## 빌드

C++17 컴파일러와 CMake 3.16 이상이 필요합니다. 검증/제어 Python 도구와 해당 테스트는
Python 3.10 이상을 사용합니다.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure
```

macOS/Linux의 실행 파일은 `build/cloud_sim_server`, Visual Studio 계열 Windows 빌드는
`build/Release/cloud_sim_server.exe`에 생성됩니다. 아래 예시는 macOS/Linux 경로이며
Windows에서는 실행 파일 경로만 바꾸면 옵션은 같습니다.

## 실행

구름 volume과 하늘 상태를 각각 검증하려면 서로 다른 두 터미널에서 수신기를 실행합니다.

```bash
python3 tools/receive_probe.py --port 7777 --output-dir received-frame
```

```bash
python3 tools/receive_sky.py --port 7779
```

다른 터미널에서 서버를 실행합니다. 기본값은 모든 `CLD2` 렌더링 field, 자동 압축,
`SKS1` 5 Hz, 서울 위치, seed 기반 natural 날씨입니다.

```bash
./build/cloud_sim_server --grid 64 --hz 20 --send-hz 10 --host 127.0.0.1 --port 7777
```

특정 일시·위치에서 자연 날씨와 빠른 일주 변화를 시험할 수 있습니다.

```bash
./build/cloud_sim_server \
  --utc 2026-08-26T07:00:00Z \
  --latitude 37.5665 --longitude 126.9780 --elevation-m 38 \
  --weather natural --weather-seed 55 --time-scale 60
```

실행 중 폭풍으로 8초간 부드럽게 전환하려면 제어 도구를 사용합니다.

```bash
python3 tools/send_sky_control.py --transition 8 preset storm
```

서버만 짧게 시험하려면 다음처럼 실행할 수 있습니다.

```bash
./build/cloud_sim_server --no-send --no-sky --seconds 5
```

즉시 0초 상태부터 관찰해야 하면 `--warmup-seconds 0`을 사용합니다.

비행기/물체 상호작용은 서버를 실행한 상태에서 별도 터미널로 바로 시험할 수 있습니다.

```bash
python3 tools/send_interactor.py animate --shape ellipsoid --duration 0.8 --rate 60 --strength 2
```

동적 물체는 기본 8개, 명시적 상한 64개입니다. 많은 대형 proxy나 긴 후류가 한 tick의
voxel 작업 예산을 넘으면 서버는 물체 처리 시작점을 매 tick 순환하고
`quality[..., budget=exhausted]`를 기록합니다. 실시간 운용에서는 이 로그가 `ok`인지
확인한 뒤 필요할 때만 `--max-interactors`를 올리십시오.

Unreal 좌표 변환과 최소 컴포넌트 구성, 128바이트 제어 패킷은
[동적 interactor/Unreal 연결 문서](docs/interactor-control.md)에 정리되어 있습니다.

하늘 상태의 정확한 512바이트 배치와 날씨 제어 계약은 각각
[SKS1 상태 문서](docs/sky-state-protocol.md), [SKC1 제어 문서](docs/sky-control.md)에
정리되어 있습니다.

밀도와 속도만 보내 전송량을 줄일 수도 있습니다.

```bash
./build/cloud_sim_server --fields density,velocity --compression auto
```

기존 v1 수신기와 연결해야 할 때는 다음 호환 모드를 사용합니다.

```bash
./build/cloud_sim_server --protocol 1
```

## 프로토콜 v2 필드

| Field ID | 이름 | 형식 | 물리값 복원 | 렌더러 용도 |
|---:|---|---|---|---|
| 1 | density | `R16_UNORM` | `sample * value_scale + value_bias` | 기본 extinction/shape |
| 2 | velocity | `RGB16_SNORM` | `sample * value_scale + value_bias` | 프레임 사이 advection/reprojection 및 물체 후류 |
| 3 | temperature | `R8_UNORM` | `sample * 4 - 1` | 상승부와 구름 유형 보조 |
| 4 | vapor | `R8_UNORM` | `sample * value_scale` | 응결 가능 영역과 디테일 보조 |
| 5 | occupancy | `R8_UNORM` | `sample * value_scale` | 빈 공간 ray-march skip |

모든 볼륨의 메모리 순서는 `x`가 가장 빠르고 그다음 `y`, `z`입니다.

```text
index = (z * grid_y + y) * grid_x + x
```

속도 필드는 voxel당 `x, y, z` 순서로 세 채널이 연속 저장됩니다. occupancy의 한 voxel이 나타내는 원본 격자 폭은 헤더 `flags`의 상위 8비트에 기록됩니다. 기본값은 `4`입니다.

전체 v2 헤더와 RLE 형식은 [프로토콜 v2 문서](docs/protocol-v2.md)에 정의되어 있습니다.
현재 Unreal 프로젝트 사용법은 [Unreal Editor 사용 가이드](docs/unreal-editor-guide.md),
전용 ray marcher와 temporal 확장 설계는
[Unreal 렌더링 아키텍처와 로드맵](docs/unreal-rendering.md)을 따릅니다.

## Unreal Engine 5.6 프로젝트

`Unreal/uskysim`에는 실행 가능한 기준 프로젝트가 포함됩니다. `NewWorld`의
`SkySimSystem` 액터 하나가 Play 없이 `CLD2`/`SKS1`을 수신하고, Details에서 날짜·위치,
시간 배속, Natural/preset/고급 날씨, 최대 네 개 구름층, 태양·달·Sky Light, Weather Fog,
광역 구름과 presentation 값을 직접 조정합니다.

서버의 물리 reference domain은 기본 20km이고 Unreal은 이를 기본 120km로 확장합니다.
서버 쪽 cloud-family 군집과 크기·높이 변화에 더해, 렌더러 쪽 regional clear-sky mask,
position warp와 비정수 보조 density sample이 균일한 배치와 6×6 반복을 줄입니다.

UE 에디터 프리뷰에는 실제 사용하는 density만 5Hz로 보내는 다음 구성이 부하와 반응성의
균형이 좋습니다. 저장된 `NewWorld`는 Natural seed 55와 60배속을 사용합니다.

```powershell
& ".\build\Release\cloud_sim_server.exe" `
  --grid 64 --hz 5 --send-hz 5 --protocol 2 --fields density --compression auto `
  --weather natural --weather-seed 55 --time-scale 60 `
  --domain-width-m 20000 --domain-height-m 14000
```

```powershell
$ueRoot = "C:\Program Files\Epic Games\UE_5.6"
$project = (Resolve-Path ".\Unreal\uskysim\uskysim.uproject").Path

& "$ueRoot\Engine\Build\BatchFiles\Build.bat" `
  uskysimEditor Win64 Development `
  "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

현재 처리 경로는 game/editor tick 수신, RLE/CRC/frame 조립, density CPU shaping,
transient `PF_G16` texture upload, Heterogeneous Volume material 표시와 `SKS1` 환경 component
갱신입니다. velocity 보간, occupancy skip, custom RDG ray march와 temporal upscale는 목표
구조이며 현재 기능으로 간주하면 안 됩니다.

전체 설치, Details 속성, Spectator 조작, 안개 구분, 머티리얼 재생성과 문제 해결은
[Unreal Editor 사용 가이드](docs/unreal-editor-guide.md)에 정리되어 있습니다.
이번 Unreal 통합과 구름 분포 개선의 파일별 범위·기본값·검증 항목은
[구현 현황 및 검증 기록](docs/implementation-status.md)을 참고하십시오.

## 해상도와 전송량

`128³`는 `64³`보다 voxel 수가 8배입니다. 모든 v2 필드를 매 프레임 보낼 경우 압축되지 않은 크기는 voxel당 약 10바이트이므로 고해상도에서는 전송량이 빠르게 증가합니다. 먼저 `64³` 전체 필드로 렌더러를 연결하고, 이후 다음 순서로 확장하는 것을 권장합니다.

1. `128³` density와 낮은 빈도의 velocity 전송
2. 변경된 3D brick만 보내는 sparse/delta 스트리밍
3. 중요 brick 우선순위와 손실 복구
4. 서버 시뮬레이션 GPU 이전

## 프로토콜 v1

v1은 40바이트 `CLD1` 헤더와 `uint8` optical density 하나만 보냅니다. 신규 언리얼 렌더러는 원본 밀도와 속도를 제공하는 v2를 사용해야 합니다.

## 현재 한계와 확장 순서

현재 natural weather는 날짜·위치·seed에 따라 연속적으로 변하는 결정론적 절차 모델입니다.
실제 수치예보나 관측 동화가 아니며, 지역별 전선·지형성 상승·레이더 강수대를 공간적으로
재현하지 않습니다. 번개는 event/flash 상태까지만 만들며 볼트 위치·형상·음향, 비·눈
입자와 젖은 지면 표현은 후속 Unreal 렌더 패스가 구현해야 합니다. 현재 포함된 Unreal
프로젝트는 Heterogeneous Volume 기반 구름, 태양·달·대기·Sky Light와 시정·습도 기반
안개, editor preview, Spectator 조작까지 연결하며, 강수 입자·번개 볼트·젖은 지면 셰이더는
아직 범위 밖입니다.

개발 단계는 다음 순서가 안전합니다.

1. **현재 기반**: 시간·위치·preset/natural weather, 태양·달, 대기 광학, 구름 forcing,
   `SKS1`/`SKC1`
2. **Unreal 단일 시스템**: 현재 Heterogeneous Volume 연결을 전용 구름 ray marcher,
   해·달·별과 depth/shadow 합성으로 확장
3. **국지 현상**: 3D 강수 shaft, 지표 안개, 번개 위치/분기, 젖음·적설 feedback
4. **중규모 날씨**: 전선과 기단, 지형·해륙풍, 공간 pressure/humidity/temperature field
5. **대규모 운용**: sparse multi-domain GPU simulation, 관측/예보 입력, timeline/replay,
   손실 복구와 다중 클라이언트 동기화

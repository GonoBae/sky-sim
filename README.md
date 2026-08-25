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
- 첫 전송부터 구름이 보이도록 하는 기본 2초 사전 시뮬레이션
- 기존 단일 밀도 프로토콜 v1 호환 모드
- macOS, Linux, Windows 소켓 코드

현재 기본 격자는 `64 x 64 x 64`입니다. 서버 밀도는 구름의 큰 형태와 물체 후류를
나타내며, TrueSky 계열의 표면 디테일은 Unreal GPU에서 월드 좌표 기반
Worley/Perlin/Curl 노이즈와 대기·구름 산란으로 추가합니다. Unreal 기본
`VolumetricCloud`는 사용하지 않습니다.

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
  --utc 2026-08-25T03:00:00Z \
  --latitude 37.5665 --longitude 126.9780 --elevation-m 38 \
  --weather natural --weather-seed 42 --time-scale 3600
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
custom ray marcher와 조명·노이즈·temporal 구성은
[Unreal 렌더링 가이드](docs/unreal-rendering.md)를 따릅니다.

## Unreal 렌더러 계약

언리얼 플러그인은 다음 순서로 처리하는 것을 권장합니다.

1. UDP 스레드에서 같은 `frame_id`의 필드를 조립하고 CRC를 검사합니다.
2. `SKS1`을 검증하고 UTC·태양/달·대기·날씨 상태 두 개를 시간 보간합니다.
3. 모든 `field_mask` 필드가 완성된 최신 프레임만 렌더 스레드로 전달합니다.
4. density를 3D `R16_UNORM`, velocity를 3D `RGB16_SNORM` GPU 리소스로 업로드합니다.
5. 직전/현재 density를 velocity로 보간하여 기본 10Hz 전송의 끊김을 감춥니다.
6. occupancy를 사용해 빈 구간을 건너뛰고 서버 density 가장자리에 고주파 절차 노이즈를 적용합니다.
7. `SKS1` 광학 계수와 천체 방향으로 대기 LUT, 구름 조명, 해·달·별, 강수·안개를 한 render pipeline에서 합성합니다.
8. 저해상도 ray march, temporal accumulation, bilateral upscale 순서로 Scene Color에 합성합니다.

수신기는 UDP 콜백에서 UObject나 RHI 리소스를 직접 수정하지 않고, 완성된 CPU 버퍼만 game/render thread에 넘겨야 합니다.

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
입자와 젖은 지면 표현은 Unreal 렌더러가 구현해야 합니다. 이 저장소에는 아직 Unreal
플러그인 자체가 포함되어 있지 않습니다.

개발 단계는 다음 순서가 안전합니다.

1. **현재 기반**: 시간·위치·preset/natural weather, 태양·달, 대기 광학, 구름 forcing,
   `SKS1`/`SKC1`
2. **Unreal 단일 시스템**: 대기 LUT, 전용 구름 ray marcher, 해·달·별과 depth/shadow 합성
3. **국지 현상**: 3D 강수 shaft, 지표 안개, 번개 위치/분기, 젖음·적설 feedback
4. **중규모 날씨**: 전선과 기단, 지형·해륙풍, 공간 pressure/humidity/temperature field
5. **대규모 운용**: sparse multi-domain GPU simulation, 관측/예보 입력, timeline/replay,
   손실 복구와 다중 클라이언트 동기화

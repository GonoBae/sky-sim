# Cloud Simulation Server

외부 프로세스에서 구름의 큰 형태와 운동을 계산하고 Unreal Engine의 전용 GPU 렌더러로 전달하기 위한 실시간 구름 시뮬레이션 서버입니다. 이 서버는 카메라에 의존하지 않는 물리 데이터와 빈 공간 가속 정보를 만들고, 언리얼 플러그인은 절차적 미세 디테일·조명·레이마칭·시간적 누적을 담당하는 구조를 전제로 합니다.

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
- 첫 전송부터 구름이 보이도록 하는 기본 2초 사전 시뮬레이션
- 기존 단일 밀도 프로토콜 v1 호환 모드
- macOS, Linux, Windows 소켓 코드

현재 기본 격자는 `64 x 64 x 64`입니다. 서버 밀도는 구름의 큰 형태를 나타내며, TrueSky와 비슷한 표면 디테일은 언리얼 GPU에서 월드 좌표 기반 Worley/Perlin/Curl 노이즈로 추가하는 것을 권장합니다.

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

터미널 1에서 수신 검증기를 실행합니다.

```bash
python3 tools/receive_probe.py --port 7777 --output-dir received-frame
```

터미널 2에서 서버를 실행합니다. 기본값은 프로토콜 v2의 모든 렌더링 필드와 자동 압축입니다.

```bash
./build/cloud_sim_server --grid 64 --hz 20 --send-hz 10 --host 127.0.0.1 --port 7777
```

서버만 짧게 시험하려면 다음처럼 실행할 수 있습니다.

```bash
./build/cloud_sim_server --no-send --seconds 5
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
2. 모든 `field_mask` 필드가 완성된 최신 프레임만 렌더 스레드로 전달합니다.
3. density를 3D `R16_UNORM`, velocity를 3D `RGB16_SNORM` GPU 리소스로 업로드합니다.
4. 직전/현재 density를 velocity로 보간하여 기본 10Hz 전송의 끊김을 감춥니다.
5. occupancy를 사용해 빈 구간을 건너뛰고 서버 density 가장자리에 고주파 절차 노이즈를 적용합니다.
6. 저해상도 ray march, 태양 투과도, 다중 산란 근사, temporal accumulation, bilateral upscale 순서로 Scene Color에 합성합니다.

수신기는 UDP 콜백에서 UObject나 RHI 리소스를 직접 수정하지 않고, 완성된 CPU 버퍼만 game/render thread에 넘겨야 합니다.

## 해상도와 전송량

`128³`는 `64³`보다 voxel 수가 8배입니다. 모든 v2 필드를 매 프레임 보낼 경우 압축되지 않은 크기는 voxel당 약 10바이트이므로 고해상도에서는 전송량이 빠르게 증가합니다. 먼저 `64³` 전체 필드로 렌더러를 연결하고, 이후 다음 순서로 확장하는 것을 권장합니다.

1. `128³` density와 낮은 빈도의 velocity 전송
2. 변경된 3D brick만 보내는 sparse/delta 스트리밍
3. 중요 brick 우선순위와 손실 복구
4. 서버 시뮬레이션 GPU 이전

## 프로토콜 v1

v1은 40바이트 `CLD1` 헤더와 `uint8` optical density 하나만 보냅니다. 신규 언리얼 렌더러는 원본 밀도와 속도를 제공하는 v2를 사용해야 합니다.

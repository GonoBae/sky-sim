# Cloud Simulation Server

외부 프로세스에서 구름 물리를 계산하고 Unreal Engine 쪽으로 3차원 밀도 볼륨을 보내기 위한 첫 번째 프로토타입입니다.

## 현재 구현

- 3차원 반-라그랑주 이류(advection)
- 온도와 수증기에 의한 부력
- 포화 수증기량에 따른 응결과 증발
- 반복 압력 투영을 통한 속도장 안정화
- `uint8` 구름 밀도 볼륨 생성
- MTU를 넘지 않는 1,200바이트 조각으로 UDP 전송
- macOS, Linux, Windows 소켓 코드

이 버전은 기상 예보 모델이 아니라 실시간 그래픽용 물리 프로토타입입니다. 기본 격자는 `64 x 64 x 64`입니다.

## 빌드

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

## 실행

터미널 1에서 먼저 수신 검증기를 실행합니다.

```bash
python3 tools/receive_probe.py --port 7777 --output cloud-density.raw
```

터미널 2에서 서버를 실행합니다.

```bash
./build/cloud_sim_server --grid 64 --hz 20 --send-hz 10 --host 127.0.0.1 --port 7777
```

서버만 짧게 시험하려면 다음처럼 실행할 수 있습니다.

```bash
./build/cloud_sim_server --no-send --seconds 5
```

`128³`는 `64³`보다 셀 수가 8배입니다. CPU 프로토타입에서는 먼저 `64³`로 연결을 확인한 뒤 올리는 것을 권장합니다.

## UDP 프로토콜 v1

모든 정수와 실수는 little-endian입니다. 하나의 UDP 패킷은 40바이트 헤더와 최대 1,200바이트의 payload로 구성됩니다.

| Offset | Type | Name | Description |
|---:|---:|---|---|
| 0 | char[4] | magic | `CLD1` |
| 4 | uint16 | version | `1` |
| 6 | uint16 | header_bytes | `40` |
| 8 | uint32 | frame_id | 밀도 프레임 번호 |
| 12 | float32 | simulation_time | 시뮬레이션 시간(초) |
| 16 | uint16 x3 | grid_x/y/z | 볼륨 크기 |
| 22 | uint8 | voxel_format | `1`: uint8 |
| 23 | uint8 | field_id | `1`: cloud density |
| 24 | uint16 | chunk_index | 현재 조각 번호, 0부터 시작 |
| 26 | uint16 | chunk_count | 전체 조각 수 |
| 28 | uint16 | payload_bytes | 현재 payload 크기 |
| 30 | uint16 | reserved | `0` |
| 32 | uint32 | payload_offset | 프레임 내 바이트 위치 |
| 36 | uint32 | frame_bytes | 전체 프레임 크기 |

볼륨 메모리 순서는 `x`가 가장 빠르고 그다음 `y`, `z`입니다.

```text
index = (z * grid_y + y) * grid_x + x
```

Unreal 수신기는 `frame_id`별 버퍼를 만든 뒤, `payload_offset` 위치에 각 payload를 복사합니다. 모든 `chunk_index`를 받았을 때 해당 버퍼를 3D 텍스처로 업로드합니다. UDP에서는 조각이 유실될 수 있으므로 완성되지 않은 오래된 프레임은 버리고 다음 프레임을 사용하는 방식이 적합합니다.

## 다음 구현 단계

1. Unreal Engine C++ 수신 컴포넌트와 런타임 3D 텍스처 업로드
2. 서버로 열원·비행기·바람 명령을 보내는 역방향 제어 프로토콜
3. 변경된 3D 블록만 보내는 sparse brick 전송
4. 시뮬레이션 GPU 이전과 더 높은 해상도
5. 강수, 결빙, 번개와 복사 냉각 모델

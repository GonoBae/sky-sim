# 물체 제어 프로토콜 (`CLC2`)

`CLC2`는 물체의 위치·회전·속도를 서버로 보내는 단방향 UDP 프로토콜이다. 서버는 단순한 충돌 모양(proxy)을 사용해 세 효과를 계산한다.

- `solid`: 물체 표면과 내부의 유체 속도를 물체 속도에 맞춘다.
- `displacement`: 물체가 차지한 구름·수증기·온도 값을 표면 밖으로 이동한다.
- `wake`: 진행 반대쪽에 후류와 난류를 만든다.

서버 수신·상호작용 계산과 [참조 송신 도구](../tools/send_interactor.py)는 구현되어 있다. 현재 `Unreal/uskysim`에는 Actor 상태를 자동 전송하는 컴포넌트가 없다.

Unreal 렌더러는 서버가 보낸 density를 표시하며, velocity 필드를 이용한 세부 와류 표현은 아직 구현하지 않았다.

## 사용 예

서버의 기본 제어 주소는 `127.0.0.1:7778`이다. 다음 명령은 타원체를 3초 동안 X축으로 이동한 뒤 제거한다.

```bash
python3 tools/send_interactor.py animate \
  --shape ellipsoid \
  --start 0.15 0.50 0.55 --end 0.85 0.50 0.55 \
  --duration 3 --half-extent 0.06 0.025 0.025 \
  --rate 30 --strength 1.5
```

`animate`는 `(end-start)/duration`으로 선속도를 계산한다. `--keep`은 마지막 `remove`만 생략하며 TTL 만료를 막지는 않는다. `--rate`의 허용 범위는 1~240Hz이다.

한 번만 갱신하거나 명시적으로 제거하려면 다음과 같이 보낸다. 여러 프로세스 호출에서 같은 세션을 이어 가려면 고정 `--source-port`, 같은 `--session`, 증가하는 `--sequence`를 함께 지정해야 한다. 이 공통 옵션과 `--host`·`--port`는 하위 명령 앞에 둔다.

```bash
python3 tools/send_interactor.py --source-port 17778 --session 100 --sequence 1 upsert \
  --id 42 --shape sphere --position 0.5 0.5 0.6 \
  --half-extent 0.04 0.04 0.04 --velocity 0.2 0 0

python3 tools/send_interactor.py --source-port 17778 --session 100 --sequence 2 remove --id 42
python3 tools/send_interactor.py --source-port 17778 --session 100 --sequence 3 clear
```

`upsert`와 `animate`는 `--rotation X Y Z W`, `--angular-velocity X Y Z`, `--ttl-ms`, `--strength`, `--flags`도 받는다. flags는 기본 `all`이며 `solid,wake` 같은 목록이나 비트마스크를 사용할 수 있다. 실행별 기본 session은 임의의 0이 아닌 값, sequence와 물체 ID는 각각 1이다.

## 좌표와 모양

위치·크기는 시뮬레이션 볼륨 기준으로 정규화하며, 속도는 정규화 길이/초이다. 축이 월드와 정렬된 볼륨에서는 성분별로 계산한다.

```text
normalized_position = (world_position - volume_min) / volume_world_size
normalized_velocity = world_velocity / volume_world_size
normalized_extent   = world_half_extent / volume_world_size
```

모든 입력에 같은 길이 단위를 쓰면 별도 cm→m 변환은 필요 없다. X 폭 `1,000,000cm`인 볼륨에서 X 속도 `25,000cm/s`는 `0.025 volume/s`이다.

볼륨 Actor의 원점이 중심이고 `local_half_size`가 스케일 적용 전 크기라면 다음과 같이 로컬 좌표로 바꾼다. 위치와 속도 모두 볼륨 스케일을 역변환해야 한다.

```text
local_position      = VolumeTransform.InverseTransformPosition(world_position)
normalized_position = (local_position + local_half_size) / (2 * local_half_size)
local_velocity      = VolumeTransform.InverseTransformVector(world_velocity)
normalized_velocity = local_velocity / (2 * local_half_size)
local_rotation      = inverse(volume_rotation) * world_rotation
```

회전은 물체 로컬축에서 볼륨축으로의 quaternion이며 순서는 `X, Y, Z, W`이다. 각속도는 볼륨 로컬축 기준 rad/s이다. 볼륨축과 서버축이 같으면 축 교환은 하지 않는다.

비균일한 볼륨에서 회전한 모양은 quaternion·extent만으로 정확히 표현하기 어려우므로 정육면체 볼륨이나 물체를 감싸는 proxy를 사용한다.

| 값 | CLI | `half_extent` 의미 |
|---:|---|---|
| 1 | `sphere` | X=Y=Z인 반지름 |
| 2 | `box` | 물체 로컬 X/Y/Z half-size |
| 3 | `capsule-z` | 로컬 Z축 capsule. X=Y=반지름, Z=cap 포함 half-height, Z≥반지름 |
| 4 | `ellipsoid` | 물체 로컬 X/Y/Z 반지름 |

복잡한 물체는 서로 다른 ID의 proxy 여러 개로 구성할 수 있다.

X/Y 경계는 반복되며 Z는 반복되지 않는다. Z 상·하단과 겹친 solid의 경계 속도는 유지되지만, 볼륨 밖으로 향하는 scalar 이동은 실패할 수 있다. 질량을 안정적으로 밀어내려면 중심을 bounding extent만큼 상·하단 안쪽에 둔다. 실패한 목표 수는 로그의 `invalid_targets`에서 확인한다.

## 패킷

명령 하나는 정확히 128바이트인 UDP 데이터그램 하나다. 정수와 IEEE-754 실수는 모두 little-endian이다.

| Offset | Type | 이름 | 의미 |
|---:|---|---|---|
| 0 | `char[4]` | magic | `CLC2` |
| 4 | `uint16` | version | `2` |
| 6 | `uint16` | packet_bytes | `128` |
| 8 | `uint32` | session_id | 실행마다 새로 만드는 0이 아닌 값 |
| 12 | `uint32` | sequence | 세션 전체의 명령 순서, uint32 wrap 허용 |
| 16 | `uint64` | interactor_id | 물체 ID. `clear`만 0 |
| 24 | `float64` | client_time | 클라이언트 monotonic 시간(초), 유한한 0 이상 값 |
| 32 | `uint8` | opcode | `1`: upsert, `2`: remove, `3`: clear |
| 33 | `uint8` | shape | `1`: sphere, `2`: box, `3`: capsule-Z, `4`: ellipsoid |
| 34 | `uint16` | flags | bit 0: solid, bit 1: displacement, bit 2: wake |
| 36 | `uint32` | ttl_ms | 수신 후 유효 시간, 50~2,000ms. 기본 300ms |
| 40 | `float32[3]` | position | 볼륨 기준 정규화 위치, 축별 `[0,1]` |
| 52 | `float32[4]` | rotation | 물체 로컬→볼륨 quaternion, X/Y/Z/W |
| 68 | `float32[3]` | half_extent | 모양별 정규화 half extent |
| 80 | `float32[3]` | linear_velocity | 정규화 길이/초 |
| 92 | `float32[3]` | angular_velocity | 볼륨축 기준 rad/s |
| 104 | `float32` | strength | 효과 강도 `(0,4]`, 기본 1 |
| 108 | `uint8[16]` | reserved | 모두 0 |
| 124 | `uint32` | crc32 | 바이트 0..123의 IEEE CRC32 |

```python
struct.Struct("<4sHHIIQdBBHI3f4f3f3f3ff16sI")  # 128 bytes
```

CRC는 반사 다항식 `0xedb88320`, 초기값·최종 XOR `0xffffffff`를 사용한다. Python에서는 `zlib.crc32(packet[:124]) & 0xffffffff`이다.

`remove`·`clear`는 shape부터 strength까지의 필드를 모두 0으로 보낸다. `remove`는 지정 물체, `clear`는 같은 source IP·port·session의 물체 전체를 지운다.

## 검증과 세션 수명

서버는 패킷 크기·magic·version·CRC·예약 바이트를 검사하고, `upsert`에 다음 조건을 적용한다.

- 모든 실수는 유한해야 하며 flags는 bit 0..2 중 하나 이상이어야 한다.
- 위치는 축별 `[0,1]`, 모든 half extent는 양수이다. box의 bounding extent는 `length(half_extent)`, 나머지는 `max(half_extent)`이며 최대 `0.45`이다.
- sphere의 X/Y/Z, capsule의 X/Y 반지름은 같아야 한다. 서버의 차이 허용치는 `0.0001`, 참조 도구는 `0.00001`이다. capsule의 Z는 반지름 이상이어야 한다.
- 선속도 크기는 최대 `2 volume/s`, 각속도 성분은 `[-100,100] rad/s`이며 `length(angular_velocity) * bounding_extent <= 2`도 만족해야 한다.
- quaternion은 정규화해 보낸다. 서버는 길이 제곱 `[0.81,1.21]`, 참조 도구는 `abs(length²-1) <= 0.001`을 허용한다. CLI는 입력 quaternion을 정규화한다.
- strength는 `(0,4]`, TTL은 50~2,000ms이다.

세션 키는 source IP·source port·`session_id`이다. 새 sequence는 `(new-old) mod 2³²`가 `1..2³¹-1`일 때만 인정하며, 순서는 물체별이 아닌 세션 전체에 적용된다.

제거·TTL 만료 후에도 마지막 sequence를 최대 30초 동안 보관한다(최대 256개, 가득 차면 오래된 기록부터 제거). 따라서 이전 패킷을 재전송하지 말고 항상 새 sequence를 사용한다.

TTL은 서버 수신 시점부터 계산한다. `client_time`은 연속 시뮬레이션 스냅샷 사이의 차이가 `(0,0.25]`초일 때만 반복 경계 이동과 다중 회전 경로 복원에 쓰며, 물리 timestep으로 사용하지 않는다.

서버는 동시 물체를 기본 8개, `--max-interactors`로 최대 64개까지 받으며 활성 세션 상한은 32개이다. 제어 입력은 poll당 기본 512개, 호출 인자의 상한은 4,096개이다. 별도의 초당 패킷 제한은 없다.

## 송신기 통합과 품질 조정

Unreal 송신기를 추가할 때는 실행 동안 UDP socket 하나를 재사용하고, 새 session과 증가하는 sequence를 유지한다. 물체마다 안정적인 ID를 부여해 주기적으로 `upsert`를 보내고, `EndPlay`에는 `remove`, 종료 시에는 필요하면 `clear`를 보낸다. 비정상 종료는 TTL이 정리한다.

시작값은 30Hz·TTL 300ms·strength 1이다. 갱신 간격은 TTL의 1/3 이하로 유지한다. 빠르거나 작은 물체는 60Hz부터 조정하되, 갱신 사이 이동이 proxy의 최소 지름을 넘으면 갱신률을 높이거나 proxy를 늘린다.

`64³ / 20Hz`에서 tick당 약 2 cells 이하의 이동은 대략 `0.625 volume/s`이다. 실제 물체가 서버 속도 상한을 넘으면 볼륨의 월드 크기를 키워 정규화 속도를 낮춘다.

서버는 각 half extent를 최소 1.5 cells로 확대한다. 작은 proxy는 반지름·반두께를 1.5~2 cells(`64³`에서 약 `0.024~0.031`)로 잡으면 조정하기 쉽다. 넓은 box·ellipsoid에는 조건에 따라 날개 끝 와류 쌍도 생성한다.

이동·회전 경로는 `max(0.75 cell, 최소 half extent의 절반)` 간격을 목표로 최대 48번 샘플링한다. 속도와 맞지 않는 큰 이동·회전은 순간 이동으로 처리한다.

tick당 후보 voxel 방문 예산은 solid 두 회 합계 `8N`, force/wake `8N`, 과거·현재 scalar displacement 합계 `8N`이다(`N=grid³`). 예산이 소진되면 로그에 `budget=exhausted`가 표시되고 해당 tick의 계산 범위가 줄어든다. 처리 시작 물체는 tick마다 순환한다.

`CLC2`에는 인증·암호화가 없다. 기본 loopback bind를 유지하고, 다른 PC에서 보내야 할 때만 `--control-host`를 바꿔 방화벽이나 VPN으로 송신자를 제한한다.

구현: [서버 수신기](../src/control.hpp), [상호작용 계산](../src/simulation.hpp). 볼륨 응답 형식은 [CLD2](protocol-v2.md)를 참고한다.

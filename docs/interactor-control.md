# Dynamic interactor control (`CLC2`)

`CLC2`는 비행기·차량·캐릭터 같은 Unreal 물체의 상태를 서버에 전달하는 단방향 UDP 제어 프로토콜입니다. Unreal은 물체의 위치와 속도만 주기적으로 보내고, 서버가 다음 효과를 밀도·속도 볼륨에 반영합니다.

- `solid`: 물체 표면과 내부의 유체 속도를 물체 속도에 결합
- `displacement`: 물체가 차지한 구름·수증기·온도 scalar를 표면 바깥으로 이동
- `wake`: 진행 반대쪽에 후류와 난류를 생성

따라서 Unreal의 기본 `VolumetricCloud` 충돌 설정은 필요하지 않습니다. Unreal 쪽 커스텀 볼륨 렌더러는 서버가 돌려준 density/velocity를 그리기만 하면 됩니다.

## 빠른 시험

서버의 기본 제어 포트는 `127.0.0.1:7778`입니다. 다음 명령은 타원체 프록시를 3초 동안 X축으로 통과시킨 뒤 제거합니다.

```bash
python3 tools/send_interactor.py animate \
  --shape ellipsoid \
  --start 0.15 0.50 0.55 \
  --end 0.85 0.50 0.55 \
  --duration 3 \
  --half-extent 0.06 0.025 0.025 \
  --rate 30 \
  --strength 1.5
```

빠른 비행기를 더 강하게 시험하려면 이동 시간을 줄이고 갱신률을 높입니다.

```bash
python3 tools/send_interactor.py animate \
  --shape ellipsoid \
  --start 0.10 0.45 0.55 \
  --end 0.90 0.55 0.55 \
  --duration 0.8 \
  --half-extent 0.05 0.025 0.025 \
  --rate 60 \
  --ttl-ms 300 \
  --strength 2.0
```

한 상태만 갱신하거나 물체를 명시적으로 지울 수도 있습니다. 서버의 세션 키에는 송신 IP와 UDP source port도 포함됩니다. 같은 실행 세션을 여러 프로세스 호출에 걸쳐 유지하려면 `--session`, 증가하는 `--sequence`, 고정 `--source-port`를 모두 지정해야 합니다. 실제 Unreal 플러그인은 실행 중 하나의 UDP socket을 계속 재사용합니다.

```bash
python3 tools/send_interactor.py --source-port 17778 --session 100 --sequence 1 upsert \
  --id 42 --shape sphere --position 0.5 0.5 0.6 \
  --half-extent 0.04 0.04 0.04 --velocity 0.2 0 0

python3 tools/send_interactor.py --source-port 17778 --session 100 --sequence 2 remove --id 42
python3 tools/send_interactor.py --source-port 17778 --session 100 --sequence 3 clear
```

`animate`는 기본적으로 끝에서 `remove`를 전송합니다. `--keep`을 지정하면 제거 패킷을 생략하지만, 갱신이 멈춘 물체는 TTL이 지나면 서버에서 자동 삭제됩니다.

## 좌표와 단위

프로토콜의 위치와 크기는 cloud simulation volume을 기준으로 정규화됩니다.

```text
normalized_position = (world_position - volume_min) / volume_world_size
normalized_velocity = world_velocity / volume_world_size
normalized_extent   = world_half_extent / volume_world_size
```

나눗셈은 X/Y/Z 성분별로 합니다. Unreal이 센티미터를 사용해도 위치·크기·속도 모두 같은 단위를 사용하면 별도의 `cm -> m` 변환은 필요 없습니다. 예를 들어 X 폭이 `1,000,000 cm`인 볼륨에서 `25,000 cm/s`로 움직이는 비행기의 X 속도는 `0.025 normalized unit/s`입니다.

볼륨 Actor의 원점이 중심이면 먼저 볼륨 로컬 좌표로 바꿉니다.

```text
local_position      = VolumeTransform.InverseTransformPosition(world_position)
normalized_position = (local_position + local_half_size) / (2 * local_half_size)

local_velocity      = VolumeTransform.InverseTransformVectorNoScale(world_velocity)
normalized_velocity = local_velocity / (2 * local_half_size)

local_rotation      = inverse(volume_rotation) * world_rotation
```

회전은 local-to-volume quaternion이며 필드 순서는 Unreal `FQuat`과 같은 `X, Y, Z, W`입니다. 각속도는 volume 로컬축 기준 `radian/s`입니다. 볼륨 축과 서버 축을 그대로 맞추면 축 교환은 하지 않습니다.

비균일한 volume 크기에서 회전한 물체는 정규화 과정 자체가 비균일 스케일을 만들 수 있습니다. 충돌 모양 정확도가 중요하면 simulation volume을 정육면체로 두거나, 실제 물체를 완전히 감싸는 보수적인 proxy extent를 보내는 것이 안전합니다.

### Shape 규칙

| 값 | CLI | `half_extent` 의미 |
|---:|---|---|
| 1 | `sphere` | X=Y=Z인 반지름. 세 값이 같아야 함 |
| 2 | `box` | 로컬 X/Y/Z half-size |
| 3 | `capsule-z` | 로컬 Z축 capsule. X=Y=반지름, Z=cap을 포함한 half-height이고 Z>=반지름 |
| 4 | `ellipsoid` | 로컬 X/Y/Z 반지름 |

비행기에는 보통 `ellipsoid`가 가장 간단합니다. 동체를 더 정확히 나타내려면 stable ID가 서로 다른 여러 ellipsoid/capsule proxy를 보낼 수 있습니다.

## Unreal 최소 구현

Unreal 프로젝트에는 하나의 `CloudInteractorSenderComponent`만 두는 구성이 가장 단순합니다.

1. 게임 시작 시 임의의 nonzero `session_id`를 한 번 만들고 전역 `sequence`를 1부터 증가시킵니다.
2. `CloudInteractor` 태그가 붙은 Actor를 모으고 stable `uint64 interactor_id`를 부여합니다. 실행 중 ID를 재사용하지 않는 편이 안전합니다.
3. 실행 중 같은 UDP socket/source port를 유지하고, 30~60Hz timer에서 각 Actor의 transform, physics linear velocity, angular velocity를 위 공식으로 변환합니다.
4. 아래 표의 offset에 little-endian 값을 직접 기록하고 CRC32를 계산한 뒤 UDP `127.0.0.1:7778`로 보냅니다.
5. Actor `EndPlay` 때 `remove`를 보내고, level 종료 때 선택적으로 `clear`를 보냅니다. 비정상 종료는 TTL이 정리합니다.

UDP 송신은 game/render thread를 기다리게 하지 않도록 작은 송신 큐나 별도 socket thread에서 처리하는 것을 권장합니다. 한 패킷이 128바이트뿐이므로 Unreal에서 구름 물리나 voxel 데이터를 만들 필요는 없습니다.

언리얼 에디터에서 필요한 값은 사실상 다음뿐입니다.

- simulation volume의 transform/월드 크기
- 서버 주소와 control port
- 물체별 shape, proxy extent, flags, strength

기본 `VolumetricCloud` Actor/Component는 제거하거나 비활성화합니다. 화면의 구름 표현은 별도의 서버 volume 수신·ray-march renderer가 담당합니다.

## 갱신률, TTL, 빠른 물체

권장 시작값은 `30Hz`, `ttl_ms=300`, `strength=1.0`입니다.

- TTL은 update interval의 최소 3배로 두고 heartbeat/update 간격은 TTL의 1/3 이하로 유지합니다. 기본 300ms는 30Hz update 약 9회에 해당해 일시적인 UDP 손실을 견디면서 연결 종료 후 빠르게 정리됩니다. 서버 허용 범위는 50~2,000ms입니다.
- 빠른 비행기나 작은 proxy는 60Hz를 권장합니다. 한 update 사이 이동 거리가 proxy의 가장 작은 지름보다 커지면 120Hz까지 올리거나 proxy를 길게 잡습니다.
- `strength`는 solid coupling, density displacement, wake의 공통 배율이며 범위는 `(0, 4]`입니다. 일반 물체는 `1.0`, 눈에 띄는 항공기 후류는 `1.5~2.5`부터 조정합니다.
- 선속도는 normalized volume unit/s, 각속도는 rad/s입니다. `animate` 명령은 `(end-start)/duration`으로 선속도를 자동 계산합니다.
- 기본 `64³ / 20Hz`에서 가장 선명한 후류를 얻으려면 한 simulation tick의 이동을 약 2 cells 이하, 즉 선속도 크기를 약 `0.625 volume/s` 이하로 맞춥니다. 서버의 안전 상한은 선속도 크기 `2 volume/s`이며 그보다 빠른 실제 항공기는 simulation volume의 월드 크기를 키워 정규화 속도를 낮춥니다.

서버는 직전/현재 translation과 rotation 사이를 proxy 두께에 맞춰 swept sampling합니다.
가장 작은 proxy는 약 `0.75 cell`, 큰 proxy는 가장 작은 half extent의 절반을 목표 간격으로
사용하고 물체별 상한은 48 samples입니다. 서버는 각 단계의 후보 voxel 방문량을 격자
크기의 일정 배수로 제한합니다(두 solid raster pass 합계 `8N`, force/wake `8N`, 과거/현재
scalar displacement 합계 `8N`, `N=grid³`). 예산을 넘으면 다음 tick의 시작 물체를 순환해
영구 누락을 막고 로그에 `budget=exhausted`를 표시합니다. 이 상태는 입력을 잃었다는 뜻이
아니라 그 tick의 상호작용 품질이 축소됐다는 뜻입니다. 기본 동시 물체 상한은 실시간성을
위해 8개이며 `--max-interactors`로 최대 64개까지 늘릴 수 있습니다. 속도와 맞지 않는 큰
순간 이동은 teleport로 판단해 긴 구름 터널을 만들지 않습니다.

서버의 기본 `64^3` macro grid에서는 한 cell이 정규화 길이 `1/64`입니다. 서버가 각 proxy 축을 최소 `1.5 cells`로 확대하지만, 렌더용 물체 크기와 별개로 cloud physics proxy의 최소 반지름/두께를 약 `1.5~2 cells`(`64^3`에서 `0.024~0.031`)로 명시하는 편이 결과를 예측하기 쉽습니다. 이것은 실제 mesh 충돌이 아니라 구름의 큰 흐름을 만드는 프록시입니다. 폭이 넓은 box/ellipsoid proxy에는 서버가 서로 반대 방향의 날개 끝 와류 쌍을 만들며, Unreal에서는 수신 velocity를 이용한 고주파 curl noise로 작은 소용돌이를 보강합니다.

## `CLC2` packet

모든 수와 IEEE-754 실수는 little-endian입니다. 한 command는 UDP datagram 하나이며 길이는 항상 128바이트입니다.

| Offset | Type | 이름 | 설명 |
|---:|---:|---|---|
| 0 | `char[4]` | magic | `CLC2` |
| 4 | `uint16` | version | `2` |
| 6 | `uint16` | packet_bytes | `128` |
| 8 | `uint32` | session_id | 클라이언트 실행마다 새 nonzero 값 |
| 12 | `uint32` | sequence | session 전체에서 단조 증가, uint32 wrap 허용 |
| 16 | `uint64` | interactor_id | 물체 stable ID. `clear`만 0 |
| 24 | `float64` | client_time | 클라이언트 monotonic seconds. 250ms 이하의 연속 update 간격만 periodic 이동·다중 회전 경로 복원에 사용하며 서버 물리 timestep으로는 사용하지 않음 |
| 32 | `uint8` | opcode | 1 upsert, 2 remove, 3 clear |
| 33 | `uint8` | shape | 1 sphere, 2 box, 3 capsule-Z, 4 ellipsoid |
| 34 | `uint16` | flags | bit 0 solid, bit 1 displacement, bit 2 wake |
| 36 | `uint32` | ttl_ms | upsert 유효 시간. 기본 300ms, 허용 50~2,000ms |
| 40 | `float32[3]` | position | normalized volume position `[0,1]` |
| 52 | `float32[4]` | rotation | local-to-volume quaternion X/Y/Z/W |
| 68 | `float32[3]` | half_extent | shape별 normalized half extent |
| 80 | `float32[3]` | linear_velocity | normalized volume unit/s |
| 92 | `float32[3]` | angular_velocity | simulation volume 축 기준 rad/s |
| 104 | `float32` | strength | 공통 효과 배율 `(0,4]` |
| 108 | `uint8[16]` | reserved | 모두 0이어야 함 |
| 124 | `uint32` | crc32 | offset 0..123의 IEEE CRC32 |

참조 struct 형식은 다음과 같습니다.

```python
struct.Struct("<4sHHIIQdBBHI3f4f3f3f3ff16sI")  # 128 bytes
```

`remove`와 `clear`는 interactor 전용 필드(shape부터 strength까지)를 0으로 보냅니다. `clear`는 같은 source IP/source port/session의 물체를 모두 지우며 `interactor_id=0`입니다. 서버는 오래되거나 중복된 sequence, 잘못된 CRC, 지원하지 않는 flag, non-finite 값을 거부합니다. position 각 축은 `[0,1]`, 모든 half extent는 양수여야 합니다. periodic 경계 안에서 모양이 모호해지지 않도록 box는 `length(half_extent) <= 0.45`, 나머지 shape은 `max(half_extent) <= 0.45`를 요구합니다. 선속도 크기는 최대 `2 normalized volume/s`, 각속도 각 성분은 `[-100,100] rad/s`이며 `length(angular_velocity) * shape_bounding_extent <= 2`도 만족해야 합니다. strength 범위는 `(0,4]`입니다. 송신자는 rotation quaternion을 정규화해야 하며 참조 도구가 이를 자동으로 처리합니다.

Z는 반복 경계가 아닙니다. 상·하단과 겹친 solid의 경계 속도는 유지되지만 volume 밖으로 향하는 scalar 목표는 버려질 수 있으므로, 안정적인 질량 밀어내기가 필요하면 proxy 중심을 최소 bounding extent만큼 상·하단 안쪽에 둡니다. 목표를 찾지 못한 수는 서버 로그의 `invalid_targets`로 확인할 수 있습니다.

## 네트워크 주의사항

control packet은 인증이나 암호화가 없는 UDP입니다. 기본 loopback bind를 유지하는 것이 가장 안전합니다. 다른 PC에서 Unreal을 실행해야 한다면 방화벽/VPN으로 송신자를 제한하고 control port를 공용 인터넷에 직접 노출하지 마십시오.

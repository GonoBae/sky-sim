# 구름 볼륨 프로토콜 v2 (`CLD2`)

`CLD2`는 서버의 한 시뮬레이션 프레임을 여러 3D 필드로 보낸다. UDP 데이터그램 하나는 64바이트 헤더와 최대 1,200바이트 payload로 구성되며, 정수와 IEEE-754 실수는 모두 little-endian이다. 기본 목적지는 `127.0.0.1:7777`, 송신률은 10Hz이다.

서버는 기본으로 다섯 필드를 모두 보낸다. `--fields`로 줄일 수 있지만 density는 반드시 포함해야 한다.

현재 Unreal 수신기도 선언된 필드 전체를 조립·검증한 뒤 density만 렌더러에 전달한다. velocity·temperature·vapor·occupancy는 아직 렌더링에 사용하지 않는다.

## 패킷 헤더

| Offset | Type | 이름 | 의미 |
|---:|---|---|---|
| 0 | `char[4]` | magic | `CLD2` |
| 4 | `uint16` | version | `2` |
| 6 | `uint16` | header_bytes | `64` |
| 8 | `uint32` | frame_id | 시뮬레이션 프레임 번호 |
| 12 | `uint32` | field_crc32 | 압축 해제한 필드 전체의 CRC32 |
| 16 | `float64` | simulation_time | solver 시간(초). UTC와 별개 |
| 24 | `uint16` | grid_x | 필드의 X 크기 |
| 26 | `uint16` | grid_y | 필드의 Y 크기 |
| 28 | `uint16` | grid_z | 필드의 Z 크기 |
| 30 | `uint8` | voxel_format | 아래 형식 표의 값 |
| 31 | `uint8` | field_id | 아래 필드 표의 ID |
| 32 | `uint8` | channel_count | voxel당 채널 수 |
| 33 | `uint8` | compression | `0`: none, `1`: RLE |
| 34 | `uint16` | chunk_index | 조각 번호, 0부터 시작 |
| 36 | `uint16` | chunk_count | 이 필드의 전체 조각 수 |
| 38 | `uint16` | payload_bytes | 이 조각의 payload 크기 |
| 40 | `uint16` | field_mask | 프레임에 포함된 필드 비트마스크 |
| 42 | `uint16` | flags | bit 0: keyframe, 상위 8비트: occupancy brick 폭 |
| 44 | `uint32` | payload_offset | 인코딩된 필드 안의 payload 위치 |
| 48 | `uint32` | encoded_field_bytes | 인코딩된 필드 전체 크기 |
| 52 | `uint32` | decoded_field_bytes | 압축 해제 후 필드 크기 |
| 56 | `float32` | value_scale | 값 복원 배율 |
| 60 | `float32` | value_bias | 값 복원 오프셋 |

필드 비트는 `1 << (field_id - 1)`이며 지원 마스크는 `0x001f`이다.

`flags`의 bit 1..7은 0이어야 한다. 상위 8비트는 occupancy에서만 `2`, `4`, `8` 중 하나를 쓰고 나머지 필드에서는 0이다. 현재 서버는 모든 필드를 keyframe으로 보낸다.

## 필드와 값 복원

| ID | 필드 | 형식 | 채널 | 값의 의미 |
|---:|---|---|---:|---|
| 1 | density | `UNorm16` | 1 | optical density로 변환하기 전의 구름 수분량 |
| 2 | velocity | `SNorm16` | 3 | X/Y/Z 속도, solver cell/s |
| 3 | temperature | `UNorm8` | 1 | 시뮬레이션 온도 변수, 범위 `[-1,3]` |
| 4 | vapor | `UNorm8` | 1 | 수증기량 |
| 5 | occupancy | `UNorm8` | 1 | density brick별 최댓값을 양자화한 값 |

이 형식·채널 조합은 고정이다. 아래 enum에 `SNorm8`이 있어도 현재 velocity 필드에 사용할 수는 없다. density·temperature·vapor는 시뮬레이션 변수이며 SI 단위로 해석하지 않는다.

| 값 | 형식 | 채널당 크기 | 정규화 값 `n` |
|---:|---|---:|---|
| 1 | `UNorm8` | 1바이트 | `encoded / 255` |
| 2 | `UNorm16` | 2바이트 | `encoded / 65535` |
| 3 | `SNorm8` | 1바이트 | `clamp(int8(encoded) / 127, -1, 1)` |
| 4 | `SNorm16` | 2바이트 | `clamp(int16(encoded) / 32767, -1, 1)` |

원래 값은 `n * value_scale + value_bias`로 복원한다. 배율은 필드·프레임마다 달라질 수 있으므로 항상 해당 헤더를 사용한다. 16비트 voxel 값도 little-endian이다.

density·velocity·temperature·vapor는 같은 격자를 쓴다. occupancy 크기는 각 축을 brick 폭으로 올림 나눈 값이다. 원본이 `64³`, brick 폭이 `4`이면 occupancy는 `16³`이다.

저장 순서는 X, Y, Z 순으로 X가 가장 빠르며, velocity 채널은 X/Y/Z 순서로 붙는다.

```text
voxel_index = (z * grid_y + y) * grid_x + x
byte_index = (voxel_index * channel_count + channel) * bytes_per_channel
```

## 압축과 분할

RLE은 필드 전체의 바이트열에 적용한 뒤 1,200바이트 단위로 나눈다.

- control `0..127`: 뒤의 `control + 1`바이트를 그대로 복사한다.
- control `128..255`: 다음 한 바이트를 `(control & 127) + 3`회 반복한다.

서버의 `--compression auto`는 RLE 결과가 원본보다 64바이트 넘게 작을 때만 압축한다. `none`은 압축하지 않고, `rle`은 크기가 늘어도 압축한다.

CRC는 압축 해제된 바이트열에 대한 IEEE CRC32이다(반사 다항식 `0xedb88320`, 초기값·최종 XOR `0xffffffff`, Python `zlib.crc32`와 동일).

## 수신 검증

버퍼를 할당하기 전에 헤더와 크기를 검사하고, 조립·압축 해제 단계에서 나머지를 확인한다.

1. magic·version·header 크기, 필드별 형식·채널 수, compression·flags가 위 정의와 일치해야 한다. 시간·scale·bias는 유한한 값이어야 한다.
2. 격자와 encoded 크기는 0보다 커야 하고, `decoded_field_bytes == x * y * z * channels * bytes_per_channel`이어야 한다. 곱셈 오버플로와 필드·전체 버퍼 상한을 검사한다.
3. `field_mask`는 0이 아니고 지원 비트만 사용하며, 현재 `field_id`의 비트를 포함해야 한다.
4. `chunk_count == ceil(encoded_field_bytes / 1200)`, `chunk_index < chunk_count`, `payload_offset == chunk_index * 1200`이어야 한다.
5. `payload_bytes == min(1200, encoded_field_bytes - payload_offset)`이고 데이터그램 길이는 `64 + payload_bytes`여야 한다. payload 범위가 필드 끝을 넘으면 거부한다.
6. 같은 프레임의 `field_mask`·`simulation_time`, 같은 필드의 크기·형식·채널·압축·조각 수·CRC·flags·scale·bias가 일치해야 한다. 도착 순서는 무관하며 같은 조각이 중복되면 내용도 같아야 한다.
7. macro 필드(ID 1..4)의 격자는 같아야 한다. occupancy는 macro 필드를 하나 이상 동반하고, 크기가 `ceil(macro_grid / brick_width)`와 일치해야 한다.
8. 압축 해제 결과의 크기와 CRC를 확인한다. `compression=0`이면 encoded·decoded 크기가 같아야 한다.
9. `field_mask`의 모든 필드가 완성된 뒤에만 프레임을 공개한다. 미완성 프레임은 시간·개수 상한으로 폐기하고, 늦게 온 과거 프레임이 최신 프레임을 덮지 않게 한다.

여러 송신자를 받는 수신기는 source IP·port도 조립 키에 포함해야 한다. 참조 도구는 이를 구분하지만 현재 Unreal 수신기는 `frame_id`만 키로 사용하므로 볼륨 포트당 송신 서버를 하나만 둔다.

참조 도구의 기본 상한은 필드당 64MiB, 전체 버퍼 128MiB, 미완성 프레임 8개, 마지막 수신 후 2초이다.

Unreal은 필드당 64MiB·미완성 8개·1초를 사용하며 전체 버퍼 합산 제한은 없다.

참조 도구로 첫 완성 프레임의 모든 필드와 메타데이터를 저장할 수 있다.

```bash
python3 tools/receive_probe.py --protocol 2 --output-dir capture
```

서버 송신은 non-blocking이다. 송신 버퍼 부족·would-block·interrupted send가 나면 해당 프레임의 나머지를 버리고 다음 프레임에서 재개한다.

로그의 `drops[frame=..., packet=...]`은 누적 실패 횟수이며, 영구 socket 오류는 오류 번호와 함께 보고한다. UDP 손실 중에는 마지막 완성 프레임을 계속 표시한다.

구현: [송신기](../src/protocol.hpp), [참조 수신기](../tools/receive_probe.py), [Unreal 수신기](../Unreal/uskysim/Source/uskysim/SkySimVolumeReceiver.cpp).

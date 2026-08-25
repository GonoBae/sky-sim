# Cloud UDP Protocol v2

프로토콜 v2는 한 시뮬레이션 프레임에 여러 3D 필드를 전송합니다. 모든 정수와 IEEE-754 실수는 명시적 little-endian이며, UDP datagram은 64바이트 헤더와 최대 1,200바이트 payload로 구성됩니다.

## Packet header

| Offset | Type | 이름 | 설명 |
|---:|---:|---|---|
| 0 | `char[4]` | magic | `CLD2` |
| 4 | `uint16` | version | `2` |
| 6 | `uint16` | header_bytes | `64` |
| 8 | `uint32` | frame_id | 시뮬레이션 프레임 번호 |
| 12 | `uint32` | field_crc32 | 압축 해제된 필드 전체의 CRC32 |
| 16 | `float64` | simulation_time | 시뮬레이션 시간(초) |
| 24 | `uint16` | grid_x | 현재 필드 X 크기 |
| 26 | `uint16` | grid_y | 현재 필드 Y 크기 |
| 28 | `uint16` | grid_z | 현재 필드 Z 크기 |
| 30 | `uint8` | voxel_format | 아래 voxel format 표 참고 |
| 31 | `uint8` | field_id | 아래 field 표 참고 |
| 32 | `uint8` | channel_count | voxel당 채널 수 |
| 33 | `uint8` | compression | `0`: none, `1`: RLE |
| 34 | `uint16` | chunk_index | 현재 조각 번호, 0부터 시작 |
| 36 | `uint16` | chunk_count | 현재 필드의 전체 조각 수 |
| 38 | `uint16` | payload_bytes | 현재 payload 크기 |
| 40 | `uint16` | field_mask | 이 frame이 포함하는 field bit mask |
| 42 | `uint16` | flags | bit 0: keyframe, 상위 8비트: occupancy brick 폭 |
| 44 | `uint32` | payload_offset | 압축된 필드 내 payload 위치 |
| 48 | `uint32` | encoded_field_bytes | 압축된 필드 전체 크기 |
| 52 | `uint32` | decoded_field_bytes | 압축 해제 후 필드 크기 |
| 56 | `float32` | value_scale | 물리값 복원 scale |
| 60 | `float32` | value_bias | 물리값 복원 bias |

각 field bit는 `1 << (field_id - 1)`입니다. 수신기는 같은 `frame_id`의 모든 패킷에서 `field_mask`와 `simulation_time`이 일치하는지 검사해야 합니다.

`flags`의 bit 1..7은 예약 영역이므로 0이어야 합니다. 상위 8비트의 brick 폭은
occupancy field에서만 사용하며 현재 값은 `2`, `4`, `8` 중 하나입니다. 다른 field는 상위
8비트를 0으로 보냅니다.

## Voxel formats

| 값 | 이름 | 저장 크기 | 정규화 |
|---:|---|---:|---|
| 1 | `UNorm8` | 채널당 1바이트 | `encoded / 255` |
| 2 | `UNorm16` | 채널당 2바이트 | `encoded / 65535` |
| 3 | `SNorm8` | 채널당 1바이트 | `int8(encoded) / 127`, `[-1,1]` clamp |
| 4 | `SNorm16` | 채널당 2바이트 | `int16(encoded) / 32767`, `[-1,1]` clamp |

정규화한 값을 `n`이라고 할 때 물리값은 다음과 같습니다.

```text
physical_value = n * value_scale + value_bias
```

16비트 voxel 값 자체도 little-endian입니다.

## Fields

| ID | 이름 | 기본 형식 | 채널 | 설명 |
|---:|---|---|---:|---|
| 1 | density | `UNorm16` | 1 | 비선형 optical density가 아닌 원본 cloud water density |
| 2 | velocity | `SNorm16` | 3 | 시뮬레이션 cell/second 단위 X/Y/Z 속도 |
| 3 | temperature | `UNorm8` | 1 | 기본 범위 `[-1,3]` |
| 4 | vapor | `UNorm8` | 1 | 원본 수증기량 |
| 5 | occupancy | `UNorm8` | 1 | 원본 density brick별 최댓값 |

occupancy field의 `grid_x/y/z`는 원본 격자를 brick 크기로 올림 나눈 값입니다. 예를 들어 원본 `64³`, brick 폭 `4`이면 occupancy는 `16³`입니다.

## Memory layout

X축이 가장 빠르고 그다음 Y, Z 순서입니다.

```text
voxel_index = (z * grid_y + y) * grid_x + x
byte_index = (voxel_index * channel_count + channel) * bytes_per_channel
```

Velocity 채널 순서는 X, Y, Z입니다.

## RLE codec

RLE은 decoded byte stream 전체에 적용한 뒤 UDP chunk로 나눕니다.

- control byte `0..127`: 뒤따르는 `control + 1`바이트를 literal로 복사
- control byte `128..255`: 다음 한 바이트를 `(control & 127) + 3`회 반복

`compression=auto`일 때 서버는 압축 결과가 원본보다 64바이트 넘게 작을 때만 RLE을
사용합니다. CRC32는 압축된 데이터가 아니라 압축 해제된 원본 필드에 대해 계산합니다.

## Receiver validation

수신기는 메모리를 할당하기 전에 적어도 다음을 검사해야 합니다.

1. magic, version, header size
2. 지원 가능한 격자와 최대 field byte 크기
3. `decoded_field_bytes == x * y * z * channels * bytes_per_channel`
4. `chunk_count == ceil(encoded_field_bytes / 1200)`
5. `chunk_index < chunk_count`
6. `payload_offset == chunk_index * 1200`
7. `payload_offset + payload_bytes <= encoded_field_bytes`
8. 같은 field의 모든 메타데이터 일치
9. RLE 출력 크기와 CRC32 일치
10. `field_mask`의 모든 필드가 완성됐을 때만 frame 공개
11. density/velocity/temperature/vapor의 격자 크기가 서로 일치
12. occupancy 크기가 `ceil(macro_grid / brick_width)`와 일치
13. occupancy를 포함한 frame에 적어도 하나의 macro field가 존재

서로 다른 송신자의 패킷이 섞이지 않도록 조립 키에는 source address와 port도 포함해야 합니다. 오래된 미완성 frame은 시간 기준으로 폐기하고, 렌더러에는 가장 최근의 완성 frame만 전달합니다.

UDP는 신뢰 전송이 아닙니다. 서버 송신 socket은 simulation tick을 막지 않도록 non-blocking이며, OS send buffer 부족·would-block·interrupted send가 나면 해당 frame의 나머지 chunk를 버리고 다음 frame에서 재개합니다. 서버 로그의 `drops[frame=..., packet=...]`은 누적 일시적 송신 실패 수입니다. 잘못된 목적지 권한이나 유효하지 않은 socket 같은 영구 오류는 숫자 socket error와 함께 서버 오류로 보고합니다. 수신기는 미완성 frame을 정상적인 손실로 취급하고 이전의 완성 frame을 계속 렌더링해야 합니다.

# 하늘·날씨 제어 프로토콜 (`SKC1`)

`SKC1`은 Unreal이나 운영 도구가 서버의 UTC, 위치, 시간 배율, 날씨와 구름층을 바꾸는
단방향 UDP command입니다. 서버 기본 bind 주소는 `127.0.0.1:7780`이며, 각 command는
정확히 512바이트인 datagram 하나입니다.

- byte order: little-endian
- CRC: offset `0..507`의 IEEE CRC32
- 응답: 별도 ACK packet은 없고, 다음 `SKS1`의 `last_control_session`,
  `last_control_sequence`, `last_control_result`가 마지막 적용 상태를 반영
- 기본 bind 변경: `--sky-control-host`, `--sky-control-port`
- 비활성화: `--no-sky-control`

## 빠른 사용

참조 송신 도구가 제공되는 경우 다음 형태로 preset과 시간을 제어합니다.

```bash
python3 tools/send_sky_control.py --transition 8 preset storm
python3 tools/send_sky_control.py time-scale 3600
python3 tools/send_sky_control.py keyframe
python3 tools/send_sky_control.py --transition 15 release
```

global option은 subcommand 앞에 둡니다. 전체 형식은 다음과 같습니다.

```text
python3 tools/send_sky_control.py \
  [--host IP] [--port N] [--source-port N] [--session N] [--sequence N] \
  [--transition SECONDS] [--seed N] COMMAND ...
```

지원 command는 `preset`, `time`, `location`, `time-scale`, `domain`, `wind`, `weather`,
`cloud-layer`, `release`, `keyframe`입니다. 도구의 정확한 값 순서와 weather group은
`python3 tools/send_sky_control.py --help`로 확인합니다. 참조 도구는 단발 제어 시험용이고,
실제 Unreal 컴포넌트는 하나의 UDP socket/source port와 nonzero session ID를 실행 중
유지하며 sequence를 계속 증가시킵니다.

```bash
python3 tools/send_sky_control.py time 2026-08-25T03:00:00Z
python3 tools/send_sky_control.py location 37.5665 126.9780 38
python3 tools/send_sky_control.py wind 8 2 0 --gust 13
python3 tools/send_sky_control.py weather \
  --temperature-c 18 --pressure-hpa 1004 --humidity 0.92 \
  --precipitation-mm-h 12 --snow-fraction 0 --surface-wetness 0.5
python3 tools/send_sky_control.py --transition 5 cloud-layer \
  0 convective 1200 5000 0.65 --optical-depth 35 --convection 0.6
```

`weather`는 누락된 값이 이전 상태의 0으로 덮이지 않도록 다음 group별로 원자적입니다.

| Group | 함께 제공해야 하는 옵션 |
|---|---|
| thermodynamics | `--temperature-c`, `--pressure-hpa`, `--humidity` |
| visibility | `--visibility-km`, `--aerosol`, `--ozone-du` |
| precipitation | `--precipitation-mm-h`, `--snow-fraction`, `--surface-wetness` |
| convection | `--convection`, `--lightning` |

참조 도구를 여러 호출에 걸쳐 같은 session으로 시험할 때는 세 값 모두 고정/증가시킵니다.

```bash
python3 tools/send_sky_control.py --source-port 17780 \
  --session 100 --sequence 1 preset clear
python3 tools/send_sky_control.py --source-port 17780 \
  --session 100 --sequence 2 --transition 10 preset storm
```

## 64바이트 command header

| Offset | Type | 이름 | 설명 |
|---:|---:|---|---|
| 0 | `char[4]` | magic | ASCII `SKC1` |
| 4 | `uint16` | version | `1` |
| 6 | `uint16` | packet_bytes | `512` |
| 8 | `uint32` | session_id | 클라이언트 실행마다 새 nonzero 값 |
| 12 | `uint32` | sequence | session에서 단조 증가, uint32 wrap 허용 |
| 16 | `float64` | client_time_seconds | finite이고 0 이상인 클라이언트 시간 |
| 24 | `uint8` | opcode | 1 patch, 2 release, 3 preset, 4 keyframe |
| 25 | `uint8` | evolution_mode | 1 natural, 2 timeline, 3 manual, 4 replay |
| 26 | `uint16` | reserved | 반드시 0 |
| 28 | `uint32` | transition_milliseconds | `0..86,400,000` ms |
| 32 | `uint32` | hold_milliseconds | 예약, 현재 반드시 0 |
| 36 | `uint32` | preset | 0 natural, 1 clear, 2 cumulus, 3 overcast, 4 rain, 5 storm, 6 snow, 7 fog |
| 40 | `uint64` | apply_mask | patch에서 적용할 field group |
| 48 | `uint64` | clear_mask | release에서 해제할 field group |
| 56 | `uint32` | weather_seed | natural weather와 lightning seed |
| 60 | `uint16` | requested_cloud_field_mask | 예약, 현재 반드시 0 |
| 62 | `uint16` | requested_state_hz | 예약, 현재 반드시 0 |

## Opcode

| 값 | 이름 | header 조건 | 현재 동작 |
|---:|---|---|---|
| 1 | `PatchOverride` | `apply_mask != 0`, `clear_mask == 0` | 선택한 시간/위치/기상 group을 적용. 기상 group이 있으면 custom weather로 부드럽게 전환 |
| 2 | `ReleaseOverride` | `apply_mask == 0`, `clear_mask == 0x7be0` | 현재 weather override 전체를 natural weather로 전환 |
| 3 | `LoadPreset` | apply/clear mask 0, preset 0..7 | preset 또는 natural weather로 전환하고 seed 적용 |
| 4 | `RequestKeyframe` | apply/clear mask 0, transition 0, preset natural | 다음 주기까지 기다리지 않고 `SKS1` 즉시 전송 요청 |

현재 `ReleaseOverride`는 일부 group 해제를 지원하지 않고 weather override 전체를 natural
상태로 되돌립니다. 따라서 `clear_mask`에는 thermodynamics, visibility, wind,
precipitation, convection, cloud layer 0..3을 모두 합친 `0x7be0`이 반드시 들어가야 합니다.

`timeline`과 `replay` evolution 값은 protocol 확장용입니다. 이 버전에서 실제 자동 진화는
`natural` preset이며, 파일 기반 timeline/replay scheduler는 아직 없습니다.
일반 UTC/location/domain/time-scale patch와 keyframe 요청은 현재 evolution mode를 바꾸지
않습니다. bit 4를 적용할 때만 `natural` 또는 `manual`을 받을 수 있습니다. `natural`은
현재 weather override를 해제하며 manual weather group과 같은 patch에 넣을 수 없고,
`manual`은 현재 상태를 고정한 뒤 선택한 weather group을 적용합니다.

## Apply/clear mask

| Bit | Hex | 이름 | data 영역 |
|---:|---:|---|---|
| 0 | `0x000001` | UTC | 64..71 |
| 1 | `0x000002` | location | 72..91 |
| 2 | `0x000004` | domain | 96..107 |
| 3 | `0x000008` | time scale | 92..95 |
| 4 | `0x000010` | evolution/seed | header seed |
| 5 | `0x000020` | thermodynamics | 108..119 |
| 6 | `0x000040` | visibility | 120..131 |
| 7 | `0x000080` | wind | 132..155 |
| 8 | `0x000100` | precipitation | 160..171, 176..179 |
| 9 | `0x000200` | convection/lightning | 184..187, 192..195 |
| 11 | `0x000800` | cloud layer 0 | 224..253 |
| 12 | `0x001000` | cloud layer 1 | 256..285 |
| 13 | `0x002000` | cloud layer 2 | 288..317 |
| 14 | `0x004000` | cloud layer 3 | 320..349 |
| 19 | `0x080000` | atmosphere optics | wire 공간은 있으나 **현재 수신 지원 mask에 포함되지 않음** |

bit 10, 15..18, 20..63은 예약입니다. `PatchOverride`에 지원하지 않는 bit를 넣으면 packet
전체가 거부됩니다. `ReleaseOverride`는 thermodynamics, visibility, wind,
precipitation, convection, cloud layer 0..3을 모두 합친 mask만 받습니다.

## 조건부 data 영역

offset `64..503`은 `SKS1`과 같은 위치에 대응하지만, `apply_mask`로 선택한 byte만 채우고
나머지는 반드시 0으로 보내야 합니다. 서버는 사용하지 않는 byte가 하나라도 0이 아니면
packet을 거부합니다.

| Offset | Type | 해당 mask | 입력 단위와 허용 범위 |
|---:|---:|---|---|
| 64 | `float64` | UTC | Unix seconds, 1900..2100 |
| 72 | `float64` | location | latitude `[-90,90] deg` |
| 80 | `float64` | location | longitude `[-180,180] deg` |
| 88 | `float32` | location | elevation AMSL `[-500,100000] m` |
| 92 | `float32` | time scale | `[-86400,86400]` UTC seconds/wall second |
| 96,100 | `float32` | domain | 동일한 horizontal extent, `100..2,000,000 m` |
| 104 | `float32` | domain | vertical extent, `100..100,000 m` |
| 108 | `float32` | thermodynamics | temperature `203.15..333.15 K` |
| 112 | `float32` | thermodynamics | pressure `80,000..108,000 Pa` |
| 116 | `float32` | thermodynamics | relative humidity `0.01..1` |
| 120 | `float32` | visibility | visibility `25..200,000 m` |
| 124 | `float32` | visibility | AOD 550 nm `0.005..3` |
| 128 | `float32` | visibility | ozone `100..600 DU` |
| 132 | `float32[3]` | wind | mean ENU wind, vector length 최대 `150 m/s` |
| 144 | `float32[3]` | wind | mean 방향의 gust delta, 합산 gust 최대 `200 m/s` |
| 160 | `float32` | precipitation | 수분 등가 flux `kg/(m2 s)`, 수치상 `mm/h / 3600`; 내부 허용량 `0..300 mm/h` |
| 164 | `float32` | precipitation | rain fraction, 정확히 `1 - snow_fraction` |
| 168 | `float32` | precipitation | snow fraction `0..1` |
| 176 | `float32` | precipitation | surface wetness `0..1` |
| 184 | `float32` | convection | convective index `0..3000` (`activity * 3000`) |
| 192 | `float32` | convection | lightning index `0..0.25` (`activity * 0.25`) |
| 504 | `uint32` | 항상 | reserved, 반드시 0 |
| 508 | `uint32` | 항상 | CRC32 |

wind의 offset 144 vector는 평균 바람 방향으로 gust speed와 mean speed의 차이를 넣습니다.
평균 풍속이 0인데 돌풍만 있으면 결정적인 ENU east 방향에 delta를 넣어 크기를 보존합니다.
UTC가 지원 범위의 양 끝에 도달하면 서버는 해당 경계에서 멈추고 `time_scale`을 자동으로
0으로 바꿉니다.

### Cloud layer data

```text
layer_offset = 224 + layer_index * 32
```

| Relative offset | Type | 입력 |
|---:|---:|---|
| +0 | `float32` | base altitude AMSL, `>= -500 m` |
| +4 | `float32` | top altitude AMSL, base보다 크고 `<=100,000 m` |
| +8 | `float32` | coverage `0..1` |
| +12 | `float32` | condensate `kg/m3`; 복원 optical depth가 `0..min(500, thickness_m)`이어야 함 |
| +16 | `float32` | ice fraction `0..1` |
| +20 | `float32` | 수분 등가 precipitation flux `kg/(m2 s)`, 내부 `0..300 mm/h` |
| +24 | `float32` | turbulence `m/s`; 복원 convective activity `0..1` |
| +28 | `uint8` | kind 1..5 |
| +29 | `uint8` | layer flags |
| +30 | `uint16` | 반드시 0 |

layer 고도는 `SKC1`에서 AMSL로 보냅니다. 서버는 현재 지표 elevation을 빼서 내부 AGL
높이로 바꿉니다. 따라서 location과 layer를 같은 patch에서 바꿀 때도 layer에는 새 위치의
AMSL 고도를 넣어야 합니다. fog와 stratiform은 wire kind 1을 공유합니다. AGL base가
약 `5 m` 이하이고 top이 `1,000 m` 이하인 얕은 지표층은 서버 적용 단계에서 fog로
분류합니다. 이 버전에는 layer 하나만 삭제하는 opcode가 없습니다. coverage 0은 렌더
피복과 해당 layer의 유체 forcing을 0으로 만들지만 layer metadata와 연속 index slot은
남깁니다. 습도·바람·대류처럼 layer와 별개인 전역 weather forcing도 그대로 적용됩니다.
layer 목록 자체를 완전히 정리하려면 preset 또는 `release`를 사용합니다.
layer 0..3은 현재 서버의 연속된 layer 목록 index입니다. `index < count`는 해당 layer를
교체하고 `index == count`는 맨 뒤에 추가합니다. 중간을 건너뛴 `index > count` 명령은
상태를 바꾸지 않고 `SKS1.last_control_result = 2`로 거부합니다. 이 버전에는 영구 layer
ID가 없으므로 적용 후에는 반환된 `SKS1` layer 배열을 서버 권위 상태로 취급해야 합니다.
layer top이 지표 elevation 이하이거나, 지표와 겹치는 부분의 두께가 1 m 미만이어도 같은
결과 코드로 거부합니다. base만 지표 아래이고 top은 충분히 위라면 base를 지표면으로
잘라 AGL 0 m부터 적용하고, 잘려나간 두께 비율만큼 optical depth도 줄여 wire 상태와 유체
forcing의 광학량을 일치시킵니다. 시뮬레이션 domain 위로 걸친 layer도 유체 forcing에서는
domain 안에 남은 두께 비율로 optical depth를 줄입니다.

offset 196..219와 432..479에는 atmosphere-optics wire 공간이 있지만, bit 19는 현재
supported apply mask에서 제외되어 있으므로 `SKC1`로 직접 덮어쓸 수 없습니다. 대기
계수는 temperature, pressure, humidity, visibility/AOD, ozone과 snow 상태에서 서버가
다시 계산해 `SKS1`으로 보냅니다.

## 제어 적용 확인

`SKC1`은 UDP이므로 송신 성공이 서버 적용을 뜻하지 않습니다. Unreal은 다음 `SKS1`에서
아래 세 값을 확인합니다.

```text
last_control_session  == sent.session_id
last_control_sequence == sent.sequence
last_control_result   == 1
```

일정 시간 안에 확인되지 않으면 **같은 datagram/sequence**를 약 2 Hz로 제한 횟수
재전송합니다. 첫 packet이 유실됐으면 서버가 재전송본을 적용하고, 이미 적용됐다면 같은
sequence는 stale로 버려져 전이가 다시 시작되지 않습니다. 새 사용자 command를 만들 때만
sequence를 증가시킵니다. 불안정한 네트워크에서는 마지막 서버 snapshot을 기준으로 UI를
표시합니다.

## Session, replay 방어와 큐

서버는 `(source IPv4, source UDP port, session_id)`별 마지막 sequence를 기억합니다.

- 같은 sequence와 오래된 sequence는 stale packet으로 버립니다.
- uint32 wrap은 half-range 비교로 허용합니다.
- 최대 32개 session을 추적합니다.
- 10분 동안 보이지 않은 session은 만료합니다.
- 적용 대기 큐는 최대 256개이며 넘치면 가장 오래된 command를 버립니다.
- 한 poll에서 최대 128개를 읽습니다.

프로세스를 매 command마다 새로 실행하면 source port가 달라져 별도 session으로 인식될 수
있습니다. 운영 UI와 Unreal 플러그인은 socket과 source port를 유지하십시오.

## 보안

`SKC1`에는 인증, 권한, 암호화가 없습니다. CRC32는 전송 오류 검사용이며 공격자 방어가
아닙니다. 기본 `127.0.0.1` bind를 유지하는 것이 안전합니다. 다른 PC에서 제어해야 하면
`--sky-control-host`를 LAN 주소로 바꾸기 전에 다음을 적용합니다.

- 방화벽에서 Unreal/운영 장비의 source IP만 허용
- 공용 인터넷 대신 VPN 또는 인증된 tunnel 사용
- control port `7780`을 인터넷에 직접 노출하지 않음
- 서버 로그의 rejected/stale control counter 감시

## Unreal 최소 제어 UI

레벨별 Blueprint에 많은 weather parameter를 흩어놓지 말고, 하나의 `SkySimSystem`이 다음
항목만 노출하는 구성이 권장됩니다.

- preset + transition seconds
- UTC/일시 + time scale
- latitude/longitude/elevation
- advanced override: wind, humidity, visibility, precipitation, cloud layer

UI는 이 값을 `SKC1` command로 만들고, 화면에 표시할 실제 값은 로컬 입력이 아니라 다시
수신한 `SKS1`을 사용합니다. 이렇게 해야 서버의 clamp, transition과 자연 날씨 결과가
Unreal 화면 및 운영 UI에서 일치합니다.

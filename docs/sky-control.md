# 하늘·날씨 제어 프로토콜 (`SKC1`)

`SKC1`은 서버의 UTC, 위치, 시간 배율, 날씨와 구름층을 바꾸는 UDP 제어 프로토콜이다.
명령 하나를 512바이트 패킷으로 보낸다.

- 기본 수신 주소: `127.0.0.1:7780` (`--sky-control-host`, `--sky-control-port`)
- 바이트 순서: 모든 정수와 IEEE-754 실수는 little-endian
- CRC: offset `0..507`의 IEEE CRC32를 offset `508`에 기록
- 응답: 별도 ACK 패킷 없이 [`SKS1`](sky-state-protocol.md)의 `last_control_*`로 확인
- 비활성화: `--no-sky-control`

## 빠른 사용

서버 실행 후 저장소 루트에서 호출한다. 공통 옵션은 하위 명령 앞에 둔다.

```powershell
python tools/send_sky_control.py --transition 8 preset storm
python tools/send_sky_control.py time-scale 3600
python tools/send_sky_control.py time-scale 0
python tools/send_sky_control.py --transition 15 release
python tools/send_sky_control.py keyframe
```

`time-scale 0`은 UTC와 천체 위치, UTC 기반 자연 날씨 목표를 멈춘다. 날씨 전환·수렴,
지면 젖음·번개와 `CLD2` 유체 계산은 계속된다. 음수 배율은 UTC 역행을 지원한다.

```powershell
python tools/send_sky_control.py time 2026-08-25T03:00:00Z
python tools/send_sky_control.py location 37.5665 126.9780 38
python tools/send_sky_control.py domain 20000 14000
python tools/send_sky_control.py wind 8 2 0 --gust 13
python tools/send_sky_control.py weather `
  --temperature-c 18 --pressure-hpa 1004 --humidity 0.92 `
  --precipitation-mm-h 12 --snow-fraction 0 --surface-wetness 0.5
python tools/send_sky_control.py --transition 5 cloud-layer `
  0 convective 1200 5000 0.65 --optical-depth 35 --convection 0.6
```

위 구름층 고도는 AMSL이다. `weather`는 아래 그룹에서 하나를 지정하면 같은 그룹의 나머지
옵션도 모두 요구한다. 생략한 그룹은 서버의 현재 값을 유지한다.

| Group | 함께 제공해야 하는 옵션 |
|---|---|
| thermodynamics | `--temperature-c`, `--pressure-hpa`, `--humidity` |
| visibility | `--visibility-km`, `--aerosol`, `--ozone-du` |
| precipitation | `--precipitation-mm-h`, `--snow-fraction`, `--surface-wetness` |
| convection | `--convection`, `--lightning` |

주소·포트·seed 등 전체 옵션은 `python tools/send_sky_control.py --help`로 확인한다.
도구는 한 번 보내고 종료하며 ACK를 기다리지 않는다. 같은 세션으로 여러 번 시험하려면
송신 포트와 session을 고정하고 새 명령의 sequence만 증가시킨다.

```powershell
python tools/send_sky_control.py --source-port 17780 `
  --session 100 --sequence 1 preset clear
python tools/send_sky_control.py --source-port 17780 `
  --session 100 --sequence 2 --transition 10 preset storm
```

## 명령 헤더 (64바이트)

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
| 28 | `uint32` | transition_milliseconds | 날씨 전환 시간, 실제 시간 기준 `0..86,400,000 ms` |
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

UTC·위치·영역·시간 배율은 즉시 적용한다. `transition_milliseconds`는 날씨 전환에 사용한다.
기상 그룹을 수정하면 선택하지 않은 그룹은 현재 값으로 유지한 채 `manual`로 전환한다.
`release`는 모든 날씨 덮어쓰기를 해제하며 일부 그룹만 해제할 수 없다.

시간·위치·영역·배율 수정과 keyframe은 evolution mode를 바꾸지 않는다. bit 4를 지정하면
seed를 적용하고 `natural`은 날씨 덮어쓰기를 해제한다. `manual`은 현재 날씨를 고정하고
선택한 기상 그룹을 적용한다. `natural`과 기상 그룹을 한 patch에 함께 지정할 수 없다.
`timeline`과 `replay`는 확장용이며 bit 4의 입력으로 받지 않는다.

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
| 19 | `0x080000` | atmosphere optics | 현재 미지원 |

지원 apply mask는 `0x7bff`다. bit 10, 15..18, 20..63은 예약이며 미지원 bit가 있으면
패킷 전체를 거부한다. release의 `clear_mask`는 기상 그룹과 구름층 0..3을 합친 `0x7be0`이다.

## 조건부 데이터 영역

offset `64..503`은 `SKS1`과 같은 필드 위치를 사용한다. `apply_mask`가 선택한 바이트만
채우고 나머지는 0으로 보낸다. 모든 입력 실수는 유한해야 한다.

| Offset | Type | 해당 mask | 입력 단위와 허용 범위 |
|---:|---:|---|---|
| 64 | `float64` | UTC | Unix seconds, `-2208988800..4133980800` (1900-01-01..2101-01-01) |
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
| 164 | `float32` | precipitation | rain fraction `0..1`, `1 - snow_fraction` (허용 오차 `0.0001`) |
| 168 | `float32` | precipitation | snow fraction `0..1` |
| 176 | `float32` | precipitation | surface wetness `0..1` |
| 184 | `float32` | convection | convective index `0..3000` (`activity * 3000`) |
| 192 | `float32` | convection | lightning index `0..0.25` (`activity * 0.25`) |
| 504 | `uint32` | 항상 | reserved, 반드시 0 |
| 508 | `uint32` | 항상 | CRC32 |

offset 144에는 평균 바람 방향으로 `gust speed - mean speed`를 넣는다. 평균 풍속이
`0.0001 m/s` 이하이면 ENU east 방향을 사용한다. 서버는 두 벡터 길이의 합으로 돌풍
속도를 복원한다. UTC가 지원 범위 끝에 닿으면 경계값에 고정하고 `time_scale`을 0으로 바꾼다.

### 구름층 데이터

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
| +24 | `float32` | turbulence `0.25..4.25 m/s`; convective activity = `(turbulence - 0.25) / 4` |
| +28 | `uint8` | kind 1..5 |
| +29 | `uint8` | bit 0 대류, bit 1 강수, bit 2 번개 환경. bit 3..7은 0 |
| +30 | `uint16` | 반드시 0 |

kind 값은 [`SKS1` 구름층 표](sky-state-protocol.md#구름층-4개)와 같다. fog와 stratiform은
kind 1을 공유한다. 서버 적용 시 지표 부근의 stratiform(AGL base `<=5 m`, top `<=1000 m`)을
fog로 분류한다. 광학 깊이는 `condensate * 100 * max(1, thickness_m)`로 복원한다.
layer flags는 알려진 bit인지 검사하지만 기상값을 덮어쓰는 입력으로 쓰지 않는다.

층 적용 규칙은 다음과 같다.

- 고도는 AMSL로 보낸다. 위치를 함께 수정하면 새 위치의 지표 고도를 빼서 AGL로 변환한다.
- `index < count`는 교체, `index == count`는 추가다. 여러 층을 보내면 작은 index부터 처리한다.
  `index > count`로 중간을 건너뛰면 patch 전체를 거부한다(`last_control_result = 2`).
- 지표 위에 남은 두께가 1 m 미만이어도 전체를 거부한다. 지표 아래 base는 지표면으로 자르고,
  남은 두께 비율로 광학 깊이를 줄인다. 영역 상단을 넘은 부분도 유체 계산에서는 같은 비율로 줄인다.
- 층 하나를 삭제하는 명령은 없다. coverage 0은 해당 층의 피복과 유체 작용을 없애지만
  층 정보·index와 전역 습도·바람·대류는 남는다. 층 목록을 다시 설정하려면 preset 또는 release를 쓴다.
- 영구 층 ID는 없다. 적용 후에는 반환된 `SKS1` 배열을 기준으로 다음 명령을 만든다.

대기 광학 계수(offset 196..219, 432..479)는 직접 수정할 수 없다. 서버가 기상값에서
계산해 `SKS1`으로 보낸다.

## 제어 적용 확인

송신 성공만으로 적용을 확인할 수 없다. `SKS1`의 session·sequence가 보낸 명령과 같고
result가 1이면 적용된 것이다. 날씨 전환 완료를 뜻하지는 않는다.

```text
last_control_session  == sent.session_id
last_control_sequence == sent.sequence
last_control_result   == 1
```

result 2는 층 index·지표 고도 등 현재 상태와 맞지 않아 적용 단계에서 거부된 명령이다.
길이, magic/version, CRC, 예약 바이트, enum, mask 또는 값 범위 검증에 실패한 패킷은
ACK를 갱신하지 않고 버린다. 선택하지 않은 데이터 바이트도 모두 0이어야 한다.

클라이언트는 명령 하나의 ACK를 확인한 뒤 다음 명령을 보낸다. ACK에는 마지막으로 처리한
명령 하나만 남으므로 여러 명령을 연달아 보내면 중간 결과를 놓칠 수 있다. 응답이 없으면
같은 패킷·sequence를 약 2 Hz로 제한 횟수 재전송한다. 이미 받은 sequence는 버리므로
전환이 다시 시작되지 않는다. 새 명령을 만들 때만 sequence를 증가시킨다.

## 세션과 수신 큐

서버는 `(송신 IPv4, 송신 UDP 포트, session_id)`별 마지막 sequence를 기억한다.

- `d = uint32(sequence - previous)`가 `0 < d < 0x80000000`일 때만 새 명령이다.
- 세션은 최대 32개다. 꽉 차면 새 세션을 거부하며, 새 sequence를 10분간 받지 않은 세션은 만료한다.
- 대기 큐는 최대 256개이며 넘치면 가장 오래된 명령을 버린다. 한 poll에서 최대 128개를 읽는다.

지속 실행하는 클라이언트는 소켓·송신 포트·0이 아닌 session ID를 유지한다.

## 보안

인증·암호화는 없으며 CRC32는 전송 오류만 검사한다. 기본 수신 주소는 로컬이다.
외부 제어가 필요하면 방화벽에서 송신 IP를 제한하고 VPN 또는 인증된 터널을 사용한다.
제어 포트를 인터넷에 직접 노출하지 않는다.

Unreal의 조작 방법과 연결 시 설정 동기화는 [에디터 가이드](unreal-editor-guide.md)를 참고한다.

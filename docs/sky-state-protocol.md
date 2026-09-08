# 하늘 상태 프로토콜 (`SKS1`)

`SKS1`은 서버가 Unreal에 보내는 하늘·날씨 스냅샷이다. UTC, 위치, 태양·달, 대기,
바람, 강수와 구름층 정보를 UDP 패킷 하나에 담는다. 구름 복셀은 별도
[`CLD2`](protocol-v2.md), 제어 명령은 [`SKC1`](sky-control.md)을 사용한다.

- 기본 목적지: `127.0.0.1:7779`
- 기본 전송률: `5 Hz` (`--sky-hz`로 `1..simulation_hz` 범위에서 변경)
- 패킷 크기: `512 bytes`
- 바이트 순서: 모든 정수와 IEEE-754 실수는 little-endian
- 좌표계: 오른손 로컬 ENU, `X=east`, `Y=north`, `Z=up`
- CRC: offset `0..507`의 IEEE CRC32를 offset `508`에 기록

`volume_frame_id`는 직전에 보낸 볼륨 프레임을 가리키는 동기화 힌트다. 두 스트림은
독립적으로 도착하므로 최신 유효 상태를 각각 보관한다. 패킷을 잃으면 직전 상태를 유지한다.
Unreal의 조명·Weather Fog 적용은 [렌더링 가이드](unreal-rendering.md)를 참고한다.

## 공통 헤더

| Offset | Type | 이름 | 단위/의미 |
|---:|---:|---|---|
| 0 | `char[4]` | magic | ASCII `SKS1` |
| 4 | `uint16` | version | `1` |
| 6 | `uint16` | packet_bytes | `512` |
| 8 | `uint32` | state_sequence | `SKS1` datagram을 보낼 때마다 1씩 증가하는 sequence |
| 12 | `uint32` | last_control_sequence | 마지막으로 처리한 `SKC1.sequence`, 아직 없으면 0 |
| 16 | `uint32` | last_control_session | 마지막으로 처리한 `SKC1.session_id`, 아직 없으면 0 |
| 20 | `uint32` | volume_frame_id | 직전에 보낸 `CLD2` frame ID |
| 24 | `float64` | fluid_time_seconds | 서버 시작 후 유체 시뮬레이션 시간, 초 |
| 32 | `float64` | predicted_valid_time_seconds | 이 상태의 예측 유효 유체 시간, 초 |
| 40 | `uint32` | flags | 아래 상태 flag |
| 44 | `uint32` | weather_seed | 자연 날씨 결정 seed |
| 48 | `uint8` | evolution_mode | 1 natural, 2 timeline, 3 manual, 4 replay |
| 49 | `uint8` | last_control_result | 0 제어 없음, 1 적용됨, 2 현재 상태와 맞지 않아 거부됨 |
| 50 | `uint8` | cloud_layer_count | 유효 구름층 수, `0..4` |
| 51 | `uint8` | reserved | 반드시 0 |
| 52 | `uint32` | weather_model_revision | 현재 `1` |
| 56 | `uint32` | lightning_event_id | 새 번개가 발생할 때 증가 |
| 60 | `uint16` | supported_cloud_field_mask | 서버가 지원하는 `CLD2` field bit mask, 현재 `0x001f` |
| 62 | `uint16` | active_cloud_field_mask | 현재 `CLD2` 설정에서 보내는 field bit mask |

`timeline`과 `replay`는 확장용 값이다. 현재 서버는 `natural`과 프리셋·사용자 설정 기반
`manual` 모드로 날씨를 생성한다.

### 상태 flags

| Bit | 이름 | 의미 |
|---:|---|---|
| 0 | sun above horizon | 태양의 기하 고도가 `-0.833 deg`보다 높음 |
| 1 | moon above horizon | 달의 기하 고도가 `0 deg`보다 높음 |
| 2 | precipitation | 강수량이 `0.001 mm/h`보다 큼 |
| 3 | fog | 상대습도와 가시거리로 계산한 안개 extinction이 활성 |
| 4 | lightning flash | 약 `0.35..0.50 s`의 현재 번개 flash 구간 |
| 5 | snow | 강수가 있고 snow fraction이 `0.15`보다 큼 |
| 6..31 | reserved | 현재 0 |

## 시간·위치·기상 상태

| Offset | Type | 이름 | 단위/의미 |
|---:|---:|---|---|
| 64 | `float64` | utc_unix_seconds | UTC Unix timestamp, `-2208988800..4133980800` (1900-01-01..2101-01-01) |
| 72 | `float64` | latitude_degrees | 위도, `[-90,90] deg` |
| 80 | `float64` | longitude_degrees | 경도, `[-180,180] deg`, east positive |
| 88 | `float32` | elevation_amsl_m | volume 바닥의 평균 해수면 기준 고도, m |
| 92 | `float32` | time_scale | 실제 1초당 UTC 진행 초, `-86400..86400`. 0은 UTC 정지, 음수는 역행 |
| 96 | `float32` | domain_extent_east_m | simulation domain 동서 폭, m |
| 100 | `float32` | domain_extent_north_m | 남북 폭, m. 현재 east 폭과 같음 |
| 104 | `float32` | domain_extent_up_m | 수직 높이, m |
| 108 | `float32` | surface_temperature_k | 지표 온도, K |
| 112 | `float32` | sea_level_pressure_pa | 해면 기압, Pa |
| 116 | `float32` | relative_humidity | 상대습도, `0..1` |
| 120 | `float32` | visibility_m | 기상 시정, m |
| 124 | `float32` | aerosol_optical_depth_550nm | 550 nm aerosol optical depth |
| 128 | `float32` | ozone_dobson_units | 전 오존량, DU |
| 132 | `float32[3]` | mean_wind_enu_m_s | 평균 바람 east/north/up, m/s |
| 144 | `float32[3]` | gust_delta_enu_m_s | 평균 바람에 더할 돌풍 delta vector, m/s |
| 156 | `float32` | boundary_layer_height_m | 대류 활동도에서 만든 유효 경계층 높이, m |
| 160 | `float32` | precipitation_flux_kg_m2_s | 수분 등가 강수 flux, kg/(m2 s). 수치상 `mm/h / 3600` |
| 164 | `float32` | rain_fraction | 강수 중 비 비율, `0..1` |
| 168 | `float32` | snow_fraction | 강수 중 눈 비율, `0..1` |
| 172 | `float32` | hail_fraction | 예약, 현재 0 |
| 176 | `float32` | surface_wetness | 누적 지면 젖음, `0..1` |
| 180 | `float32` | snow_water_equivalent_kg_m2 | 예약, 현재 0 kg/m2 |
| 184 | `float32` | cape_j_kg | CAPE 형태의 대류 강도 지표, 현재 `0..3000 J/kg` |
| 188 | `float32` | cin_j_kg | CIN 형태의 억제 강도 지표, 현재 `0..120 J/kg` |
| 192 | `float32` | lightning_rate_hz | 번개 활동도에서 만든 빈도, 현재 `0..0.25 Hz` |

offset 184, 188, 192는 대류·번개 활동도에서 만든 렌더링용 지표이며 관측값이 아니다.
강수량이 0이면 rain/snow/hail fraction도 모두 0이다.

돌풍 속도는 평균 풍속과 `gust_delta_enu_m_s`의 크기를 더한 값이다. delta는 평균 바람
방향이며 평균 풍속이 `0.0001 m/s` 이하이면 ENU east 방향을 사용한다.

## 지면과 대기

| Offset | Type | 이름 | 단위/의미 |
|---:|---:|---|---|
| 196 | `float32[3]` | ground_albedo_rgb | 선형 RGB 지면 albedo |
| 208 | `float32` | mie_anisotropy_g | Henyey-Greenstein `g` |
| 212 | `float32` | mie_scale_height_m | Mie 밀도 scale height, m |
| 216 | `float32` | rayleigh_scale_height_m | Rayleigh 밀도 scale height, m |
| 220 | `float32` | temperature_lapse_k_m | 현재 `-0.0065 K/m` |

행성 반지름은 전송하지 않는다. 서버 기준값은 지표 `6,371,000 m`, 대기 상단 `6,471,000 m`다.
offset 432..479의 계수는 해면기압 기준이다. 관측자 반지름은
`지표 반지름 + elevation_amsl_m`이며 고도에 따른 감쇠에는 scale height를 사용한다. 태양 직달 일사·조도에는
이미 관측 고도의 기압과 남은 에어로졸 기둥이 반영되어 있다.

## 구름층 4개

offset `224`부터 구름층이 32바이트씩 이어진다.

```text
layer_offset = 224 + layer_index * 32   # layer_index 0..3
```

| Relative offset | Type | 이름 | 단위/의미 |
|---:|---:|---|---|
| +0 | `float32` | base_altitude_amsl_m | 구름 하단 AMSL, m |
| +4 | `float32` | top_altitude_amsl_m | 구름 상단 AMSL, m |
| +8 | `float32` | coverage | 수평 피복률, `0..1` |
| +12 | `float32` | condensate_kg_m3 | 유효 응결수 `optical_depth / (100 * max(1, thickness_m))`, `0..0.01 kg/m3`로 제한 |
| +16 | `float32` | ice_fraction | 얼음상 비율, `0..1` |
| +20 | `float32` | precipitation_flux_kg_m2_s | 해당 층의 수분 등가 강수 flux, kg/(m2 s) |
| +24 | `float32` | turbulence_m_s | 현재 `0.25 + 4 * convective_activity`, m/s |
| +28 | `uint8` | cloud_kind | 아래 wire kind |
| +29 | `uint8` | layer_flags | bit 0 convective, bit 1 precipitation, bit 2 lightning environment |
| +30 | `uint16` | reserved | 반드시 0 |

`condensate_kg_m3`와 `turbulence_m_s`는 날씨 모델에서 만든 렌더링용 근사값이다.

| cloud_kind | 의미 |
|---:|---|
| 0 | 없음. 유효 layer에서는 사용하지 않음 |
| 1 | stratiform 또는 지표에 붙은 fog |
| 2 | 일반 convective |
| 3 | AMSL top이 `8,000 m` 이상인 깊은 convective |
| 4 | cirrus |
| 5 | generic stratiform 예약 표현 |

fog와 stratiform은 kind 1을 공유한다. 수신기는 지표 고도를 뺀 AGL base가 `5 m` 이하이면
fog로 해석한다. `cloud_layer_count` 이후 층은 32바이트 모두 0이다.

## 태양·달·별과 대기 광학

| Offset | Type | 이름 | 단위/의미 |
|---:|---:|---|---|
| 352 | `float32[3]` | sun_direction_enu | 관측자에서 태양을 향하는 단위 vector |
| 364 | `float32` | sun_angular_radius_rad | 태양 각반경, rad |
| 368 | `float32` | sun_direct_irradiance_w_m2 | 대기 감쇠 후 직달 일사, W/m2 |
| 372 | `float32` | sun_direct_illuminance_lux | 직달 조도, lux |
| 376 | `float32[3]` | moon_direction_enu | 관측자에서 달을 향하는 단위 vector |
| 388 | `float32` | moon_angular_radius_rad | 달 각반경, rad |
| 392 | `float32` | moon_illuminated_fraction | 보이는 달 면의 조명 비율, `0..1` |
| 396 | `float32` | moon_illuminance_lux | 지표 달빛 조도 근사, lux |
| 400 | `float32[4]` | celestial_to_enu_xyzw | 적도 천구 좌표를 현재 위치의 ENU로 회전하는 quaternion |
| 416 | `float32` | local_sidereal_angle_rad | local apparent sky 회전 입력, `0..2pi` |
| 420 | `float32` | geomagnetic_kp | 예약, 현재 0 |
| 424 | `float32` | aurora_intensity | 예약, 현재 0 |
| 428 | `float32` | star_radiance_scale | 별 노출용 상대 radiance scale, 현재 `0..0.04` |
| 432 | `float32[3]` | rayleigh_scattering_rgb_per_m | RGB Rayleigh scattering, 1/m |
| 444 | `float32[3]` | mie_scattering_rgb_per_m | RGB Mie scattering, 1/m |
| 456 | `float32[3]` | mie_absorption_rgb_per_m | RGB Mie absorption, 1/m |
| 468 | `float32[3]` | ozone_absorption_rgb_per_m | RGB ozone absorption, 1/m |
| 480 | `float32[3]` | sun_color_rgb | 정규화된 선형 RGB 근사 |
| 492 | `float32[3]` | moon_color_rgb | 정규화된 선형 RGB 근사 |
| 504 | `uint32` | reserved | 반드시 0 |
| 508 | `uint32` | crc32 | offset `0..507` CRC32 |

태양·달 위치는 UTC와 위·경도로 계산한다. 달 궤도는 렌더링용 근사식이다.

## 수신 검증과 순서

수신 검증 기준은 [`deserializeSkyState`](../src/sky_protocol.hpp)와
[`receive_sky.py`](../tools/receive_sky.py)다. 검사를 통과한 상태만 렌더링에 사용한다.

1. 길이 512, magic, version, packet size와 CRC32를 확인한다.
2. 예약 바이트·비트와 비활성 구름층이 0인지, enum과 구름층 수가 유효한지 검사한다.
   지원 field mask는 `0x001f`의 부분집합, 활성 mask는 지원 mask의 부분집합이어야 한다.
3. 모든 실수가 유한하고 필드별 범위 안인지 검사한다. 태양·달 방향과 천구 quaternion의
   길이는 `0.99..1.01`, 유체 시간은 0 이상, 예측 유효 시간은 유체 시간 이상이어야 한다.
   강수가 있으면 비·눈·우박 비율의 합은 1, 없으면 0이다(오차 `0.0001`).
4. `d = uint32(new_sequence - old_sequence)`가 `0 < d < 0x80000000`일 때 새 상태로 받는다.
   sequence 간격은 손실 지표이며 시간 불연속의 근거가 아니다.

이 비교는 같은 서버 실행 내의 순서에 적용한다. 재시작하면 sequence 기준을 새로 잡는다.
UTC와 유체 시간은 별개이며 음수 `time_scale`에 따른 UTC 감소도 유효한 상태다.
현재 Unreal 수신·조명 동작은 [렌더링 가이드](unreal-rendering.md)를 참고한다.

ENU는 볼륨 로컬 X=east, Y=north, Z=up에 대응한다. Directional Light에는 광원의 반대
방향, 즉 `-sun_direction` 또는 `-moon_direction`을 적용한다.

## 시간 진행

UTC는 실제 경과 시간에 `time_scale`을 곱해 진행한다. 0이면 UTC와 천체 위치, UTC 기반
자연 날씨 목표가 멈춘다. 날씨 전환·목표값 수렴, 지면 젖음·번개는 실제 시간에 따라 계속된다.
유체 계산과 `CLD2` 구름 전송도 계속 진행한다.

긴 중단 후에도 UTC는 경과 시간을 반영한다. 누적 경과 시간을 하루 이하로 나눈 뒤 각 구간의
날씨 적분을 최대 60회로 나누고
오래전에 끝난 번개를 다시 재생하지 않는다. UTC가 지원 범위 끝에 닿으면 경계값에 고정하고
`time_scale`을 0으로 바꾼다.

## 실행 예

저장소 루트에서 실행한다. 수신기는 유효한 패킷 하나를 검증·출력한 뒤 종료한다.

```powershell
python tools/receive_sky.py --host 127.0.0.1 --port 7779
python tools/receive_sky.py --port 7779 --json
```

서버 빌드 방법은 [README](../README.md#실행)를 참고한다. 서울 위치에서 실제 1초당
UTC 한 시간을 진행하는 예시다.

```powershell
.\build\Release\cloud_sim_server.exe `
  --sky-host 127.0.0.1 --sky-port 7779 --sky-hz 5 `
  --utc 2026-08-25T03:00:00Z `
  --latitude 37.5665 --longitude 126.9780 --elevation-m 38 `
  --weather natural --weather-seed 42 --time-scale 3600
```

`--no-sky`는 `SKS1`, `--no-send`는 구름 볼륨 전송을 끈다.

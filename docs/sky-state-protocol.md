# 하늘 상태 프로토콜 (`SKS1`)

`SKS1`은 서버가 Unreal에 보내는 전역 하늘·날씨 상태 스냅샷입니다. 구름 voxel은 기존
`CLD2`가 담당하고, `SKS1`은 같은 장면을 그리는 데 필요한 UTC, 위치, 태양·달, 대기
산란 계수, 바람, 강수, 구름층 메타데이터를 한 UDP datagram에 담습니다.

- 기본 목적지: `127.0.0.1:7779`
- 기본 전송률: `5 Hz` (`--sky-hz`로 `1..simulation_hz` 범위에서 변경)
- datagram 크기: 항상 `512 bytes`
- byte order: 모든 정수와 IEEE-754 실수는 little-endian
- 좌표계: 오른손 로컬 ENU, `X=east`, `Y=north`, `Z=up`
- CRC: offset `0..507`의 IEEE CRC32를 offset `508`에 기록

`SKS1`은 한 datagram이므로 조각 재조립이 필요하지 않습니다. UDP 손실 시 직전의 유효
상태를 유지하고 다음 패킷을 기다립니다.

## 포트와 스트림 관계

| 기본 포트 | 방향 | 프로토콜 | 역할 |
|---:|---|---|---|
| 7777 | 서버 -> Unreal | `CLD2` | density, velocity 등 3D volume field |
| 7778 | Unreal -> 서버 | `CLC2` | 비행기·물체 interactor |
| 7779 | 서버 -> Unreal | `SKS1` | 전역 하늘·날씨 상태 |
| 7780 | Unreal -> 서버 | `SKC1` | 시간·위치·날씨 제어 |

`SKS1.volume_frame_id`는 가장 최근에 전송한 `CLD2.frame_id`를 가리킵니다. 두 스트림은
독립 UDP이므로 항상 동시에 도착하지는 않습니다. 수신기는 각 스트림의 최신 완성 상태를
따로 보관하고 `volume_frame_id`를 동기화 힌트로만 사용해야 합니다.

## 현재 Unreal 소비 필드

포함된 `SkySimSystem`은 현재 다음 `SKS1` 값을 직접 사용합니다.

| 상태 | 현재 Unreal 용도 |
|---|---|
| UTC, 위치, elevation, time scale | Server State 표시와 editor authoring 동기화 |
| domain extent, cloud layer count | Heterogeneous Volume 크기와 상태 표시 |
| sun direction/color/lux | 태양 Directional Light |
| moon direction/phase/lux | 달 Directional Light |
| relative humidity, visibility | `SkySimWeatherFog` 밀도 계산 |
| mean wind | 광역 weather-map 이동 |
| control session/sequence/result | `SKC1` ACK queue 완료·거부·timeout 판정 |

구름 voxel density는 이 packet에 들어 있지 않으며 별도 `CLD2` field ID 1을 사용합니다.
Weather Fog도 cloud layer가 아닙니다. Unreal이 `SKS1.visibility`와 humidity에서 파생해 만든
`ExponentialHeightFog` 표현이므로, 지면의 뿌연 층과 3D cloud density를 진단할 때 두 경로를
구분해야 합니다. 현재 fog 활성 조건과 임시 비활성화 방법은
[Unreal Editor 사용 가이드](unreal-editor-guide.md#현재-weather-fog-동작과-알려진-제한)에
정리되어 있습니다.

## 공통 헤더

| Offset | Type | 이름 | 단위/의미 |
|---:|---:|---|---|
| 0 | `char[4]` | magic | ASCII `SKS1` |
| 4 | `uint16` | version | `1` |
| 6 | `uint16` | packet_bytes | `512` |
| 8 | `uint32` | state_sequence | `SKS1` datagram을 보낼 때마다 1씩 증가하는 sequence |
| 12 | `uint32` | last_control_sequence | 마지막으로 수락한 `SKC1.sequence`, 아직 없으면 0 |
| 16 | `uint32` | last_control_session | 마지막으로 수락한 `SKC1.session_id`, 아직 없으면 0 |
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

`evolution_mode`의 `timeline`과 `replay` 값은 wire 호환을 위해 예약되어 있습니다. 현재
서버가 실제로 날씨를 생성하는 모드는 `natural`과 preset/custom 기반 `manual`입니다.

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
| 64 | `float64` | utc_unix_seconds | UTC Unix timestamp, 지원 범위 1900..2100 |
| 72 | `float64` | latitude_degrees | 위도, `[-90,90] deg` |
| 80 | `float64` | longitude_degrees | 경도, `[-180,180] deg`, east positive |
| 88 | `float32` | elevation_amsl_m | volume 바닥의 평균 해수면 기준 고도, m |
| 92 | `float32` | time_scale | wall 1초당 UTC 진행 초. 0 정지, 음수 역행 |
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
| 156 | `float32` | boundary_layer_height_m | 현재 대류도로 만든 유효 경계층 높이, m |
| 160 | `float32` | precipitation_flux_kg_m2_s | 수분 등가 강수 flux, kg/(m2 s). 수치상 `mm/h / 3600` |
| 164 | `float32` | rain_fraction | 강수 중 비 비율, `0..1` |
| 168 | `float32` | snow_fraction | 강수 중 눈 비율, `0..1` |
| 172 | `float32` | hail_fraction | 예약, 현재 0 |
| 176 | `float32` | surface_wetness | 누적 지면 젖음, `0..1` |
| 180 | `float32` | snow_water_equivalent_kg_m2 | 예약, 현재 0 kg/m2 |
| 184 | `float32` | cape_j_kg | CAPE 형태의 대류 강도 지표, 현재 `0..3000 J/kg` |
| 188 | `float32` | cin_j_kg | CIN 형태의 억제 강도 지표, 현재 `0..120 J/kg` |
| 192 | `float32` | lightning_rate_hz | 번개 활동도에서 만든 빈도, 현재 `0..0.25 Hz` |

offset 184, 188, 192는 현 버전에서 완전한 sounding 기반 CAPE/CIN/물리적 번개율이
아니라 서버의 정규화된 대류·번개 활동도를 전달하기 위한 파생 지표입니다. 절대 관측값으로
해석하지 말고 구름 난류, 번개 빈도, 렌더 품질을 조절하는 입력으로 사용합니다.

`gust_delta_enu_m_s`의 크기를 평균 풍속에 더하면 서버의 gust speed가 됩니다. 평균 풍속이
0인데 돌풍만 있으면 방향을 정할 기준이 없으므로 서버는 결정적인 ENU east 방향에 delta를
넣어 크기를 보존합니다.

## 지면과 대기 scale

| Offset | Type | 이름 | 단위/의미 |
|---:|---:|---|---|
| 196 | `float32[3]` | ground_albedo_rgb | 선형 RGB 지면 albedo |
| 208 | `float32` | mie_anisotropy_g | Henyey-Greenstein `g` |
| 212 | `float32` | mie_scale_height_m | Mie 밀도 scale height, m |
| 216 | `float32` | rayleigh_scale_height_m | Rayleigh 밀도 scale height, m |
| 220 | `float32` | temperature_lapse_k_m | 현재 `-0.0065 K/m` |

행성 반지름은 이 버전의 packet에 포함되지 않습니다. 현재 서버는 Earth 기준 bottom
`6,371,000 m`, atmosphere top `6,471,000 m`를 사용하므로 Unreal 수신기도 동일 값을
기본값으로 사용합니다.

offset 432 이후의 산란/흡수 계수는 atmosphere bottom의 해면기압 기준값입니다. 렌더러는
관측자 반지름을 `bottom radius + elevation_amsl_m`로 두고 scale height를 적용합니다.
offset 112는 이름 그대로 해면기압이며 현지 기압이 아닙니다. 서버가 계산한 태양 직달
irradiance/lux에는 관측자 고도의 기압과 남은 aerosol column이 이미 반영되어 있습니다.

## 구름층 4개

offset `224`부터 32바이트씩 최대 4개 layer가 이어집니다.

```text
layer_offset = 224 + layer_index * 32   # layer_index 0..3
```

| Relative offset | Type | 이름 | 단위/의미 |
|---:|---:|---|---|
| +0 | `float32` | base_altitude_amsl_m | 구름 하단 AMSL, m |
| +4 | `float32` | top_altitude_amsl_m | 구름 상단 AMSL, m |
| +8 | `float32` | coverage | 수평 피복률, `0..1` |
| +12 | `float32` | condensate_kg_m3 | 유효 응결수 `optical_depth / (100 * thickness_m)`, kg/m3 |
| +16 | `float32` | ice_fraction | 얼음상 비율, `0..1` |
| +20 | `float32` | precipitation_flux_kg_m2_s | 해당 층의 수분 등가 강수 flux, kg/(m2 s) |
| +24 | `float32` | turbulence_m_s | 현재 `0.25 + 4 * convective_activity`, m/s |
| +28 | `uint8` | cloud_kind | 아래 wire kind |
| +29 | `uint8` | layer_flags | bit 0 convective, bit 1 precipitation, bit 2 lightning environment |
| +30 | `uint16` | reserved | 반드시 0 |

`condensate_kg_m3`와 `turbulence_m_s`는 현재 macro weather model에서 만든 renderer용
파생값입니다. 관측된 liquid-water content나 TKE와 동일한 절대량으로 취급하지 않습니다.

| cloud_kind | 의미 |
|---:|---|
| 0 | 없음. 유효 layer에서는 사용하지 않음 |
| 1 | stratiform 또는 지표에 붙은 fog |
| 2 | 일반 convective |
| 3 | AMSL top이 `8,000 m` 이상인 깊은 convective |
| 4 | cirrus |
| 5 | generic stratiform 예약 표현 |

fog와 stratiform은 wire kind 1을 공유하므로 수신기는 지표 elevation을 뺀 AGL base가
약 `5 m` 이하인지 함께 보고 fog를 구분합니다. `cloud_layer_count` 이후 layer의
32바이트는 모두 0입니다.

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

태양·달 위치는 UTC와 위·경도로 계산합니다. 달은 렌더링용 truncated lunar series이며,
천문 관측이나 항법용 고정밀 ephemeris는 아닙니다.

## Unreal 수신 규칙

하나의 `SkySimSystem` 컴포넌트가 `CLD2`와 `SKS1`용 socket을 소유하는 구성이 단순합니다.
네트워크 worker에서 다음 검사를 통과한 상태만 game/render thread로 넘깁니다.

1. 정확히 512바이트인지 확인합니다.
2. magic, version, packet size, reserved byte를 검사합니다.
3. offset `0..507`의 CRC32를 계산합니다.
4. 모든 실수가 finite인지, 방향 vector 길이가 `0.99..1.01`인지 확인합니다.
5. `state_sequence`가 현재 상태보다 새 값인지 wrap-safe 비교합니다. 1보다 큰 간격은 UDP
   손실을 뜻하지만 그 자체가 시간 불연속을 뜻하지는 않습니다.
6. 최신 유효 snapshot 두 개를 보관하고 렌더 시간에 보간합니다.

방향은 ENU를 `SkySimVolume` local 축에 대응시킵니다. local X를 east, local Y를 north,
local Z를 up으로 두면 `FVector(East, North, Up)`으로 바로 구성할 수 있습니다. 태양
Directional Light가 **빛이 진행하는 방향**을 요구하면 `-sun_direction`을 사용합니다.

`SKS1` 5 Hz 사이에는 연속값을 `predicted_valid_time_seconds`까지 보간할 수 있습니다. 빠른
time scale에서는 endpoint 최단 호를 쓰지 말고 `delta UTC * 2*pi / 86164.0905`를 기준으로
sidereal angle을 unwrap합니다. 태양·달은 endpoint quaternion의 역회전으로 천구 좌표에
옮겨 보간한 뒤, 중간 sidereal 회전으로 다시 ENU에 놓습니다. 정확한 식과 Unreal 적용
순서는 [렌더링 가이드](unreal-rendering.md#sks1-상태-보간과-좌표)에 있습니다. UTC·위치·domain이
크게 바뀌었거나 유효 시간이 역행한 경우에는 이전 상태를 섞지 말고 temporal history를
재설정합니다. 단순 sequence 간격은 packet loss 표시에만 사용합니다.

구름층에는 persistent ID가 없으므로 두 snapshot의 같은 index를 곧바로 보간하지 않습니다.
같은 kind 중 중심 고도가 가장 가까운 층을 먼저 대응시키고, unmatched layer는 base/top을
고정한 채 coverage·condensate·precipitation만 0과 교차 fade합니다. 단, 양 endpoint의
서로 다른 층 합계가 protocol 한도 4개를 넘으면 남은 층 중 중심 고도가 가장 가까운 쌍을
대응시켜 고도와 kind도 전환합니다. renderer는 같은 fallback을 적용하거나 해당 snapshot에
snap해야 합니다.

서버의 달력은 실제 monotonic wall time을 사용하므로 짧은 stall 시간을 버리지 않습니다.
긴 절전·중단 후에는 전체 UTC를 따라잡되 날씨 상태를 최대 60개 구간으로 안정적으로
적분하고, 이미 지나간 번개 flash를 재개 시점에 다시 재생하지 않습니다. UTC가 지원 범위
끝에 닿으면 해당 경계에서 `time_scale = 0`으로 자동 정지합니다.

## 실행 예

참조 수신기는 한 패킷을 엄격히 검증해 요약하거나 JSON으로 출력합니다.

```bash
python3 tools/receive_sky.py --host 127.0.0.1 --port 7779
python3 tools/receive_sky.py --port 7779 --json
```

서울 위치에서 실제 UTC 속도로 적운 상태를 보냅니다.

```bash
./build/cloud_sim_server \
  --host 127.0.0.1 --port 7777 \
  --sky-host 127.0.0.1 --sky-port 7779 --sky-hz 5 \
  --latitude 37.5665 --longitude 126.9780 --elevation-m 38 \
  --weather cumulus --time-scale 1
```

한 wall-clock 초에 UTC 한 시간을 진행하는 일주 시연은 다음과 같습니다.

```bash
./build/cloud_sim_server \
  --utc 2026-08-25T03:00:00Z \
  --latitude 37.5665 --longitude 126.9780 \
  --weather natural --weather-seed 42 --time-scale 3600
```

`--no-sky`는 `SKS1`만 끕니다. `CLD2` 구름 volume 전송 여부는 `--no-send`가 별도로
제어합니다.

# Unreal 렌더링 아키텍처와 로드맵

이 문서는 현재 저장소에 포함된 Unreal Engine 5.6 기준 구현과 장기 목표인 전용 all-sky
renderer를 구분합니다. 이 서버는 Unreal 기본 `VolumetricCloud`에 값을 주입하지 않습니다.
현재는 `Heterogeneous Volume` backend를 사용하고, 장기적으로 서버의 macro
density/velocity와 `SKS1`을 하나의 custom render pipeline에서 합성합니다.

| 기능 | 현재 포함 | 목표 |
|---|:---:|:---:|
| `CLD2` RLE/CRC/frame 조립과 density 3D texture upload | O | O |
| `SKS1` 기반 태양·달·Sky Light·Sky Atmosphere·Weather Fog | O | O |
| Play 없는 editor preview와 Details authoring | O | O |
| Heterogeneous Volume material | O | 교체 가능 |
| regional mask, coordinate warp, 이중 density de-tiling | O | O |
| previous/current density와 velocity 기반 temporal 보간 | | O |
| occupancy skip, custom RDG ray march와 cloud shadow | | O |
| atmosphere LUT, 해·달 disk, 별과 depth-aware 합성 | | O |
| 강수 입자, 번개 bolt, 젖음·적설 material feedback | | O |

실제 프로젝트 실행과 Details 사용법은
[Unreal Editor 사용 가이드](unreal-editor-guide.md)를 먼저 참고하십시오. 아래에서 `[목표]`로
표시한 절은 현재 완료 기능이 아니라 다음 구현 계약입니다.

## 현재 Unreal에서 필요한 최소 구성

레벨에는 `SkySimSystem` Actor 하나를 배치합니다. 현재 Actor는 다음 항목을 노출하거나
내부에서 생성합니다.

- Network와 Editor Preview
- Time and Location, Weather와 Advanced Weather
- 최대 4개 Cloud Authoring slot
- Rendering, Wide Cloud World와 Cloud Presentation
- 태양·달·Sky Light, Sky Atmosphere와 Weather Fog
- 연결, control ACK, volume과 server state 진단 정보

Actor는 game/editor tick에서 `CLD2`와 `SKS1` 수신 socket, `SKC1` 송신/ACK queue, 최신
density CPU buffer와 transient `PF_G16` texture를 소유합니다. 현재 Unreal 프로젝트에는
`CLC2`를 보내는 `SkySimInteractorComponent`가 아직 없으며 서버 기능은
`tools/send_interactor.py`로 검증합니다.

내부 `HeterogeneousVolumeComponent`는 `/Game/SkySim/M_SkySimVolume`을 사용합니다. 이
material은 20km reference density를 광역 영역에 연속 반복하되 regional clear-sky mask,
weather-map coordinate warp와 비정수 scale/offset의 두 번째 live-density lookup으로 정확한
반복을 숨깁니다. detail erosion은 낮은 비정수 tiling과 mip 2를 사용합니다.

## 서버와 Unreal의 책임

| 처리 | 서버 | Unreal GPU |
|---|:---:|:---:|
| UTC·위치·time scale과 자연 날씨/preset 전이 | O | |
| 태양·달 방향/세기, 별 회전, 대기 산란 계수 | O | |
| 응결·증발·부력과 큰 구름 형태 | O | |
| 물체의 moving-solid 경계와 구름 질량 밀어내기 | O | |
| 빠른 물체의 swept wake·난류·날개 끝 와류 | O | |
| density/velocity 프레임 사이 물리적 이동 | O | 목표: 화면 보간 |
| 작은 billow와 wispy edge | | 현재 material erosion / 목표 전용 noise |
| 대기 LUT와 태양/달 disk, 별 catalog raster | | 목표 |
| 태양 투과, cloud phase, 다중 산란 근사 | | 현재 HV / 목표 custom pass |
| 비·눈 입자, 안개 depth 합성, 번개 bolt/음향 | 상태/event | Fog만 현재, 나머지 목표 |
| 카메라별 ray march, depth 합성, temporal AA | | 목표 |

서버에서 카메라 종속 ray march까지 계산하면 카메라마다 큰 영상을 전송해야 하고 Scene
Depth와 정확히 합성하기 어렵습니다. 카메라·조명에 따라 달라지는 마지막 렌더링 단계만
Unreal GPU에 남기는 것이 설정과 대역폭을 모두 줄입니다.

## [목표] 단일 수신 시스템과 GPU 업로드

1. volume socket이 같은 `CLD2.frame_id`의 모든 `field_mask` field를 조립합니다.
2. RLE을 풀고 field CRC32를 확인합니다.
3. sky socket은 정확히 512바이트인 `SKS1`의 CRC32와 범위·reserved field를 검사합니다.
4. 완성되지 않은 오래된 volume frame은 버리고, 가장 최신의 완성 frame만 game/render thread로
   넘깁니다.
5. density는 1채널 16-bit normalized 3D texture로 올립니다.
6. velocity는 protocol의 3채널 signed 16-bit 값을 decode합니다. 엔진/플랫폼에서
   3채널 SNORM volume format이 불편하면 RGBA16F 또는 RGBA16 SNORM으로 확장하고 A를 0으로
   둡니다.
7. temperature/vapor는 선택적인 detail·색조 입력, occupancy는 빈 공간 건너뛰기용 coarse
   texture로 사용합니다.
8. 최신 두 `SKS1`을 작은 uniform/structured buffer로 올려 대기·구름·천체 pass가 함께
   사용합니다.

GPU texture는 항상 두 세트(`previous`, `current`)를 유지합니다. 패킷 callback에서 GPU
resource를 직접 건드리지 말고, 완성된 CPU staging buffer의 소유권만 render thread에
넘깁니다. `CLD2`와 `SKS1`은 서로를 기다리며 render thread를 막지 않습니다.

`SKS1.volume_frame_id`는 어느 volume frame까지 송신됐는지 알려주는 힌트입니다. UDP 손실
때문에 정확히 짝이 없는 것은 정상이며, 최신 완성 volume과 최신 유효 sky snapshot을 각각
계속 사용합니다.

## [목표] SKS1 상태 보간과 좌표

서버의 하늘 상태 기본 전송률은 5 Hz입니다. 일반 scalar는 `previous/current` 사이에서 시간
보간하고 `predicted_valid_time_seconds` 이후 장시간 extrapolation하지 않습니다. 다만 허용된
최대 time scale에서는 한 packet 사이에 천구가 한 바퀴 이상 돌 수 있으므로 sidereal angle과
천체 방향을 endpoint 최단 호로만 보간하면 안 됩니다.

```text
siderealRate = 2*pi / 86164.0905
expected     = (utc1 - utc0) * siderealRate
deltaTheta   = expected + WrapPi(theta1 - theta0 - expected)
theta(t)     = theta0 + frameFraction * deltaTheta
```

위 식으로 sidereal 위상을 unwrap하고, `theta(t)`와 위도에서 `astronomy.hpp`와 같은
celestial-to-ENU 회전을 다시 만듭니다. 태양·달은 각 endpoint의 quaternion 역회전으로 먼저
천구 좌표에 옮기고, 그 천구 vector를 보간·normalize한 뒤 중간 celestial-to-ENU 회전으로
ENU에 되돌립니다. 이렇게 해야 `--sky-hz 1 --time-scale 86400`에서도 하루 회전이 약 1도로
접히지 않습니다. 짧은 구간에서 quaternion 자체를 보간할 때만 `dot(q0,q1) < 0`이면 두 번째
부호를 뒤집고 slerp합니다. 위치가 바뀌었거나 endpoint UTC 차이가 7일을 넘는 경우에는
천체 history를 재설정하고 최신 상태로 snap합니다.

서버 환경 vector는 오른손 ENU입니다.

```text
SkySim local X = east
SkySim local Y = north
SkySim local Z = up
DirectionUE = SkySimVolume.TransformVectorNoScale(DirectionENU)
```

`sun_direction`과 `moon_direction`은 관측자에서 천체를 향합니다. Directional Light가 빛의
진행 방향을 요구하면 `-sun_direction`을 사용합니다. `state_sequence` 간격은 UDP packet
손실 표시이며 그 자체로 history를 버리지 않습니다. 손실 구간도 위 UTC 기반 unwrap으로
복원합니다. UTC, 위치, domain이 크게 바뀌거나 유효 시간이 역행한 snapshot에는 보간하지
말고 cloud/sky temporal history를 재설정합니다.

구름층 배열에는 영구 layer ID가 없습니다. snapshot 사이에서 같은 index를 무조건 짝짓지
말고, 먼저 같은 `cloud_kind`끼리 층 중심 고도가 가장 가까운 항목을 대응시킵니다. 대응되지
않은 새 층은 자신의 base/top 고도를 유지한 채 coverage·condensate·precipitation을 0에서
fade-in하고, 사라지는 층도 원래 고도에서 fade-out합니다. 서버의 preset/natural 전이도
우선 이 규칙을 사용합니다. 다만 양 끝의 서로 다른 층 합계가 protocol 한도인 4개를 넘으면
남은 층 중 중심 고도가 가장 가까운 쌍을 대응시켜 base/top과 kind도 전환합니다. 이는 특히
사용자 지정 4층에서 preset/natural로 바꿀 때 발생할 수 있는 표현 한계입니다. renderer도
같은 4-slot fallback을 쓰거나 해당 snapshot에서 최신 상태로 snap해야 서버와 어긋나지
않습니다.

정확한 packet offset, 단위, flag는 [SKS1 문서](sky-state-protocol.md)를 따릅니다.

## [목표] 대기, 해, 달과 별

최종 렌더러는 `SKS1`의 Rayleigh/Mie/ozone RGB 계수, scale height, Mie anisotropy와 지면
albedo로 transmittance 및 multi-scattering LUT를 갱신합니다. 계수가 조금 변할 때 매
frame 전체 LUT를 다시 만들지 말고, 변경 임계값을 넘은 경우에만 분할 갱신하거나 두 LUT를
교차 보간합니다.

산란 RGB는 atmosphere bottom의 해면기압 기준 계수입니다. 카메라/volume 바닥 반지름을
`planetBottomRadius + elevation_amsl_m`로 두어 scale-height 감쇠를 한 번만 적용합니다.
`sea_level_pressure_pa`를 현지 기압으로 오해해 계수를 다시 고도 보정하면 고지대 대기가
이중으로 옅어집니다. 서버의 직달 일사 값은 이미 관측자 고도의 기압과 남은 aerosol column을
반영합니다.

- 태양 disk: `sun_direction`, angular radius, direct irradiance/lux, sun color 사용
- 달 disk: `moon_direction`, angular radius, illuminated fraction, moon color/lux 사용
- 별: local sidereal angle로 catalog를 회전하고 `star_radiance_scale`로 노출 조절
- 대기: 태양/달 light path에 같은 atmosphere transmittance를 적용

현 `SKS1`의 달 정보는 illuminated fraction까지이며 terminator 방향이나 표면 orientation은
보내지 않습니다. 처음에는 fraction 기반 analytic phase mask를 사용하고, 천문학적 방향까지
필요한 단계에서 protocol을 확장합니다. geomagnetic Kp와 aurora intensity는 현재 0이므로
오로라를 구현됐다고 간주하면 안 됩니다.

Unreal `SkyAtmosphere`를 임시 backend로 쓰는 경우에도 레벨에서 수동 보정하지 말고,
`SkySimSystem`이 `SKS1` 값을 component parameter로 변환합니다. 최종 전용 atmosphere
compute pass를 붙이면 같은 상태 buffer를 그대로 사용하므로 서버 계약은 바뀌지 않습니다.

### [목표] 하나의 render pipeline 순서

Render Dependency Graph 기준으로는 여러 shader dispatch를 하나의 `SkySim` feature/pass
안에서 다음 순서로 예약합니다.

1. 새 volume frame이 있을 때만 previous/current 3D texture upload 또는 swap
2. 대기 계수가 임계값 이상 바뀌었을 때만 transmittance/multi-scattering LUT 분할 갱신
3. 해·달·별을 포함한 atmosphere background 계산
4. half-resolution cloud primary/light ray march와 cloud shadow 계산
5. Scene Depth 앞까지만 aerial perspective와 cloud를 premultiplied 합성
6. velocity/depth 기반 temporal resolve 후 bilateral upscale

Directional Light는 지형·물체 조명과 shadow용이며, 해와 달 원반 자체는 all-sky pass가
그립니다. 이렇게 해야 사용자가 level마다 sky component를 조합하지 않아도 되고, 노출과
대기 감쇠도 한 상태에서 일관되게 계산됩니다.

## 현재 World 좌표와 광역 density sampling

Heterogeneous Volume의 world sample 위치를 object-local `[0,1]^3`으로 정규화합니다.

```text
Plocal = inverse(VolumeTransform) * Pworld
P01    = (Plocal + VolumeHalfSize) / (2 * VolumeHalfSize)
```

현재 Wide Cloud World는 `P01.xy`에 자동 tile count를 곱해 20km reference density를
연속적으로 wrap sampling합니다. X/Y texture address는 wrap이고 Z만 top/bottom
half-texel 범위로 clamp합니다. 첫 좌표에는 weather-map position warp를 적용하고, 두 번째
좌표에는 비정수 scale/offset을 적용한 뒤 regional map으로 두 density를 혼합합니다. 따라서
120km 볼륨이 6×6의 정확한 복사본처럼 보이는 현상을 줄이면서 reference tile 경계의
bilinear 연속성은 유지합니다. 향후 open-boundary/clipmap 방식으로 전환하면 이 반복 계약도
함께 바뀌어야 합니다.

## [목표] 프레임 사이 보간

서버가 10Hz, 화면이 60Hz여도 density를 단순 선형 혼합하면 형태가 녹아 보입니다. 먼저
velocity로 두 프레임을 서로 향해 반-이류한 뒤 혼합합니다.

```text
age0 = renderTime - previousSimulationTime
age1 = currentSimulationTime - renderTime

d0 = Sample(previousDensity, P01 - previousVelocity * age0 / GridSize)
d1 = Sample(currentDensity,  P01 + currentVelocity  * age1 / GridSize)
densityMacro = lerp(d0, d1, frameFraction)
```

패킷 지연이 커지면 무한 extrapolation하지 말고 약 100~150ms에서 멈춘 뒤 최신 frame을
유지합니다. 새 frame이 올 때 temporal history weight를 잠시 낮추면 ghosting을 줄일 수
있습니다.

## [목표] 전용 renderer의 density 함수

ray marcher가 사용할 최종 density는 서버 density를 그대로 확대하지 않고 다음처럼
만듭니다.

```text
macro      = SampleServerDensity(P01)
coverage   = smoothstep(lowThreshold, highThreshold, macro)

baseNoise  = lowFrequencyWorleyPerlin(Pworld * BaseFrequency)
edgeNoise  = highFrequencyWorley(Pworld * DetailFrequency)
curlOffset = curlNoise(Pworld, time, SampleServerVelocity(P01))

shaped     = remap(baseNoise, 1 - coverage, 1)
density    = max(0, shaped - edgeNoise * ErosionStrength) * macro
```

노이즈는 camera-relative UV가 아니라 절대 world 좌표를 사용해야 카메라가 움직일 때
구름이 미끄러지지 않습니다. 서버 velocity를 noise 좌표와 temporal reprojection에도
사용하면 비행기 후류의 작은 디테일이 큰 흐름을 따라갑니다.

## [목표] Ray-march와 조명

권장 시작 순서는 다음과 같습니다.

1. 카메라 ray와 `SkySimVolume` AABB의 진입/이탈 거리를 계산합니다.
2. occupancy가 0인 brick은 한 번에 건너뜁니다.
3. 구름이 있는 구간에서만 64~96개의 adaptive primary step을 사용합니다.
4. 각 sample에서 Beer-Lambert extinction으로 transmittance를 누적합니다.
5. `SKS1.sun_direction`으로 6~10개의 더 큰 light step을 사용하고 대기 transmittance도
   곱합니다.
6. 전방/후방 산란을 섞은 Henyey-Greenstein phase와 powder 효과를 적용합니다.
7. 저주파 multiple-scattering octave 2~3개를 저렴하게 더합니다.
8. scene depth보다 뒤의 sample은 중단하고 premultiplied color/alpha로 Scene Color에
   합성합니다.

```text
absorption   = exp(-density * extinction * stepLength)
scattered   += transmittance * (1 - absorption) * lighting
transmittance *= absorption
```

초기에는 half 또는 quarter resolution으로 ray march한 뒤 depth/normal-aware bilateral
upscale를 사용합니다. 완전 해상도에서 step 수만 줄이는 것보다 구름 경계 품질과 비용의
균형이 좋습니다.

## [목표] 강수, 안개와 번개

현재 서버는 전체 강수 flux, rain/snow fraction, 시정, 지면 젖음, cloud layer별 강수와
번개 event/flash를 `SKS1`으로 보냅니다. 이 버전에는 3D 강수 shaft나 번개 위치 field가
없으므로 Unreal은 다음처럼 표현합니다.

- 카메라 주변 비·눈 입자 수는 precipitation flux와 rain/snow fraction으로 결정
- cloud layer base와 coverage를 사용해 강수 발생 고도와 screen mask를 제한
- visibility와 습도에서 얻은 안개 상태를 scene depth 기반 aerial perspective에 반영
- `lightning_event_id`가 바뀌면 bolt/천둥 event를 한 번 생성하고 flash flag 동안 구름
  내부광과 exposure를 올림
- `surface_wetness`로 재질 collection parameter를 갱신하되 갑자기 0으로 snap하지 않음

비·눈 입자의 속도에는 `mean_wind + gust_delta`를 넣습니다. 빠른 비행기 주변의 국지적인
흐름은 `CLD2.velocity`를 sample해 입자와 mist에 더합니다. 이것이 서버의 swept wake와
화면의 작은 입자가 같은 방향으로 움직이게 하는 연결점입니다.

번개 위치가 아직 없으므로 event ID만으로 정밀한 구름 내부 bolt를 재현할 수 없습니다.
현 단계에서는 deterministic seed로 활성 cloud layer 안에 위치를 고르고, 이후 protocol에
서버가 계산한 위치·branch seed를 추가해야 네트워크 client 간 완전히 같은 번개를 만들 수
있습니다.

## [목표] Temporal accumulation

구름은 step noise가 눈에 잘 띄므로 frame마다 blue-noise/jitter로 sample 위치를 바꾸고
이전 결과를 재투영합니다.

- 카메라 motion vector와 서버 velocity를 함께 사용합니다.
- depth 차이, volume 진입 거리 변화, density 차이가 크면 history를 거부합니다.
- 빠른 비행기 주변은 velocity gradient가 크므로 history weight를 낮춥니다.
- 평균 영역은 history 85~95%, silhouette와 새로 생긴 구름은 훨씬 낮게 시작합니다.

## [목표] 시작 품질 프리셋

| 항목 | 빠른 확인 | 권장 시작 | 고품질 캡처 |
|---|---:|---:|---:|
| 렌더 해상도 | quarter | half | half/full |
| primary step | 40 | 72 | 128 |
| sun light step | 4 | 8 | 12~16 |
| multiple-scatter octave | 1 | 2 | 3 |
| temporal history | 85% | 92% | 95% (경계에서는 낮춤) |
| 서버 grid/send | `64³ / 10Hz` | `64³~128³ / 10Hz` | sparse `128³+` |

물체가 지나가는 효과를 확인할 때는 detail noise를 잠시 약하게 두고 서버 velocity와 macro
density 변형부터 검증합니다. 그다음 curl detail, lighting, temporal 순서로 켜야 문제의
원인이 물리·전송·렌더링 중 어디인지 쉽게 구분할 수 있습니다.

## 현실성의 범위와 단계별 로드맵

현재 서버가 구현한 부분은 서로 일관된 **전역 환경 상태 + 한 개 macro cloud domain**이며,
저장소의 `Unreal/uskysim`이 이를 보여주는 Heterogeneous Volume 기준 구현입니다. Natural
weather는 날짜·위치·seed에 따른 연속 절차 모델이지 수치예보가 아닙니다. 실제 전선, 도시
열섬, 산악파, 해륙풍, 레이더 강수 세포가 공간을 통과하는 모델은 아직 없고, 달
ephemeris도 렌더링용 근사입니다.

1. **현재 연결 기준선**: `64³ CLD2 + 5 Hz SKS1`, Heterogeneous Volume, editor authoring,
   태양/달 Directional Light와 SkyAtmosphere를 `SKS1`로 자동 구동
2. **완전한 전용 하늘 pass**: atmosphere LUT, 해·달 disk, 별 catalog, cloud shadow와
   aerial perspective를 한 pipeline으로 통합
3. **국지 효과**: 3D precipitation/icing field, 지표 fog volume, 번개 위치·branch seed,
   젖음·적설과 지면 feedback
4. **중규모 물리**: 여러 기단과 전선, 고도별 온습도/압력 field, 지형성 상승과 domain 간
   경계 교환
5. **운영 품질**: sparse brick/delta, GPU solver, timeline/replay 기록, 실제 관측·예보 입력,
   다중 Unreal client의 손실 복구와 clock 동기화

각 단계를 끝낼 때 clear noon만 보지 말고 일출/일몰, 야간 달빛, fog, storm, snow, 빠른
비행기 통과를 같은 test scene에서 회귀 검증해야 합니다. 특히 구름 모양, 대기 노출,
temporal ghosting을 따로 측정해야 한 항목을 개선하면서 다른 항목을 망가뜨리지 않습니다.

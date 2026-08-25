# Unreal rendering guide (custom cloud renderer)

이 서버는 Unreal 기본 `VolumetricCloud`에 값을 주입하는 구조가 아닙니다. 서버가 보내는
macro density/velocity를 전용 3D texture로 올리고, 별도의 GPU ray marcher가 Scene Color에
합성해야 합니다. 따라서 기본 구름 표현은 쓰지 않으면서도 `SkyAtmosphere`, 태양 역할의
Directional Light, 지형/물체의 Scene Depth는 그대로 사용할 수 있습니다.

## Unreal에서 필요한 최소 구성

레벨 설정은 다음 세 항목이면 충분합니다.

1. 구름 영역을 나타내는 `SkySimVolume` Actor 하나
2. 서버 프레임을 받는 `SkySimReceiverComponent` 하나
3. custom full-screen/volume render pass 하나

구름과 상호작용할 Actor에는 analytic collision proxy를 보내는
`SkySimInteractorComponent`만 붙입니다. Actor마다 voxel simulation이나 Niagara를 만들
필요는 없습니다. Unreal 기본 `VolumetricCloud` Actor/Component는 비활성화합니다.

## 서버와 Unreal의 책임

| 처리 | 서버 | Unreal GPU |
|---|:---:|:---:|
| 응결·증발·부력과 큰 구름 형태 | O | |
| 물체의 moving-solid 경계와 구름 질량 밀어내기 | O | |
| 빠른 물체의 swept wake·난류·날개 끝 와류 | O | |
| density/velocity 프레임 사이 물리적 이동 | O | O (화면 보간) |
| 작은 billow와 wispy edge | | O |
| 태양 투과, 위상 함수, 다중 산란 근사 | | O |
| 카메라별 ray march, depth 합성, temporal AA | | O |

서버에서 카메라 종속 ray march까지 계산하면 카메라마다 큰 영상을 전송해야 하고 Scene
Depth와 정확히 합성하기 어렵습니다. 카메라·조명에 따라 달라지는 마지막 렌더링 단계만
Unreal GPU에 남기는 것이 설정과 대역폭을 모두 줄입니다.

## 프레임 수신과 GPU 업로드

1. socket thread가 같은 `frame_id`의 모든 `field_mask` field를 조립합니다.
2. RLE을 풀고 field CRC32를 확인합니다.
3. 완성되지 않은 오래된 frame은 버리고, 가장 최신의 완성 frame만 game/render thread로
   넘깁니다.
4. density는 1채널 16-bit normalized 3D texture로 올립니다.
5. velocity는 protocol의 3채널 signed 16-bit 값을 decode합니다. 엔진/플랫폼에서
   3채널 SNORM volume format이 불편하면 RGBA16F 또는 RGBA16 SNORM으로 확장하고 A를 0으로
   둡니다.
6. temperature/vapor는 선택적인 detail·색조 입력, occupancy는 빈 공간 건너뛰기용 coarse
   texture로 사용합니다.

GPU texture는 항상 두 세트(`previous`, `current`)를 유지합니다. 패킷 callback에서 GPU
resource를 직접 건드리지 말고, 완성된 CPU staging buffer의 소유권만 render thread에
넘깁니다.

## World 좌표에서 volume 좌표로 변환

ray의 world sample 위치 `Pworld`를 `SkySimVolume` local 좌표로 옮긴 뒤 `[0,1]^3`으로
정규화합니다.

```text
Plocal = inverse(VolumeTransform) * Pworld
P01    = (Plocal + VolumeHalfSize) / (2 * VolumeHalfSize)
```

`P01`이 volume 밖이면 density는 0입니다. 서버와 동일하게 X/Y를 반복시키고 싶을 때만
`frac(P01.xy)`를 사용합니다. 월드 경계에서 구름이 반대편으로 나타나는 것이 싫다면
Unreal 렌더러는 volume 밖을 0으로 처리하고, 추후 서버의 open-boundary 모드와 함께
변경해야 합니다.

## 프레임 사이 보간

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

## TrueSky 계열 품질을 만드는 density 함수

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

## Ray-march와 조명

권장 시작 순서는 다음과 같습니다.

1. 카메라 ray와 `SkySimVolume` AABB의 진입/이탈 거리를 계산합니다.
2. occupancy가 0인 brick은 한 번에 건너뜁니다.
3. 구름이 있는 구간에서만 64~96개의 adaptive primary step을 사용합니다.
4. 각 sample에서 Beer-Lambert extinction으로 transmittance를 누적합니다.
5. 태양 방향으로 6~10개의 더 큰 light step을 사용합니다.
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

## Temporal accumulation

구름은 step noise가 눈에 잘 띄므로 frame마다 blue-noise/jitter로 sample 위치를 바꾸고
이전 결과를 재투영합니다.

- 카메라 motion vector와 서버 velocity를 함께 사용합니다.
- depth 차이, volume 진입 거리 변화, density 차이가 크면 history를 거부합니다.
- 빠른 비행기 주변은 velocity gradient가 크므로 history weight를 낮춥니다.
- 평균 영역은 history 85~95%, silhouette와 새로 생긴 구름은 훨씬 낮게 시작합니다.

## 시작 품질 프리셋

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

# Unreal 사용 가이드

`Unreal/uskysim/uskysim.uproject`에서 서버 연결, 하늘 설정, 자유 비행을 확인한다.
서버 빌드와 실행 명령은 [README](../README.md)에 있다.

## 프로젝트 실행

Unreal Engine 5.6, Visual Studio 2022 C++ 개발 도구와 Windows SDK가 필요하다.
프로젝트는 DX12·Shader Model 6과 Heterogeneous Volumes를 사용한다.
Python은 머티리얼 생성·검증 도구를 사용할 때만 필요하다.

C++ 변경 후에는 에디터를 닫고 다음 명령으로 빌드한다. 저장소 루트에서 실행하며,
`$ueRoot`는 설치 경로에 맞춘다.

```powershell
$ueRoot = "C:\Program Files\Epic Games\UE_5.6"
$project = (Resolve-Path ".\Unreal\uskysim\uskysim.uproject").Path

& "$ueRoot\Engine\Build\BatchFiles\Build.bat" `
  uskysimEditor Win64 Development "-Project=$project" `
  -WaitMutex -NoHotReloadFromIDE
```

1. 서버를 실행하고 프로젝트를 연다.
2. 기본 맵 `NewWorld`에서 `SkySimSystem` 액터를 선택한다.
3. 뷰포트의 Realtime을 켜고 Details에서 설정을 조절한다.
4. Play를 누르면 Spectator로 자유 비행한다.

한 레벨에는 `SkySimSystem`을 하나만 둔다. 에디터와 PIE는 수신 포트를 자동으로
넘겨받는다. `NewWorld`는 하늘 확인용 맵이므로 실제 지형은 별도로 준비해야 한다.

| 용도 | 기본 UDP 포트 |
|---|---:|
| 구름 데이터 `CLD2` | 7777 |
| 물체 상호작용 제어 `CLC2` | 7778 |
| 하늘 상태 `SKS1` | 7779 |
| 날짜·날씨 제어 `SKC1` | 7780 |

서버의 `--port`, `--sky-port`, `--sky-control-port`를 바꿨다면 액터의 Network 설정도
맞춘다. `CLC2`용 Unreal 컴포넌트는 아직 없다.

## 에디터 프리뷰

`Sky Sim | Editor Preview`에서 설정한다.

| 항목 | 기본값 | 동작 |
|---|---:|---|
| Preview In Editor | 켜짐 | Play 없이 구름 수신·표시 |
| Animate Time In Editor | 켜짐 | 서버 상태가 끊겨도 마지막 UTC와 배속에서 시계 진행 |
| Auto Apply Sky Controls In Editor | 켜짐 | Details 변경을 잠시 모아 서버에 전송 |
| Sync Authoring Settings On Connect | 켜짐 | 최초 연결과 감지된 서버 재시작 때 설정 동기화 |
| Apply Advanced Weather On Connect | 꺼짐 | 연결 시 수동 날씨 값을 적용 |

일반적인 소켓 재연결은 진행 중인 UTC를 유지한다. `Apply Advanced Weather On Connect`를
켜면 Natural 날씨에 수동 값이 적용된다.

## 날짜·시간·위치

`Sky Sim | Time and Location`의 `Start / Authoring Local Date and Time`은 서버에 보낼
시작 시각이다. 현재 시각은 `Runtime Clock`의 `Current Local Date and Time`으로 확인한다.

| 항목·버튼 | 용도 |
|---|---|
| UTC Offset Hours | 현지 시각과 UTC의 차이 |
| Control Latitude Degrees / Control Longitude Degrees | 기준 위도·경도 |
| Control Elevation Meters | 기준 고도 |
| Control Time Scale | UTC 진행 배율. 음수는 역방향, 0은 달력 시계 정지 |
| Apply Date Time And Location | 작성한 날짜·위치 적용 (`ApplyDateTimeAndLocation`) |
| Apply Time Scale | 배율 적용 |
| Pause Sky Time / Resume Sky Time | UTC 정지 / 마지막 0이 아닌 배율로 복원 |

클래스 기본값은 서울·UTC+9·1배속이다. `NewWorld` 권장 설정은 Natural, seed 55,
60배속이다. `Control Status`가 `Applied`인지 확인하면 서버 적용 여부를 알 수 있다.

서버 상태가 2초 이상 끊기면 시계는 마지막 기준점에서 계속 진행한다. 기본 동기화 설정에서는
재시작한 서버에도 이 시각을 다시 적용하므로 작성 시작 시각으로 되감기지 않는다.

`Time Scale = 0`은 전체 시뮬레이션 일시정지가 아니다. UTC·천체 시계와 UTC에 따른 Natural
목표의 이동은 멈추지만, 진행 중인 날씨 전환·완화·젖음 변화와 구름 유체 계산은 계속된다.

## 날씨와 구름층

`Weather`에는 Natural, Clear, Cumulus, Overcast, Rain, Storm, Snow, Fog가 있다.
Natural은 날짜·위치·seed에 따라 날씨를 변화시키는 절차 모델이다. 실제 관측·예보 데이터는
사용하지 않는다. 다른 프리셋은 `Weather Transition Seconds` 동안 목표 상태로 전환한다.

`Advanced`에서는 온도, 습도, 시정, 바람, 강수 등을 직접 설정한다.
`Apply Custom Weather Settings`나 직접 작성한 구름층을 적용하면 수동 상태가 된다.
자동 변화로 돌아가려면 `Release Weather To Natural`을 누른다.

`Cloud Authoring`은 최대 네 개 구름층을 지원한다.

| 항목 | 의미 |
|---|---|
| Enabled / Type | 층 활성화와 Convective·Stratiform·Cirrus·Fog 유형 |
| Base Altitude (AGL) / Top Altitude (AGL) | 지표 기준 하단·상단 고도, 미터 |
| Coverage | 구름 피복률과 생성량 |
| Optical Depth | 층의 광학 두께 |
| Convective Activity | 대류 발달 정도 |
| Liquid Fraction / Precipitation (mm/h) | 액상 비율과 강수량 |

먼저 층의 고도·Coverage를 맞추고 `Weather Seed`로 배치를 바꾼다. Rain·Snow·Storm의
환경 상태는 바뀌지만 비·눈 입자와 번개 볼트는 아직 표시하지 않는다.

## 구름 움직임과 밀도

`Rendering | Cloud Motion`은 프레임 사이 움직임을 조절한다.

| 항목 | 기본값 | 조절 방법 |
|---|---:|---|
| Interpolate Cloud Frames | 켜짐 | 이전·현재 구름을 보간 |
| Cloud Interpolation Delay Seconds | 0.30초 | 5Hz 기준. 수신 간격이 불안정하면 늘리고 반응성을 높이려면 줄임 |
| Use Mean Wind Cloud Motion | 켜짐 | 평균 수평 바람으로 구름과 노이즈 이동 보정 |

완성 프레임을 최대 16개 보관한다. 표시 시각은 뒤로 가지 않으며, 지연을 늘리면 잠시
멈춰 새 지연에 맞춘다. 서버 전송이 끊기면 마지막 프레임까지 재생한 뒤 이동을 멈춘다.
수신을 직접 끈 경우에는 표시 버퍼를 유지해 밀도·형태 설정을 계속 편집할 수 있다.

`Rendering | Cloud Presentation`의 값은 화면 표현만 바꾸며 서버 밀도에는 영향을 주지 않는다.

| 항목 | 기본값 | 조절 방법 |
|---|---:|---|
| Density Shape Power | 0.92 | 1보다 작으면 중간 밀도가 진해짐 |
| Density Presentation Gain | 1.20 | 전체 표시 밀도 배율 |
| Cloud Spread Iterations | 0 | 주변으로 밀도 확장. 높이면 형태가 뭉개질 수 있음 |
| Stabilize Density Scale | 켜짐 | 프레임별 인코딩 범위 차이로 생기는 밝기 변동 완화 |
| Density Reference Scale | 8.0 | 높이면 구름이 옅어짐. 기준을 넘는 밀도는 포화 |
| Use Physical Density Scale For Rendering | 꺼짐 | 공통 밀도 기준을 머티리얼에 다시 적용 |

표시 기준 8은 시각 조정값이다. 서버 밀도는 Unreal의 광학 소멸 계수로 보정되지 않았으므로
물리 배율 옵션을 켜면 구름이 지나치게 불투명해질 수 있다. 이 옵션이 켜지면 밀도 안정화
설정과 관계없이 이전·현재 프레임을 같은 기준으로 변환한다.

`Extinction Scale`은 기본 0.10, `Detail Erosion Strength`는 0.07이다. 먼저 Gain과
Reference Scale을 맞추고 조정한다. Erosion을 크게 올리면 구름 중심이 비거나 잘게 찢어진다.

## 넓은 하늘 설정

README의 프리뷰 명령은 20km × 20km × 14km 영역을 사용한다. 서버 옵션의 기본 높이는
12km다. `Wide Cloud World`는 이 밀도장을 반복해 기본 120km 폭으로 표시한다.
표시 범위만큼 독립적인 구름을 계산하는 구조는 아니다.

| 항목 | 기본값 | 용도 |
|---|---:|---|
| Cloud World Horizontal Extent Km | 120km | 표시할 수평 폭 |
| Auto Horizontal Tile Count | 켜짐 | 서버 영역에 맞춰 반복 수 결정 |
| Wide Cloud Sampling Quality | 1.0 | 광역 볼륨의 수평 렌더 해상도 |
| Regional Clear-Sky Strength | 0.13 | 큰 청천 영역을 만드는 강도 |
| Large-Scale Position Warp | 0.16 | 위치 왜곡으로 반복 완화 |
| Pattern De-Tiling Blend | 0.72 | 다른 좌표로 읽은 구름과 혼합 |

반복이 보이면 seed·Coverage부터 조절하고 위치 왜곡과 혼합량은 조금씩 바꾼다.
고고도에서 보이는 사각 외곽과 반복 패턴은 아직 남아 있다. 단순히 폭을 늘려도
비행 시뮬레이터용 지평선 문제가 해결되지는 않는다.

## 자유 비행

| 입력 | 동작 |
|---|---|
| W / S, A / D | 앞뒤·좌우 이동 |
| Q / E | 하강·상승 |
| Shift | 4배 속도 |
| 마우스 | 시점 회전 |

기본 속도는 100,000cm/s이고 충돌은 꺼져 있다. `Invert Mouse Y` 기본값은 꺼짐이다.
조작이 안 되면 Play 뷰포트를 클릭하고 GameMode가 `uskysimGameModeBase`인지 확인한다.

## 문제 확인

| 증상 | 확인할 항목 |
|---|---|
| Details에 SkySim이 없음 | 액터 자체 선택 여부, 에디터 종료 후 C++ 빌드, 모듈 로드 오류 |
| 연결 안 됨 | Network 포트, 같은 포트를 쓰는 다른 수신기, `Is Receiving` |
| 로그만 나오고 구름이 없음 | `Has Complete Volume Frame`, 밀도 최댓값, `Enable Volume Rendering`, 구름 고도와 카메라 위치 |
| 시간·빛이 고정됨 | 작성 시작 시각 대신 Runtime Clock 확인, 실제 배율과 Pause 상태 확인 |
| 구름이 끊겨 움직임 | 보간 활성화, 수신 빈도와 완성 프레임 수, 보간 지연, 머티리얼 갱신 여부 |
| 화면이 어두움 | `Enable Environment Lighting`, 현재 태양 고도, 레벨의 중복 조명 |
| 지면이 뿌옇게 보임 | `Enable Weather Fog`를 꺼 비교하거나 `Fog Density Multiplier`를 0으로 설정 |
| 제어가 Pending에 머묾 | 7780 포트와 서버 실행 여부 |
| 제어가 Rejected됨 | 고도·날짜·습도·풍속 등의 입력 범위 |

지면의 뿌연 층은 구름과 별개인 `SkySimWeatherFog`일 수 있다. 현재는 Natural에서도
시정·습도에 따라 적용되며 시작 거리는 기본 0m다. 맑은 날의 안개 정책은 개선 항목이다.

Output Log의 `SkySim: CLD2 완성`은 구름 수신, `SKC1 applied`는 제어 적용을 뜻한다.
`cloud_motion`의 `prev/current`, `alpha`, `solver`, `buffered`, `phase_m`로 구름 보간을
확인한다. `SkySim clock:`은 UTC·현지 시각·배율·태양 고도를 출력한다.

진행·정지 구간을 기록한 로그는 다음 명령으로 검사한다.

```powershell
python .\tools\verify_unreal_time_progression.py `
  ".\Unreal\uskysim\Saved\Logs\uskysim.log" `
  --require-running --require-paused --require-sun-change 0.01
```

검증 결과와 남은 작업은 [구현 현황](implementation-status.md),
[개발 일정](development-schedule.md)에 정리한다. 로그가 정상이라고 화면의 속도·잔상까지
검증된 것은 아니다.

## 머티리얼·맵 도구

에디터 모듈은 `/Game/SkySim/M_SkySimVolume`이 없거나 생성 버전이 오래되면 다시 만든다.
직접 수정할 머티리얼은 별도 에셋으로 복사해 사용한다. 기본 그래프를 강제로 재생성하려면
에디터를 닫고 아래 명령을 실행한다. 기존 기본 머티리얼 그래프를 덮어쓴다.

```powershell
$ueRoot = "C:\Program Files\Epic Games\UE_5.6"
$project = (Resolve-Path ".\Unreal\uskysim\uskysim.uproject").Path
$script = (Resolve-Path ".\tools\create_unreal_volume_material.py").Path

& "$ueRoot\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  $project -run=pythonscript "-script=$script" -unattended -nop4 -nosplash
```

| 도구 | 동작 |
|---|---|
| `configure_unreal_dynamic_sky_defaults.py` | `NewWorld`에 권장 설정 저장 |
| `ensure_unreal_preview_start.py` | 시작 카메라·PlayerStart 배치 후 맵 저장 |
| `inspect_unreal_level.py` | 레벨과 SkySim 속성 조회 |
| `inspect_unreal_material.py` | 머티리얼 연결 조회 |

위 도구는 `tools/`에 있으며 Unreal Python으로 실행한다. 렌더링 코드를 수정할 때는
[렌더링 구조](unreal-rendering.md)를 참고한다.

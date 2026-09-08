# SkySim

Unreal Engine 5.6 기반의 실시간 하늘·구름 시뮬레이터.
C++ 서버가 구름과 날씨를 계산하고, Unreal이 UDP로 받아 렌더링한다.

## 기능

- 날짜·시간·위치에 따른 태양, 달, 하늘 조명
- 날씨 프리셋과 seed 기반 자동 날씨 변화
- 최대 4개 구름층과 실시간 밀도 보간
- 에디터에서 설정 변경 및 프리뷰
- Spectator 자유 비행

구름의 반복 패턴과 고고도에서 보이는 영역 경계는 개선 중이다.
비·눈 입자와 번개 효과는 아직 구현하지 않았다.

## 실행

Windows 기준으로 아래 환경이 필요하다.

- Unreal Engine 5.6
- Visual Studio 2022 C++ 개발 도구 및 Windows SDK
- CMake 3.16 이상
- Python 3.10 이상 — 테스트·검증 도구 사용 시

저장소 루트에서 서버를 빌드한다.

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

서버를 실행한다.

```powershell
.\build\Release\cloud_sim_server.exe `
  --grid 64 --hz 5 --send-hz 5 --sky-hz 5 `
  --protocol 2 --fields density --compression auto `
  --weather natural --weather-seed 55 --time-scale 60 `
  --domain-width-m 20000 --domain-height-m 14000
```

1. [Unreal 프로젝트](Unreal/uskysim/uskysim.uproject)를 열고 C++ 모듈 빌드 안내가 나오면 진행한다.
2. `NewWorld` 맵에서 `SkySimSystem` 액터를 선택한다.
3. Details에서 날짜·시간, 날씨, 구름층과 렌더링 값을 조절한다.

에디터에서도 프리뷰할 수 있다. Play를 누르면 자유 비행으로 확인한다.
초기 빌드와 세부 설정은 [사용 가이드](docs/unreal-editor-guide.md)를 참고한다.

| 입력 | 동작 |
|---|---|
| `W` `A` `S` `D` | 앞뒤·좌우 이동 |
| `Q` / `E` | 하강 / 상승 |
| 마우스 | 시점 회전 |
| `Shift` | 빠르게 이동 |

## 테스트

```powershell
ctest --test-dir build --build-config Release --output-on-failure
```

## 문서

- [Unreal 사용 가이드](docs/unreal-editor-guide.md)
- [구현 현황](docs/implementation-status.md)
- [개발 일정](docs/development-schedule.md)

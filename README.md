# Superrack — ASIO 다채널 VST3/AU 인서트 프로세서

JUCE 8 기반 Windows 네이티브 GUI 앱. ASIO 입력 채널마다 독립 플러그인 체인을 적용해
같은 번호 출력으로 1:1 송출하고, 채널별 Dry 원음을 멀티트랙 녹음한다.

**플러그인 포맷**: Windows = VST3 / macOS = VST3 + AudioUnit(AUv2).
AU 는 파일이 아니라 시스템에 등록된 AudioComponent 라 검색 경로 설정이 없다 —
브라우저의 [재스캔]이 표준 VST3 위치·추가 경로와 함께 등록된 AU 를 모두 훑는다.
AUv3(앱 확장)은 비동기 인스턴스화가 필요해 지원하지 않는다.

> **현재 상태: Phase 0~4 전부 실기 검증 완료** (2026-07-03) — VST3 체인 호스팅,
> Dry 멀티트랙 녹음 + 테이크 모델(펀치인/undo·redo), 통합 타임라인 재생(SPSC
> 스트리밍·SR 리샘플), 채널 병렬 DSP(워커 풀, 실측 3.0×), 세션 머신 이동성,
> 앱 설정(저장 위치/VST3 경로/워커 수), L1 회귀 테스트.

문서: [`docs/01-plan/PLAN.md`](docs/01-plan/PLAN.md) · [`docs/02-design/DESIGN.md`](docs/02-design/DESIGN.md) · [`docs/BACKLOG.md`](docs/BACKLOG.md) · 측정 [`docs/measurements/`](docs/measurements/) · QA [`docs/05-qa/`](docs/05-qa/)

---

## 사전 준비물 (Windows)

| 도구 | 비고 |
|------|------|
| **Visual Studio 2022/2026** | "C++ 데스크톱 개발" 워크로드 (MSVC, Windows SDK) |
| **CMake 3.22+** | VS 설치에 포함되거나 별도 설치 |
| **Git** | JUCE 를 FetchContent 로 받기 위해 필요 |
| **ASIO 드라이버** | 오디오 인터페이스 ASIO 드라이버 (없으면 ASIO4ALL) |

JUCE **8.0.14** 는 **CMake `FetchContent` 가 최초 빌드 시 자동 다운로드**한다(수동 클론 불필요).

### ASIO SDK 관련
`JUCE_ASIO=1` 빌드(Windows 전용)에는 Steinberg ASIO SDK 헤더(`common/asio.h`)가 인클루드 경로에
있어야 한다. JUCE 8.0.11+ 는 헤더를 번들하므로 보통 별도 SDK 불필요. 못 찾으면:

1. https://www.steinberg.net/developers/ 에서 ASIO SDK 다운로드·압축 해제
2. configure 시 `common` 폴더 경로 전달:
   ```bash
   cmake -B build -DASIO_SDK_DIR="C:/SDKs/asiosdk/common"
   ```

---

## 빌드

```bash
# 1) 구성 (최초 1회 JUCE 다운로드 — 인터넷 필요)
cmake -B build -G "Visual Studio 17 2022" -A x64

# 2) 빌드 (Release 권장)
cmake --build build --config Release
```

산출물: `build/Superrack_artefacts/Release/Superrack.exe`

### macOS 빌드

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

유니버설 바이너리는 `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0`
(CI 와 동일). 오디오 백엔드는 CoreAudio 이고 `JUCE_ASIO` 는 Windows 에서만 켜진다.

### 테스트 (헤드리스 — ASIO/플러그인 불필요)

```bash
cmake --build build --config Release --target SuperrackTests
./build/SuperrackTests_artefacts/Release/SuperrackTests.exe   # 실패 수 = 종료 코드
```

핵심 경로 L1 회귀(녹음 FIFO 무손실 / 커밋·펀치·undo / 재생 비트 일치·리샘플 /
병렬==직렬 / 설정 직렬화) — [`docs/05-qa/asio-vst3-host.qa-report.md`](docs/05-qa/asio-vst3-host.qa-report.md)

---

## 실행 / 사용

1. `Superrack.exe` 실행 — 마지막 장치 설정·세션(플러그인 체인)·테이크 자동 복원
2. ⚙ 설정: ASIO 장치/SR/버퍼 + 저장 위치·VST3 추가 경로·병렬 워커 수
3. 채널 랙: 채널마다 플러그인(VST3/AU) 추가/제거/바이패스/출력 게인, 네이티브 에디터 창
4. 트랜스포트: ● 녹음(현재 테이크에 펀치인 커밋) / ▶ 재생(체인 통과 옵션) / 진행바 시크
5. 테이크 행: 셀렉터(자동 세션 복원) · ⟲ 녹음 복구(undo/redo) · 삭제 · ＋ 새 테이크
6. 툴바: RT 레이턴시 테스터, CPU 프로파일(채널 한계 자동 측정 → 리포트 저장)

## 파일 구조

```
Superrack/
├─ CMakeLists.txt              # JUCE FetchContent + GUI 앱 + SuperrackTests
├─ Source/
│  ├─ Main.cpp                 # JUCEApplication + 메인 윈도우
│  ├─ AudioEngine.h/.cpp       # ASIO 콜백 · 라우팅 · 트랜스포트 · 세션 (UI 무관)
│  ├─ ChannelStrip.h/.cpp      # 채널별 VST3 체인 (farbot wait-free 스왑)
│  ├─ AudioWorkerPool.h/.cpp   # 채널 병렬 DSP (fork-join, 스파이크 계측)
│  ├─ MultitrackRecorder.*     # Dry 멀티트랙 녹음 (락프리 FIFO)
│  ├─ TimelinePlayer.h/.cpp    # 스템 재생 (SPSC 링 + 리더 스레드 + 리샘플)
│  ├─ TakeManager.h/.cpp       # 테이크 모델 (펀치인 커밋 / undo·redo)
│  ├─ CpuProfiler.h/.cpp       # 합성 부하 채널 한계 측정 + 리포트
│  ├─ AppSettings.h/.cpp       # 앱 설정 (저장 루트/VST3 경로/워커 수)
│  └─ MainComponent.h/.cpp     # 채널 랙 뷰 + 트랜스포트 + 설정 창
├─ Tests/TestMain.cpp          # L1 회귀 테스트 (헤드리스)
├─ ThirdParty/farbot/          # RealtimeObject (MIT, 벤더링)
├─ docs/                       # PDCA 문서 · 백로그 · 측정 · QA
└─ asio-vst3-host-design.md    # 원본 설계서 (역사 문서 — 현행은 docs/02-design)
```

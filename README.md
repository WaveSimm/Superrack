# Superrack — ASIO 다채널 VST3 인서트 프로세서

JUCE 8 기반 Windows 네이티브 GUI 앱. ASIO 입력 채널마다 독립 VST3 체인을 적용해
같은 번호 출력으로 1:1 송출하고, 채널별 Dry 원음을 멀티트랙 녹음한다.

> **현재 상태: Phase 0** — GUI 골격 + ASIO 장치 선택 + **2채널 1:1 패스스루**.
> VST3 호스팅과 녹음은 후속 Phase(코드 미구현, CMake 플래그만 켜둠).

설계: [`docs/01-plan/PLAN.md`](docs/01-plan/PLAN.md) · [`docs/02-design/DESIGN.md`](docs/02-design/DESIGN.md)

---

## 사전 준비물 (Windows)

| 도구 | 비고 |
|------|------|
| **Visual Studio 2022/2026** | "C++ 데스크톱 개발" 워크로드 (MSVC, Windows SDK) |
| **CMake 3.22+** | VS 설치에 포함되거나 별도 설치 |
| **Git** | JUCE 를 FetchContent 로 받기 위해 필요 |
| **ASIO 드라이버** | 오디오 인터페이스 ASIO 드라이버 (없으면 ASIO4ALL) |

JUCE 8.0.11 은 **CMake `FetchContent` 가 최초 빌드 시 자동 다운로드**한다(수동 클론 불필요).

### ASIO SDK 관련
`JUCE_ASIO=1` 빌드에는 Steinberg ASIO SDK 헤더(`common/asio.h`)가 인클루드 경로에
있어야 한다. 빌드 시 헤더를 못 찾으면 SDK 를 받아 경로를 지정한다:

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

#   (ASIO SDK 경로가 필요하면)
# cmake -B build -G "Visual Studio 17 2022" -A x64 -DASIO_SDK_DIR="C:/SDKs/asiosdk/common"

# 2) 빌드 (Release 권장)
cmake --build build --config Release
```

산출물: `build/Superrack_artefacts/Release/Superrack.exe`

## 실행 / 사용

1. `Superrack.exe` 실행
2. 장치 선택 UI 에서 **ASIO** 드라이버 선택, 샘플레이트·버퍼·입출력 채널 설정
3. 입력 1/2 신호가 출력 1/2 로 1:1 패스스루 (저지연 모니터)
4. 하단 **입력 레벨 미터**가 신호에 반응
5. 창 닫기 → 오디오 장치 정상 해제 후 종료

---

## Phase 0 완료 기준 (Acceptance)

- [ ] MSVC 로 빌드 성공
- [ ] 실행 시 장치 선택 UI 표시, ASIO 장치 선택 가능
- [ ] 입력 1/2 → 출력 1/2 저지연 패스스루 (소리 들림)
- [ ] 입력 미터가 신호에 반응
- [ ] 창 닫기 시 정상 종료 (크래시·행 없음)

## 파일 구조

```
Superrack/
├─ CMakeLists.txt           # JUCE FetchContent + GUI 앱 타깃
├─ Source/
│  ├─ Main.cpp              # JUCEApplication + 메인 윈도우
│  ├─ AudioEngine.h/.cpp    # ASIO 콜백 · 1:1 패스스루 · peak 미터 (UI 무관)
│  └─ MainComponent.h/.cpp  # 장치 선택 패널 + 입력 미터 뷰
├─ docs/                    # PDCA Plan / Design 문서
└─ asio-vst3-host-design.md # 원본 설계서
```

## 후속 Phase

| Phase | 내용 |
|-------|------|
| 1 | 채널별 VST3 체인 로드 + 네이티브 에디터 창 |
| 2 | 채널별 Dry 멀티트랙 녹음 (락프리 FIFO + ThreadedWriter) |
| 3 | 출력 게인 · xrun 처리 · 세션 저장/로드 |
| 4 | 32채널 확장 + CPU 프로파일링 |

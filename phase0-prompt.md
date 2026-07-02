# Phase 0 스캐폴딩 프롬프트 (Claude Code CLI용)

아래 블록을 그대로 Claude Code에 붙여넣으세요.

---

## [Phase 0] ASIO 채널 FX — GUI 골격 + 2채널 1:1 패스스루

JUCE 8 기반 Windows GUI 데스크톱 앱의 **Phase 0 스캐폴딩**을 만들어줘.
이번 단계 목표는 **빌드되고, ASIO 장치를 선택하고, 입력 2채널을 같은 번호 출력으로 1:1 패스스루**하는 것까지다.
**이번 단계에서는 VST3 호스팅과 녹음은 구현하지 마라**(후속 Phase). 단, CMake 플래그만 미리 켜둬도 된다.

### 기술 스택 / 환경
- JUCE 8.0.11+ — **CMake `FetchContent`로 자동 다운로드**(수동 클론·서브모듈 불필요, GitHub `8.0.11` 태그 고정), **CMake**, **MSVC (VS2022/2026)**, Windows
- 타깃: `juce_add_gui_app`
- 백엔드: **ASIO** (`JUCE_ASIO=1` — JUCE 8 번들 ASIO 사용, 별도 SDK 불필요)
- VST3 플래그(`JUCE_PLUGINHOST_VST3=1`)는 켜두되 코드에선 미사용

### 산출물(파일)
- `CMakeLists.txt`
- `Source/Main.cpp` — `JUCEApplication` + 메인 윈도우
- `Source/MainComponent.h` / `Source/MainComponent.cpp`
- `README.md` — **사전 준비물**(Visual Studio C++ 데스크톱 워크로드, CMake, Git) + 빌드/실행 방법 (cmake configure/build 명령 포함)

### 기능 요구사항
1. 메인 윈도우에 **`AudioDeviceSelectorComponent`** 배치
   - ASIO 장치 / 샘플레이트 / 버퍼 크기 / 입출력 채널 선택 가능
   - 입력 2채널, 출력 2채널을 기본 활성화 시도
2. **`AudioIODeviceCallback`** 구현, 콜백에서 **1:1 패스스루**
   - `out[ch][n] = in[ch][n]` (ch = 0..min(numIn, numOut)-1, 우선 2채널)
   - 입력보다 출력 채널이 많으면 나머지 출력은 무음(0)으로
3. 간단한 **입력 레벨 미터**(채널별 peak, atomic로 GUI 폴링) — 동작 확인용, 최소 구현 OK
4. 창 닫을 때 오디오 장치 정상 정지/해제, 크래시 없이 종료

### 실시간 안전성(오디오 콜백 내부)
- 동적 할당 / lock / 파일 I/O / 로깅(printf, DBG) **금지**
- `juce::ScopedNoDenormals` 사용
- 미터 등 공유 값은 **atomic**으로만 주고받기

### 코드 품질
- 클린 아키텍처 지향: 오디오 엔진 로직과 UI 분리(엔진 클래스 + 뷰 클래스)
- 매직 넘버 최소화(채널 수 등은 상수/멤버로)
- 주석은 핵심만 한국어로

### 완료 기준 (Acceptance)
- [ ] MSVC로 빌드 성공
- [ ] 앱 실행 시 장치 선택 UI 표시, ASIO 장치 선택 가능
- [ ] 입력 1/2 → 출력 1/2 저지연 패스스루 확인(소리 들림)
- [ ] 입력 미터가 신호에 반응
- [ ] 창 닫기 시 정상 종료(크래시·행 없음)

먼저 전체 파일 구조와 `CMakeLists.txt`부터 제시하고, 이어서 각 소스 파일을 구현해줘.
`CMakeLists.txt`에서 **`FetchContent`로 JUCE 8.0.11을 가져오도록** 구성하고(인터넷 최초 1회 다운로드, 이후 캐시), 빌드 명령을 README에 적어줘.

# ASIO 다채널 VST3 인서트 프로세서 (GUI) — 1:1 라우팅 + Dry 멀티트랙 레코더

> **상태: 역사 문서** (설계 시점 원문, 2026-06-29) — 이후 구현·개정 사항은 반영되지 않음.
> **현행 기준 문서**: [`docs/02-design/DESIGN.md`](docs/02-design/DESIGN.md) (모듈 계약·구현 노트) · [`docs/BACKLOG.md`](docs/BACKLOG.md)
> 워크플로우: Claude Code CLI(개발 도구) · Bkit/PDCA

---

## 1. 한 줄 정의

ASIO 입력 채널(시작 **2** → 최대 32)마다 **독립 VST3 체인**을 적용해 **같은 번호의 출력 채널로 1:1 송출**(라이브 모니터/리턴)하고, **동시에 채널별 Dry(원음)만 멀티트랙 녹음**하는 **GUI 프로그램**.

```
ASIO In 1 ─┬─►[Dry tap]─► (녹음: ch01_dry.wav)
           └─► Strip 1: VST3…VST3 ─► ASIO Out 1   (wet)
ASIO In 2 ─┬─►[Dry tap]─► (녹음: ch02_dry.wav)
           └─► Strip 2: VST3…VST3 ─► ASIO Out 2   (wet)
   ⋮  (최대 32, 입력 i ↔ 출력 i 1:1, 합산 없음)
```

- **합산(믹스 버스) 없음** → 채널 간 PDC 강제 없음, 페이더/팬/마스터 불필요
- **녹음은 Dry만** → 채널당 1파일

---

## 2. 기술 스택 결정

| 항목 | 선택 | 이유 |
|------|------|------|
| 프레임워크 | **JUCE 8.0.11+** | ASIO·VST3·오디오 파일 I/O·GUI 일괄 |
| 언어 | **C++17/20** | JUCE 기본 |
| 빌드 | **CMake** (`juce_add_gui_app`) | GUI 앱 타깃 |
| 컴파일러 | **MSVC (VS2022/2026)** | ASIO는 Windows 전용 |
| 오디오 백엔드 | **ASIO** (`JUCE_ASIO=1`) | 저지연 입출력 |
| 플러그인 호스팅 | **VST3** (`JUCE_PLUGINHOST_VST3=1`) | 요구사항 |
| 라우팅 | **수동 채널별 처리** | N개 독립 체인, 합산 없음 → 그래프 불필요 |
| GUI | `juce_gui_basics` / `juce_gui_extra` | 채널 뷰·플러그인 에디터 창 |
| 설정 | **JSON 세션 파일** | 채널 스트립 상태 직렬화 |

### JUCE 8.0.11 라이선스/SDK 메모
- **VST3 SDK 3.8.0 = MIT** (배포 부담 없음).
- **ASIO 오픈소스(GPLv3) + JUCE 번들**: `JUCE_ASIO=1`만 켜면 됨. 별도 SDK 불필요.
- ⚠️ **ASIO는 GPLv3** → 외부 바이너리 배포 시 전염 이슈. 사내/개인용 무관.

---

## 3. 핵심 제약사항

1. **ASIO 단일 디바이스 독점**. 입력·출력 같은 드라이버. (1:1 라우팅이므로 입력 채널 수 ≤ 출력 채널 수 필요.)
2. **샘플레이트·버퍼는 드라이버가 결정**. 32채널 확장 시 버퍼 128~256 권장.
3. **오디오 콜백 스레드 금지사항**: 할당·락·파일 I/O·로깅·예외.
4. **단일 오디오 스레드**: 32채널 × 체인 = CPU 1순위 리스크(§7, §9). 2채널은 여유.

---

## 4. 시스템 아키텍처

### 4.1 스레드 모델

```
┌──────────────────────────────────────────────────────────────┐
│ Audio Thread (ASIO 콜백, 최고 우선순위)                        │
│   for ch in 0..N:                                              │
│     Dry tap[ch] → 락프리 FIFO push                             │
│     copy In[ch] → 스트립[ch] VST3 체인 처리(in-place) → Out[ch]│
│   ※ 할당·락·I/O 금지. 컨트롤은 atomic/락프리 큐로만 수신       │
└──────────────────────────────────────────────────────────────┘
        │ 채널별 락프리 FIFO
        ▼
┌──────────────────────────────────────────────────────────────┐
│ Writer Thread (TimeSliceThread)                              │
│   FIFO drain → ThreadedWriter → 채널별 Dry WAV                 │
└──────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────┐
│ Message / GUI Thread                                         │
│   채널 뷰 렌더·미터 폴링(atomic)·트랜스포트                    │
│   VST3 네이티브 에디터 창(DocumentWindow) 호스팅              │
│   플러그인/라우팅 변경 → 락프리 명령 큐로 오디오 스레드에 전달 │
└──────────────────────────────────────────────────────────────┘
```

### 4.2 라우팅: 수동 채널별 처리 (확정)
- 각 입력 채널을 독립 처리: `In[ch]` → Dry 탭 → 스트립[ch] 체인 in-place 처리 → `Out[ch]`
- 채널 간 연결·합산 없음 → AudioProcessorGraph 불필요(보일러플레이트 절약)
- 입력 채널 수가 출력보다 많으면 초과분은 Dry 녹음만(출력 매핑 없음)
- 향후 합산/마스터 버스가 필요해지면 그때 `AudioProcessorGraph`로 전환

---

## 5. 모듈 설계

### 5.1 AudioEngine (ASIO I/O + 채널 처리)
- `AudioDeviceManager` + `AudioIODeviceCallback`
- 콜백에서 채널 루프: Dry push + 스트립 처리 + 1:1 출력
- 장치 열기/닫기, SR·버퍼 협상, 채널 활성화(시작 2 → 최대 32)
- 디바이스/SR 변경·드롭아웃 처리

### 5.2 ChannelStrip (채널별 VST3 체인) ×N
- 한 입력 채널당 1개. 내부: VST3 인스턴스 직렬 체인 (+ 출력 게인 옵션)
- `AudioPluginFormatManager` + `VST3PluginFormat`로 스캔(`KnownPluginList` 캐시)·로드
- `prepareToPlay(sr, maxBlock)` — 처리 시작 전 필수
- 기능: 플러그인 추가/제거/순서변경, 플러그인별 **Bypass**, **상태**(`get/setStateInformation`·`.vstpreset`)
- **GUI 장점**: 플러그인 **네이티브 에디터 창**을 띄움 → 숫자 파라미터 입력 불필요

### 5.3 출력 라우팅
- 스트립[ch] 출력 → ASIO **Out[ch]** (1:1, wet 모니터/리턴)
- 채널별 **출력 게인**(옵션, dB) — 합산 없으니 팬·페이더 불필요
- 출력 채널 매핑은 기본 입력과 동일 번호, 필요 시 오프셋 설정 가능

### 5.4 MultitrackRecorder (Dry 전용, 실시간 안전)
- **원칙: 오디오 스레드는 FIFO push만, 디스크 쓰기는 Writer 스레드.**
- 탭 지점: **Dry tap = 채널 입력 직후(스트립 전)** — 채널별 N트랙. **Wet 녹음 없음**
- 포맷(확정): **32-bit float WAV, 채널당 모노 파일**
- 폴더/파일: `rec/YYYYMMDD_HHMMSS/ch01_dry.wav`, `ch02_dry.wav`, …
- **샘플 정확 동기화**: 전 트랙 arm → 다음 콜백 경계에서 동시 시작 + 세션 타임스탬프 기록
- 디스크: 2ch면 ~0.4MB/s, 32ch면 ~6MB/s (SSD 여유). 96k면 2배
- **>4GB/장시간**: 자동 분할 또는 RF64 검토
- **언더런/디스크풀**: FIFO 오버플로 시 블록 드롭 + **xrun 카운터/플래그**(오디오 무중단)

### 5.5 ChannelView (GUI)
- **채널 스트립 ×N**: 입력 미터 · 플러그인 슬롯 목록(+에디터 열기) · Bypass · 출력 게인 · Dry 녹음 arm
- **트랜스포트**: 전체 녹음 시작/정지 · 세션 타이머 · xrun 표시 · CPU 미터
- **장치 설정 패널**: `AudioDeviceSelectorComponent`(장치/SR/버퍼/채널)
- **플러그인 에디터**: 별도 `DocumentWindow`(메시지 스레드)
- 컨트롤 전달: GUI → 락프리 명령 큐 → 오디오 스레드

---

## 6. 실시간 안전성 체크리스트 (Audio Thread)

- [ ] 동적 할당 없음 — 채널 버퍼·FIFO 사전 할당
- [ ] mutex/lock 없음 — atomic·락프리 FIFO만
- [ ] 파일 I/O·로깅·printf 없음
- [ ] 예외 throw 없음
- [ ] 전 플러그인 `prepareToPlay`는 처리 전 완료
- [ ] 가변 블록 대비 — maxBlock 기준 prepare
- [ ] **denormal 방지** — `ScopedNoDenormals`
- [ ] in/out 채널 수 정렬(1:1 매핑)

---

## 7. 지연(Latency) — 합산 없어 단순화

- **합산 버스가 없으므로 채널 간 PDC 강제 불필요.** 각 채널 모니터 지연 = ASIO in+out 버퍼 + 그 채널 플러그인 지연
- 저지연 모니터링: 버퍼 작게(64~128). 32채널 확장 시 CPU와 균형
- (옵션·v2) **스테레오 페어 정렬**: ch를 L/R로 묶어 들을 때 두 채널 플러그인 지연을 맞추고 싶으면 페어 단위 지연 보상
- Dry 녹음은 전 채널 입력 동시 탭이라 상호 정렬 보장

---

## 8. 세션 파일 스키마 (초안)

```json
{
  "device": { "name": "ASIO Device", "sampleRate": 48000, "bufferSize": 128 },
  "channels": 2,
  "strips": [
    {
      "input": 1, "output": 1,
      "plugins": [
        { "path": "C:/VST3/EQ.vst3", "bypass": false, "state": "<base64>" }
      ],
      "outGainDb": 0.0,
      "recDry": true
    },
    { "input": 2, "output": 2, "plugins": [], "outGainDb": 0.0, "recDry": true }
  ],
  "recording": { "dir": "rec", "format": "float32" }
}
```

---

## 9. 에러 처리 / 엣지 케이스

| 상황 | 처리 |
|------|------|
| **CPU 과부하**(채널 확장) | xrun 누적 표시, 버퍼 상향 안내. 단계적 확장으로 한계 측정 |
| ASIO 분리/SR 변경 | 콜백 정지 감지 → 녹음 안전 종료 → 재협상, 상태 표시 |
| 플러그인 로드 실패 | 해당 슬롯 제외 + 경고 |
| 플러그인 크래시 | **in-process**(재녹음 가능 전제) → 앱 동반 종료 위험 감수. 검증된 플러그인 우선 |
| 디스크 풀/쓰기 지연 | FIFO 오버플로 → 블록 드롭 + xrun, 오디오 무중단 |
| 입력>출력 채널 | 초과 입력은 Dry 녹음만, 출력 매핑 없음 |

---

## 10. 빌드 구성 (CMake 개요)

```cmake
cmake_minimum_required(VERSION 3.22)
project(AsioChannelFx VERSION 0.1 LANGUAGES CXX)

add_subdirectory(JUCE)

juce_add_gui_app(AsioChannelFx PRODUCT_NAME "AsioChannelFx")

target_sources(AsioChannelFx PRIVATE
    Source/Main.cpp
    Source/AudioEngine.cpp
    Source/ChannelStrip.cpp
    Source/MultitrackRecorder.cpp
    Source/ChannelView.cpp)

target_compile_definitions(AsioChannelFx PRIVATE
    JUCE_ASIO=1                 # 번들 ASIO (GPLv3 유의)
    JUCE_PLUGINHOST_VST3=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0)

target_link_libraries(AsioChannelFx PRIVATE
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_formats
    juce::juce_audio_utils
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
```

---

## 11. 개발 단계 (PDCA / Bkit) — 2채널로 완성 후 확장

| Phase | 목표 | 완료 기준 |
|-------|------|-----------|
| **0. GUI 골격** | `juce_add_gui_app` + 장치 설정 패널 + **2ch 1:1 패스스루**(이펙트·녹음 없음) | UI에서 ASIO 장치 선택, In 1/2 → Out 1/2 저지연 통과 |
| **1. 채널 VST3** | 스트립당 VST3 체인 로드 + 네이티브 에디터 창 + 1:1 wet 출력 | 채널별 이펙트 적용된 소리가 해당 출력으로 |
| **2. Dry 녹음** | 채널별 Dry TapProcessor + ThreadedWriter + 트랜스포트 | sync 맞는 Dry 스템 2파일 생성 |
| **3. 견고화** | 출력 게인, xrun 처리, 세션 저장/로드 | 세션 파일로 2채널 재현 |
| **4. 32채널 확장** | 채널 수 동적 확장, CPU 프로파일링, 버퍼 튜닝 | 32채널 안정 동작·한계 측정 |

---

## 12. 핵심 JUCE 클래스 빠른 참조

| 용도 | 클래스 |
|------|--------|
| 장치/콜백 | `AudioDeviceManager`, `AudioIODeviceCallback`, `AudioIODeviceType` |
| 무할당 처리 | `AudioBuffer<float>`, `ScopedNoDenormals` |
| VST3 호스팅 | `AudioPluginFormatManager`, `VST3PluginFormat`, `KnownPluginList`, `PluginDescription`, `AudioPluginInstance` |
| 플러그인 에디터 | `AudioProcessorEditor`, `createEditorIfNeeded()`, `DocumentWindow` |
| 녹음 | `AudioFormatManager`, `WavAudioFormat`, `AudioFormatWriter::ThreadedWriter`, `TimeSliceThread` |
| 락프리 | `AbstractFifo` (또는 SPSC 자체 구현) |
| GUI | `Component`, `AudioDeviceSelectorComponent`, `Slider`, `Viewport` |
| 설정 | `juce::JSON`, `var`, `File` |

---

## 13. 확정된 결정 (2026-06-29)

| # | 항목 | 확정 |
|---|------|------|
| 1 | 프로그램 형태 | **GUI 앱** (채널 뷰 + 플러그인 네이티브 에디터 창) |
| 2 | 채널 구조 | **채널별 독립 VST3 체인**, 시작 **2ch** → 최대 32ch |
| 3 | 출력 라우팅 | **1:1** (In i → 체인 → Out i), **합산·페이더·팬·마스터 없음** |
| 4 | 라우팅 엔진 | **수동 채널별 처리** (그래프 불필요) |
| 5 | 녹음 | **채널별 Dry만**, 32f WAV, 채널당 1파일 (**Wet 녹음 없음**) |
| 6 | 크래시 격리 | **in-process** (재녹음 가능 전제) |
| 7 | 지연 | 채널 독립, PDC 강제 없음(페어 정렬은 v2 옵션) |

### 미확정 (정보 필요, 설계 비차단)
- **ASIO 인터페이스 모델**: 채널 수는 런타임 동적 처리. 모델만 알면 검증 환경 맞춤.

---

## 14. 다음 액션

- [x] 형태(GUI)·채널 구조·1:1 라우팅·Dry 전용 녹음 확정
- [x] **Phase 0** 진행 (2채널 우선)
- [ ] Phase 0 스캐폴딩 → Claude Code CLI 실행 (GUI 골격 + 장치 패널 + 2ch 1:1 패스스루)
- [ ] 테스트용 저부하 VST3(간단 EQ) 준비
- [ ] (확장 시) ASIO 인터페이스 모델 확인

# DESIGN — ASIO 다채널 VST3 인서트 프로세서 (Superrack)

> PDCA: Design · 개정일: 2026-06-29 · Plan: [`../01-plan/PLAN.md`](../01-plan/PLAN.md)
> 전체 원문(다이어그램·클래스 참조 포함): [`../../asio-vst3-host-design.md`](../../asio-vst3-host-design.md)

이 문서는 gap 분석/검증의 기준이 되는 **모듈 계약**을 정리한다. 상세 다이어그램·JUCE 클래스 표는 원문 참조.

## 1. 스레드 모델
- **Audio Thread** (ASIO 콜백, 최고 우선순위): 채널 루프 — Dry tap → 락프리 FIFO push, In[ch] → 스트립[ch] VST3 체인 in-place → Out[ch]. 할당·락·I/O·예외 금지. 컨트롤은 atomic/락프리 큐로만.
- **Writer Thread** (`TimeSliceThread`): FIFO drain → `ThreadedWriter` → 채널별 Dry WAV.
- **Message/GUI Thread**: 채널 뷰·미터 폴링(atomic)·트랜스포트, VST3 네이티브 에디터(`DocumentWindow`), 변경을 락프리 명령 큐로 오디오 스레드에 전달.

## 2. 라우팅 (확정)
입력 채널 독립 처리: `In[ch]` → Dry tap → 스트립[ch] 체인 in-place → `Out[ch]`. 채널 간 연결·합산 없음 → `AudioProcessorGraph` 불필요. 입력>출력이면 초과분은 Dry 녹음만. 향후 합산 필요 시 그래프로 전환.

## 3. 모듈 계약
### 3.1 AudioEngine (ASIO I/O + 채널 처리)
- `AudioDeviceManager` + `AudioIODeviceCallback`
- 콜백: 채널 루프(Dry push + 스트립 처리 + 1:1 출력), 장치 열기/닫기, SR·버퍼 협상, 채널 활성화(2→32), 드롭아웃 처리

### 3.2 ChannelStrip ×N (채널별 VST3 체인)
- 입력 채널당 1개, 내부 VST3 직렬 체인(+출력 게인 옵션)
- `AudioPluginFormatManager`+`VST3PluginFormat` 스캔(`KnownPluginList` 캐시)·로드, `prepareToPlay(sr,maxBlock)` 선행
- 플러그인 추가/제거/순서변경, 플러그인별 Bypass, 상태(`get/setStateInformation`·`.vstpreset`), 네이티브 에디터 창

### 3.3 출력 라우팅
스트립[ch] → ASIO Out[ch] (1:1 wet). 채널별 출력 게인(dB, 옵션). 기본 동일 번호, 오프셋 설정 가능.

### 3.4 MultitrackRecorder (Dry 전용, 실시간 안전)
- 오디오 스레드는 FIFO push만, 디스크 쓰기는 Writer 스레드
- Dry tap = 채널 입력 직후(스트립 전), Wet 녹음 없음
- 32-bit float WAV, 채널당 모노 파일, `rec/YYYYMMDD_HHMMSS/chNN_dry.wav`
- 전 트랙 arm → 다음 콜백 경계 동시 시작, 세션 타임스탬프 기록
- FIFO 오버플로 시 블록 드롭 + xrun 카운터(오디오 무중단), >4GB는 분할/RF64 검토

### 3.5 ChannelView (GUI)
채널 스트립 ×N(입력 미터·플러그인 슬롯·Bypass·출력 게인·Dry arm), 트랜스포트(녹음 시작/정지·타이머·xrun·CPU 미터), 장치 패널(`AudioDeviceSelectorComponent`), 플러그인 에디터(`DocumentWindow`). GUI → 락프리 명령 큐 → 오디오 스레드.

## 4. 실시간 안전성 체크리스트 (Audio Thread)
- [ ] 동적 할당 없음(버퍼·FIFO 사전 할당)  - [ ] mutex/lock 없음(atomic·락프리 FIFO만)
- [ ] 파일 I/O·로깅·printf 없음  - [ ] 예외 throw 없음
- [ ] 전 플러그인 `prepareToPlay` 선행  - [ ] maxBlock 기준 prepare(가변 블록 대비)
- [ ] `ScopedNoDenormals`  - [ ] in/out 채널 수 정렬(1:1)

## 5. 세션 파일 스키마 (초안)
```json
{
  "device": { "name": "ASIO Device", "sampleRate": 48000, "bufferSize": 128 },
  "channels": 2,
  "strips": [
    { "input": 1, "output": 1, "plugins": [ { "path": "C:/VST3/EQ.vst3", "bypass": false, "state": "<base64>" } ], "outGainDb": 0.0, "recDry": true },
    { "input": 2, "output": 2, "plugins": [], "outGainDb": 0.0, "recDry": true }
  ],
  "recording": { "dir": "rec", "format": "float32" }
}
```

## 5.1 구현 노트 (Phase 1)
- **lock-free 체인 재구성**: 플러그인 추가/제거/순서변경은 메시지 스레드에서만. `ChannelStrip` 의 `reconfiguring` atomic 플래그를 켠 뒤 in-flight 콜백 종료를 확인하고 수정 → 그동안 오디오 콜백은 **dry 패스스루**. 오디오 스레드엔 락 없음. **(R1 해소, 2026-07-02)** 종료 확인은 고정 sleep(30) 이 아니라 `sr::CallbackFence`(Util.h) — 오디오 스레드가 콜백마다 세대 카운터를 bump 하고, 메시지 스레드는 세대가 **+2 전진**(= 플래그 이후 시작한 콜백 관측 ⇒ in-flight 종료 보장)할 때까지 대기, 콜백 정지 상태(비활성 채널·장치 정지)면 타임아웃(기존 상한과 동일)으로 복귀. 같은 패턴을 `MultitrackRecorder::stop`·`TimelinePlayer::unload` 에도 적용.
- **(R2 해소)** `ChannelStrip::process` 는 협상된 maxBlock 초과 블록·미준비 상태에서 dry 패스스루(부분 처리로 out 꼬리를 미기록으로 남기지 않음) + jassert.
- **(R3 해소)** `PluginScanCache`(경로→PluginDescription, AudioEngine 소유·전 스트립 공유, 메시지 스레드 전용) — `findAllTypesForFile` 파일 스캔을 경로당 1회로. 세션 복원·프로파일 32ch 체인 복제가 크게 빨라짐.
- **채널 내부 처리**: 모노 입력을 스테레오(2ch)로 복제→체인 처리→좌채널을 모노 출력. Phase 1 은 2ch 이하 플러그인만 허용(초과 시 로드 거부).
- **JUCE 8.0.11 API**: `AudioPluginFormatManager::addDefaultFormats()` 는 삭제됨 → 자유 함수 `juce::addDefaultFormatsToManager(mgr)` 사용.
- **에디터 수명**: `PluginWindow`(DocumentWindow) 는 항상 플러그인 인스턴스보다 먼저 파괴(`MainComponent` 멤버 선언 순서로 보장). 플러그인 제거 시 해당 에디터 창 먼저 닫음. 제거 버튼은 `callAsync` 로 지연(콜백 중 자기 파괴 방지).

## 5.2 루프백 레이턴시 테스터 (진단 유틸)
- Out ch0 에 임펄스(0.5, 1샘플) 송출 → In ch1 에서 임계값(0.05) 교차 검출 → `도착-송출` 샘플차 = 실측 왕복 지연. ÷SR → ms.
- 측정 중엔 일반 처리 생략하고 모든 출력 무음. atomic 상태기계(idle/measuring/done/timeout), 1초 타임아웃. 오디오 스레드 무할당·무락.
- GUI 는 드라이버 보고값(`getInput/OutputLatencyInSamples`)과 실측값을 나란히 표시.
- UI 한글 렌더링: 기본 sans-serif 를 `Malgun Gothic` 으로 지정(글리프). **+ 인코딩**: `juce::String(const char*)` 는 8비트 ASCII 전용이라 한글 리터럴이 깨짐 → 모든 비ASCII 리터럴은 `sr::u8()`(= `CharPointer_UTF8`)로 감싼다(`Source/Util.h`). 소스는 UTF-8, 빌드는 `/utf-8`.

## 5.4 구현 노트 (Phase 2 — Dry 녹음)
- **`MultitrackRecorder`** (`Source/MultitrackRecorder.*`): 채널마다 `juce::AudioFormatWriter::ThreadedWriter`(내부 락프리 FIFO) 1개 + 공유 `TimeSliceThread` 1개. 오디오 스레드는 `write()`(FIFO push)만, 디스크 쓰기는 Writer 스레드.
- **파일**: 32-bit float 모노 WAV. `AudioFormatWriterOptions{}.withBitsPerSample(32).withSampleFormat(floatingPoint)` (JUCE 8 신 API — 구 `createWriterFor(bits=32)`는 int32 PCM이라 float 명시 필수). 경로 `<Documents>/Superrack/rec/YYYYMMDD_HHMMSS/chNN_dry.wav`.
- **Dry tap 위치**: `AudioEngine` 콜백에서 스트립 처리는 `in→out`(별도 버퍼)이라 `inputChannelData`가 원음 그대로 유지됨 → 콜백 말미에 `recorder.writeBlock(inputChannelData, …)` 한 번. Wet 녹음 없음.
- **start/stop 동기화**: `active` atomic. start는 writers 완성 후 `active=true`(release). stop은 `active=false` 후 30ms 대기(in-flight 콜백 통과)→`writers.clear()`(ThreadedWriter 소멸 시 FIFO 플러시 + WAV 헤더 마감). ChannelStrip::reconfigure와 동일 가드.
- **오버플로**: `write()`가 false면 해당 블록 드롭 + `xruns` 카운터 증가(오디오 무중단). GUI가 경과시간·xrun 폴링.
- **채널 수**: 활성 스트립 수(현 2). 장치 입력 < 스트립 수면 `jmin`으로 가드. 장치 정지/앱 종료 시 자동 stop.
- (미검증) >4GB/RF64 분할은 미구현 — 장시간 녹음 시 Phase 3에서 검토.

## 5.6 영속화 3계층 + 테이크 모델
3가지 저장 파일을 **역할별로 분리**한다:
- **설정** `%APPDATA%/Superrack/audio-settings.xml` — ASIO 장치·SR·버퍼 (하드웨어 환경).
- **세션** `%APPDATA%/Superrack/session.json`(자동) + `.superrack`(수동) — 플러그인 체인·게인·채널 이름 (작업 구성).
- **테이크** `<Documents>/Superrack/takes/<id>/` — 녹음물(자기완결 프로젝트).

**Take** = 하나의 녹음 단위(`TakeManager`):
- `chNN_dry.wav` — 트랙별 32f 모노. 한 테이크 안에서 여러 번 녹음/펀치해도 **트랙당 1파일 제자리 갱신**.
- `timeline.json` — 환경 스냅샷(deviceName·sampleRate·bufferSize·channels) + `lengthSamples` + **history**(각 record/punch 의 `{op,startSample,endSample,at}` = 작업 이력).
- `session.json` — 녹음 시점 **세션 스냅샷**(환경 재현용).
- **새 테이크** = 새 폴더(기존 보존). **녹음**은 현재 테이크에 펀치인 커밋. 폴더는 "녹음마다"가 아니라 "테이크마다".
- **녹음 스크래치**: `<Documents>/Superrack/.rectmp/` — recorder 가 여기 캡처 → 정지 시 `TakeManager::commitRecording` 이 (펀치면 기존 [0..P) + 새 녹음) 을 테이크 파일에 임시파일 스왑으로 커밋.
- **테이크 로드** (콤보 선택): 세션 스냅샷 **자동복원**(`applySessionVar` → 체인/게인/이름) + 환경 불일치(SR ≠ 현재 장치) 시 **경고만**(장치 자동변경 안 함). 시작 시 가장 최근 테이크를 현재 테이크로.
- **UI**: 트랜스포트 아래 테이크 행 = 셀렉터(id·ch·SR·길이) + `＋ 새 테이크`.

## 5.5 구현 노트 (통합 타임라인 녹음/재생)
- **트랜스포트 상태기계** (`AudioEngine`): `tsStopped`(라이브 모니터) / `tsRecording`(라이브 모니터+Dry 캡처) / `tsPlaying`(스템 재생). atomic 전환, 콜백이 상태로 분기.
- **`TimelinePlayer`** (`Source/TimelinePlayer.*`): 최근 테이크의 `chNN_dry.wav` 스템들을 **같은 read 위치로 동기 재생**. `StemSource`(N개 모노 리더를 같은 pos로 읽는 `PositionableAudioSource`) → `BufferingAudioSource`(백그라운드 read-ahead)로 감싸 오디오 스레드는 미리 채워진 버퍼에서만 읽음. 디스크 읽기는 Reader 스레드.
- **재생 경로 옵션**: `playThroughChain` atomic. on=스템→채널 VST3 체인→출력(플러그인 적용/재처리), off=스템 그대로 출력. **재생 중 라이브 ASIO 입력은 뮤트**.
- **통합 타임라인**: 모든 채널 동일 시작·위치(오프셋 없음). 녹음은 콜백 경계 동시 arm(기존 recorder), 재생은 단일 위치. 끝 도달 시 콜백이 `tsStopped`로 복귀.
- **펀치인 녹음**: 녹음 시작 시 현재 재생 위치 P를 기록. 새 녹음은 새 폴더에 0부터 캡처(=타임라인 P 이후). 정지 시 `assembleTake`가 채널별로 **기존 테이크 [0..P) + 새 녹음**을 이어붙여 새 테이크 폴더(`*_asm`) 생성 → `currentTakeDir`로 승격, 재생기는 언로드 후 다음 재생 때 그 테이크 로드. P=0(테이크 없음)이면 새 녹음이 곧 현재 테이크. (v1: 펀치 지점부터 끝까지 덮어씀, 펀치아웃/오버더빙 없음. 조립은 메시지 스레드 오프라인 파일 I/O.)
- **트랜스포트 UI**: ⏮/●/▶/■ + `pos/len` 시간 + 진행바(드래그 시크) + "재생 시 플러그인 통과" 토글.
- (v1 트레이드오프) `BufferingAudioSource::getNextAudioBlock`은 내부에 짧은 락 사용 — JUCE 표준(AudioTransportSource와 동일). 완전 락프리 스템 스트리밍은 후속 견고화 항목.
- (v1 제약) 스템 SR과 장치 SR이 다르면 피치가 어긋남(리샘플 미적용). 장치 재시작 시 테이크 언로드 후 새 버퍼로 재로드.

## 5.7 구현 노트 (Phase 4 — DSP 부하 실측 + 합성 부하 CPU 프로파일링)
- **DSP 부하 실측** (`AudioEngine`): 콜백 전체를 QPC(`Time::getHighResolutionTicks`)로 감싸 `벽시계 시간 ÷ 버퍼 예산(numSamples/SR)` = load. avg(EMA α=0.05)·peak(CAS 루프)·예산 초과 콜백 수(load>1)를 atomic 으로 표면화. `getCpuUsage()`(평균만)보다 정밀. peak 리셋은 GUI store(0) — 오디오 스레드 CAS 와 경쟁해도 무락.
- **합성 부하** (`synthChannels` atomic, 0=끔): 물리 라우팅(`routed`) 이후 `strips[routed..N)` 을 입력 복제(`in[ch % numIn]`)로 **실제 처리**하고 출력은 사전할당 스크래치(1×maxBlock)에 폐기. UR22mkII(2ch)로도 32ch 체인 부하를 재현. 재생·레이턴시 측정 경로에서는 미적용(조기 return). 합성 채널 체인은 세션에 저장되지 않음(세션은 활성 채널만 직렬화).
- **`CpuProfiler`** (`Source/CpuProfiler.*`, 메시지 스레드 Timer): 시작 시 Ch1 체인을 32채널에 복제(`prepareSyntheticStrips`, 채널별 독립 플러그인 인스턴스) → 단계 {활성,4,8,16,24,32}마다 안정화 1초 + 측정 8초(통계 리셋 후 avg/peak/예산 초과/장치 xrun 델타) → 종료 시 합성 부하 해제 + 체인 제거 + 마크다운 리포트(`<Documents>/Superrack/profiles/cpu-profile-*.md`).
- **안정 판정**: 측정 구간 `xrun 0 && 예산 초과 0 && peak < 0.95`. **한계 채널 수** = 처음부터 연속 안정인 마지막 단계. 장치 정지·트랜스포트 개입 시 자동 중단.
- **GUI**: 툴바 `CPU 프로파일` 버튼(실행 중 `■ 중단` 토글), 진행 상황은 상태 행 라벨, 완료 시 요약 팝업+리포트 경로. perfLabel 은 `DSP avg% (pk %) xrun` 상시 표시(pk≥95% 또는 xrun>0 시 주황).
- (제약) 합성 부하는 단일 오디오 스레드 직렬 처리 기준 — 실제 32ch 장치의 드라이버 오버헤드(채널 I/O 전송)는 포함되지 않으므로 실기 확보 시 재검증 필요.

## 5.8 구현 노트 (D2 + A2 — wait-free 체인 스왑 & 채널 병렬 DSP, 2026-07-02)
- **D2 체인 스왑** (`ChannelStrip`, farbot `RealtimeObject<NodeList, nonRealtimeMutatable>` — ThirdParty/farbot 벤더링, MIT, **fifo 모듈 사용 금지**): 메시지 스레드가 원본 `editList` 수정 후 `publishChain()`(= `nonRealtimeReplace`) 복사 스왑. RT 획득은 wait-free(ScopedAccess), 편집 중에도 dry 갭 없이 직전 체인 유지. 구버전 벡터·노드(shared_ptr) 해제는 항상 메시지 스레드. Bypass 는 공유 Node atomic — 스왑 불필요. 기존 reconfiguring 플래그+CallbackFence 가드는 ChannelStrip 에서 제거(recorder/player 에는 유지).
- **A2 병렬 DSP** (`Source/AudioWorkerPool.*`): 채널=잡 fork-join. 콜백이 잡 배열 게시 → 티켓 카운터(fetch_add) 클레임으로 워커+오디오 스레드 분담 → jobsDone 스핀 조인. 게이트(GATE_CLOSED=2^40) 덕에 블록 경계에서 늦게 깬 워커의 스테일 클레임은 idx≥count 로 자연 폐기 — 이중 실행 불가. 워커 = `TIME_CRITICAL`+MMCSS "Pro Audio"(프로세스 클래스 불변), 대기 = 짧은 spin→이벤트(2ms 타임아웃 폴백). 워커 수 = 물리코어−2.
- 적용 경로: 라이브 모니터/녹음, 재생(체인 통과 시), 합성 부하(채널별 스크래치로 분리). GUI 토글 "병렬 DSP ×N"(off=직렬, 프로파일 비교용).
- 안전성 논거: 한 잡=한 스트립(스트립 내부 버퍼 스레드 격리), 클레임:완료 1:1 이라 조인 후 워커가 잡 실행 중일 수 없음 → 다음 블록 상태 변경과 격리. 잡 배열 가시성은 게이트 release-store ↔ 클레임 acquire RMW 페어링.

## 5.3 설정 영속화
- `AudioDeviceManager` 상태를 `%APPDATA%/Superrack/audio-settings.xml` 에 저장/복원. 시작 시 `initialise(2,2, savedState, true)` 로 복원, `ChangeListener` 로 설정 변경마다 + 종료 시 저장. → ASIO 장치/SR/버퍼/채널이 다음 실행에 유지됨.
- (예정) 플러그인 체인 세션 저장은 Phase 3.

## 6. 빌드 구성
`juce_add_gui_app`, 컴파일 정의 `JUCE_ASIO=1` / `JUCE_PLUGINHOST_VST3=1` / `JUCE_USE_CURL=0` / `JUCE_WEB_BROWSER=0`. 링크: `juce_audio_devices/processors/formats/utils`, `gui_basics/gui_extra`, recommended config·warning flags. JUCE는 CMake `FetchContent`로 8.0.11 태그 고정.

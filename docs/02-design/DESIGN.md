# DESIGN — ASIO 다채널 VST3/AU 인서트 프로세서 (Superrack)

> PDCA: Design · 개정일: 2026-07-03 (갭 분석 G1~G7 문서 동기화) · Plan: [`../01-plan/PLAN.md`](../01-plan/PLAN.md)
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

### 3.2 ChannelStrip ×N (채널별 플러그인 체인)
- 입력 채널당 1개, 내부 플러그인 직렬 체인(+출력 게인 옵션)
- `AudioPluginFormatManager` 스캔(`KnownPluginList` 캐시)·로드, `prepareToPlay(sr,maxBlock)` 선행
- **호스팅 포맷 (2026-09-05)**: Windows = VST3, macOS = VST3 + **AudioUnit(AUv2)**. 포맷 판별·식별자 비교는 `Source/PluginFormats.*` 한 곳으로 모은다 — VST3 는 *파일 경로*, AU 는 *식별자*(`AudioUnit:Effects/aufx,subt,manu`)로 플러그인을 가리키므로 "경로 가정"을 코드에서 지워야 한다. AUv3(앱 확장)은 비동기 인스턴스화가 필요해 동기 로드 경로와 맞지 않아 스캔에서 제외(`allowAsync=false`).
- 플러그인 추가/제거/순서변경, 플러그인별 Bypass, 상태(`get/setStateInformation`·`.vstpreset`), 네이티브 에디터 창

### 3.3 출력 라우팅
스트립[ch] → ASIO Out[ch] (1:1 wet). 채널별 출력 게인(dB, 옵션). 기본 동일 번호, 오프셋 설정 가능.

### 3.4 MultitrackRecorder (Dry 전용, 실시간 안전)
- 오디오 스레드는 FIFO push만, 디스크 쓰기는 Writer 스레드
- Dry tap = 채널 입력 직후(스트립 전), Wet 녹음 없음
- 32-bit float WAV, 채널당 모노 파일 `chNN_dry.wav` — 캡처는 `.rectmp/` 스크래치, 정지 시 테이크로 커밋(§5.6)
- 전 트랙 arm → 다음 콜백 경계 동시 시작, 세션 타임스탬프 기록
- FIFO 오버플로 시 블록 드롭 + xrun 카운터(오디오 무중단). >4GB 는 JUCE WAV writer 가 RF64 자동 전환(기술검토 확인) — 별도 작업 불필요

### 3.5 ChannelView (GUI)
채널 스트립 ×N(입력 미터·플러그인 슬롯·Bypass·출력 게인·Dry arm), 트랜스포트(녹음 시작/정지·타이머·xrun·CPU 미터), 장치 패널(`AudioDeviceSelectorComponent`), 플러그인 에디터(`DocumentWindow`). GUI → 락프리 명령 큐 → 오디오 스레드.

## 4. 실시간 안전성 체크리스트 (Audio Thread)
- [ ] 동적 할당 없음(버퍼·FIFO 사전 할당)  - [ ] mutex/lock 없음(atomic·락프리 FIFO만)
- [ ] 파일 I/O·로깅·printf 없음  - [ ] 예외 throw 없음
- [ ] 전 플러그인 `prepareToPlay` 선행  - [ ] maxBlock 기준 prepare(가변 블록 대비)
- [ ] `ScopedNoDenormals`  - [ ] in/out 채널 수 정렬(1:1)

## 5. 세션 파일 스키마 (확정 — 2026-07-03 갱신, 구현 `getSessionVar` 기준)
```json
{
  "device": { "name": "Yamaha Steinberg USB ASIO", "sampleRate": 48000.0, "bufferSize": 128 },
  "channels": 2,
  "strips": [
    { "name": "보컬", "outGainDb": 0.0, "input": 1, "output": 1,
      "plugins": [ { "path": "C:/.../EQ.vst3", "uid": 123456, "name": "EQ", "bypass": false, "state": "<base64>" } ] },
    { "name": "", "outGainDb": 0.0, "input": 2, "output": 2, "plugins": [] }
  ]
}
```
- 초안에 있던 `recDry`/`recording` 블록은 폐기 — 녹음은 §5.6 테이크 모델(폴더 자기완결), 저장 위치는 §5.10 앱 설정 소관.
- `uid`/`name` = 머신 이동성 폴백 키 (§5.3).

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
- **파일**: 32-bit float 모노 WAV. `AudioFormatWriterOptions{}.withBitsPerSample(32).withSampleFormat(floatingPoint)` (JUCE 8 신 API — 구 `createWriterFor(bits=32)`는 int32 PCM이라 float 명시 필수). 경로는 `<저장 루트>/.rectmp/chNN_dry.wav` 캡처 → 테이크 커밋(§5.6, 저장 루트는 §5.10).
- **Dry tap 위치**: `AudioEngine` 콜백에서 스트립 처리는 `in→out`(별도 버퍼)이라 `inputChannelData`가 원음 그대로 유지됨 → 콜백 말미에 `recorder.writeBlock(inputChannelData, …)` 한 번. Wet 녹음 없음.
- **start/stop 동기화**: `active` atomic. start는 writers 완성 후 `active=true`(release). stop은 `active=false` 후 30ms 대기(in-flight 콜백 통과)→`writers.clear()`(ThreadedWriter 소멸 시 FIFO 플러시 + WAV 헤더 마감). ChannelStrip::reconfigure와 동일 가드.
- **오버플로**: `write()`가 false면 해당 블록 드롭 + `xruns` 카운터 증가(오디오 무중단). GUI가 경과시간·xrun 폴링.
- **채널 수**: 활성 스트립 수(현 2). 장치 입력 < 스트립 수면 `jmin`으로 가드. 장치 정지/앱 종료 시 자동 stop.
- >4GB: JUCE WAV writer 의 RF64 자동 전환에 의존(기술검토 2026-07-02 소스 확인) — 별도 구현 불요.

## 5.6 영속화 계층 + 테이크 모델
저장 파일을 **역할별로 분리**한다 (4계층 — 앱 설정은 §5.10):
- **설정** `%APPDATA%/Superrack/audio-settings.xml` — ASIO 장치·SR·버퍼 (하드웨어 환경).
- **앱 설정** `%APPDATA%/Superrack/app-settings.json` — 저장 루트·VST3 경로·워커 수 (§5.10).
- **세션** `%APPDATA%/Superrack/session.json`(자동) + `.superrack`(수동) — 플러그인 체인·게인·채널 이름 (작업 구성).
- **테이크** `<저장 루트>/takes/<id>/` — 녹음물(자기완결 프로젝트). 저장 루트 기본값 `<Documents>/Superrack`, §5.10 에서 변경 가능.

**Take** = 하나의 녹음 단위(`TakeManager`):
- `chNN_dry.wav` — 트랙별 32f 모노. 한 테이크 안에서 여러 번 녹음/펀치해도 **트랙당 1파일 제자리 갱신**.
- `timeline.json` — 환경 스냅샷(deviceName·sampleRate·bufferSize·channels) + `lengthSamples` + **history**(각 record/punch 의 `{op,startSample,endSample,at}` = 작업 이력).
- `session.json` — 녹음 시점 **세션 스냅샷**(환경 재현용).
- **새 테이크** = 새 폴더(기존 보존). **녹음**은 현재 테이크에 펀치인 커밋. 폴더는 "녹음마다"가 아니라 "테이크마다".
- **녹음 스크래치**: `<저장 루트>/.rectmp/` — recorder 가 여기 캡처 → 정지 시 `TakeManager::commitRecording` 이 (펀치면 기존 [0..P) + 새 녹음) 을 테이크 파일에 임시파일 스왑으로 커밋.
- **녹음 undo/redo (C, 2026-07-03)**: 커밋이 직전 상태(스템·timeline.json·session.json)를 `*.prev` 로 **렌임 보존**(오디오 복사 없음, 테이크당 1세대 — 디스크 2×). `TakeManager::swapUndoState` 가 현재↔보존 세트를 스왑 — 같은 호출이 undo/redo 토글. 방향 판별은 `isCurrentNewer`(timeline updatedAt 사전순). 첫 녹음 undo = 빈 테이크 복귀. 호출 전 재생기 리더 해제 필수(`AudioEngine::undoRecording` 이 정지→언로드→스왑→재로드+플레이헤드 클램프). GUI: 테이크 행 "⟲ 녹음 복구"/"⟳ 복구 취소" 버튼(재생/녹음 중 비활성).
- **테이크 로드** (콤보 선택): 세션 스냅샷 **자동복원**(`applySessionVar` → 체인/게인/이름) + 환경 불일치(SR ≠ 현재 장치) 시 **경고만**(장치 자동변경 안 함). 시작 시 가장 최근 테이크를 현재 테이크로.
- **UI**: 트랜스포트 아래 테이크 행 = 셀렉터(id·ch·SR·길이) + `＋ 새 테이크`.

## 5.5 구현 노트 (통합 타임라인 녹음/재생) — 2026-07-03 개정
- **트랜스포트 상태기계** (`AudioEngine`): `tsStopped`(라이브 모니터) / `tsRecording`(라이브 모니터+Dry 캡처) / `tsPlaying`(스템 재생). atomic 전환, 콜백이 상태로 분기.
- **`TimelinePlayer`**: 최근 테이크의 `chNN_dry.wav` 스템들을 같은 read 위치로 동기 재생. 스트리밍 내부 구조는 **§5.9 로 대체됨**(v1 의 StemSource+BufferingAudioSource 설계는 우선순위 역전으로 폐기 — SPSC 링+리더 스레드, SR 리샘플 포함).
- **재생 경로 옵션**: `playThroughChain` atomic. on=스템→채널 VST3 체인→출력(플러그인 적용/재처리), off=스템 그대로 출력. **재생 중 라이브 ASIO 입력은 뮤트**.
- **통합 타임라인**: 모든 채널 동일 시작·위치(오프셋 없음). 녹음은 콜백 경계 동시 arm(기존 recorder), 재생은 단일 위치. 끝 도달 시 콜백이 `tsStopped`로 복귀.
- **펀치인 녹음** (v1 의 `*_asm` 조립 폴더 방식 폐기): 녹음 시작 시 플레이헤드 P 기록 → 새 녹음은 `.rectmp/` 스크래치에 캡처 → 정지 시 `TakeManager::commitRecording` 이 채널별 **기존 [0..P) + 새 녹음**을 임시파일 스왑으로 **현재 테이크에 제자리 커밋**(§5.6). P 이후 끝까지 덮어씀 — 펀치아웃/구간 오버덥은 제품 판단으로 보류(BACKLOG C), 실수 복구는 undo/redo(§5.6).
- **트랜스포트 UI**: ⏮/●/▶/■ + `pos/len` 시간 + 진행바(드래그 시크) + "재생 시 플러그인 통과" 토글.
- 장치 재시작 시 테이크 언로드 후 새 버퍼(블록/SR)로 재로드.

## 5.7 구현 노트 (Phase 4 — DSP 부하 실측 + 합성 부하 CPU 프로파일링)
- **DSP 부하 실측** (`AudioEngine`): 콜백 전체를 QPC(`Time::getHighResolutionTicks`)로 감싸 `벽시계 시간 ÷ 버퍼 예산(numSamples/SR)` = load. avg(EMA α=0.05)·peak(CAS 루프)·예산 초과 콜백 수(load>1)를 atomic 으로 표면화. `getCpuUsage()`(평균만)보다 정밀. peak 리셋은 GUI store(0) — 오디오 스레드 CAS 와 경쟁해도 무락.
- **합성 부하** (`synthChannels` atomic, 0=끔): 물리 라우팅(`routed`) 이후 `strips[routed..N)` 을 입력 복제(`in[ch % numIn]`)로 **실제 처리**하고 출력은 사전할당 스크래치(1×maxBlock)에 폐기. UR22mkII(2ch)로도 32ch 체인 부하를 재현. 재생·레이턴시 측정 경로에서는 미적용(조기 return). 합성 채널 체인은 세션에 저장되지 않음(세션은 활성 채널만 직렬화).
- **`CpuProfiler`** (`Source/CpuProfiler.*`, 메시지 스레드 Timer): 시작 시 Ch1 체인을 32채널에 복제(`prepareSyntheticStrips`, 채널별 독립 플러그인 인스턴스) → 단계 {활성,4,8,16,24,32}마다 안정화 1초 + 측정 8초(통계 리셋 후 avg/peak/예산 초과/장치 xrun 델타) → 종료 시 합성 부하 해제 + 체인 제거 + 마크다운 리포트(`<Documents>/Superrack/profiles/cpu-profile-*.md`).
- **안정 판정**: 측정 구간 `xrun 0 && 예산 초과 0 && peak < 0.95`. **한계 채널 수** = 처음부터 연속 안정인 마지막 단계. 장치 정지·트랜스포트 개입 시 자동 중단.
- **GUI**: 툴바 `CPU 프로파일` 버튼(실행 중 `■ 중단` 토글), 진행 상황은 상태 행 라벨, 완료 시 요약 팝업+리포트 경로. perfLabel 은 `DSP avg% (pk %) xrun` 상시 표시(pk≥95% 또는 xrun>0 시 주황).
- (제약) 합성 부하는 단일 오디오 스레드 직렬 처리 기준 — 실제 32ch 장치의 드라이버 오버헤드(채널 I/O 전송)는 포함되지 않으므로 실기 확보 시 재검증 필요.

## 5.8 구현 노트 (D2 + A2 — wait-free 체인 스왑 & 채널 병렬 DSP, 2026-07-02)
- **D2 체인 스왑** (`ChannelStrip`, farbot `RealtimeObject<NodeList, nonRealtimeMutatable>` — ThirdParty/farbot 벤더링, MIT, **fifo 모듈 사용 금지**): 메시지 스레드가 원본 `editList` 수정 후 `publishChain()`(= `nonRealtimeReplace`) 복사 스왑. RT 획득은 wait-free(ScopedAccess), 편집 중에도 dry 갭 없이 직전 체인 유지. 구버전 벡터·노드(shared_ptr) 해제는 항상 메시지 스레드. Bypass 는 공유 Node atomic — 스왑 불필요. 기존 reconfiguring 플래그+CallbackFence 가드는 ChannelStrip 에서 제거(recorder/player 에는 유지).
- **A2 병렬 DSP** (`Source/AudioWorkerPool.*`): 채널=잡 fork-join. 콜백이 잡 배열 게시 → 티켓 카운터(fetch_add) 클레임으로 워커+오디오 스레드 분담 → jobsDone 스핀 조인. 게이트(GATE_CLOSED=2^40) 덕에 블록 경계에서 늦게 깬 워커의 스테일 클레임은 idx≥count 로 자연 폐기 — 이중 실행 불가. 대기 = 짧은 spin→이벤트(2ms 타임아웃 폴백).
- **A3 개정(2026-07-03, 실측)**: 워커 = `TIME_CRITICAL` **만**(MMCSS 등록 시 예약 윈도 스로틀 ~2.3ms 스톨), 워커 수 = **물리코어−3**(잔여 1코어면 저순위 스레드 굶음 → 10~28ms 실행 스톨). 오버라이드 `SUPERRACK_WORKERS`/`SUPERRACK_MMCSS=1`. 잡별 {클레임·종료·실행 스레드} 스파이크 계측 상시 탑재(예산 초과 최악 블록 seqlock 스냅샷 → 프로파일 리포트 "스파이크 상세"). → `measurements/cpu-profile-parallel-2026-07-03.md`
- 적용 경로: 라이브 모니터/녹음, 재생(체인 통과 시), 합성 부하(채널별 스크래치로 분리). GUI 토글 "병렬 DSP ×N"(off=직렬, 프로파일 비교용).
- 안전성 논거: 한 잡=한 스트립(스트립 내부 버퍼 스레드 격리), 클레임:완료 1:1 이라 조인 후 워커가 잡 실행 중일 수 없음 → 다음 블록 상태 변경과 격리. 잡 배열 가시성은 게이트 release-store ↔ 클레임 acquire RMW 페어링.

## 5.9 재생 스트리밍 (B — BufferingAudioSource 교체, 2026-07-03)
- `TimelinePlayer` = 채널 공유 SPSC 링버퍼(`AbstractFifo`, 장치 SR ~3초) + 전용 리더 스레드. JUCE `BufferingAudioSource` 는 백그라운드 스레드가 callbackLock 을 쥔 채 디스크 read(우선순위 역전, 검토 §5) → 폐기.
- 오디오 스레드(`readBlock`)는 FIFO 소비만 — 락·디스크·할당 없음. 언더런 = 무음 + 카운터, 위치는 소비량만큼만 전진(디스크 대기). EOF 는 리더가 표시, 소진 시 위치=total 로 정지 유도.
- 시크 = epoch 핸드셰이크: `setPosition` 이 epoch++ → 오디오는 ackEpoch 기록 후 validEpoch 될 때까지 무음(소비 정지) → 리더가 ack(또는 30ms 타임아웃, 정지 상태) 확인 후 FIFO 리셋·리필 → validEpoch 발행. setPosition 은 리필 완료를 ≤50ms 대기해 재생 시작이 무음 없이 붙는다.
- SR 불일치(B-2): 스템 SR ≠ 장치 SR 이면 리더에서 `LagrangeInterpolator` 로 장치 SR 리샘플. 위치/길이/getSampleRate 는 전부 장치 샘플 도메인. 같은 SR 이면 1:1 직행(비트 동일).
- 메시지↔리더 스레드는 configMutex 로 직렬화(오디오 스레드는 이 뮤텍스에 접근하지 않음).

## 5.3 설정 영속화
- `AudioDeviceManager` 상태를 `%APPDATA%/Superrack/audio-settings.xml` 에 저장/복원. 시작 시 `initialise(2,2, savedState, true)` 로 복원, `ChangeListener` 로 설정 변경마다 + 종료 시 저장. → ASIO 장치/SR/버퍼/채널이 다음 실행에 유지됨.
- **세션 머신 이동성 (2026-07-03)**: 플러그인 항목을 `{path, uid, name, bypass, state}` 로 저장 (uid = `PluginDescription::uniqueId`). 로드 시 경로 실패 → 폴백: ① 표준 VST3 위치에서 같은 파일명 검색(+uid 검증) ② uid 로 전체 스캔(scanCache 로 중복 제거). 성공 시 노드의 filePath 를 로컬 경로로 **자기치유** — 다음 저장부터 이 머신 경로가 기록됨. uid 없는 구버전 세션은 파일명 폴백만 적용(하위 호환). base64 state 는 플러그인 내부 포맷이라 머신 무관.
- **AU 세션 복원 (2026-09-05)**: `path` 자리에 AU 식별자가 들어간다 — 파일 경로가 아니므로 `juce::File` 로 감싸면 안 된다(`sr::plugins::sameTarget` 이 식별자/경로를 구분). AU 는 식별자·uid 모두 머신 독립(uid = componentType^SubType^Manufacturer)이라 경로 재탐색이 필요 없고, 식별자가 죽었을 때만 같은 uid 의 AudioComponent 를 찾는다. 이때 uid 를 **식별자 문자열에서 계산**하므로(`sr::plugins::audioUnitUid`) 후보를 고를 때까지 AU 를 하나도 로드하지 않는다 — VST3 의 ② 전체 스캔과 달리 비용이 사실상 0.

## 5.10 앱 설정 (H — 범용화, 2026-07-03)
- `AppSettings` 싱글턴 (`Source/AppSettings.*`), `%APPDATA%/Superrack/app-settings.json`, 세터 즉시 저장. 모든 접근 메시지 스레드.
- 항목: ① **storageRoot** — takes/.rectmp/profiles 루트 (기본 `<Documents>/Superrack`; OneDrive 문서 리다이렉트 회피용, 변경 시 새 테이크부터 적용) ② **vst3ExtraPaths** — 세션 폴백 재탐색(§5.3)·카탈로그 스캔에서 표준 위치보다 우선 검색 (AU 는 시스템 등록 기준이라 경로 추가 개념이 없다 — 설정 창은 표준 Components 위치를 안내 문구로만 표시) ③ **workerCount** — 0=자동(물리코어−3, §5.8), 1~6 수동, 재시작 적용.
- 우선순위: 환경변수(진단: `SUPERRACK_WORKERS`/`SUPERRACK_MMCSS`/`SUPERRACK_APPDATA`) > 앱 설정 > 자동. `SUPERRACK_APPDATA` 는 설정 폴더 오버라이드(테스트 격리·포터블).
- GUI: ⚙ 설정 창 하단 패널 — 저장 위치(변경/기본값), VST3 경로(기본 위치 회색 고정 표시 + 사용자 멀티라인), 워커 수 콤보.

## 5.11 타임라인 뷰 (L — 3레인 재정비, 2026-08-14)

**문제.** 진행바 한 줄에 성격이 다른 셋이 같은 bounds 로 겹쳐 있었다 — `positionSlider`(위치/시크), `PunchStripOverlay`(녹음 이력), `LoopRangeStrip`(반복 구간). `LoopRangeStrip::hitTest` 로 위 밴드만 마우스를 받아 공존시켰지만, 셋이 한 자리를 경쟁해 읽기 어렵다.

**더 나쁜 것은 이력 표시가 거짓이었다는 점이다.** 펀치인은 펀치 지점부터 **끝까지 덮어쓰는데**(§5.6 `commitRecording`) 오버레이는 `history` 항목을 날것으로 그렸다. 30초 테이크의 10초에서 5초를 펀치인하면 결과는 15초인데, 회색 막대는 0~30초로 남아 이미 지워진 구간이 현재 오디오처럼 보인다.

**유효 구간 스위프** (`Source/TimelineSegments.h`, 순수 함수 `computeEffectiveSegments`). history 를 그리지 않고, 덮어쓰기를 적용해 **지금 들리는 오디오의 구간 지도**를 계산한다:

```
segs = []
for (gen, h) in history:              // 커밋 순
    segs 에서 h.start 이상을 잘라낸다   // 펀치는 그 지점 뒤를 전부 지운다
    segs.push({ h.start, h.end, gen })
```

결과는 시간 오름차순·무겹침이며 각 구간은 자기가 몇 회차 녹음에서 왔는지(`gen`)를 안다 — DAW 의 클립 레인과 같은 것이다. 회색은 사라지는 게 아니라 **비로소 사실이 된다.** 헤더 전용(juce_core 만 의존)이라 gui 없는 L1 타깃에서 검증한다.

**3레인 구성** (`TimelineView`, `preferredHeight` 50px). 버튼과 한 줄을 나눠 쓰던 자리를 떠나 **전체 폭 전용 행**이 된다 — 좁은 잔여 폭에서는 3레인이 읽히지 않고, 폭이 두 배가 되면서 시간 해상도도 그만큼 올라간다. 트랜스포트 버튼 행(30px)은 위에 그대로 남는다. 레인 비율의 원칙: **루프는 잡는 곳이라 손가락 폭이 필요하고, 클립은 보는 곳이라 얇아도 된다** (2026-08-14 실기 피드백으로 조정).

| 레인 | 높이 | 내용 | 마우스 |
|:--|--:|:--|:--|
| 룰러 | 14 | 눈금 + `m:ss` 라벨 | 클릭·드래그 = 시크 |
| 루프 | 14 | 반복 구간 — 양 끝에 **직각 삼각형 로케이터**(DAW 관행: 수직변 = 경계선, 윗변이 구간 안쪽으로) + 잇는 바 | 드래그 = 구간 지정, 삼각형 = 조정, 더블클릭 = 해제 |
| 클립 | 18 | 유효 구간 — 마지막 `gen` 살짝 진하게, 잔존 구간 옅은 회색, 회차 경계선 | 클릭·드래그 = 시크 |

커서 머리는 룰러 **라벨 하단(눈금 시작선)** 에 맞춘다 — 숫자를 뚫고 올라가면 룰러가 읽기 어렵다.

**색은 상태를 나르지 않는다 — 커서가 나른다.** (2026-08-14 제품 결정)
- `positionSlider`(LinearBar) **폐기.** 왼쪽이 채워지는 막대는 "채워진 부분이 오디오"로 읽혀 클립 레인과 정면 충돌한다. 진행 표시는 세 레인을 관통하는 **세로 커서 하나**뿐이고, 채우기는 없다.
- **시크 = 커서 끌기.** 룰러/클립 어디를 눌러도 커서가 그 지점으로 와서 잡힌다. `TimelineView` 가 직접 처리 — 드래그 중에는 표시만 갱신하고 놓을 때 `setPlayPositionSeconds`(기존 `seeking` 플래그 규약으로 타이머 역갱신 억제).
- **루프의 활성/비활성 색 구분 제거.** 반복이 켜졌는지는 `↺` 버튼 토글이 이미 말한다. 스트립을 초록/회색으로 또 말하면 같은 상태를 두 곳에서 다른 언어로 말하는 셈이다. 스트립은 단일 색, 구간의 **위치**만 표시한다.
- 클립의 회차 구분은 상태가 아니라 **정보**(이 소리가 어느 녹음에서 왔는가)라 남기되, 채도를 낮춰 배경으로 물린다 — 커서가 화면의 유일한 강조가 되도록.
- 히트 영역이 레인으로 나뉘므로 `LoopRangeStrip::hitTest` 의 밴드 트릭은 불필요해진다. 루프 드래그와 시크가 서로 다른 레인이라 충돌이 없다.
- 눈금 간격은 폭/길이에 맞춰 {0.5, 1, 2, 5, 10, 15, 30, 60, 120}초 중 라벨 간격 ≥ 64px 인 최소값.
- 툴팁: 클립 세그먼트 = `N회차 · mm:ss~mm:ss · 녹음 시각`, 루프 = 구간과 길이.

## 5.12 채널 M/S/R + 로케이터 펀치 녹음 (2026-08-15 확정)

**모델 요약.** DAW 트랙 모델(M/S/R) + 로케이터 모델(구간 하나가 반복 재생과 펀치 녹음을 겸용)을 채택한다.

```
채널 행:  [이름] [게인] [미터] [M][S][R] [B][E][X]
타임라인: 구간 스트립 = 반복 재생 + 펀치 로케이터 겸용
↺ 반복    = 재생이 구간을 반복할지        (재생 제어)
⏺ 구간녹음 = 녹음이 구간을 펀치로 쓸지     (녹음 제어)
```

같은 구간을 두 버튼이 서로 다른 동사로 쓴다 — "구간을 그린다 = 이 구간을 작업하겠다"는 의도 표현이고, 어느 동사로 쓸지는 버튼이 말한다(§5.11 "같은 상태를 두 곳에서 말하지 않는다"와 일관).

**M(뮤트)/S(솔로) — 재생 스템 전용.** 라이브 입력 경로는 불가침(라이브 인서트가 본업, 재생은 리허설 도구 — 공연 중 오클릭으로 라이브 채널이 죽는 사고를 원천 차단). 솔로는 가산식: 하나라도 켜지면 솔로 아닌 채널은 뮤트 취급. 가청 = `!muted && (soloMask==0 || soloed)`. 오디오 스레드는 atomic 마스크만 읽는다.

**R(녹음 암).** 다음 녹음/펀치가 덮어쓸 채널. 기본 전 채널 armed(현행 동작 보존 — R 을 안 건드리면 아무 변화 없음). 전부 unarmed 에서 ● → 녹음 거부 + 안내. **레코더는 무수정** — 오디오 스레드는 지금처럼 전 채널을 rectmp 에 쓰고, 커밋이 armed 채널 파일만 머지한다(RT 경로 불변, rectmp 는 커밋 후 삭제라 낭비는 일시적).

**M/R 직교.** 녹음은 dry 캡처(게인·체인 무관)라 뮤트된 채널도 armed 면 정상 녹음된다.

**펀치 패스(⏺ 구간녹음 on + 구간 존재) — 2026-08-15 개정.** 처음 구현(in 에서 시작·out 에서 정지)은 연주자가 맥락 없이 구간에 내던져졌다. 펀치의 본질은 **재생이 주도하고 녹음은 그 안의 창(window)** 이라는 것:

```
커서 ▶── 프리롤(재생, 부르며 들어옴) ──┤in├─ 녹음 창 ─┤out├── 포스트롤(재생 계속) ── ■ 커밋
```

- ● = 정지가 아니라 **커서 위치에서 재생 시작** (`tsPunchPass` — 재생+캡처 동시 상태 신설).
- 캡처는 패스 시작부터 전부 스크래치에 받고, **커밋이 `srcOffset`(프리롤)을 건너뛰고 `[in,out)` 창만 절단**해 머지 — 실시간 스위칭 타이밍에 경계가 걸리지 않아 샘플 정확.
- out 을 지나도 재생은 계속 — ■(또는 스페이스)로 끝낼 때 커밋. 테이크 끝 도달 시 GUI 타이머가 정지(콜백은 커밋을 못 하므로 `shouldAutoStopRecording` 폴링).
- **input echo**: 구간 안에서 armed 채널은 스템 대신 **라이브 입력**을 모니터링(체인 통과 — 라이브 모드와 동일). 전환은 블록 단위(모니터링용이라 충분 — 녹음 경계는 커밋이 정확히 자름). 구간 밖에서는 기존 스템이 들린다.
- 엣지: 커서가 구간 안 → 부분 펀치(`[커서,out)` 만 대체) / 커서가 out 뒤 → 거부 / in 도달 전 정지 → 커밋 없음 / 창 중간 정지 → `[in,정지점)` 만 대체 / 패스 중 ↺·시크·되감기 잠금(캡처-타임라인 정렬 보호).

**길이 불변식: 늘어남만 허용, 줄어듦 금지.** 모든 커밋이 꼬리를 보존한다:

```
armed 채널   = old[0..in) + new[0..N) + old[in+N..oldEnd)   (N = 녹음 길이, 펀치면 out−in 절단)
unarmed 채널 = 파일 불가침
테이크 길이  = max(전 채널 파일 길이)                        → 절대 줄지 않음
```

- 종전 "펀치는 그 지점부터 끝까지 덮어쓴다"(§5.6)는 **이 절로 대체**된다. 백로그 C 의 펀치아웃 보류 판정은 머지 엔진(head 복사+이어붙임+.prev 스왑)이 이미 있는 지금은 근거가 소멸 — 꼬리 복사 루프 하나 추가가 전부다.
- 의도적으로 짧은 테이크를 원하면 **새 테이크**로 시작한다(그게 의미상으로도 맞다).
- armed 부분집합 녹음으로 스템 길이가 갈라져도 플레이어는 무수정 — `totalSamples = max(스템 길이)` 이고 `AudioFormatReader` 가 EOF 이후를 0 으로 채워 짧은 스템은 자동 무음 패딩된다.

**history 의미 개정.** 항목 = **대체된 구간** `[startSample, endSample)` + `channels` 배열(빈/부재 = 전 채널, 구버전 호환). §5.11 의 유효 구간 스위프는 "start 이후 전부 잘림"에서 **구간 절단**(스팬 구간을 셋으로 분할)으로 바뀐다. 구버전 테이크는 커밋마다 길이가 절단됐으므로, 스위프 결과를 `lengthSamples` 로 클램프하면 옛 의미와 정확히 일치한다(별도 마이그레이션 불필요). 클립 레인은 1차로 채널 union 표시 + 툴팁에 채널 명기.

**undo.** 커밋 시작 시 테이크의 **모든 `*.prev` 를 먼저 삭제**한 뒤 이번에 만진(armed) 파일만 `.prev` 로 보존 — 이전 커밋의 stale `.prev` 가 남아 있으면 부분 채널 undo 가 회차가 섞인 상태를 복원하게 된다. `swapUndoState` 는 존재하는 `.prev` 만 스왑하므로 무수정.

**영속화.** M/S/R = 세션 strips[] 항목의 `mute`/`solo`/`arm`, ⏺ = 세션 `loop.punchRecord` (믹서 상태의 일부 — 테이크에 넣으면 전환마다 리셋돼 어색).

## 6. 빌드 구성
`juce_add_gui_app`, 컴파일 정의 `JUCE_ASIO=1`(Windows) / `JUCE_PLUGINHOST_VST3=1` / `JUCE_PLUGINHOST_AU=1`(macOS, `superrack_enable_au()`) / `JUCE_USE_CURL=0` / `JUCE_WEB_BROWSER=0`. macOS 는 AU 에디터 폴백(`AUGenericView`)이 CoreAudioKit 에 있는데 JUCE 모듈이 이 프레임워크를 선언하지 않아 CMake 에서 직접 링크한다. 링크: `juce_audio_devices/processors/formats/utils`, `gui_basics/gui_extra`, recommended config·warning flags. JUCE는 CMake `FetchContent`로 **8.0.14** 태그 고정(2026-07-03 업그레이드, VST3 SDK 3.8.0).
- **테스트 타깃** `SuperrackTests` (`juce_add_console_app`, `Tests/TestMain.cpp`): 핵심 경로 L1 회귀(녹음 FIFO/커밋·펀치·undo/재생 스트리밍·리샘플/병렬==직렬/설정) — ASIO·플러그인 불필요, 실패 수=종료 코드. → `docs/05-qa/asio-vst3-host.qa-report.md`

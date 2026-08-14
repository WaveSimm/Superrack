# 전체 구조 분석 + 최적화 방안 (2026-08-15)

> 대상: `Source/` 7,267줄 (헤더+구현 30파일) + `Tests/TestMain.cpp` (L1 159 어서션)
> 관련: [`02-design/DESIGN.md`](../02-design/DESIGN.md) · [`BACKLOG.md`](../BACKLOG.md)

## 1. 모듈 지도

| 모듈 | 줄수(h+cpp) | 역할 | 스레드 |
|:--|--:|:--|:--|
| `AudioEngine` | 267+1084 | 장치/콜백 허브, 트랜스포트 상태 머신(정지/녹음/재생/펀치패스), 세션 영속화, 레이턴시 테스트, M/S/R 마스크 | 오디오+메시지 |
| `ChannelStrip` ×32 | 147+367 | 채널별 VST3 직렬 체인. farbot `RealtimeObject` 로 wait-free 체인 스왑(D2) | 오디오/워커+메시지 |
| `AudioWorkerPool` | 151+326 | 채널=잡 fork-join. 티켓 카운터+게이트, spin→event, A3 스파이크 계측 | 워커 N+오디오 |
| `TimelinePlayer` | 122+369 | 스템 재생: 공유 SPSC 링(~3초)+전용 리더, Lagrange 리샘플, epoch 시크, 구간반복 되감기 | 오디오+리더+메시지 |
| `MultitrackRecorder` | 76+153 | Dry 캡처: 채널별 `ThreadedWriter`(락프리 FIFO→디스크) | 오디오+Writer |
| `TakeManager` | 91+331 | 테이크 영속화: 구간 대체 커밋(꼬리 보존·절단·armed), .prev undo 스왑 | 메시지 |
| `TimelineSegments` | 107(h) | 유효 구간 스위프(순수 함수) — history→클립 지도 | 메시지 |
| `MainComponent` | 219+1044 | 메인 GUI: 툴바/트랜스포트/타임라인/테이크/채널 랙 + AppSettingsPanel + SettingsWindow | 메시지 |
| `TimelineView` | 101+330 | 3레인 타임라인(룰러/루프/클립), 커서 시크 | 메시지 |
| `ChannelRow`+`PluginChip` | 84+283 | 채널 행: 이름/M/S/R/미터/게인/칩 | 메시지 |
| `PluginCatalog` | 60+184 | KnownPluginList + out-of-process 스캔 + 블랙리스트 | 메시지(+워커 프로세스) |
| `PluginBrowser` | 53+292 | 카탈로그 브라우저 UI | 메시지 |
| `CpuProfiler` | 88+285 | 합성 부하 단계 측정 → 마크다운 리포트 | 메시지(Timer) |
| `AppSettings` / `BetaGate` / `Util` | 42+84 / 42+130 / 49 | 앱 설정 / 베타 만료 / u8·CallbackFence | 메시지 |

## 2. 스레드/데이터 흐름

```
[CoreAudio/ASIO IO 스레드]                          [워커 ×(P코어−2)]
  audioDeviceIOCallbackWithContext                    workerLoop: 티켓 클레임
    ├─ 재생/펀치패스: player.readBlock ──────┐          → strip.process()
    │    스템(M/S 게이팅) or 라이브(echo) ────┼─ jobScratch → workerPool.processJobs (조인)
    │    recorder.writeBlock (펀치패스)      │
    ├─ 라이브: in → strip.process → out ─────┘
    │    recorder.writeBlock (dry tap, 녹음중만 실효)
    └─ DSP 부하 실측 (QPC)

[리더 스레드]  TimelinePlayer::readerLoop — prefetch/시크/리샘플/루프 되감기 → SPSC 링
[Writer 스레드] TimeSliceThread — 채널별 FIFO → chNN_dry.wav
[메시지 스레드] GUI 30Hz 틱(updateTransportUI, 펀치 자동정지 폴링), 커밋/undo(TakeManager),
               세션 JSON, 플러그인 편집(publishChain 스왑)
[스캔 워커]    별도 프로세스 "--scan-file" (크래시 격리)
```

**영속화 3계층**: `audio-settings.xml`(장치) / `app-settings.json`(앱 설정) / `session.json`(체인·게인·M/S/R·루프) — 테이크 폴더엔 timeline.json + session.json 스냅샷 + 스템.

**RT 규율 요약** (전반적으로 우수): 콜백 무할당·무락 일관, farbot wait-free 체인, CallbackFence(R1) 세대 가드, SPSC+epoch 시크, ThreadedWriter 락프리, atomic 마스크(M/S/R·펀치 창). 스캔은 프로세스 격리. 커밋/undo 는 렌임 기반이라 오디오와 완전 분리.

## 3. 최적화 방안 (우선순위별)

### P1 — 디스크/커밋 I/O (가장 큰 실효 레버)

**(a) 커밋의 전체 파일 재작성.** `commitRecording` 은 armed 채널마다 기존 파일 전체를 읽어 새로 쓴다(head+창+tail). 32ch × 10분 × 48k/32f ≈ 채널당 110MB → 커밋 1회에 수 GB I/O. 5초 펀치라도 비용이 테이크 길이에 비례한다.
→ **in-place 구간 패치**: WAV 32f 는 고정 샘플 크기이므로 `[in,out)` 만 seek+overwrite 가능. 길이가 안 변하는 커밋(펀치 패스의 대부분)은 O(창 크기)가 된다. 전제: `.prev` 렌임 undo 와 충돌하므로 undo 전략을 "덮어쓸 구간만 별도 파일로 백업 → undo 시 되돌려 쓰기"로 바꿔야 한다(디스크도 2×→창 크기로 절약). 길이가 늘어나는 커밋만 기존 재작성 경로 유지. **중규모 작업, L1 로 검증 가능.**

**(b) 세션 자동저장 디바운스.** `notifySessionChanged` → `autoSaveSession` 이 게인 드래그 끝·이름·바이패스·M/S/R 클릭·루프 드래그마다 **동기** 실행되는데, `getSessionVar` 는 전 채널 플러그인의 `getStateInformation`(base64 상태 덤프)까지 부른다 — 플러그인 상태가 크면 클릭 한 번에 수십 ms 멈춤.
→ 1초 코얼레싱 타이머로 묶고, 종료/커밋 시엔 즉시 플러시. **소규모, 체감 큼.**

**(c) `listTakes` 선형 파싱.** 테이크 목록 갱신마다 전 테이크 폴더의 timeline.json 파싱. 테이크 수십 개까진 무해하나 수백이면 기동·refreshTakes 가 밀린다. → dir mtime 기반 캐시. **소규모, 당장은 낮은 우선순위.**

### P2 — 기동 시간

**(d) 세션 복원의 파일 스캔.** 복원은 `ChannelStrip::scanPath`(=`findAllTypesForFile`) 경유 — WaveShell 은 파일 스캔에 수 초. BACKLOG J 조사에서 "카탈로그 우선 조회 = 기동 10.4→5초, 오로드 현상과는 독립"이 이미 확인돼 있다.
→ makeNode 가 `PluginCatalog` 를 먼저 조회하고 실패 시에만 파일 스캔으로 폴백. **J(오로드) 규명과 별개로 안전하게 재도입 가능** — 단 J 의 "인덱스 vs cid" 패치와 같은 차수로 묶으면 검증이 한 번에 끝난다.

### P3 — GUI 30Hz 틱

**(e) TimelineView 전체 repaint.** 커서가 움직일 때마다 뷰 전체를 다시 그린다(룰러 라벨·클립·루프 포함). VNC 환경에서 체감 비용. → 커서 이전/새 위치 rect 만 `repaint(Rectangle)` 부분 무효화. **소규모.**

**(f) 틱당 문자열 할당.** timeLabel/perfLabel 이 매 틱 `String::formatted` — 값이 안 변해도 할당 발생. 이전 값 비교 후 setText. **미세, (e)와 같이 처리.**

### P4 — 구조 (성능 아님, 유지보수)

**(g) MainComponent 1,263줄 분해**: AppSettingsPanel·SettingsWindow 를 별도 파일로, 트랜스포트/테이크 패널 분리. 지금 파일 하나에 창 3개+패널 2개.
**(h) AudioEngine 1,351줄**: 트랜스포트 상태 머신(4상태 × 전이 규칙이 transportRecord/Play/Stop/startPunchPass 에 흩어짐)을 명시적으로 분리할 시점 — 펀치 패스 추가로 전이 조합이 늘었다.
**(i) 채널 파일명 파싱 중복**: `"ch"+번호+"_dry.wav"` 파싱이 TakeManager 3곳+엔진에 산재 → `stemFileName(ch)`/`stemIndexOf(name)` 헬퍼로.

### 분석 중 발견한 정확성 관찰 (최적화 아님)

- **(j) 펀치 패스 + SR 불일치**: 테이크 SR ≠ 장치 SR 이면 커밋이 장치 SR 캡처를 파일 SR 기존 오디오에 이어붙인다(기존 punch 도 동일한 한계 — 경고문만 있음). 구간녹음 시작 시 SR 불일치면 **거부**가 안전하다. → BACKLOG 반영 권장.
- **(k) `getRecordedSeconds` 는 FIFO 드롭 블록도 카운트** — xrun 발생 시 커밋 창 계산이 실제 파일과 어긋날 수 있음(기존부터 존재, xrun 0 이 전제인 운용에선 무해).

## 4. DSP 자체는? (하지 않을 것)

실측(mac-load-test)이 이미 말해준다: 병렬 효율 89%, 스톨은 스케줄링이 아니라 플러그인 비용(채널당 0.164ms). 앱 쪽 DSP 경로엔 짜낼 게 거의 없다 — 남은 것은 **측정 과제**뿐: time-constraint 정책의 peak/avg 지터(1.6) 개선 효과가 미확인이므로 A/B 실측이 먼저다. `ChannelStrip::process` 의 모노→스테레오 복제는 플러그인 호환성 대가라 유지.

## 5. 추천 실행 순서

1. **커밋 정리 먼저** — 미커밋 작업(3레인 타임라인+단축키 / M/S/R+펀치 패스)을 2 커밋으로.
2. 단기 가성비: **(b) 자동저장 디바운스 → (e)(f) repaint/문자열 → (j) SR 가드**.
3. 중기: **(a) in-place 커밋 패치**(+구간 백업 undo — L1 확장), (d) 카탈로그 우선 조회(J 패치와 같은 차수).
4. 구조: (g)(h)(i) — 다음 기능 확장(오버덥 모니터링 P2) 전에 트랜스포트 상태 머신 정리가 특히 유효.
5. 측정: time-constraint A/B, 지터 리포트 항목화.

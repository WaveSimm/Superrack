# BACKLOG — Superrack 다음 작업

> 개정: 2026-07-02 · 우선순위: P0(높음)~P3(낮음) · 관련: [`01-plan/PLAN.md`](01-plan/PLAN.md) [`02-design/DESIGN.md`](02-design/DESIGN.md)

## 로드맵 위치
P0~P2 완료(실기), P3 견고화 + 통합 타임라인 녹음/재생 + 테이크 모델 완료(실기). **다음 마일스톤: P4-32ch-scale.**

## 다음 작업

### A. P4 — 32채널 확장 + CPU 프로파일링  [P0] — **완료(2026-07-02, 실기 측정)**
- 프로파일러 구현(DESIGN §5.7) + 실기 측정 완료 → [`measurements/cpu-profile-2026-07-02.md`](measurements/cpu-profile-2026-07-02.md).
- 결과(96kHz/96smp): 경량 체인(Pro-Q 3) **32ch 안정**, 무거운 체인(Pro-Q 3→Pro-R) **한계 4ch**(부하 채널 선형).
- 잔여: 다채널 ASIO 장치 확보 시 실기 확장 검증(합성 부하는 드라이버 채널 I/O 오버헤드 미포함).

### A2. 채널 병렬 워커 풀  [P1] — **완전 종결(2026-07-03)** · 스톨 이슈는 A3 로 분리·해결
- 직렬/병렬 완전 A/B(96k/512 3단): **스피드업 3.0×** @5레인(효율 62%), 한계 4ch→16ch. 48k/256 Q3→R 는 16ch→32ch 여유. → [`measurements/cpu-profile-parallel-2026-07-03.md`](measurements/cpu-profile-parallel-2026-07-03.md)
- 부하 중 체인 편집(D2 wait-free 스왑) 글리치 없음 실기 확인(2026-07-03).
- 설계 레퍼런스: tracktion_graph `LockFreeMultiThreadedNodePlayer`(참고만, GPL). 대기 = spin(backoff)→event. Windows: `TIME_CRITICAL`+MMCSS "Pro Audio", 워커 = 물리코어−2. → [`research/tech-stack-review-2026-07-02.md`](research/tech-stack-review-2026-07-02.md) §3

### A3. 병렬 모드 간헐 스톨 조사  [P1] — **해결(2026-07-03, 계측 + 3실험)**
- 원인 2계층 확정: ① **메가 스톨(10~28ms) = 코어 포화**(워커4+오디오=RT 5스레드/6코어 → 잔여 1코어 저순위 굶음, MMCSS 무관) ② **~2.3ms 주기 스톨 = 워커 MMCSS 스로틀링**(예약 윈도 20%/10ms 와 일치). 동기화 로직(게이트/티켓/조인)은 결백. → [`measurements/cpu-profile-parallel-2026-07-03.md`](measurements/cpu-profile-parallel-2026-07-03.md) A3 절
- **기본값 개정**: 워커 = 물리코어 **−3** + MMCSS **미등록**(TIME_CRITICAL 만). 오버라이드 `SUPERRACK_WORKERS` / `SUPERRACK_MMCSS=1`. 96k/96 Q3→R 한계 8ch(직렬 2×), 최악 블록 12.4→1.3ms.
- 부산물: 잡별 스파이크 계측(리포트 "스파이크 상세") 상시 탑재 — 향후 스톨 재발 시 즉시 판별 가능.

### B. 재생 견고화  [P1] — **완료(2026-07-03, 실기 검증)**
- ~~BufferingAudioSource 교체~~: TimelinePlayer 를 채널 공유 SPSC 링(AbstractFifo, ~3초) + 전용 리더 스레드(prefetch)로 재작성. 오디오 스레드는 소비만(무락·무디스크), 언더런 무음+카운터(getUnderrunCount). 시크 = epoch 핸드셰이크(오디오 ack → 리셋·리필, 리필 완료까지 setPosition 이 ≤50ms 대기 → 시작 무음 없음).
- ~~스템 SR ≠ 장치 SR~~: 리더 스레드에서 LagrangeInterpolator 리샘플 — 피치/길이 정상. 위치·길이는 장치 샘플 도메인 통일. 테이크 선택 경고 문구를 리샘플 안내로 갱신(펀치인은 여전히 SR 일치 권장).
- 실기 검증(2026-07-03): 재생 시작 무음 없음 / 정지·재생중 시크 클릭 없음 / 96k 테이크→48k 장치 리샘플 피치 정상 / 경고 문구 확인. 전부 PASS.

### D2. farbot RealtimeObject 체인 스왑  [P1] — **구현 완료(2026-07-02)** DESIGN §5.8
- `RealtimeObject<Chain, nonRealtimeMutatable>` 헤더 벤더링(MIT, Tracktion 프로덕션 검증) — reconfiguring 플래그+Fence 수제 패턴을 wait-free 획득 + 비RT 지연 삭제로 대체. **farbot fifo 모듈은 레이스 리포트 있음 — RealtimeObject 만 사용.** 검토 §2
- A2 병렬화의 전제 조건.

### D3. 하우스키핑(기술검토 소과제)  [P3]
- ~~CpuProfiler 리포트에 병렬 DSP on/off + 워커 수 기록~~ — 완료(2026-07-03, A3 계측과 함께).
- ~~JUCE 8.0.11 → 8.0.14 업그레이드~~ — **완료(2026-07-03, 실기 검증)**: GIT_TAG 변경 + `createEditorIfNeeded`→`createEditorAndMakeActive`(deprecated 정리). ASIO/체인/에디터 창/녹음·재생 실기 확인.
- ThreadedWriter 튜닝: 채널당 FIFO 수 초(총 ~25-60MB), `setFlushInterval()` 주기 flush(크래시 복구성). RF64 는 JUCE 자동 — 작업 불필요.
- CLAP 호스팅 **보류**(메이저 벤더 CLAP 출시 or JUCE 공식 호스팅 지원 시 재검토).

### C. 녹음 실수 복구 + 펀치 워크플로  [P1→재편(2026-07-03, 제품 판단)]
- **녹음 undo/redo — 완료(2026-07-03, 실기 검증)**: 커밋이 직전 상태를 `*.prev` 로 보존(렌임 — 복사 없음, 테이크당 1세대·디스크 2×), "⟲ 녹음 복구"/"⟳ 복구 취소" 버튼으로 스왑 토글. 스템+timeline+session 세트 스왑, 첫 녹음 undo 는 빈 테이크로 복귀. 재녹음→복구→복구취소 왕복 실기 확인.
- 펀치인/아웃(구간 오버덥)·구간 편집: **보류** — 라이브 인서트+dry 캡처 용도에 불필요(스튜디오 DAW 영역), 실요구 발생 시 부활.
- 녹음 중 기존 `[0..P)` 프리롤 재생 모니터링 [P2 로 유지].

### D. 오디오 스레드 견고화(설계 R1/R2/R3)  [P2] — **완료(2026-07-02 리팩토링)**
- ~~R1~~ `sr::CallbackFence`(콜백 세대 카운터) 로 in-flight 종료 확인 — ChannelStrip/recorder/player 공통 적용. DESIGN §5.1.
- ~~R2~~ 협상 초과 블록 dry 패스스루 방어. ~~R3~~ `PluginScanCache` 경로당 1회 스캔.
- 추가: GUI 구조 분리(ChannelRow.*, UiLookAndFeel.h), peak 스캔 SIMD화. `git init` + 스냅샷/리팩토링 커밋(G 일부 완료).

### E. 테이크/세션 고도화  [P2]
- ~~세션 플러그인 머신 이동성~~ — **완료(2026-07-03)**: 저장에 uid/name 추가, 로드 시 경로 실패 → 파일명/UID 폴백 + 로컬 경로 자기치유. 가짜 경로 세션으로 실검증(6/6 복원). DESIGN §5.3.
- ~~테이크 목록 UI 고도화~~ — **완료(2026-07-03, 실기 확인)**: 정렬 updatedAt 최신순 + 라벨 "녹음 N회", 진행바 하단 녹음/펀치 구간 밴드 오버레이(`PunchStripOverlay`, 마지막 커밋 주황 강조, 마우스 통과, 테이크 변경 시에만 갱신).
- (선택) 테이크 ↔ 세션 종속 모드: `.superrack` 저장/로드 시 연결 테이크도 함께.
- 세션 파일에 테이크 참조 저장 여부 결정.

### H. 앱 설정 — 범용화(다른 머신/사용자)  [P1] — **1차 완료(2026-07-03, 실기 확인)**
- `AppSettings`(`%APPDATA%/Superrack/app-settings.json`) 신설 + 설정 창 하단 패널: ① **녹음/테이크 저장 위치**(takes/.rectmp/profiles 루트 — OneDrive 문서 리다이렉트 회피용, 변경 시 새 테이크부터) ② **VST3 검색 경로**(기본 위치 회색 고정 표시 + 사용자 추가 멀티라인, 폴백 재탐색에서 우선 검색) ③ **병렬 워커 수**(자동/1~6, 재시작 적용).
- 우선순위: 환경변수(진단) > 앱 설정 > 자동. ~~언더런 카운터 표면화~~ 완료(2026-07-03, 재생 중 상태 라벨). 설정 항목 추가는 실요구 발생 시.

### I. 베타 배포 게이트  [P2] — **완료(2026-07-03, 만료 실검증)**
- `SuperrackBeta` 타깃(EXCLUDE_FROM_ALL, `SUPERRACK_BETA=1`/`_DAYS=15`) + `BetaGate`: 머신별 최초 실행일+15일 만료. `.beta-state`+HKCU 이중 저장, 체크섬 난독 인코딩, 시계 되돌림 감지. 창 제목 "남은 기간 N일", 만료 시 안내 후 종료.
- 검증: `SUPERRACK_BETA_TODAY` 훅으로 day0 실행/day15 차단 실기 확인 + `testBetaGate` 단위(6). DRM 아님(관리용) 명시. → [`DEPLOY.md`](DEPLOY.md)

### J. macOS — WaveShell 서브플러그인이 다른 것으로 로드됨  [P1] — **미해결(2026-08-13 조사)**

**증상.** 요청한 Waves 서브플러그인 대신 같은 쉘 안의 다른 플러그인이 만들어진다.
로드는 성공하므로 조용히 넘어간다. 실측 대응:

| 요청 | 실제 |
|:--|:--|
| Curves Resolve Stereo | Magma Lil Tube Stereo |
| AudioTrack Stereo | GTR Solo Tool Rack Stereo |

**확정된 사실.**
- 이름만 틀린 게 아니라 **실제로 다른 플러그인**이다 — 에디터를 열어 확인했다.
- **macOS 전용**. 동일 코드가 Windows 에서는 정상.
- 세션 복원만의 문제가 아니다 — **브라우저에서 직접 추가할 때도** 발생한다.
- 단일 클래스 .vst3(TDR·Surge XT 등)는 영향 없음. 다중 클래스(WaveShell)만.
- 세션 저장은 정상 — `path`·`uid`(`0x2f227020` = Curves Resolve Stereo)·이름 모두 정확히 기록된다.
- uid 충돌 아님 — 카탈로그상 해당 uid 를 가진 항목은 하나뿐.
- 같은 채널이라도 재현이 비대칭이다(ch1 실패 / ch2 성공) — 순서·상태 의존 정황.

**반증된 가설 4개** (같은 길을 다시 가지 말 것):
1. 쉘의 지연 초기화 → 즉시 4회 재시도. 전부 동일하게 실패.
2. 메시지 루프 미가동 → 복원을 `callAsync` 로 루프 진입 후로 미룸. 동일.
3. 다른 플러그인과의 로드 순서 간섭 → 그 플러그인만 단독으로 복원. 동일.
4. 팩토리 미열거 → 생성 직전 `findAllTypesForFile` 워밍업. 동일.

**다음 조사 지점.** JUCE 는 `VST3ModuleHandle::create` 에서 이름+uid 로 클래스
**인덱스**를 구하고(`findClassMatchingDescription`), `VST3ComponentHolder` 가 그
인덱스로 cid 를 다시 읽어 생성한다. 이 두 시점 사이에 쉘의 클래스 열거가
달라지면 어긋난다 — 인덱스 대신 cid 를 직접 들고 생성하도록 JUCE 를 패치해
검증하는 것이 다음 단계다.

**시도했다가 되돌린 것** (`c7f023e` → revert `b449cc1`):
- 이름 불일치 시 **로드 거부** → Waves 가 아예 못 쓰게 되고, 거부된 슬롯이
  종료 시 자동 저장에 반영되어 **세션의 체인이 삭제된다**. 거부 방식을 다시
  택한다면 자동 저장이 실패 슬롯을 지우지 않도록 함께 고쳐야 한다.
- 이름 불일치 시 **경고 후 로드 진행** → UI·세션에 요청한 이름이 남아
  **라벨이 거짓말한다**. 측정 리포트까지 오염되므로 더 나쁘다.
- 복원 시 `PluginCatalog` 우선 조회 → 기동 10.4초→5초로 빨라지지만 위 문제와
  독립적으로 이 현상은 그대로다.

### F. 다중 ASIO 장치 통합 (보류중)  [P3]
- PLAN §7 참조. 클럭 드리프트로 SW만으론 불가. 하드웨어 워드클럭 동기 권장 → 집계 드라이버 → 앱 내 다중 ASIO+마스터클럭+비동기 SRC 순. 현재 보류.

### G. 하우스키핑  [P3]
- `git init` + `build/` `.gitignore`(JUCE FetchContent 소스 3천+ 파일 제외).
- 과거 테스트로 생긴 `rec/`·`*_asm` 잔여 폴더 정리(신규 모델은 `takes/`·`.rectmp/` 사용).
- ~~QA: L1~L5 테스트 부재~~ — **L1 완료(2026-07-03)**: `SuperrackTests` 헤드리스 타깃(71 어서션, 녹음 FIFO 무손실/커밋·펀치·undo/재생 비트일치·시크·리샘플/병렬==직렬/설정·직렬화). 샌드박스 격리, 3회 반복 무플레이크. → [`05-qa/asio-vst3-host.qa-report.md`](05-qa/asio-vst3-host.qa-report.md). 잔여: 커밋 전 테스트 실행 습관/훅, VST3 실로딩·GUI 는 실기 영역.

# BACKLOG — Superrack 다음 작업

> 개정: 2026-07-02 · 우선순위: P0(높음)~P3(낮음) · 관련: [`01-plan/PLAN.md`](01-plan/PLAN.md) [`02-design/DESIGN.md`](02-design/DESIGN.md)

## 로드맵 위치
P0~P2 완료(실기), P3 견고화 + 통합 타임라인 녹음/재생 + 테이크 모델 완료(실기). **다음 마일스톤: P4-32ch-scale.**

## 다음 작업

### A. P4 — 32채널 확장 + CPU 프로파일링  [P0] — **완료(2026-07-02, 실기 측정)**
- 프로파일러 구현(DESIGN §5.7) + 실기 측정 완료 → [`measurements/cpu-profile-2026-07-02.md`](measurements/cpu-profile-2026-07-02.md).
- 결과(96kHz/96smp): 경량 체인(Pro-Q 3) **32ch 안정**, 무거운 체인(Pro-Q 3→Pro-R) **한계 4ch**(부하 채널 선형).
- 잔여: 다채널 ASIO 장치 확보 시 실기 확장 검증(합성 부하는 드라이버 채널 I/O 오버헤드 미포함).

### A2. 채널 병렬 워커 풀  [P1] — **실기 검증 종결(2026-07-03)** · 스톨 이슈는 A3 로 분리
- 직렬/병렬 완전 A/B(96k/512 3단): **스피드업 3.0×** @5레인(효율 62%), 한계 4ch→16ch. 48k/256 Q3→R 는 16ch→32ch 여유. → [`measurements/cpu-profile-parallel-2026-07-03.md`](measurements/cpu-profile-parallel-2026-07-03.md)
- 잔여: 부하 중 체인 편집(D2) 글리치 확인 1건.
- 설계 레퍼런스: tracktion_graph `LockFreeMultiThreadedNodePlayer`(참고만, GPL). 대기 = spin(backoff)→event. Windows: `TIME_CRITICAL`+MMCSS "Pro Audio", 워커 = 물리코어−2. → [`research/tech-stack-review-2026-07-02.md`](research/tech-stack-review-2026-07-02.md) §3

### A3. 병렬 모드 간헐 스톨 조사  [P1] — 신규(2026-07-03 실측에서 분리)
- 증상: **병렬 모드에서만** 300~1400%(3~14ms급) peak 스파이크. 직렬은 과부하에도 peak/avg ≈ 1.1~1.3. 저버퍼(96k/96)는 저채널부터, 96k/512 는 24ch+ 부터 표면화. avg 여유 구간(2단 16ch avg 62%)도 스파이크로 탈락 — **한계 확장의 실질 상한**(스톨 해결 시 96k/96 2단 8ch→16ch 기대).
- 가설: 잡 클레임 후 워커 프리엠션(오디오 스레드 조인 대기) 또는 웨이크 지연. 13ms 는 event 2ms 타임아웃으로 설명 불가 — MMCSS 미적용 확인, 스핀 구간, 클레임-후-슬립 레이스 등 코드 검사 + 스톨 시 워커 상태 로깅(스파이크 블록의 잡별 소요 기록) 필요.

### B. 재생 견고화  [P1] — 기술검토 Top 3
- **BufferingAudioSource 교체**: "짧은 락"이 아니라 백그라운드 스레드가 callbackLock 을 쥔 채 디스크 read(우선순위 역전, 소스 확인). → 채널당 SPSC 링버퍼 + 리더 스레드(prefetch), 언더런 무음. 레퍼런스: Mixxx CachingReader, Ardour butler. 검토 §5
- **스템 SR ≠ 장치 SR 리샘플**: 현재 같은 SR 가정(다르면 피치 어긋남). 로드 시 리샘플 또는 경고+차단.

### D2. farbot RealtimeObject 체인 스왑  [P1] — **구현 완료(2026-07-02)** DESIGN §5.8
- `RealtimeObject<Chain, nonRealtimeMutatable>` 헤더 벤더링(MIT, Tracktion 프로덕션 검증) — reconfiguring 플래그+Fence 수제 패턴을 wait-free 획득 + 비RT 지연 삭제로 대체. **farbot fifo 모듈은 레이스 리포트 있음 — RealtimeObject 만 사용.** 검토 §2
- A2 병렬화의 전제 조건.

### D3. 하우스키핑(기술검토 소과제)  [P3]
- CpuProfiler 리포트 환경 섹션에 **병렬 DSP on/off + 워커 수 기록**(2026-07-03 검토에서 병렬 여부를 baseline 대비로 추정해야 했음).
- JUCE 8.0.11 → **8.0.14** 업그레이드(CMake GIT_TAG 변경, VST3 SDK 3.8.0 반영).
- ThreadedWriter 튜닝: 채널당 FIFO 수 초(총 ~25-60MB), `setFlushInterval()` 주기 flush(크래시 복구성). RF64 는 JUCE 자동 — 작업 불필요.
- CLAP 호스팅 **보류**(메이저 벤더 CLAP 출시 or JUCE 공식 호스팅 지원 시 재검토).

### C. 펀치인 정밀화  [P1]
- 현재: 펀치 지점부터 **끝까지 덮어쓰기**(펀치아웃 없음). 부분 오버덥(punch-in/out 구간)·구간 선택 편집 추가.
- 녹음 중 기존 `[0..P)` 프리롤 재생 모니터링(현재 라이브 입력만).

### D. 오디오 스레드 견고화(설계 R1/R2/R3)  [P2] — **완료(2026-07-02 리팩토링)**
- ~~R1~~ `sr::CallbackFence`(콜백 세대 카운터) 로 in-flight 종료 확인 — ChannelStrip/recorder/player 공통 적용. DESIGN §5.1.
- ~~R2~~ 협상 초과 블록 dry 패스스루 방어. ~~R3~~ `PluginScanCache` 경로당 1회 스캔.
- 추가: GUI 구조 분리(ChannelRow.*, UiLookAndFeel.h), peak 스캔 SIMD화. `git init` + 스냅샷/리팩토링 커밋(G 일부 완료).

### E. 테이크/세션 고도화  [P2]
- ~~세션 플러그인 머신 이동성~~ — **완료(2026-07-03)**: 저장에 uid/name 추가, 로드 시 경로 실패 → 파일명/UID 폴백 + 로컬 경로 자기치유. 가짜 경로 세션으로 실검증(6/6 복원). DESIGN §5.3.
- 테이크 목록 UI 고도화(이력/펀치 구간 시각화, 정렬을 updatedAt 기준으로).
- (선택) 테이크 ↔ 세션 종속 모드: `.superrack` 저장/로드 시 연결 테이크도 함께.
- 세션 파일에 테이크 참조 저장 여부 결정.

### F. 다중 ASIO 장치 통합 (보류중)  [P3]
- PLAN §7 참조. 클럭 드리프트로 SW만으론 불가. 하드웨어 워드클럭 동기 권장 → 집계 드라이버 → 앱 내 다중 ASIO+마스터클럭+비동기 SRC 순. 현재 보류.

### G. 하우스키핑  [P3]
- `git init` + `build/` `.gitignore`(JUCE FetchContent 소스 3천+ 파일 제외).
- 과거 테스트로 생긴 `rec/`·`*_asm` 잔여 폴더 정리(신규 모델은 `takes/`·`.rectmp/` 사용).
- QA: L1~L5 테스트 부재 → 핵심 경로(녹음/펀치/커밋/재생/테이크전환) 테스트 추가.

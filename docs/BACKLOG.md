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
- 테이크 목록 UI 고도화(이력/펀치 구간 시각화, 정렬을 updatedAt 기준으로).
- (선택) 테이크 ↔ 세션 종속 모드: `.superrack` 저장/로드 시 연결 테이크도 함께.
- 세션 파일에 테이크 참조 저장 여부 결정.

### F. 다중 ASIO 장치 통합 (보류중)  [P3]
- PLAN §7 참조. 클럭 드리프트로 SW만으론 불가. 하드웨어 워드클럭 동기 권장 → 집계 드라이버 → 앱 내 다중 ASIO+마스터클럭+비동기 SRC 순. 현재 보류.

### G. 하우스키핑  [P3]
- `git init` + `build/` `.gitignore`(JUCE FetchContent 소스 3천+ 파일 제외).
- 과거 테스트로 생긴 `rec/`·`*_asm` 잔여 폴더 정리(신규 모델은 `takes/`·`.rectmp/` 사용).
- QA: L1~L5 테스트 부재 → 핵심 경로(녹음/펀치/커밋/재생/테이크전환) 테스트 추가.

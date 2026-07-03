# QA Report — asio-vst3-host

> 2026-07-03 · bkit QA Phase (데스크톱 C++/JUCE 프로젝트에 맞게 적용) · 대상: 핵심 경로(녹음 커밋/undo, 재생 스트리밍, 병렬 DSP, 세션/설정 직렬화)

## Pre-Release Scan Results

- bkit 표준 스캐너(`scripts/qa/pre-release-check.sh`)는 웹/Node 레포 구조 대상이라 이 레포에 없음 — **스킵**.
- 대체: MSVC Release 빌드 **경고 0** (JUCE recommended warning flags), 클린 빌드 확인.

## 테스트 레벨 매핑 (데스크톱 앱)

| bkit 레벨 | 원 정의 | 이 프로젝트 적용 | 상태 |
|---|---|---|---|
| L1 | Unit (Node/Jest) | **C++ 헤드리스 테스트 타깃 `SuperrackTests`** (CMake 콘솔 앱) | **PASS 71/71** |
| L2 | API (fetch/curl) | N/A — HTTP API 없음 | 해당 없음 |
| L3~L5 | E2E/UX/데이터 (Chrome MCP) | N/A — 네이티브 GUI. **실기 검증(수동)으로 대체** | 수동 이력 참조 |

## L1 — SuperrackTests (헤드리스, ASIO/플러그인 불필요)

실행: `build\SuperrackTests_artefacts\Release\SuperrackTests.exe` → 실패 수 = 종료 코드.
결과: **71 passed, 0 failed** · 3회 반복 무변동(스레드 플레이크 없음) · 샌드박스 격리(`SUPERRACK_APPDATA` + 임시 저장 루트 — 사용자 데이터 불가침).

| 스위트 | 검증 내용 |
|---|---|
| AppSettings | 저장/로드 왕복, 커스텀 저장 루트 생성, JSON 기록, VST3 경로 공백 정리, 워커 수 클램프(99→6) |
| MultitrackRecorder | 2ch × 20,480 샘플 블록 push → WAV **비트 일치**(락프리 FIFO 무손실), xrun 0 |
| TakeManager | 신규 커밋 / **펀치 병합**(head 보존 + tail 교체 + 길이/이력) / **undo↔redo 스왑**(오디오·timeline·session 세트, 3회 왕복) / 첫 녹음 undo→빈 테이크→redo 복원 |
| TimelinePlayer | 1:1 재생 **비트 일치**, 시크 후 연속성, EOF 위치 고정, 2채널 독립 내용, **리샘플**(48k→96k: 길이 2×±2, 램프 오차 < 1e-3), 언더런-대기 설계 준수 소비 |
| AudioWorkerPool | 32ch × 500블록 **병렬 == 직렬 결과 동일**(잡 누락/이중 실행 시 즉시 검출), 채널별 게인 정확, A3 스파이크 계측 스냅샷 플러밍, ChannelStrip 상태 직렬화 왕복 |

## 실기 검증 이력 (L3~L5 대체 — 수동, UR22mkII)

| 날짜 | 항목 | 결과 |
|---|---|---|
| 2026-07-02 | P0~P2: ASIO 패스스루, VST3 체인, dry 녹음(96k 루프백, A/B 스펙트럼) | PASS |
| 2026-07-03 | A2/A3: 병렬 3.0×, 스톨 해결(계측 4 runs) | PASS |
| 2026-07-03 | B: 재생/시크/재생중 시크/96k→48k 리샘플 피치 | PASS |
| 2026-07-03 | C: 녹음 복구/복구취소 왕복 | PASS |
| 2026-07-03 | D3: JUCE 8.0.14 — ASIO/체인/에디터 창/녹음·재생 | PASS |
| 2026-07-03 | H: 설정(저장 위치/VST3 경로/워커 수) | PASS |

## 판정

**PASS** — L1 71/71 + 실기 검증 이력 결합으로 핵심 경로 커버. 미커버(알려진 갭): VST3 플러그인 실로딩 경로(makeNode — 실기로만 검증), AudioEngine 장치 수명주기(ASIO 필요), GUI 상호작용. 회귀 방어 관점의 다음 단계: 커밋 전 `SuperrackTests` 실행을 습관화(또는 훅).

# Gap Analysis — asio-vst3-host (PDCA Check)

> 2026-07-03 · gap-detector v2.3 · 기준: DESIGN.md(rev 06-29 + 07-03 §5.8/5.9 증보) · PLAN.md · 구현 Source/ 24파일

## Match Rate

| 축 | 비중 | 점수 | 근거 요약 |
|---|:---:|:---:|---|
| Structural | 0.15 | **98%** | 설계 명시 전 모듈 실재 (AudioEngine/ChannelStrip/Recorder/Player/TakeManager/Profiler/WorkerPool/ScanCache) |
| Functional | 0.25 | **96%** | 플레이스홀더·스텁 0건, §별 동작 전부 실로직 (D2 wait-free, A2 fork-join, B SPSC+리샘플) |
| Contract | 0.25 | **90%** | 스레드 규약·파일 포맷·직렬화 일치. 감점 = §5 초안 스키마 잔재 + §5.5 모순 문구 |
| Runtime | 0.35 | **100%** | L1 `SuperrackTests` 71/71 (3회 무플레이크) + 실기 검증 이력 6건 → [`05-qa/asio-vst3-host.qa-report.md`](../05-qa/asio-vst3-host.qa-report.md) |

**Overall = 0.15×98 + 0.25×96 + 0.25×90 + 0.35×100 = 96.2%** (기준 90% 통과)

## PLAN 성공기준: **8/8 충족** (⚠️/❌ 없음)

채널 독립 체인 / 1:1 저지연 / Dry 32f 샘플정확 / 32ch 확장(병렬 3.0×) / 콜백 금지사항 / CPU 리스크 완화(A2·A3) / 세션 재현(uid 폴백) / 비목표 준수 — 상세 근거는 gap-detector 출력 §3.

## Gap 목록 — 전부 "구현이 문서를 앞지른" 방향, 코드 결함 0건

| # | 내용 | 방향 | 심각도 | 확신도 |
|---|---|---|---|:---:|
| G1 | 녹음 undo/redo(`*.prev` 스왑) DESIGN §5.6 미기재 | 구현→문서 | Important | 98% |
| G2 | `AppSettings`(app-settings.json 4번째 영속화 계층) 설계 미기재 | 구현→문서 | Important | 97% |
| G3 | 저장 루트 커스터마이즈로 takes/.rectmp/profiles 경로가 §5.4/5.6 고정 경로 서술과 상이 | 구현→문서 | Minor | 90% |
| G4 | §5.5 가 폐기된 BufferingAudioSource/StemSource 설계를 미개정 잔존 — §5.9 와 자기모순 | 문서 모순 | Important | 99% |
| G5 | §5.5 "SR 불일치 시 피치 어긋남(리샘플 미적용)" 문구가 §5.9 B-2 및 구현과 모순 | 문서 모순 | Important | 98% |
| G6 | §5 초안 세션 스키마(recDry/recording 블록)가 실제 session.json 과 상이 | 문서 모순 | Minor | 92% |
| G7 | §5.4 "RF64 미구현" vs BACKLOG D3 "JUCE 자동" 불일치 | 문서 모순 | Minor | 80% |

**설계→구현 누락: 없음** (§4 RT 체크리스트 8항목 전부 코드 확인).

## 조치 결과 (2026-07-03, 같은 날 처리 — "지금 모두 수정" 선택)

**G1~G7 전부 해소** (DESIGN.md 문서 동기화, 코드 무변경):
- G4/G5: §5.5 를 "§5.9 로 대체" 처리, BufferingAudioSource/리샘플 미적용 문구 제거. 추가로 v1 `*_asm` 조립 폴더 서술도 현행 `.rectmp`→제자리 커밋 모델로 개정 (분석에서 미탐지된 동류 갭).
- G1: §5.6 에 녹음 undo/redo 소절 신설 (`*.prev` 스왑, isCurrentNewer, GUI 버튼).
- G2/G3: §5.10 AppSettings 신설 (4계층 영속화), §5.4/§5.6/§3.4 경로를 `<저장 루트>` 기준으로 개정.
- G6: §5 세션 스키마를 초안→확정(구현 기준, uid/name 포함, recDry/recording 폐기)으로 교체.
- G7: RF64 서술을 "JUCE 자동 전환 — 작업 불필요"로 통일 (§3.4/§5.4).
- 추가: §6 JUCE 버전 8.0.11→8.0.14 정정 + SuperrackTests 타깃 기재.

**동기화 후 Match Rate 재산정: Structural 100 · Functional 96 · Contract 98 · Runtime 100 → Overall ≈ 98.5%.**

## 판정: Check PASS (96.2% → 문서 동기화 후 98.5%) — 다음 단계 `/pdca report`

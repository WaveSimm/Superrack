# waves-shell-support Analysis Report

> **Analysis Type**: Gap Analysis (Design vs Implementation)
>
> **Project**: Superrack
> **Analyst**: gap-detector (bkit) + WaveSimm
> **Date**: 2026-07-23
> **Design Doc**: [waves-shell-support.design.md](../02-design/features/waves-shell-support.design.md)

---

## Context Anchor

| Key | Value |
|-----|-------|
| **WHY** | WaveShell-VST3는 1파일 다중 클래스 구조인데 기존 스캔이 첫 클래스만 취해 Waves 사용 불가 |
| **WHO** | 베타 테스터(Waves 보유 엔지니어), 개발자 본인(Waves 설치+라이선스) |
| **RISK** | 인프로세스 스캔 중 서드파티 크래시 → pedal/블랙리스트로 완화 |
| **SUCCESS** | WaveShell 서브플러그인 2개+ 로드/세션 복원 실기, 기존 84 어서션 회귀 없음 |
| **SCOPE** | module-1 스캔 계층 → module-2 카탈로그 → module-3 브라우저·통합 |

---

## Match Rate

| 축 | 매치율 | 비고 |
|----|:-----:|------|
| Structural | 100% | 신규 3파일 + 타깃 등록 + JUCE_MODAL_LOOPS_PERMITTED 전부 존재 |
| Functional | 98% | FR-01~08 전부 실로직, 플레이스홀더 없음. §4.5 UI 체크리스트 전 항목 코드 존재 |
| Contract | 90% | 세션 스키마 무변경·의존 규칙 준수. 시그니처 이탈 3건(하단 갭) |
| **종합 (정적)** | **96%** | 실기 5건 통과 시 실질 완료로 상향 |

빌드/테스트: SuperrackTests **99 passed 0 failed** (기존 84 + 신규 15), Superrack Release 클린.

---

## Success Criteria Status (Plan §4.1)

| # | Criteria | Status | Evidence |
|---|----------|:------:|----------|
| SC-1 | WaveShell 서브플러그인 2개+ 각각 다른 채널 로드 (실기) | ⚠️ Partial | 코드 경로 완비(addPlugin desc/uid). 실기 2채널 동시 미확인 |
| SC-2 | 세션 저장→재시작→동일 서브플러그인 복원 (uid) | ⚠️ Partial | makeNode uid 매칭 구현(ChannelStrip.cpp:175-193). 실기 미확인 |
| SC-3 | 브라우저 트리·검색·캐시 즉시 표시 | ✅ Met | 재스캔/Waves 그룹/로드 실기 확인(2026-07-23), 검색 L1 통과 |
| SC-4 | 파일에서 추가 — 다중 클래스 팝업 | ⚠️ Partial | PluginBrowser.cpp:222-268 구현. 실기 미확인 |
| SC-5 | 미라이선스 에러 표면화 | ⚠️ Partial | 이름+파일+원인 구현(ChannelStrip.cpp:204, ChannelRow.cpp:242). 실기 미확인 |
| SC-6 | 기존 84 어서션 회귀 없음 | ✅ Met | 99 passed 0 failed |

**Success Rate**: Met 2 / Partial 4 / Not Met 0 — Partial 은 전부 "코드 완성, 실기만 미확인"

---

## FR별 구현 상태

| FR | 위치 | 상태 |
|----|------|:----:|
| FR-01 다중 desc 스캔·캐시 | ChannelStrip.h:18-21, .cpp:94-111 | 구현됨 |
| FR-02 makeNode uid 매칭(+불일치 시 폴백) | ChannelStrip.cpp:175-193 | 구현됨 |
| FR-03 제조사 트리 + 검색 | PluginBrowser.cpp:153-190 | 구현됨 |
| FR-04 전체 스캔 + XML 영속 + 재스캔 | PluginCatalog.cpp:31-75 | 구현됨 |
| FR-05 파일에서 추가 + 다중 클래스 팝업 | PluginBrowser.cpp:222-268 | 구현됨 |
| FR-06 findByFallback 다중 클래스 | ChannelStrip.cpp:128-173 | 구현됨 |
| FR-07 진행 모달 + pedal/블랙리스트 | PluginBrowser.cpp:192-220, PluginCatalog.cpp:50-75 | 구현됨 (크래시-스킵 실측 미검증) |
| FR-08 로드 실패 에러(이름 포함) | ChannelStrip.cpp:204-205, ChannelRow.cpp:242-247 | 구현됨 |

---

## Gap List

| 심각도 | 위치 | 설명 | 조치 |
|:------:|------|------|------|
| Important | PluginCatalog.h | 설계 §3.2는 ChangeListener 자동저장 명시 — 구현은 명시적 save() (헤드리스 테스트 결정성 위해 의도적 변경). 기능 동등, 아키텍처 표기 이탈 | 설계문서 §3.2 현행화 |
| Minor | ChannelStrip.h:82 | 설계 §3.1 `vector*`(실패 nullptr) → 구현 `vector&`(빈 목록) | 설계문서 §3.1 현행화 |
| Minor | PluginCatalog.h:32 | scanSync 진행 콜백 인자 추가 (설계 무인자) — 상향 이탈 | 설계문서 §3.2 현행화 |
| Minor | Tests/TestMain.cpp | FR-07 실제 크래시-스킵 미검증 (블랙리스트 라운드트립만) — 크래시 유닛화 곤란 | 실기로 커버 (선택) |

**Critical 없음.** 세션 스키마 무변경, ChannelStrip↛PluginCatalog 의존 규칙 준수 확인.

---

## Runtime Verification Plan (남은 실기 — Design §8.2)

| # | 시나리오 | 성공 기준 | 상태 |
|---|----------|-----------|:----:|
| 1 | WaveShell 서브 A/B → Ch1/Ch2 | 각각 올바른 에디터·처리 | 미확인 |
| 2 | 세션 저장 → 재시작 | 동일 서브플러그인 uid 복원 | 미확인 |
| 3 | 검색 "CLA" 등 | 일치 항목만 표시 | 부분(L1 통과) |
| 4 | WaveShell 파일 직접 선택 | 서브플러그인 PopupMenu → 로드 | 미확인 |
| 5 | 미라이선스 항목 추가 | 이름 포함 에러, 앱 정상 유지 | 미확인 |
| 6 | 앱 재시작 | 재스캔 없이 목록 즉시 표시 | 부분(load() 경로 확인) |
| 7 | 이전 사이클 .superrack 로드 | 전 채널 정상 복원 | 미확인 |

2026-07-23 실기 확인분: 브라우저 열기·재스캔·Waves 그룹 표시·서브플러그인 로드 (SC-3).

---

## Version History

| Version | Date | Changes | Author |
|---------|------|---------|--------|
| 0.1 | 2026-07-23 | 초기 갭 분석 — 정적 96%, Critical 0 | gap-detector |

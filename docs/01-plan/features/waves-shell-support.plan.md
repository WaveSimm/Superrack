# waves-shell-support Planning Document

> **Summary**: WaveShell 등 다중 클래스 VST3 모듈 지원 + KnownPluginList 기반 플러그인 브라우저 도입
>
> **Project**: Superrack (JUCE 8 ASIO 다채널 VST3 인서트 프로세서)
> **Version**: 베타 (2026-07-03 사이클 완료 후)
> **Author**: WaveSimm
> **Date**: 2026-07-23
> **Status**: Draft

---

## Executive Summary

| Perspective | Content |
|-------------|---------|
| **Problem** | 호스트가 `.vst3` 파일당 1개 클래스만 인식(`types[0]`)하여, 수십 개 플러그인을 하나의 쉘 파일로 배포하는 Waves(WaveShell-VST3)의 서브플러그인에 접근할 수 없다. 플러그인 선택도 파일 탐색기 의존이라 탐색성이 낮다. |
| **Solution** | 다중 클래스 스캔(경로 → description 목록) + uid 기반 정확한 클래스 선택/세션 복원 + KnownPluginList 기반 플러그인 브라우저(제조사 트리 + 검색). FileChooser는 보조로 유지. |
| **Function/UX Effect** | Waves 전 제품을 포함한 다중 클래스 VST3 사용 가능. 플러그인을 파일이 아닌 이름/제조사로 탐색·검색해 추가. 재시작 시 스캔 캐시로 목록 즉시 표시. |
| **Core Value** | 상용 플러그인 생태계(특히 Waves) 호환성 확보 — 베타 테스터의 실사용 플러그인 체인을 그대로 수용. |

---

## Context Anchor

| Key | Value |
|-----|-------|
| **WHY** | WaveShell-VST3는 1파일 다중 클래스 구조인데 현재 스캔이 첫 클래스만 취해 Waves 플러그인을 사실상 사용할 수 없음 |
| **WHO** | Superrack 베타 테스터(Waves 라이선스 보유 엔지니어), 개발자 본인(실기 검증 환경: Waves 설치+라이선스) |
| **RISK** | 인프로세스 스캔 중 서드파티 플러그인 크래시 시 앱 전체 다운 — 실패 파일 기록 후 재실행 시 스킵으로 완화 |
| **SUCCESS** | WaveShell 서브플러그인 2개+를 서로 다른 채널에 로드/세션 복원 실기 검증, 기존 L1 84 어서션 회귀 없음 |
| **SCOPE** | 스캔 계층 다중화(FR-01/02/06) → 브라우저 UI(FR-03/04) → 파일 선택 보조 경로(FR-05) → 에러 표면화·실기 검증(FR-07/08) |

---

## 1. Overview

### 1.1 Purpose

WaveShell-VST3처럼 하나의 `.vst3` 모듈 팩토리가 여러 플러그인 클래스를 노출하는 경우를 정식 지원한다. VST3 표준 기능이므로 Waves 전용 처리가 아닌 "다중 클래스 모듈 일반 지원"으로 구현한다.

### 1.2 Background

- VST3에는 VST2식 별도 쉘 API가 없다. WaveShell은 `IPluginFactory`에 라이선스된 플러그인들을 개별 `PClassInfo`로 노출하며, JUCE `VST3PluginFormat::findAllTypesForFile()`이 이미 전 클래스를 배열로 반환한다.
- 현재 코드는 파일 1개 = 플러그인 1개 가정: `ChannelStrip::scanPath()`가 `types[0]`만 취하고, `scanCache.byPath`도 단일 description 캐시, `ChannelRow::chooseAndAddPlugin()`은 FileChooser로 파일만 선택.
- 사용자 결정(2026-07-23): 선택 UI는 **플러그인 브라우저**(제조사 트리 + 검색), 스캔은 **동기 유지**, FileChooser는 **보조로 유지**, 실기 검증 환경 **Waves 설치+라이선스 보유**.

### 1.3 Related Documents

- 이전 사이클 설계: `docs/02-design/DESIGN.md` (asio-vst3-host)
- 소스: `Source/ChannelStrip.cpp` (scanPath/findByFallback/makeNode), `Source/ChannelRow.cpp` (chooseAndAddPlugin), `Source/AppSettings.*`

---

## 2. Scope

### 2.1 In Scope

- [ ] 다중 클래스 스캔: `scanPath` 를 경로 → `std::vector<PluginDescription>` 반환/캐시로 변경
- [ ] uid 우선 매칭: `makeNode` 정상 경로에서도 uid로 서브플러그인 선택 (uid=0이면 기존처럼 첫 항목)
- [ ] `findByFallback` 다중 클래스 대응 (파일명/uid 폴백 순회 시 전체 클래스 검사)
- [ ] KnownPluginList 기반 전체 스캔(표준 위치 + AppSettings 추가 경로) + XML 영속화 + 재스캔
- [ ] 플러그인 브라우저 UI: 제조사 트리 + 검색창, 선택 시 채널에 추가
- [ ] FileChooser 보조 경로: "파일에서 추가" — 다중 클래스 파일이면 서브플러그인 PopupMenu 선택
- [ ] 스캔 진행 표시(동기·모달) + 스캔 실패 파일 기록 후 다음 스캔에서 스킵
- [ ] 라이선스 없는 클래스 인스턴스 생성 실패 시 에러 메시지 표면화
- [ ] L1 유닛 테스트 (다중 desc 캐시, uid 매칭, 브라우저 필터)

### 2.2 Out of Scope

- 별도 프로세스(크래시 격리) 스캔 — 인프로세스 스캔 문제가 실측되면 후속 사이클
- 비동기(백그라운드 스레드) 스캔 — 동기 유지 결정
- VST2 쉘 / AU 등 타 포맷 지원
- 플러그인 즐겨찾기·태그 등 브라우저 고도화

---

## 3. Requirements

### 3.1 Functional Requirements

| ID | Requirement | Priority | Status |
|----|-------------|----------|--------|
| FR-01 | `scanPath` 가 파일의 모든 클래스를 반환하고 `scanCache` 가 경로 → 목록으로 캐시 | High | Pending |
| FR-02 | `makeNode` 가 uid≠0이면 `uniqueId/deprecatedUid` 일치 클래스를 선택해 인스턴스화 (세션 복원 정확성) | High | Pending |
| FR-03 | 플러그인 브라우저: 제조사별 트리 + 검색 필터, 항목 선택으로 채널에 플러그인 추가 | High | Pending |
| FR-04 | 전체 스캔: VST3 표준 위치 + 설정의 추가 경로를 스캔해 KnownPluginList 구성, XML을 앱 설정에 영속화, 수동 재스캔 제공 | High | Pending |
| FR-05 | "파일에서 추가" 보조 경로 유지 — 다중 클래스 파일 선택 시 서브플러그인 PopupMenu | Medium | Pending |
| FR-06 | `findByFallback` 이 다중 클래스 파일에서도 uid 일치 항목을 찾음 (세션 머신 이동성 유지) | High | Pending |
| FR-07 | 스캔 진행 표시(모달) + 실패 파일 기록(블랙리스트)으로 반복 크래시 방지 | Medium | Pending |
| FR-08 | 인스턴스 생성 실패(미라이선스 등) 시 플러그인 이름 포함 명확한 에러 표시 | Medium | Pending |

### 3.2 Non-Functional Requirements

| Category | Criteria | Measurement Method |
|----------|----------|-------------------|
| 성능 | 재실행 시 브라우저 목록 즉시 표시(재스캔 없이 캐시 로드), 오디오 스레드 무영향 | 실기 확인, 기존 언더런 카운터 |
| 호환성 | 기존 세션 파일(비쉘 플러그인, uid 저장분) 무변경 로드 — 세션 스키마 변경 없음 | 기존 세션 로드 회귀 테스트 |
| 안정성 | 스캔 실패 파일이 있어도 앱 기동·나머지 스캔 정상 진행 | 블랙리스트 동작 L1 테스트 |

---

## 4. Success Criteria

### 4.1 Definition of Done

- [ ] WaveShell 서브플러그인 2개 이상을 서로 다른 채널에 로드, 각각 올바른 플러그인으로 동작 (실기)
- [ ] WaveShell 서브플러그인 포함 세션 저장 → 앱 재시작 → 동일 서브플러그인 복원 (uid 매칭, 실기)
- [ ] 브라우저: 스캔 후 제조사 트리 표시, 검색 필터 동작, 재시작 시 캐시로 즉시 표시
- [ ] FileChooser 경로: 다중 클래스 파일 선택 시 서브플러그인 팝업 선택 동작
- [ ] 미라이선스 Waves 클래스 로드 시도 시 명확한 에러 메시지 (실기)
- [ ] 기존 비쉘 플러그인 로드·세션 복원 회귀 없음 (기존 L1 84 어서션 통과 유지)

### 4.2 Quality Criteria

- [ ] 신규 L1 테스트: 다중 desc 캐시 / uid 매칭 / 브라우저 검색 필터 / 블랙리스트
- [ ] Superrack·SuperrackTests 빌드 성공, 기존 테스트 전체 통과

---

## 5. Risks and Mitigation

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| 인프로세스 스캔 중 서드파티 플러그인 크래시 → 앱 다운 | High | Medium | 스캔 시작 전 대상 파일 기록, 크래시 후 재실행 시 해당 파일 스킵(dead man's pedal 패턴). 반복 발생 시 별도 프로세스 스캔을 후속 사이클로 |
| WaveShell 전체 스캔 소요 시간(동기, 수 초~수십 초) | Medium | High | 1회성(KnownPluginList XML 캐시) + 모달 진행 표시. 자동 스캔이 아닌 명시적 스캔 버튼 |
| 미라이선스 클래스가 목록에 노출 → 로드 실패로 사용자 혼란 | Medium | High | FR-08 에러 메시지에 플러그인 이름·원인 표시. 실기(라이선스 보유 환경)에서 실패 케이스 재현 확인 |
| Waves 버전업 시 쉘 파일명 변경(V15→V16 등)으로 세션 경로 유실 | Medium | Medium | 기존 uid 전체 스캔 폴백(FR-06)이 커버 — 회귀 테스트로 보장 |
| `scanPath` 시그니처 변경이 기존 호출부에 파급 | Medium | Low | 6장 Impact Analysis 대로 호출부 전수 수정 + 기존 L1 테스트로 회귀 확인 |

---

## 6. Impact Analysis

### 6.1 Changed Resources

| Resource | Type | Change Description |
|----------|------|--------------------|
| `ChannelStrip::scanPath` | API (내부) | 단일 `PluginDescription` → 목록 반환, `scanCache.byPath` 값 타입 변경 |
| `ChannelStrip::makeNode` | API (내부) | uid 우선 매칭 로직 추가 |
| `ChannelStrip::findByFallback` | API (내부) | 다중 클래스 순회로 변경 |
| `ChannelRow::chooseAndAddPlugin` | UI | 브라우저 열기로 대체, FileChooser는 보조 진입점으로 이동 |
| `AppSettings` | Config | KnownPluginList XML + 스캔 블랙리스트 저장 키 추가 (기존 키 무변경) |
| 플러그인 브라우저 컴포넌트 | UI (신규) | 제조사 트리 + 검색, KnownPluginList 구독 |
| 세션 포맷 | Schema | **변경 없음** — uid(`uniqueId`/`deprecatedUid`) 이미 저장·복원 중 |

### 6.2 Current Consumers

| Resource | Operation | Code Path | Impact |
|----------|-----------|-----------|--------|
| `scanPath` | READ | `makeNode` (로드/세션 복원) | 수정 대상 — uid 매칭 추가 |
| `scanPath` | READ | `findByFallback` ①빠른 경로 ②uid 전체 스캔 | 수정 대상 — 다중 desc 순회 |
| `chooseAndAddPlugin` | UI | `ChannelRow` 추가 버튼 | 브라우저 호출로 교체 |
| `AppSettings::vst3ExtraPaths` | READ | `findByFallback`, (신규) 전체 스캔 | 무변경 재사용 |
| 세션 저장/복원 uid | READ/WRITE | 세션 직렬화 (`makeNode` 인자) | 무변경 — FR-02가 소비만 추가 |

### 6.3 Verification

- [ ] `scanPath`/`findByFallback` 호출부 전수 수정 확인 (컴파일 에러로 강제됨)
- [ ] 기존 세션 파일 로드 회귀 확인 (uid=0 구세션 포함)
- [ ] AppSettings 기존 키 하위 호환 확인

---

## 7. Architecture Considerations

### 7.1 Project Level

데스크톱 C++/JUCE 앱 — bkit 웹 레벨 구분 비해당. 기존 asio-vst3-host 아키텍처(단일 프로세스, `Source/` 평면 구조) 위에 증분 구현.

### 7.2 Key Architectural Decisions (Design 단계에서 확정)

| Decision | Options | 예상 선택 | Rationale |
|----------|---------|----------|-----------|
| 플러그인 목록 관리 | 자체 캐시 확장 / `juce::KnownPluginList` | KnownPluginList | XML 직렬화·스캔 API 내장, JUCE 표준 |
| 스캔 실행 | `PluginDirectoryScanner` 메시지 스레드 펌핑 / 수동 순회 | Design에서 결정 | 동기 + 진행 표시 요구 충족 방식 비교 |
| 브라우저 UI | `juce::PluginListComponent` / 커스텀 TreeView+검색 | 커스텀 TreeView | 제조사 트리 + 검색 요구 — PluginListComponent는 테이블형 |
| 브라우저 표시 | 모달 다이얼로그 / 도킹 패널 | Design에서 결정 | 기존 UI(채널 로우) 흐름에 맞춤 |

---

## 8. Convention Prerequisites

기존 프로젝트 컨벤션 유지: `Source/` 평면 구조, 한국어 주석(설계 근거 중심), JUCE 코딩 스타일, 오디오 스레드 락프리 원칙, L1 테스트는 `SuperrackTests` 타깃. 신규 환경 변수·외부 의존성 없음.

---

## 9. Next Steps

1. [ ] Design 문서 작성 (`/pdca design waves-shell-support`)
2. [ ] 구현 (`/pdca do waves-shell-support`)
3. [ ] 갭 분석 + 실기 검증 (`/pdca analyze waves-shell-support`)

---

## Version History

| Version | Date | Changes | Author |
|---------|------|---------|--------|
| 0.1 | 2026-07-23 | Initial draft — 요구사항·UI 방식 사용자 확정 반영 | WaveSimm |

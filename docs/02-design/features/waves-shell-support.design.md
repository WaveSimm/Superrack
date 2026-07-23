# waves-shell-support Design Document

> **Summary**: 다중 클래스 VST3(WaveShell) 지원 — 스캔 계층 일반화 + PluginCatalog/PluginBrowser 신설
>
> **Project**: Superrack (JUCE 8 ASIO 다채널 VST3 인서트 프로세서)
> **Version**: 베타
> **Author**: WaveSimm
> **Date**: 2026-07-23
> **Status**: Draft
> **Planning Doc**: [waves-shell-support.plan.md](../../01-plan/features/waves-shell-support.plan.md)

---

## Context Anchor

| Key | Value |
|-----|-------|
| **WHY** | WaveShell-VST3는 1파일 다중 클래스 구조인데 현재 스캔이 첫 클래스만 취해 Waves 플러그인을 사실상 사용할 수 없음 |
| **WHO** | Superrack 베타 테스터(Waves 라이선스 보유 엔지니어), 개발자 본인(실기 검증 환경: Waves 설치+라이선스) |
| **RISK** | 인프로세스 스캔 중 서드파티 플러그인 크래시 시 앱 전체 다운 — 실패 파일 기록 후 재실행 시 스킵으로 완화 |
| **SUCCESS** | WaveShell 서브플러그인 2개+를 서로 다른 채널에 로드/세션 복원 실기 검증, 기존 L1 84 어서션 회귀 없음 |
| **SCOPE** | 스캔 계층 다중화(FR-01/02/06) → 카탈로그(FR-04/07) → 브라우저 UI·통합(FR-03/05/08) |

---

## 1. Overview

### 1.1 Design Goals

- `.vst3` 파일 1개 = 플러그인 N개를 스캔·로드·세션 복원 전 경로에서 일관 지원 (VST3 표준, Waves 전용 처리 없음)
- 플러그인 탐색을 파일 기반에서 카탈로그 기반(제조사 트리 + 검색)으로 전환, 파일 선택은 보조 유지
- 지난 사이클에서 실기 검증된 세션 복원·uid 폴백 경로의 구조는 보존 (다중 desc 일반화만)

### 1.2 Design Principles

- 메시지 스레드 전용: 스캔/카탈로그/브라우저는 전부 메시지 스레드. 오디오 경로 무접촉
- 단일 식별자: 서브플러그인 식별은 기존 세션 필드 `uid`(`PluginDescription::uniqueId`) 재사용 — 세션 스키마 무변경
- 크래시 내성: 스캔 중 크래시가 다음 실행을 막지 않도록 JUCE dead-man's-pedal 사용

---

## 2. Architecture

### 2.0 Architecture Comparison

| Criteria | Option A: Minimal | Option B: Clean | Option C: Pragmatic |
|----------|:-:|:-:|:-:|
| **Approach** | PopupMenu 트리 대체, ChannelStrip만 수정 | 스캔 전면 PluginCatalog 일원화 | 신규 모듈 추가 + 검증된 스캔 경로 보존 |
| **New Files** | 0 | 4 | 4 |
| **Modified Files** | 2 | 7+ | 6 |
| **검색창** | 불가 (요구 미달) | 가능 | 가능 |
| **Complexity** | Low | High | Medium |
| **Risk** | 요구 미달 | 검증된 세션 복원 코드 재작성 | Low |

**Selected**: **Option C** — **Rationale**: 실기 검증 완료된 ChannelStrip 세션/폴백 경로를 다중 desc로 일반화만 하고, 브라우저/전체 스캔은 독립 모듈(PluginCatalog/PluginBrowser)로 추가. 요구사항 전부 충족 + 회귀 리스크 최소. (Checkpoint 3 사용자 확정, 2026-07-23)

### 2.1 Component Diagram

```
ChannelRow [+VST3 버튼]
    │ open
    ▼
PluginBrowser (DialogWindow)                PluginCatalog
  ├ 검색 TextEditor                ◀──읽기──  ├ juce::KnownPluginList
  ├ TreeView (제조사별 그룹)                   ├ scanSync()  ── PluginDirectoryScanner
  ├ [재스캔] ─────────호출──────────▶          │               (메시지 스레드 펌핑 + 진행창)
  └ [파일에서 추가] ── FileChooser             ├ XML 영속화  ── %APPDATA%/Superrack/plugin-catalog.xml
        │ (다중 클래스면 PopupMenu)            └ dead-man's-pedal / 블랙리스트
        ▼ PluginDescription 선택
ChannelStrip::addPlugin (const PluginDescription&)   ← 신규 오버로드
    │
    ▼
makeNode → scanPath(다중 desc) → uid 매칭 → createPluginInstance
```

### 2.2 Data Flow

```
[추가]   브라우저 선택 → desc → addPlugin(desc) → makeNode(desc.fileOrIdentifier, uid=desc.uniqueId)
[복원]   session.json plugins[{path,uid,...}] → loadChain → makeNode(path, uid)
           → scanPath(path) 다중 desc → uid 매칭 → 실패 시 findByFallback(uid 포함 순회)
[스캔]   재스캔 버튼 → PluginCatalog::scanSync (진행 모달) → KnownPluginList → XML 저장
[기동]   PluginCatalog 생성자 → plugin-catalog.xml 로드 (재스캔 없음, 즉시 표시)
```

### 2.3 Dependencies

| Component | Depends On | Purpose |
|-----------|-----------|---------|
| PluginBrowser | PluginCatalog | 목록 표시·재스캔 트리거 |
| PluginBrowser | ChannelStrip (콜백) | 선택된 desc 로 addPlugin |
| PluginCatalog | AppSettings | 추가 검색 경로(`vst3ExtraPaths`) 재사용 |
| ChannelStrip | PluginScanCache (기존) | 다중 desc 캐시 — 카탈로그와 독립 (세션 복원은 카탈로그 없이도 동작) |

> 설계 결정: ChannelStrip 은 PluginCatalog 에 의존하지 않는다. 세션 복원은 카탈로그
> 미구성(첫 실행) 상태에서도 기존 scanPath/폴백만으로 동작해야 하기 때문.

---

## 3. Data Model

### 3.1 PluginScanCache (수정 — FR-01)

```cpp
// ChannelStrip.h — 경로 → 그 파일의 "모든" 클래스
struct PluginScanCache
{
    std::map<juce::String, std::vector<juce::PluginDescription>> byPath;
};
```

- `scanPath` 시그니처: `const std::vector<juce::PluginDescription>* scanPath (const juce::String& path)` — 실패 시 nullptr. 빈 목록도 캐시(반복 실패 스캔 방지).
- uid 선택 헬퍼: `static const juce::PluginDescription* pickByUid (const std::vector<PluginDescription>&, int uid)` — uid==0 이면 첫 항목(기존 동작 보존, 단일 클래스 파일 하위 호환), 아니면 `uniqueId/deprecatedUid` 일치 항목, 없으면 nullptr.

### 3.2 PluginCatalog (신규 — FR-04/07)

```cpp
class PluginCatalog : private juce::ChangeListener
{
public:
    static PluginCatalog& get();                       // AppSettings 와 동일한 싱글턴 패턴

    juce::KnownPluginList&       list();               // 브라우저가 구독 (ChangeBroadcaster)
    void  scanSync();                                  // 전체 스캔 (모달 진행창, 메시지 스레드)
    std::vector<juce::PluginDescription> scanSingleFile (const juce::String& path); // 파일에서 추가용

private:
    void  load();                                      // 기동 시 XML → list
    void  save();                                      // 변경 시 XML 저장 (changeListenerCallback)
    juce::File catalogFile();                          // %APPDATA%/Superrack/plugin-catalog.xml
    juce::File deadMansPedalFile();                    // %APPDATA%/Superrack/plugin-scan-inflight.txt

    juce::KnownPluginList knownList;
    juce::VST3PluginFormat vst3Format;
};
```

**영속 파일** (세션 스키마 무변경):

| File | Content | Format |
|------|---------|--------|
| `%APPDATA%/Superrack/plugin-catalog.xml` | `KnownPluginList::createXml()` — 플러그인 목록 + 블랙리스트 항목 포함 | XML |
| `%APPDATA%/Superrack/plugin-scan-inflight.txt` | `PluginDirectoryScanner::setDeadMansPedalFile` 대상 — 스캔 중 크래시 시 해당 파일이 다음 스캔에서 자동 블랙리스트 | text |

**scanSync 절차** (동기 + 진행 표시, 사용자 결정):

1. 검색 경로 = `VST3PluginFormat::getDefaultLocationsToSearch()` + `AppSettings::vst3ExtraPaths()` (findByFallback 과 동일 규칙)
2. `juce::PluginDirectoryScanner` 생성, `setDeadMansPedalFile()` 지정
3. 모달 진행창(`AlertWindow` + 진행바)에서 `scanNextFile (true, name)` 루프 — 메시지 스레드 펌핑, 파일당 이름/진행률 갱신
4. 완료 후 `save()`. 실패 파일은 KnownPluginList 블랙리스트에 남아 다음 스캔에서 스킵 (FR-07)

### 3.3 세션 포맷 (무변경)

`plugins:[{path, uid, name, bypass, state}]` — uid 는 이미 저장·복원 중. FR-02 는 소비 로직만 추가.

---

## 4. Module Design

### 4.1 M1 — ChannelStrip 스캔 계층 다중화 (FR-01/02/06)

| 함수 | 변경 |
|------|------|
| `scanPath` | 다중 desc 반환 (§3.1). 호출부 컴파일 에러로 전수 수정 강제 |
| `makeNode` | `scanPath` 성공 시 `pickByUid(descs, uid)` 로 클래스 선택. uid≠0 인데 매칭 실패면 **경로 성공이라도 폴백 진입** (쉘 버전업으로 클래스 구성이 바뀐 경우) |
| `findByFallback` | ①파일명 검색·②uid 전체 스캔 모두 `pickByUid` 로 다중 desc 순회. uid 매칭 의미는 기존과 동일 |
| `addPlugin (const PluginDescription&, String& err)` | **신규 오버로드** — 브라우저/팝업에서 확정된 desc 로 직접 노드 생성 (재스캔 없이 `desc.fileOrIdentifier` + `desc.uniqueId` 를 makeNode 에 전달). 기존 `addPlugin(File)` 은 유지(단일 클래스 파일 하위 호환) |

### 4.2 M2 — PluginCatalog (FR-04/07)

§3.2 참조. 추가 규칙:

- 기동 시 `load()` 만 수행 — **자동 스캔 없음** (스캔은 브라우저의 재스캔 버튼으로만, Plan 리스크 완화)
- `scanSingleFile(path)`: `findAllTypesForFile` 로 스캔 후 결과를 KnownPluginList 에도 병합(다음 브라우저 표시에 반영). "파일에서 추가" 경로가 카탈로그를 점진 구축
- 스캔 옵션: 이미 스캔된 파일은 `KnownPluginList` 가 타임스탬프로 스킵 (재스캔 고속화)

### 4.3 M3 — PluginBrowser + ChannelRow 통합 (FR-03/05/08)

- `PluginBrowser` : `juce::Component`, `DialogWindow::LaunchOptions` 로 표시. 콜백 `std::function<void(const juce::PluginDescription&)> onPick`
- 트리 모델: KnownPluginList 를 제조사명(`manufacturerName`)으로 그룹 → 각 그룹 아래 플러그인 이름 정렬. 검색어 입력 시 이름/제조사 부분 일치 필터(트리 자동 전개)
- 선택: 항목 더블클릭 또는 [추가] 버튼 → `onPick(desc)` → 창 닫힘
- `ChannelRow::chooseAndAddPlugin()` → `openPluginBrowser()` 로 교체. onPick 에서 `strip.addPlugin(desc, err)` + `rebuildChips()` + `notifySessionChanged()` (기존 후처리 동일)
- [파일에서 추가]: 기존 FileChooser 흐름을 브라우저 안으로 이동. 선택 파일을 `scanSingleFile` → 1개면 즉시 추가, N개면 `PopupMenu` 로 서브플러그인 선택 (FR-05)
- 에러 표면화 (FR-08): `addPlugin` 실패 시 `AlertWindow` 에 **플러그인 이름 + 파일명 + JUCE 에러 문자열** 표시. 미라이선스 Waves 가 대표 케이스 — 실기 확인 항목

### 4.4 UI Layout (§5 대체 — 데스크톱)

```
┌─ 플러그인 브라우저 ──────────────────────────┐
│ [검색: ________________]  [재스캔] [파일에서 추가] │
│ ┌──────────────────────────────────────┐ │
│ │ ▼ FabFilter                          │ │
│ │     Pro-Q 3                          │ │
│ │ ▼ Waves                              │ │
│ │     API 550A                         │ │
│ │     CLA-76                           │ │
│ │     ...                              │ │
│ └──────────────────────────────────────┘ │
│                          [추가]  [닫기]    │
└──────────────────────────────────────────┘
```

### 4.5 Page UI Checklist (gap-detector 검증용)

#### PluginBrowser

- [ ] 검색 TextEditor: 이름/제조사 부분 일치, 입력 즉시 필터
- [ ] TreeView: 제조사 그룹(접기/펼치기) + 플러그인 항목(이름 정렬)
- [ ] 버튼: 재스캔 (진행 모달: 현재 파일명 + 진행률)
- [ ] 버튼: 파일에서 추가 (FileChooser, 다중 클래스 시 PopupMenu)
- [ ] 버튼: 추가 (선택 항목), 더블클릭 동등 동작
- [ ] 빈 카탈로그 상태: "재스캔을 눌러 플러그인을 검색하세요" 안내

#### ChannelRow

- [ ] [+ VST3] 버튼 → 브라우저 열림 (기존 FileChooser 직행 제거)
- [ ] 추가 실패 시 AlertWindow: 플러그인 이름 + 파일 + 원인

---

## 6. Error Handling

| 상황 | 처리 |
|------|------|
| 스캔 중 플러그인 크래시 | dead-man's-pedal → 다음 스캔에서 해당 파일 자동 블랙리스트, 나머지 계속 |
| 인스턴스 생성 실패 (미라이선스 Waves 등) | AlertWindow: 이름+파일+원인. 체인은 무변경 유지 |
| 세션 uid 매칭 실패 (쉘 구성 변경) | 경로 스캔 성공이어도 findByFallback 진입 → uid 전체 스캔. 최종 실패 시 기존 세션 에러 목록에 표시 |
| 카탈로그 XML 손상 | load 실패 시 빈 목록으로 기동 (재스캔으로 복구), 크래시 금지 |
| 스캔 경로에 접근 불가 폴더 | 해당 폴더 스킵, 스캔 계속 |

---

## 7. Security Considerations

해당 없음 (로컬 데스크톱, 네트워크 무접촉). 스캔 대상은 사용자 지정 경로 + OS 표준 VST3 위치로 한정.

---

## 8. Test Plan

> L1 = SuperrackTests(JUCE UnitTest, CTest) 어서션. L2/L3 = 실기 수동 시나리오 (Waves 설치+라이선스 머신).

### 8.1 L1: 유닛 테스트 (Do 단계에서 코드와 함께 작성)

| # | Target | Test | Expected |
|---|--------|------|----------|
| 1 | PluginScanCache | 다중 desc 캐시 — 같은 경로 2회 scanPath | 2회째 스캔 없이 동일 목록 반환 |
| 2 | pickByUid | uid=0 / 일치 uid / deprecatedUid / 불일치 | 첫 항목 / 해당 항목 / 해당 항목 / nullptr |
| 3 | makeNode 경유 로드 | 단일 클래스 파일 + uid=0 (기존 세션 회귀) | 기존과 동일 로드 |
| 4 | PluginCatalog | save→load 라운드트립 (블랙리스트 포함) | 목록·블랙리스트 보존 |
| 5 | PluginBrowser 필터 모델 | 검색어 → 그룹/항목 필터 | 이름·제조사 부분 일치만 잔존 |
| 6 | 기존 84 어서션 | 전체 재실행 | 회귀 없음 |

### 8.2 L2/L3: 실기 시나리오 (Check 단계 실행)

| # | Scenario | Steps | Success Criteria |
|---|----------|-------|-----------------|
| 1 | WaveShell 다중 로드 | 재스캔 → Waves 그룹 확인 → 서브플러그인 A/B 를 Ch1/Ch2 에 추가 | 각각 올바른 플러그인 에디터·처리 동작 |
| 2 | 세션 복원 | 시나리오1 상태 저장 → 앱 재시작 | 동일 서브플러그인 복원 (uid 매칭) |
| 3 | 검색 | "CLA" 입력 | Waves 그룹 내 해당 항목만 표시 |
| 4 | 파일에서 추가 | WaveShell .vst3 직접 선택 | PopupMenu 로 서브플러그인 목록 → 선택 로드 |
| 5 | 미라이선스 에러 | 라이선스 없는 항목 추가 시도 | 이름 포함 에러 AlertWindow, 앱 정상 유지 |
| 6 | 캐시 기동 | 앱 재시작 | 재스캔 없이 브라우저 목록 즉시 표시 |
| 7 | 기존 세션 회귀 | 이전 사이클 세션(.superrack) 로드 | 전 채널 정상 복원 |

---

## 10. Coding Convention

기존 프로젝트 관례 유지: `Source/` 평면 구조, PascalCase 파일/클래스, 한국어 주석(설계 근거 중심, `// Design Ref: §N` 링크), 메시지 스레드 규약 주석 명기, JUCE LeakDetector 매크로. 신규 외부 의존성 없음.

---

## 11. Implementation Guide

### 11.1 File Structure

```
Source/
├── PluginCatalog.h/.cpp      (신규 — M2)
├── PluginBrowser.h/.cpp      (신규 — M3)
├── ChannelStrip.h/.cpp       (수정 — M1: scanCache/scanPath/makeNode/findByFallback/addPlugin 오버로드)
├── ChannelRow.h/.cpp         (수정 — M3: 브라우저 열기, FileChooser 이동)
└── CMakeLists.txt            (수정 — 신규 파일 등록, Superrack/SuperrackBeta/SuperrackTests)
Tests/                         (L1 추가 — 기존 테스트 타깃 구조 따름)
```

### 11.2 Implementation Order

1. [ ] M1: PluginScanCache 다중화 + pickByUid + makeNode/findByFallback 수정 + addPlugin(desc) — L1 #1~3, #6
2. [ ] M2: PluginCatalog (load/save/scanSync/scanSingleFile/pedal) — L1 #4
3. [ ] M3: PluginBrowser + ChannelRow 통합 + 에러 표면화 — L1 #5, 실기 시나리오
4. [ ] 실기 검증 (L2/L3 시나리오 1~7)

### 11.3 Session Guide

#### Module Map

| Module | Scope Key | Description | Estimated Turns |
|--------|-----------|-------------|:---------------:|
| 스캔 계층 다중화 | `module-1` | ChannelStrip: FR-01/02/06 + L1 테스트 | 15-20 |
| PluginCatalog | `module-2` | FR-04/07: 스캔·영속화·pedal + L1 테스트 | 15-20 |
| PluginBrowser + 통합 | `module-3` | FR-03/05/08: UI·ChannelRow·에러 표면화 | 20-25 |

#### Recommended Session Plan

| Session | Phase | Scope | 비고 |
|---------|-------|-------|------|
| 1 | Do | `--scope module-1,module-2` | 빌드+L1 통과까지 |
| 2 | Do | `--scope module-3` | 실기 1차 확인 포함 |
| 3 | Check + Report | 전체 | Waves 실기 시나리오 1~7 |

---

## Version History

| Version | Date | Changes | Author |
|---------|------|---------|--------|
| 0.1 | 2026-07-23 | Initial draft — Option C 확정 반영 | WaveSimm |

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include <vector>

//==============================================================================
/** 플러그인 카탈로그 (VST3 + macOS AudioUnit) — KnownPluginList + XML 영속화 +
    크래시 내성 스캔. Design Ref: waves-shell-support §3.2/§4.2

    - 기동 시 XML 로드만(자동 스캔 없음) — 스캔은 브라우저의 [재스캔]으로만.
      첫 실행(빈 카탈로그)에도 세션 복원은 ChannelStrip 의 기존 scanPath/폴백
      경로로 동작한다 — ChannelStrip 은 이 클래스에 의존하지 않는다 (§2.3).
    - 스캔은 파일별 별도 프로세스(out-of-process, "--scan-file" 워커 모드) —
      문제 플러그인이 멈추거나 죽어도 앱은 계속 진행. 진행 콜백으로 UI 갱신.
    - 타임아웃/크래시/무효 파일은 자동 블랙리스트 → 다음 스캔에서 제외.
    - GUI 무의존(juce_audio_processors 만) — 헤드리스 L1 테스트 대상.
    메시지 스레드 전용. */
class PluginCatalog
{
public:
    /** 생성 시 load(). 테스트는 로컬 인스턴스로 격리 검증(SUPERRACK_APPDATA). */
    PluginCatalog();

    /** 앱 전역 인스턴스 (AppSettings 와 동일 패턴 — 최초 접근 시 로드). */
    static PluginCatalog& get();

    juce::KnownPluginList& list() noexcept { return knownList; }

    /** 등록된 전 포맷 스캔 — VST3 는 표준 위치 + AppSettings 추가 경로, AU 는
        시스템에 등록된 AudioComponent 열거(경로 개념 없음). (메시지 스레드에서 호출,
        파일별 워커 프로세스 + 120초 타임아웃). onProgress(0..1, 현재 파일 경로) 가
        false 를 반환하면 중단. 완료 후 save().
        이미 스캔된 최신 항목·블랙리스트 파일은 건너뛴다. */
    void scanSync (const std::function<bool (float, const juce::String&)>& onProgress = nullptr);

    /** 워커 프로세스 진입점 — Main 이 "--scan-file <plugin> <outXml>" 인자로 호출.
        plugin 은 VST3 파일 경로 또는 AU 식별자. 1개를 로드해 PluginDescription XML 을
        outXml 에 쓴다. 반환값 = 종료 코드. */
    static int runScanWorker (const juce::String& pluginPath, const juce::String& outFilePath);

    /** 파일/식별자 1개 스캔 — "파일에서 추가" 경로. 결과를 카탈로그에도 병합(점진
        구축). 다중 클래스 파일이면 클래스 전부를 반환한다. */
    std::vector<juce::PluginDescription> scanSingleFile (const juce::String& path);

    /** 브라우저 검색 필터: 이름/제조사 부분 일치(대소문자 무시). 빈 검색어 = 전부. */
    static bool matchesFilter (const juce::PluginDescription& d, const juce::String& query);

    void load();
    void save();

    static juce::File catalogFile();        // <appdata>/plugin-catalog.xml
    static juce::File deadMansPedalFile();  // <appdata>/plugin-scan-inflight.txt

private:
    static juce::File appDataDir();

    juce::KnownPluginList          knownList;
    juce::AudioPluginFormatManager formats;   // VST3 (+ macOS AudioUnit)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginCatalog)
};

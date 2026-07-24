#include "PluginCatalog.h"
#include "AppSettings.h"

//==============================================================================
PluginCatalog::PluginCatalog()
{
    load();
}

PluginCatalog& PluginCatalog::get()
{
    static PluginCatalog instance;   // 최초 접근 시 XML 로드 (메시지 스레드)
    return instance;
}

//==============================================================================
juce::File PluginCatalog::appDataDir()
{
    // AppSettings::settingsFile 과 동일 규칙 — SUPERRACK_APPDATA 로 테스트 격리.
    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("SUPERRACK_APPDATA", "");
        dir.isNotEmpty())
        return juce::File (dir);

    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Superrack");
}

juce::File PluginCatalog::catalogFile()       { return appDataDir().getChildFile ("plugin-catalog.xml"); }
juce::File PluginCatalog::deadMansPedalFile() { return appDataDir().getChildFile ("plugin-scan-inflight.txt"); }

void PluginCatalog::load()
{
    knownList.clear();
    // 손상/부재 시 빈 목록으로 기동(재스캔으로 복구) — 크래시 금지 (Design §6).
    if (const auto xml = juce::XmlDocument::parse (catalogFile()))
        knownList.recreateFromXml (*xml);   // 블랙리스트 항목 포함 복원
}

void PluginCatalog::save()
{
    if (const auto xml = knownList.createXml())
    {
        auto f = catalogFile();
        f.getParentDirectory().createDirectory();
        xml->writeTo (f);
    }
}

//==============================================================================
int PluginCatalog::runScanWorker (const juce::String& pluginPath, const juce::String& outFilePath)
{
    // 워커 프로세스 본체 — 플러그인 로드는 크래시/행 위험이 있으므로 여기(자식
    // 프로세스)에서만 수행한다. 결과는 XML 파일로 부모에게 전달.
    juce::VST3PluginFormat fmt;
    juce::OwnedArray<juce::PluginDescription> types;
    fmt.findAllTypesForFile (types, pluginPath);

    juce::XmlElement root ("SCAN_RESULT");
    for (const auto* t : types)
        root.addChildElement (t->createXml().release());

    return root.writeTo (juce::File (outFilePath)) ? 0 : 1;
}

//==============================================================================
void PluginCatalog::scanSync (const std::function<bool (float, const juce::String&)>& onProgress)
{
    // Design §4.2 개정: 파일별 별도 프로세스(out-of-process) 스캔.
    // 문제 플러그인이 멈추거나 죽어도 워커 프로세스만 죽는다 — 앱은 다음 파일로
    // 진행. 타임아웃/크래시/무효 파일은 블랙리스트 → 다음 스캔에서 제외
    // (기존 dead-man's-pedal 을 대체하며, UI "응답 없음" 문제를 해결).
    auto locations = vst3Format.getDefaultLocationsToSearch();

    // 사용자 지정 추가 경로 — ChannelStrip::findByFallback 과 동일 규칙(표준보다 먼저).
    const auto extras = AppSettings::get().vst3ExtraPaths();
    for (int i = extras.size(); --i >= 0;)
        if (juce::File dir (extras[i]); dir.isDirectory())
            locations.add (dir, 0);

    // 전체 재스캔은 블랙리스트도 다시 평가 — 이전 회차의 타임아웃/크래시/환경 문제가
    // 영구 제외로 굳지 않게 한다. 진짜 문제 플러그인은 이번 회차에서 다시 걸러진다.
    knownList.clearBlacklistedFiles();

    const auto files = vst3Format.searchPathsForPlugins (locations, true /*recursive*/,
                                                         false /*async instantiation 불필요*/);
    const auto exe   = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

    // UAD 등 첫 로드에 하드웨어/인증 확인이 걸리는 플러그인 감안 — 넉넉히.
    constexpr juce::uint32 perFileTimeoutMs = 120'000;

    for (int i = 0; i < files.size(); ++i)
    {
        const auto& path = files[i];
        const float progress = (float) (i + 1) / (float) juce::jmax (1, files.size());

        if (onProgress != nullptr && ! onProgress (progress, path))
            break;

        if (knownList.getBlacklistedFiles().contains (path)
            || knownList.isListingUpToDate (path, vst3Format))
            continue;

        const auto tmpOut = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("superrack-scan", ".xml");

        juce::ChildProcess worker;
        bool aborted = false;

        juce::StringArray cmd;
        cmd.add (exe.getFullPathName());
        cmd.add ("--scan-file");
        cmd.add (path);
        cmd.add (tmpOut.getFullPathName());

        if (worker.start (cmd, 0))
        {
            const auto t0 = juce::Time::getMillisecondCounter();
            while (worker.isRunning())
            {
                if (juce::Time::getMillisecondCounter() - t0 > perFileTimeoutMs)
                {
                    worker.kill();   // 행(hang) — 아래에서 블랙리스트
                    break;
                }
                if (onProgress != nullptr && ! onProgress (progress, path))
                {
                    worker.kill();   // 사용자 중단
                    aborted = true;
                    break;
                }
                juce::Thread::sleep (30);
            }
        }

        bool gotResults = false;
        if (! aborted && worker.getExitCode() == 0)
            if (const auto xml = juce::XmlDocument::parse (tmpOut))
                for (const auto* e : xml->getChildIterator())
                {
                    juce::PluginDescription d;
                    if (d.loadFromXml (*e))
                    {
                        knownList.addType (d);
                        gotResults = true;
                    }
                }

        tmpOut.deleteFile();

        if (aborted)
            break;

        if (! gotResults)
            knownList.addToBlacklist (path);   // 크래시/타임아웃/무효 — 재스캔 시 제외
    }

    save();
}

std::vector<juce::PluginDescription> PluginCatalog::scanSingleFile (const juce::String& path)
{
    juce::OwnedArray<juce::PluginDescription> types;
    vst3Format.findAllTypesForFile (types, path);

    std::vector<juce::PluginDescription> out;
    out.reserve ((size_t) types.size());
    for (const auto* t : types)
    {
        knownList.addType (*t);   // 브라우저에도 즉시 반영 — 카탈로그 점진 구축
        out.push_back (*t);
    }

    if (! out.empty())
        save();
    return out;
}

bool PluginCatalog::matchesFilter (const juce::PluginDescription& d, const juce::String& query)
{
    const auto q = query.trim();
    return q.isEmpty()
        || d.name.containsIgnoreCase (q)
        || d.manufacturerName.containsIgnoreCase (q);
}

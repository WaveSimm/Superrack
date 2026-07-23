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
void PluginCatalog::scanSync (const std::function<bool (float, const juce::String&)>& onProgress)
{
    auto locations = vst3Format.getDefaultLocationsToSearch();

    // 사용자 지정 추가 경로 — ChannelStrip::findByFallback 과 동일 규칙(표준보다 먼저).
    const auto extras = AppSettings::get().vst3ExtraPaths();
    for (int i = extras.size(); --i >= 0;)
        if (juce::File dir (extras[i]); dir.isDirectory())
            locations.add (dir, 0);

    const auto pedal = deadMansPedalFile();
    pedal.getParentDirectory().createDirectory();

    // Plan RISK: 스캔 중 크래시한 파일은 pedal 에 남아 다음 스캔에서 자동
    // 블랙리스트된다 (JUCE dead-man's-pedal). 블랙리스트 파일은 건너뛴다.
    juce::PluginDirectoryScanner scanner (knownList, vst3Format, locations,
                                          true /*recursive*/, pedal,
                                          false /*async instantiation 불필요 (VST3/Win)*/);

    juce::String current;
    while (scanner.scanNextFile (true /*최신 항목 재스캔 안 함*/, current))
        if (onProgress != nullptr && ! onProgress (scanner.getProgress(), current))
            break;

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

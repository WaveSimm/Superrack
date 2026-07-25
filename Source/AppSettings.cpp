#include "AppSettings.h"

AppSettings& AppSettings::get()
{
    static AppSettings instance;   // 최초 접근 시 로드 (메시지 스레드)
    return instance;
}

AppSettings::AppSettings()
{
    root = juce::JSON::parse (settingsFile());
    if (! root.isObject())
        root = juce::var (new juce::DynamicObject());
}

juce::File AppSettings::settingsFile()
{
    // SUPERRACK_APPDATA: 설정 폴더 오버라이드 (테스트 격리·포터블 설치용)
    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("SUPERRACK_APPDATA", "");
        dir.isNotEmpty())
        return juce::File (dir).getChildFile ("app-settings.json");

    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Superrack").getChildFile ("app-settings.json");
}

void AppSettings::save() const
{
    auto f = settingsFile();
    f.getParentDirectory().createDirectory();
    f.replaceWithText (juce::JSON::toString (root));
}

//==============================================================================
juce::File AppSettings::storageRoot() const
{
    const auto custom = root.getProperty ("storageRoot", "").toString();
    if (custom.isNotEmpty())
        if (juce::File dir (custom); dir.isDirectory() || dir.createDirectory())
            return dir;

   #if JUCE_MAC
    // macOS: 문서 폴더는 TCC(개인정보 보호) 승인 대상 — 미서명 베타에서는 승인이
    // 유지되지 않아 녹음 파일 생성 실패의 원인이 된다. 승인 불필요한 음악 폴더 사용.
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
               .getChildFile ("Superrack");
   #else
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
               .getChildFile ("Superrack");
   #endif
}

void AppSettings::setStorageRoot (const juce::File& dir)
{
    root.getDynamicObject()->setProperty ("storageRoot",
        dir == juce::File() ? juce::String() : dir.getFullPathName());
    save();
}

juce::StringArray AppSettings::vst3ExtraPaths() const
{
    juce::StringArray out;
    if (auto* arr = root.getProperty ("vst3ExtraPaths", {}).getArray())
        for (auto& v : *arr)
            if (v.toString().isNotEmpty())
                out.add (v.toString());
    return out;
}

void AppSettings::setVst3ExtraPaths (const juce::StringArray& paths)
{
    juce::Array<juce::var> arr;
    for (auto& p : paths)
        if (p.trim().isNotEmpty())
            arr.add (p.trim());
    root.getDynamicObject()->setProperty ("vst3ExtraPaths", arr);
    save();
}

void AppSettings::setWorkerCountOverride (int n)
{
    root.getDynamicObject()->setProperty ("workerCount", juce::jlimit (0, 6, n));
    save();
}

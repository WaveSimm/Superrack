#include "PluginFormats.h"
#include "Util.h"

using sr::u8;

namespace sr::plugins
{
namespace
{
    const char* const auPrefix = "AudioUnit:";

    /** JUCE AudioUnitFormatHelpers::stringToOSType 과 동일 규칙 (4바이트 FourCC). */
    juce::uint32 stringToOSType (juce::String s)
    {
        if (s.trim().length() >= 4)     // 선행 공백이 의미를 갖는 코드를 지키려 길이 확인 후 trim
            s = s.trim();

        s += "    ";

        return (((juce::uint32) (unsigned char) s[0]) << 24)
             | (((juce::uint32) (unsigned char) s[1]) << 16)
             | (((juce::uint32) (unsigned char) s[2]) << 8)
             |  ((juce::uint32) (unsigned char) s[3]);
    }
}

//==============================================================================
bool isIdentifier (const juce::String& fileOrIdentifier)
{
    return fileOrIdentifier.startsWithIgnoreCase (auPrefix);
}

bool sameTarget (const juce::String& a, const juce::String& b)
{
    if (a.isEmpty() || b.isEmpty())
        return false;

    // AU 식별자는 문자열 자체가 정체성(머신 독립) — File 로 감싸면 안 된다.
    if (isIdentifier (a) || isIdentifier (b))
        return a.equalsIgnoreCase (b);

    // 다른 OS 에서 저장된 세션의 경로는 이 플랫폼 기준 절대 경로가 아닐 수 있다
    // (맥에서 본 "C:/..."). juce::File 로 감싸면 상대 경로로 해석되므로 문자열 비교.
    if (juce::File::isAbsolutePath (a) && juce::File::isAbsolutePath (b))
        return juce::File (a) == juce::File (b);

    return a == b;
}

int audioUnitUid (const juce::String& identifier)
{
    if (! isIdentifier (identifier))
        return 0;

    // "AudioUnit:Effects/aufx,FQ40,FabF" → 마지막 ':' 또는 '/' 뒤의 3개 FourCC.
    const auto tail = identifier.substring (juce::jmax (identifier.lastIndexOfChar (':'),
                                                        identifier.lastIndexOfChar ('/')) + 1);
    juce::StringArray tokens;
    tokens.addTokens (tail, ",", juce::StringRef());
    tokens.removeEmptyStrings();

    if (tokens.size() != 3)
        return 0;

    return (int) (stringToOSType (tokens[0]) ^ stringToOSType (tokens[1]) ^ stringToOSType (tokens[2]));
}

juce::String shortName (const juce::String& fileOrIdentifier)
{
    // AU 식별자: audioUnitUid 와 같은 규칙으로 FourCC 세 개만 남긴다.
    if (isIdentifier (fileOrIdentifier))
        return fileOrIdentifier.substring (juce::jmax (fileOrIdentifier.lastIndexOfChar (':'),
                                                       fileOrIdentifier.lastIndexOfChar ('/')) + 1);

    // 경로: 다른 OS 에서 저장된 세션도 파일명이 나와야 한다(윈도 세션을 맥에서 열면
    // 폴백 재탐색이 이 파일명으로 검색한다) — juce::File 은 이 플랫폼 구분자만 알므로
    // 양쪽 구분자를 직접 처리한다.
    return fileOrIdentifier.substring (juce::jmax (fileOrIdentifier.lastIndexOfChar ('/'),
                                                   fileOrIdentifier.lastIndexOfChar ('\\')) + 1);
}

//==============================================================================
juce::AudioPluginFormat* formatFor (juce::AudioPluginFormatManager& formats,
                                    const juce::String& fileOrIdentifier)
{
    if (fileOrIdentifier.isEmpty())
        return nullptr;

    for (int i = 0; i < formats.getNumFormats(); ++i)
        if (auto* fmt = formats.getFormat (i); fmt->fileMightContainThisPluginType (fileOrIdentifier))
            return fmt;

    return nullptr;
}

void findAllTypes (juce::AudioPluginFormatManager& formats,
                   juce::OwnedArray<juce::PluginDescription>& results,
                   const juce::String& fileOrIdentifier)
{
    if (auto* fmt = formatFor (formats, fileOrIdentifier))
        fmt->findAllTypesForFile (results, fileOrIdentifier);
}

juce::StringArray defaultLocationLines()
{
    juce::AudioPluginFormatManager formats;
    juce::addDefaultFormatsToManager (formats);

    juce::StringArray lines;

    for (int i = 0; i < formats.getNumFormats(); ++i)
    {
        auto* fmt = formats.getFormat (i);
        const auto locations = fmt->getDefaultLocationsToSearch();

        if (locations.getNumPaths() == 0)
        {
            // AU 는 파일 검색이 아니라 시스템에 등록된 AudioComponent 열거라
            // 검색 경로가 없다 — 사용자가 어디를 보는지 알 수 있게 표준 위치를 안내.
            if (fmt->getName() == "AudioUnit")
            {
                lines.add (u8 ("/Library/Audio/Plug-Ins/Components  (AU — 시스템 등록, 경로 추가 불가)"));
                lines.add (juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                               .getChildFile ("Library/Audio/Plug-Ins/Components").getFullPathName()
                           + u8 ("  (AU)"));
            }
            continue;
        }

        for (int p = 0; p < locations.getNumPaths(); ++p)
            lines.addIfNotAlreadyThere (locations[p].getFullPathName());
    }

    return lines;
}
}

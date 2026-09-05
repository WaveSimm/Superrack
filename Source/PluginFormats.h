#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

//==============================================================================
/** 호스팅 플러그인 포맷 추상화 — Windows: VST3 / macOS: VST3 + AudioUnit(AUv2).

    두 포맷은 플러그인을 가리키는 방식이 다르다:
      · VST3 — 파일 경로 ("/Library/Audio/Plug-Ins/VST3/Pro-Q 4.vst3")
      · AU   — 식별자   ("AudioUnit:Effects/aufx,FQ40,FabF")  ← 파일이 아니다
    세션 저장·카탈로그 조회·재탐색이 둘을 함께 다루려면 "경로 가정"을 지워야 한다.
    이 파일이 그 단일 창구다 — 다른 코드는 VST3PluginFormat 을 직접 만들지 않는다.

    AUv3(앱 확장)은 비동기 인스턴스화가 필요해 이 앱의 동기 로드 경로와 맞지 않아
    스캔 목록에서 제외된다 (searchPathsForPlugins 의 allowAsync=false).

    순수 문자열 함수(isIdentifier/sameTarget/audioUnitUid/shortName)는 스레드 무관,
    나머지는 메시지 스레드 전용. */
namespace sr::plugins
{
    /** fileOrIdentifier 를 맡을 수 있는 첫 포맷. 어느 것도 못 맡으면 nullptr. */
    juce::AudioPluginFormat* formatFor (juce::AudioPluginFormatManager& formats,
                                        const juce::String& fileOrIdentifier);

    /** 포맷 자동 판별 findAllTypesForFile — 다중 클래스(WaveShell)는 전부 반환. */
    void findAllTypes (juce::AudioPluginFormatManager& formats,
                       juce::OwnedArray<juce::PluginDescription>& results,
                       const juce::String& fileOrIdentifier);

    /** 파일 경로가 아닌 식별자인가 (AU). 파일 존재 여부와 무관한 순수 판정. */
    bool isIdentifier (const juce::String& fileOrIdentifier);

    /** 같은 플러그인을 가리키는가 — 식별자는 문자열 비교, 절대 경로는 juce::File
        비교(구분자·대소문자 정규화). 경로처럼 보이나 이 플랫폼 기준 절대 경로가
        아니면(다른 OS 에서 저장된 세션) 문자열 비교로 떨어진다. */
    bool sameTarget (const juce::String& a, const juce::String& b);

    /** AU 식별자에서 uid 계산 = componentType ^ SubType ^ Manufacturer —
        JUCE 가 PluginDescription::uniqueId 에 넣는 값과 동일하다. 플러그인을
        로드하지 않고 문자열만으로 얻으므로 uid 재탐색이 사실상 비용 0.
        AU 식별자가 아니면 0. */
    int audioUnitUid (const juce::String& identifier);

    /** 표시·에러 메시지용 짧은 이름 — 경로는 파일명, 식별자는 마지막 토큰. */
    juce::String shortName (const juce::String& fileOrIdentifier);

    /** 설정 창의 "기본 검색 위치" 표시줄. 경로 개념이 없는 포맷(AU)은 시스템이
        AudioComponent 를 등록하는 표준 위치를 안내 문구와 함께 넣는다. */
    juce::StringArray defaultLocationLines();
}

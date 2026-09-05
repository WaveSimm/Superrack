#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "ChannelStrip.h"

class MainComponent;
class ChannelRow;

//==============================================================================
/** 한 채널 행 안의 플러그인 1개(컴팩트 칩): 이름 + Bypass / 에디터 / 제거.
    랙 로우에서 가로로 나열된다. */
class PluginChip : public juce::Component
{
public:
    PluginChip (int pluginIndex, ChannelRow& rowRef);
    void paint (juce::Graphics&) override;
    void resized() override;

    /** 이름 실측폭 + 버튼 + 좌우 패딩 + 오른쪽 화살표 꼭짓점에 맞춘 실제 폭. */
    int getChipWidth() const noexcept { return (2 * padX) + nameW + gapAfterName + buttonsW + arrowW; }

    static constexpr int gapAfterName = 5;                 // 이름 끝 ~ 첫 버튼
    static constexpr int buttonsW     = 20 + 2 + 20 + 2 + 20;   // B E X
    static constexpr int padX         = 6;                 // 카드 좌우 내부 여백
    static constexpr int arrowW       = 11;                // 오른쪽 화살표(셰브런) 깊이

private:
    int index;
    ChannelRow& row;

    int              nameW = 40;      // 이름 실측폭 (ctor 에서 설정)
    juce::Label      nameLabel;
    juce::TextButton bypassButton { "B" };
    juce::TextButton editorButton { "E" };
    juce::TextButton removeButton { "X" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginChip)
};

//==============================================================================
/** 한 채널 = 가로 한 줄(랙 로우): 헤더 + 입력 미터 + 플러그인 체인(가로) + [+플러그인].
    세로로 쌓여 Viewport 안에서 스크롤된다. 32채널까지 동일 구조로 확장. */
class ChannelRow : public juce::Component
{
public:
    ChannelRow (int channelIndex, ChannelStrip& stripRef, MainComponent& ownerRef);

    void paint (juce::Graphics&) override;
    void resized() override;

    void setMeterLevel (float level);
    void rebuildChips();

    // PluginChip / 자식이 사용
    ChannelStrip&  getStrip() noexcept { return strip; }
    MainComponent& getOwner() noexcept { return owner; }
    void removePluginAndRefresh (int index);

    static constexpr int rowH = 28;

private:
    void openPluginBrowser();

    int channel;
    ChannelStrip& strip;
    MainComponent& owner;

    juce::Label      nameField;           // 편집 가능한 채널 이름 (세션 저장)
    juce::Slider     gainSlider;          // 채널 출력 게인 (dB)
    juce::TextButton muteButton { "M" };  // 재생 스템 뮤트 (라이브 입력 불가침, §5.12)
    juce::TextButton soloButton { "S" };  // 재생 스템 솔로 (가산식)
    juce::TextButton armButton  { "R" };  // 녹음 암 — 다음 녹음이 덮어쓸 채널
    juce::TextButton addButton { "+ FX" };   // VST3 / macOS AU — 포맷 무관
    juce::OwnedArray<PluginChip> chips;

    float meterLevel = 0.0f;
    juce::Rectangle<int> meterBounds;

    static constexpr int headerW  = 40;   // "Ch N"
    static constexpr int nameW    = 104;  // 편집 가능한 이름 필드
    static constexpr int meterW   = 96;
    static constexpr int gainW    = 72;   // 출력 게인 슬라이더
    static constexpr int msrW     = 20;   // M/S/R 토글 하나의 폭
    static constexpr int addW     = 72;
    static constexpr int arrowGap = 6;    // 셰브런 칩 사이 간격

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelRow)
};

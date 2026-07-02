#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>   // AudioDeviceSelectorComponent
#include "AudioEngine.h"
#include "CpuProfiler.h"
#include "PluginWindow.h"

class MainComponent;
class ChannelRow;

//==============================================================================
/** 오디오 장치 설정을 담는 별도 창 (툴바의 ⚙ 설정 버튼으로 토글).
    32채널 랙 뷰에서 본 화면 공간을 비우기 위해 장치 패널을 분리한다. */
class SettingsWindow : public juce::DocumentWindow
{
public:
    SettingsWindow (juce::AudioDeviceManager& dm, std::function<void()> onCloseCallback);
    void closeButtonPressed() override;

private:
    std::function<void()> onClose;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsWindow)
};

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
/** 한 채널 = 가로 한 줄(랙 로우): 헤더 + 입력 미터 + 플러그인 체인(가로) + [+VST3].
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
    void chooseAndAddPlugin();

    int channel;
    ChannelStrip& strip;
    MainComponent& owner;

    juce::Label      nameField;           // 편집 가능한 채널 이름 (세션 저장)
    juce::Slider     gainSlider;          // 채널 출력 게인 (dB)
    juce::TextButton addButton { "+ VST3" };
    juce::OwnedArray<PluginChip> chips;
    std::unique_ptr<juce::FileChooser> chooser;

    float meterLevel = 0.0f;
    juce::Rectangle<int> meterBounds;

    static constexpr int headerW  = 40;   // "Ch N"
    static constexpr int nameW    = 104;  // 편집 가능한 이름 필드
    static constexpr int meterW   = 96;
    static constexpr int gainW    = 72;   // 출력 게인 슬라이더
    static constexpr int addW     = 72;
    static constexpr int arrowGap = 6;    // 셰브런 칩 사이 간격

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelRow)
};

//==============================================================================
/** 메인 뷰: 얇은 툴바(설정/레이턴시/녹음) + 세로 스크롤 채널 랙. */
class MainComponent : public juce::Component,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // ── ChannelRow / PluginChip 가 호출 ──
    void openPluginEditor (juce::AudioPluginInstance* plugin);
    void closeEditorFor   (juce::AudioPluginInstance* plugin);
    /** 체인/게인 변경 시 호출 → 세션 자동 저장. */
    void notifySessionChanged();

private:
    void timerCallback() override;
    void openSettings();
    void layoutChannelContainer();
    void rebuildChannelRows();   // 활성 채널 수만큼 행 재생성

    AudioEngine engine;
    CpuProfiler profiler { engine };       // engine 뒤 선언 → engine 보다 먼저 소멸

    // 툴바
    juce::TextButton settingsButton { juce::CharPointer_UTF8 ("\xe2\x9a\x99 설정") };
    juce::TextButton sessionButton  { juce::CharPointer_UTF8 ("세션 \xe2\x96\xbe") };
    juce::TextButton latencyButton  { juce::CharPointer_UTF8 ("RT 레이턴시 (Out1\xe2\x86\x92In2)") };
    juce::TextButton profileButton  { juce::CharPointer_UTF8 ("CPU 프로파일") };
    juce::Label      latencyLabel;
    juce::Label      perfLabel;            // DSP 부하 / 장치 xrun
    int              lastLatencyState = -1;

    // ── 트랜스포트 행 (통합 타임라인) ──
    juce::TextButton   rewindButton;
    juce::TextButton   toEndButton;
    juce::TextButton   recordButton;
    juce::TextButton   playButton;
    juce::TextButton   stopButton;
    juce::Label        timeLabel;
    juce::Slider       positionSlider;
    juce::ToggleButton throughChainToggle { juce::CharPointer_UTF8 ("재생 시 플러그인 통과") };
    juce::Label        recordLabel;        // 녹음 상태(폴더/드롭)
    bool               seeking = false;
    int                lastTransportState = -1;
    void updateTransportUI();

    // ── 테이크 행 ──
    juce::ComboBox   takeSelector;
    juce::TextButton newTakeButton    { juce::CharPointer_UTF8 ("\xef\xbc\x8b 새 테이크") };
    juce::TextButton deleteTakeButton { juce::CharPointer_UTF8 ("삭제") };
    juce::Array<juce::File> takePaths;   // combo id-1 → 폴더
    void refreshTakes();

    // 세로 스크롤 채널 랙
    juce::Viewport   channelViewport;
    juce::Component   channelContainer;      // ChannelRow 들의 부모
    juce::OwnedArray<ChannelRow> channelRows;
    juce::Label      emptyHint;              // 활성 채널 0 일 때 안내

    std::unique_ptr<SettingsWindow>    settingsWindow;
    std::unique_ptr<juce::FileChooser> sessionChooser;
    void showSessionMenu();

    juce::OwnedArray<PluginWindow>     pluginWindows;   // engine 보다 먼저 소멸되도록 뒤에 선언

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

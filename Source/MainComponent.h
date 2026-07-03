#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>   // AudioDeviceSelectorComponent
#include "AudioEngine.h"
#include "ChannelRow.h"
#include "CpuProfiler.h"
#include "PluginWindow.h"

//==============================================================================
/** 오디오 장치 + 앱 설정(저장 위치/VST3 경로/워커 수)을 담는 별도 창
    (툴바의 ⚙ 설정 버튼으로 토글). */
class SettingsWindow : public juce::DocumentWindow
{
public:
    SettingsWindow (AudioEngine& engine, std::function<void()> onCloseCallback);
    void closeButtonPressed() override;

private:
    std::function<void()> onClose;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsWindow)
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
    juce::ToggleButton parallelToggle;     // 병렬 DSP on/off (A2)
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
    juce::TextButton undoButton;         // 녹음 undo/redo — 라벨은 상태에 따라 갱신
    bool             takeHasUndo = false;   // refreshTakes 에서 캐시 (매 틱 파일 조회 방지)
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

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
/** 진행바 위 녹음/펀치 구간 오버레이 — 마우스 통과, 페인트 전용 (백로그 E).
    각 record/punch 이력 구간을 하단 밴드로 표시, 마지막 커밋은 주황 강조. */
/** 진행바 위에 겹쳐 놓는 반복 구간 오버레이 (가상 리허설).

    진행바와 **같은 좌표계**를 써야 "반복 구간이 녹음 구간의 어디쯤인지"가 바로
    읽힌다. 그래서 별도 줄이 아니라 슬라이더와 같은 bounds 에 올리고,
    hitTest 로 **위쪽 밴드에서만** 마우스를 받는다 — 아래쪽은 그대로 통과해
    기존 시크(네비게이션)가 동작한다.

    위 밴드를 드래그하면 구간을 새로 잡고, 양 끝 핸들을 끌면 조정, 더블클릭이면 해제.
    드래그 중에는 표시만 갱신하고 엔진에는 **드래그가 끝날 때 한 번** 반영한다 —
    구간이 바뀔 때마다 리필을 강제하면 끄는 소리가 난다. */
class LoopRangeStrip : public juce::Component
{
public:
    std::function<void (double startSec, double endSec)> onRangeChanged;

    void setTotalSeconds (double s)
    {
        if (std::abs (s - totalSec) < 1.0e-9)
            return;
        totalSec = s;
        repaint();
    }

    void setRange (double s, double e)
    {
        if (dragMode != DragMode::none)
            return;   // 사용자가 잡고 있는 동안은 외부 갱신 무시
        startSec = s; endSec = e;
        repaint();
    }

    void setActive (bool b) { active = b; repaint(); }

    bool hasRange() const noexcept { return endSec > startSec; }

    /** 위쪽 밴드만 이 컴포넌트가 받는다. 나머지는 아래(진행바)로 통과 —
        JUCE 는 hitTest 가 false 인 지점을 이 컴포넌트가 없는 것처럼 다룬다. */
    bool hitTest (int x, int y) override;

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp   (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    enum class DragMode { none, create, moveStart, moveEnd };

    double xToSec (int x) const
    {
        if (totalSec <= 0.0 || getWidth() <= 0)
            return 0.0;
        return juce::jlimit (0.0, totalSec, (double) x / (double) getWidth() * totalSec);
    }
    float secToX (double s) const
    {
        if (totalSec <= 0.0)
            return 0.0f;
        return (float) (juce::jlimit (0.0, totalSec, s) / totalSec) * (float) getWidth();
    }
    void commit()
    {
        if (onRangeChanged != nullptr)
            onRangeChanged (juce::jmin (startSec, endSec), juce::jmax (startSec, endSec));
    }

    double   totalSec = 0.0, startSec = 0.0, endSec = 0.0, anchorSec = 0.0;
    bool     active = false;
    DragMode dragMode = DragMode::none;

    int bandHeight() const { return juce::jlimit (6, 12, getHeight() / 2); }

    static constexpr int handlePx = 6;   // 끝 핸들 히트 영역(픽셀)
};

//==============================================================================
class PunchStripOverlay : public juce::Component
{
public:
    PunchStripOverlay() { setInterceptsMouseClicks (false, false); }

    struct Range { double startSec = 0.0, endSec = 0.0; };

    void setRanges (juce::Array<Range> r, double totalSeconds)
    {
        ranges = std::move (r);
        totalSec = totalSeconds;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        if (totalSec <= 0.0 || ranges.isEmpty())
            return;

        const auto area = getLocalBounds().toFloat();
        const float y = area.getBottom() - 5.0f, h = 4.0f;

        for (int i = 0; i < ranges.size(); ++i)
        {
            const auto& r = ranges.getReference (i);
            const auto x1 = area.getX() + (float) (juce::jlimit (0.0, totalSec, r.startSec) / totalSec) * area.getWidth();
            const auto x2 = area.getX() + (float) (juce::jlimit (0.0, totalSec, r.endSec)   / totalSec) * area.getWidth();

            const bool isLast = (i == ranges.size() - 1);
            g.setColour (isLast ? juce::Colours::orange.withAlpha (0.90f)
                                : juce::Colours::grey.withAlpha (0.40f));
            g.fillRoundedRectangle (x1, y, juce::jmax (2.0f, x2 - x1), h, 2.0f);
        }
    }

private:
    juce::Array<Range> ranges;
    double totalSec = 0.0;
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
    PunchStripOverlay  punchOverlay;      // 진행바 위 녹음/펀치 구간 밴드
    LoopRangeStrip     loopStrip;         // 진행바 위 반복 구간 지정 스트립
    juce::TextButton   loopButton;        // 반복 on/off
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

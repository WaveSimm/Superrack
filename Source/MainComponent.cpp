#include "MainComponent.h"
#include "AppSettings.h"
#include "UiLookAndFeel.h"
#include "Util.h"

using sr::u8;
using sr::boldButtonLnf;

//==============================================================================
// AppSettingsPanel — 저장 위치 / VST3 추가 경로 / 병렬 워커 수 (범용화 설정)
//==============================================================================
class AppSettingsPanel : public juce::Component
{
public:
    explicit AppSettingsPanel (AudioEngine& eng) : engine (eng)
    {
        auto initLabel = [this] (juce::Label& l, const juce::String& text)
        {
            l.setText (text, juce::dontSendNotification);
            l.setFont (juce::FontOptions (13.0f));
            addAndMakeVisible (l);
        };

        // ── 저장 위치 ──
        initLabel (storageTitle, u8 ("녹음/테이크 저장 위치"));
        storagePath.setReadOnly (true);
        storagePath.setCaretVisible (false);
        addAndMakeVisible (storagePath);

        browseButton.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> (
                u8 ("녹음/테이크 저장 폴더 선택"), AppSettings::get().storageRoot());
            chooser->launchAsync (juce::FileBrowserComponent::openMode
                                      | juce::FileBrowserComponent::canSelectDirectories,
                [this] (const juce::FileChooser& fc)
                {
                    if (fc.getResult() == juce::File())
                        return;
                    AppSettings::get().setStorageRoot (fc.getResult());
                    refresh();
                    if (engine.onTakesChanged != nullptr)
                        engine.onTakesChanged();   // 테이크 목록이 새 루트 기준으로 갱신
                });
        };
        addAndMakeVisible (browseButton);

        defaultButton.onClick = [this]
        {
            AppSettings::get().setStorageRoot ({});
            refresh();
            if (engine.onTakesChanged != nullptr)
                engine.onTakesChanged();
        };
        addAndMakeVisible (defaultButton);

        initLabel (storageHint, u8 ("변경 시 새 테이크부터 적용 — 기존 테이크는 이전 위치에 남습니다. "
                                    "클라우드 동기화 폴더(OneDrive 등)는 녹음 중 I/O 간섭이 있어 로컬 드라이브 권장."));
        storageHint.setFont (juce::FontOptions (11.0f));
        storageHint.setColour (juce::Label::textColourId, juce::Colours::grey);

        // ── VST3 검색 경로 ──
        initLabel (vst3Title, u8 ("VST3 검색 경로 — 회색 = 기본(항상 검색, 편집 불가), 아래에 추가 (한 줄에 하나)"));

        // 기본 검색 위치: 입력 박스 상단에 회색 고정 줄로 표시 (읽기전용).
        {
            juce::VST3PluginFormat fmt;
            const auto defaults = fmt.getDefaultLocationsToSearch();
            juce::StringArray lines;
            for (int i = 0; i < defaults.getNumPaths(); ++i)
                lines.add (defaults[i].getFullPathName());
            numDefaultLines = juce::jmax (1, lines.size());

            vst3Defaults.setMultiLine (true);
            vst3Defaults.setReadOnly (true);
            vst3Defaults.setCaretVisible (false);
            vst3Defaults.setColour (juce::TextEditor::textColourId, juce::Colours::grey);
            vst3Defaults.setText (lines.joinIntoString ("\n"), juce::dontSendNotification);
            addAndMakeVisible (vst3Defaults);
        }

        vst3Paths.setMultiLine (true);
        vst3Paths.setReturnKeyStartsNewLine (true);
       #if JUCE_MAC
        vst3Paths.setTextToShowWhenEmpty (u8 ("예) /Volumes/External/VST3"), juce::Colours::grey);
       #else
        vst3Paths.setTextToShowWhenEmpty (u8 ("예) D:\\MyPlugins\\VST3"), juce::Colours::grey);
       #endif
        vst3Paths.onFocusLost = [this]
        {
            juce::StringArray lines;
            lines.addLines (vst3Paths.getText());
            lines.removeEmptyStrings (true);
            AppSettings::get().setVst3ExtraPaths (lines);
        };
        addAndMakeVisible (vst3Paths);

        // ── 병렬 워커 수 ──
        initLabel (workerTitle, u8 ("병렬 DSP 워커 수 (재시작 후 적용)"));
       #if JUCE_MAC
        workerCombo.addItem (u8 ("자동 (성능코어-2)"), 1);
       #else
        workerCombo.addItem (u8 ("자동 (물리코어-3)"), 1);
       #endif
        for (int n = 1; n <= 6; ++n)
            workerCombo.addItem (juce::String (n), n + 1);
        workerCombo.onChange = [this]
        {
            AppSettings::get().setWorkerCountOverride (workerCombo.getSelectedId() - 1);
        };
        addAndMakeVisible (workerCombo);

        refresh();
        setSize (540, 232 + 17 * numDefaultLines);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 8);

        storageTitle.setBounds (r.removeFromTop (20));
        auto row = r.removeFromTop (26);
        defaultButton.setBounds (row.removeFromRight (70));   row.removeFromRight (6);
        browseButton.setBounds  (row.removeFromRight (70));   row.removeFromRight (8);
        storagePath.setBounds (row);
        storageHint.setBounds (r.removeFromTop (30));
        r.removeFromTop (8);

        vst3Title.setBounds (r.removeFromTop (20));
        vst3Defaults.setBounds (r.removeFromTop (8 + 17 * numDefaultLines));
        vst3Paths.setBounds (r.removeFromTop (44).translated (0, -1));   // 경계 겹침 — 한 목록처럼
        r.removeFromTop (8);

        row = r.removeFromTop (26);
        workerTitle.setBounds (row.removeFromLeft (240));
        workerCombo.setBounds (row.removeFromLeft (170));
    }

private:
    void refresh()
    {
        storagePath.setText (AppSettings::get().storageRoot().getFullPathName(),
                             juce::dontSendNotification);
        vst3Paths.setText (AppSettings::get().vst3ExtraPaths().joinIntoString ("\n"),
                           juce::dontSendNotification);
        workerCombo.setSelectedId (
            juce::jlimit (0, 6, AppSettings::get().workerCountOverride()) + 1,
            juce::dontSendNotification);
    }

    AudioEngine& engine;
    juce::Label      storageTitle, storageHint, vst3Title, workerTitle;
    juce::TextEditor storagePath, vst3Defaults, vst3Paths;
    int              numDefaultLines = 1;
    juce::TextButton browseButton  { juce::CharPointer_UTF8 ("변경...") };
    juce::TextButton defaultButton { juce::CharPointer_UTF8 ("기본값") };
    juce::ComboBox   workerCombo;
    std::unique_ptr<juce::FileChooser> chooser;
};

//==============================================================================
// SettingsWindow — 장치 설정 + 앱 설정을 담는 별도 창
//==============================================================================
SettingsWindow::SettingsWindow (AudioEngine& engine, std::function<void()> onCloseCallback)
    : DocumentWindow (u8 ("설정"), juce::Colours::darkgrey, DocumentWindow::closeButton),
      onClose (std::move (onCloseCallback))
{
    setUsingNativeTitleBar (true);

    // 장치 셀렉터(위, 가변) + 앱 설정 패널(아래 고정) — 컨테이너가 자식 소유 + 리레이아웃
    struct OwningComponent : juce::Component
    {
        juce::Component* top = nullptr;
        juce::Component* bottom = nullptr;
        int bottomHeight = 0;

        void resized() override
        {
            auto r = getLocalBounds();
            if (bottom != nullptr) bottom->setBounds (r.removeFromBottom (bottomHeight));
            if (top != nullptr)    top->setBounds (r);
        }
        ~OwningComponent() override { deleteAllChildren(); }
    };

    auto* sel = new juce::AudioDeviceSelectorComponent (
        engine.getDeviceManager(), 1, AudioEngine::maxChannels, 1, AudioEngine::maxChannels,
        false, false, false, false);
    auto* panel = new AppSettingsPanel (engine);

    auto* content = new OwningComponent();
    content->top = sel;
    content->bottom = panel;
    content->bottomHeight = panel->getHeight();
    content->addAndMakeVisible (sel);
    content->addAndMakeVisible (panel);
    content->setSize (540, 404 + panel->getHeight());
    setContentOwned (content, true);

    setResizable (true, false);
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
    toFront (true);
}

void SettingsWindow::closeButtonPressed()
{
    if (onClose != nullptr)
        onClose();
}

//==============================================================================
// MainComponent
//==============================================================================
MainComponent::MainComponent()
{
    // 세션 복원은 메시지 루프 진입 후 비동기로 끝난다(AudioEngine::initialise 참조).
    // 완료 시 체인 행을 다시 그리고, 실패 항목이 있으면 알린다 — 조용히 넘기면
    // 짧아진 체인으로 계속 쓰다가 다음 자동 저장에서 원본이 덮여 사라진다.
    engine.onSessionRestored = [this]
    {
        rebuildChannelRows();

        if (const auto& errs = engine.getSessionRestoreErrors(); ! errs.isEmpty())
            juce::NativeMessageBox::showMessageBoxAsync (
                juce::MessageBoxIconType::WarningIcon,
                u8 ("세션 복원 — 일부 플러그인 실패"),
                errs.joinIntoString ("\n")
                    + u8 ("\n\n해당 슬롯은 비어 있습니다. 다시 추가하기 전에 저장하면"
                          " 이 체인이 그대로 기록됩니다."));
    };

    engine.initialise();

    // ── 툴바 ──
    settingsButton.onClick = [this] { openSettings(); };
    addAndMakeVisible (settingsButton);

    sessionButton.onClick = [this] { showSessionMenu(); };
    addAndMakeVisible (sessionButton);

    latencyButton.onClick = [this]
    {
        engine.startLatencyTest (0, 1);   // Out1 → In2
        latencyLabel.setText (u8 ("측정 중..."), juce::dontSendNotification);
        lastLatencyState = -1;
    };
    addAndMakeVisible (latencyButton);

    latencyLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    latencyLabel.setText (u8 ("Out1\xe2\x86\x92In2 케이블 후 측정"), juce::dontSendNotification);
    addAndMakeVisible (latencyLabel);

    // ── CPU 프로파일 (Phase 4: 합성 부하 2→32ch 단계 측정) ──
    profileButton.onClick = [this]
    {
        if (profiler.isRunning())
        {
            profiler.abort();
            profileButton.setButtonText (u8 ("CPU 프로파일"));
            return;
        }

        juce::String error;
        if (! profiler.start (error))
        {
            juce::NativeMessageBox::showMessageBoxAsync (
                juce::MessageBoxIconType::WarningIcon, u8 ("프로파일 시작 실패"), error);
            return;
        }
        profileButton.setButtonText (u8 ("\xe2\x96\xa0 중단"));
    };
    addAndMakeVisible (profileButton);

    profiler.onProgress = [this] (const juce::String& text)
    {
        latencyLabel.setText (text, juce::dontSendNotification);
    };

    profiler.onFinished = [this] (const CpuProfiler::Report& report)
    {
        profileButton.setButtonText (u8 ("CPU 프로파일"));

        juce::String msg;
        if (report.maxStableChannels >= AudioEngine::maxChannels)
            msg << u8 ("32채널 전 단계 안정 — 현 설정으로 32ch 여유 있음.\n\n");
        else if (report.maxStableChannels > 0)
            msg << u8 ("한계 채널 수: ") << report.maxStableChannels
                << u8 ("ch (xrun 0 · 예산 초과 0 · peak < 95% 기준)\n\n");
        else
            msg << u8 ("전 단계 불안정 — 버퍼 상향 또는 체인 경량화 필요.\n\n");
        msg << u8 ("리포트: ") << report.file.getFullPathName();

        latencyLabel.setText (u8 ("프로파일 완료 — ") + report.file.getFileName(),
                              juce::dontSendNotification);
        juce::NativeMessageBox::showMessageBoxAsync (
            juce::MessageBoxIconType::InfoIcon, u8 ("CPU 프로파일 완료"), msg);
    };

    // ── 병렬 DSP 토글 (A2) — 워커 수 표시, 끄면 직렬(비교 측정용) ──
    parallelToggle.setButtonText (u8 ("병렬 DSP ×") + juce::String (engine.getNumDspWorkers() + 1));
    parallelToggle.setToggleState (engine.isParallelDsp(), juce::dontSendNotification);
    parallelToggle.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    parallelToggle.setTooltip (u8 ("채널 병렬 처리 (워커 ") + juce::String (engine.getNumDspWorkers())
                               + u8 (" + 오디오 스레드). 끄면 직렬 — 프로파일 비교용."));
    parallelToggle.onClick = [this]
    {
        engine.setParallelDsp (parallelToggle.getToggleState());
    };
    addAndMakeVisible (parallelToggle);

    perfLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    perfLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (perfLabel);

    // ── 트랜스포트 컨트롤 행 ──
    rewindButton.setButtonText (juce::CharPointer_UTF8 ("\xe2\x8f\xae"));   // ⏮ 처음으로
    rewindButton.setLookAndFeel (&boldButtonLnf());
    rewindButton.onClick = [this] { engine.transportRewind(); updateTransportUI(); };
    addAndMakeVisible (rewindButton);

    toEndButton.setButtonText (juce::CharPointer_UTF8 ("\xe2\x8f\xad"));    // ⏭ 끝으로
    toEndButton.setLookAndFeel (&boldButtonLnf());
    toEndButton.onClick = [this] { engine.transportToEnd(); updateTransportUI(); };
    addAndMakeVisible (toEndButton);

    recordButton.setButtonText (juce::CharPointer_UTF8 ("\xe2\x97\x8f"));   // ●
    recordButton.setLookAndFeel (&boldButtonLnf());
    recordButton.onClick = [this]
    {
        if (engine.getTransportState() == AudioEngine::tsRecording)
            engine.transportStop();
        else
        {
            juce::String error;
            if (! engine.transportRecord (error))
                juce::NativeMessageBox::showMessageBoxAsync (
                    juce::MessageBoxIconType::WarningIcon, u8 ("녹음 시작 실패"), error);
        }
        updateTransportUI();
    };
    addAndMakeVisible (recordButton);

    playButton.setButtonText (juce::CharPointer_UTF8 ("\xe2\x96\xb6"));     // ▶
    playButton.setLookAndFeel (&boldButtonLnf());
    playButton.onClick = [this]
    {
        if (engine.getTransportState() == AudioEngine::tsPlaying)
            engine.transportStop();
        else
        {
            juce::String error;
            if (! engine.transportPlay (error))
                juce::NativeMessageBox::showMessageBoxAsync (
                    juce::MessageBoxIconType::WarningIcon, u8 ("재생 실패"), error);
        }
        updateTransportUI();
    };
    addAndMakeVisible (playButton);

    stopButton.setButtonText (juce::CharPointer_UTF8 ("\xe2\x96\xa0"));     // ■
    stopButton.setLookAndFeel (&boldButtonLnf());
    stopButton.onClick = [this] { engine.transportStop(); updateTransportUI(); };
    addAndMakeVisible (stopButton);

    timeLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    timeLabel.setJustificationType (juce::Justification::centred);
    timeLabel.setFont (juce::Font (juce::FontOptions (14.0f)));
    addAndMakeVisible (timeLabel);

    positionSlider.setSliderStyle (juce::Slider::LinearBar);
    positionSlider.setRange (0.0, 1.0, 0.0);
    positionSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    positionSlider.onDragStart = [this] { seeking = true; };
    positionSlider.onDragEnd   = [this]
    {
        engine.setPlayPositionSeconds (positionSlider.getValue());
        seeking = false;
        updateTransportUI();
    };
    addAndMakeVisible (positionSlider);
    addAndMakeVisible (punchOverlay);   // 슬라이더 뒤에 추가 = 위에 그려짐 (마우스 통과)

    throughChainToggle.setToggleState (engine.isPlaybackThroughChain(), juce::dontSendNotification);
    throughChainToggle.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    throughChainToggle.onClick = [this]
    {
        engine.setPlaybackThroughChain (throughChainToggle.getToggleState());
    };
    addAndMakeVisible (throughChainToggle);

    recordLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible (recordLabel);

    // ── 테이크 행 ──
    takeSelector.setTextWhenNothingSelected (u8 ("테이크 없음"));
    takeSelector.onChange = [this]
    {
        const int id = takeSelector.getSelectedId();
        if (id <= 0 || id > takePaths.size())
            return;
        const auto dir = takePaths[id - 1];
        if (dir == engine.getCurrentTakeDir())
            return;   // 이미 현재 테이크

        juce::String warning;
        engine.loadTake (dir, warning);
        rebuildChannelRows();     // 세션 자동복원 반영
        updateTransportUI();
        if (warning.isNotEmpty())
            juce::NativeMessageBox::showMessageBoxAsync (
                juce::MessageBoxIconType::WarningIcon, u8 ("환경 불일치"), warning);
    };
    addAndMakeVisible (takeSelector);

    newTakeButton.onClick = [this]
    {
        engine.newTake();
        refreshTakes();
        updateTransportUI();
    };
    addAndMakeVisible (newTakeButton);

    deleteTakeButton.setColour (juce::TextButton::buttonColourId, juce::Colours::darkred);
    deleteTakeButton.onClick = [this]
    {
        const int id = takeSelector.getSelectedId();
        if (id <= 0 || id > takePaths.size())
            return;
        const auto dir = takePaths[id - 1];

        const auto opts = juce::MessageBoxOptions()
                              .withIconType (juce::MessageBoxIconType::WarningIcon)
                              .withTitle (u8 ("테이크 삭제"))
                              .withMessage (u8 ("이 테이크의 녹음 파일이 영구 삭제됩니다.\n") + dir.getFileName())
                              .withButton (u8 ("삭제"))
                              .withButton (u8 ("취소"));
        juce::Component::SafePointer<MainComponent> safe (this);
        juce::NativeMessageBox::showAsync (opts, [safe, dir] (int result)
        {
            if (safe == nullptr || result != 0)   // 0 = 첫 버튼(삭제)
                return;
            safe->engine.deleteTake (dir);
            safe->refreshTakes();
            safe->updateTransportUI();
        });
    };
    addAndMakeVisible (deleteTakeButton);

    // 녹음 undo/redo — 커밋 직전 상태와 스왑 (실수한 녹음 즉시 복구)
    undoButton.onClick = [this]
    {
        juce::String err;
        if (! engine.undoRecording (err))
            juce::NativeMessageBox::showMessageBoxAsync (
                juce::MessageBoxIconType::WarningIcon, u8 ("녹음 복구"), err);
        rebuildChannelRows();   // 세션 스냅샷도 스왑됨 — 체인 UI 반영
        updateTransportUI();
    };
    addAndMakeVisible (undoButton);

    // 테이크 목록/현재 테이크 변화 시 콤보 갱신 (메시지 스레드로 안전하게)
    engine.onTakesChanged = [this]
    {
        juce::Component::SafePointer<MainComponent> safe (this);
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->refreshTakes(); });
    };

    // ── 세로 스크롤 채널 랙 ──
    channelViewport.setViewedComponent (&channelContainer, false);
    channelViewport.setScrollBarsShown (true, false);   // 세로만
    addAndMakeVisible (channelViewport);

    emptyHint.setText (u8 ("활성 채널 없음 — [⚙ 설정]에서 ASIO 장치와 입력 채널을 선택하세요."),
                       juce::dontSendNotification);
    emptyHint.setColour (juce::Label::textColourId, juce::Colours::grey);
    emptyHint.setJustificationType (juce::Justification::centred);
    channelContainer.addChildComponent (emptyHint);   // 필요 시에만 표시

    // 장치 활성 채널 수가 바뀌면 행 재생성 (메시지 스레드로 안전하게)
    engine.onChannelCountChanged = [this]
    {
        juce::Component::SafePointer<MainComponent> safe (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
                safe->rebuildChannelRows();
        });
    };

    rebuildChannelRows();   // 초기 행
    refreshTakes();

    updateTransportUI();
    setSize (860, 630);
    startTimerHz (30);

}

MainComponent::~MainComponent()
{
    engine.onChannelCountChanged = nullptr;   // 파괴 중 콜백 차단
    engine.onTakesChanged = nullptr;
    profiler.onProgress = nullptr;
    profiler.onFinished = nullptr;
    profiler.abort();                         // engine.shutdown() 전에 합성 부하 해제
    stopTimer();
    pluginWindows.clear();      // 플러그인보다 먼저 에디터 창 파괴
    settingsWindow.reset();
    channelRows.clear();
    engine.shutdown();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (8);

    // 툴바 (버튼 행)
    auto bar = area.removeFromTop (32);
    settingsButton.setBounds (bar.removeFromLeft (84));
    bar.removeFromLeft (8);
    sessionButton.setBounds (bar.removeFromLeft (84));
    bar.removeFromLeft (8);
    latencyButton.setBounds (bar.removeFromLeft (220));
    bar.removeFromLeft (8);
    profileButton.setBounds (bar.removeFromLeft (96));
    bar.removeFromLeft (8);
    parallelToggle.setBounds (bar.removeFromLeft (110));
    perfLabel.setBounds (bar.removeFromRight (juce::jmin (200, bar.getWidth())));   // 우측 DSP/xrun

    area.removeFromTop (6);

    // 트랜스포트 행: ⏮ ⏭ ● ▶ ■  시간  [진행바]  [플러그인 통과]
    auto tr = area.removeFromTop (30);
    rewindButton.setBounds (tr.removeFromLeft (38));  tr.removeFromLeft (4);
    toEndButton.setBounds  (tr.removeFromLeft (38));  tr.removeFromLeft (10);
    recordButton.setBounds (tr.removeFromLeft (38));  tr.removeFromLeft (4);
    playButton.setBounds   (tr.removeFromLeft (38));  tr.removeFromLeft (4);
    stopButton.setBounds   (tr.removeFromLeft (38));  tr.removeFromLeft (10);
    timeLabel.setBounds    (tr.removeFromLeft (150)); tr.removeFromLeft (8);
    throughChainToggle.setBounds (tr.removeFromRight (170));  tr.removeFromRight (8);
    const auto sliderArea = tr.reduced (0, 4);
    positionSlider.setBounds (sliderArea);
    punchOverlay.setBounds (sliderArea);

    area.removeFromTop (6);

    // 테이크 행: [테이크 셀렉터] [삭제] [＋ 새 테이크]
    auto takeRow = area.removeFromTop (26);
    newTakeButton.setBounds (takeRow.removeFromRight (110));
    takeRow.removeFromRight (6);
    deleteTakeButton.setBounds (takeRow.removeFromRight (60));
    takeRow.removeFromRight (6);
    undoButton.setBounds (takeRow.removeFromRight (110));
    takeRow.removeFromRight (8);
    takeSelector.setBounds (takeRow);

    area.removeFromTop (6);

    // 상태 행 (녹음 상태 | 레이턴시)
    auto status = area.removeFromTop (20);
    recordLabel.setBounds (status.removeFromLeft (status.getWidth() / 2));
    latencyLabel.setBounds (status);

    area.removeFromTop (6);

    channelViewport.setBounds (area);
    layoutChannelContainer();
}

void MainComponent::rebuildChannelRows()
{
    channelRows.clear();

    const int n = engine.getActiveChannelCount();
    for (int ch = 0; ch < n; ++ch)
        if (auto* strip = engine.getStrip (ch))
        {
            auto* rowc = new ChannelRow (ch, *strip, *this);
            channelContainer.addAndMakeVisible (rowc);
            channelRows.add (rowc);
        }

    layoutChannelContainer();
}

void MainComponent::layoutChannelContainer()
{
    const int n  = channelRows.size();
    const int sb = channelViewport.getScrollBarThickness();
    const int w  = juce::jmax (420, channelViewport.getWidth() - sb);

    channelContainer.setSize (w, juce::jmax (channelViewport.getHeight(), n * ChannelRow::rowH));

    for (int i = 0; i < n; ++i)
        channelRows[i]->setBounds (0, i * ChannelRow::rowH, w, ChannelRow::rowH);

    emptyHint.setVisible (n == 0);
    if (n == 0)
        emptyHint.setBounds (0, 0, w, 40);
}

void MainComponent::timerCallback()
{
    for (int ch = 0; ch < channelRows.size(); ++ch)
        channelRows[ch]->setMeterLevel (engine.getInputPeak (ch));

    updateTransportUI();

    // DSP 부하(실측 avg/peak) / 장치 xrun 표시
    const int xr = engine.getDeviceXRuns();
    const int dspAvg  = juce::roundToInt (engine.getDspLoadAvg()  * 100.0f);
    const int dspPeak = juce::roundToInt (engine.getDspLoadPeak() * 100.0f);
    const bool warn = xr > 0 || dspPeak >= 95;
    perfLabel.setColour (juce::Label::textColourId, warn ? juce::Colours::orange : juce::Colours::grey);
    perfLabel.setText ("DSP " + juce::String (dspAvg) + "% (pk " + juce::String (dspPeak)
                           + "%)   xrun " + juce::String (xr),
                       juce::dontSendNotification);

    // 레이턴시 측정 상태 변화 시 라벨 갱신
    const int state = engine.getLatencyState();
    if (state != lastLatencyState)
    {
        lastLatencyState = state;
        const double sr = engine.getCurrentSampleRate();
        const auto ms = [sr] (int s) { return sr > 0.0 ? (s * 1000.0 / sr) : 0.0; };

        if (state == AudioEngine::ltDone)
        {
            const int meas = engine.getLatencyResultSamples();
            const int drv  = engine.getDriverRoundTripSamples();

            juce::String txt;
            txt << u8 ("실측 RTL: ") << meas << " smp (" << juce::String (ms (meas), 2) << " ms)";
            if (drv >= 0)
                txt << u8 ("  |  드라이버: ") << drv << " smp (" << juce::String (ms (drv), 2) << " ms)";

            latencyLabel.setText (txt, juce::dontSendNotification);
        }
        else if (state == AudioEngine::ltTimeout)
        {
            latencyLabel.setText (u8 ("측정 실패: 신호 미검출 \xe2\x80\x94 Out1\xe2\x86\x92In2 케이블/게인 확인"),
                                  juce::dontSendNotification);
        }
    }
}

//==============================================================================
void MainComponent::openSettings()
{
    if (settingsWindow != nullptr)
    {
        settingsWindow->toFront (true);
        return;
    }

    settingsWindow = std::make_unique<SettingsWindow> (
        engine,
        [this] { juce::MessageManager::callAsync ([this] { settingsWindow.reset(); }); });
}

static juce::String fmtTime (double secs)
{
    if (secs < 0.0) secs = 0.0;
    const int mm = (int) (secs / 60.0);
    const double ss = secs - mm * 60.0;
    return juce::String::formatted ("%02d:%05.2f", mm, ss);
}

void MainComponent::updateTransportUI()
{
    const int  st  = engine.getTransportState();

    // 재생 자동 종료(콜백이 tsStopped 로) 감지 → playhead 를 끝 위치로 동기화
    if (lastTransportState == AudioEngine::tsPlaying && st == AudioEngine::tsStopped)
        engine.syncPlayheadFromPlayer();
    lastTransportState = st;

    const bool rec = (st == AudioEngine::tsRecording);
    const bool ply = (st == AudioEngine::tsPlaying);

    const auto defBtn = juce::LookAndFeel::getDefaultLookAndFeel()
                            .findColour (juce::TextButton::buttonColourId);
    recordButton.setColour (juce::TextButton::buttonColourId, rec ? juce::Colours::darkred    : defBtn);
    playButton.setColour   (juce::TextButton::buttonColourId, ply ? juce::Colours::darkgreen  : defBtn);

    // 시간 표시 pos / len
    const double pos = engine.getTimelineSeconds();
    const double len = engine.getTimelineLengthSeconds();
    timeLabel.setText (fmtTime (pos) + " / " + fmtTime (len), juce::dontSendNotification);

    // 진행바 (드래그 중이 아닐 때만 갱신)
    if (! seeking)
    {
        positionSlider.setRange (0.0, juce::jmax (0.001, len), 0.0);
        positionSlider.setValue (juce::jlimit (0.0, juce::jmax (0.001, len), pos),
                                 juce::dontSendNotification);
    }
    positionSlider.setEnabled (engine.isTakeLoaded() && ! rec);
    playButton.setEnabled (! rec);
    recordButton.setEnabled (! ply);

    undoButton.setEnabled (takeHasUndo && ! rec && ! ply);

    // 녹음 상태 라벨
    if (rec)
        recordLabel.setText (u8 ("녹음 중  드롭: ") + juce::String (engine.getRecordXruns()),
                             juce::dontSendNotification);
    else
    {
        juce::String txt;
        const auto dir = engine.getLastRecordDir();
        txt = (dir != juce::File() && dir.isDirectory()) ? u8 ("테이크: ") + dir.getFileName()
                                                         : u8 ("녹음 없음");
        // 재생 스트리밍 언더런(디스크 지연) 표면화 — 있으면 원인 추적 신호
        if (const int ur = engine.getPlayerUnderruns(); ply && ur > 0)
            txt << u8 ("  ·  재생 언더런 ") << ur;
        recordLabel.setText (txt, juce::dontSendNotification);
    }
}

void MainComponent::refreshTakes()
{
    takePaths.clear();
    takeSelector.clear (juce::dontSendNotification);

    const auto takes = engine.listTakes();
    const auto cur   = engine.getCurrentTakeDir();
    int selId = 0;

    for (int i = 0; i < takes.size(); ++i)
    {
        const auto& t = takes.getReference (i);
        takePaths.add (t.dir);

        // 녹음 시각(마지막 커밋) 을 사람이 읽기 좋게
        const auto rt = juce::Time::fromISO8601 (t.updatedAt);
        const juce::String when = rt.toMilliseconds() != 0 ? rt.formatted ("%m/%d %H:%M") : t.id;

        juce::String label;
        label << when << "  (" << t.env.channels << "ch, "
              << juce::String (t.env.sampleRate / 1000.0, 1) << "k, " << fmtTime (t.lengthSeconds());
        if (t.historyCount > 0)
            label << u8 (" · 녹음 ") << t.historyCount << u8 ("회");
        label << ")";
        takeSelector.addItem (label, i + 1);

        if (t.dir == cur)
            selId = i + 1;
    }
    takeSelector.setSelectedId (selId, juce::dontSendNotification);

    // 녹음 undo/redo 버튼 — 보존 상태 유무 + 방향 캐시 (테이크 변경 시에만 파일 조회)
    takeHasUndo = engine.canUndoRecording();
    undoButton.setButtonText (! takeHasUndo || engine.undoIsRestore()
                                  ? u8 ("\xe2\x9f\xb2 녹음 복구")      // ⟲ 직전 상태로
                                  : u8 ("\xe2\x9f\xb3 복구 취소"));    // ⟳ 다시 앞으로

    // 진행바 오버레이 — 현재 테이크의 녹음/펀치 구간 (테이크 변경 시에만 파일 조회)
    {
        const auto info = engine.getCurrentTakeInfo();
        const double sr = info.env.sampleRate > 0.0 ? info.env.sampleRate : 48000.0;

        juce::Array<PunchStripOverlay::Range> ranges;
        for (const auto& h : info.history)
            ranges.add ({ (double) h.startSample / sr, (double) h.endSample / sr });

        punchOverlay.setRanges (std::move (ranges), info.lengthSeconds());
    }
}

//==============================================================================
void MainComponent::notifySessionChanged()
{
    engine.autoSaveSession();
}

void MainComponent::showSessionMenu()
{
    juce::PopupMenu m;
    m.addItem (1, u8 ("세션 저장..."));
    m.addItem (2, u8 ("세션 불러오기..."));

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (sessionButton),
        [this] (int choice)
        {
            if (choice == 0)
                return;

            // 저장 위치 설정과 동일 루트 사용 (맥에서 문서 폴더 TCC 프롬프트 회피).
            const auto dir = AppSettings::get().storageRoot();
            dir.createDirectory();

            if (choice == 1)   // 저장
            {
                sessionChooser = std::make_unique<juce::FileChooser> (
                    u8 ("세션 저장"), dir.getChildFile ("session.superrack"), "*.superrack");
                sessionChooser->launchAsync (
                    juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                        | juce::FileBrowserComponent::warnAboutOverwriting,
                    [this] (const juce::FileChooser& fc)
                    {
                        const auto f = fc.getResult();
                        if (f == juce::File())
                            return;
                        if (! engine.saveSessionToFile (f.withFileExtension ("superrack")))
                            juce::NativeMessageBox::showMessageBoxAsync (
                                juce::MessageBoxIconType::WarningIcon, u8 ("저장 실패"), f.getFullPathName());
                    });
            }
            else if (choice == 2)   // 불러오기
            {
                sessionChooser = std::make_unique<juce::FileChooser> (
                    u8 ("세션 불러오기"), dir, "*.superrack");
                sessionChooser->launchAsync (
                    juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                    [this] (const juce::FileChooser& fc)
                    {
                        const auto f = fc.getResult();
                        if (f == juce::File())
                            return;

                        juce::StringArray errors;
                        engine.loadSessionFromFile (f, errors);
                        rebuildChannelRows();       // 로드된 체인/게인 반영
                        engine.autoSaveSession();   // 자동복원 상태도 갱신

                        if (! errors.isEmpty())
                            juce::NativeMessageBox::showMessageBoxAsync (
                                juce::MessageBoxIconType::WarningIcon,
                                u8 ("일부 플러그인 복원 실패"), errors.joinIntoString ("\n"));
                    });
            }
        });
}

void MainComponent::openPluginEditor (juce::AudioPluginInstance* plugin)
{
    if (plugin == nullptr)
        return;

    for (auto* w : pluginWindows)
        if (w->getPlugin() == plugin)
        {
            w->toFront (true);
            return;
        }

    auto* window = new PluginWindow (*plugin, [this] (PluginWindow* w)
    {
        pluginWindows.removeObject (w);
    });
    pluginWindows.add (window);
}

void MainComponent::closeEditorFor (juce::AudioPluginInstance* plugin)
{
    for (int i = pluginWindows.size(); --i >= 0;)
        if (pluginWindows[i]->getPlugin() == plugin)
            pluginWindows.remove (i);
}

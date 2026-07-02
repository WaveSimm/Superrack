#include "MainComponent.h"
#include "Util.h"

using sr::u8;

//==============================================================================
// 버튼 라벨(알파벳)을 굵게 렌더하는 LookAndFeel — ASCII 버튼(B/E/X, +VST3)용.
namespace
{
    struct BoldButtonLnf : juce::LookAndFeel_V4
    {
        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
        {
            return juce::Font (juce::FontOptions ((float) juce::jmin (16, buttonHeight) * 0.62f)
                                   .withStyle ("Bold"));
        }
    };

    BoldButtonLnf& boldButtonLnf()   // 정적 싱글턴 (컴포넌트보다 오래 삶)
    {
        static BoldButtonLnf lnf;
        return lnf;
    }
}

//==============================================================================
// SettingsWindow — 장치 설정을 담는 별도 창
//==============================================================================
SettingsWindow::SettingsWindow (juce::AudioDeviceManager& dm, std::function<void()> onCloseCallback)
    : DocumentWindow (u8 ("오디오 설정"), juce::Colours::darkgrey, DocumentWindow::closeButton),
      onClose (std::move (onCloseCallback))
{
    setUsingNativeTitleBar (true);

    auto* sel = new juce::AudioDeviceSelectorComponent (
        dm, 1, AudioEngine::maxChannels, 1, AudioEngine::maxChannels,
        false, false, false, false);
    sel->setSize (540, 420);
    setContentOwned (sel, true);

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
// PluginChip — 한 채널 행 안의 플러그인 1개 (컴팩트)
//==============================================================================
PluginChip::PluginChip (int pluginIndex, ChannelRow& rowRef)
    : index (pluginIndex), row (rowRef)
{
    auto& strip = row.getStrip();
    const auto name = strip.getPluginName (index);
    const juce::Font nameFont (juce::FontOptions (12.0f));

    // 이름 텍스트 실측폭 → 그 뒤 gapAfterName 만큼만 띄우고 버튼이 붙게.
    nameW = juce::jlimit (24, 150, juce::GlyphArrangement::getStringWidthInt (nameFont, name) + 2);

    nameLabel.setText (name, juce::dontSendNotification);
    nameLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    nameLabel.setFont (nameFont);
    nameLabel.setBorderSize (juce::BorderSize<int> (0));   // 기본 좌우 인셋 제거
    nameLabel.setMinimumHorizontalScale (0.85f);
    nameLabel.setTooltip (name);
    addAndMakeVisible (nameLabel);

    bypassButton.setLookAndFeel (&boldButtonLnf());
    editorButton.setLookAndFeel (&boldButtonLnf());
    removeButton.setLookAndFeel (&boldButtonLnf());

    bypassButton.setClickingTogglesState (true);
    bypassButton.setToggleState (strip.isBypassed (index), juce::dontSendNotification);
    bypassButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::orange);
    bypassButton.setTooltip (u8 ("Bypass"));
    bypassButton.onClick = [this]
    {
        row.getStrip().setBypass (index, bypassButton.getToggleState());
        row.getOwner().notifySessionChanged();
    };
    addAndMakeVisible (bypassButton);

    editorButton.setTooltip (u8 ("에디터 열기"));
    editorButton.onClick = [this]
    {
        row.getOwner().openPluginEditor (row.getStrip().getPlugin (index));
    };
    addAndMakeVisible (editorButton);

    removeButton.setColour (juce::TextButton::buttonColourId, juce::Colours::darkred);
    removeButton.setTooltip (u8 ("제거"));
    // 콜백 안에서 이 칩이 즉시 파괴되면 크래시 → 비동기 지연.
    removeButton.onClick = [&r = row, idx = index]
    {
        juce::MessageManager::callAsync ([&r, idx] { r.removePluginAndRefresh (idx); });
    };
    addAndMakeVisible (removeButton);
}

void PluginChip::paint (juce::Graphics& g)
{
    // 오른쪽 변이 뾰족한 화살표(셰브런) 모양 카드 → 신호 흐름 방향 표현
    auto b = getLocalBounds().toFloat().reduced (1.0f);
    const float aw = (float) arrowW;
    const float rx = b.getRight();
    const float my = b.getCentreY();

    juce::Path sharp;
    sharp.startNewSubPath (b.getX(), b.getY());
    sharp.lineTo          (rx - aw,  b.getY());
    sharp.lineTo          (rx,       my);          // 오른쪽 꼭짓점
    sharp.lineTo          (rx - aw,  b.getBottom());
    sharp.lineTo          (b.getX(), b.getBottom());
    sharp.closeSubPath();

    // 모든 모서리(뾰족한 끝점 포함)를 둥글게
    const juce::Path p = sharp.createPathWithRoundedCorners (5.0f);

    g.setColour (juce::Colours::grey.withAlpha (0.20f));
    g.fillPath (p);
    g.setColour (juce::Colours::grey.withAlpha (0.60f));
    g.strokePath (p, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
}

void PluginChip::resized()
{
    // 내부 여백(padX) 안, 오른쪽 화살표 꼭짓점(arrowW)은 비워둠: 이름 → gap → B/E/X
    auto r = getLocalBounds().reduced (padX, 2);
    r.removeFromRight (arrowW);
    nameLabel.setBounds (r.removeFromLeft (nameW));
    r.removeFromLeft (gapAfterName);
    bypassButton.setBounds (r.removeFromLeft (20));
    r.removeFromLeft (2);
    editorButton.setBounds (r.removeFromLeft (20));
    r.removeFromLeft (2);
    removeButton.setBounds (r.removeFromLeft (20));
}

//==============================================================================
// ChannelRow — 채널 = 가로 한 줄
//==============================================================================
ChannelRow::ChannelRow (int channelIndex, ChannelStrip& stripRef, MainComponent& ownerRef)
    : channel (channelIndex), strip (stripRef), owner (ownerRef)
{
    // 편집 가능한 채널 이름 (더블클릭 편집, 세션 저장)
    nameField.setText (strip.getChannelName(), juce::dontSendNotification);
    nameField.setEditable (false, true, false);   // 더블클릭 시 편집
    nameField.setColour (juce::Label::textColourId, juce::Colours::white);
    nameField.setColour (juce::Label::backgroundColourId, juce::Colours::white.withAlpha (0.06f));
    nameField.setColour (juce::Label::outlineColourId, juce::Colours::grey.withAlpha (0.35f));
    nameField.setFont (juce::Font (juce::FontOptions (13.0f)));
    nameField.setTooltip (u8 ("더블클릭해 채널 이름 입력 (세션 저장)"));
    nameField.onTextChange = [this]
    {
        strip.setChannelName (nameField.getText());
        owner.notifySessionChanged();
    };
    addAndMakeVisible (nameField);

    // 출력 게인 슬라이더 (LinearBar, dB 값 표시)
    gainSlider.setSliderStyle (juce::Slider::LinearBar);
    gainSlider.setRange (-60.0, 12.0, 0.1);
    gainSlider.setSkewFactorFromMidPoint (0.0);
    gainSlider.setValue (strip.getOutGainDb(), juce::dontSendNotification);
    gainSlider.setTextValueSuffix (" dB");
    gainSlider.setTooltip (u8 ("채널 출력 게인 (Dry 녹음엔 영향 없음)"));
    gainSlider.onValueChange = [this]
    {
        strip.setOutGainDb ((float) gainSlider.getValue());
    };
    gainSlider.onDragEnd = [this] { owner.notifySessionChanged(); };
    addAndMakeVisible (gainSlider);

    addButton.setLookAndFeel (&boldButtonLnf());
    addButton.onClick = [this] { chooseAndAddPlugin(); };
    addAndMakeVisible (addButton);

    rebuildChips();
}

void ChannelRow::paint (juce::Graphics& g)
{
    // 짝/홀 교차 배경 — 여러 채널 구분
    const auto base = juce::Colours::darkgrey.darker (0.6f);
    g.setColour ((channel % 2 == 0) ? base : base.brighter (0.12f));
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f);

    auto r = getLocalBounds().reduced (3, 2);
    auto header = r.removeFromLeft (headerW);
    g.setColour (juce::Colours::white);
    g.setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));
    g.drawText ("Ch " + juce::String (channel + 1), header, juce::Justification::centredLeft);

    if (! meterBounds.isEmpty())
    {
        g.setColour (juce::Colours::black);
        g.fillRect (meterBounds);
        const float level = juce::jlimit (0.0f, 1.0f, meterLevel);
        g.setColour (level > 0.9f ? juce::Colours::red : juce::Colours::limegreen);
        g.fillRect (meterBounds.withWidth (juce::roundToInt (meterBounds.getWidth() * level)));
        g.setColour (juce::Colours::grey);
        g.drawRect (meterBounds, 1);
    }

    // (체인 화살표는 각 칩 카드의 오른쪽 셰브런으로 표현 → PluginChip::paint)
}

void ChannelRow::resized()
{
    auto r = getLocalBounds().reduced (3, 2);
    r.removeFromLeft (headerW);          // "Ch N" (paint)
    r.removeFromLeft (4);
    nameField.setBounds (r.removeFromLeft (nameW).reduced (0, 2));   // 편집 가능한 이름
    r.removeFromLeft (8);
    meterBounds = r.removeFromLeft (meterW).reduced (0, 5);    // 미터 (paint)
    r.removeFromLeft (8);

    gainSlider.setBounds (r.removeFromLeft (gainW).reduced (0, 2));   // 출력 게인
    r.removeFromLeft (10);

    for (auto* chip : chips)
    {
        chip->setBounds (r.removeFromLeft (chip->getChipWidth()).reduced (0, 2));
        r.removeFromLeft (arrowGap);   // 칩 사이 간격
    }
    addButton.setBounds (r.removeFromLeft (addW).reduced (0, 2));
}

void ChannelRow::setMeterLevel (float level)
{
    meterLevel = level;
    if (! meterBounds.isEmpty())
        repaint (meterBounds);
}

void ChannelRow::rebuildChips()
{
    chips.clear();
    for (int i = 0; i < strip.getNumPlugins(); ++i)
    {
        auto* chip = new PluginChip (i, *this);
        addAndMakeVisible (chip);
        chips.add (chip);
    }
    resized();
}

void ChannelRow::removePluginAndRefresh (int index)
{
    if (auto* p = strip.getPlugin (index))
        owner.closeEditorFor (p);

    strip.removePlugin (index);
    rebuildChips();
    owner.notifySessionChanged();
}

void ChannelRow::chooseAndAddPlugin()
{
    juce::File initialDir ("C:/Program Files/Common Files/VST3");
    if (! initialDir.isDirectory())
        initialDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

    chooser = std::make_unique<juce::FileChooser> (u8 ("VST3 플러그인 선택"),
                                                   initialDir, "*.vst3");
    auto browserFlags = juce::FileBrowserComponent::openMode
                      | juce::FileBrowserComponent::canSelectFiles;

    chooser->launchAsync (browserFlags, [this] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File())
            return;

        juce::String error;
        if (strip.addPlugin (file, error))
        {
            rebuildChips();
            owner.notifySessionChanged();
        }
        else
        {
            juce::NativeMessageBox::showMessageBoxAsync (
                juce::MessageBoxIconType::WarningIcon, u8 ("플러그인 로드 실패"), error);
        }
    });
}

//==============================================================================
// MainComponent
//==============================================================================
MainComponent::MainComponent()
{
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
    perfLabel.setBounds (bar.removeFromRight (juce::jmin (220, bar.getWidth())));   // 우측 DSP/xrun

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
    positionSlider.setBounds (tr.reduced (0, 4));

    area.removeFromTop (6);

    // 테이크 행: [테이크 셀렉터] [삭제] [＋ 새 테이크]
    auto takeRow = area.removeFromTop (26);
    newTakeButton.setBounds (takeRow.removeFromRight (110));
    takeRow.removeFromRight (6);
    deleteTakeButton.setBounds (takeRow.removeFromRight (60));
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
        engine.getDeviceManager(),
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

    // 녹음 상태 라벨
    if (rec)
        recordLabel.setText (u8 ("녹음 중  드롭: ") + juce::String (engine.getRecordXruns()),
                             juce::dontSendNotification);
    else
    {
        const auto dir = engine.getLastRecordDir();
        if (dir != juce::File() && dir.isDirectory())
            recordLabel.setText (u8 ("테이크: ") + dir.getFileName(), juce::dontSendNotification);
        else
            recordLabel.setText (u8 ("녹음 없음"), juce::dontSendNotification);
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
              << juce::String (t.env.sampleRate / 1000.0, 1) << "k, " << fmtTime (t.lengthSeconds()) << ")";
        takeSelector.addItem (label, i + 1);

        if (t.dir == cur)
            selId = i + 1;
    }
    takeSelector.setSelectedId (selId, juce::dontSendNotification);
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

            const auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                 .getChildFile ("Superrack");
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

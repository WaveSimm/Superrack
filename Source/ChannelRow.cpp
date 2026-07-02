#include "ChannelRow.h"
#include "MainComponent.h"
#include "UiLookAndFeel.h"
#include "Util.h"

using sr::u8;
using sr::boldButtonLnf;

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

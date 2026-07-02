#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>

//==============================================================================
/** VST3 네이티브 에디터를 호스팅하는 데스크톱 창 (메시지 스레드).

    플러그인 인스턴스의 수명은 ChannelStrip 이 소유하므로, 이 창은 항상
    해당 플러그인보다 먼저 닫혀야 한다(소유자가 보장).
*/
class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioPluginInstance& pluginToEdit,
                  std::function<void (PluginWindow*)> onCloseCallback)
        : DocumentWindow (pluginToEdit.getName(),
                          juce::Colours::black,
                          DocumentWindow::closeButton),
          plugin (pluginToEdit),
          onClose (std::move (onCloseCallback))
    {
        setUsingNativeTitleBar (true);

        if (auto* editor = plugin.createEditorIfNeeded())
        {
            setContentOwned (editor, true);                 // 창 크기를 에디터에 맞춤
            setResizable (editor->isResizable(), false);
        }
        else
        {
            auto* label = new juce::Label ({}, juce::CharPointer_UTF8 ("이 플러그인은 GUI 에디터를 제공하지 않습니다."));
            label->setJustificationType (juce::Justification::centred);
            label->setSize (360, 120);
            setContentOwned (label, true);
        }

        setVisible (true);
        toFront (true);
    }

    /** 이 창이 편집 중인 플러그인 인스턴스. (제거 시 매칭용) */
    juce::AudioPluginInstance* getPlugin() const noexcept { return &plugin; }

    void closeButtonPressed() override
    {
        if (onClose != nullptr)
            onClose (this);
    }

private:
    juce::AudioPluginInstance& plugin;
    std::function<void (PluginWindow*)> onClose;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};

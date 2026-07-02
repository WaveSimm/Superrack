#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"

//==============================================================================
class SuperrackApplication : public juce::JUCEApplication
{
public:
    SuperrackApplication() = default;

    const juce::String getApplicationName() override       { return "Superrack"; }
    const juce::String getApplicationVersion() override    { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override             { return false; }

    void initialise (const juce::String& /*commandLine*/) override
    {
        // 한글 글리프 렌더링을 위해 기본 sans-serif 폰트를 한국어 지원 폰트로.
        juce::LookAndFeel::getDefaultLookAndFeel()
            .setDefaultSansSerifTypefaceName ("Malgun Gothic");

        mainWindow.reset (new MainWindow (getApplicationName()));
    }

    void shutdown() override
    {
        mainWindow = nullptr;   // 창 파괴 → MainComponent 소멸 → 오디오 정상 해제
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    //==========================================================================
    /** MainComponent 를 담는 데스크톱 윈도우. */
    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name,
                              juce::Desktop::getInstance().getDefaultLookAndFeel()
                                  .findColour (juce::ResizableWindow::backgroundColourId),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        // 닫기 버튼 → 앱 종료 요청 (shutdown 에서 오디오 정상 해제).
        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

//==============================================================================
START_JUCE_APPLICATION (SuperrackApplication)

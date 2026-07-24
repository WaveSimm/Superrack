#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"
#include "PluginCatalog.h"

#if SUPERRACK_BETA
 #include "BetaGate.h"
 #include "Util.h"
#endif

//==============================================================================
class SuperrackApplication : public juce::JUCEApplication
{
public:
    SuperrackApplication() = default;

    const juce::String getApplicationName() override
    {
       #if SUPERRACK_BETA
        return "Superrack Beta";
       #else
        return "Superrack";
       #endif
    }
    const juce::String getApplicationVersion() override    { return "0.1.0"; }
    // 스캔 워커("--scan-file")는 부모 앱과 동시에 떠야 하므로 단일 인스턴스 예외.
    // (예외 없이는 워커가 기존 인스턴스에 인자만 전달하고 즉시 종료 → 스캔 결과가 전부 빈다.)
    bool moreThanOneInstanceAllowed() override
    {
        return getCommandLineParameterArray().contains ("--scan-file");
    }

    void initialise (const juce::String& /*commandLine*/) override
    {
        // ── 스캔 워커 모드: "--scan-file <plugin> <outXml>" ───────────────────
        // 플러그인 로드는 크래시/행 위험이 있어 별도 프로세스(여기)에서만 수행.
        // 창·베타게이트·오디오를 만들지 않고 스캔 결과만 쓰고 즉시 종료한다.
        {
            const auto args = getCommandLineParameterArray();
            const int  idx  = args.indexOf ("--scan-file");
            if (idx >= 0 && idx + 2 < args.size())
            {
               #if JUCE_MAC
                juce::Process::setDockIconVisible (false);   // 독 아이콘 깜빡임 방지
               #endif
                setApplicationReturnValue (PluginCatalog::runScanWorker (args[idx + 1], args[idx + 2]));
                quit();
                return;
            }
        }

        // 한글 글리프 렌더링을 위해 기본 sans-serif 폰트를 한국어 지원 폰트로.
        juce::LookAndFeel::getDefaultLookAndFeel()
            .setDefaultSansSerifTypefaceName ("Malgun Gothic");

        juce::String title = getApplicationName();

       #if SUPERRACK_BETA
        // 베타 게이트: 이 머신 최초 실행일 + SUPERRACK_BETA_DAYS 일 후 실행 차단.
        const auto beta = BetaGate::checkAndTouch (SUPERRACK_BETA_DAYS);
        if (beta.expired)
        {
            juce::NativeMessageBox::showMessageBoxAsync (
                juce::MessageBoxIconType::InfoIcon,
                sr::u8 ("베타 기간 만료"),
                sr::u8 ("이 베타 빌드의 평가 기간(") + juce::String (SUPERRACK_BETA_DAYS)
                    + sr::u8 ("일)이 끝났습니다.\n정식 빌드를 사용하거나 새 베타를 받아주세요."),
                nullptr,
                juce::ModalCallbackFunction::create ([] (int) { JUCEApplication::getInstance()->quit(); }));
            return;   // 창을 만들지 않음 — 안내 후 종료
        }
        title << sr::u8 (" — 남은 기간 ") << beta.daysLeft << sr::u8 ("일");
       #endif

        mainWindow.reset (new MainWindow (title));
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

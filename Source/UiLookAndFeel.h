#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
namespace sr
{
    /** 버튼 라벨을 굵게 렌더하는 LookAndFeel — ASCII 버튼(B/E/X, +VST3)과
        트랜스포트 기호(⏮ ● ▶ ■)용. */
    struct BoldButtonLnf : juce::LookAndFeel_V4
    {
        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
        {
            return juce::Font (juce::FontOptions ((float) juce::jmin (16, buttonHeight) * 0.62f)
                                   .withStyle ("Bold"));
        }
    };

    /** 공용 싱글턴 (컴포넌트보다 오래 삶). */
    inline BoldButtonLnf& boldButtonLnf()
    {
        static BoldButtonLnf lnf;
        return lnf;
    }
}

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
namespace sr
{
    /** 버튼 라벨을 굵게 렌더하는 LookAndFeel — ASCII 버튼(B/E/X, +FX)과
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

    /** 트랜스포트 기호 전용(⏮ ⏭ ● ▶ ■) — 글리프가 아니라 **도형으로 직접** 그린다.
        macOS 에서 이 문자들은 서로 다른 폴백 폰트로 렌더돼 같은 폰트 크기에서도
        보이는 크기가 제각각이다. 도형은 공통 높이 하나로 정확히 맞는다. */
    struct TransportButtonLnf : juce::LookAndFeel_V4
    {
        void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool) override
        {
            const auto  r  = b.getLocalBounds().toFloat();
            const float h  = r.getHeight() * 0.40f;     // 아이콘 공통 높이
            const auto  c  = r.getCentre();
            const float y1 = c.y - h * 0.5f, y2 = c.y + h * 0.5f;

            g.setColour (b.findColour (juce::TextButton::textColourOffId)
                             .withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.5f));

            const auto t = b.getButtonText();
            juce::Path p;

            if (t == juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x8f")))        // ● 녹음
                g.fillEllipse (c.x - h * 0.5f, y1, h, h);
            else if (t == juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xa0")))   // ■ 정지
                g.fillRect (c.x - h * 0.5f, y1, h, h);
            else if (t == juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb6")))   // ▶ 재생
            {
                p.addTriangle (c.x - h * 0.42f, y1, c.x - h * 0.42f, y2, c.x + h * 0.58f, c.y);
                g.fillPath (p);
            }
            else if (t == juce::String (juce::CharPointer_UTF8 ("\xe2\x8f\xae")))   // ⏮ 처음으로
            {
                g.fillRect (c.x - h * 0.58f, y1, h * 0.18f, h);
                p.addTriangle (c.x + h * 0.58f, y1, c.x + h * 0.58f, y2, c.x - h * 0.28f, c.y);
                g.fillPath (p);
            }
            else if (t == juce::String (juce::CharPointer_UTF8 ("\xe2\x8f\xad")))   // ⏭ 끝으로
            {
                p.addTriangle (c.x - h * 0.58f, y1, c.x - h * 0.58f, y2, c.x + h * 0.28f, c.y);
                g.fillPath (p);
                g.fillRect (c.x + h * 0.40f, y1, h * 0.18f, h);
            }
            else
                juce::LookAndFeel_V4::drawButtonText (g, b, false, false);
        }
    };

    inline TransportButtonLnf& transportButtonLnf()
    {
        static TransportButtonLnf lnf;
        return lnf;
    }
}

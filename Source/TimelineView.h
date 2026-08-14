#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "TimelineSegments.h"

/*  타임라인 뷰 — 룰러 / 반복 구간 / 클립 3레인 (DESIGN §5.11).

    이전에는 진행바 한 줄에 슬라이더·녹음 이력·반복 구간이 같은 bounds 로 겹쳐
    hitTest 로 밴드를 나눠 공존했다. 여기서는 레인을 나눠 각자 자리를 준다 —
    히트 영역이 물리적으로 분리되므로 트릭이 필요 없다.

    **색은 상태를 나르지 않는다. 커서가 나른다.**
    채워지는 막대(LinearBar)를 없앴다 — 채워진 부분이 오디오처럼 읽혀 클립 레인과
    충돌한다. 재생 위치는 세 레인을 관통하는 세로 커서 하나뿐이고, 시크는 그 커서를
    끄는 것이다. 반복이 켜졌는지는 ↺ 버튼이 말하므로 스트립을 초록/회색으로 또
    말하지 않는다(같은 상태를 두 언어로 말하게 된다).
*/
class TimelineView : public juce::Component,
                     public juce::TooltipClient
{
public:
    TimelineView();

    //== 콜백 (전부 조작이 끝나는 시점에 한 번) ================================
    /** 시크 확정. 드래그 중에는 표시만 갱신하고 놓을 때 한 번 부른다. */
    std::function<void (double posSec)>                 onSeek;
    /** 시크 드래그 시작/끝 — 호출측의 `seeking` 플래그(타이머 역갱신 억제)용. */
    std::function<void (bool dragging)>                 onSeekDragChanged;
    /** 반복 구간 확정. 해제는 (0, 0). */
    std::function<void (double startSec, double endSec)> onLoopRangeChanged;

    //== 상태 주입 (타이머에서 매 틱) ==========================================
    void setLengthSeconds (double s);
    void setPositionSeconds (double s);            // 드래그 중이면 무시
    void setLoopRange (double s, double e);        // 드래그 중이면 무시

    /** 클립 레인 내용. `genLabels[gen]` = 그 회차의 사람이 읽는 시각(툴팁용). */
    void setSegments (juce::Array<sr::ClipSegment> segs, juce::StringArray genLabels);

    /** 지금 그려지는 커서 위치 — 시크 드래그 중에는 사용자가 쥔 값이다.
        호출측이 드래그 중 시간 표시를 커서에 맞출 때 쓴다. */
    double getPositionSeconds() const noexcept { return posSec; }

    //== Component ============================================================
    void paint (juce::Graphics& g) override;
    void mouseDown        (const juce::MouseEvent& e) override;
    void mouseDrag        (const juce::MouseEvent& e) override;
    void mouseUp          (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseMove        (const juce::MouseEvent& e) override;
    void mouseExit        (const juce::MouseEvent& e) override;

    juce::String getTooltip() override;

    /** 3레인이 필요로 하는 세로 크기 — MainComponent::resized 가 쓴다. */
    static constexpr int preferredHeight = 50;

private:
    //== 레인 =================================================================
    // 루프는 잡는 곳이라 손가락 폭이 필요하고, 클립은 보는 곳이라 얇아도 된다.
    enum { rulerH = 14, laneGap = 2, loopH = 14 };

    juce::Rectangle<int> rulerLane() const { return getLocalBounds().withHeight (rulerH); }
    juce::Rectangle<int> loopLane()  const { return getLocalBounds().withTop (rulerH + laneGap).withHeight (loopH); }
    juce::Rectangle<int> clipLane()  const { return getLocalBounds().withTop (rulerH + laneGap + loopH + laneGap); }

    //== 좌표 =================================================================
    double xToSec (float x) const;
    float  secToX (double s) const;

    /** 눈금 간격 — 라벨이 겹치지 않는 최소 후보. */
    double tickInterval() const;

    //== 조작 =================================================================
    enum class Drag { none, seek, loopCreate, loopStart, loopEnd };

    Drag  dragMode = Drag::none;
    double anchorSec = 0.0;

    void paintRuler (juce::Graphics&);
    void paintLoop  (juce::Graphics&);
    void paintClips (juce::Graphics&);
    void paintCursor (juce::Graphics&);

    /** 마우스 x 아래의 클립 세그먼트 (없으면 -1). */
    int segmentAt (float x) const;

    /** 커서(세로선+머리)가 차지하는 사각형 — 부분 repaint 용. */
    juce::Rectangle<int> cursorBounds() const;
    /** 커서만 움직였을 때: 이전+새 위치 사각형만 무효화 (30Hz 전체 리페인트 방지). */
    void moveCursorTo (double s);

    double totalSec = 0.0, posSec = 0.0, loopStartSec = 0.0, loopEndSec = 0.0;
    bool   hasLoop() const noexcept { return loopEndSec > loopStartSec; }

    juce::Array<sr::ClipSegment> segments;
    juce::StringArray            genLabels;

    juce::Point<int> lastMouse;
    bool             mouseInside = false;

    static constexpr int handlePx = 7;   // 삼각형 로케이터 히트 폭

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineView)
};

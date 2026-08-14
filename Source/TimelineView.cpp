#include "TimelineView.h"

//==============================================================================
namespace
{
    juce::String fmtClock (double secs)          // 툴팁용 mm:ss.SS
    {
        if (secs < 0.0) secs = 0.0;
        const int mm = (int) (secs / 60.0);
        return juce::String::formatted ("%02d:%05.2f", mm, secs - mm * 60.0);
    }

    juce::String fmtTick (double secs)           // 룰러 라벨 m:ss
    {
        if (secs < 0.0) secs = 0.0;
        const int total = (int) std::floor (secs + 0.5);
        return juce::String::formatted ("%d:%02d", total / 60, total % 60);
    }

    // 색은 구조만 나른다 — 상태는 커서와 버튼이 말한다.
    const auto colLaneBg   = juce::Colours::black.withAlpha (0.25f);
    const auto colTick     = juce::Colours::white.withAlpha (0.26f);
    const auto colTickText = juce::Colours::white.withAlpha (0.45f);
    const auto colClipOld  = juce::Colours::white.withAlpha (0.13f);   // 잔존 구간
    const auto colClipNew  = juce::Colour::fromRGB (198, 154, 106).withAlpha (0.50f);   // 마지막 회차(채도 낮춤)
    const auto colClipEdge = juce::Colours::black.withAlpha (0.55f);
    const auto colLoop     = juce::Colours::white.withAlpha (0.46f);   // 단일 색 — 활성 여부는 ↺ 버튼이
    const auto colLoopCol  = juce::Colours::white.withAlpha (0.05f);
    const auto colCursor   = juce::Colours::white.withAlpha (0.92f);   // 화면의 유일한 강조
}

//==============================================================================
TimelineView::TimelineView()
{
    setWantsKeyboardFocus (false);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

//==============================================================================
void TimelineView::setLengthSeconds (double s)
{
    s = juce::jmax (0.0, s);
    if (std::abs (s - totalSec) < 1.0e-9)
        return;
    totalSec = s;
    repaint();
}

juce::Rectangle<int> TimelineView::cursorBounds() const
{
    const int x   = juce::roundToInt (secToX (posSec));
    const int top = loopLane().getY();       // 커서 상단 = 루프 레인 가로줄
    return { x - 6, top, 13, getHeight() - top };
}

void TimelineView::moveCursorTo (double s)
{
    // 재생 중 30Hz 틱마다 뷰 전체(룰러 라벨·클립·로케이터)를 다시 그리지 않도록
    // 이전/새 커서 자리만 무효화한다 — VNC 같은 원격 화면에서 체감이 크다.
    const auto oldR = cursorBounds();
    posSec = s;
    repaint (oldR.getUnion (cursorBounds()));
}

void TimelineView::setPositionSeconds (double s)
{
    if (dragMode == Drag::seek)
        return;                              // 사용자가 커서를 쥐고 있다
    s = juce::jlimit (0.0, juce::jmax (0.0, totalSec), s);
    if (std::abs (s - posSec) < 1.0e-4)
        return;
    moveCursorTo (s);
}

void TimelineView::setLoopRange (double s, double e)
{
    if (dragMode == Drag::loopCreate || dragMode == Drag::loopStart || dragMode == Drag::loopEnd)
        return;                              // 사용자가 잡고 있는 동안은 외부 갱신 무시
    if (std::abs (s - loopStartSec) < 1.0e-9 && std::abs (e - loopEndSec) < 1.0e-9)
        return;
    loopStartSec = s; loopEndSec = e;
    repaint();
}

void TimelineView::setSegments (juce::Array<sr::ClipSegment> segs, juce::StringArray labels)
{
    segments  = std::move (segs);
    genLabels = std::move (labels);
    repaint();
}

//==============================================================================
double TimelineView::xToSec (float x) const
{
    if (totalSec <= 0.0 || getWidth() <= 0)
        return 0.0;
    return juce::jlimit (0.0, totalSec, (double) x / (double) getWidth() * totalSec);
}

float TimelineView::secToX (double s) const
{
    if (totalSec <= 0.0)
        return 0.0f;
    return (float) (juce::jlimit (0.0, totalSec, s) / totalSec) * (float) getWidth();
}

double TimelineView::tickInterval() const
{
    static const double candidates[] = { 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0 };

    if (totalSec <= 0.0 || getWidth() <= 0)
        return 0.0;

    // 라벨이 겹치지 않는 최소 간격 — 64px 확보.
    for (const double c : candidates)
        if ((c / totalSec) * (double) getWidth() >= 64.0)
            return c;

    return candidates[juce::numElementsInArray (candidates) - 1];
}

//==============================================================================
void TimelineView::paint (juce::Graphics& g)
{
    // 레인 바탕
    g.setColour (colLaneBg);
    g.fillRoundedRectangle (clipLane().toFloat(), 3.0f);

    if (totalSec <= 0.0)
    {
        g.setColour (juce::Colours::grey.withAlpha (0.55f));
        g.setFont (juce::Font (juce::FontOptions (10.5f)));
        g.drawText (juce::CharPointer_UTF8 ("테이크를 선택하면 타임라인이 표시됩니다"),
                    clipLane(), juce::Justification::centred, false);
        return;
    }

    paintRuler (g);
    paintLoop  (g);
    paintClips (g);
    paintCursor (g);       // 항상 맨 위 — 유일한 강조
}

void TimelineView::paintRuler (juce::Graphics& g)
{
    const auto lane = rulerLane().toFloat();
    const double iv = tickInterval();
    if (iv <= 0.0)
        return;

    g.setFont (juce::Font (juce::FontOptions (9.5f)));

    for (double t = 0.0; t <= totalSec + 1.0e-6; t += iv)
    {
        const float x = secToX (t);

        g.setColour (colTick);
        g.fillRect (x, lane.getBottom() - 4.0f, 1.0f, 4.0f);

        g.setColour (colTickText);
        g.drawText (fmtTick (t),
                    juce::Rectangle<float> (x + 3.0f, lane.getY(), 44.0f, lane.getHeight() - 3.0f)
                        .toNearestInt(),
                    juce::Justification::centredLeft, false);
    }
}

void TimelineView::paintLoop (juce::Graphics& g)
{
    const auto lane = loopLane().toFloat();

    if (! hasLoop())
    {
        g.setColour (juce::Colours::white.withAlpha (0.04f));
        g.fillRect (lane);
        g.setColour (juce::Colours::grey.withAlpha (0.50f));
        g.setFont (juce::Font (juce::FontOptions (9.0f)));
        g.drawText (juce::CharPointer_UTF8 ("드래그 = 반복 구간"),
                    lane.toNearestInt(), juce::Justification::centred, false);
        return;
    }

    const float x1 = secToX (juce::jmin (loopStartSec, loopEndSec));
    const float x2 = secToX (juce::jmax (loopStartSec, loopEndSec));

    // 클립 레인을 관통하는 옅은 기둥 — 구간이 어느 소리 위인지 정렬해 읽힌다.
    g.setColour (colLoopCol);
    g.fillRect (x1, (float) clipLane().getY(), juce::jmax (1.0f, x2 - x1), (float) clipLane().getHeight());

    // 두 로케이터를 잇는 가는 바 — 삼각형 윗변 높이에 맞춘다. 양 끝의 수직 경계선과
    // 만나 반복 구간이 직사각형 틀로 읽힌다.
    g.setColour (colLoop.withMultipliedAlpha (0.55f));
    g.fillRect (x1, lane.getY(), juce::jmax (1.0f, x2 - x1), 2.0f);

    // DAW 관행의 직각 삼각형 로케이터 — 수직변이 경계선, 윗변이 구간 안쪽으로.
    const float w = (float) handlePx, top = lane.getY(), bot = lane.getBottom();

    juce::Path start;
    start.startNewSubPath (x1, top);
    start.lineTo (x1 + w, top);
    start.lineTo (x1, bot);
    start.closeSubPath();

    juce::Path end;
    end.startNewSubPath (x2, top);
    end.lineTo (x2 - w, top);
    end.lineTo (x2, bot);
    end.closeSubPath();

    g.setColour (colLoop);
    g.fillPath (start);
    g.fillPath (end);

    // 로케이터에서 클립 레인 바닥까지 내려가는 경계선
    g.setColour (colLoop.withMultipliedAlpha (0.45f));
    g.fillRect (x1, lane.getY(), 1.0f, (float) (getHeight() - lane.getY()));
    g.fillRect (x2 - 1.0f, lane.getY(), 1.0f, (float) (getHeight() - lane.getY()));
}

void TimelineView::paintClips (juce::Graphics& g)
{
    const auto lane = clipLane().toFloat().reduced (0.0f, 1.0f);

    if (segments.isEmpty())
        return;

    const int lastGen = segments.getLast().gen;

    for (const auto& s : segments)
    {
        const float x1 = secToX (s.startSec);
        const float x2 = secToX (s.endSec);
        const float w  = juce::jmax (1.0f, x2 - x1);

        g.setColour (s.gen == lastGen ? colClipNew : colClipOld);
        g.fillRoundedRectangle (x1, lane.getY(), w, lane.getHeight(), 2.0f);

        // 회차가 바뀌는 지점 — 색 대비가 약하므로 경계를 명시한다.
        g.setColour (colClipEdge);
        g.fillRect (x1, lane.getY(), 1.0f, lane.getHeight());
    }
}

void TimelineView::paintCursor (juce::Graphics& g)
{
    const float x = secToX (posSec);

    // 커서 상단은 루프 레인의 가로줄(삼각형 윗변)에 맞춘다 — 룰러 숫자를 침범하지 않고,
    // 반복 구간 틀과 커서가 같은 선에서 시작해 정렬돼 보인다.
    const float yTop = (float) loopLane().getY();

    g.setColour (colCursor.withMultipliedAlpha (isEnabled() ? 1.0f : 0.45f));

    // 루프·클립 레인을 관통하는 세로선 = 재생 위치
    g.fillRect (x - 1.0f, yTop, 2.0f, (float) getHeight() - yTop);

    // 손잡이 머리 — 잡아 끄는 것임을 알린다.
    juce::Path head;
    head.startNewSubPath (x - 4.0f, yTop);
    head.lineTo (x + 4.0f, yTop);
    head.lineTo (x,        yTop + 5.0f);
    head.closeSubPath();
    g.fillPath (head);
}

//==============================================================================
int TimelineView::segmentAt (float x) const
{
    for (int i = 0; i < segments.size(); ++i)
        if (x >= secToX (segments.getReference (i).startSec) - 1.0f
         && x <= secToX (segments.getReference (i).endSec) + 1.0f)
            return i;
    return -1;
}

//==============================================================================
void TimelineView::mouseDown (const juce::MouseEvent& e)
{
    if (! isEnabled() || totalSec <= 0.0)
        return;

    if (loopLane().contains (e.getPosition()))
    {
        if (hasLoop())
        {
            const float x1 = secToX (juce::jmin (loopStartSec, loopEndSec));
            const float x2 = secToX (juce::jmax (loopStartSec, loopEndSec));

            if (std::abs ((float) e.x - x1) <= (float) handlePx) { dragMode = Drag::loopStart; return; }
            if (std::abs ((float) e.x - x2) <= (float) handlePx) { dragMode = Drag::loopEnd;   return; }
        }

        anchorSec = xToSec ((float) e.x);
        loopStartSec = loopEndSec = anchorSec;
        dragMode = Drag::loopCreate;
        repaint();
        return;
    }

    // 룰러/클립 어디를 눌러도 커서가 그리로 와서 잡힌다.
    dragMode = Drag::seek;
    if (onSeekDragChanged != nullptr)
        onSeekDragChanged (true);
    moveCursorTo (xToSec ((float) e.x));
}

void TimelineView::mouseDrag (const juce::MouseEvent& e)
{
    if (dragMode == Drag::none)
        return;

    const double s = xToSec ((float) e.x);
    lastMouse = e.getPosition();

    switch (dragMode)
    {
        case Drag::seek:       moveCursorTo (s); return;   // 커서 자리만 무효화
        case Drag::loopCreate: loopStartSec = juce::jmin (anchorSec, s);
                               loopEndSec   = juce::jmax (anchorSec, s); break;
        case Drag::loopStart:  loopStartSec = s; break;
        case Drag::loopEnd:    loopEndSec   = s; break;
        default: break;
    }

    repaint();
}

void TimelineView::mouseUp (const juce::MouseEvent&)
{
    const auto mode = dragMode;
    dragMode = Drag::none;

    if (mode == Drag::none)
        return;

    if (mode == Drag::seek)
    {
        if (onSeekDragChanged != nullptr) onSeekDragChanged (false);
        if (onSeek != nullptr)            onSeek (posSec);
        return;
    }

    // 클릭에 가까운 미세 드래그는 구간으로 치지 않는다(실수로 0길이 구간이 생김).
    if (std::abs (loopEndSec - loopStartSec) < 0.05)
        loopStartSec = loopEndSec = 0.0;

    repaint();
    if (onLoopRangeChanged != nullptr)
        onLoopRangeChanged (juce::jmin (loopStartSec, loopEndSec),
                            juce::jmax (loopStartSec, loopEndSec));
}

void TimelineView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! isEnabled() || ! loopLane().contains (e.getPosition()))
        return;

    loopStartSec = loopEndSec = 0.0;
    dragMode = Drag::none;
    repaint();
    if (onLoopRangeChanged != nullptr)
        onLoopRangeChanged (0.0, 0.0);
}

void TimelineView::mouseMove (const juce::MouseEvent& e)
{
    lastMouse = e.getPosition();
    mouseInside = true;
}

void TimelineView::mouseExit (const juce::MouseEvent&)
{
    mouseInside = false;
}

//==============================================================================
juce::String TimelineView::getTooltip()
{
    if (! mouseInside || totalSec <= 0.0)
        return {};

    if (loopLane().contains (lastMouse))
    {
        if (! hasLoop())
            return juce::CharPointer_UTF8 ("드래그해서 반복 구간을 지정합니다");

        const double s = juce::jmin (loopStartSec, loopEndSec);
        const double e = juce::jmax (loopStartSec, loopEndSec);
        return juce::String (juce::CharPointer_UTF8 ("반복 구간  "))
             + fmtClock (s) + " ~ " + fmtClock (e)
             + juce::String (juce::CharPointer_UTF8 ("  (")) + juce::String (e - s, 2)
             + juce::String (juce::CharPointer_UTF8 ("초)\n양 끝 삼각형 = 조정 · 더블클릭 = 해제"));
    }

    if (clipLane().contains (lastMouse))
    {
        const int i = segmentAt ((float) lastMouse.x);
        if (i < 0)
            return juce::CharPointer_UTF8 ("빈 구간 — 클릭하면 재생 위치를 옮깁니다");

        const auto& s = segments.getReference (i);

        juce::String t (s.gen + 1);
        t += juce::String (juce::CharPointer_UTF8 ("회차"));
        if (s.isPunch)
            t += juce::String (juce::CharPointer_UTF8 ("(펀치)"));
        t += "  " + fmtClock (s.startSec) + " ~ " + fmtClock (s.endSec)
           + juce::String (juce::CharPointer_UTF8 ("  ("))
           + juce::String (s.lengthSec(), 2)
           + juce::String (juce::CharPointer_UTF8 ("초)"));

        if (juce::isPositiveAndBelow (s.gen, genLabels.size()) && genLabels[s.gen].isNotEmpty())
            t += "\n" + genLabels[s.gen];

        return t;
    }

    return juce::String (juce::CharPointer_UTF8 ("재생 위치  ")) + fmtClock (xToSec ((float) lastMouse.x));
}

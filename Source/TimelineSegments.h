#pragma once

#include <algorithm>
#include "TakeManager.h"

/*  타임라인 클립 레인의 데이터 (DESIGN §5.11, §5.12).

    history 항목은 **대체된 구간** [startSample, endSample) 이다 (§5.12 — 커밋은
    꼬리를 보존하므로 구간 밖의 오디오는 그대로 남는다). 이력을 날것으로 그리면
    나중 커밋이 덮은 부분까지 옛 회차처럼 보이므로, 대체를 순서대로 적용해
    **지금 들리는 오디오의 구간 지도**를 계산한다.

    [10,15) 펀치가 [0,30) 위에 오면 결과는 셋이다: [0,10) 옛 회차 / [10,15) 새
    회차 / [15,30) 옛 회차 — 스팬하는 구간은 잘리는 게 아니라 **쪼개진다**.

    구버전 테이크(꼬리를 절단하던 시절) 호환: 그 시절 커밋은 길이를 자기 끝으로
    잘랐으므로, 결과를 totalSamples 로 클램프하면 옛 의미와 정확히 일치한다.

    gui 를 쓰지 않는 헤더 전용이라 L1 테스트 타깃에서 그대로 검증된다.
*/
namespace sr
{

struct ClipSegment
{
    double startSec = 0.0, endSec = 0.0;
    int    gen      = 0;      // 이 구간을 만든 history 인덱스(0 = 최초 녹음)
    bool   isPunch  = false;  // 그 회차가 펀치였는지 (툴팁용)

    double lengthSec() const noexcept { return endSec - startSec; }
};

/** history(커밋 순)에 구간 대체를 적용해 현재 유효 구간을 계산한다.
    결과는 시간 오름차순·무겹침.

    @param history       timeline.json 의 이력 (커밋된 순서 그대로)
    @param sampleRate    샘플→초 변환용. 0 이하면 빈 배열.
    @param totalSamples  0 초과면 결과를 이 길이로 클램프 — 구버전 테이크(꼬리
                         절단 시절)의 이력이 현재 길이 밖을 가리키는 것을 걸러낸다.
*/
inline juce::Array<ClipSegment> computeEffectiveSegments (const juce::Array<TakeManager::HistoryEntry>& history,
                                                          double sampleRate,
                                                          juce::int64 totalSamples = 0)
{
    juce::Array<ClipSegment> segs;

    if (sampleRate <= 0.0)
        return segs;

    for (int i = 0; i < history.size(); ++i)
    {
        const auto& h = history.getReference (i);

        const double s = (double) h.startSample / sampleRate;
        const double e = (double) h.endSample   / sampleRate;

        if (e <= s)
            continue;   // 길이 0 커밋은 지도에 남길 것이 없다

        // 이 커밋은 [s, e) 만 대체한다 — 겹치는 기존 구간을 자르거나 쪼갠다.
        for (int j = segs.size(); --j >= 0;)
        {
            auto& prev = segs.getReference (j);

            if (prev.endSec <= s || prev.startSec >= e)
                continue;                                   // 안 겹침

            if (prev.startSec < s && prev.endSec > e)
            {
                // 스팬 — 앞쪽은 남기고 뒤쪽을 새 구간으로 쪼갠다
                ClipSegment tail = prev;
                tail.startSec = e;
                prev.endSec = s;
                segs.insert (j + 1, tail);
            }
            else if (prev.startSec >= s && prev.endSec <= e)
                segs.remove (j);                            // 통째로 덮임
            else if (prev.startSec < s)
                prev.endSec = s;                            // 뒷부분만 잘림
            else
                prev.startSec = e;                          // 앞부분만 잘림
        }

        segs.add ({ s, e, i, h.op == "punch" });
    }

    // 시간 오름차순 정렬 (대체 삽입으로 순서가 섞인다)
    std::sort (segs.begin(), segs.end(),
               [] (const ClipSegment& a, const ClipSegment& b) { return a.startSec < b.startSec; });

    // 구버전 호환 클램프 — 현재 길이 밖의 이력은 이미 존재하지 않는 오디오다.
    if (totalSamples > 0)
    {
        const double lenSec = (double) totalSamples / sampleRate;
        for (int j = segs.size(); --j >= 0;)
        {
            auto& g = segs.getReference (j);
            if (g.startSec >= lenSec)      segs.remove (j);
            else if (g.endSec > lenSec)    g.endSec = lenSec;
        }
    }

    return segs;
}

} // namespace sr

#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
/** 베타 만료 게이트 — 머신별 최초 실행 시점부터 N일 후 실행 차단.

    기록: %APPDATA%/Superrack/.beta-state 파일 + HKCU 레지스트리 **이중 저장**
    (한쪽 삭제로는 리셋 안 됨 — 둘 중 이른 first 를 채택, 읽은 뒤 양쪽 재기록).
    시계 되돌림 감지: 오늘 < 마지막 실행일 이면 만료 처리.
    값은 체크섬 포함 난독 인코딩 — 우발적 편집 방지 수준이며 DRM 이 아님
    (베타 배포 관리 장치. 결심한 사용자는 우회 가능함을 전제).

    베타 빌드(SuperrackBeta 타깃, SUPERRACK_BETA=1)에서만 Main 이 호출한다. */
class BetaGate
{
public:
    struct Status
    {
        bool expired  = false;
        int  daysLeft = 0;      // 오늘 포함 잔여일 (만료면 0)
    };

    /** 앱 시작 시 1회(메시지 스레드): 기록 읽기 → 판정 → 기록 갱신. */
    static Status checkAndTouch (int trialDays);

    /** 순수 판정 로직 (단위 테스트용). 인자는 epoch 일 단위. */
    static Status evaluate (juce::int64 firstDay, juce::int64 lastDay,
                            juce::int64 today, int trialDays) noexcept;

private:
    struct Record { juce::int64 firstDay = 0, lastDay = 0; bool valid = false; };

    static juce::int64  todayEpochDays() noexcept;
    static juce::String encode (juce::int64 firstDay, juce::int64 lastDay);
    static Record       decode (const juce::String& s) noexcept;

    static juce::File   stateFile();
    static Record       readFile();
    static Record       readRegistry();
    static void         writeBoth (juce::int64 firstDay, juce::int64 lastDay);
};

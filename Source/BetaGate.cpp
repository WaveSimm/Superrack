#include "BetaGate.h"

//==============================================================================
BetaGate::Status BetaGate::evaluate (juce::int64 firstDay, juce::int64 lastDay,
                                     juce::int64 today, int trialDays) noexcept
{
    Status s;

    // 시계 되돌림 (하루 오차 허용 — 시간대/서머타임 여유)
    if (today + 1 < lastDay)
    {
        s.expired = true;
        return s;
    }

    const auto used = today - firstDay;              // 최초 실행일 = 0일차
    const auto left = (juce::int64) trialDays - used;
    s.daysLeft = (int) juce::jlimit ((juce::int64) 0, (juce::int64) trialDays, left);
    s.expired  = left <= 0;
    return s;
}

BetaGate::Status BetaGate::checkAndTouch (int trialDays)
{
    const auto today = todayEpochDays();

    const auto a = readFile();
    const auto b = readRegistry();

    juce::int64 first = today, last = today;
    if (a.valid || b.valid)
    {
        first = juce::jmin (a.valid ? a.firstDay : today, b.valid ? b.firstDay : today);
        last  = juce::jmax (a.valid ? a.lastDay  : 0,     b.valid ? b.lastDay  : 0);
    }

    const auto status = evaluate (first, last, today, trialDays);

    writeBoth (first, juce::jmax (last, today));   // 최초값 보존 + 마지막 실행 갱신
    return status;
}

//==============================================================================
juce::int64 BetaGate::todayEpochDays() noexcept
{
   #if JUCE_DEBUG
    // 테스트/QA 훅(Debug 전용): 시간 경과 시뮬레이션. Release 배포본에는 컴파일되지
    // 않는다 — 환경변수로 만료를 우회할 수 없다.
    if (const auto o = juce::SystemStats::getEnvironmentVariable ("SUPERRACK_BETA_TODAY", "");
        o.isNotEmpty())
        return o.getLargeIntValue();
   #endif

    return juce::Time::currentTimeMillis() / (24LL * 3600 * 1000);
}

// 인코딩: "first:last" 를 XOR 마스크 + FNV 체크섬으로 감싼 hex 문자열.
// 우발적 편집·복사 방지 수준의 난독화 — 암호학적 보호 아님.
static constexpr juce::uint64 kMask = 0x5eba11ab5eba11abULL;

juce::String BetaGate::encode (juce::int64 firstDay, juce::int64 lastDay)
{
    const juce::uint64 payload = ((juce::uint64) firstDay << 24) | (juce::uint64) lastDay;
    const juce::uint64 mixed   = payload ^ kMask;

    juce::uint64 sum = 1469598103934665603ULL;               // FNV-1a
    for (int i = 0; i < 8; ++i)
        sum = (sum ^ ((mixed >> (i * 8)) & 0xff)) * 1099511628211ULL;

    return juce::String::toHexString ((juce::int64) mixed) + "-"
         + juce::String::toHexString ((juce::int64) sum);
}

BetaGate::Record BetaGate::decode (const juce::String& s) noexcept
{
    Record r;
    const auto dash = s.indexOfChar ('-');
    if (dash <= 0)
        return r;

    const auto mixed = (juce::uint64) s.substring (0, dash).getHexValue64();
    const auto sum   = (juce::uint64) s.substring (dash + 1).getHexValue64();

    juce::uint64 expect = 1469598103934665603ULL;
    for (int i = 0; i < 8; ++i)
        expect = (expect ^ ((mixed >> (i * 8)) & 0xff)) * 1099511628211ULL;
    if (expect != sum)
        return r;   // 체크섬 불일치 = 손상/변조 → 무효(없는 것으로 취급)

    const auto payload = mixed ^ kMask;
    r.firstDay = (juce::int64) (payload >> 24);
    r.lastDay  = (juce::int64) (payload & 0xffffff);
    r.valid    = r.firstDay > 0 && r.lastDay > 0;
    return r;
}

//==============================================================================
juce::File BetaGate::stateFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Superrack").getChildFile (".beta-state");
}

BetaGate::Record BetaGate::readFile()
{
    return decode (stateFile().loadFileAsString().trim());
}

BetaGate::Record BetaGate::readRegistry()
{
   #if JUCE_WINDOWS
    return decode (juce::WindowsRegistry::getValue (
        "HKEY_CURRENT_USER\\Software\\Superrack\\BetaState", "").trim());
   #else
    return {};
   #endif
}

void BetaGate::writeBoth (juce::int64 firstDay, juce::int64 lastDay)
{
    const auto enc = encode (firstDay, lastDay);

    auto f = stateFile();
    f.getParentDirectory().createDirectory();
    f.replaceWithText (enc);

   #if JUCE_WINDOWS
    juce::WindowsRegistry::setValue ("HKEY_CURRENT_USER\\Software\\Superrack\\BetaState", enc);
   #endif
}

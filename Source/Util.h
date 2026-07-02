#pragma once

#include <juce_core/juce_core.h>
#include <atomic>

//==============================================================================
// JUCE 의 String(const char*) 는 8비트 ASCII 전용이라 한글 리터럴이 깨진다.
// 비ASCII 문자열은 반드시 이 헬퍼(또는 CharPointer_UTF8)로 UTF-8 디코딩한다.
namespace sr
{
    inline juce::String u8 (const char* utf8Literal)
    {
        return juce::String (juce::CharPointer_UTF8 (utf8Literal));
    }

    //==========================================================================
    /** 오디오 스레드 접근 가드 — 콜백 세대 카운터 기반 정밀 대기 (설계 R1).

        고정 sleep(30) 은 타이밍 의존(콜백이 30ms 안에 빠져나간다는 가정)이었다.
        대신 오디오 스레드가 콜백 진입마다 bump() 하고, 메시지 스레드는
        "차단 플래그를 켠 뒤" waitForIdle() 로 세대가 2 이상 전진할 때까지 기다린다:
        +1 = 플래그를 못 봤을 수 있는 in-flight 콜백, +2 = 플래그 이후 시작한 콜백
        ⇒ in-flight 콜백 종료가 보장된다. 콜백이 멈춘 상태(장치 정지)면 타임아웃으로
        복귀한다(기존 sleep 과 동일한 상한).

        스레드 규약: bump() 는 오디오 스레드 전용(무락·wait-free),
        waitForIdle() 은 메시지 스레드 전용. */
    class CallbackFence
    {
    public:
        void bump() noexcept { generation.fetch_add (1, std::memory_order_acq_rel); }

        void waitForIdle (int timeoutMs = 30) const noexcept
        {
            const auto start = generation.load (std::memory_order_acquire);
            const auto t0    = juce::Time::getMillisecondCounter();

            while ((int) (juce::Time::getMillisecondCounter() - t0) < timeoutMs)
            {
                if (generation.load (std::memory_order_acquire) >= start + 2)
                    return;
                juce::Thread::sleep (1);
            }
        }

    private:
        std::atomic<juce::uint64> generation { 0 };
    };
}

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

//==============================================================================
/** 테이크(녹음 단위) 관리: 테이크 폴더(트랙별 WAV) + timeline.json(길이·펀치구간·이력).

    - 테이크마다 폴더 하나: `<Documents>/Superrack/takes/<id>/chNN_dry.wav` + `timeline.json`.
    - 한 테이크 안에서 여러 번 녹음/펀치해도 파일은 트랙당 1개로 제자리 갱신.
    - timeline.json = 그 테이크의 SR·채널·길이 + 작업 이력(펀치 구간들).
    파일/메타데이터 전용(오디오 스레드와 무관, 메시지 스레드에서 호출).
*/
class TakeManager
{
public:
    struct HistoryEntry
    {
        juce::String op;          // "record" | "punch"
        juce::int64  startSample = 0;
        juce::int64  endSample   = 0;    // 대체된 구간 [start, end) — 꼬리는 보존된다 (§5.12)
        juce::String at;          // ISO 시각
        juce::Array<int> channels;  // 이 커밋이 만진 채널(0기준). 비어있음 = 전 채널(구버전 호환)
    };

    /** 녹음 시점의 장치 환경(설정 스냅샷). */
    struct TakeEnv
    {
        juce::String deviceName;
        double       sampleRate = 0.0;
        int          bufferSize = 0;
        int          channels   = 0;
    };

    struct TakeInfo
    {
        juce::File   dir;
        juce::String id;
        juce::String createdAt;
        juce::String updatedAt;      // 마지막 녹음 시각(커밋마다 갱신)
        TakeEnv      env;
        juce::int64  lengthSamples = 0;
        int          historyCount = 0;
        juce::Array<HistoryEntry> history;   // 각 record/punch 구간 (UI 시각화용)
        bool isValid() const { return dir.isDirectory(); }
        double lengthSeconds() const { return env.sampleRate > 0.0 ? (double) lengthSamples / env.sampleRate : 0.0; }
    };

    static juce::File takesRoot();

    /** 새 빈 테이크 폴더 생성 + timeline.json(환경) + session.json(세션 스냅샷). 폴더 반환. */
    juce::File createTake (const TakeEnv& env, const juce::var& sessionSnapshot);

    /** takes 폴더의 모든 테이크(최신순). */
    juce::Array<TakeInfo> listTakes() const;

    /** dir 의 timeline.json 을 읽어 TakeInfo. */
    TakeInfo readTake (const juce::File& dir) const;

    /** 테이크에 저장된 세션 스냅샷(session.json) — 로드 시 복원용. */
    juce::var readSession (const juce::File& takeDir) const;

    /** recTmp 의 새 녹음을 takeDir 에 커밋 — 구간 대체 의미 (DESIGN §5.12):
        armed 채널 = old[0..punch) + new + old[punch+N..oldEnd), 미선택 채널 = 불가침.
        테이크 길이는 절대 줄지 않는다(늘어남만 허용).
        트랙별 파일 제자리 갱신 + timeline.json(이력·환경) 갱신 + session.json 스냅샷 저장.
        커밋 직전 상태는 *.prev 로 보존된다(1단계 undo — swapUndoState).

        @param trimSamples       >0 이면 새 녹음을 이 길이로 절단 — 펀치아웃(out−in).
                                 정지 타이밍 지터와 무관하게 샘플 정확해진다.
        @param armedMask         비트 = 채널(ch01 = bit0). 기본 전 채널.
        @param srcOffsetSamples  새 녹음의 앞에서 건너뛸 샘플 — 펀치 패스의 프리롤
                                 (캡처는 패스 시작부터, 창은 in 부터). */
    void commitRecording (const juce::File& takeDir, const juce::File& recTmp,
                          juce::int64 punchSamples, const TakeEnv& env,
                          const juce::var& sessionSnapshot,
                          juce::int64 trimSamples = 0,
                          juce::uint32 armedMask = 0xffffffffu,
                          juce::int64 srcOffsetSamples = 0);

    //==========================================================================
    // ── 녹음 undo/redo (1단계, 커밋 직전 상태와 스왑) ─────────────────────────
    /** 직전 커밋 상태(*.prev)가 보존돼 있는가. */
    bool hasUndoState (const juce::File& takeDir) const;

    /** 현재 ↔ 직전 상태를 통째로 스왑(렌임 — 오디오 데이터 복사 없음).
        같은 호출이 undo/redo 토글이 된다. 성공 시 true.
        주의: 테이크 파일을 연 리더(재생기)는 호출 전에 해제할 것. */
    bool swapUndoState (const juce::File& takeDir) const;

    /** 현재 상태가 보존 상태보다 최신인가 — true 면 "실행취소", false 면 "다시실행". */
    bool isCurrentNewer (const juce::File& takeDir) const;

private:
    mutable juce::AudioFormatManager formatMgr;
    juce::WavAudioFormat wavFormat;

    void ensureFormats() const;
    static juce::File timelineFile (const juce::File& takeDir);
    static juce::var  readTimeline (const juce::File& takeDir);
    static void       writeTimeline (const juce::File& takeDir, const juce::var& v);

    JUCE_LEAK_DETECTOR (TakeManager)
};

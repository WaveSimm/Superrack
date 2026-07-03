#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
/** 앱 설정 — %APPDATA%/Superrack/app-settings.json (장치 설정 xml 과 별개).

    범용화(다른 머신/사용자) 대비 설정 계층. 모든 접근은 메시지 스레드
    (엔진·워커 풀 생성 시점 포함). 세터는 즉시 저장.

    환경변수 오버라이드(SUPERRACK_WORKERS/SUPERRACK_MMCSS)는 진단용으로
    유지되며 설정보다 우선한다.
*/
class AppSettings
{
public:
    static AppSettings& get();

    //== 저장 위치 (테이크/녹음 스크래치 루트) =================================
    /** 설정된 루트, 미설정이면 기본(<문서>/Superrack). 항상 유효 경로 반환. */
    juce::File storageRoot() const;
    bool isStorageRootCustom() const { return root.getProperty ("storageRoot", "").toString().isNotEmpty(); }
    /** 빈 File = 기본값 복귀. */
    void setStorageRoot (const juce::File& dir);

    //== VST3 추가 검색 경로 (표준 위치에 더해 폴백/재탐색에 사용) ==============
    juce::StringArray vst3ExtraPaths() const;
    void setVst3ExtraPaths (const juce::StringArray& paths);

    //== 병렬 DSP 워커 수 (0 = 자동 = 물리코어-3, 재시작 후 적용) ===============
    int  workerCountOverride() const { return (int) root.getProperty ("workerCount", 0); }
    void setWorkerCountOverride (int n);

private:
    AppSettings();
    static juce::File settingsFile();
    void save() const;

    juce::var root;   // DynamicObject — 알 수 없는 키는 보존

    JUCE_DECLARE_NON_COPYABLE (AppSettings)
};

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>

//==============================================================================
/** 플러그인 브라우저 — 제조사 트리 + 검색 + 재스캔 + 파일에서 추가.
    Design Ref: waves-shell-support §4.3/§4.4

    PluginBrowser::open() 으로 DialogWindow 표시. 항목 선택(더블클릭 또는 [추가])
    시 onPick(desc) 1회 호출 후 닫힌다 — desc 는 uid 포함이라 WaveShell 서브플러그인도
    정확히 로드된다. [파일에서 추가]는 스캔 경로 밖 플러그인용 보조 경로 —
    다중 클래스 파일이면 PopupMenu 로 서브플러그인을 고른다 (FR-05).
    메시지 스레드 전용. */
class PluginBrowser : public juce::Component
{
public:
    using PickFn = std::function<void (const juce::PluginDescription&)>;

    explicit PluginBrowser (PickFn onPickFn);
    ~PluginBrowser() override;

    /** 브라우저 다이얼로그 열기(비동기 모달). 선택 시 onPick 호출 후 자동 닫힘. */
    static void open (PickFn onPick);

    void resized() override;

    // ── TreeViewItem 이 사용 ──────────────────────────────────────────────
    void pickAndClose (const juce::PluginDescription& desc);
    void setSelectedDesc (const juce::PluginDescription& desc);

private:
    void rebuildTree();
    void rescan();
    void addFromFile();
    void closeDialog();

    PickFn onPick;

    juce::TextEditor searchBox;
    juce::TreeView   tree;
    juce::TextButton rescanButton, fileButton, addButton, closeButton;
    juce::Label      emptyHint;

    std::unique_ptr<juce::TreeViewItem> rootItem;
    std::unique_ptr<juce::FileChooser>  chooser;

    juce::PluginDescription selected;      // [추가] 버튼용 현재 선택
    bool hasSelection = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginBrowser)
};

#include "PluginBrowser.h"
#include "PluginCatalog.h"
#include "Util.h"

#include <algorithm>
#include <vector>

using sr::u8;

//==============================================================================
namespace
{
class PluginLeafItem : public juce::TreeViewItem
{
public:
    PluginLeafItem (PluginBrowser& b, juce::PluginDescription d)
        : browser (b), desc (std::move (d)) {}

    bool mightContainSubItems() override { return false; }
    int  getItemHeight() const override  { return 20; }

    void paintItem (juce::Graphics& g, int w, int h) override
    {
        if (isSelected())
            g.fillAll (juce::Colours::white.withAlpha (0.12f));

        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        g.drawText (desc.name, 6, 0, w - 10, h, juce::Justification::centredLeft);
    }

    void itemSelectionChanged (bool nowSelected) override
    {
        if (nowSelected)
            browser.setSelectedDesc (desc);
    }

    void itemDoubleClicked (const juce::MouseEvent&) override
    {
        browser.pickAndClose (desc);
    }

private:
    PluginBrowser& browser;
    juce::PluginDescription desc;
};

class ManufacturerItem : public juce::TreeViewItem
{
public:
    explicit ManufacturerItem (juce::String n) : name (std::move (n)) {}

    bool mightContainSubItems() override { return true; }
    int  getItemHeight() const override  { return 22; }

    void paintItem (juce::Graphics& g, int w, int h) override
    {
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));
        g.drawText (name, 4, 0, w - 8, h, juce::Justification::centredLeft);
    }

private:
    juce::String name;
};

struct RootItem : public juce::TreeViewItem
{
    bool mightContainSubItems() override { return true; }
    void paintItem (juce::Graphics&, int, int) override {}
};
}

//==============================================================================
PluginBrowser::PluginBrowser (PickFn onPickFn) : onPick (std::move (onPickFn))
{
    searchBox.setTextToShowWhenEmpty (u8 ("검색 (이름/제조사)"), juce::Colours::grey);
    searchBox.onTextChange = [this] { rebuildTree(); };
    addAndMakeVisible (searchBox);

    tree.setRootItemVisible (false);
    tree.setDefaultOpenness (true);   // 검색/스캔 후 그룹이 펼쳐진 채 보이게
    addAndMakeVisible (tree);

    rescanButton.setButtonText (u8 ("재스캔"));
    rescanButton.setTooltip (u8 ("표준 VST3 위치 + 설정의 추가 경로를 스캔합니다"));
    rescanButton.onClick = [this] { rescan(); };

    fileButton.setButtonText (u8 ("파일에서 추가"));
    fileButton.setTooltip (u8 ("스캔 경로 밖의 .vst3 파일을 직접 선택합니다"));
    fileButton.onClick = [this] { addFromFile(); };

    addButton.setButtonText (u8 ("추가"));
    addButton.onClick = [this] { if (hasSelection) pickAndClose (selected); };

    closeButton.setButtonText (u8 ("닫기"));
    closeButton.onClick = [this] { closeDialog(); };

    for (auto* b : { &rescanButton, &fileButton, &addButton, &closeButton })
        addAndMakeVisible (b);

    emptyHint.setText (u8 ("카탈로그가 비어 있습니다 — [재스캔]으로 플러그인을 검색하세요"),
                       juce::dontSendNotification);
    emptyHint.setJustificationType (juce::Justification::centred);
    emptyHint.setColour (juce::Label::textColourId, juce::Colours::grey);
    addChildComponent (emptyHint);

    setSize (520, 480);
    rebuildTree();
}

PluginBrowser::~PluginBrowser()
{
    tree.setRootItem (nullptr);   // rootItem 파괴 전 분리
}

void PluginBrowser::open (PickFn onPick)
{
    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned (new PluginBrowser (std::move (onPick)));
    opts.dialogTitle = u8 ("플러그인 브라우저");
    opts.dialogBackgroundColour = juce::Colours::darkgrey.darker (0.7f);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = true;
    opts.resizable = true;
    opts.launchAsync();   // 닫힐 때 자동 파괴
}

void PluginBrowser::resized()
{
    auto r = getLocalBounds().reduced (8);

    auto top = r.removeFromTop (26);
    rescanButton.setBounds (top.removeFromRight (76));
    top.removeFromRight (6);
    fileButton.setBounds (top.removeFromRight (110));
    top.removeFromRight (6);
    searchBox.setBounds (top);

    r.removeFromTop (6);

    auto bottom = r.removeFromBottom (28);
    closeButton.setBounds (bottom.removeFromRight (76));
    bottom.removeFromRight (6);
    addButton.setBounds (bottom.removeFromRight (76));

    r.removeFromBottom (6);
    tree.setBounds (r);
    emptyHint.setBounds (r);
}

//==============================================================================
void PluginBrowser::rebuildTree()
{
    hasSelection = false;
    tree.setRootItem (nullptr);
    rootItem = std::make_unique<RootItem>();

    const auto query = searchBox.getText();

    // 필터 → 제조사/이름 정렬 → 제조사 그룹 트리 (Design §4.3)
    std::vector<juce::PluginDescription> v;
    for (const auto& t : PluginCatalog::get().list().getTypes())
        if (PluginCatalog::matchesFilter (t, query))
            v.push_back (t);

    std::sort (v.begin(), v.end(), [] (const auto& a, const auto& b)
    {
        const int m = a.manufacturerName.compareIgnoreCase (b.manufacturerName);
        return m != 0 ? m < 0 : a.name.compareIgnoreCase (b.name) < 0;
    });

    juce::TreeViewItem* group = nullptr;
    juce::String        groupName;
    for (auto& d : v)
    {
        const auto manu = d.manufacturerName.isNotEmpty() ? d.manufacturerName
                                                          : juce::String (u8 ("(기타)"));
        if (group == nullptr || groupName != manu)
        {
            group = new ManufacturerItem (manu);
            groupName = manu;
            rootItem->addSubItem (group);
        }
        group->addSubItem (new PluginLeafItem (*this, d));
    }

    tree.setRootItem (rootItem.get());
    emptyHint.setVisible (v.empty());
}

void PluginBrowser::rescan()
{
    // Design Ref: §4.2 — 동기 스캔(사용자 결정) + 진행 표시. WaveShell 은 파일당
    // 수 초 걸릴 수 있어 파일마다 메시지 루프를 잠깐 펌핑해 진행창을 갱신한다.
    setEnabled (false);   // 펌핑 중 재진입(재스캔 재클릭 등) 차단

    double progress = 0.0;
    juce::AlertWindow win (u8 ("플러그인 스캔"), u8 ("VST3 검색 중…"),
                           juce::MessageBoxIconType::NoIcon);
    win.addProgressBarComponent (progress);
    win.setVisible (true);
    win.enterModalState (false);

    PluginCatalog::get().scanSync ([&] (float p, const juce::String& file) -> bool
    {
        progress = (double) p;
        win.setMessage (juce::File (file).getFileName());
       #if JUCE_MODAL_LOOPS_PERMITTED
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
       #endif
        return true;
    });

    win.exitModalState (0);
    win.setVisible (false);

    setEnabled (true);
    rebuildTree();
}

void PluginBrowser::addFromFile()
{
    juce::File initialDir ("C:/Program Files/Common Files/VST3");
    if (! initialDir.isDirectory())
        initialDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

    chooser = std::make_unique<juce::FileChooser> (u8 ("VST3 플러그인 선택"),
                                                   initialDir, "*.vst3");
    const auto chooserFlags = juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles;

    chooser->launchAsync (chooserFlags, [this] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File())
            return;

        const auto types = PluginCatalog::get().scanSingleFile (file.getFullPathName());
        rebuildTree();   // 카탈로그 병합 반영

        if (types.empty())
        {
            juce::NativeMessageBox::showMessageBoxAsync (
                juce::MessageBoxIconType::WarningIcon, u8 ("플러그인 로드 실패"),
                u8 ("VST3 플러그인을 찾지 못했습니다: ") + file.getFileName());
            return;
        }

        if (types.size() == 1)
        {
            pickAndClose (types.front());
            return;
        }

        // FR-05: 다중 클래스(WaveShell 등) — 서브플러그인 선택 팝업.
        juce::PopupMenu m;
        for (int i = 0; i < (int) types.size(); ++i)
            m.addItem (i + 1, types[(size_t) i].name);

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&fileButton),
                         [this, types] (int result)
                         {
                             if (result > 0)
                                 pickAndClose (types[(size_t) (result - 1)]);
                         });
    });
}

//==============================================================================
void PluginBrowser::setSelectedDesc (const juce::PluginDescription& desc)
{
    selected = desc;
    hasSelection = true;
}

void PluginBrowser::pickAndClose (const juce::PluginDescription& desc)
{
    if (onPick != nullptr)
        onPick (desc);
    closeDialog();
}

void PluginBrowser::closeDialog()
{
    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
        dw->exitModalState (0);
}

#pragma once

// Special Functions > Change Options page. App owns it via unique_ptr and resets on request:
// neither App nor page may call the self-deleting base Close().

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/overlay/achievement_icon_cache.h>
#include <rex/ui/windowed_app_context.h>

#include "achievements_list.h"
#include "assets_list.h"
#include "keybind_capture.h"
#include "maps_tab.h"
#include "mods_list.h"
#include "native_settings.h"
#include "options_glyphs.h"
#include "options_skin.h"
#include "page_set.h"
#include "retail_ui_art.h"
#include "text_entry.h"
#include "title_text.h"

namespace rerevved
{

struct OptionsPageEnv
{
    native_settings::HostBindings              host;
    std::function<std::filesystem::path()>     gameRoot;
    std::function<std::filesystem::path()>     keyboardArtDir;
    std::function<rex::ui::ImmediateDrawer*()> immediateDrawer;
    // Called once on the UI thread, never inside OnDraw: the owner resets its pointer afterwards.
    std::function<void()> requestClose;
    // Defer F1 Settings open/close one UI-thread turn for pending Mods edits.
    std::function<void()> closeSettings;
    std::function<void()> openSettings;
    // Capture before game, ImGui and hotkeys; null when no source feeds it.
    keybind_capture::Gate* captureKeys = nullptr;
    // Linux UI-thread clipboard context (ImGui has none); null elsewhere.
    rex::ui::WindowedAppContext* appContext = nullptr;
};

class OptionsPage final : public rex::ui::ImGuiDialog
{
public:
    OptionsPage(rex::ui::ImGuiDrawer* drawer, OptionsPageEnv env);
    ~OptionsPage() override;

    void RequestClose();

    bool Closing() const
    {
        return closing_;
    }

protected:
    void OnDraw(ImGuiIO& io) override;

private:
    struct Glyph
    {
        uint32_t                                   width  = 0;
        uint32_t                                   height = 0;
        std::unique_ptr<rex::ui::ImmediateTexture> texture;
    };

    struct KeyArt
    {
        bool                                       tried  = false;
        uint32_t                                   width  = 0;
        uint32_t                                   height = 0;
        std::vector<uint8_t>                       rgba;
        std::unique_ptr<rex::ui::ImmediateTexture> texture;
    };

    const KeyArt*                 bindArt(const native_settings::OptionRow& row);
    std::map<std::string, KeyArt> keyArt_;
    // Clipped Keybind text: written while drawing rows, read by the footer.
    std::string clippedBind_;

    struct Metrics;

    enum class PageKind
    {
        kOptions,
        kMods,
        kAchievements,
    };

    void     addPage(PageKind kind, std::string title);
    PageKind currentKind() const;
    void     handleInput();
    void     handleTextInput(bool up, bool down, bool left, bool right, bool padX, bool padY, bool padStart, bool back);
    void     pressTextKey(const text_entry::Key& key);
    void     pasteText();
    // Paste/Copy affect only the highlighted Invite Code row.
    void pasteInvite();
    void copyInvite();
    void handleCapture(bool back);
    void clearBind();
    void resetBind();
    bool takeRowAction();
    bool keyboardFamily() const;

public:
    // While capturing, the app must keep the bind key from its hotkeys.
    bool CapturingBind() const
    {
        return model_.Capturing();
    }

private:
    void activate();
    // Mods consumes B for a grab, question or any tab's pending edits.
    void                    onBack();
    size_t                  tabCount() const;
    size_t                  currentTab() const;
    bool                    stepTab(int direction);
    void                    selectTab(size_t tab);
    void                    releaseWheel();
    mods_list::Model&       activeModel();
    const mods_list::Model& activeModel() const;
    bool                    anyModsDirty() const;
    bool                    askDiscard();
    void                    finishDiscard();
    // Apply input after the strip; a same-frame mouse page switch drops it.
    void  runDeferredInput();
    void  beginSettingsHandoff();
    void  refreshModsLock();
    void  drawPage(ImGuiIO& io);
    void  drawPicker(ImGuiIO& io);
    void  drawTextEntry(ImGuiIO& io);
    float drawPrompt(int glyph, const std::string& caption, ImVec2 min, float size);
    void  drawLegend(const Metrics& m);
    void  drawBackdrop(const Metrics& m);
    void  drawBand(const Metrics& m);
    void  drawFrame(const Metrics& m);
    void  drawPanelSummary(const Metrics& m);
    void  footerLines(float wrap, int& tipLines, int& noteLines);
    void  drawScrollArrows(const Metrics& m, float scrollY, float scrollMax, float rowHeight);
    bool  beginBodyChild(const char* id, const Metrics& m);
    bool  drawSkinImage(options_skin::Art art, ImVec2 min, ImVec2 max, ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1));
    // Arrow: 0 right, 1 down, 2 left, 3 up; false without art.
    bool                       drawSkinArrow(ImVec2 center, float size, int turns, float alpha = 1.0f);
    rex::ui::ImmediateTexture* skinTexture(options_skin::Art art);
    ImVec2                     frameMin(const Metrics& m) const;
    ImVec2                     frameMax(const Metrics& m) const;
    void                       drawOptionsBody(ImGuiIO& io, const Metrics& m);
    void                       refreshModsIfDue();
    void                       drawModsBody(ImGuiIO& io, const Metrics& m);
    void                       refreshMods();
    void                       refreshAssets();
    void                       refreshMaps();
    void                       rescanMaps();
    void                       refreshActiveTab();
    std::string                modsTabText(title_text::Id packages, title_text::Id assets, title_text::Id maps) const;
    std::filesystem::path      mapsProfileRoot() const;
    void                       activateMod();
    void                       grabMod();
    void                       shiftMod(int direction);
    void                       acceptMods();
    void                       applyMods();
    void                       runApply(bool replace);
    bool                       modsBusy() const;
    std::string                modsTip() const;
    std::string                modsNote() const;
    rex::ui::ImmediateTexture* modIcon(const std::filesystem::path& path);
    void                       refreshAchievementsIfDue();
    void                       drawAchievementsBody(ImGuiIO& io, const Metrics& m);
    void                       refreshAchievements();
    void                       moveAchievement(int direction);
    std::string                achievementStatus(const achievements_list::Entry& entry) const;
    // Prompt clicks/wheel set step to -1 or +1, otherwise 0. Returns the clicked title or -1.
    int drawStrip(const std::vector<std::string>& titles, size_t current, float top, int leftHint, int rightHint, const Metrics& m, int& step, int idScope);

    struct HintSize
    {
        float width  = 0.0f;
        float height = 0.0f;
    };

    HintSize                   hintSize(int which, float height);
    void                       drawHint(int which, ImVec2 min, HintSize size);
    void                       drawButtonFace(const char* caption, int glyph, ImVec2 min, ImVec2 size, rex::ui::ChromeState state, bool dim);
    bool                       legendButton(const char* id, int glyph, const char* caption, ImVec2 min, const Metrics& m, bool enabled = true, float width = 0.0f, bool veil = false);
    float                      drawGlyph(int which, ImVec2 min, float size);
    bool                       drawArt(int which, ImVec2 min, float height);
    float                      glyphWidth(int which, float height, float fallbackWidth);
    rex::ui::ImmediateTexture* glyphTexture(int which);
    void                       loadGlyphs();

    OptionsPageEnv                             env_;
    std::unique_ptr<native_settings::Services> services_;
    native_settings::OptionsModel              model_;
    bool                                       closing_ = false;
    // Require every page key released once to suppress keys held through open.
    bool                             armed_         = false;
    bool                             ensureVisible_ = true;
    bool                             pickerEnsure_  = false;
    bool                             settingsIdle_  = false;
    int                              openFrame_     = -1;
    PageSet                          pages_;
    std::vector<PageKind>            pageKinds_;
    mods_list::Model                 mods_;
    mods_list::Model                 assets_;
    mods_list::Model                 maps_;
    rex::system::AssetOverlayCatalog mapsCatalog_;
    int                              mapsFrame_               = -1;
    uint64_t                         mapsValidatedRevision_   = ~uint64_t{ 0 };
    size_t                           modsTab_                 = 0;
    int                              modsFrame_               = -1;
    uint64_t                         modsValidatedRevision_   = ~uint64_t{ 0 };
    int                              assetsFrame_             = -1;
    uint64_t                         assetsValidatedRevision_ = ~uint64_t{ 0 };
    bool                             modsEnsure_              = true;
    std::function<void()>            deferredInput_;
    size_t                           inputPage_ = 0;
    size_t                           inputTab_  = 0;
    // Keep Settings closed during the question, ignoring its departing frame and the question's opening frame.
    bool handoffAsking_     = false;
    int  handoffQuietUntil_ = 0;
    bool consumeInput_      = false;
    // The frame a Mods question first showed; its buttons ignore clicks then.
    int                                                                         questionFrame_ = -1;
    bool                                                                        questionShown_ = false;
    std::unordered_map<std::string, std::unique_ptr<rex::ui::ImmediateTexture>> modIcons_;
    achievements_list::Groups                                                   achievements_;
    int                                                                         achievementsFrame_  = -1;
    size_t                                                                      achievementTab_     = 0;
    size_t                                                                      achievementRows_[2] = { 0, 0 };
    bool                                                                        achievementEnsure_  = true;
    PageKind                                                                    shownKind_          = PageKind::kOptions;
    std::unique_ptr<rex::ui::AchievementIconCache>                              icons_;
    size_t                                                                      shownTab_  = 0;
    size_t                                                                      wheelPage_ = static_cast<size_t>(-1);
    size_t                                                                      wheelTab_  = static_cast<size_t>(-1);
    TriggerLatch                                                                triggerLatch_[2];
    // The SDK maps no ImGui key for X or Start, and Y only for text input.
    TriggerLatch faceLatch_[3];
    // Apply right-stick row steps only once the list's scroll extent is known.
    StickRepeat                                                                                          stick_;
    int                                                                                                  stickScroll_    = 0;
    int                                                                                                  pickerFrame_    = -1;
    bool                                                                                                 glyphsLoaded_   = false;
    int                                                                                                  family_         = 0;
    int                                                                                                  rowActionFrame_ = -1;
    Glyph                                                                                                glyphs_[options_glyphs::kButtonCount];
    Glyph                                                                                                skin_[options_skin::kArtCount];
    size_t                                                                                               pendingPage_ = static_cast<size_t>(-1);
    float                                                                                                scrollTo_    = -1.0f;
    size_t                                                                                               scrollPage_  = 0;
    size_t                                                                                               scrollTab_   = 0;
    std::shared_ptr<const std::array<std::optional<retail_ui_art::Image>, options_glyphs::kButtonCount>> images_;
    std::shared_ptr<const std::array<std::optional<retail_ui_art::Image>, options_skin::kArtCount>>      skinImages_;
};

} // namespace rerevved

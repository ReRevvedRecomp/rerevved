// Settings, Mods and Achievements pages; reads ImGui keys directly, independent of ImGui focus.

#include "options_page.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_stdinc.h>

#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/style.h>

#include "button_icons.h"
#include "game_language.h"
#include "gameplay_session_settings.h"
#include "keybind_capture.h"
#include "map_packs.h"
#include "options_glyphs.h"
#include "options_skin.h"
#include "overlay_palette.h"
#include "tab_bar_layout.h"
#include "title_text.h"

namespace rerevved
{

namespace
{

using namespace palette;
using native_settings::OptionRow;
using native_settings::Timing;

constexpr ImVec4 kArrow = rgba(255, 214, 64);
constexpr ImVec4 kValue = rgba(200, 220, 255);

constexpr float kBodyScale  = 1.3f;
constexpr float kTitleScale = 1.9f;

constexpr float kRowEm              = 2.4f;
constexpr float kHeadingEm          = 2.2f;
constexpr float kPadEm              = 0.6f;
constexpr float kValueEm            = 26.0f;
constexpr float kArrowEm            = 2.2f;
constexpr float kColumnEm           = 60.0f;
constexpr float kPickerWidth        = 30.0f;
constexpr float kTextWidth          = 44.0f;
constexpr float kTabPadEm           = 1.0f;
constexpr float kHintGapEm          = 0.5f;
constexpr float kHintCapShare       = 0.6f;
constexpr float kMinTabScale        = 0.7f;
constexpr float kAchRowEm           = 4.4f;
constexpr float kAchIconEm          = 3.2f;
constexpr float kAchRightEm         = 13.0f;
constexpr int   kAchRefreshFrames   = 30;
constexpr float kModRowEm           = 4.0f;
constexpr float kModBoxEm           = 1.6f;
constexpr float kModIconEm          = 3.0f;
constexpr float kModStateEm         = 8.0f;
constexpr float kModArrowEm         = 2.2f;
constexpr int   kModRefreshFrames   = 30;
constexpr int   kHandoffQuietFrames = 5;

// Sampled from lossy retail Civilopedia video frames; replaced by loaded art.
constexpr ImVec4 kMapTint       = rgba(8, 22, 84, 0.38f);
constexpr ImVec4 kBarFallback   = rgba(6, 15, 46, 0.92f);
constexpr ImVec4 kBodyTop       = rgba(29, 49, 118, 0.94f);
constexpr ImVec4 kBodyBottom    = rgba(39, 63, 127, 0.94f);
constexpr ImVec4 kBorderOuter   = rgba(0, 7, 30);
constexpr ImVec4 kBorderInner   = rgba(78, 110, 190, 0.85f);
constexpr float  kMinListRows   = 3.0f;
constexpr float  kFrameBorder   = 3.0f;
constexpr float  kGutterEm      = 2.2f;
constexpr float  kScrollLsEm    = 1.3f;
constexpr float  kScrollArrowEm = 0.8f;
constexpr float  kValueArrowEm  = 1.2f;
constexpr float  kOrderArrowEm  = 1.1f;

using options_glyphs::Button;
constexpr int kHintLB = static_cast<int>(Button::kLB);
constexpr int kHintRB = static_cast<int>(Button::kRB);
constexpr int kHintLT = static_cast<int>(Button::kLT);
constexpr int kHintRT = static_cast<int>(Button::kRT);
constexpr int kGlyphA = static_cast<int>(Button::kA);
constexpr int kGlyphB = static_cast<int>(Button::kB);
constexpr int kGlyphX = static_cast<int>(Button::kX);
constexpr int kGlyphY = static_cast<int>(Button::kY);

constexpr const char* kFpkName = "Pregame.FPK";

constexpr uint64_t kMaxGlyphFileBytes = 1ull * 1024 * 1024;

std::optional<std::vector<uint8_t>> readFile(const std::filesystem::path& path, std::string* error)
{
    std::error_code ec;
    const auto      size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > kMaxGlyphFileBytes)
    {
        *error = "missing or the wrong size";
        return std::nullopt;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    std::ifstream        file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    {
        *error = "unreadable";
        return std::nullopt;
    }
    return bytes;
}

ImU32 color(const ImVec4& c, float alpha = 1.0f)
{
    return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
}

constexpr const char* kChordPaste = "Ctrl+V";
constexpr const char* kChordCopy  = "Ctrl+C";
constexpr const char* kChordClear = "Del";
constexpr const char* kChordReset = "Bksp";
constexpr int         kGlyphNone  = -1;

std::string rowButtonCaption(bool keyboard, const char* chord, const std::string& caption)
{
    return keyboard ? std::string(chord) + " " + caption : caption;
}

// Linux ImGui clipboard handlers stay internal, so go through SDL on its main thread.
constexpr bool kSdlClipboard =
#if defined(_WIN32) || defined(__APPLE__)
    false;
#else
    true;
#endif

rex::ui::WindowedAppContext* clipboardContext(ImGuiContext* ctx)
{
    return static_cast<rex::ui::WindowedAppContext*>(ctx->PlatformIO.Platform_ClipboardUserData);
}

const char* sdlGetClipboardText(ImGuiContext* ctx)
{
    for (char& c : ctx->ClipboardHandlerData)
        static_cast<volatile char&>(c) = 0;
    ctx->ClipboardHandlerData.clear();
    auto* app = clipboardContext(ctx);
    if (!app)
        return "";
    // Zero SDL's buffer before freeing and ImGui's on paste so no invite copy remains.
    app->CallInUIThreadSynchronous([&]
                                   {
                                       if (char* got = SDL_GetClipboardText())
                                       {
                                           const size_t size = std::strlen(got);
                                           ctx->ClipboardHandlerData.resize(static_cast<int>(size) + 1);
                                           std::copy(got, got + size + 1, ctx->ClipboardHandlerData.Data);
                                           for (size_t i = 0; i < size; ++i)
                                               static_cast<volatile char*>(got)[i] = 0;
                                           SDL_free(got);
                                       }
                                   });
    if (ctx->ClipboardHandlerData.empty())
        return "";
    return ctx->ClipboardHandlerData.Data;
}

void sdlSetClipboardText(ImGuiContext* ctx, const char* text)
{
    if (auto* app = clipboardContext(ctx))
    {
        app->CallInUIThreadSynchronous([&]
                                       {
                                           SDL_SetClipboardText(text);
                                       });
    }
}

void installSystemClipboard(rex::ui::WindowedAppContext* app)
{
    if (!kSdlClipboard || !app)
        return;
    ImGuiPlatformIO& io = ImGui::GetPlatformIO();
    if (io.Platform_ClipboardUserData == app)
        return;
    io.Platform_ClipboardUserData  = app;
    io.Platform_GetClipboardTextFn = sdlGetClipboardText;
    io.Platform_SetClipboardTextFn = sdlSetClipboardText;
}

void forgetClipboardCache()
{
    if (ImGuiContext* ctx = ImGui::GetCurrentContext())
    {
        for (int i = 0; i < ctx->ClipboardHandlerData.Size; ++i)
            static_cast<volatile char&>(ctx->ClipboardHandlerData.Data[i]) = 0;
        ctx->ClipboardHandlerData.clear();
    }
}

bool pressed(std::initializer_list<ImGuiKey> keys, bool repeat)
{
    for (const ImGuiKey key : keys)
    {
        if (ImGui::IsKeyPressed(key, repeat))
            return true;
    }
    return false;
}

bool anyPageKeyDown()
{
    static constexpr ImGuiKey keys[] = {
        ImGuiKey_UpArrow,
        ImGuiKey_DownArrow,
        ImGuiKey_LeftArrow,
        ImGuiKey_RightArrow,
        ImGuiKey_PageUp,
        ImGuiKey_PageDown,
        ImGuiKey_Q,
        ImGuiKey_E,
        ImGuiKey_Enter,
        ImGuiKey_KeypadEnter,
        ImGuiKey_Escape,
        ImGuiKey_X,
        ImGuiKey_Y,
        ImGuiKey_GamepadDpadUp,
        ImGuiKey_GamepadDpadDown,
        ImGuiKey_GamepadDpadLeft,
        ImGuiKey_GamepadDpadRight,
        ImGuiKey_GamepadFaceDown,
        ImGuiKey_GamepadFaceRight,
        ImGuiKey_GamepadL1,
        ImGuiKey_GamepadR1,
    };
    for (const ImGuiKey key : keys)
    {
        if (ImGui::IsKeyDown(key))
            return true;
    }
    return false;
}

// The SDK maps none of these to ImGui (except Y in text input); no pad reads as released.
struct PadSample
{
    int triggers[2] = { 0, 0 };
    int faces[3]    = { 0, 0, 0 };
    int stickY      = 0;
};

PadSample padSample()
{
    auto*                       kernel  = REX_KERNEL_STATE();
    auto*                       runtime = kernel ? kernel->emulator() : nullptr;
    auto*                       input   = runtime ? dynamic_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
    rex::input::X_INPUT_GAMEPAD pad{};
    PadSample                   sample;
    if (!input || !input->GetNavigationPadState(&pad))
        return sample;
    const uint16_t buttons = pad.buttons;
    sample.triggers[0]     = pad.left_trigger;
    sample.triggers[1]     = pad.right_trigger;
    sample.faces[0]        = (buttons & rex::input::X_INPUT_GAMEPAD_X) ? 255 : 0;
    sample.faces[1]        = (buttons & rex::input::X_INPUT_GAMEPAD_Y) ? 255 : 0;
    sample.faces[2]        = (buttons & rex::input::X_INPUT_GAMEPAD_START) ? 255 : 0;
    const int16_t stickY   = pad.thumb_ry;
    sample.stickY          = stickY;
    return sample;
}

float labelScale(float textHeight, float capHeight)
{
    return textHeight > capHeight && textHeight > 0.0f ? capHeight * 0.85f / textHeight : 1.0f;
}

void drawPill(ImDrawList* dl, ImVec2 min, float height, const char* label)
{
    const ImVec2 full  = ImGui::CalcTextSize(label);
    const float  scale = labelScale(full.y, height);
    const ImVec2 text(full.x * scale, full.y * scale);
    const float  width = std::max(height, text.x + height * 0.6f);
    const ImVec2 max(min.x + width, min.y + height);
    dl->AddRectFilled(min, max, color(rgba(230, 232, 238)), height * 0.3f);
    dl->AddRect(min, max, color(rgba(90, 96, 110)), height * 0.3f, 0, 2.0f);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * scale, ImVec2(min.x + (width - text.x) * 0.5f, min.y + (height - text.y) * 0.5f), color(rgba(30, 32, 40)), label);
}

float pillWidth(float height, const char* label)
{
    const ImVec2 full = ImGui::CalcTextSize(label);
    return std::max(height, full.x * labelScale(full.y, height) + height * 0.6f);
}

void drawBar(const rex::ui::Style& style, ImDrawList* dl, ImVec2 min, ImVec2 max, rex::ui::ChromeState state)
{
    if (style.chrome.draw_selection)
    {
        style.chrome.draw_selection(dl, min, max, state);
        return;
    }
    dl->AddRectFilled(min, max, color(kHighlight, 0.9f));
    dl->AddRect(min, max, color(kGlow));
}

void drawArrow(ImDrawList* dl, ImVec2 center, float half, bool left, float alpha)
{
    const float dir = left ? -1.0f : 1.0f;
    dl->AddTriangleFilled(ImVec2(center.x + dir * half * 0.7f, center.y),
                          ImVec2(center.x - dir * half * 0.5f, center.y - half),
                          ImVec2(center.x - dir * half * 0.5f, center.y + half),
                          color(kArrow, alpha));
}

bool startsGroup(const rerevved::native_settings::OptionSection& section, size_t r)
{
    const auto& group = section.rows[r].group;
    return !group.empty() && (r == 0 || section.rows[r - 1].group != group);
}

bool localTime(std::time_t when, std::tm* out)
{
#ifdef _WIN32
    return localtime_s(out, &when) == 0;
#else
    return localtime_r(&when, out) != nullptr;
#endif
}

title_text::Language pageLanguage()
{
    return title_text::LanguageForXboxId(game_language::ActiveId());
}

std::string pageText(title_text::Id id)
{
    return std::string(title_text::Localized(id, pageLanguage()));
}

std::string pageText(title_text::Id id, const std::vector<std::string>& args)
{
    return achievements_list::Substitute(pageText(id), args);
}

void drawVerticalArrow(ImDrawList* dl, ImVec2 center, float half, bool up, float alpha)
{
    const float dir = up ? -1.0f : 1.0f;
    dl->AddTriangleFilled(ImVec2(center.x, center.y + dir * half * 0.7f),
                          ImVec2(center.x - half, center.y - dir * half * 0.5f),
                          ImVec2(center.x + half, center.y - dir * half * 0.5f),
                          color(kArrow, alpha));
}

template <typename Diagnostic>
std::string describe(const Diagnostic& diagnostic)
{
    if (diagnostic.line != 0)
        return pageText(title_text::Id::kDiagnosticLine, { std::to_string(diagnostic.line), diagnostic.message });
    return diagnostic.message;
}

std::string severityText(bool error, const std::string& message)
{
    return pageText(error ? title_text::Id::kDiagnosticError : title_text::Id::kDiagnosticWarning, { message });
}

void drawRightText(ImDrawList* dl, float leftLimit, float right, float y, const std::string& text, ImU32 col)
{
    const float x = std::max(leftLimit, right - ImGui::CalcTextSize(text.c_str()).x);
    dl->AddText(ImVec2(std::floor(x), y), col, text.c_str());
}

void drawCenteredText(ImDrawList* dl, ImVec2 min, ImVec2 max, const char* text, ImU32 col)
{
    const ImVec2 size = ImGui::CalcTextSize(text);
    dl->PushClipRect(min, max, true);
    dl->AddText(ImVec2(std::floor(min.x + (max.x - min.x - size.x) * 0.5f), std::floor(min.y + (max.y - min.y - size.y) * 0.5f)), col, text);
    dl->PopClipRect();
}

} // namespace

struct OptionsPage::Metrics
{
    float  em = 0.0f;
    ImVec2 size;
    float  banner     = 0.0f;
    float  strip      = 0.0f;
    float  strips     = 0.0f;
    float  top        = 0.0f;
    float  footer     = 0.0f;
    float  titleStrip = 0.0f;
    float  margin     = 0.0f;
    float  column     = 0.0f;
    float  left       = 0.0f;
};

OptionsPage::OptionsPage(rex::ui::ImGuiDrawer* drawer, OptionsPageEnv env)
: rex::ui::ImGuiDialog(drawer, true)
, env_(std::move(env))
, services_(native_settings::MakeUiServices(env_.host))
, model_(*services_, pageLanguage())
{
    addPage(PageKind::kOptions, pageText(title_text::Id::kPageOptions));
    addPage(PageKind::kMods, pageText(title_text::Id::kPageMods));
    addPage(PageKind::kAchievements, pageText(title_text::Id::kPageAchievements));
}

void OptionsPage::addPage(PageKind kind, std::string title)
{
    pages_.Add(std::move(title));
    pageKinds_.push_back(kind);
}

OptionsPage::PageKind OptionsPage::currentKind() const
{
    return pageKinds_[pages_.Current()];
}

OptionsPage::~OptionsPage() = default;

void OptionsPage::RequestClose()
{
    if (closing_)
        return;
    closing_ = true;
    if (env_.requestClose)
        env_.requestClose();
}

void OptionsPage::OnDraw(ImGuiIO& io)
{
    if (closing_)
        return;
    installSystemClipboard(env_.appContext);
    // F1's snapshot would revert page edits; ignore activity until F1 is inactive.
    {
        constexpr int kCloseGraceFrames = 3;
        if (openFrame_ < 0)
            openFrame_ = ImGui::GetFrameCount();
        const ImGuiWindow* settings = ImGui::FindWindowByName("Settings##rex");
        if (!settings || !settings->WasActive)
            settingsIdle_ = true;
        else if (handoffAsking_)
        {
            if (env_.closeSettings)
                env_.closeSettings();
        }
        else if (ImGui::GetFrameCount() < handoffQuietUntil_)
        {
        }
        else if (settingsIdle_ || ImGui::GetFrameCount() - openFrame_ > kCloseGraceFrames)
        {
            if (anyModsDirty())
            {
                beginSettingsHandoff();
                if (closing_)
                    return;
            }
            else
            {
                RequestClose();
                return;
            }
        }
    }
    if (pendingPage_ != static_cast<size_t>(-1))
    {
        if (!modsBusy() && !model_.Capturing())
        {
            pages_.Select(pendingPage_);
            releaseWheel();
        }
        pendingPage_ = static_cast<size_t>(-1);
    }
    ImGui::SetNextFrameWantCaptureKeyboard(true);
    ImGui::SetNextFrameWantCaptureMouse(true);

    model_.Poll();
    deferredInput_ = nullptr;
    // The lock can appear between refreshes; check it every frame.
    if (currentKind() == PageKind::kMods)
        refreshModsLock();
    handleInput();
    drawPage(io);
    stickScroll_ = 0;
    if (model_.PickerOpen())
        drawPicker(io);
    else if (model_.TextOpen())
        drawTextEntry(io);
}

void OptionsPage::handleInput()
{
    const auto sample    = padSample();
    const bool pageBack  = triggerLatch_[0].Update(sample.triggers[0]);
    const bool pageNext  = triggerLatch_[1].Update(sample.triggers[1]);
    const bool padX      = faceLatch_[0].Update(sample.faces[0]);
    const bool padY      = faceLatch_[1].Update(sample.faces[1]);
    const bool padStart  = faceLatch_[2].Update(sample.faces[2]);
    const int  stickStep = stick_.Update(sample.stickY, ImGui::GetIO().DeltaTime);
    if (consumeInput_)
    {
        consumeInput_ = false;
        return;
    }
    if (!armed_)
    {
        armed_ = !anyPageKeyDown() && !triggerLatch_[0].Down() && !triggerLatch_[1].Down() && !faceLatch_[0].Down() && !faceLatch_[1].Down() &&
                 !faceLatch_[2].Down() && !stick_.Held();
        return;
    }
    const bool up           = pressed({ ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp }, true);
    const bool down         = pressed({ ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown }, true);
    const bool left         = pressed({ ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft }, true);
    const bool right        = pressed({ ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight }, true);
    const bool previous     = pressed({ ImGuiKey_PageUp, ImGuiKey_GamepadL1 }, false);
    const bool next         = pressed({ ImGuiKey_PageDown, ImGuiKey_GamepadR1 }, false);
    const bool pagePrevious = pageBack || ImGui::IsKeyPressed(ImGuiKey_Q, false);
    const bool pageForward  = pageNext || ImGui::IsKeyPressed(ImGuiKey_E, false);
    const bool select       = pressed({ ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_GamepadFaceDown }, false);
    const bool back         = pressed({ ImGuiKey_Escape, ImGuiKey_GamepadFaceRight }, false);
    const bool ctrl         = ImGui::GetIO().KeyCtrl;
    const bool wantPaste    = padX || (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false));
    const bool wantCopy     = padY || (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false));
    const bool wantClear    = padX || ImGui::IsKeyPressed(ImGuiKey_Delete, false);
    const bool wantReset    = padY || ImGui::IsKeyPressed(ImGuiKey_Backspace, false);

    const bool onOptions = currentKind() == PageKind::kOptions;
    if (onOptions && model_.PickerOpen())
    {
        if (up != down)
        {
            model_.MovePicker(up ? -1 : 1);
            pickerEnsure_ = true;
        }
        if (select)
            model_.CommitPicker();
        else if (back)
            model_.ClosePicker();
        return;
    }
    if (onOptions && model_.TextOpen())
    {
        handleTextInput(up, down, left, right, padX, padY, padStart, back);
        return;
    }
    if (onOptions && model_.Capturing())
    {
        handleCapture(back);
        return;
    }
    const bool onMods = currentKind() == PageKind::kMods;
    if (onMods && activeModel().Confirmation() != mods_list::Confirm::kNone && !questionShown_)
        return;
    if (back)
    {
        onBack();
        return;
    }
    if (onMods && activeModel().Confirmation() != mods_list::Confirm::kNone)
    {
        if (select)
            acceptMods();
        return;
    }
    if (!modsBusy() && pagePrevious != pageForward && pages_.Step(pagePrevious ? -1 : 1))
    {
        releaseWheel();
        return;
    }
    inputPage_     = pages_.Current();
    inputTab_      = currentTab();
    deferredInput_ = [=, this]
    {
        bool tabSwitched = false;
        if (stickStep != 0 && !modsBusy())
            stickScroll_ = stickStep;
        if (!onOptions && previous != next && stepTab(previous ? -1 : 1))
            return;
        const size_t before = model_.Highlight();
        if (onOptions)
        {
            if (up != down)
            {
                model_.Move(up ? -1 : 1);
                ensureVisible_ = true;
            }
            if (left != right)
                model_.Step(left ? -1 : 1);
            if (previous != next)
            {
                tabSwitched = stepTab(previous ? -1 : 1);
                if (tabSwitched)
                    releaseWheel();
                ensureVisible_ = true;
            }
        }
        else if (onMods)
        {
            auto& model = activeModel();
            if (up != down)
            {
                if (model.Grabbing())
                    shiftMod(up ? -1 : 1);
                else
                    model.Move(up ? -1 : 1);
                modsEnsure_ = true;
            }
            if (select)
                activateMod();
            if (padX || ImGui::IsKeyPressed(ImGuiKey_X, false))
                grabMod();
            if (padY || ImGui::IsKeyPressed(ImGuiKey_Y, false))
                applyMods();
        }
        else if (up != down)
        {
            moveAchievement(up ? -1 : 1);
        }
        // One row action per frame: A before X before Y.
        if (select && onOptions && !tabSwitched)
            activate();
        else if (onOptions && !tabSwitched && model_.Highlight() == before)
        {
            const OptionRow* current = model_.Row(model_.Highlight());
            if (current && current->HasBindButtons())
            {
                if (wantClear)
                    clearBind();
                else if (wantReset)
                    resetBind();
            }
            else if (wantPaste)
            {
                pasteInvite();
            }
            else if (wantCopy)
            {
                copyInvite();
            }
        }
    };
}

void OptionsPage::runDeferredInput()
{
    auto run       = std::move(deferredInput_);
    deferredInput_ = nullptr;
    if (run && pages_.Current() == inputPage_ && currentTab() == inputTab_)
        run();
}

void OptionsPage::refreshModsLock()
{
    auto* kernel  = REX_KERNEL_STATE();
    auto* runtime = kernel ? kernel->emulator() : nullptr;
    if (runtime)
        mods_.SetLock(runtime->mod_loadout_lock_reason());
}

size_t OptionsPage::tabCount() const
{
    switch (currentKind())
    {
        case PageKind::kOptions:
            return model_.Sections().size();
        case PageKind::kMods:
            return maps_tab::kModsTabCount;
        case PageKind::kAchievements:
            return achievements_.HasTabs() ? 2 : 0;
    }
    return 0;
}

size_t OptionsPage::currentTab() const
{
    switch (currentKind())
    {
        case PageKind::kOptions:
            return model_.Tab();
        case PageKind::kMods:
            return modsTab_;
        case PageKind::kAchievements:
            return achievementTab_;
    }
    return 0;
}

bool OptionsPage::stepTab(int direction)
{
    bool changed = false;
    switch (currentKind())
    {
        case PageKind::kOptions:
            changed = model_.StepTab(direction);
            break;
        case PageKind::kMods:
            if (modsBusy())
                break;
            modsTab_ = maps_tab::StepModsTab(modsTab_, direction);
            changed  = true;
            break;
        case PageKind::kAchievements:
            if (!achievements_.HasTabs())
                break;
            achievementTab_ = 1 - achievementTab_;
            changed         = true;
            break;
    }
    if (changed)
        releaseWheel();
    return changed;
}

void OptionsPage::selectTab(size_t tab)
{
    const size_t before = currentTab();
    switch (currentKind())
    {
        case PageKind::kOptions:
            model_.SelectTab(tab);
            break;
        case PageKind::kMods:
            if (!modsBusy() && tab < maps_tab::kModsTabCount && tab != modsTab_)
                modsTab_ = tab;
            break;
        case PageKind::kAchievements:
            if (achievements_.HasTabs() && tab < 2 && tab != achievementTab_)
                achievementTab_ = tab;
            break;
    }
    if (currentTab() != before)
        releaseWheel();
}

// ImGui pins wheel events to their starting window; release ours, never F2's lock.
void OptionsPage::releaseWheel()
{
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx || !ctx->WheelingWindow)
        return;
    const ImGuiWindow* root = ImGui::FindWindowByName("##rerevved_options");
    if (!root || ctx->WheelingWindow->RootWindow != root)
        return;
    ctx->WheelingWindow              = nullptr;
    ctx->WheelingWindowStartFrame    = -1;
    ctx->WheelingWindowScrolledFrame = -1;
    ctx->WheelingWindowReleaseTimer  = 0.0f;
}

mods_list::Model& OptionsPage::activeModel()
{
    if (modsTab_ == maps_tab::kAssetsTab)
        return assets_;
    if (modsTab_ == maps_tab::kMapsTab)
        return maps_;
    return mods_;
}

const mods_list::Model& OptionsPage::activeModel() const
{
    if (modsTab_ == maps_tab::kAssetsTab)
        return assets_;
    if (modsTab_ == maps_tab::kMapsTab)
        return maps_;
    return mods_;
}

std::string OptionsPage::modsTabText(title_text::Id packages, title_text::Id assets, title_text::Id maps) const
{
    if (modsTab_ == maps_tab::kAssetsTab)
        return pageText(assets);
    if (modsTab_ == maps_tab::kMapsTab)
        return pageText(maps);
    return pageText(packages);
}

bool OptionsPage::anyModsDirty() const
{
    return mods_.Dirty() || assets_.Dirty() || maps_.Dirty();
}

// Match F2 discard order: Packages, Assets, then Maps.
bool OptionsPage::askDiscard()
{
    size_t tab = maps_tab::kPackagesTab;
    if (!mods_.Dirty())
    {
        if (assets_.Dirty())
            tab = maps_tab::kAssetsTab;
        else if (maps_.Dirty())
            tab = maps_tab::kMapsTab;
        else
            return false;
    }
    for (size_t i = 0; i < pageKinds_.size(); ++i)
    {
        if (pageKinds_[i] == PageKind::kMods)
            pages_.Select(i);
    }
    releaseWheel();
    modsTab_    = tab;
    auto& model = activeModel();
    for (int i = 0; i < 3 && model.Confirmation() != mods_list::Confirm::kDiscard; ++i)
        model.Back();
    return model.Confirmation() == mods_list::Confirm::kDiscard;
}

void OptionsPage::finishDiscard()
{
    if (askDiscard())
    {
        questionShown_ = false;
        return;
    }
    if (handoffAsking_ && env_.openSettings)
        env_.openSettings();
    handoffAsking_ = false;
    RequestClose();
}

void OptionsPage::onBack()
{
    if (currentKind() == PageKind::kMods)
    {
        auto& model = activeModel();
        if (model.Grabbing() || model.Confirmation() != mods_list::Confirm::kNone)
        {
            model.Back();
            if (model.Confirmation() == mods_list::Confirm::kNone && handoffAsking_)
            {
                handoffAsking_     = false;
                handoffQuietUntil_ = ImGui::GetFrameCount() + kHandoffQuietFrames;
                if (env_.closeSettings)
                    env_.closeSettings();
            }
            return;
        }
    }
    if (askDiscard())
        return;
    RequestClose();
}

void OptionsPage::beginSettingsHandoff()
{
    if (model_.PickerOpen())
        model_.ClosePicker();
    model_.CancelText();
    model_.CancelCapture();
    for (auto* model : { &mods_, &assets_, &maps_ })
    {
        for (int i = 0; i < 2 && (model->Grabbing() || model->Confirmation() != mods_list::Confirm::kNone); ++i)
            model->Back();
    }
    if (!askDiscard())
    {
        RequestClose();
        return;
    }
    handoffAsking_ = true;
    // The press that may have opened Settings must not also answer the question.
    consumeInput_ = true;
    if (env_.closeSettings)
        env_.closeSettings();
}

void OptionsPage::handleCapture(bool back)
{
    keybind_capture::Gate* keys = env_.captureKeys;
    if (back)
    {
        // Capture owns B this frame; ignore clicks on the exposed Clear area.
        // ImGui derives Back from B when focus has no active item.
        for (const ImGuiKey padKey : { ImGuiKey_GamepadFaceRight, ImGuiKey_GamepadBack })
            ImGui::SetKeyOwner(padKey, ImGui::GetID("##keybind_capture"), ImGuiInputFlags_LockThisFrame);
        takeRowAction();
        model_.CancelCapture();
        if (keys)
            keys->Discard();
        return;
    }
    if (!keys || !keys->Pending())
        return;
    if (!takeRowAction())
        return;
    const OptionRow* row = model_.Row(model_.Highlight());
    // Controller binds allow chords ("Shift+Up"); overlay hotkeys take one
    // key or a modifier pressed and released alone.
    const bool chords = row && row->bindController;
    while (const auto key = keys->Take())
    {
        std::string binding;
        switch (keybind_capture::Classify(key->virtualKey))
        {
            case keybind_capture::Kind::kCancel:
                model_.CancelCapture();
                keys->Discard();
                return;
            case keybind_capture::Kind::kUnbindable:
                model_.RefuseCapture();
                continue;
            case keybind_capture::Kind::kModifier:
                if (key->modifierOnly && !chords)
                    binding = keybind_capture::ModifierName(key->virtualKey);
                break;
            case keybind_capture::Kind::kKey:
                binding = keybind_capture::Binding(key->virtualKey, key->shift, key->ctrl, key->alt, chords);
                break;
        }
        if (binding.empty())
            continue;
        model_.CommitBinding(binding);
        keys->Discard();
        return;
    }
}

void OptionsPage::clearBind()
{
    const auto* row = model_.Row(model_.Highlight());
    if (!row || !row->CanClearBind() || model_.ModalOpen() || !takeRowAction())
        return;
    model_.ClearBind();
}

void OptionsPage::resetBind()
{
    const auto* row = model_.Row(model_.Highlight());
    if (!row || !row->CanResetBind() || model_.ModalOpen() || !takeRowAction())
        return;
    model_.ResetBind();
}

bool OptionsPage::takeRowAction()
{
    const int frame = ImGui::GetFrameCount();
    if (rowActionFrame_ == frame)
        return false;
    rowActionFrame_ = frame;
    return true;
}

bool OptionsPage::keyboardFamily() const
{
    return static_cast<button_icons::Family>(family_) == button_icons::Family::kKeyboard;
}

void OptionsPage::activate()
{
    if (!takeRowAction())
        return;
    const auto press = model_.Activate();
    if (press == native_settings::OptionsModel::Press::kOpenPicker)
    {
        pickerFrame_  = ImGui::GetFrameCount();
        pickerEnsure_ = true;
    }
    else if (press == native_settings::OptionsModel::Press::kCapture && env_.captureKeys)
    {
        env_.captureKeys->Discard();
    }
}

// Text entry owns keys, pad and characters, ignoring its opening press.
void OptionsPage::handleTextInput(bool up, bool down, bool left, bool right, bool padX, bool padY, bool padStart, bool back)
{
    ImGuiIO&              io       = ImGui::GetIO();
    text_entry::Editor&   editor   = model_.TextEditor();
    text_entry::Keyboard& keyboard = model_.TextKeyboard();
    bool                  edited   = false;
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i)
    {
        const ImWchar c = io.InputQueueCharacters[i];
        if (c >= 0x20 && c < 0x7f)
            edited = editor.Append(static_cast<char>(c)) || edited;
    }
    if (padX || ImGui::IsKeyPressed(ImGuiKey_Backspace, true))
        edited = editor.Backspace() || edited;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
        pasteText();
    else if (edited)
        model_.SetTextRefused(false);
    if (padY && keyboard.GetLayout() == text_entry::Layout::kFull)
        keyboard.ToggleShift();
    keyboard.Move((right ? 1 : 0) - (left ? 1 : 0), (down ? 1 : 0) - (up ? 1 : 0));
    if (pressed({ ImGuiKey_GamepadFaceDown }, false))
        pressTextKey(keyboard.Current());
    else if (padStart || pressed({ ImGuiKey_Enter, ImGuiKey_KeypadEnter }, false))
        model_.CommitText();
    else if (back)
        model_.CancelText();
}

void OptionsPage::pressTextKey(const text_entry::Key& key)
{
    text_entry::Editor&   editor   = model_.TextEditor();
    text_entry::Keyboard& keyboard = model_.TextKeyboard();
    switch (key.kind)
    {
        case text_entry::KeyKind::kChar:
        case text_entry::KeyKind::kSpace:
            if (keyboard.Enabled(key, editor.GetRules().charset) && editor.Append(keyboard.CharOf(key)))
                model_.SetTextRefused(false);
            break;
        case text_entry::KeyKind::kShift:
            keyboard.ToggleShift();
            break;
        case text_entry::KeyKind::kPaste:
            pasteText();
            break;
        case text_entry::KeyKind::kClear:
            editor.Clear();
            model_.SetTextRefused(false);
            break;
        case text_entry::KeyKind::kDelete:
            editor.Backspace();
            model_.SetTextRefused(false);
            break;
        case text_entry::KeyKind::kDone:
            model_.CommitText();
            break;
    }
}

void OptionsPage::pasteText()
{
    const char* clipboard = ImGui::GetClipboardText();
    model_.SetTextRefused(!model_.TextEditor().Paste(clipboard ? clipboard : ""));
    forgetClipboardCache();
}

void OptionsPage::pasteInvite()
{
    const auto* row = model_.Row(model_.Highlight());
    if (!row || !row->HasCopyPaste() || model_.ModalOpen() || !takeRowAction())
        return;
    const char* clipboard = ImGui::GetClipboardText();
    model_.PasteSecret(clipboard ? clipboard : "");
    forgetClipboardCache();
}

// Read the saved invite at copy time; wipe the model copy after the clipboard transfer.
void OptionsPage::copyInvite()
{
    const auto* row = model_.Row(model_.Highlight());
    if (!row || !row->CanCopy() || model_.ModalOpen() || !takeRowAction())
        return;
    model_.CopySecret([](const char* value)
                      {
                          ImGui::SetClipboardText(value);
                      });
}

void OptionsPage::drawPage(ImGuiIO& io)
{
    const float base = ImGui::GetStyle().FontSizeBase;
    loadGlyphs();

    // PushFont(nullptr, 0) preserves size when no font base is available.
    ImGui::PushFont(nullptr, base * kBodyScale);
    // Read data before layout: Achievements has tabs only with custom entries.
    if (currentKind() == PageKind::kMods)
        refreshModsIfDue();
    else if (currentKind() == PageKind::kAchievements)
        refreshAchievementsIfDue();
    if (pages_.Current() != wheelPage_ || currentTab() != wheelTab_)
    {
        releaseWheel();
        wheelPage_ = pages_.Current();
        wheelTab_  = currentTab();
    }
    Metrics m;
    m.em     = ImGui::GetFontSize();
    m.size   = io.DisplaySize;
    m.column = std::min(m.size.x - 4.0f * m.em, kColumnEm * m.em);
    m.left   = std::floor((m.size.x - m.column) * 0.5f);
    {
        const int   stripRows = (pages_.Switchable() ? 1 : 0) + (tabCount() > 0 ? 1 : 0);
        const float rowH      = currentKind() == PageKind::kMods ? kModRowEm : currentKind() == PageKind::kAchievements ? kAchRowEm
                                                                                                                        : kRowEm;
        int         tipLines  = 0;
        int         noteLines = 0;
        footerLines(m.column - 2.0f * m.em, tipLines, noteLines);
        const float lineH       = ImGui::GetTextLineHeight();
        const float footerFull  = options_skin::FooterHeight(m.em, lineH, tipLines, noteLines);
        const float footerFloor = options_skin::FooterHeight(m.em, lineH, std::min(tipLines, 1), std::min(noteLines, 1));
        const auto  chrome      = options_skin::ChromeFor(m.size.y, m.em, stripRows, rowH * m.em, kMinListRows, kFrameBorder, footerFull, footerFloor);
        m.banner                = chrome.banner;
        m.footer                = chrome.footer;
        m.strip                 = chrome.strip;
        m.titleStrip            = chrome.titleStrip;
        m.margin                = chrome.margin;
        m.strips                = chrome.strip * static_cast<float>(stripRows);
    }
    m.top = m.banner + 2.0f + m.strips;

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(m.size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##rerevved_options", nullptr, flags))
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        drawBackdrop(m);

        {
            ImGui::PushFont(nullptr, base * kTitleScale);
            const std::string title = pages_.Title(pages_.Current());
            const char*       text  = title.c_str();
            const ImVec2      size  = ImGui::CalcTextSize(text);
            const ImVec2      at(std::floor((m.size.x - size.x) * 0.5f), std::floor((m.banner - size.y) * 0.5f));
            dl->AddText(ImVec2(at.x + 2.0f, at.y + 2.0f), color(kPanelEdge, 0.7f), text);
            dl->AddText(at, color(rgba(255, 255, 255)), text);
            ImGui::PopFont();
        }
        drawBand(m);

        {
            float stripTop = m.banner + 2.0f;
            if (pages_.Switchable())
            {
                std::vector<std::string> titles;
                for (size_t i = 0; i < pages_.Count(); ++i)
                    titles.push_back(pages_.Title(i));
                int       step    = 0;
                const int clicked = drawStrip(titles, pages_.Current(), stripTop, kHintLT, kHintRT, m, step, -1);
                if (!modsBusy())
                {
                    size_t target = static_cast<size_t>(-1);
                    if (clicked >= 0)
                        target = static_cast<size_t>(clicked);
                    else if (step < 0)
                        target = (pages_.Current() + pages_.Count() - 1) % pages_.Count();
                    else if (step > 0)
                        target = (pages_.Current() + 1) % pages_.Count();
                    if (target < pages_.Count() && target != pages_.Current())
                    {
                        pendingPage_   = target;
                        deferredInput_ = nullptr;
                    }
                }
                stripTop += m.strip;
            }
            if (tabCount() > 0)
            {
                std::vector<std::string> titles;
                switch (currentKind())
                {
                    case PageKind::kOptions:
                        for (const auto& section : model_.Sections())
                            titles.push_back(section.title);
                        break;
                    case PageKind::kMods:
                        titles = { pageText(title_text::Id::kModsTabPackages), pageText(title_text::Id::kModsTabAssets), pageText(title_text::Id::kModsTabMaps) };
                        break;
                    case PageKind::kAchievements:
                        titles = { pageText(title_text::Id::kAchievementsOriginal), pageText(title_text::Id::kAchievementsCustom) };
                        break;
                }
                int       step    = 0;
                const int clicked = drawStrip(titles, currentTab(), stripTop, kHintLB, kHintRB, m, step, static_cast<int>(pages_.Current()));
                if (clicked >= 0)
                    selectTab(static_cast<size_t>(clicked));
                else if (step != 0)
                    stepTab(step);
            }
            runDeferredInput();
        }

        drawFrame(m);
        drawPanelSummary(m);
        if (currentKind() == PageKind::kAchievements)
            drawAchievementsBody(io, m);
        else if (currentKind() == PageKind::kMods)
            drawModsBody(io, m);
        else
            drawOptionsBody(io, m);
        drawLegend(m);
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
    shownKind_ = currentKind();
}

void OptionsPage::drawOptionsBody(ImGuiIO& io, const Metrics& m)
{
    const rex::ui::Style& style    = imgui_drawer()->style();
    const auto&           sections = model_.Sections();
    const size_t          tab      = model_.Tab();
    if (tab != shownTab_)
    {
        shownTab_      = tab;
        ensureVisible_ = true;
        scrollTo_      = 0.0f;
        scrollPage_    = pages_.Current();
        scrollTab_     = tab;
    }
    float scrollY   = 0.0f;
    float scrollMax = 0.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    if (beginBodyChild("##rerevved_options_rows", m))
    {
        ImDrawList*  rows      = ImGui::GetWindowDrawList();
        const float  rowH      = kRowEm * m.em;
        const float  pad       = kPadEm * m.em;
        const float  valueW    = std::min(kValueEm * m.em, ImGui::GetWindowWidth() * 0.55f);
        const float  arrowW    = kArrowEm * m.em;
        const ImVec2 origin    = ImGui::GetCursorScreenPos();
        float        y         = origin.y;
        const bool   requested = ensureVisible_;
        // Apply rebuilds sections, so defer it until iteration ends.
        int  stepDirection = 0;
        bool activateRow   = false;
        // A click on a row button: 1 runs or activates, 2 pastes, 3 copies.
        int buttonAction = 0;
        if (tab < sections.size())
        {
            const auto& section     = sections[tab];
            size_t      flat        = model_.SectionStart(tab);
            size_t      subheadings = 0;
            for (size_t r = 0; r < section.rows.size(); ++r)
            {
                if (startsGroup(section, r))
                    ++subheadings;
            }
            const float  panelH = pad + rowH * static_cast<float>(section.rows.size() + subheadings) + pad;
            const ImVec2 pMin(origin.x, y);
            const ImVec2 pMax(origin.x + ImGui::GetWindowWidth(), y + panelH);

            float rowY = pMin.y + pad;
            for (size_t r = 0; r < section.rows.size(); ++r, ++flat)
            {
                const OptionRow& row = section.rows[r];
                if (startsGroup(section, r))
                {
                    const float  textY = rowY + (rowH - m.em) * 0.5f;
                    const ImVec2 size  = ImGui::CalcTextSize(row.group.c_str());
                    const float  midY  = rowY + rowH * 0.5f;
                    const float  textX = pMin.x + pad * 2.0f + m.em * 1.5f;
                    rows->AddLine(ImVec2(pMin.x + pad * 2.0f, midY), ImVec2(textX - m.em * 0.5f, midY), color(kEdge, 0.8f));
                    rows->AddText(ImVec2(textX, textY), color(kGlow), row.group.c_str());
                    rows->AddLine(ImVec2(textX + size.x + m.em * 0.5f, midY), ImVec2(pMax.x - pad * 2.0f, midY), color(kEdge, 0.8f));
                    rowY += rowH;
                }
                const ImVec2 min(pMin.x + pad, rowY);
                const ImVec2 max(pMax.x - pad, rowY + rowH);
                const bool   highlighted = flat == model_.Highlight();

                ImGui::PushID(static_cast<int>(flat));
                ImGui::SetCursorScreenPos(min);
                ImGui::InvisibleButton("##row", ImVec2(max.x - min.x, rowH), ImGuiButtonFlags_MouseButtonLeft);
                const bool hovered = ImGui::IsItemHovered();
                const bool held    = ImGui::IsItemActive();
                const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

                const float  cellMax = max.x - pad;
                const float  cellMin = cellMax - valueW;
                const ImVec2 valueMin(cellMin + arrowW, min.y);
                const ImVec2 valueMax(cellMax - arrowW, max.y);
                if (highlighted)
                {
                    drawBar(style, rows, min, max, rex::ui::ChromeState::kSelected);
                    // Off-grid values can step either way, even at the ends.
                    const bool  choice   = row.kind == native_settings::RowKind::kChoice;
                    const bool  canLeft  = choice && (row.offGrid || row.current > 0);
                    const bool  canRight = choice && (row.offGrid || row.current + 1 < row.choices.size());
                    const float half     = m.em * 0.45f;
                    const auto  arrow    = [&](ImVec2 center, bool left, float alpha)
                    {
                        if (!drawSkinArrow(center, kValueArrowEm * m.em, left ? 2 : 0, alpha))
                            drawArrow(rows, center, half, left, alpha);
                    };
                    if (choice)
                    {
                        arrow(ImVec2(cellMin + arrowW * 0.5f, min.y + rowH * 0.5f), true, canLeft ? 1.0f : 0.25f);
                        arrow(ImVec2(cellMax - arrowW * 0.5f, min.y + rowH * 0.5f), false, canRight ? 1.0f : 0.25f);
                    }
                }
                if (hovered && !highlighted && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
                    model_.Select(flat);

                const float textY = min.y + (rowH - m.em) * 0.5f;
                rows->PushClipRect(ImVec2(min.x, min.y), ImVec2(cellMin, max.y), true);
                rows->AddText(ImVec2(min.x + pad, textY), color(rgba(255, 255, 255)), row.label.c_str());
                rows->PopClipRect();

                // Open Log Folder and Invite Code share hit zones; covered rows cannot act.
                struct Zone
                {
                    ImVec2      min;
                    ImVec2      size;
                    int         glyph;
                    int         action;
                    bool        enabled;
                    std::string caption;
                };

                const std::string shown = model_.IsCapturing(row.key) ? pageText(title_text::Id::kBindPress) : row.Caption();
                if (row.kind == native_settings::RowKind::kBind && row.conflicts > 0)
                    rows->AddText(ImVec2(cellMin + 0.1f * m.em, textY), color(kArrow), "(!)");
                if (highlighted)
                    clippedBind_.clear();
                const KeyArt* keycap        = row.kind == native_settings::RowKind::kBind && !model_.IsCapturing(row.key) ? bindArt(row) : nullptr;
                const float   capH          = rowH * 0.75f;
                const float   capW          = keycap ? options_glyphs::WidthForHeight(keycap->width, keycap->height, capH) : 0.0f;
                const auto    drawBindValue = [&](float lo, float hi, ImU32 col)
                {
                    if (keycap)
                    {
                        const float x = std::floor(lo + (hi - lo - capW) * 0.5f);
                        rows->AddImage(reinterpret_cast<ImTextureID>(keycap->texture.get()), ImVec2(x, min.y + (rowH - capH) * 0.5f), ImVec2(x + capW, min.y + (rowH + capH) * 0.5f));
                        return;
                    }
                    rows->PushClipRect(ImVec2(lo, min.y), ImVec2(hi, max.y), true);
                    drawCenteredText(rows, ImVec2(lo, min.y), ImVec2(hi, max.y), shown.c_str(), col);
                    rows->PopClipRect();
                };
                std::vector<Zone> zones;
                if (row.HasOpenButton() || row.HasCopyPaste() || (row.HasBindButtons() && highlighted && !model_.ModalOpen()))
                {
                    const float height = rowH * 0.8f;
                    const float glyph  = height * 0.8f;
                    const float top    = min.y + (rowH - height) * 0.5f;
                    const float gap    = 0.4f * m.em;
                    const auto  zone   = [&](int glyphId, int action, bool enabled, std::string caption)
                    {
                        const float want  = height * 0.6f + (glyphId == kGlyphNone ? 0.0f : glyphWidth(glyphId, glyph, glyph)) + ImGui::CalcTextSize(caption.c_str()).x;
                        const float width = std::min(want, valueW * 0.5f);
                        zones.push_back({ ImVec2(0.0f, top), ImVec2(width, height), glyphId, action, enabled, std::move(caption) });
                    };
                    if (row.HasOpenButton())
                    {
                        zone(kGlyphA, 1, true, row.Caption());
                        zones[0].min.x = std::floor(cellMin + (valueW - zones[0].size.x) * 0.5f);
                    }
                    else if (row.HasBindButtons())
                    {
                        const float left      = cellMin + arrowW;
                        const float valueSpan = keycap ? capW : ImGui::CalcTextSize(shown.c_str()).x;
                        const auto  place     = [&](bool compact)
                        {
                            zones.clear();
                            const std::string clearCaption = compact ? (keyboardFamily() ? std::string(kChordClear) : std::string()) : rowButtonCaption(keyboardFamily(), kChordClear, pageText(title_text::Id::kKeyClear));
                            const std::string resetCaption = compact ? (keyboardFamily() ? std::string(kChordReset) : std::string()) : rowButtonCaption(keyboardFamily(), kChordReset, pageText(title_text::Id::kLegendReset));
                            zone(keyboardFamily() ? kGlyphNone : kGlyphX, 4, row.CanClearBind(), clearCaption);
                            zone(keyboardFamily() ? kGlyphNone : kGlyphY, 5, row.CanResetBind(), resetCaption);
                            zones[1].min.x = std::floor(cellMax - zones[1].size.x);
                            zones[0].min.x = std::floor(zones[1].min.x - gap - zones[0].size.x);
                        };
                        place(false);
                        if (valueSpan > zones[0].min.x - gap - left)
                            place(true);
                        if (valueSpan > zones[0].min.x - gap - left)
                            zones.clear();
                        const float right = zones.empty() ? cellMax - arrowW : zones[0].min.x - gap;
                        drawBindValue(left, right, color(rgba(255, 255, 255)));
                        if (valueSpan > right - left)
                            clippedBind_ = row.label + ": " + row.Caption();
                    }
                    else
                    {
                        zone(keyboardFamily() ? kGlyphNone : kGlyphX, 2, true, rowButtonCaption(keyboardFamily(), kChordPaste, pageText(title_text::Id::kKeyPaste)));
                        zone(keyboardFamily() ? kGlyphNone : kGlyphY, 3, row.CanCopy(), rowButtonCaption(keyboardFamily(), kChordCopy, pageText(title_text::Id::kButtonCopy)));
                        zones[1].min.x     = std::floor(cellMax - zones[1].size.x);
                        zones[0].min.x     = std::floor(zones[1].min.x - gap - zones[0].size.x);
                        const ImVec2 state = ImGui::CalcTextSize(row.Caption().c_str());
                        const float  room  = zones[0].min.x - gap - cellMin;
                        if (state.x <= room)
                            rows->AddText(ImVec2(zones[0].min.x - gap - state.x, min.y + (rowH - state.y) * 0.5f), color(highlighted ? rgba(255, 255, 255) : kValue), row.Caption().c_str());
                    }
                }
                else if (row.kind == native_settings::RowKind::kBind)
                {
                    drawBindValue(valueMin.x, valueMax.x, color(highlighted ? rgba(255, 255, 255) : kValue));
                }
                else
                {
                    drawCenteredText(rows, valueMin, valueMax, shown.c_str(), color(highlighted ? rgba(255, 255, 255) : kValue));
                }
                const ImVec2 mouse   = ImGui::GetMousePos();
                int          zoneHit = -1;
                for (size_t z = 0; z < zones.size(); ++z)
                {
                    const Zone& zn   = zones[z];
                    const bool  over = mouse.x >= zn.min.x && mouse.x < zn.min.x + zn.size.x && mouse.y >= zn.min.y && mouse.y < zn.min.y + zn.size.y;
                    if (over && hovered)
                        zoneHit = static_cast<int>(z);
                    const bool live  = zn.enabled && over && hovered && !model_.ModalOpen();
                    const auto state = live && held ? rex::ui::ChromeState::kActive : live ? rex::ui::ChromeState::kHovered
                                                                                           : rex::ui::ChromeState::kNormal;
                    drawButtonFace(zn.caption.c_str(), zn.glyph, zn.min, zn.size, state, !zn.enabled);
                }

                if (clicked && !zones.empty() && !model_.ModalOpen())
                {
                    model_.Select(flat);
                    if (zoneHit >= 0 && zones[static_cast<size_t>(zoneHit)].enabled)
                        buttonAction = zones[static_cast<size_t>(zoneHit)].action;
                    else if (row.HasBindButtons() && zoneHit < 0)
                        activateRow = true;
                    else if (row.HasCopyPaste() && highlighted && zoneHit < 0 && mouse.x >= cellMin)
                        activateRow = true;
                }
                else if (clicked)
                {
                    const float mouseX = ImGui::GetMousePos().x;
                    if (row.kind == native_settings::RowKind::kBind && !model_.ModalOpen())
                    {
                        model_.Select(flat);
                        activateRow = true;
                    }
                    else if (highlighted && mouseX >= cellMin && mouseX < cellMin + arrowW)
                        stepDirection = -1;
                    else if (highlighted && mouseX >= cellMax - arrowW && mouseX < cellMax)
                        stepDirection = 1;
                    else if (highlighted && mouseX >= cellMin)
                        activateRow = true;
                    else
                        model_.Select(flat);
                }
                if (highlighted && requested)
                {
                    const float top    = r == 0 ? pMin.y : startsGroup(section, r) ? min.y - rowH
                                                                                   : min.y;
                    const float margin = m.em * 0.5f;
                    const float winTop = ImGui::GetWindowPos().y;
                    const float winEnd = winTop + ImGui::GetWindowHeight();
                    if (top < winTop + margin)
                        ImGui::SetScrollY(ImGui::GetScrollY() - (winTop + margin - top));
                    else if (max.y > winEnd - margin)
                        ImGui::SetScrollY(ImGui::GetScrollY() + (max.y - (winEnd - margin)));
                    ensureVisible_ = false;
                }
                ImGui::PopID();
                rowY += rowH;
            }
            y += panelH;
        }
        if (stepDirection != 0)
            model_.Step(stepDirection);
        if (activateRow)
            activate();
        if (buttonAction == 1)
            activate();
        else if (buttonAction == 2)
            pasteInvite();
        else if (buttonAction == 3)
            copyInvite();
        else if (buttonAction == 4)
            clearBind();
        else if (buttonAction == 5)
            resetBind();
        // Gives the child its scroll extent.
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
        scrollY   = ImGui::GetScrollY();
        scrollMax = ImGui::GetScrollMaxY();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    drawScrollArrows(m, scrollY, scrollMax, kRowEm * m.em);
}

bool OptionsPage::modsBusy() const
{
    return currentKind() == PageKind::kMods && (activeModel().Grabbing() || activeModel().Confirmation() != mods_list::Confirm::kNone);
}

void OptionsPage::refreshMods()
{
    modsFrame_    = ImGui::GetFrameCount();
    auto* kernel  = REX_KERNEL_STATE();
    auto* runtime = kernel ? kernel->emulator() : nullptr;

    std::vector<mods_list::Mod> mods;
    mods_list::RuntimeState     state;
    if (runtime)
    {
        const auto& catalog = runtime->mod_catalog();
        for (const auto& package : catalog.packages)
        {
            mods_list::Mod mod;
            mod.id          = package.id.empty() ? package.folder_name : package.id;
            mod.name        = package.display_name.empty() ? mod.id : package.display_name;
            mod.folder      = package.folder_name;
            mod.version     = package.version;
            mod.author      = package.author;
            mod.description = package.description;
            mod.iconPath    = package.icon_path;
            mod.native      = package.payload_kind == rex::system::ModPayloadKind::kNative;
            for (const auto& diagnostic : package.diagnostics)
            {
                const bool error = diagnostic.severity == rex::system::ModDiagnosticSeverity::kError;
                mod.messages.push_back({ error, severityText(error, diagnostic.message) });
            }
            mods.push_back(std::move(mod));
        }
        for (const auto& diagnostic : catalog.diagnostics)
        {
            if (!diagnostic.blocking)
            {
                const bool error = diagnostic.severity == rex::system::ModDiagnosticSeverity::kError;
                state.catalogMessages.push_back({ error, severityText(error, diagnostic.message) });
            }
        }
        state.applied          = runtime->mod_order_ids();
        state.lock             = runtime->mod_loadout_lock_reason();
        state.orderFileExists  = runtime->mod_order_file_exists();
        state.orderFileInvalid = runtime->mod_order_file_invalid();
        state.restartRequired  = runtime->restart_required();
        const auto* host       = runtime->mod_script_host();
        state.liveApply        = host && host->supports_replacement();
        const auto& current    = runtime->mod_loadout_diagnostics();
        state.currentInvalid   = !current.empty();
        if (!current.empty())
            state.currentProblem = describe(current.front());
    }
    mods_.Update(std::move(mods), std::move(state));
    if (runtime)
    {
        const auto  selection = runtime->ValidateModLoadout(mods_.Staged());
        std::string problem;
        if (!selection.diagnostics.empty())
            problem = describe(selection.diagnostics.front());
        mods_.SetStagedValidity(selection.IsValid(), problem);
    }
    modsValidatedRevision_ = mods_.Revision();
}

void OptionsPage::refreshAssets()
{
    assetsFrame_  = ImGui::GetFrameCount();
    auto* kernel  = REX_KERNEL_STATE();
    auto* runtime = kernel ? kernel->emulator() : nullptr;

    std::vector<mods_list::Mod> packs;
    mods_list::RuntimeState     state;
    if (runtime)
    {
        packs                  = assets_list::FromCatalog(runtime->asset_overlay_catalog());
        state.applied          = runtime->asset_order_ids();
        state.orderFileExists  = runtime->asset_order_file_exists();
        state.orderFileInvalid = runtime->asset_order_file_invalid();
        state.restartRequired  = runtime->asset_overlay_restart_required();
        const auto& current    = runtime->asset_loadout_diagnostics();
        state.currentInvalid   = !current.empty();
        if (!current.empty())
            state.currentProblem = describe(current.front());
    }
    assets_.Update(std::move(packs), std::move(state));
    if (runtime)
    {
        const auto  selection = runtime->ValidateAssetOverlayLoadout(assets_.Staged());
        std::string problem;
        if (!selection.diagnostics.empty())
            problem = describe(selection.diagnostics.front());
        assets_.SetStagedValidity(selection.IsValid(), problem);
    }
    assetsValidatedRevision_ = assets_.Revision();
}

// No runtime catalog: scan <exe>/maps and read the profile's map_order.txt.
void OptionsPage::refreshMaps()
{
    mapsFrame_         = ImGui::GetFrameCount();
    auto        loaded = maps_tab::Read(mapsCatalog_, mapsProfileRoot(), map_packs::StartupOrder());
    std::string problem;
    if (!loaded.diagnostics.empty())
        loaded.state.currentProblem = describe(loaded.diagnostics.front());
    maps_.Update(std::move(loaded.packs), std::move(loaded.state));
    const auto selection = maps_tab::Validate(mapsCatalog_, mapsProfileRoot(), maps_.Staged());
    if (!selection.diagnostics.empty())
        problem = describe(selection.diagnostics.front());
    maps_.SetStagedValidity(selection.IsValid(), problem);
    mapsValidatedRevision_ = maps_.Revision();
}

std::filesystem::path OptionsPage::mapsProfileRoot() const
{
    auto* kernel  = REX_KERNEL_STATE();
    auto* runtime = kernel ? kernel->emulator() : nullptr;
    return runtime ? std::filesystem::path(runtime->user_data_root()) : std::filesystem::path();
}

void OptionsPage::rescanMaps()
{
    mapsCatalog_ = rex::system::DiscoverAssetOverlayCatalog(rex::filesystem::GetExecutableFolder() / map_packs::kMapsDirectory);
}

void OptionsPage::refreshModsIfDue()
{
    const int  frame = ImGui::GetFrameCount();
    const bool entry = shownKind_ != PageKind::kMods;
    if (entry)
    {
        auto* kernel  = REX_KERNEL_STATE();
        auto* runtime = kernel ? kernel->emulator() : nullptr;
        if (runtime)
        {
            runtime->RescanModCatalog();
            runtime->RescanAssetOverlayCatalog();
        }
        rescanMaps();
    }
    if (modsFrame_ < 0 || entry || frame - modsFrame_ >= kModRefreshFrames || mods_.Revision() != modsValidatedRevision_)
        refreshMods();
    if (assetsFrame_ < 0 || entry || frame - assetsFrame_ >= kModRefreshFrames || assets_.Revision() != assetsValidatedRevision_)
        refreshAssets();
    if (mapsFrame_ < 0 || entry || frame - mapsFrame_ >= kModRefreshFrames || maps_.Revision() != mapsValidatedRevision_)
        refreshMaps();
}

void OptionsPage::activateMod()
{
    if (!takeRowAction())
        return;
    activeModel().Activate();
    modsEnsure_ = true;
}

void OptionsPage::grabMod()
{
    if (!takeRowAction())
        return;
    auto& model = activeModel();
    if (model.Grabbing())
        model.Drop();
    else
        model.Grab();
}

void OptionsPage::shiftMod(int direction)
{
    if (!takeRowAction())
        return;
    activeModel().Shift(direction);
    modsEnsure_ = true;
}

void OptionsPage::refreshActiveTab()
{
    if (modsTab_ == maps_tab::kAssetsTab)
        refreshAssets();
    else if (modsTab_ == maps_tab::kMapsTab)
        refreshMaps();
    else
        refreshMods();
}

void OptionsPage::acceptMods()
{
    if (!takeRowAction())
        return;
    auto& model = activeModel();
    switch (model.Accept())
    {
        case mods_list::Accepted::kClose:
            finishDiscard();
            break;
        case mods_list::Accepted::kApplyReplacing:
            refreshActiveTab();
            if (model.CanApply())
                runApply(true);
            break;
        case mods_list::Accepted::kNone:
            break;
    }
}

void OptionsPage::applyMods()
{
    // F2 may have saved since the last refresh: decide on the runtime as it is.
    if (!takeRowAction())
        return;
    refreshActiveTab();
    if (activeModel().Apply() == mods_list::ApplyStep::kApply)
        runApply(false);
}

void OptionsPage::runApply(bool replace)
{
    auto* kernel  = REX_KERNEL_STATE();
    auto* runtime = kernel ? kernel->emulator() : nullptr;
    if (!runtime)
        return;
    if (modsTab_ == maps_tab::kMapsTab)
    {
        const std::vector<std::string> staged = maps_.Staged();
        const auto                     result = maps_tab::Apply(mapsCatalog_, mapsProfileRoot(), staged, replace);
        if (result.succeeded())
        {
            maps_.ApplyFinished(mods_list::ApplyResult::kSaved, {});
        }
        else if (result.status == rex::system::AssetOverlayLoadoutApplyStatus::kInvalidCurrent)
        {
            maps_.ApplyFinished(mods_list::ApplyResult::kInvalidCurrent, {});
        }
        else
        {
            maps_.ApplyFinished(mods_list::ApplyResult::kFailed,
                                result.diagnostics.empty() ? pageText(title_text::Id::kMapsSaveFailed) : result.diagnostics.front().message);
        }
        refreshMaps();
        return;
    }
    if (modsTab_ == maps_tab::kAssetsTab)
    {
        const std::vector<std::string> staged = assets_.Staged();
        const auto                     result = runtime->ApplyAssetOverlayLoadout(staged, replace);
        if (result.succeeded())
        {
            assets_.ApplyFinished(mods_list::ApplyResult::kSaved, {});
        }
        else if (result.status == rex::system::AssetOverlayLoadoutApplyStatus::kInvalidCurrent)
        {
            assets_.ApplyFinished(mods_list::ApplyResult::kInvalidCurrent, {});
        }
        else
        {
            assets_.ApplyFinished(mods_list::ApplyResult::kFailed,
                                  result.diagnostics.empty() ? pageText(title_text::Id::kAssetsSaveFailed) : result.diagnostics.front().message);
        }
        refreshAssets();
        return;
    }
    const std::vector<std::string> staged = mods_.Staged();
    const auto                     result = runtime->ApplyModLoadout(staged, replace);
    if (result.succeeded())
    {
        mods_.ApplyFinished(mods_list::ApplyResult::kSaved, {});
    }
    else if (result.status == rex::system::ModLoadoutApplyStatus::kInvalidCurrent)
    {
        mods_.ApplyFinished(mods_list::ApplyResult::kInvalidCurrent, {});
    }
    else
    {
        mods_.ApplyFinished(mods_list::ApplyResult::kFailed,
                            result.diagnostics.empty() ? pageText(title_text::Id::kModsSaveFailed) : result.diagnostics.front().message);
    }
    refreshMods();
}

rex::ui::ImmediateTexture* OptionsPage::modIcon(const std::filesystem::path& path)
{
    if (path.empty())
        return nullptr;
    const std::string key = path.string();
    if (const auto it = modIcons_.find(key); it != modIcons_.end())
        return it->second.get();
    // A failed load is remembered, so nothing is tried before the drawer exists.
    auto* drawer = env_.immediateDrawer ? env_.immediateDrawer() : nullptr;
    if (!drawer)
        return nullptr;
    std::unique_ptr<rex::ui::ImmediateTexture> texture;
    std::string                                error;
    if (const auto bytes = readFile(path, &error))
    {
        int        width  = 0;
        int        height = 0;
        const auto rgba   = rex::ui::DecodeImageRGBA(bytes->data(), bytes->size(), width, height);
        if (!rgba.empty() && width > 0 && height > 0)
            texture = drawer->CreateTexture(static_cast<uint32_t>(width), static_cast<uint32_t>(height), rex::ui::ImmediateTextureFilter::kLinear, false, rgba.data());
    }
    auto* raw = texture.get();
    modIcons_.emplace(key, std::move(texture));
    return raw;
}

std::string OptionsPage::modsTip() const
{
    using title_text::Id;
    const auto& model = activeModel();
    switch (model.Confirmation())
    {
        case mods_list::Confirm::kDiscard:
            return modsTabText(Id::kModsDiscardAsk, Id::kAssetsDiscardAsk, Id::kMapsDiscardAsk);
        case mods_list::Confirm::kReplace:
            return modsTabText(Id::kModsReplaceAsk, Id::kAssetsReplaceAsk, Id::kMapsReplaceAsk);
        case mods_list::Confirm::kNone:
            break;
    }
    return {};
}

std::string OptionsPage::modsNote() const
{
    using mods_list::NoteKind;
    using title_text::Id;
    const auto& model = activeModel();
    if (model.Confirmation() != mods_list::Confirm::kNone)
        return {};
    const auto note = model.FooterNote();
    switch (note.kind)
    {
        case NoteKind::kNone:
            return {};
        case NoteKind::kLocked:
            return note.text == gameplay_session::kModsLockedReason ? pageText(Id::kModsLocked) : note.text;
        case NoteKind::kStatus:
        case NoteKind::kCatalog:
            return note.text;
        case NoteKind::kMoving:
            return modsTabText(Id::kModsMoving, Id::kAssetsMoving, Id::kMapsMoving);
        case NoteKind::kSelectedInvalid:
            return pageText(Id::kModsFixErrors) + (note.text.empty() ? std::string() : " " + note.text);
        case NoteKind::kCurrentInvalid:
            return pageText(Id::kModsReplaceFile) + (note.text.empty() ? std::string() : " " + note.text);
        case NoteKind::kUnsaved:
            return pageText(Id::kModsUnsaved);
        case NoteKind::kRestart:
            return pageText(Id::kModsRestart);
    }
    return {};
}

void OptionsPage::drawModsBody(ImGuiIO& io, const Metrics& m)
{
    using mods_list::RowKind;
    auto&                             model     = activeModel();
    const rex::ui::Style&             style     = imgui_drawer()->style();
    const rex::ui::AchievementsStyle& look      = style.achievements;
    const bool                        requested = modsEnsure_;
    const bool                        locked    = model.Locked();
    const bool                        question  = model.Confirmation() != mods_list::Confirm::kNone;
    const float                       alpha     = locked ? 0.6f : 1.0f;
    float                             scrollY   = 0.0f;
    float                             scrollMax = 0.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    if (beginBodyChild(modsTab_ == maps_tab::kAssetsTab ? "##rerevved_assets_rows" : modsTab_ == maps_tab::kMapsTab ? "##rerevved_maps_rows"
                                                                                                                    : "##rerevved_mods_rows",
                       m))
    {
        ImDrawList*  rows     = ImGui::GetWindowDrawList();
        const float  rowH     = kModRowEm * m.em;
        const float  pad      = kPadEm * m.em;
        const float  boxSize  = kModBoxEm * m.em;
        const float  iconSize = kModIconEm * m.em;
        const float  arrowW   = kModArrowEm * m.em;
        const ImVec2 origin   = ImGui::GetCursorScreenPos();
        const float  winTop   = ImGui::GetWindowPos().y;
        const float  winEnd   = winTop + ImGui::GetWindowHeight();
        const float  panelTop = origin.y;
        float        y        = panelTop;
        const float  width    = ImGui::GetWindowWidth();
        const auto&  list     = model.Rows();
        // Actions rebuild the rows being walked, so they wait for the walk.
        int  shiftDirection = 0;
        bool toggleRow      = false;
        if (list.empty())
        {
            const std::string text = modsTabText(title_text::Id::kModsNone, title_text::Id::kAssetsNone, title_text::Id::kMapsNone);
            const ImVec2      size = ImGui::CalcTextSize(text.c_str());
            rows->AddText(ImVec2(std::floor(origin.x + (width - size.x) * 0.5f), std::floor(panelTop + m.em)), color(kValue), text.c_str());
            y += 3.0f * m.em;
        }
        else
        {
            const float  panelH = pad + rowH * static_cast<float>(list.size()) + pad;
            const ImVec2 pMin(origin.x, panelTop);
            const ImVec2 pMax(origin.x + width, panelTop + panelH);

            for (size_t r = 0; r < list.size(); ++r)
            {
                const float rowY = pMin.y + pad + rowH * static_cast<float>(r);
                if (r == model.Highlight() && requested)
                {
                    const float top    = r == 0 ? pMin.y : rowY;
                    const float margin = m.em * 0.5f;
                    if (top < winTop + margin)
                        ImGui::SetScrollY(ImGui::GetScrollY() - (winTop + margin - top));
                    else if (rowY + rowH > winEnd - margin)
                        ImGui::SetScrollY(ImGui::GetScrollY() + (rowY + rowH - (winEnd - margin)));
                    modsEnsure_ = false;
                }
                if (rowY + rowH < winTop || rowY > winEnd)
                    continue;
                const auto&  row = list[r];
                const ImVec2 min(pMin.x + pad, rowY);
                const ImVec2 max(pMax.x - pad, rowY + rowH);
                const bool   highlighted = r == model.Highlight();
                const bool   enabled     = row.kind != RowKind::kDisabled;

                ImGui::PushID(row.id.c_str());
                ImGui::SetCursorScreenPos(min);
                ImGui::InvisibleButton("##row", ImVec2(max.x - min.x, rowH), ImGuiButtonFlags_MouseButtonLeft);
                const bool hovered = ImGui::IsItemHovered();
                const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
                ImGui::PopID();

                const float zoneMin = max.x - pad - arrowW;
                const bool  arrows  = highlighted && row.kind == RowKind::kEnabled;
                if (!question && !model.Grabbing())
                {
                    if (hovered && !highlighted && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
                        model.Select(r);
                    if (clicked)
                    {
                        const float mouseX = ImGui::GetMousePos().x;
                        if (!highlighted)
                            model.Select(r);
                        else if (arrows && mouseX >= zoneMin && mouseX < max.x - pad)
                            shiftDirection = ImGui::GetMousePos().y < min.y + rowH * 0.5f ? -1 : 1;
                        else
                            toggleRow = true;
                    }
                }

                if (row.kind == RowKind::kEnabled)
                    rows->AddRectFilled(min, max, color(look.row_unlocked_bg), look.row_rounding);
                if (highlighted)
                {
                    drawBar(style, rows, min, max, rex::ui::ChromeState::kSelected);
                    if (model.Grabbing())
                        rows->AddRect(min, max, color(kArrow), 0.0f, 0, 2.0f);
                }

                const ImVec2 boxMin(min.x + pad, min.y + (rowH - boxSize) * 0.5f);
                const ImVec2 boxMax(boxMin.x + boxSize, boxMin.y + boxSize);
                rows->AddRectFilled(boxMin, boxMax, color(kNavyDeep, 0.8f), 0.2f * m.em);
                rows->AddRect(boxMin, boxMax, color(enabled ? kArrow : kEdge, alpha), 0.2f * m.em, 0, 1.5f);
                if (enabled)
                {
                    const ImVec2 a(boxMin.x + boxSize * 0.2f, boxMin.y + boxSize * 0.55f);
                    const ImVec2 b(boxMin.x + boxSize * 0.42f, boxMin.y + boxSize * 0.78f);
                    const ImVec2 c(boxMin.x + boxSize * 0.82f, boxMin.y + boxSize * 0.24f);
                    rows->AddLine(a, b, color(kArrow, alpha), 2.5f);
                    rows->AddLine(b, c, color(kArrow, alpha), 2.5f);
                }
                const ImVec2 iconMin(boxMax.x + 0.6f * m.em, min.y + (rowH - iconSize) * 0.5f);
                const ImVec2 iconMax(iconMin.x + iconSize, iconMin.y + iconSize);
                rows->AddRectFilled(iconMin, iconMax, color(kNavyDeep, 0.8f), 0.2f * m.em);
                if (row.mod)
                {
                    if (auto* texture = modIcon(row.mod->iconPath))
                        rows->AddImage(reinterpret_cast<ImTextureID>(texture), iconMin, iconMax, ImVec2(0, 0), ImVec2(1, 1), ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, alpha)));
                }
                rows->AddRect(iconMin, iconMax, color(enabled ? kArrow : kEdge, enabled ? alpha : 0.7f * alpha), 0.2f * m.em);

                const float  textX  = iconMax.x + 0.8f * m.em;
                const float  stateR = max.x - pad - arrowW;
                const float  stateX = stateR - kModStateEm * m.em;
                const float  line1  = min.y + 0.55f * m.em;
                const float  line2  = min.y + 1.95f * m.em;
                const ImVec4 title  = highlighted ? rgba(255, 255, 255) : enabled ? look.unlocked_title
                                                                                  : look.locked_title;
                const ImVec4 desc   = highlighted ? rgba(255, 255, 255) : enabled ? look.unlocked_desc
                                                                                  : look.locked_desc;
                std::string  name;
                std::string  meta;
                std::string  second;
                bool         warn = false;
                if (row.mod)
                {
                    name = row.kind == RowKind::kEnabled ? std::to_string(row.position) + ". " + row.mod->name : row.mod->name;
                    if (!row.mod->version.empty())
                        meta = "v" + row.mod->version;
                    if (!row.mod->author.empty())
                        meta += (meta.empty() ? "" : "  ") + row.mod->author;
                    if (!row.mod->messages.empty())
                    {
                        second = row.mod->messages.front().text;
                        warn   = true;
                    }
                    else
                    {
                        second = row.mod->description;
                    }
                }
                else
                {
                    const std::vector<std::string> args = { row.id };
                    name                                = std::to_string(row.position) + ". " + achievements_list::Substitute(pageText(title_text::Id::kModUnavailable), args);
                    second                              = pageText(title_text::Id::kModUnavailableTip);
                    warn                                = true;
                }
                rows->PushClipRect(ImVec2(textX, min.y), ImVec2(stateX, max.y), true);
                rows->AddText(ImVec2(textX, line1), color(title, alpha), name.c_str());
                if (!meta.empty())
                    rows->AddText(ImVec2(textX + ImGui::CalcTextSize(name.c_str()).x + 0.8f * m.em, line1), color(highlighted ? rgba(255, 255, 255) : look.locked_desc, alpha), meta.c_str());
                rows->AddText(ImVec2(textX, line2), color(warn ? kArrow : desc, alpha), second.c_str());
                rows->PopClipRect();

                if (row.mod && row.mod->native)
                    drawRightText(rows, stateX, stateR, line2, pageText(title_text::Id::kModNative), color(highlighted ? rgba(255, 255, 255) : look.locked_desc, alpha));
                if (arrows)
                {
                    const float  half = m.em * 0.45f;
                    const ImVec2 up(zoneMin + arrowW * 0.5f, min.y + rowH * 0.3f);
                    const ImVec2 down(zoneMin + arrowW * 0.5f, min.y + rowH * 0.7f);
                    const float  upAlpha   = locked || row.position <= 1 ? 0.25f : 1.0f;
                    const float  downAlpha = locked || row.position >= model.EnabledCount() ? 0.25f : 1.0f;
                    if (!drawSkinArrow(up, kOrderArrowEm * m.em, 3, upAlpha))
                        drawVerticalArrow(rows, up, half, true, upAlpha);
                    if (!drawSkinArrow(down, kOrderArrowEm * m.em, 1, downAlpha))
                        drawVerticalArrow(rows, down, half, false, downAlpha);
                }
            }
            y = pMax.y;
        }
        if (shiftDirection != 0)
            shiftMod(shiftDirection);
        if (toggleRow)
            activateMod();
        // Gives the child its scroll extent.
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
        scrollY   = ImGui::GetScrollY();
        scrollMax = ImGui::GetScrollMaxY();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    drawScrollArrows(m, scrollY, scrollMax, kModRowEm * m.em);
}

void OptionsPage::refreshAchievements()
{
    achievementsFrame_ = ImGui::GetFrameCount();
    achievements_      = {};
    auto* kernel       = REX_KERNEL_STATE();
    if (!kernel)
        return;
    auto&      manager = kernel->achievements();
    const auto infos   = manager.ListAchievements();
    const auto all     = achievements_list::Build(
        infos,
        [&manager](uint32_t id)
        {
            return manager.IsUnlocked(id);
        },
        [&manager](uint32_t id)
        {
            return manager.GetUnlockTime(id);
        });
    achievements_ = achievements_list::Split(all);
    if (!achievements_.HasTabs())
        achievementTab_ = 0;
    for (size_t t = 0; t < 2; ++t)
    {
        const size_t count = achievements_.Tab(t).size();
        if (achievementRows_[t] >= count)
            achievementRows_[t] = count == 0 ? 0 : count - 1;
    }
    if (!icons_)
    {
        // The cache remembers a failed load, so it waits for the drawer.
        auto* drawer = env_.immediateDrawer ? env_.immediateDrawer() : nullptr;
        if (drawer && kernel->emulator())
            icons_ = std::make_unique<rex::ui::AchievementIconCache>(drawer, kernel->emulator());
    }
}

void OptionsPage::moveAchievement(int direction)
{
    const auto& list = achievements_.Tab(achievementTab_);
    size_t&     row  = achievementRows_[achievementTab_ == 1 ? 1 : 0];
    if (list.empty())
        return;
    if (direction < 0)
        row = row > 0 ? row - 1 : 0;
    else
        row = std::min(row + 1, list.size() - 1);
    achievementEnsure_ = true;
}

std::string OptionsPage::achievementStatus(const achievements_list::Entry& entry) const
{
    using title_text::Id;
    if (entry.unlocked)
    {
        if (const auto seconds = achievements_list::UnixSeconds(entry.unlockedAt))
        {
            const std::time_t when = static_cast<std::time_t>(*seconds);
            std::tm           local{};
            if (localTime(when, &local))
            {
                const std::vector<std::string> args = { achievements_list::FormatDate(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday) };
                return achievements_list::Substitute(pageText(Id::kAchievementUnlockedOn), args);
            }
        }
        return pageText(Id::kAchievementUnlocked);
    }
    return pageText(Id::kAchievementLocked);
}

void OptionsPage::refreshAchievementsIfDue()
{
    if (achievementsFrame_ < 0 || shownKind_ != PageKind::kAchievements || ImGui::GetFrameCount() - achievementsFrame_ >= kAchRefreshFrames)
        refreshAchievements();
}

void OptionsPage::drawPanelSummary(const Metrics& m)
{
    using title_text::Id;
    if (currentKind() == PageKind::kOptions || m.titleStrip < 1.2f * m.em)
        return;
    ImDrawList*  dl    = ImGui::GetWindowDrawList();
    const ImVec2 min   = frameMin(m);
    const ImVec2 max   = frameMax(m);
    const float  y     = std::floor(min.y + (m.titleStrip - m.em) * 0.5f);
    const float  left  = min.x + kFrameBorder + 0.8f * m.em;
    const float  right = max.x - kFrameBorder - kGutterEm * m.em;
    if (currentKind() == PageKind::kMods)
    {
        const auto&                    model  = activeModel();
        const std::vector<std::string> counts = { std::to_string(model.InstalledCount()), std::to_string(model.EnabledCount()) };
        const std::string              text   = achievements_list::Substitute(pageText(Id::kModsSummary), counts);
        dl->AddText(ImVec2(left, y), color(kGlow), text.c_str());
        return;
    }
    const auto                     totals = achievements_list::Summarize(achievements_.Tab(achievementTab_));
    const std::vector<std::string> counts = { std::to_string(totals.unlocked), std::to_string(totals.total) };
    const std::vector<std::string> score  = { std::to_string(totals.earned), std::to_string(totals.possible) };
    const std::string              first  = achievements_list::Substitute(pageText(Id::kAchievementsSummary), counts);
    const std::string              second = achievements_list::Substitute(pageText(Id::kAchievementsScore), score);
    dl->AddText(ImVec2(left, y), color(kGlow), first.c_str());
    drawRightText(dl, left, right, y, second, color(kArrow));
}

void OptionsPage::drawAchievementsBody(ImGuiIO& io, const Metrics& m)
{
    const rex::ui::Style&             style     = imgui_drawer()->style();
    const rex::ui::AchievementsStyle& look      = style.achievements;
    const bool                        requested = achievementEnsure_;
    const size_t                      tab       = achievementTab_ == 1 && achievements_.HasTabs() ? 1 : 0;
    const auto&                       list      = achievements_.Tab(tab);
    size_t&                           row       = achievementRows_[tab];
    float                             scrollY   = 0.0f;
    float                             scrollMax = 0.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    if (beginBodyChild(tab == 1 ? "##rerevved_achievements_custom_rows" : "##rerevved_achievements_rows", m))
    {
        ImDrawList*  rows     = ImGui::GetWindowDrawList();
        const float  rowH     = kAchRowEm * m.em;
        const float  pad      = kPadEm * m.em;
        const float  iconSize = kAchIconEm * m.em;
        const ImVec2 origin   = ImGui::GetCursorScreenPos();
        const float  winTop   = ImGui::GetWindowPos().y;
        const float  winEnd   = winTop + ImGui::GetWindowHeight();
        const float  panelTop = origin.y;
        float        y        = panelTop;
        const float  width    = ImGui::GetWindowWidth();
        if (list.empty())
        {
            const std::string text = pageText(title_text::Id::kAchievementsNone);
            const ImVec2      size = ImGui::CalcTextSize(text.c_str());
            rows->AddText(ImVec2(std::floor(origin.x + (width - size.x) * 0.5f), std::floor(panelTop + m.em)), color(kValue), text.c_str());
            y += 3.0f * m.em;
        }
        else
        {
            const float  panelH = pad + rowH * static_cast<float>(list.size()) + pad;
            const ImVec2 pMin(origin.x, panelTop);
            const ImVec2 pMax(origin.x + width, panelTop + panelH);

            for (size_t r = 0; r < list.size(); ++r)
            {
                const float rowY = pMin.y + pad + rowH * static_cast<float>(r);
                if (r == row && requested)
                {
                    const float top    = r == 0 ? pMin.y : rowY;
                    const float margin = m.em * 0.5f;
                    if (top < winTop + margin)
                        ImGui::SetScrollY(ImGui::GetScrollY() - (winTop + margin - top));
                    else if (rowY + rowH > winEnd - margin)
                        ImGui::SetScrollY(ImGui::GetScrollY() + (rowY + rowH - (winEnd - margin)));
                    achievementEnsure_ = false;
                }
                if (rowY + rowH < winTop || rowY > winEnd)
                    continue;
                const auto&  entry = list[r];
                const ImVec2 min(pMin.x + pad, rowY);
                const ImVec2 max(pMax.x - pad, rowY + rowH);
                const bool   highlighted = r == row;

                ImGui::PushID(static_cast<int>(entry.id));
                ImGui::SetCursorScreenPos(min);
                ImGui::InvisibleButton("##row", ImVec2(max.x - min.x, rowH), ImGuiButtonFlags_MouseButtonLeft);
                const bool hovered = ImGui::IsItemHovered();
                const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
                ImGui::PopID();
                if ((clicked || (hovered && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))) && !highlighted)
                    row = r;

                if (entry.unlocked)
                    rows->AddRectFilled(min, max, color(look.row_unlocked_bg), look.row_rounding);
                if (highlighted)
                    drawBar(style, rows, min, max, rex::ui::ChromeState::kSelected);

                const ImVec2 iconMin(min.x + pad, min.y + (rowH - iconSize) * 0.5f);
                const ImVec2 iconMax(iconMin.x + iconSize, iconMin.y + iconSize);
                rows->AddRectFilled(iconMin, iconMax, color(kNavyDeep, 0.8f), 0.2f * m.em);
                if (icons_)
                {
                    rex::system::AchievementInfo source;
                    source.id        = entry.id;
                    source.icon_path = entry.iconPath;
                    source.image_id  = entry.imageId;
                    if (auto* texture = icons_->GetIcon(source))
                        rows->AddImage(reinterpret_cast<ImTextureID>(texture), iconMin, iconMax, ImVec2(0, 0), ImVec2(1, 1), ImGui::GetColorU32(entry.unlocked ? look.unlocked_icon_tint : look.locked_icon_tint));
                }
                rows->AddRect(iconMin, iconMax, color(entry.unlocked ? kArrow : kEdge, entry.unlocked ? 1.0f : 0.7f), 0.2f * m.em);

                const float  textX  = iconMax.x + 0.8f * m.em;
                const float  rightW = std::min(kAchRightEm * m.em, (max.x - textX) * 0.45f);
                const float  rightX = max.x - pad - rightW;
                const float  line1  = min.y + 0.4f * m.em;
                const float  line2  = min.y + 1.55f * m.em;
                const ImVec4 title  = highlighted ? rgba(255, 255, 255) : entry.unlocked ? look.unlocked_title
                                                                                         : look.locked_title;
                const ImVec4 desc   = highlighted ? rgba(255, 255, 255) : entry.unlocked ? look.unlocked_desc
                                                                                         : look.locked_desc;
                rows->PushClipRect(ImVec2(textX, min.y), ImVec2(rightX, max.y), true);
                rows->AddText(ImVec2(textX, line1), color(title), entry.label.c_str());
                rows->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(textX, line2), color(desc), entry.description.c_str(), nullptr, rightX - textX - 0.5f * m.em);
                rows->PopClipRect();

                drawRightText(rows, rightX, max.x - pad, line1, pageText(title_text::Id::kAchievementGamerscore, { std::to_string(entry.gamerscore) }), color(look.badge_gamerscore));
                const ImVec4 state = entry.unlocked ? look.unlocked_title : look.locked_desc;
                drawRightText(rows, rightX, max.x - pad, line2, achievementStatus(entry), color(highlighted ? rgba(255, 255, 255) : state));
            }
            y = pMax.y;
        }
        // Gives the child its scroll extent.
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
        scrollY   = ImGui::GetScrollY();
        scrollMax = ImGui::GetScrollMaxY();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    drawScrollArrows(m, scrollY, scrollMax, kAchRowEm * m.em);
}

namespace
{

constexpr const char* kHintLabels[3][4] = {
    { "LB", "RB", "LT", "RT" },
    { "L1", "R1", "L2", "R2" },
    { "PgUp", "PgDn", "Q", "E" },
};

} // namespace

ImVec2 OptionsPage::frameMin(const Metrics& m) const
{
    return ImVec2(m.left, std::floor(m.top + 0.5f * m.margin));
}

ImVec2 OptionsPage::frameMax(const Metrics& m) const
{
    return ImVec2(m.left + m.column, std::floor(m.size.y - m.footer - 0.5f * m.margin));
}

rex::ui::ImmediateTexture* OptionsPage::skinTexture(options_skin::Art art)
{
    loadGlyphs();
    Glyph&                      slot  = skin_[static_cast<size_t>(art)];
    const retail_ui_art::Image* image = skinImages_ && (*skinImages_)[static_cast<size_t>(art)] ? &*(*skinImages_)[static_cast<size_t>(art)] : nullptr;
    if (!slot.texture && image && env_.immediateDrawer)
    {
        // The drawer returns null until ready.
        if (auto* drawer = env_.immediateDrawer())
            slot.texture = drawer->CreateTexture(image->width, image->height, rex::ui::ImmediateTextureFilter::kLinear, false, image->rgba.data());
        if (slot.texture)
        {
            slot.width  = image->width;
            slot.height = image->height;
        }
    }
    return slot.texture.get();
}

bool OptionsPage::drawSkinImage(options_skin::Art art, ImVec2 min, ImVec2 max, ImVec2 uv0, ImVec2 uv1)
{
    auto* texture = skinTexture(art);
    if (!texture)
        return false;
    ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(texture), min, max, uv0, uv1);
    return true;
}

bool OptionsPage::drawSkinArrow(ImVec2 center, float size, int turns, float alpha)
{
    auto* texture = skinTexture(options_skin::Art::kGoldArrow);
    if (!texture)
        return false;
    // The image points right; each turn is a quarter clockwise on screen.
    const float  angle      = static_cast<float>(turns) * 1.57079633f;
    const float  c          = std::cos(angle);
    const float  s          = std::sin(angle);
    const float  h          = size * 0.5f;
    const ImVec2 offsets[4] = { ImVec2(-h, -h), ImVec2(h, -h), ImVec2(h, h), ImVec2(-h, h) };
    ImVec2       p[4];
    for (int i = 0; i < 4; ++i)
        p[i] = ImVec2(center.x + offsets[i].x * c - offsets[i].y * s, center.y + offsets[i].x * s + offsets[i].y * c);
    ImGui::GetWindowDrawList()->AddImageQuad(reinterpret_cast<ImTextureID>(texture), p[0], p[1], p[2], p[3], ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, alpha)));
    return true;
}

void OptionsPage::drawBackdrop(const Metrics& m)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), m.size, color(kNavyDeep, 0.97f));
    if (auto* texture = skinTexture(options_skin::Art::kMap))
    {
        const Glyph& map = skin_[static_cast<size_t>(options_skin::Art::kMap)];
        const auto   uv  = options_skin::MapCrop(map.width, map.height, m.size.x, m.size.y);
        dl->AddImage(reinterpret_cast<ImTextureID>(texture), ImVec2(0.0f, 0.0f), m.size, ImVec2(uv.u0, uv.v0), ImVec2(uv.u1, uv.v1));
        dl->AddRectFilled(ImVec2(0.0f, 0.0f), m.size, color(kMapTint));
    }
}

void OptionsPage::drawBand(const Metrics& m)
{
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImVec2 min(0.0f, m.banner);
    const ImVec2 max(m.size.x, m.top);
    if (!drawSkinImage(options_skin::Art::kBar, min, max))
        dl->AddRectFilled(min, max, color(kBarFallback));
    dl->AddLine(ImVec2(min.x, min.y), ImVec2(max.x, min.y), color(kBorderOuter, 0.8f));
    dl->AddLine(ImVec2(min.x, max.y), ImVec2(max.x, max.y), color(kBorderOuter, 0.8f));
}

void OptionsPage::drawFrame(const Metrics& m)
{
    ImDrawList*  dl     = ImGui::GetWindowDrawList();
    const ImVec2 min    = frameMin(m);
    const ImVec2 max    = frameMax(m);
    const float  stripY = min.y + m.titleStrip;
    dl->AddRectFilledMultiColor(min, max, color(kBodyTop), color(kBodyTop), color(kBodyBottom), color(kBodyBottom));
    if (!drawSkinImage(options_skin::Art::kBar, min, ImVec2(max.x, stripY)))
        dl->AddRectFilled(min, ImVec2(max.x, stripY), color(kBarFallback));
    dl->AddLine(ImVec2(min.x, stripY), ImVec2(max.x, stripY), color(kBorderOuter), 1.0f);
    dl->AddRect(min, max, color(kBorderOuter), 0.0f, 0, 2.0f);
    dl->AddRect(ImVec2(min.x + 2.0f, min.y + 2.0f), ImVec2(max.x - 2.0f, max.y - 2.0f), color(kBorderInner), 0.0f, 0, 1.0f);
}

bool OptionsPage::beginBodyChild(const char* id, const Metrics& m)
{
    const ImVec2 min = frameMin(m);
    const ImVec2 max = frameMax(m);
    const ImVec2 start(min.x + kFrameBorder, min.y + m.titleStrip + 1.0f);
    const ImVec2 size(std::max(1.0f, max.x - min.x - 2.0f * kFrameBorder - kGutterEm * m.em), std::max(1.0f, max.y - start.y - kFrameBorder));
    ImGui::SetCursorScreenPos(start);
    if (scrollTo_ >= 0.0f && scrollPage_ == pages_.Current() && scrollTab_ == currentTab())
        ImGui::SetNextWindowScroll(ImVec2(-1.0f, scrollTo_));
    scrollTo_ = -1.0f;
    return ImGui::BeginChild(id, size, 0, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar);
}

void OptionsPage::drawScrollArrows(const Metrics& m, float scrollY, float scrollMax, float rowHeight)
{
    if (stickScroll_ != 0)
    {
        const float next = options_skin::StepScroll(scrollY, scrollMax, rowHeight, stickScroll_);
        stickScroll_     = 0;
        if (next != scrollY)
        {
            scrollTo_   = next;
            scrollPage_ = pages_.Current();
            scrollTab_  = currentTab();
        }
    }
    const auto arrows = options_skin::ArrowsFor(scrollY, scrollMax);
    if (!arrows.up && !arrows.down)
        return;
    ImDrawList*  dl      = ImGui::GetWindowDrawList();
    const ImVec2 min     = frameMin(m);
    const ImVec2 max     = frameMax(m);
    const float  ls      = kScrollLsEm * m.em;
    const float  arrow   = kScrollArrowEm * m.em;
    const float  centerX = std::floor(max.x - kFrameBorder - 0.5f * m.em - ls * 0.5f);
    const float  upLsY   = min.y + 1.3f * m.em;
    const float  downLsY = max.y - 1.3f * m.em;
    const bool   hasLs   = skinTexture(options_skin::Art::kLs) != nullptr;
    const auto   one     = [&](bool up)
    {
        const float  lsY    = up ? upLsY : downLsY;
        const float  arrowY = lsY + (up ? -1.0f : 1.0f) * (ls * 0.5f - 0.1f * m.em);
        const ImVec2 hitMin(centerX - ls * 0.5f, std::min(lsY, arrowY) - arrow * 0.5f);
        const ImVec2 hitMax(centerX + ls * 0.5f, std::max(lsY, arrowY) + arrow * 0.5f);
        ImGui::SetCursorScreenPos(hitMin);
        ImGui::PushID(static_cast<int>(pages_.Current()));
        ImGui::PushID(static_cast<int>(currentTab()));
        const bool clicked = ImGui::InvisibleButton(up ? "##scroll_up" : "##scroll_down", ImVec2(hitMax.x - hitMin.x, hitMax.y - hitMin.y), ImGuiButtonFlags_MouseButtonLeft);
        const bool lit     = ImGui::IsItemHovered() || ImGui::IsItemActive();
        ImGui::PopID();
        ImGui::PopID();
        if (hasLs)
            drawSkinImage(options_skin::Art::kLs, ImVec2(centerX - ls * 0.5f, lsY - ls * 0.5f), ImVec2(centerX + ls * 0.5f, lsY + ls * 0.5f));
        if (lit)
            dl->AddCircleFilled(ImVec2(centerX, arrowY), arrow * 0.7f, color(kHighlight, 0.5f), 16);
        if (!drawSkinArrow(ImVec2(centerX, arrowY), arrow, up ? 3 : 1))
            drawVerticalArrow(dl, ImVec2(centerX, arrowY), arrow * 0.5f, up, 1.0f);
        return clicked;
    };
    float target = -1.0f;
    if (arrows.up && one(true))
        target = std::max(0.0f, scrollY - rowHeight);
    if (arrows.down && one(false))
        target = std::min(scrollMax, scrollY + rowHeight);
    if (target >= 0.0f)
    {
        scrollTo_   = target;
        scrollPage_ = pages_.Current();
        scrollTab_  = currentTab();
    }
}

OptionsPage::HintSize OptionsPage::hintSize(int which, float height)
{
    if (glyphTexture(which))
    {
        const Glyph& glyph = glyphs_[which];
        const float  width = options_glyphs::WidthForHeight(glyph.width, glyph.height, height);
        if (width > 0.0f && height > 0.0f)
            return { width, height };
    }
    const float cap = height * kHintCapShare;
    return { pillWidth(cap, kHintLabels[family_][which - kHintLB]), cap };
}

void OptionsPage::drawHint(int which, ImVec2 min, HintSize size)
{
    if (auto* texture = glyphTexture(which); texture && glyphs_[which].width > 0)
    {
        ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(texture), min, ImVec2(min.x + size.width, min.y + size.height));
        return;
    }
    drawPill(ImGui::GetWindowDrawList(), min, size.height, kHintLabels[family_][which - kHintLB]);
}

int OptionsPage::drawStrip(const std::vector<std::string>& titles, size_t current, float top, int leftHint, int rightHint, const Metrics& m, int& step, int idScope)
{
    step = 0;
    ImGui::PushID(idScope);
    const rex::ui::Style& style  = imgui_drawer()->style();
    ImDrawList*           dl     = ImGui::GetWindowDrawList();
    const float           height = m.strip;
    // Triggers are full height (1:2), shoulders smaller (2:1), for equal visual weight.
    const auto promptHeight = [&](int hint)
    {
        return hint == kHintLB || hint == kHintRB ? options_skin::ShoulderHintHeight(m.em, height) : options_skin::HintHeight(m.em, height);
    };
    const auto sizeL = hintSize(leftHint, promptHeight(leftHint));
    const auto sizeR = hintSize(rightHint, promptHeight(rightHint));

    const float hintL        = sizeL.width;
    const float hintR        = sizeR.width;
    const auto  promptButton = [&](int hint, float x, HintSize size)
    {
        const ImVec2 min(x, std::floor(top + (height - size.height) * 0.5f));
        const ImVec2 max(min.x + size.width, min.y + size.height);
        ImGui::PushID(hint);
        ImGui::SetCursorScreenPos(min);
        const bool clicked = ImGui::InvisibleButton("##prompt", ImVec2(size.width, size.height), ImGuiButtonFlags_MouseButtonLeft);
        const bool hovered = ImGui::IsItemHovered();
        const bool held    = ImGui::IsItemActive();
        ImGui::PopID();
        if (hovered || held)
        {
            const float grow = 0.15f * m.em;
            dl->AddRectFilled(ImVec2(min.x - grow, min.y - grow), ImVec2(max.x + grow, max.y + grow), color(held ? kGlow : kHighlight, held ? 0.55f : 0.4f), 0.3f * m.em);
        }
        drawHint(hint, min, size);
        return clicked;
    };
    if (promptButton(leftHint, m.left, sizeL))
        step = -1;
    if (promptButton(rightHint, m.left + m.column - hintR, sizeR))
        step = 1;
    // Wheel switches tabs unless ImGui's wheel lock belongs to another window.
    {
        const ImVec2        min(m.left, top);
        const ImVec2        max(m.left + m.column, top + height);
        const float         wheel    = ImGui::GetIO().MouseWheel;
        const ImGuiContext* ctx      = ImGui::GetCurrentContext();
        const ImGuiWindow*  wheeling = ctx->WheelingWindow;
        const bool          ours     = wheeling == nullptr || wheeling == ctx->CurrentWindow;
        if (wheel != 0.0f && ours && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(min, max))
            step = wheel > 0.0f ? -1 : 1;
    }
    const float        available = m.column - hintL - hintR - 2.0f * kHintGapEm * m.em;
    const float        spanLeft  = m.left + hintL + kHintGapEm * m.em;
    std::vector<float> widths;
    widths.reserve(titles.size());
    for (const auto& title : titles)
        widths.push_back(ImGui::CalcTextSize(title.c_str()).x);
    const auto  fit   = tab_bar_layout::FitTabBar(widths, available, 0.25f * m.em, kTabPadEm * m.em, kMinTabScale, current);
    const float pad   = fit.pad;
    ImFont*     font  = ImGui::GetFont();
    const float size  = ImGui::GetFontSize() * fit.scale;
    float       x     = spanLeft + fit.offset;
    const float clipL = spanLeft;
    const float clipR = spanLeft + available;

    int result = -1;
    dl->PushClipRect(ImVec2(clipL, top), ImVec2(clipR, top + height), true);
    for (size_t i = 0; i < titles.size(); ++i)
    {
        const float  textW = widths[i] * fit.scale;
        const float  textH = ImGui::GetTextLineHeight() * fit.scale;
        const ImVec2 min(x, top + 0.2f * m.em);
        const ImVec2 max(x + textW + 2.0f * pad, top + height - 0.2f * m.em);
        x = max.x;
        if (max.x <= clipL || min.x >= clipR)
            continue;
        const ImVec2 hitMin(std::max(min.x, clipL), min.y);
        const ImVec2 hitMax(std::min(max.x, clipR), max.y);
        ImGui::PushID(leftHint);
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetCursorScreenPos(hitMin);
        const bool clicked = ImGui::InvisibleButton("##strip", ImVec2(hitMax.x - hitMin.x, hitMax.y - hitMin.y), ImGuiButtonFlags_MouseButtonLeft);
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        ImGui::PopID();
        const bool selected = i == current;
        if (selected)
            drawBar(style, dl, min, max, rex::ui::ChromeState::kSelected);
        const ImU32 col = selected || hovered ? color(rgba(255, 255, 255)) : color(kValue, 0.8f);
        dl->AddText(font, size, ImVec2(min.x + pad, min.y + (max.y - min.y - textH) * 0.5f), col, titles[i].c_str());
        if (clicked)
            result = static_cast<int>(i);
    }
    dl->PopClipRect();
    ImGui::PopID();
    return result;
}

void OptionsPage::footerLines(float wrap, int& tipLines, int& noteLines)
{
    using title_text::Id;
    tipLines          = 0;
    noteLines         = 0;
    const float lineH = ImGui::GetTextLineHeight();
    const auto  lines = [&](const std::string& text)
    {
        if (text.empty() || lineH <= 0.0f)
            return 0;
        return std::max(1, static_cast<int>(std::ceil(ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap).y / lineH - 0.01f)));
    };
    constexpr int kMaxTipLines = 3;
    switch (currentKind())
    {
        case PageKind::kOptions:
        {
            const auto&  sections = model_.Sections();
            const size_t tab      = model_.Tab();
            if (tab < sections.size())
            {
                for (const auto& row : sections[tab].rows)
                    tipLines = std::max(tipLines, lines(row.description));
            }
            tipLines  = std::clamp(tipLines, 1, kMaxTipLines);
            noteLines = 1;
            break;
        }
        case PageKind::kMods:
        {
            tipLines  = std::max(tipLines, lines(modsTabText(Id::kModsDiscardAsk, Id::kAssetsDiscardAsk, Id::kMapsDiscardAsk)));
            tipLines  = std::max(tipLines, lines(modsTabText(Id::kModsReplaceAsk, Id::kAssetsReplaceAsk, Id::kMapsReplaceAsk)));
            tipLines  = std::clamp(tipLines, 1, kMaxTipLines);
            noteLines = 1;
            break;
        }
        case PageKind::kAchievements:
            break;
    }
}

void OptionsPage::drawLegend(const Metrics& m)
{
    using title_text::Id;
    ImDrawList* dl  = ImGui::GetWindowDrawList();
    const float top = m.size.y - m.footer;
    dl->AddRectFilled(ImVec2(0.0f, top), m.size, color(kNavyDeep, 0.98f));
    dl->AddLine(ImVec2(0.0f, top), ImVec2(m.size.x, top), color(kEdge), 2.0f);

    const float textLeft = m.left + m.em;
    const float wrap     = m.column - 2.0f * m.em;
    ImFont*     font     = ImGui::GetFont();
    const float size     = ImGui::GetFontSize();
    float       y        = top + options_skin::kFooterPadEm * m.em;
    const auto  kind     = currentKind();
    const bool  options  = kind == PageKind::kOptions;
    const bool  mods     = kind == PageKind::kMods;
    auto&       model    = activeModel();
    {
        const bool asking = mods && model.Confirmation() != mods_list::Confirm::kNone;
        if (asking && !questionShown_)
            questionFrame_ = ImGui::GetFrameCount();
        questionShown_ = asking;
    }
    const bool  questionFresh = ImGui::GetFrameCount() == questionFrame_;
    std::string tip;
    std::string note;
    if (options)
    {
        tip  = model_.Description();
        note = model_.Note();
        if (!clippedBind_.empty())
            tip = clippedBind_ + "\n" + tip;
    }
    else if (mods)
    {
        tip  = modsTip();
        note = modsNote();
    }
    const float buttonH = options_skin::kFooterButtonEm * m.em;
    const float buttonW = 9.0f * m.em;
    const float rowTop  = m.size.y - buttonH - options_skin::kFooterPadEm * m.em;
    {
        const float lineH  = ImGui::GetTextLineHeight();
        const float limit  = rowTop - options_skin::kFooterButtonGap * m.em;
        const float avail  = std::max(0.0f, limit - y);
        const float gap    = options_skin::kFooterTextGap * m.em;
        const auto  height = [&](const std::string& text)
        {
            return text.empty() ? 0.0f : ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap).y;
        };
        const auto fitLines = [&](float wanted, float room)
        {
            return wanted <= room ? wanted : std::floor(std::max(0.0f, room) / lineH) * lineH;
        };
        const float noteH = fitLines(height(note), avail * 0.5f);
        const float tipH  = fitLines(height(tip), avail - noteH - (noteH > 0.0f ? gap : 0.0f));
        const float noteY = y + tipH + (tipH > 0.0f ? gap : 0.0f);
        const auto  block = [&](const std::string& text, float at, float h, ImU32 col)
        {
            if (text.empty() || h <= 0.0f)
                return;
            dl->PushClipRect(ImVec2(0.0f, at), ImVec2(m.size.x, at + h), true);
            dl->AddText(font, size, ImVec2(textLeft, at), col, text.c_str(), nullptr, wrap);
            dl->PopClipRect();
        };
        block(tip, y, tipH, color(rgba(255, 255, 255)));
        block(note, noteY, noteH, color(kArrow));
    }

    float       x         = m.em * 1.5f;
    std::string back      = pageText(Id::kLegendBack);
    const float gap       = 0.6f * m.em;
    const float backW     = std::min(buttonW, 0.4f * m.size.x);
    const float backX     = m.size.x - backW - m.em * 1.5f;
    int         actions   = 1;
    const auto* footerRow = options && !model_.ModalOpen() ? model_.Row(model_.Highlight()) : nullptr;
    const bool  copyPaste = footerRow && footerRow->HasCopyPaste();
    // Pasting rebuilds the rows, so what the Copy button needs is read first.
    const bool canCopy  = footerRow && footerRow->CanCopy();
    const bool bindRow  = footerRow && footerRow->HasBindButtons();
    const bool canClear = footerRow && footerRow->CanClearBind();
    const bool canReset = footerRow && footerRow->CanResetBind();
    if (copyPaste || bindRow)
        actions = 3;
    if (mods && model.Confirmation() == mods_list::Confirm::kNone && !model.Grabbing())
    {
        const auto* current = model.Current();
        actions             = (current && !model.Locked() ? 1 + (current->kind == mods_list::RowKind::kEnabled ? 1 : 0) : 0) + (model.CanApply() ? 1 : 0);
    }
    const float actionW = options_skin::ActionWidth(actions, backX - gap - x, gap, buttonW);
    const float step    = actionW + gap;
    if (options)
    {
        if (legendButton("##legend_a", kGlyphA, pageText(Id::kLegendSelect).c_str(), ImVec2(x, rowTop), m, true, actionW))
            activate();
        if (copyPaste)
        {
            // Page/tab/action IDs keep held presses from becoming Paste/Copy.
            ImGui::PushID(static_cast<int>(pages_.Current()));
            ImGui::PushID(static_cast<int>(currentTab()));
            x += step;
            const std::string paste = rowButtonCaption(keyboardFamily(), kChordPaste, pageText(Id::kKeyPaste));
            const std::string copy  = rowButtonCaption(keyboardFamily(), kChordCopy, pageText(Id::kButtonCopy));
            const int         padX  = keyboardFamily() ? kGlyphNone : kGlyphX;
            const int         padY  = keyboardFamily() ? kGlyphNone : kGlyphY;
            if (legendButton("##options_paste", padX, paste.c_str(), ImVec2(x, rowTop), m, true, actionW))
                pasteInvite();
            x += step;
            if (legendButton("##options_copy", padY, copy.c_str(), ImVec2(x, rowTop), m, canCopy, actionW, true))
                copyInvite();
            ImGui::PopID();
            ImGui::PopID();
        }
        else if (bindRow)
        {
            ImGui::PushID(static_cast<int>(pages_.Current()));
            ImGui::PushID(static_cast<int>(currentTab()));
            x += step;
            const std::string clear = rowButtonCaption(keyboardFamily(), kChordClear, pageText(Id::kKeyClear));
            const std::string reset = rowButtonCaption(keyboardFamily(), kChordReset, pageText(Id::kLegendReset));
            const int         padX  = keyboardFamily() ? kGlyphNone : kGlyphX;
            const int         padY  = keyboardFamily() ? kGlyphNone : kGlyphY;
            if (legendButton("##options_clear", padX, clear.c_str(), ImVec2(x, rowTop), m, canClear, actionW, true))
                clearBind();
            x += step;
            if (legendButton("##options_reset", padY, reset.c_str(), ImVec2(x, rowTop), m, canReset, actionW, true))
                resetBind();
            ImGui::PopID();
            ImGui::PopID();
        }
    }
    else if (mods)
    {
        const auto* row = model.Current();
        switch (model.Confirmation())
        {
            case mods_list::Confirm::kDiscard:
            case mods_list::Confirm::kReplace:
            {
                const bool        discard = model.Confirmation() == mods_list::Confirm::kDiscard;
                const std::string yes     = pageText(discard ? Id::kLegendDiscard : Id::kLegendReplace);
                if (legendButton("##question_yes", kGlyphA, yes.c_str(), ImVec2(x, rowTop), m, !questionFresh, actionW))
                    acceptMods();
                back = pageText(Id::kLegendKeep);
                break;
            }
            case mods_list::Confirm::kNone:
                if (model.Grabbing())
                {
                    const std::string drop = pageText(Id::kLegendDrop);
                    if (legendButton("##legend_a", kGlyphA, drop.c_str(), ImVec2(x, rowTop), m, true, actionW) && takeRowAction())
                        model.Drop();
                    back = pageText(Id::kLegendCancel);
                    break;
                }
                if (row && !model.Locked())
                {
                    // An action rebuilds the row; copy what is needed first.
                    const mods_list::RowKind kind   = row->kind;
                    const std::string        toggle = pageText(kind == mods_list::RowKind::kDisabled ? Id::kLegendEnable : kind == mods_list::RowKind::kEnabled ? Id::kLegendDisable
                                                                                                                                                                : Id::kLegendRemove);
                    if (legendButton("##legend_a", kGlyphA, toggle.c_str(), ImVec2(x, rowTop), m, true, actionW))
                        activateMod();
                    x += step;
                    if (kind == mods_list::RowKind::kEnabled)
                    {
                        const std::string move = pageText(Id::kLegendMove);
                        if (legendButton("##legend_x", kGlyphX, move.c_str(), ImVec2(x, rowTop), m, true, actionW))
                            grabMod();
                        x += step;
                    }
                }
                if (model.CanApply())
                {
                    const std::string apply = pageText(Id::kLegendApply);
                    if (legendButton("##legend_y", kGlyphY, apply.c_str(), ImVec2(x, rowTop), m, true, actionW))
                        applyMods();
                }
                break;
        }
    }
    const bool questionOpen = mods && model.Confirmation() != mods_list::Confirm::kNone;
    if (legendButton(questionOpen ? "##question_no" : "##legend_b", kGlyphB, back.c_str(), ImVec2(backX, rowTop), m, !(questionOpen && questionFresh), backW))
    {
        if (options && model_.PickerOpen())
            model_.ClosePicker();
        else if (options && model_.Capturing())
        {
            takeRowAction();
            model_.CancelCapture();
        }
        else
            onBack();
    }
}

void OptionsPage::drawButtonFace(const char* caption, int glyph, ImVec2 min, ImVec2 size, rex::ui::ChromeState state, bool dim)
{
    const rex::ui::Style& style  = imgui_drawer()->style();
    ImDrawList*           dl     = ImGui::GetWindowDrawList();
    const float           height = size.y;
    const ImVec2          max(min.x + size.x, min.y + size.y);
    if (style.chrome.draw_button)
        style.chrome.draw_button(dl, min, max, state);
    else
        dl->AddRectFilled(min, max, color(state == rex::ui::ChromeState::kNormal ? kNavyMid : kHighlight));
    const ImVec2 text = ImGui::CalcTextSize(caption);
    if (glyph == kGlyphNone)
    {
        dl->PushClipRect(min, max, true);
        dl->AddText(ImVec2(min.x + std::max(height * 0.2f, (size.x - text.x) * 0.5f), min.y + (height - text.y) * 0.5f), color(rgba(255, 255, 255)), caption);
        dl->PopClipRect();
        if (dim)
            dl->AddRectFilled(min, max, color(kNavyDeep, 0.6f));
        return;
    }
    const float glyphSize   = height * 0.8f;
    const float glyphWide   = glyphWidth(glyph, glyphSize, glyphSize);
    const bool  showCaption = size.x >= height * 0.45f + glyphWide + text.x;
    const float glyphX      = showCaption ? min.x + height * 0.15f : min.x + (size.x - glyphWide) * 0.5f;
    drawGlyph(glyph, ImVec2(glyphX, min.y + (height - glyphSize) * 0.5f), glyphSize);
    if (showCaption)
        dl->AddText(ImVec2(min.x + height * 0.3f + glyphWide, min.y + (height - text.y) * 0.5f), color(rgba(255, 255, 255)), caption);
    // The art ignores the style alpha, so a disabled button is veiled instead.
    if (dim)
        dl->AddRectFilled(min, max, color(kNavyDeep, 0.6f));
}

// veil dims only permanently disabled buttons (empty Copy).
bool OptionsPage::legendButton(const char* id, int glyph, const char* caption, ImVec2 min, const Metrics& m, bool enabled, float width, bool veil)
{
    const float  height = options_skin::kFooterButtonEm * m.em;
    const ImVec2 size(width > 0.0f ? width : 9.0f * m.em, height);
    ImGui::SetCursorScreenPos(min);
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_MouseButtonLeft);
    ImGui::EndDisabled();
    const bool hovered = ImGui::IsItemHovered();
    const bool held    = ImGui::IsItemActive();
    const auto state   = held ? rex::ui::ChromeState::kActive : hovered ? rex::ui::ChromeState::kHovered
                                                                        : rex::ui::ChromeState::kNormal;
    drawButtonFace(caption, glyph, min, size, state, veil && !enabled);
    return clicked;
}

namespace
{

using GlyphImages = std::array<std::optional<retail_ui_art::Image>, options_glyphs::kButtonCount>;

// Decoded per process; failures are logged once.
std::shared_ptr<const GlyphImages> decodeGlyphs(button_icons::Family family, const std::filesystem::path& root, const std::filesystem::path& keys)
{
    static std::mutex                                                mutex;
    static std::map<std::string, std::shared_ptr<const GlyphImages>> cache;
    const std::string                                                key = std::to_string(static_cast<int>(family)) + '|' + root.string() + '|' + keys.string();
    std::lock_guard                                                  lock(mutex);
    if (const auto it = cache.find(key); it != cache.end())
        return it->second;

    auto                          images = std::make_shared<GlyphImages>();
    std::vector<int>              retailSlots;
    std::vector<std::string_view> retailNames;
    for (int i = 0; i < options_glyphs::kButtonCount; ++i)
    {
        const auto file = options_glyphs::FileFor(family, static_cast<Button>(i));
        if (file.source == options_glyphs::Source::kRetailFpk)
        {
            retailSlots.push_back(i);
            retailNames.push_back(file.name);
        }
    }
    std::vector<std::optional<std::vector<uint8_t>>> retailBytes;
    std::vector<std::string>                         retailErrors;
    std::string                                      archiveError = "no folder";
    bool                                             archiveOk    = false;
    if (!retailNames.empty() && !root.empty())
        archiveOk = retail_ui_art::ReadFpkEntries(root / "Resource" / "Common" / kFpkName, retailNames, retailBytes, retailErrors, &archiveError);
    for (size_t r = 0; r < retailSlots.size(); ++r)
    {
        std::string error = archiveOk ? retailErrors[r] : archiveError;
        if (archiveOk && retailBytes[r])
            (*images)[retailSlots[r]] = retail_ui_art::DecodeDds(*retailBytes[r], &error);
        if (!(*images)[retailSlots[r]])
            REXLOG_WARN("Options page: {}:{} unavailable ({}); drawing the prompt", kFpkName, retailNames[r], error);
    }
    for (int i = 0; i < options_glyphs::kButtonCount; ++i)
    {
        const auto file = options_glyphs::FileFor(family, static_cast<Button>(i));
        if (file.source != options_glyphs::Source::kKeyboardArt)
            continue;
        const std::string                   name(file.name);
        std::string                         error = "no folder";
        std::optional<std::vector<uint8_t>> bytes;
        if (!keys.empty())
            bytes = readFile(keys / name, &error);
        if (bytes)
            (*images)[i] = retail_ui_art::DecodeDds(*bytes, &error);
        if (!(*images)[i])
            REXLOG_WARN("Options page: {} unavailable ({}); drawing the prompt", (keys / name).string(), error);
    }
    cache.emplace(key, images);
    return images;
}

using SkinImages = std::array<std::optional<retail_ui_art::Image>, options_skin::kArtCount>;

// Decoded per process; missing art is logged once and substitutes are drawn.
std::shared_ptr<const SkinImages> decodeSkin(button_icons::Family family, const std::filesystem::path& root)
{
    static std::mutex                                               mutex;
    static std::map<std::string, std::shared_ptr<const SkinImages>> cache;
    const std::string                                               key = std::to_string(static_cast<int>(family)) + '|' + root.string();
    std::lock_guard                                                 lock(mutex);
    if (const auto it = cache.find(key); it != cache.end())
        return it->second;

    auto images = std::make_shared<SkinImages>();
    for (const std::string_view archive : { std::string_view("Common0.FPK"), std::string_view("Pregame.FPK") })
    {
        std::vector<int>              slots;
        std::vector<std::string_view> names;
        for (int i = 0; i < options_skin::kArtCount; ++i)
        {
            const auto file = options_skin::FileFor(family, static_cast<options_skin::Art>(i));
            if (file && file->archive == archive)
            {
                slots.push_back(i);
                names.push_back(file->name);
            }
        }
        if (names.empty())
            continue;
        std::vector<std::optional<std::vector<uint8_t>>> bytes;
        std::vector<std::string>                         errors;
        std::string                                      archiveError = "no folder";
        bool                                             archiveOk    = false;
        if (!root.empty())
            archiveOk = retail_ui_art::ReadFpkEntries(root / "Resource" / "Common" / std::string(archive), names, bytes, errors, &archiveError);
        for (size_t r = 0; r < slots.size(); ++r)
        {
            std::string error = archiveOk ? errors[r] : archiveError;
            if (archiveOk && bytes[r])
                (*images)[slots[r]] = retail_ui_art::DecodeDds(*bytes[r], &error);
            if (!(*images)[slots[r]])
                REXLOG_WARN("Options page: {}:{} unavailable ({}); drawing the plain look", archive, names[r], error);
        }
    }
    cache.emplace(key, images);
    return images;
}

} // namespace

void OptionsPage::loadGlyphs()
{
    if (glyphsLoaded_)
        return;
    glyphsLoaded_                    = true;
    const auto family                = button_icons::ActiveFamily();
    family_                          = static_cast<int>(family);
    const std::filesystem::path root = env_.gameRoot ? env_.gameRoot() : std::filesystem::path();
    const std::filesystem::path keys = env_.keyboardArtDir ? env_.keyboardArtDir() : std::filesystem::path();
    images_                          = decodeGlyphs(family, root, keys);
    skinImages_                      = decodeSkin(family, root);
    for (int i = 0; i < options_glyphs::kButtonCount; ++i)
    {
        if ((*images_)[i])
        {
            glyphs_[i].width  = (*images_)[i]->width;
            glyphs_[i].height = (*images_)[i]->height;
        }
    }
}

const OptionsPage::KeyArt* OptionsPage::bindArt(const OptionRow& row)
{
    // Keypad Enter aliases Return, the available cap.
    std::string name = row.bindValue;
    if (name.empty() || name.find_first_of(",+") != std::string::npos)
        return nullptr;
    if (name == "NumpadEnter")
        name = "Return";
    KeyArt& art = keyArt_[name];
    if (!art.tried)
    {
        art.tried                        = true;
        const std::filesystem::path keys = env_.keyboardArtDir ? env_.keyboardArtDir() : std::filesystem::path();
        std::string                 error;
        if (!keys.empty())
        {
            if (const auto bytes = readFile(keys / (name + ".dds"), &error))
            {
                if (auto image = retail_ui_art::DecodeDds(*bytes, &error))
                {
                    art.width  = image->width;
                    art.height = image->height;
                    art.rgba   = std::move(image->rgba);
                }
            }
        }
    }
    if (art.rgba.empty())
        return nullptr;
    if (!art.texture && env_.immediateDrawer)
    {
        if (auto* drawer = env_.immediateDrawer())
            art.texture = drawer->CreateTexture(art.width, art.height, rex::ui::ImmediateTextureFilter::kLinear, false, art.rgba.data());
    }
    return art.texture ? &art : nullptr;
}

rex::ui::ImmediateTexture* OptionsPage::glyphTexture(int which)
{
    loadGlyphs();
    Glyph&      glyph = glyphs_[which];
    const auto& image = (*images_)[which];
    if (!glyph.texture && image && env_.immediateDrawer)
    {
        // The drawer returns null until ready; pixels outlive the page, textures do not.
        if (auto* drawer = env_.immediateDrawer())
            glyph.texture = drawer->CreateTexture(image->width, image->height, rex::ui::ImmediateTextureFilter::kLinear, false, image->rgba.data());
    }
    return glyph.texture.get();
}

float OptionsPage::glyphWidth(int which, float height, float fallbackWidth)
{
    const Glyph& glyph = glyphs_[which];
    const bool   art   = glyphTexture(which) != nullptr;
    return options_glyphs::PromptWidth(art ? glyph.width : 0, art ? glyph.height : 0, height, fallbackWidth);
}

bool OptionsPage::drawArt(int which, ImVec2 min, float height)
{
    auto* texture = glyphTexture(which);
    if (!texture)
        return false;
    const Glyph& glyph = glyphs_[which];
    const float  width = options_glyphs::WidthForHeight(glyph.width, glyph.height, height);
    ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(texture), min, ImVec2(min.x + width, min.y + height));
    return true;
}

float OptionsPage::drawGlyph(int which, ImVec2 min, float size)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (drawArt(which, min, size))
        return glyphWidth(which, size, size);
    const auto family = static_cast<button_icons::Family>(family_);
    const bool a      = which == kGlyphA;
    const bool b      = which == kGlyphB;
    const bool x      = which == kGlyphX;
    if (family == button_icons::Family::kKeyboard)
    {
        const char*  label = a ? "Enter" : b ? "Esc"
                                       : x   ? "X"
                                             : "Y";
        const ImVec2 text  = ImGui::CalcTextSize(label);
        const float  width = std::max(size, text.x + size * 0.4f);
        dl->AddRectFilled(min, ImVec2(min.x + width, min.y + size), color(rgba(230, 232, 238)), size * 0.2f);
        dl->AddRect(min, ImVec2(min.x + width, min.y + size), color(rgba(90, 96, 110)), size * 0.2f, 0, 2.0f);
        dl->AddText(ImVec2(min.x + (width - text.x) * 0.5f, min.y + (size - text.y) * 0.5f), color(rgba(30, 32, 40)), label);
        return width;
    }
    const bool  ps = family == button_icons::Family::kPlayStation;
    const char* label;
    ImVec4      fill;
    if (a)
    {
        label = ps ? "X" : "A";
        fill  = ps ? rgba(70, 110, 200) : rgba(60, 160, 60);
    }
    else if (b)
    {
        label = ps ? "O" : "B";
        fill  = ps ? rgba(200, 60, 70) : rgba(200, 50, 50);
    }
    else if (x)
    {
        label = ps ? "Sq" : "X";
        fill  = ps ? rgba(190, 100, 170) : rgba(50, 100, 200);
    }
    else
    {
        label = ps ? "Tr" : "Y";
        fill  = ps ? rgba(60, 170, 140) : rgba(220, 180, 40);
    }
    const ImVec2 center(min.x + size * 0.5f, min.y + size * 0.5f);
    dl->AddCircleFilled(center, size * 0.5f, color(fill), 24);
    dl->AddCircle(center, size * 0.5f, color(rgba(0, 0, 0)), 24, 1.5f);
    const ImVec2 text = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(center.x - text.x * 0.5f, center.y - text.y * 0.5f), color(rgba(255, 255, 255)), label);
    return size;
}

void OptionsPage::drawPicker(ImGuiIO& io)
{
    const rex::ui::Style& style = imgui_drawer()->style();
    const float           base  = ImGui::GetStyle().FontSizeBase;
    const OptionRow*      row   = model_.Row(model_.Highlight());
    if (!row)
        return;
    // Committing rebuilds the model, so what the walk needs is copied first.
    const std::string                          label        = row->label;
    const std::vector<native_settings::Choice> choices      = row->choices;
    const size_t                               currentIndex = row->current;

    ImGui::PushFont(nullptr, base * kBodyScale);
    const float  em     = ImGui::GetFontSize();
    const ImVec2 size   = io.DisplaySize;
    const float  rowH   = kRowEm * em;
    const float  titleH = 2.6f * em;
    const float  panelW = std::min(kPickerWidth * em, size.x - 2.0f * em);
    const float  wanted = titleH + rowH * static_cast<float>(choices.size()) + em;
    const float  panelH = std::min(wanted, size.y * 0.7f);
    const ImVec2 pMin(std::floor((size.x - panelW) * 0.5f), std::floor((size.y - panelH) * 0.5f));
    const ImVec2 pMax(pMin.x + panelW, pMin.y + panelH);

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##rerevved_options_picker", nullptr, flags))
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(0.0f, 0.0f), size, color(rgba(0, 0, 0), 0.6f));
        // A click outside the list closes it; the click that opened it does not.
        ImGui::SetCursorScreenPos(ImVec2(0.0f, 0.0f));
        ImGui::InvisibleButton("##picker_backdrop", size, ImGuiButtonFlags_MouseButtonLeft);
        const ImVec2 mouse   = ImGui::GetMousePos();
        const bool   inside  = mouse.x >= pMin.x && mouse.x < pMax.x && mouse.y >= pMin.y && mouse.y < pMax.y;
        const bool   outside = ImGui::IsItemClicked(ImGuiMouseButton_Left) && !inside && ImGui::GetFrameCount() != pickerFrame_;

        if (style.chrome.draw_window)
            style.chrome.draw_window(dl, pMin, pMax, 0.0f);
        else
            dl->AddRectFilled(pMin, pMax, color(kPanelFill));
        const float pad = em * 0.8f;
        dl->AddText(ImVec2(pMin.x + pad, pMin.y + (titleH - em) * 0.5f), color(kGlow), label.c_str());
        dl->AddLine(ImVec2(pMin.x + em * 0.6f, pMin.y + titleH), ImVec2(pMax.x - em * 0.6f, pMin.y + titleH), color(kEdge, 0.8f));

        ImGui::SetCursorScreenPos(ImVec2(pMin.x, pMin.y + titleH + 2.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        if (ImGui::BeginChild("##rerevved_options_picker_rows", ImVec2(panelW, panelH - titleH - 2.0f), 0, ImGuiWindowFlags_NoNav))
        {
            ImDrawList*  rows   = ImGui::GetWindowDrawList();
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            for (size_t i = 0; i < choices.size(); ++i)
            {
                const ImVec2 min(origin.x + em * 0.4f, origin.y + rowH * static_cast<float>(i));
                const ImVec2 max(origin.x + panelW - em * 0.4f, min.y + rowH);
                ImGui::PushID(static_cast<int>(i));
                ImGui::SetCursorScreenPos(min);
                ImGui::InvisibleButton("##choice", ImVec2(max.x - min.x, rowH), ImGuiButtonFlags_MouseButtonLeft);
                if (ImGui::IsItemHovered() && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
                    model_.SetPickerCursor(i);
                const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
                if (clicked)
                    model_.SetPickerCursor(i);
                const bool cursor = i == model_.PickerCursor();
                if (cursor)
                    drawBar(style, rows, min, max, rex::ui::ChromeState::kSelected);
                const bool current = i == currentIndex;
                rows->PushClipRect(min, max, true);
                rows->AddText(ImVec2(min.x + pad, min.y + (rowH - em) * 0.5f), current ? color(kArrow) : color(rgba(255, 255, 255)), choices[i].caption.c_str());
                rows->PopClipRect();
                if (cursor && pickerEnsure_)
                {
                    const float winTop = ImGui::GetWindowPos().y;
                    const float winEnd = winTop + ImGui::GetWindowHeight();
                    if (min.y < winTop)
                        ImGui::SetScrollY(ImGui::GetScrollY() - (winTop - min.y));
                    else if (max.y > winEnd)
                        ImGui::SetScrollY(ImGui::GetScrollY() + (max.y - winEnd));
                    pickerEnsure_ = false;
                }
                ImGui::PopID();
                if (clicked)
                {
                    model_.CommitPicker();
                    break;
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rowH * static_cast<float>(choices.size())));
            ImGui::Dummy(ImVec2(1.0f, 1.0f));
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        if (outside)
            model_.ClosePicker();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
}

float OptionsPage::drawPrompt(int glyph, const std::string& caption, ImVec2 min, float size)
{
    ImDrawList*  dl   = ImGui::GetWindowDrawList();
    const float  gap  = size * 0.25f;
    const float  wide = drawGlyph(glyph, min, size);
    const ImVec2 text = ImGui::CalcTextSize(caption.c_str());
    dl->AddText(ImVec2(min.x + wide + gap, min.y + (size - text.y) * 0.5f), color(rgba(255, 255, 255)), caption.c_str());
    return wide + gap + text.x;
}

void OptionsPage::drawTextEntry(ImGuiIO& io)
{
    using title_text::Id;
    const rex::ui::Style& style = imgui_drawer()->style();
    const float           base  = ImGui::GetStyle().FontSizeBase;
    const OptionRow*      row   = model_.TextRow();
    if (!row)
        return;
    const std::string label  = row->label;
    const bool        secret = row->secret;
    const std::string note   = model_.Note();

    ImGui::PushFont(nullptr, base * kBodyScale);
    const float  em   = ImGui::GetFontSize();
    const ImVec2 size = io.DisplaySize;
    Metrics      m;
    m.em = em;

    const auto   family   = static_cast<button_icons::Family>(family_);
    const float  margin   = em * 0.8f;
    const float  panelW   = std::min(kTextWidth * em, size.x - 2.0f * em);
    const float  titleH   = 2.6f * em;
    const float  fieldH   = 2.4f * em;
    const float  noteH    = 1.6f * em;
    const float  footerH  = options_skin::kFooterButtonEm * em;
    const float  fixedH   = titleH + margin + fieldH + noteH + margin + footerH + margin;
    const size_t rowCount = model_.TextKeyboard().RowCount();
    const float  keyH     = std::clamp((size.y * 0.94f - fixedH) / static_cast<float>(rowCount), 1.4f * em, 2.3f * em);
    const float  panelH   = fixedH + keyH * static_cast<float>(rowCount);
    const ImVec2 pMin(std::floor((size.x - panelW) * 0.5f), std::floor((size.y - panelH) * 0.5f));
    const ImVec2 pMax(pMin.x + panelW, pMin.y + panelH);

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##rerevved_options_text", nullptr, flags))
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(0.0f, 0.0f), size, color(rgba(0, 0, 0), 0.6f));
        // Mark text active like InputText so SDL delivers characters.
        if (ImGuiContext* ctx = ImGui::GetCurrentContext())
            ctx->PlatformImeData.WantTextInput = true;

        if (style.chrome.draw_window)
            style.chrome.draw_window(dl, pMin, pMax, 0.0f);
        else
            dl->AddRectFilled(pMin, pMax, color(kPanelFill));
        dl->AddText(ImVec2(pMin.x + margin, pMin.y + (titleH - em) * 0.5f), color(kGlow), label.c_str());
        dl->AddLine(ImVec2(pMin.x + em * 0.6f, pMin.y + titleH), ImVec2(pMax.x - em * 0.6f, pMin.y + titleH), color(kEdge, 0.8f));

        auto&        editor = model_.TextEditor();
        auto&        board  = model_.TextKeyboard();
        const ImVec2 fMin(pMin.x + margin, pMin.y + titleH + margin);
        const ImVec2 fMax(pMax.x - margin, fMin.y + fieldH);
        dl->AddRectFilled(fMin, fMax, color(kNavyDeep, 0.9f), 0.2f * em);
        dl->AddRect(fMin, fMax, color(kEdge), 0.2f * em);
        {
            const std::string shown  = secret ? text_entry::Masked(editor.Text(), 4) : editor.Text();
            const std::string count  = std::to_string(editor.Text().size()) + "/" + std::to_string(editor.GetRules().maxLength);
            const float       countW = ImGui::CalcTextSize(count.c_str()).x;
            const float       textY  = fMin.y + (fieldH - em) * 0.5f;
            dl->PushClipRect(ImVec2(fMin.x, fMin.y), ImVec2(fMax.x - countW - em, fMax.y), true);
            if (shown.empty())
            {
                dl->AddText(ImVec2(fMin.x + em * 0.6f, textY), color(kValue, 0.5f), pageText(Id::kTextHint).c_str());
            }
            else
            {
                const float width = ImGui::CalcTextSize(shown.c_str()).x;
                const float avail = fMax.x - countW - em - fMin.x - em * 1.2f;
                const float x     = fMin.x + em * 0.6f - std::max(0.0f, width - avail);
                if (editor.Selected())
                    dl->AddRectFilled(ImVec2(x - 2.0f, textY), ImVec2(x + width + 2.0f, textY + em), color(kHighlight, 0.6f));
                dl->AddText(ImVec2(x, textY), color(rgba(255, 255, 255)), shown.c_str());
                if (!editor.Selected() && std::fmod(ImGui::GetTime(), 1.0) < 0.5)
                    dl->AddLine(ImVec2(x + width + 1.0f, textY), ImVec2(x + width + 1.0f, textY + em), color(rgba(255, 255, 255)), 2.0f);
            }
            dl->PopClipRect();
            dl->AddText(ImVec2(fMax.x - countW - em * 0.6f, textY), color(kValue, 0.7f), count.c_str());
        }

        if (!note.empty())
            dl->AddText(ImVec2(fMin.x, fMax.y + (noteH - em) * 0.5f), color(kArrow), note.c_str());

        const float     gridTop    = fMax.y + noteH;
        const float     gridW      = fMax.x - fMin.x;
        const float     keyGap     = 0.2f * em;
        const auto      charset    = editor.GetRules().charset;
        bool            pressedKey = false;
        text_entry::Key chosen;
        const auto      layout = board.GetLayout();
        for (size_t r = 0; r < rowCount; ++r)
        {
            const size_t count = text_entry::Keyboard::RowSize(r, layout);
            const float  keyW  = r + 1 < rowCount ? (gridW - keyGap * 9.0f) / 10.0f : (gridW - keyGap * static_cast<float>(count - 1)) / static_cast<float>(count);
            const float  rowW  = keyW * static_cast<float>(count) + keyGap * static_cast<float>(count - 1);
            const float  left  = fMin.x + (gridW - rowW) * 0.5f;
            for (size_t c = 0; c < count; ++c)
            {
                const text_entry::Key key = text_entry::Keyboard::KeyAt(r, c, layout);
                const ImVec2          kMin(left + (keyW + keyGap) * static_cast<float>(c), gridTop + keyH * static_cast<float>(r));
                const ImVec2          kMax(kMin.x + keyW, kMin.y + keyH - keyGap);
                ImGui::PushID(static_cast<int>(r * 16 + c));
                ImGui::SetCursorScreenPos(kMin);
                ImGui::InvisibleButton("##key", ImVec2(kMax.x - kMin.x, kMax.y - kMin.y), ImGuiButtonFlags_MouseButtonLeft);
                if (ImGui::IsItemHovered() && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
                    board.Set(r, c);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                {
                    board.Set(r, c);
                    pressedKey = true;
                    chosen     = key;
                }
                ImGui::PopID();

                const bool cursor  = r == board.Row() && c == board.Col();
                const bool enabled = board.Enabled(key, charset);
                if (cursor)
                    drawBar(style, dl, kMin, kMax, rex::ui::ChromeState::kSelected);
                else
                    dl->AddRectFilled(kMin, kMax, color(kNavyMid, 0.7f), 0.15f * em);
                std::string caption;
                switch (key.kind)
                {
                    case text_entry::KeyKind::kChar:
                        caption = std::string(1, board.CharOf(key));
                        break;
                    case text_entry::KeyKind::kSpace:
                        caption = pageText(Id::kKeySpace);
                        break;
                    case text_entry::KeyKind::kShift:
                        caption = pageText(Id::kKeyShift);
                        break;
                    case text_entry::KeyKind::kPaste:
                        caption = pageText(Id::kKeyPaste);
                        break;
                    case text_entry::KeyKind::kClear:
                        caption = pageText(Id::kKeyClear);
                        break;
                    case text_entry::KeyKind::kDelete:
                        caption = pageText(Id::kKeyDelete);
                        break;
                    case text_entry::KeyKind::kDone:
                        caption = pageText(Id::kKeyDone);
                        break;
                }
                const bool  lit = key.kind == text_entry::KeyKind::kShift && board.Shifted();
                const ImU32 col = color(lit ? kArrow : rgba(255, 255, 255), enabled ? 1.0f : 0.3f);
                drawCenteredText(dl, kMin, kMax, caption.c_str(), col);
            }
        }
        if (pressedKey)
            pressTextKey(chosen);

        const float footTop  = pMax.y - margin - footerH;
        const float promptSz = footerH * 0.8f;
        const float promptY  = footTop + (footerH - promptSz) * 0.5f;
        float       x        = pMin.x + margin;
        const float step     = em * 0.9f;
        if (family != button_icons::Family::kKeyboard)
        {
            x += drawPrompt(kGlyphA, pageText(Id::kLegendType), ImVec2(x, promptY), promptSz) + step;
            x += drawPrompt(kGlyphX, pageText(Id::kKeyDelete), ImVec2(x, promptY), promptSz) + step;
            if (layout == text_entry::Layout::kFull)
                x += drawPrompt(kGlyphY, pageText(Id::kKeyShift), ImVec2(x, promptY), promptSz) + step;
            const char* startLabel = family == button_icons::Family::kPlayStation ? "Options" : "Start";
            drawPill(dl, ImVec2(x, promptY), promptSz, startLabel);
            const std::string ok = pageText(Id::kKeyDone);
            dl->AddText(ImVec2(x + pillWidth(promptSz, startLabel) + promptSz * 0.25f, promptY + (promptSz - ImGui::CalcTextSize(ok.c_str()).y) * 0.5f), color(rgba(255, 255, 255)), ok.c_str());
        }
        else
        {
            drawPrompt(kGlyphA, pageText(Id::kKeyDone), ImVec2(x, promptY), promptSz);
        }
        const float       cancelW = std::min(9.0f * em, 0.3f * panelW);
        const std::string cancel  = pageText(Id::kLegendCancel);
        if (legendButton("##text_cancel", kGlyphB, cancel.c_str(), ImVec2(pMax.x - margin - cancelW, footTop), m, true, cancelW))
            model_.CancelText();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
}

} // namespace rerevved

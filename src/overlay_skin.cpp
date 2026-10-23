// Host overlays use game navy/Civilopedia colors, system Trebuchet MS, square
// panels and buttons from player FPK retail menu tiles. No retail assets ship;
// missing files fall back to flat button colors.

#include "app.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/style.h>

#include "retail_ui_art.h"

namespace
{

constexpr ImVec4 rgba(int r, int g, int b, float a = 1.0f)
{
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

// Colors sampled from the Options screen tiles.
constexpr ImVec4 kNavy      = rgba(16, 28, 74);
constexpr ImVec4 kNavyDeep  = rgba(8, 13, 35);
constexpr ImVec4 kNavyMid   = rgba(25, 57, 148);
constexpr ImVec4 kEdge      = rgba(78, 122, 217);
constexpr ImVec4 kHighlight = rgba(0, 95, 169);
constexpr ImVec4 kGlow      = rgba(117, 205, 255);
constexpr ImVec4 kBand      = rgba(38, 83, 219);
// The Civilopedia description panel's translucent navy (gfx_civilopedia.gfx),
// framed in black like the menu tiles.
constexpr ImVec4 kPanelFill = rgba(9, 26, 56, 192.0f / 255.0f);
constexpr ImVec4 kPanelEdge = rgba(0, 0, 0);

// Trebuchet is baked at twice its base size so the overlay scale shrinks
// glyphs rather than stretching them; the SDK font backend has a fixed atlas.
constexpr float kFontBaseSize = 13.0f;
constexpr float kFontBakeSize = 26.0f;

// One retail tile drawn as a 9-slice: `border` source pixels on each side
// stay unscaled relative to the destination border.
struct Tile
{
    const char*                                   archive;
    const char*                                   entry;
    float                                         border;
    float                                         width  = 0.0f;
    float                                         height = 0.0f;
    std::optional<rerevved::retail_ui_art::Image> image;
    std::unique_ptr<rex::ui::ImmediateTexture>    texture;
};

// Owned by the chrome callbacks, which live in the ImGui drawer's Style, so
// the textures are released before the SDK disposes the immediate drawer.
struct SkinArt
{
    std::function<std::filesystem::path()>     gameRoot;
    std::function<rex::ui::ImmediateDrawer*()> drawer;
    bool                                       loaded = false;
    Tile                                       button{ "Common0.FPK", "bardrkbluebutton.dds", 48.0f, {}, {} };
    Tile                                       highlight{ "Pregame.FPK", "outline64x64Highlight2.dds", 16.0f, {}, {} };

    void load()
    {
        if (loaded)
            return;
        loaded                           = true;
        const std::filesystem::path root = gameRoot();
        if (root.empty())
            return;
        for (Tile* tile : { &button, &highlight })
        {
            std::string error;
            const auto  bytes = rerevved::retail_ui_art::ReadFpkEntry(root / "Resource" / "Common" / tile->archive, tile->entry, &error);
            if (bytes)
                tile->image = rerevved::retail_ui_art::DecodeDds(*bytes, &error);
            if (tile->image)
            {
                tile->width  = static_cast<float>(tile->image->width);
                tile->height = static_cast<float>(tile->image->height);
            }
            if (!tile->image)
                REXLOG_WARN("Overlay skin: {}:{} unavailable ({}); using flat colors", tile->archive, tile->entry, error);
        }
    }

    // The native drawer returns null until it is ready, so creation retries
    // on later frames while the decoded pixels are kept.
    rex::ui::ImmediateTexture* texture(Tile& tile)
    {
        load();
        if (!tile.texture && tile.image)
        {
            if (auto* immediate = drawer())
            {
                tile.texture = immediate->CreateTexture(tile.image->width, tile.image->height, rex::ui::ImmediateTextureFilter::kLinear, false, tile.image->rgba.data());
                if (tile.texture)
                    tile.image.reset();
            }
        }
        return tile.texture.get();
    }

    void drawSliced(ImDrawList* draw_list, Tile& tile, ImVec2 min, ImVec2 max, float top, float side, ImU32 tint)
    {
        auto* texture = this->texture(tile);
        if (!texture)
            return;
        const ImTextureID id = reinterpret_cast<ImTextureID>(texture);
        side                 = std::min(side, std::min(max.x - min.x, max.y - min.y) * 0.5f);
        top                  = std::min(top, (max.y - min.y) * 0.5f);
        const float xs[]     = { min.x, min.x + side, max.x - side, max.x };
        const float ys[]     = { min.y, min.y + top, max.y - side, max.y };
        const float us[]     = { 0.0f, tile.border / tile.width, 1.0f - tile.border / tile.width, 1.0f };
        const float vs[]     = { 0.0f, tile.border / tile.height, 1.0f - tile.border / tile.height, 1.0f };
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                draw_list->AddImage(id, ImVec2(xs[column], ys[row]), ImVec2(xs[column + 1], ys[row + 1]), ImVec2(us[column], vs[row]), ImVec2(us[column + 1], vs[row + 1]), tint);
            }
        }
    }
};

// A square box inset so the whole edge stays inside the clip rect.
void drawBox(ImDrawList* draw_list, ImVec2 min, ImVec2 max, ImU32 fill)
{
    draw_list->AddRectFilled(min, max, fill);
    draw_list->AddRect(ImVec2(min.x + 1.0f, min.y + 1.0f), ImVec2(max.x - 1.0f, max.y - 1.0f), ImGui::GetColorU32(kPanelEdge), 0.0f, 0, 2.0f);
}

void drawWindow(ImDrawList* draw_list, ImVec2 min, ImVec2 max, float title_height)
{
    drawBox(draw_list, min, max, ImGui::GetColorU32(kPanelFill));
    if (title_height > 0.0f)
    {
        // Underline the title band like the menu headers.
        const float inset = ImGui::GetFontSize() * 0.6f;
        const float y     = min.y + title_height;
        draw_list->AddLine(ImVec2(min.x + inset, y), ImVec2(max.x - inset, y), ImGui::GetColorU32(rgba(78, 122, 217, 0.8f)), 1.0f);
    }
}

void drawPanel(ImDrawList* draw_list, ImVec2 min, ImVec2 max)
{
    drawBox(draw_list, min, max, ImGui::GetColorU32(rgba(8, 13, 35, 0.35f)));
}

// Scales an art tint by the current style alpha, which disabled widgets lower.
ImU32 tint(float alpha)
{
    return IM_COL32(255, 255, 255, static_cast<int>(255.0f * alpha * ImGui::GetStyle().Alpha));
}

void drawButton(SkinArt& art, ImDrawList* draw_list, ImVec2 min, ImVec2 max, rex::ui::ChromeState state)
{
    using rex::ui::ChromeState;
    const float corner = (max.y - min.y) * 0.35f;
    Tile&       tile   = state == ChromeState::kNormal ? art.button : art.highlight;
    if (art.texture(tile))
    {
        art.drawSliced(draw_list, tile, min, max, corner, corner, tint(state == ChromeState::kHovered ? 0.85f : 1.0f));
        return;
    }
    const ImVec4 fill = state == ChromeState::kNormal ? kNavyMid : state == ChromeState::kHovered ? kHighlight
                                                                                                  : rgba(20, 174, 255);
    draw_list->AddRectFilled(min, max, ImGui::GetColorU32(fill), corner * 0.5f);
    draw_list->AddRect(min, max, ImGui::GetColorU32(rgba(0, 0, 0)), corner * 0.5f);
}

void drawSelection(ImDrawList* draw_list, ImVec2 min, ImVec2 max, rex::ui::ChromeState state)
{
    const float alpha = state == rex::ui::ChromeState::kHovered ? 0.45f : 1.0f;
    draw_list->AddRectFilled(min, max, ImGui::GetColorU32(rgba(0, 95, 169, 0.9f * alpha)));
    draw_list->AddRect(min, max, ImGui::GetColorU32(rgba(117, 205, 255, alpha)));
}

void applyPalette(ImGuiStyle& style)
{
    style.WindowRounding    = 0.0f;
    style.ChildRounding     = 0.0f;
    style.FrameRounding     = 3.0f;
    style.PopupRounding     = 0.0f;
    style.GrabRounding      = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.TabRounding       = 3.0f;
    style.FrameBorderSize   = 1.0f;
    style.FontSizeBase      = kFontBaseSize;

    ImVec4* c                        = style.Colors;
    c[ImGuiCol_Text]                 = rgba(255, 255, 255);
    c[ImGuiCol_TextDisabled]         = rgba(150, 170, 210);
    c[ImGuiCol_WindowBg]             = rgba(16, 28, 74, 0.96f);
    c[ImGuiCol_ChildBg]              = rgba(0, 0, 0, 0.0f);
    c[ImGuiCol_PopupBg]              = rgba(16, 28, 74, 0.98f);
    c[ImGuiCol_Border]               = rgba(0, 0, 0, 0.85f);
    c[ImGuiCol_BorderShadow]         = rgba(0, 0, 0, 0.0f);
    c[ImGuiCol_FrameBg]              = rgba(8, 13, 35, 0.85f);
    c[ImGuiCol_FrameBgHovered]       = rgba(26, 43, 93);
    c[ImGuiCol_FrameBgActive]        = kHighlight;
    c[ImGuiCol_TitleBg]              = kNavyMid;
    c[ImGuiCol_TitleBgActive]        = kBand;
    c[ImGuiCol_TitleBgCollapsed]     = kNavyMid;
    c[ImGuiCol_MenuBarBg]            = kNavy;
    c[ImGuiCol_ScrollbarBg]          = rgba(8, 13, 35, 0.5f);
    c[ImGuiCol_ScrollbarGrab]        = rgba(51, 85, 176);
    c[ImGuiCol_ScrollbarGrabHovered] = kEdge;
    c[ImGuiCol_ScrollbarGrabActive]  = kGlow;
    c[ImGuiCol_CheckMark]            = kGlow;
    c[ImGuiCol_SliderGrab]           = kEdge;
    c[ImGuiCol_SliderGrabActive]     = kGlow;
    c[ImGuiCol_Button]               = kNavyMid;
    c[ImGuiCol_ButtonHovered]        = kHighlight;
    c[ImGuiCol_ButtonActive]         = rgba(20, 174, 255);
    c[ImGuiCol_Header]               = rgba(0, 95, 169, 0.9f);
    c[ImGuiCol_HeaderHovered]        = rgba(0, 95, 169, 0.55f);
    c[ImGuiCol_HeaderActive]         = rgba(20, 174, 255);
    c[ImGuiCol_Separator]            = rgba(78, 122, 217, 0.6f);
    c[ImGuiCol_SeparatorHovered]     = kEdge;
    c[ImGuiCol_SeparatorActive]      = kGlow;
    c[ImGuiCol_ResizeGrip]           = rgba(78, 122, 217, 0.3f);
    c[ImGuiCol_ResizeGripHovered]    = rgba(78, 122, 217, 0.7f);
    c[ImGuiCol_ResizeGripActive]     = kGlow;
    c[ImGuiCol_Tab]                  = kNavyMid;
    c[ImGuiCol_TabHovered]           = kHighlight;
    c[ImGuiCol_TabSelected]          = kHighlight;
    c[ImGuiCol_TabDimmed]            = kNavy;
    c[ImGuiCol_TabDimmedSelected]    = kNavyMid;
    c[ImGuiCol_TextSelectedBg]       = rgba(0, 95, 169, 0.6f);
    c[ImGuiCol_NavCursor]            = kGlow;
    c[ImGuiCol_ModalWindowDimBg]     = rgba(8, 13, 35, 0.6f);
}

} // namespace

void rerevved::App::OnConfigureFonts(ImFontAtlas* atlas)
{
#if REX_PLATFORM_WIN32
    const std::filesystem::path path = "C:\\Windows\\Fonts\\trebuc.ttf";
    std::error_code             ec;
    if (!std::filesystem::is_regular_file(path, ec))
    {
        REXLOG_WARN("Overlay skin: {} not found; using the default font", path.string());
        return;
    }
    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0 };
    ImFontConfig         config;
    config.OversampleH = 2;
    config.OversampleV = 2;
    if (ImFont* font = atlas->AddFontFromFileTTF(path.string().c_str(), kFontBakeSize, &config, ranges))
        ImGui::GetIO().FontDefault = font;
#else
    (void)atlas;
#endif
}

void rerevved::App::OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style)
{
    // Overlays are laid out for 900 pixels of height and grow with the window.
    ui_style.scale.reference_height = 900.0f;

    applyPalette(imgui_style);
    if (!ImGui::GetIO().FontDefault)
        imgui_style.FontSizeBase = 0.0f; // keep the SDK font at its own size

    auto art      = std::make_shared<SkinArt>();
    art->gameRoot = [this]
    {
        return game_data_root();
    };
    art->drawer = [this]
    {
        return immediate_drawer();
    };
    ui_style.chrome.draw_window = drawWindow;
    ui_style.chrome.draw_button = [art](ImDrawList* draw_list, ImVec2 min, ImVec2 max, rex::ui::ChromeState state)
    {
        drawButton(*art, draw_list, min, max, state);
    };
    ui_style.chrome.draw_selection = drawSelection;
    ui_style.chrome.draw_panel     = drawPanel;

    ui_style.fps_counter.text             = kGlow;
    ui_style.settings.lifecycle_live      = kGlow;
    ui_style.achievements.header_text     = kGlow;
    ui_style.achievements.row_unlocked_bg = rgba(0, 95, 169, 0.45f);
    ui_style.achievements.progress_bar    = kHighlight;
    ui_style.console.completion_highlight = rgba(0, 95, 169, 0.8f);
    // Keep toasts above the date panel in the game's bottom-right corner.
    ui_style.toast.bottom_inset = 0.12f;
}

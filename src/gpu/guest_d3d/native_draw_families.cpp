#include "native_draw_families.h"

#include "gfx_draw_adapter.h"
#include "label_draw_adapter.h"
#include "leader_draw_adapter.h"
#include "movie_draw_adapter.h"
#include "scene_draw_adapter.h"
#include "scene_mesh_draw_adapter.h"

#include <array>
#include <utility>

namespace rerevved::gpu::guest_d3d
{

std::span<const NativeDrawFamilyRegistration> NativeDrawFamilyRegistry()
{
    static const std::array registry{
        SolidPanelDrawFamily(),
        LabelDrawFamily(),
        GfxDrawFamily(),
        SceneMeshDrawFamily(),
        SceneMenuDrawFamily(),
        LeaderDrawFamily(),
        MovieDrawFamily(),
        SceneDrawFamily(),
    };
    return registry;
}

NativeFrameDrawBuilder ComposeNativeDrawFamilies(std::vector<NativeDrawFamily> families)
{
    return [families = std::move(families)](const NativeDrawSubmission& submission, NativeDrawReplayRecipe& draw, std::string& detail)
    {
        detail.clear();
        for (const auto& family : families)
            switch (family(submission, draw, detail))
            {
                case NativeDrawFamilyResult::Built:
                    return true;
                case NativeDrawFamilyResult::Refused:
                    return false;
                case NativeDrawFamilyResult::NotMine:
                    break;
            }
        return false;
    };
}

NativeDrawFamily MakeSolidPanelDrawFamily(ReadMemory read, NativeDrawFamilyShader shader)
{
    return [build = MakeSolidPanelDrawBuilder(std::move(read), std::move(shader))](const NativeDrawSubmission& submission, NativeDrawReplayRecipe& draw, std::string& detail)
    {
        if (build(submission, draw, detail))
            return NativeDrawFamilyResult::Built;
        return detail.empty() ? NativeDrawFamilyResult::NotMine : NativeDrawFamilyResult::Refused;
    };
}

NativeDrawFamilyRegistration SolidPanelDrawFamily()
{
    return { "solid panel", "native_draw_panel_capture", "solid adapter capture", AdmitShaderDrawFamily<LoadSolidPanelShader, MakeSolidPanelDrawFamily> };
}

NativeFrameDrawBuilder MakeBootDrawBuilder(ReadMemory read, NativeDrawFamilyShader panel, NativeDrawFamilyShader label, std::shared_ptr<const GfxDrawCatalog> gfx, NativeDrawFamilyShader sceneMesh, NativeDrawFamilyShader movie)
{
    std::vector<NativeDrawFamily> families;
    if (panel)
        families.push_back(MakeSolidPanelDrawFamily(read, std::move(panel)));
    if (label)
        families.push_back(MakeLabelDrawFamily(read, std::make_shared<LabelDrawShaders>(LabelDrawShaders{ *label, std::nullopt })));
    if (gfx)
        families.push_back(MakeGfxDrawFamily(read, std::move(gfx)));
    if (sceneMesh)
        families.push_back(MakeSceneMeshDrawFamily(read, std::move(sceneMesh)));
    if (movie)
        families.push_back(MakeMovieDrawFamily(read, std::move(movie)));
    return ComposeNativeDrawFamilies(std::move(families));
}

} // namespace rerevved::gpu::guest_d3d

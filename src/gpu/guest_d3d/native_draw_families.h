#pragma once

#include "native_frame_assembler.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

using NativeDrawFamilyShader = std::shared_ptr<const NativeGameplayShaderInterface>;

enum class NativeDrawFamilyResult
{
    NotMine, // `detail` is empty; the next family is tried
    Built,
    Refused, // `detail` names the reason, if any; composition stops
};

// One family's native route for a submission.
using NativeDrawFamily = std::function<NativeDrawFamilyResult(const NativeDrawSubmission&, NativeDrawReplayRecipe&, std::string& detail)>;

// A family from its reader and builder. A reader that returns false with an
// empty error leaves the submission to the next family; one with an error
// refuses it. Once the reader accepts, the builder's result is final.
template <typename Inputs, typename Reader, typename Builder>
NativeDrawFamily MakeReaderDrawFamily(Reader reader, Builder builder)
{
    return [reader = std::move(reader), builder = std::move(builder)](const NativeDrawSubmission& submission, NativeDrawReplayRecipe& draw, std::string& detail)
    {
        Inputs inputs;
        // The inputs' storage, or the state shadow's, serves the next draw's
        // read on this thread.
        struct Recycle
        {
            Inputs& inputs;
            ~Recycle()
            {
                if constexpr (requires { ReturnDrawInputs(std::move(inputs)); })
                    ReturnDrawInputs(std::move(inputs));
                else if constexpr (requires { inputs.state; })
                    ReturnStateBuffer(std::move(inputs.state));
            }
        } recycle{ inputs };
        if (reader(submission, inputs, detail))
            return builder(inputs, draw, detail) ? NativeDrawFamilyResult::Built : NativeDrawFamilyResult::Refused;
        return detail.empty() ? NativeDrawFamilyResult::NotMine : NativeDrawFamilyResult::Refused;
    };
}

// A family option names its shader path; admit loads its form and binds it to
// guest memory as family.
struct NativeDrawFamilyRegistration
{
    std::string_view name;   // log name, as in "<name> family admitted"
    std::string_view option; // cvar name
    std::string_view source; // what `option` names, as in "not a usable <source>"
    bool (*admit)(const std::filesystem::path& source, ReadMemory read, NativeDrawFamily& family, std::string& summary, std::string& error);
};

// `admit` for a family with one shader interface: `Load` reads it from the
// source path and `Make` binds it.
template <auto Load, auto Make>
bool AdmitShaderDrawFamily(const std::filesystem::path& source, ReadMemory read, NativeDrawFamily& family, std::string&, std::string& error)
{
    auto shader = std::make_shared<NativeGameplayShaderInterface>();
    if (!Load(source, *shader, error))
        return false;
    family = Make(std::move(read), std::move(shader));
    return true;
}

// Every family in the order composition tries them. Each adapter defines its
// registration next to its reader and builder; this list only orders them.
std::span<const NativeDrawFamilyRegistration> NativeDrawFamilyRegistry();

// Tries each family in order. The first Built or Refused result ends the
// submission; when every family passes, the result is false with an empty
// detail, which the assembler reads as no native route.
NativeFrameDrawBuilder ComposeNativeDrawFamilies(std::vector<NativeDrawFamily> families);

// The solid panel family, whose reader and builder live in the assembler.
NativeDrawFamily             MakeSolidPanelDrawFamily(ReadMemory read, NativeDrawFamilyShader shader);
NativeDrawFamilyRegistration SolidPanelDrawFamily();

struct GfxDrawCatalog;

// The boot families: the solid panel, the label batches, the GFx catalog's
// quads, the post-ESRB MRT prepass mesh, then the intro movie quad. A null
// shader or catalog leaves its family without a native route.
NativeFrameDrawBuilder MakeBootDrawBuilder(ReadMemory read, NativeDrawFamilyShader panel, NativeDrawFamilyShader label, std::shared_ptr<const GfxDrawCatalog> gfx = nullptr, NativeDrawFamilyShader sceneMesh = nullptr, NativeDrawFamilyShader movie = nullptr);

} // namespace rerevved::gpu::guest_d3d

#include "sequence.h"
#include "gameplay_input.h"
#include "gpu/d3d12/native_gameplay_render_frame.h"
#include "gpu/d3d12/native_gameplay_shaders.h"
#include "gpu/d3d12/native_renderer_d3d12.h"

#include <charconv>
#include <chrono>
#include <format>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>

#include <rex/crypto/sha256.h>
#include <toml++/toml.hpp>

namespace
{

void require(bool value, const std::string& error)
{
    if (!value)
        throw std::runtime_error(error);
}

std::filesystem::path contained(const std::filesystem::path& root, const std::string& value)
{
    const std::filesystem::path name(value);
    require(!name.empty() && !name.is_absolute(), "sequence input requires a relative path");
    const auto path     = std::filesystem::canonical(root / name);
    const auto relative = path.lexically_relative(root);
    require(!relative.empty() && !relative.is_absolute() && *relative.begin() != "..", "sequence input escapes its directory");
    return path;
}

std::string read(const std::filesystem::path& path)
{
    require(std::filesystem::file_size(path) <= 512U * 1024U * 1024U, "sequence input metadata exceeds its byte bound");
    std::ifstream file(path, std::ios::binary);
    std::string   result{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    require(bool(file), "could not read sequence metadata");
    return result;
}

std::uint64_t number(const std::string& value)
{
    std::uint64_t result = 0;
    const auto    parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(), "invalid sequence integer");
    return result;
}

} // namespace

bool ReplayGameplaySequence(const std::filesystem::path& recipePath,
                            const std::filesystem::path& outputPath,
                            std::string&                 error)
{
    using namespace rerevved::gpu;
    try
    {
        const auto root = std::filesystem::canonical(recipePath).parent_path();
        require(std::filesystem::file_size(recipePath) <= 1024U * 1024U, "sequence recipe exceeds its metadata bound");
        const auto recipe = toml::parse_file(recipePath.string());
        require(recipe["gameplay_sequence_schema_version"].value_or(0) == 1, "unsupported gameplay sequence recipe");
        const auto sessionPath = contained(root, recipe["recording"].value_or<std::string>(""));
        const auto session     = toml::parse(read(sessionPath));
        require(session["gameplay_recording_schema_version"].value_or(0) == 1 && session["complete"].value_or(false) &&
                    session["input_origin"].value_or<std::string>("") == "guest_cpu_observer" &&
                    session["error"].value_or<std::string>("").empty(),
                "sequence recording is incomplete or unsupported");
        const auto expectedFrames = session["frames"].value_or<std::int64_t>(0);
        const auto producerPage   = session["producer_page"].value_or<std::int64_t>(0);
        require(expectedFrames > 0 && producerPage >= 0 && producerPage < 0x20000, "sequence frame count or producer page is invalid");
        std::set<std::uint32_t> requestedPages;
        if (producerPage)
            requestedPages.insert(static_cast<std::uint32_t>(producerPage));
        if (recipe.contains("retain_pages"))
        {
            const auto* pages = recipe["retain_pages"].as_array();
            require(pages && pages->size() <= NativeRenderBatchCommands, "sequence retention pages must be a bounded array");
            for (const auto& entry : *pages)
            {
                const auto page = entry.value<std::int64_t>();
                require(page && *page > 0 && *page < 0x20000, "sequence retention page is invalid");
                requestedPages.insert(static_cast<std::uint32_t>(*page));
            }
        }
        const std::vector<std::uint32_t>           retainPages(requestedPages.begin(), requestedPages.end());
        std::vector<NativeGameplayShaderInterface> shaders;
        require(LoadNativeGameplayShaders(contained(root, recipe["shaders"].value_or<std::string>("")), shaders, error), error);
        const auto         recordingRoot = sessionPath.parent_path();
        std::istringstream ledger(read(contained(recordingRoot, session["ledger"].value_or<std::string>(""))));
        std::string        line;
        std::getline(ledger, line);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        require(line == "ordinal,guest_frame,elapsed_us,capture_us,write_us,owned_bytes,manifest,sha256", "sequence ledger header is invalid");
        require(!std::filesystem::exists(outputPath), "sequence result must be fresh");
        const auto framesDirectory = outputPath.parent_path() / (outputPath.stem().string() + "-frames");
        require(std::filesystem::create_directory(framesDirectory), "sequence frame output must be fresh");
        NativeRendererD3D12::ConfigureReplayDiagnostics();
        NativeRendererD3D12 renderer;
        require(renderer.StartDrawStream(error), error);
        std::vector<NativeGameplayRetainedTexture> retained;
        std::uint64_t                              frameCount = 0, lastFrame = 0;
        bool                                       producerSeen = false;
        toml::array                                results;
        while (std::getline(ledger, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            require(!line.empty(), "sequence ledger contains an empty record");
            std::istringstream       row(line);
            std::vector<std::string> fields;
            std::string              field;
            while (std::getline(row, field, ','))
                fields.push_back(field);
            require(fields.size() == 8, "sequence ledger record is malformed");
            const auto ordinal = number(fields[0]), guestFrame = number(fields[1]);
            const auto manifest = contained(recordingRoot, fields[6]);
            const auto text     = read(manifest);
            require(rex::crypto::sha256(text) == fields[7], "sequence manifest digest mismatch");
            require(toml::parse(text)["input_origin"].value_or<std::string>("") == "guest_cpu_observer", "sequence requires CPU-owned inputs");
            NativeGameplaySnapshot snapshot;
            require(LoadGameplayCaptureInputs(manifest, snapshot, error), error);
            require(!snapshot.events.empty() && snapshot.events.back().frame == guestFrame, "sequence guest-frame identity mismatch");
            NativeRenderFrame frame;
            if (!ordinal)
            {
                require(producerPage && !producerSeen && !frameCount, "unexpected sequence producer record");
                NativeGameplayRetainedTexture producer;
                require(BuildNativeGameplayProducer(snapshot, shaders, {}, static_cast<std::uint32_t>(producerPage), frame, producer, error), error);
                const auto result = renderer.SubmitRenderFrame(std::move(frame));
                require(result.success, result.error);
                retained.push_back(producer);
                producerSeen = true;
                continue;
            }
            require(ordinal == frameCount + 1 && ordinal <= static_cast<std::uint64_t>(expectedFrames) &&
                        (!frameCount || guestFrame == lastFrame + 1) && (!producerPage || producerSeen),
                    "sequence frames are missing or out of order");
            std::vector<NativeGameplayRetainedTexture> nextRetained;
            const auto                                 conversionStart = std::chrono::steady_clock::now();
            if (!BuildNativeGameplaySequenceFrame(snapshot, shaders, retained, retainPages, frame, nextRetained, error))
                throw std::runtime_error(std::format("sequence frame {} guest {} conversion: {}", ordinal, guestFrame, error));
            const auto conversionUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - conversionStart).count();
            const auto name         = std::format("frame-{:06}.rgb10", ordinal);
            frame.outputPath        = framesDirectory / name;
            const auto start        = std::chrono::steady_clock::now();
            const auto before       = renderer.ReplayTimingStats();
            const auto cacheBefore  = renderer.ReplayCacheStats();
            const auto result       = renderer.SubmitRenderFrame(std::move(frame));
            const auto renderUs     = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
            require(result.success, std::format("sequence frame {} guest {} render: {}", ordinal, guestFrame, result.error));
            const auto  pipelines = renderer.ReplayCacheStats().pipelineCreations - cacheBefore.pipelineCreations;
            const auto  pixels    = std::string_view(reinterpret_cast<const char*>(result.resolvedOutput.data()), result.resolvedOutput.size());
            const auto  after     = renderer.ReplayTimingStats();
            toml::array versions;
            for (const auto& texture : nextRetained)
                versions.push_back(toml::table{
                    { "page", texture.guestBasePage },
                    { "producer_event", static_cast<std::int64_t>(texture.texture.persistentKey) } });
            results.push_back(toml::table{
                { "ordinal", static_cast<std::int64_t>(ordinal) },
                { "guest_frame", static_cast<std::int64_t>(guestFrame) },
                { "capture_us", static_cast<std::int64_t>(number(fields[3])) },
                { "conversion_us", conversionUs },
                { "render_us", renderUs },
                { "draw_recording_ns", static_cast<std::int64_t>(after.recordingNanoseconds - before.recordingNanoseconds) },
                { "fence_wait_ns", static_cast<std::int64_t>(after.fenceWaitNanoseconds - before.fenceWaitNanoseconds) },
                { "texture_upload_ns", static_cast<std::int64_t>(after.textureUploadNanoseconds - before.textureUploadNanoseconds) },
                { "draw_upload_ns", static_cast<std::int64_t>(after.drawUploadNanoseconds - before.drawUploadNanoseconds) },
                { "pipeline_creations", static_cast<std::int64_t>(pipelines) },
                { "submissions", static_cast<std::int64_t>(after.queueSubmissions - before.queueSubmissions) },
                { "retained", std::move(versions) },
                { "output", (framesDirectory.filename() / name).generic_string() },
                { "sha256", rex::crypto::sha256(pixels) } });
            retained  = std::move(nextRetained);
            lastFrame = guestFrame;
            ++frameCount;
            if (ordinal % 100 == 0 || ordinal == static_cast<std::uint64_t>(expectedFrames))
                std::cout << "native sequence: " << ordinal << '/' << expectedFrames << " frames completed\n"
                          << std::flush;
        }
        require(frameCount == static_cast<std::uint64_t>(expectedFrames), "sequence ledger ended before the declared frame count");
        renderer.Shutdown();
        std::ofstream result(outputPath, std::ios::out | std::ios::noreplace);
        result << toml::table{ { "gameplay_sequence_replay_schema_version", 1 }, { "complete", true }, { "frames", std::move(results) } };
        result.close();
        require(bool(result), "could not publish sequence replay result");
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

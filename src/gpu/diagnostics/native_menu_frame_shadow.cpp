#include "native_menu_frame_shadow.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <limits>
#include <mutex>
#include <thread>

#include <fmt/format.h>

#include <rex/crypto/sha256.h>
#include <rex/cvar.h>
#include <rex/graphics/draw_capture_mailbox.h>
#include <rex/logging.h>
#include <toml++/toml.hpp>

#include "gpu/d3d12/native_menu_frame.h"
#include "gpu/d3d12/native_renderer_d3d12.h"

namespace rerevved::gpu::diagnostics
{
namespace
{

using namespace rex::graphics::diagnostic;

std::string digest(std::span<const std::uint8_t> bytes)
{
    return rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

bool write(const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
{
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    file.close();
    return bool(file);
}

std::uint32_t word(const std::uint8_t* bytes)
{
    return bytes[0] | (std::uint32_t(bytes[1]) << 8) | (std::uint32_t(bytes[2]) << 16) |
           (std::uint32_t(bytes[3]) << 24);
}

bool decodeTexture(const DrawCaptureTexture& source, const DrawCaptureTextureUse& use, NativeDrawReplayTexture& texture, NativeDrawReplaySampler& sampler, std::string& error)
{
    const auto fail = [&](const char* reason)
    {
        error = reason;
        return false;
    };
    if (!use.data_available || use.is_signed || use.fetch_constant != 0 || source.scaled_resolve ||
        source.mip_level != 0 || source.host_depth_or_array_size != 1 ||
        !source.host_width || !source.host_height || source.host_width > 4096 || source.host_height > 4096)
        return fail("unsupported live menu texture binding or extent");
    texture.width         = source.host_width;
    texture.height        = source.host_height;
    std::uint32_t rowSize = 0, rows = source.host_height;
    switch (source.host_format)
    {
        case 27:
        case 28:
            texture.format = NativeDrawReplayTextureFormat::Rgba8;
            rowSize        = texture.width * 4;
            break;
        case 60:
        case 61:
            texture.format = NativeDrawReplayTextureFormat::R8;
            rowSize        = texture.width;
            break;
        case 70:
        case 71:
            texture.format = NativeDrawReplayTextureFormat::Bc1;
            rowSize        = ((texture.width + 3) / 4) * 8;
            rows           = (texture.height + 3) / 4;
            break;
        case 73:
        case 74:
            texture.format = NativeDrawReplayTextureFormat::Bc2;
            rowSize        = ((texture.width + 3) / 4) * 16;
            rows           = (texture.height + 3) / 4;
            break;
        default:
            return fail("unsupported live menu texture format");
    }
    if (source.row_size != rowSize || source.row_count != rows || source.bytes.size() != std::size_t(rowSize) * rows)
        return fail("live menu texture does not contain complete tightly packed rows");
    texture.bytes = source.bytes;
    if ((use.component_mapping & ~0x1FFFU) || !(use.component_mapping & 0x1000))
        return fail("invalid live menu texture component mapping");
    for (std::size_t i = 0; i < 4; ++i)
    {
        texture.swizzle[i] = (use.component_mapping >> (i * 3)) & 7;
        if (texture.swizzle[i] > 5)
            return fail("unsupported live menu texture component selector");
    }
    const auto value = use.sampler_value;
    if ((value >> 22) || ((value >> 14) & 7) || ((value >> 17) & 15))
        return fail("live menu sampler requires unsupported anisotropy or mip levels");
    constexpr std::uint32_t addressModes[] = { 1, 2, 3, 5, 3, 5, 4, 5 };
    for (std::size_t i = 0; i < 3; ++i)
        sampler.address[i] = addressModes[(value >> (i * 3)) & 7];
    sampler.magLinear = (value >> 11) & 1;
    sampler.minLinear = (value >> 12) & 1;
    sampler.mipLinear = (value >> 13) & 1;
    const auto border = (value >> 9) & 3;
    if (border != 0)
        return fail("live menu sampler requires an unsupported border color");
    // Match D3D12TextureCache::WriteSampler's base-map magnification behavior.
    sampler.maxLod = (value & (1U << 21)) ? 0.25F : std::numeric_limits<float>::max();
    return true;
}

bool buildViews(const DrawCaptureFrameSnapshot&        snapshot,
                std::vector<NativeMenuFrameEventView>& events,
                std::vector<NativeDrawReplayTexture>&  textures,
                std::vector<NativeDrawReplaySampler>&  samplers,
                std::string&                           error)
{
    if (snapshot.events.empty() || snapshot.events.size() > 1024 ||
        snapshot.start_frame >= snapshot.end_frame || snapshot.start_submission > snapshot.end_submission)
    {
        error = "invalid owned live frame boundaries";
        return false;
    }
    events.resize(snapshot.events.size());
    textures.resize(events.size());
    samplers.resize(events.size());
    for (std::size_t i = 0; i < events.size(); ++i)
    {
        const auto& source = snapshot.events[i];
        const auto& draw   = source.draw.draw;
        auto&       event  = events[i];
        if (source.frame < snapshot.start_frame || source.frame >= snapshot.end_frame ||
            source.submission < snapshot.start_submission || source.submission > snapshot.end_submission)
        {
            error = "live event is outside its completed frame boundary";
            return false;
        }
        event.id                  = source.id;
        event.kind                = static_cast<NativeMenuFrameEventView::Kind>(source.kind);
        event.success             = source.success;
        event.hostIssued          = source.host_issued;
        event.resolve             = source.copy_mode == "resolve";
        event.primitiveType       = draw.primitive_type;
        event.hostPixelShaderHash = draw.pixel_shader_hash;
        event.frontbuffer         = source.frontbuffer_ptr;
        event.frontbufferWidth    = source.frontbuffer_width;
        event.frontbufferHeight   = source.frontbuffer_height;
        auto& view                = event.draw;
        view.registers            = draw.registers;
        view.vertexMicrocode      = draw.vertex_ucode;
        view.pixelMicrocode       = draw.pixel_ucode;
        view.vertexShaderHash     = source.draw.guest_vertex_shader_hash;
        view.pixelShaderHash      = source.draw.guest_pixel_shader_hash;
        view.indexed              = draw.index.present;
        view.indexCount           = draw.requested_index_count;
        view.indexFormat          = draw.index.format;
        view.indexEndian          = draw.index.endianness;
        view.indexBytes           = draw.index.bytes;
        view.halfPixelOffset      = draw.half_pixel_offset;
        const auto slot           = draw.primitive_type == 4 ? 95U : 0U;
        for (const auto& fetch : draw.vertex_fetches)
            if (fetch.fetch_constant == slot)
            {
                if (!view.vertexBytes.empty())
                {
                    error = "duplicate vertex fetch in owned menu draw";
                    return false;
                }
                view.vertexBytes     = fetch.bytes;
                view.vertexGuestBase = fetch.base;
            }
        if (event.kind != NativeMenuFrameEventView::Kind::Draw || draw.primitive_type != 4)
            continue;
        if (draw.guest_primitive_type != 4 || draw.host_primitive_type != 4 ||
            draw.host_vertex_shader_type != 0 || draw.tessellation_mode != 0 ||
            draw.host_primitive_reset_enabled || draw.native_topology != 4 ||
            draw.guest_draw_vertex_count != draw.requested_index_count ||
            draw.host_draw_vertex_count != draw.requested_index_count || draw.used_texture_mask > 1 ||
            draw.native_instance_count != 1 || draw.native_start_vertex || draw.native_start_index ||
            draw.native_base_vertex || draw.native_start_instance ||
            (draw.index.present ? draw.native_index_count != draw.requested_index_count
                                : draw.native_vertex_count != draw.requested_index_count) ||
            (draw.index.present && (draw.index.count != draw.requested_index_count ||
                                    draw.guest_index_base != draw.index.guest_base)))
        {
            error = "live menu host topology differs from its original draw";
            return false;
        }
        if (!source.draw.texture_bindings_complete)
        {
            error = "live menu draw has incomplete translated texture bindings";
            return false;
        }
        const DrawCaptureTextureUse* selected = nullptr;
        for (const auto& use : source.draw.texture_uses)
        {
            if (use.is_signed && !use.data_available)
                continue;
            if (use.stage != 1 || use.fetch_constant != 0 || !use.data_available ||
                use.texture_index >= snapshot.textures.size() || selected)
            {
                error = "unsupported or incomplete point-of-use texture set";
                return false;
            }
            selected = &use;
        }
        if (selected)
        {
            if (!decodeTexture(snapshot.textures[selected->texture_index], *selected, textures[i], samplers[i], error))
                return false;
            view.texture = &textures[i];
            view.sampler = &samplers[i];
        }
    }
    return true;
}

void saveInputs(const std::filesystem::path& root, const DrawCaptureFrameSnapshot& snapshot)
{
    std::filesystem::create_directory(root);
    toml::array events, textures;
    const auto  bytes = [&](const std::string& name, std::span<const std::uint8_t> data)
    {
        if (!write(root / name, data))
            throw std::runtime_error("could not save owned frame input " + name);
        return toml::table{ { "file", name }, { "bytes", std::int64_t(data.size()) }, { "sha256", digest(data) } };
    };
    const auto words = [&](const std::string& name, std::span<const std::uint32_t> data)
    {
        std::vector<std::uint8_t> packed(data.size_bytes());
        for (std::size_t i = 0; i < data.size(); ++i)
            for (unsigned c = 0; c < 4; ++c)
                packed[i * 4 + c] = static_cast<std::uint8_t>(data[i] >> (c * 8));
        return bytes(name, packed);
    };
    for (const auto& event : snapshot.events)
    {
        const auto  stem = "event-" + std::to_string(event.id);
        const auto& draw = event.draw.draw;
        toml::table row{ { "id", std::int64_t(event.id) }, { "kind", std::int64_t(event.kind) }, { "frame", std::int64_t(event.frame) }, { "submission", std::int64_t(event.submission) }, { "success", event.success }, { "host_issued", event.host_issued }, { "primitive", draw.primitive_type }, { "requested_count", draw.requested_index_count }, { "indexed", draw.index.present }, { "index_format", draw.index.format }, { "index_endian", draw.index.endianness }, { "registers", words(stem + "-registers.bin", draw.registers) }, { "vertex_ucode", words(stem + "-vs.bin", draw.vertex_ucode) }, { "pixel_ucode", words(stem + "-ps.bin", draw.pixel_ucode) }, { "indices", bytes(stem + "-indices.bin", draw.index.bytes) }, { "copy_mode", event.copy_mode }, { "frontbuffer", std::int64_t(event.frontbuffer_ptr) } };
        row.insert("guest_vertex_hash", fmt::format("{:016X}", event.draw.guest_vertex_shader_hash));
        row.insert("guest_pixel_hash", fmt::format("{:016X}", event.draw.guest_pixel_shader_hash));
        row.insert("host_vertex_hash", fmt::format("{:016X}", draw.vertex_shader_hash));
        row.insert("host_pixel_hash", fmt::format("{:016X}", draw.pixel_shader_hash));
        row.insert("host_primitive", draw.host_primitive_type);
        row.insert("host_vertex_count", draw.host_draw_vertex_count);
        row.insert("host_vertex_shader_type", draw.host_vertex_shader_type);
        row.insert("host_tessellation", draw.tessellation_mode);
        row.insert("guest_primitive", draw.guest_primitive_type);
        row.insert("guest_vertex_count", draw.guest_draw_vertex_count);
        row.insert("guest_index_base", draw.guest_index_base);
        row.insert("index_base", draw.index.guest_base);
        row.insert("index_count", draw.index.count);
        row.insert("native_topology", draw.native_topology);
        row.insert("native_vertex_count", draw.native_vertex_count);
        row.insert("native_index_count", draw.native_index_count);
        row.insert("native_instance_count", draw.native_instance_count);
        row.insert("native_start_vertex", draw.native_start_vertex);
        row.insert("native_start_index", draw.native_start_index);
        row.insert("native_base_vertex", draw.native_base_vertex);
        row.insert("native_start_instance", draw.native_start_instance);
        row.insert("host_primitive_reset", draw.host_primitive_reset_enabled);
        row.insert("used_texture_mask", draw.used_texture_mask);
        row.insert("normalized_color_mask", draw.normalized_color_mask);
        row.insert("half_pixel_offset", draw.half_pixel_offset);
        row.insert("texture_bindings_complete", event.draw.texture_bindings_complete);
        row.insert("frontbuffer_width", event.frontbuffer_width);
        row.insert("frontbuffer_height", event.frontbuffer_height);
        toml::array fetches, uses;
        for (const auto& fetch : draw.vertex_fetches)
            fetches.push_back(toml::table{ { "slot", fetch.fetch_constant }, { "base", fetch.base }, { "data", bytes(stem + "-fetch-" + std::to_string(fetch.fetch_constant) + ".bin", fetch.bytes) } });
        for (const auto& use : event.draw.texture_uses)
            uses.push_back(toml::table{ { "texture", use.texture_index }, { "stage", use.stage }, { "fetch", use.fetch_constant }, { "binding", use.shader_binding_index }, { "dimension", use.dimension }, { "available", use.data_available }, { "signed", use.is_signed }, { "component_mapping", use.component_mapping }, { "host_swizzle", use.host_swizzle }, { "swizzled_signs", use.swizzled_signs }, { "sampler", use.sampler_value } });
        row.insert("vertex_fetches", std::move(fetches));
        row.insert("texture_uses", std::move(uses));
        events.push_back(std::move(row));
    }
    for (std::size_t i = 0; i < snapshot.textures.size(); ++i)
    {
        const auto& texture = snapshot.textures[i];
        textures.push_back(toml::table{ { "index", std::int64_t(i) }, { "format", texture.host_format }, { "width", texture.host_width }, { "height", texture.host_height }, { "depth_or_array_size", texture.host_depth_or_array_size }, { "mip_level", texture.mip_level }, { "row_pitch", texture.row_pitch }, { "row_size", texture.row_size }, { "row_count", texture.row_count }, { "scaled_resolve", texture.scaled_resolve }, { "guest_base_page", texture.guest_base_page }, { "guest_mip_page", texture.guest_mip_page }, { "guest_width", texture.guest_width }, { "guest_height", texture.guest_height }, { "guest_depth_or_array_size", texture.guest_depth_or_array_size }, { "guest_pitch", texture.guest_pitch }, { "guest_format", texture.guest_format }, { "guest_endianness", texture.guest_endianness }, { "guest_tiled", texture.guest_tiled }, { "guest_packed_mips", texture.guest_packed_mips }, { "data", bytes("texture-" + std::to_string(i) + ".bin", texture.bytes) } });
    }
    toml::table manifest{ { "schema_version", 1 }, { "start_frame", std::int64_t(snapshot.start_frame) }, { "end_frame", std::int64_t(snapshot.end_frame) }, { "start_submission", std::int64_t(snapshot.start_submission) }, { "end_submission", std::int64_t(snapshot.end_submission) }, { "events", std::move(events) }, { "textures", std::move(textures) }, { "gamma", words("gamma.bin", snapshot.final_swap.gamma.gamma_256) }, { "source", bytes("source.bin", snapshot.final_swap.source_bytes) }, { "reference", bytes("reference.rgb10", snapshot.final_swap.output_bytes) } };
    manifest.insert("output_format", snapshot.final_swap.output_format);
    manifest.insert("output_width", snapshot.final_swap.output_width);
    manifest.insert("output_height", snapshot.final_swap.output_height);
    manifest.insert("source_format", snapshot.final_swap.source_format);
    manifest.insert("source_width", snapshot.final_swap.source_width);
    manifest.insert("source_height", snapshot.final_swap.source_height);
    manifest.insert("use_fxaa", snapshot.final_swap.gamma.use_fxaa);
    manifest.insert("use_pwl_gamma", snapshot.final_swap.gamma.use_pwl_gamma_ramp);
    manifest.insert("is_8bpc", snapshot.final_swap.gamma.is_8bpc);
    std::ofstream file(root / "manifest.toml");
    file << manifest;
    file.close();
    if (!file)
        throw std::runtime_error("could not save live frame input manifest");
}

} // namespace

struct NativeMenuFrameShadow::Impl
{
    std::mutex              lifecycleMutex, waitMutex;
    std::condition_variable wake;
    std::thread             worker;
    bool                    stopping = false;
    std::filesystem::path   outputDirectory;
    NativeMenuShaders       shaders;

    void Run();
    void Compare(const DrawCaptureFrameSnapshot& snapshot, toml::table& report);

    void Report(const toml::table& report)
    {
        std::ofstream file(outputDirectory / "comparison.tmp.toml");
        file << report;
        file.close();
        if (!file)
            throw std::runtime_error("could not write live frame report");
        std::filesystem::rename(outputDirectory / "comparison.tmp.toml", outputDirectory / "comparison.toml");
    }
};

void NativeMenuFrameShadow::Impl::Compare(const DrawCaptureFrameSnapshot& snapshot, toml::table& report)
{
    saveInputs(outputDirectory / "input", snapshot);
    const auto& swap = snapshot.final_swap;
    if (snapshot.events.empty() || swap.frontbuffer_ptr != snapshot.events.back().frontbuffer_ptr ||
        swap.gamma.use_pwl_gamma_ramp || swap.gamma.use_fxaa || !swap.gamma.is_8bpc ||
        swap.output_format != 24 || swap.output_width != 1280 || swap.output_height != 720 ||
        swap.output_row_size != 1280 * 4 || swap.output_row_count != 720 || swap.output_bytes.size() != 1280U * 720U * 4U)
        throw std::runtime_error("unsupported live final gamma or presentation format");
    std::vector<NativeMenuFrameEventView> views;
    std::vector<NativeDrawReplayTexture>  textures;
    std::vector<NativeDrawReplaySampler>  samplers;
    NativeFrameReplayRecipe               recipe;
    std::string                           error;
    if (!buildViews(snapshot, views, textures, samplers, error) ||
        !BuildNativeMenuFrameRecipe(views, shaders, swap.gamma.gamma_256, recipe, error))
        throw std::runtime_error(error);
    recipe.outputPath       = outputDirectory / "native.rgb10";
    recipe.sampleOutputPath = outputDirectory / "native.samples";
    NativeRendererD3D12 renderer;
    const auto          result = renderer.ReplayFrame(recipe);
    renderer.Shutdown();
    if (!result.success)
        throw std::runtime_error(result.error);
    std::vector<std::uint8_t> native(swap.output_bytes.size());
    std::ifstream             file(recipe.outputPath, std::ios::binary);
    if (std::filesystem::file_size(recipe.outputPath) != native.size() ||
        !file.read(reinterpret_cast<char*>(native.data()), native.size()))
        throw std::runtime_error("native live frame output is incomplete");
    std::uint64_t differences = 0, sum = 0, nonblack = 0;
    unsigned      maximum = 0;
    bool          opaque  = true;
    for (std::size_t i = 0; i < native.size(); i += 4)
    {
        const auto a = word(native.data() + i), b = word(swap.output_bytes.data() + i);
        opaque &= (a >> 30) == 3 && (b >> 30) == 3;
        for (unsigned shift : { 0U, 10U, 20U })
        {
            const auto channel    = (a >> shift) & 1023;
            const auto reference  = (b >> shift) & 1023;
            const auto difference = unsigned(std::abs(int(channel) - int(reference)));
            nonblack += reference != 0;
            differences += difference != 0;
            maximum = std::max(maximum, difference);
            sum += difference;
        }
    }
    report.insert_or_assign("start_frame", std::int64_t(snapshot.start_frame));
    report.insert_or_assign("end_frame", std::int64_t(snapshot.end_frame));
    report.insert_or_assign("end_submission", std::int64_t(snapshot.end_submission));
    report.insert_or_assign("event_count", std::int64_t(views.size()));
    report.insert_or_assign("draw_count", std::int64_t(recipe.halves[0].size() + recipe.halves[1].size()));
    report.insert_or_assign("texture_count", std::int64_t(snapshot.textures.size()));
    report.insert_or_assign("max_rgb10_error", maximum);
    report.insert_or_assign("mean_rgb10_error", double(sum) / (1280.0 * 720.0 * 3.0));
    report.insert_or_assign("different_rgb_channels", std::int64_t(differences));
    report.insert_or_assign("nonblack_reference_channels", std::int64_t(nonblack));
    report.insert_or_assign("opaque_alpha", opaque);
    report.insert_or_assign("native_sha256", digest(native));
    report.insert_or_assign("reference_sha256", digest(swap.output_bytes));
    if (maximum > 10 || !opaque || !nonblack)
        throw std::runtime_error("live native frame exceeded the saved full-frame comparison tolerance");
    report.insert_or_assign("status", "passed");
    REXLOG_INFO("Live native menu frame passed: {} draws, maximum RGB10 error {}", recipe.halves[0].size() + recipe.halves[1].size(), maximum);
}

void NativeMenuFrameShadow::Impl::Run()
{
    const auto                mailbox = GetDrawCaptureMailbox();
    DrawCaptureMailbox::Token token;
    toml::table               report{ { "schema_version", 1 }, { "status", "failed" }, { "mode", "live_in_process_native_frame" }, { "input_boundary", "xenos_completed_frame_owned_bytes" } };
    try
    {
        std::chrono::steady_clock::time_point armedAt{};
        for (;;)
        {
            std::unique_lock lock(waitMutex);
            if (stopping)
            {
                report.insert_or_assign("status", "cancelled");
                throw std::runtime_error("game closed before the owned frame arrived");
            }
            lock.unlock();
            if (!token)
            {
                if (std::filesystem::is_regular_file(outputDirectory / "capture" / "arm"))
                {
                    token = mailbox->StartFrame();
                    if (!token)
                        throw std::runtime_error("the live capture mailbox is already reserved");
                    armedAt = std::chrono::steady_clock::now();
                    report.insert_or_assign("request_token", std::int64_t(token.value));
                }
            }
            else
            {
                if (const auto snapshot = mailbox->TryTakeFrame(token))
                {
                    Compare(*snapshot, report);
                    break;
                }
                if (!mailbox->IsPending(token))
                    throw std::runtime_error("the Xenos producer ended the frame capture without a completed snapshot");
                if (std::chrono::steady_clock::now() - armedAt > std::chrono::seconds(60))
                    throw std::runtime_error("no completed live frame arrived within 60 seconds of arming");
            }
            lock.lock();
            wake.wait_for(lock, std::chrono::milliseconds(100), [&]
                          {
                              return stopping;
                          });
        }
    }
    catch (const std::exception& exception)
    {
        report.insert_or_assign("error", exception.what());
        REXLOG_ERROR("Live native menu frame failed: {}", exception.what());
    }
    mailbox->Cancel(token);
    try
    {
        Report(report);
    }
    catch (const std::exception& exception)
    {
        REXLOG_ERROR("{}", exception.what());
    }
}

NativeMenuFrameShadow::NativeMenuFrameShadow()
: impl(std::make_unique<Impl>())
{
}

NativeMenuFrameShadow::~NativeMenuFrameShadow()
{
    Stop();
}

bool NativeMenuFrameShadow::Start(const std::filesystem::path& outputDirectory,
                                  const std::filesystem::path& shaderDirectory,
                                  std::string&                 error)
{
    std::lock_guard lock(impl->lifecycleMutex);
    if (impl->worker.joinable() || !rex::cvar::GetFlagInfo("d3d12_capture_draw") ||
        !rex::cvar::GetFlagByName("d3d12_capture_draw").empty() || !rex::cvar::GetFlagByName("d3d12_capture_frame").empty())
    {
        error = "the native frame worker and Xenos capture slot must be idle";
        return false;
    }
    if (!LoadNativeMenuShaders(shaderDirectory, impl->shaders, error))
        return false;
    try
    {
        if (!std::filesystem::create_directories(outputDirectory))
            throw std::runtime_error("native frame comparison needs a fresh output directory");
        std::filesystem::create_directory(outputDirectory / "capture");
        impl->outputDirectory = outputDirectory;
        impl->stopping        = false;
        impl->worker          = std::thread(&Impl::Run, impl.get());
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
    REXLOG_INFO("Native menu frame waiting for {}/capture/arm", outputDirectory.string());
    return true;
}

void NativeMenuFrameShadow::Stop()
{
    std::lock_guard lifecycleLock(impl->lifecycleMutex);
    if (!impl->worker.joinable())
        return;
    {
        std::lock_guard lock(impl->waitMutex);
        impl->stopping = true;
    }
    impl->wake.notify_all();
    impl->worker.join();
}

} // namespace rerevved::gpu::diagnostics

/*
 * Fixture setup adapted from ReXGlue's installed_prepared_control/control.cpp
 * at df8ec1c544c1e8f391dcfaff295ef387a1a7fea9. Original notice:
 *
 * Copyright (c) 2026, Tom Clay <tomc@tctechstuff.com>
 *
 * Portions of this software are derived from the Xenia project:
 * Copyright (c) 2022, Ben Vanik and Xenia project contributors
 * https://xenia.jp
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "native_pixel_lod_fixture.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

#include <rex/cvar.h>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/init.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/presenter.h>

namespace
{

using rex::graphics::diagnostic::DrawCaptureExecutionStatus;
using rex::graphics::diagnostic::DrawCaptureFrameInputCopyView;
using rex::graphics::diagnostic::DrawCaptureFrameInputDrawView;
using rex::graphics::diagnostic::DrawCaptureFrameInputObserver;
using rex::graphics::diagnostic::DrawCaptureFrameInputSwapView;

void TraceOriginalPhase(std::string_view phase)
{
    std::cerr << "native_pixel_lod_original: " << phase << '\n'
              << std::flush;
}

constexpr std::uint32_t kMmioBaseAddress        = 0x7FC80000U;
constexpr std::uint32_t kCpRbWptr               = 0x01C5U;
constexpr std::uint32_t kWptrAddress            = kMmioBaseAddress + kCpRbWptr * 4U;
constexpr std::uint32_t kRingSizeLog2           = 12U;
constexpr std::uint32_t kRingWordCount          = 1U << (kRingSizeLog2 + 1U);
constexpr std::uint32_t kWritebackBlockSizeLog2 = 2U;

} // namespace

namespace rex::graphics::testing
{

using namespace pixel_lod_parts;

namespace
{

struct ObservationDraw
{
    NativePixelLodDrawRecord record;
};

class OriginalObservation final : public DrawCaptureFrameInputObserver
{
public:
    explicit OriginalObservation(std::shared_ptr<DrawCaptureFrameInputObserver> forwarded)
    : forwarded_(std::move(forwarded))
    {
    }

    bool OnFrameBegin(const DrawCaptureFrameInputSwapView& boundary) override
    {
        {
            std::lock_guard lock(mutex_);
            ++begin_count_;
            ++event_ordinal_;
        }
        condition_.notify_all();
        return !forwarded_ || forwarded_->OnFrameBegin(boundary);
    }

    bool OnDraw(const DrawCaptureFrameInputDrawView& draw) override
    {
        {
            std::lock_guard lock(mutex_);
            draws_.push_back(MakeDraw(draw, ++event_ordinal_));
        }
        condition_.notify_all();
        return !forwarded_ || forwarded_->OnDraw(draw);
    }

    bool OnCopy(const DrawCaptureFrameInputCopyView& copy) override
    {
        {
            std::lock_guard lock(mutex_);
            copies_.push_back(MakeCopy(copy, ++event_ordinal_));
        }
        condition_.notify_all();
        return !forwarded_ || forwarded_->OnCopy(copy);
    }

    bool OnSwap(const DrawCaptureFrameInputSwapView& swap) override
    {
        {
            std::lock_guard lock(mutex_);
            swaps_.push_back(MakeSwap(swap, ++event_ordinal_));
        }
        condition_.notify_all();
        return !forwarded_ || forwarded_->OnSwap(swap);
    }

    bool OnFrameEnd(std::uint64_t frame, std::uint64_t submission, bool original_succeeded) override
    {
        {
            std::lock_guard lock(mutex_);
            frame_ends_.push_back({ ++event_ordinal_, frame, submission, original_succeeded });
        }
        condition_.notify_all();
        return !forwarded_ || forwarded_->OnFrameEnd(frame, submission, original_succeeded);
    }

    bool WantsPreparedDraws() const noexcept override
    {
        return true;
    }

    bool OnDrawPrepared(const DrawCaptureFrameInputDrawView& draw) override
    {
        {
            std::lock_guard lock(mutex_);
            prepared_draws_.push_back(MakeDraw(draw, 0));
        }
        condition_.notify_all();
        return !forwarded_ || !forwarded_->WantsPreparedDraws() || forwarded_->OnDrawPrepared(draw);
    }

    bool WantsPreparedCopies() const noexcept override
    {
        return true;
    }

    bool OnCopyPrepared(const DrawCaptureFrameInputCopyView& copy) override
    {
        {
            std::lock_guard lock(mutex_);
            prepared_copies_.push_back(MakeCopy(copy, 0));
        }
        condition_.notify_all();
        return !forwarded_ || !forwarded_->WantsPreparedCopies() || forwarded_->OnCopyPrepared(copy);
    }

    bool WaitForCompletion(std::uint32_t terminal_address)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(20), [&]
                                   {
                                       for (const auto& copy : copies_)
                                       {
                                           if (copy.written_address != terminal_address || copy.written_length == 0U ||
                                               !copy.original_succeeded)
                                           {
                                               continue;
                                           }
                                           for (const auto& swap : swaps_)
                                           {
                                               if (swap.frame != copy.frame || swap.submission != copy.submission ||
                                                   swap.event_ordinal <= copy.event_ordinal ||
                                                   swap.frontbuffer_ptr != terminal_address ||
                                                   swap.width != kNativePixelLodWidth || swap.height != kNativePixelLodHeight)
                                               {
                                                   continue;
                                               }
                                               if (std::any_of(frame_ends_.begin(), frame_ends_.end(), [&](const auto& end)
                                                               {
                                                                   return end.frame == swap.frame && end.submission == swap.submission &&
                                                                          end.event_ordinal > swap.event_ordinal &&
                                                                          end.original_succeeded;
                                                               }))
                                               {
                                                   completion_copy_id_    = copy.id;
                                                   completion_swap_id_    = swap.id;
                                                   completion_frame_      = swap.frame;
                                                   completion_submission_ = swap.submission;
                                                   return true;
                                               }
                                           }
                                       }
                                       return false;
                                   });
    }

    struct Snapshot
    {
        std::size_t                               begin_count = 0;
        std::vector<NativePixelLodDrawRecord>     draws;
        std::vector<NativePixelLodDrawRecord>     prepared_draws;
        std::vector<NativePixelLodCopyRecord>     copies;
        std::vector<NativePixelLodCopyRecord>     prepared_copies;
        std::vector<NativePixelLodSwapRecord>     swaps;
        std::vector<NativePixelLodFrameEndRecord> frame_ends;
        std::uint64_t                             completion_copy_id    = 0;
        std::uint64_t                             completion_swap_id    = 0;
        std::uint64_t                             completion_frame      = 0;
        std::uint64_t                             completion_submission = 0;
    };

    Snapshot SnapshotData() const
    {
        std::lock_guard lock(mutex_);
        return { begin_count_, draws_, prepared_draws_, copies_, prepared_copies_, swaps_, frame_ends_, completion_copy_id_, completion_swap_id_, completion_frame_, completion_submission_ };
    }

private:
    static NativePixelLodDrawRecord MakeDraw(const DrawCaptureFrameInputDrawView& draw,
                                             std::uint64_t                        event_ordinal)
    {
        NativePixelLodDrawRecord result;
        result.event_ordinal         = event_ordinal;
        result.id                    = draw.id;
        result.frame                 = draw.frame;
        result.submission            = draw.submission;
        result.original_host_issued  = draw.original_host_issued;
        result.cpu_data_complete     = draw.cpu_data_complete;
        result.execution_status      = static_cast<std::uint32_t>(draw.execution_status);
        result.execution_reason      = static_cast<std::uint32_t>(draw.execution_reason);
        result.primitive_type        = draw.primitive_type;
        result.requested_index_count = draw.requested_index_count;
        result.vertex_shader_hash    = draw.vertex_shader.hash;
        result.pixel_shader_hash     = draw.pixel_shader.hash;
        result.registers.assign(draw.registers.begin(), draw.registers.end());
        result.vertex_shader_words.assign(draw.vertex_shader.words.begin(), draw.vertex_shader.words.end());
        result.pixel_shader_words.assign(draw.pixel_shader.words.begin(), draw.pixel_shader.words.end());
        return result;
    }

    static NativePixelLodCopyRecord MakeCopy(const DrawCaptureFrameInputCopyView& copy,
                                             std::uint64_t                        event_ordinal)
    {
        NativePixelLodCopyRecord result;
        result.event_ordinal      = event_ordinal;
        result.id                 = copy.id;
        result.frame              = copy.frame;
        result.submission         = copy.submission;
        result.mode               = std::string(copy.copy_mode);
        result.original_succeeded = copy.original_succeeded;
        result.cpu_data_complete  = copy.cpu_data_complete;
        result.written_address    = copy.written_address;
        result.written_length     = copy.written_length;
        result.planned_address    = copy.planned_address;
        result.planned_length     = copy.planned_length;
        result.registers.assign(copy.registers.begin(), copy.registers.end());
        return result;
    }

    static NativePixelLodSwapRecord MakeSwap(const DrawCaptureFrameInputSwapView& swap,
                                             std::uint64_t                        event_ordinal)
    {
        return { event_ordinal, swap.id, swap.frame, swap.submission, swap.frontbuffer_ptr, swap.frontbuffer_width, swap.frontbuffer_height, swap.original_succeeded };
    }

    mutable std::mutex                             mutex_;
    std::condition_variable                        condition_;
    std::size_t                                    begin_count_   = 0;
    std::uint64_t                                  event_ordinal_ = 0;
    std::vector<NativePixelLodDrawRecord>          draws_;
    std::vector<NativePixelLodDrawRecord>          prepared_draws_;
    std::vector<NativePixelLodCopyRecord>          copies_;
    std::vector<NativePixelLodCopyRecord>          prepared_copies_;
    std::vector<NativePixelLodSwapRecord>          swaps_;
    std::vector<NativePixelLodFrameEndRecord>      frame_ends_;
    std::uint64_t                                  completion_copy_id_    = 0;
    std::uint64_t                                  completion_swap_id_    = 0;
    std::uint64_t                                  completion_frame_      = 0;
    std::uint64_t                                  completion_submission_ = 0;
    std::shared_ptr<DrawCaptureFrameInputObserver> forwarded_;
};

class GuestAllocation final
{
public:
    GuestAllocation() = default;

    GuestAllocation(rex::memory::Memory& memory, std::uint32_t bytes, std::uint32_t alignment)
    : memory_(&memory)
    , virtual_address_(memory.SystemHeapAlloc(
          bytes, alignment, rex::memory::kSystemHeapPhysical))
    {
        if (virtual_address_)
        {
            physical_address_ = memory.GetPhysicalAddress(virtual_address_);
        }
    }

    GuestAllocation(const GuestAllocation&)            = delete;
    GuestAllocation& operator=(const GuestAllocation&) = delete;

    GuestAllocation(GuestAllocation&& other) noexcept
    : memory_(std::exchange(other.memory_, nullptr))
    , virtual_address_(std::exchange(other.virtual_address_, 0U))
    , physical_address_(std::exchange(other.physical_address_, 0U))
    {
    }

    GuestAllocation& operator=(GuestAllocation&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            memory_           = std::exchange(other.memory_, nullptr);
            virtual_address_  = std::exchange(other.virtual_address_, 0U);
            physical_address_ = std::exchange(other.physical_address_, 0U);
        }
        return *this;
    }

    ~GuestAllocation()
    {
        Reset();
    }

    explicit operator bool() const noexcept
    {
        return memory_ && virtual_address_ && physical_address_;
    }

    std::uint32_t physical_address() const noexcept
    {
        return physical_address_;
    }

    std::uint8_t* physical_bytes() const noexcept
    {
        return memory_ ? memory_->TranslatePhysical<std::uint8_t*>(physical_address_) : nullptr;
    }

    void Reset() noexcept
    {
        if (memory_ && virtual_address_)
        {
            memory_->SystemHeapFree(virtual_address_);
        }
        memory_           = nullptr;
        virtual_address_  = 0U;
        physical_address_ = 0U;
    }

private:
    rex::memory::Memory* memory_           = nullptr;
    std::uint32_t        virtual_address_  = 0;
    std::uint32_t        physical_address_ = 0;
};

class PacketStream final
{
public:
    void Push(std::uint32_t word)
    {
        words_.push_back(word);
    }

    void Type0(std::uint32_t index, std::span<const std::uint32_t> values)
    {
        if (values.empty())
            return;
        Push(rex::graphics::xenos::MakePacketType0(static_cast<std::uint16_t>(index),
                                                   static_cast<std::uint16_t>(values.size())));
        words_.insert(words_.end(), values.begin(), values.end());
    }

    void Type0(std::uint32_t index, std::initializer_list<std::uint32_t> values)
    {
        Type0(index, std::span<const std::uint32_t>(values.begin(), values.size()));
    }

    void LoadShader(std::uint32_t address, rex::graphics::xenos::ShaderType type, std::uint32_t dword_count)
    {
        Push(rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_IM_LOAD, 2));
        Push(address | static_cast<std::uint32_t>(type));
        Push(dword_count);
    }

    void DrawDma(std::uint32_t initiator, std::uint32_t index_address, std::uint32_t index_count, rex::graphics::xenos::Endian index_endian)
    {
        Push(rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_DRAW_INDX, 4));
        Push(0U);
        Push(initiator);
        Push(index_address);
        Push(index_count | (static_cast<std::uint32_t>(index_endian) << 30U));
    }

    void DrawAuto(std::uint32_t initiator)
    {
        Push(rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_DRAW_INDX, 2));
        Push(0U);
        Push(initiator);
    }

    void Swap(std::uint32_t address, std::uint32_t width, std::uint32_t height)
    {
        Push(rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_XE_SWAP, 4));
        Push(rex::graphics::xenos::kSwapSignature);
        Push(address);
        Push(width);
        Push(height);
    }

    std::span<const std::uint32_t> words() const noexcept
    {
        return words_;
    }

private:
    std::vector<std::uint32_t> words_;
};

void WriteShaderWords(GuestAllocation& allocation, std::span<const std::uint32_t> words)
{
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        rex::memory::store_and_swap<std::uint32_t>(allocation.physical_bytes() + i * 4U, words[i]);
    }
}

void WritePacketWords(rex::memory::Memory& memory, std::uint32_t ring_address, std::span<const std::uint32_t> words)
{
    auto* ring = memory.TranslatePhysical<std::uint8_t*>(ring_address);
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        rex::memory::store_and_swap<std::uint32_t>(ring + i * 4U, words[i]);
    }
}

void PublishWritePointer(rex::memory::Memory& memory, std::uint32_t word_count)
{
    auto* pointer = memory.TranslateVirtual<volatile std::uint32_t*>(kWptrAddress);
    *pointer      = rex::byte_swap(word_count);
}

bool WaitForWriteback(rex::memory::Memory& memory, std::uint32_t address, std::uint32_t expected)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (rex::memory::load_and_swap<std::uint32_t>(memory.TranslatePhysical(address)) == expected)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

bool Require(bool value, std::string_view message, std::string& error)
{
    if (!value)
        error = std::string(message);
    return value;
}

} // namespace

namespace
{

std::array<std::uint32_t, 6> MakeOutputFetch(std::uint32_t address)
{
    rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
    fetch.type   = rex::graphics::xenos::FetchConstantType::kTexture;
    fetch.sign_x = fetch.sign_y = fetch.sign_z = fetch.sign_w =
        rex::graphics::xenos::TextureSign::kUnsigned;
    fetch.pitch          = kNativePixelLodWidth / 32U;
    fetch.format         = rex::graphics::xenos::TextureFormat::k_8_8_8_8;
    fetch.endianness     = rex::graphics::xenos::Endian::kNone;
    fetch.base_address   = address >> 12U;
    fetch.size_2d.width  = kNativePixelLodWidth - 1U;
    fetch.size_2d.height = kNativePixelLodHeight - 1U;
    fetch.tiled          = true;
    fetch.swizzle        = rex::graphics::xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
    fetch.mag_filter     = rex::graphics::xenos::TextureFilter::kPoint;
    fetch.min_filter     = rex::graphics::xenos::TextureFilter::kPoint;
    fetch.mip_filter     = rex::graphics::xenos::TextureFilter::kPoint;
    fetch.dimension      = rex::graphics::xenos::DataDimension::k2DOrStacked;
    return { fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5 };
}

std::vector<std::uint8_t> MakeResolveGeometry()
{
    std::vector<std::uint8_t> bytes;
    for (const auto value : std::array<float, 6>{ -0.5F, -0.5F, 127.5F, -0.5F, 127.5F, 127.5F })
    {
        AppendBigEndianFloat(bytes, value);
    }
    return bytes;
}

void PatchFloat(std::vector<std::uint8_t>& bytes, std::size_t offset, float value)
{
    if (offset + 4U > bytes.size())
        return;
    const auto bits    = std::bit_cast<std::uint32_t>(value);
    bytes[offset + 0U] = static_cast<std::uint8_t>(bits >> 24U);
    bytes[offset + 1U] = static_cast<std::uint8_t>(bits >> 16U);
    bytes[offset + 2U] = static_cast<std::uint8_t>(bits >> 8U);
    bytes[offset + 3U] = static_cast<std::uint8_t>(bits);
}

void PatchLods(NativePixelLodFixture& fixture, std::uint32_t first_lod, std::uint32_t second_lod)
{
    for (std::uint32_t vertex = 0; vertex < 3U; ++vertex)
    {
        const auto base = static_cast<std::size_t>(vertex) * fixture.vertex_stride;
        PatchFloat(fixture.vertex_bytes, base + 24U, static_cast<float>(first_lod));
        PatchFloat(fixture.vertex_bytes, base + 28U, static_cast<float>(second_lod));
    }
}

bool RebuildTextureGuestBytes(NativePixelLodFixture& fixture)
{
    if (fixture.texture.mips.size() != kNativePixelLodMipCount)
        return false;
    const auto layout = texture_util::GetGuestTextureLayout(
        xenos::DataDimension::k2DOrStacked, kNativePixelLodWidth / 32U, kNativePixelLodWidth, kNativePixelLodHeight, 1U, false, xenos::TextureFormat::k_8_8_8_8, false, true, kNativePixelLodMipCount - 1U);
    fixture.texture.base_guest_bytes.assign(AlignToPage(layout.base.level_data_extent_bytes), 0U);
    fixture.texture.mip_guest_bytes.assign(AlignToPage(layout.mips_total_extent_bytes), 0U);
    for (std::uint32_t level = 0; level < kNativePixelLodMipCount; ++level)
    {
        const auto& mip = fixture.texture.mips[level];
        if (mip.width == 0U || mip.height == 0U ||
            mip.linear_rgba8.size() != static_cast<std::size_t>(mip.width) * mip.height * 4U)
        {
            return false;
        }
        if (level == 0U)
        {
            WriteLinearMip(fixture.texture.base_guest_bytes, 0U, mip);
        }
        else
        {
            WriteLinearMip(fixture.texture.mip_guest_bytes, layout.mip_offsets_bytes[level], mip);
        }
    }
    return true;
}

void RebuildExpectedPlanes(NativePixelLodFixture& fixture)
{
    fixture.expected_by_lod_pair.resize(kNativePixelLodMipCount * kNativePixelLodMipCount);
    for (std::uint32_t first = 0; first < kNativePixelLodMipCount; ++first)
    {
        for (std::uint32_t second = 0; second < kNativePixelLodMipCount; ++second)
        {
            fixture.expected_by_lod_pair[first * kNativePixelLodMipCount + second] =
                MakeExpectedPixels(fixture, first, second);
        }
    }
    const auto selected_index        = fixture.selected_lods[0] * kNativePixelLodMipCount +
                                       fixture.selected_lods[1];
    fixture.expected_selected_pixels = fixture.expected_by_lod_pair[selected_index];
}

void CopyAllocation(GuestAllocation& allocation, std::span<const std::uint8_t> bytes)
{
    std::memset(allocation.physical_bytes(), 0, AlignToPage(static_cast<std::uint32_t>(bytes.size())));
    std::memcpy(allocation.physical_bytes(), bytes.data(), bytes.size());
}

} // namespace

int RunNativePixelLodOriginal(
    const NativePixelLodOriginalParams&                                              params,
    const std::shared_ptr<rex::graphics::diagnostic::DrawCaptureFrameInputObserver>& forwarded,
    NativePixelLodOriginalResult&                                                    result,
    std::string&                                                                     error)
{
    result = {};
    error.clear();
    TraceOriginalPhase("entry_reset");
    if (params.first_lod >= kNativePixelLodMipCount || params.second_lod >= kNativePixelLodMipCount)
    {
        error          = "pixel LOD must be in the eight-level fixture range";
        result.failure = error;
        return 1;
    }

    NativePixelLodFixture fixture = params.fixture;
    TraceOriginalPhase("rebuild_texture_guest_bytes_begin");
    const bool texture_guest_bytes_rebuilt = RebuildTextureGuestBytes(fixture);
    TraceOriginalPhase(texture_guest_bytes_rebuilt ? "rebuild_texture_guest_bytes_end"
                                                   : "rebuild_texture_guest_bytes_failed");
    if (!texture_guest_bytes_rebuilt)
    {
        error          = "pixel LOD fixture does not contain eight complete RGBA8 mips";
        result.failure = error;
        return 1;
    }
    PatchLods(fixture, params.first_lod, params.second_lod);
    RebuildExpectedPlanes(fixture);
    result.expected_pixels = ExpectedNativePixelLodPixels(fixture, params.first_lod, params.second_lod);
    result.source_fixture  = fixture;
    if (result.expected_pixels.empty())
    {
        error          = "pixel LOD fixture expected plane was empty";
        result.failure = error;
        return 1;
    }

    TraceOriginalPhase("load_gpu_plugin_begin");
    auto graphics = rex::system::LoadGpuPlugin("xenos", "d3d12");
    TraceOriginalPhase(graphics != nullptr ? "load_gpu_plugin_end" : "load_gpu_plugin_failed");
    if (!Require(graphics != nullptr, "xenos d3d12 plugin was not loaded", error))
    {
        result.failure = error;
        return 1;
    }
    rex::Runtime runtime(std::filesystem::path{});
    TraceOriginalPhase("runtime_setup_begin");
    const auto runtime_status = runtime.Setup(rex::RuntimeConfig{ .kernel_init = rex::kernel::InitializeKernel,
                                                                  .tool_mode   = true });
    TraceOriginalPhase("runtime_setup_end");
    if (!Require(runtime_status == static_cast<rex::X_STATUS>(0),
                 "runtime setup failed",
                 error))
    {
        result.failure = error;
        return 1;
    }
    std::vector<GuestAllocation*> guest_allocations;
    const auto                    shutdown = [&]
    {
        TraceOriginalPhase("shutdown_begin");
        graphics->Shutdown();
        TraceOriginalPhase("shutdown_graphics_end");
        for (auto* allocation : guest_allocations)
        {
            allocation->Reset();
        }
        TraceOriginalPhase("shutdown_allocations_end");
        runtime.Shutdown();
        TraceOriginalPhase("shutdown_end");
    };
    TraceOriginalPhase("gpu_setup_begin");
    if (!Require(rex::cvar::SetFlagByName("readback_resolve", "full"),
                 "readback_resolve=full was rejected",
                 error))
    {
        shutdown();
        result.failure = error;
        return 1;
    }
    if (!Require(rex::cvar::SetFlagByName("async_shader_compilation", "false"),
                 "synchronous shader compilation was rejected",
                 error))
    {
        shutdown();
        result.failure = error;
        return 1;
    }
    TraceOriginalPhase("setup_presentation_begin");
    const auto presentation_status = graphics->SetupPresentation(nullptr);
    TraceOriginalPhase("setup_presentation_end");
    if (!Require(presentation_status == static_cast<rex::X_STATUS>(0),
                 "offscreen D3D12 presenter setup failed",
                 error))
    {
        shutdown();
        result.failure = error;
        return 1;
    }
    TraceOriginalPhase("setup_guest_gpu_begin");
    const auto guest_gpu_status = graphics->SetupGuestGpu(runtime.function_dispatcher(), runtime.kernel_state());
    TraceOriginalPhase("setup_guest_gpu_end");
    if (!Require(guest_gpu_status == static_cast<rex::X_STATUS>(0),
                 "guest GPU setup failed",
                 error))
    {
        shutdown();
        result.failure = error;
        return 1;
    }
    TraceOriginalPhase("gpu_setup_end");

    auto       mailbox        = rex::graphics::diagnostic::GetDrawCaptureMailbox();
    auto       observation    = std::make_shared<OriginalObservation>(forwarded);
    const auto observer_token = mailbox->RegisterFrameInputObserver(observation);
    if (!Require(static_cast<bool>(observer_token), "mailbox observer registration failed", error))
    {
        shutdown();
        result.failure = error;
        return 1;
    }

    rex::memory::Memory& memory          = *runtime.memory();
    const auto           terminal_layout = texture_util::GetGuestTextureLayout(
        xenos::DataDimension::k2DOrStacked, kNativePixelLodWidth / 32U, kNativePixelLodWidth, kNativePixelLodHeight, 1U, true, xenos::TextureFormat::k_8_8_8_8, false, true, 0U);
    const auto terminal_extent = AlignToPage(terminal_layout.base.level_data_extent_bytes);
    TraceOriginalPhase("allocations_begin");
    GuestAllocation ring(memory, kRingWordCount * sizeof(std::uint32_t), kPageSize);
    GuestAllocation writeback(memory, kPageSize, kPageSize);
    GuestAllocation terminal(memory, terminal_extent, kPageSize);
    GuestAllocation mip_base(memory, static_cast<std::uint32_t>(fixture.texture.base_guest_bytes.size()), kPageSize);
    GuestAllocation mip_levels(memory, static_cast<std::uint32_t>(fixture.texture.mip_guest_bytes.size()), kPageSize);
    GuestAllocation vertex_bytes(memory, static_cast<std::uint32_t>(fixture.vertex_bytes.size()), kPageSize);
    GuestAllocation indices(memory, kPageSize, kPageSize);
    GuestAllocation resolve_geometry(memory, kPageSize, kPageSize);
    GuestAllocation vertex_shader(memory, static_cast<std::uint32_t>(fixture.vertex_shader_words.size() * 4U), kPageSize);
    GuestAllocation pixel_shader(memory, static_cast<std::uint32_t>(fixture.pixel_shader_words.size() * 4U), kPageSize);
    TraceOriginalPhase("allocations_end");
    guest_allocations      = { &ring, &writeback, &terminal, &mip_base, &mip_levels, &vertex_bytes, &indices, &resolve_geometry, &vertex_shader, &pixel_shader };
    const bool allocations = ring && writeback && terminal && mip_base && mip_levels && vertex_bytes &&
                             indices && resolve_geometry && vertex_shader && pixel_shader;
    if (!Require(allocations, "guest physical test allocations failed", error))
    {
        mailbox->UnregisterFrameInputObserver(observer_token);
        shutdown();
        result.failure = error;
        return 1;
    }
    result.source_index_guest_base = indices.physical_address();

    CopyAllocation(mip_base, fixture.texture.base_guest_bytes);
    CopyAllocation(mip_levels, fixture.texture.mip_guest_bytes);
    CopyAllocation(vertex_bytes, fixture.vertex_bytes);
    CopyAllocation(indices, fixture.index_bytes);
    const auto resolve_bytes = MakeResolveGeometry();
    CopyAllocation(resolve_geometry, resolve_bytes);
    std::memset(terminal.physical_bytes(), 0, terminal_extent);
    WriteShaderWords(vertex_shader, fixture.vertex_shader_words);
    WriteShaderWords(pixel_shader, fixture.pixel_shader_words);
    graphics->InitializeRingBuffer(ring.physical_address(), kRingSizeLog2);
    graphics->EnableReadPointerWriteBack(writeback.physical_address(), kWritebackBlockSizeLog2);

    const auto terminal_texture       = MakeOutputFetch(terminal.physical_address());
    const auto source_texture         = MakeTextureFetch(mip_base.physical_address(), mip_levels.physical_address());
    const auto source_vertex_fetch    = MakeVertexFetch(vertex_bytes.physical_address(),
                                                        static_cast<std::uint32_t>(fixture.vertex_bytes.size()));
    const auto resolve_fetch          = MakeVertexFetch(resolve_geometry.physical_address(),
                                                        static_cast<std::uint32_t>(resolve_bytes.size()));
    result.source_texture_fetch_words = source_texture;
    result.source_vertex_fetch_words  = source_vertex_fetch;
    TraceOriginalPhase("command_stream_begin");
    PacketStream stream;
    stream.Type0(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, terminal_texture);
    stream.Swap(terminal.physical_address(), kNativePixelLodWidth, kNativePixelLodHeight);
    stream.LoadShader(vertex_shader.physical_address(), xenos::ShaderType::kVertex, static_cast<std::uint32_t>(fixture.vertex_shader_words.size()));
    stream.LoadShader(pixel_shader.physical_address(), xenos::ShaderType::kPixel, static_cast<std::uint32_t>(fixture.pixel_shader_words.size()));
    stream.Type0(0x2000U, { 0x02000080U, 0U, 0x530U });
    stream.Type0(0x200FU, { 0x20002000U });
    stream.Type0(0x2080U, { 0U, 0U });
    stream.Type0(0x2082U, { kNativePixelLodWidth | (kNativePixelLodHeight << 16U) });
    stream.Type0(0x2100U, { 2U });
    stream.Type0(0x2104U, { 0xFU });
    stream.Type0(0x210FU, { std::bit_cast<std::uint32_t>(64.0F), std::bit_cast<std::uint32_t>(64.0F), std::bit_cast<std::uint32_t>(-64.0F), std::bit_cast<std::uint32_t>(64.0F) });
    stream.Type0(0x2180U, { 0x10010002U });
    stream.Type0(0x2181U, { 4U });
    stream.Type0(0x2182U, { 1U });
    stream.Type0(0x2200U, { 0x24F00730U });
    stream.Type0(0x2201U, { 0x00010001U });
    stream.Type0(0x2202U, { 0x87000000U });
    stream.Type0(0x2204U, { 0x80000U });
    stream.Type0(0x2205U, { 0x18000U });
    stream.Type0(0x2206U, { 0x43FU });
    stream.Type0(0x2208U, { 4U });
    stream.Type0(0x2302U, { 4U });
    stream.Type0(0x2307U, { 0xFF000U });
    stream.Type0(0x2308U, { 0xFF100U });
    stream.Type0(XE_GPU_REG_SHADER_CONSTANT_000_X, kVertexConstants);
    stream.Type0(XE_GPU_REG_SHADER_CONSTANT_256_X, kPixelConstant0);
    stream.Type0(XE_GPU_REG_SHADER_CONSTANT_256_X + 255U * 4U, kPixelConstant255);
    stream.Type0(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, source_texture);
    stream.Type0(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2U * 95U, source_vertex_fetch);
    stream.Type0(0x21FCU, { kNativePixelLodDrawInitiator });
    stream.DrawDma(kNativePixelLodDrawInitiator,
                   indices.physical_address(),
                   kNativePixelLodIndexCount,
                   kNativePixelLodIndexEndian);
    stream.Type0(0x2000U, { 0x02000080U, 0U, 0x530U });
    stream.Type0(0x200FU, { 0x20002000U });
    stream.Type0(0x2080U, { 0U, 0U, kNativePixelLodWidth | (kNativePixelLodHeight << 16U) });
    stream.Type0(0x2205U, { 0x10000U });
    stream.Type0(0x2208U, { 6U });
    stream.Type0(0x2318U, { 0x00100000U });
    stream.Type0(0x2319U, { terminal.physical_address() });
    stream.Type0(0x231AU, { kNativePixelLodWidth | (kNativePixelLodHeight << 16U) });
    stream.Type0(0x231BU, { 0x00000300U });
    stream.Type0(0x231CU, { 0xFFFU });
    stream.Type0(0x231DU, { 0xFFFFFF00U });
    stream.Type0(0x231EU, { 0U, 0U });
    stream.Type0(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, resolve_fetch);
    stream.Type0(0x21FCU, { static_cast<std::uint32_t>(xenos::PrimitiveType::kRectangleList) | (static_cast<std::uint32_t>(xenos::SourceSelect::kAutoIndex) << 6U) | (3U << 16U) });
    stream.DrawAuto(0x00030088U);
    stream.Type0(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, terminal_texture);
    stream.Swap(terminal.physical_address(), kNativePixelLodWidth, kNativePixelLodHeight);
    if (!Require(stream.words().size() < kRingWordCount, "PM4 stream exceeds ring capacity", error))
    {
        mailbox->UnregisterFrameInputObserver(observer_token);
        shutdown();
        result.failure = error;
        return 1;
    }
    TraceOriginalPhase("command_stream_end");
    result.published_word_count = static_cast<std::uint32_t>(stream.words().size());
    TraceOriginalPhase("publish_begin");
    WritePacketWords(memory, ring.physical_address(), stream.words());
    PublishWritePointer(memory, result.published_word_count);
    TraceOriginalPhase("publish_end");
    TraceOriginalPhase("wait_writeback_begin");
    result.progressed = WaitForWriteback(memory, writeback.physical_address(), result.published_word_count);
    TraceOriginalPhase(result.progressed ? "wait_writeback_end" : "wait_writeback_failed");
    TraceOriginalPhase("wait_completion_begin");
    result.completion_observed = result.progressed && observation->WaitForCompletion(terminal.physical_address());
    TraceOriginalPhase(result.completion_observed ? "wait_completion_end" : "wait_completion_failed");
    rex::ui::RawImage image;
    result.terminal_capture = result.completion_observed && graphics->presenter() &&
                              graphics->presenter()->CaptureGuestOutput(image);
    if (result.terminal_capture)
    {
        result.terminal_width  = image.width;
        result.terminal_height = image.height;
        result.terminal_stride = image.stride;
        result.terminal_pixels = image.data;
    }
    result.terminal_pixels_match_expected = result.terminal_capture &&
                                            result.terminal_pixels == result.expected_pixels;
    result.actual_readback_owner          = result.completion_observed && result.terminal_capture;
    result.readback_owner                 = result.actual_readback_owner
                                                ? "presenter CaptureGuestOutput after matched completed swap"
                                                : "none";
    mailbox->UnregisterFrameInputObserver(observer_token);
    const auto snapshot          = observation->SnapshotData();
    result.draws                 = snapshot.draws;
    result.prepared_draws        = snapshot.prepared_draws;
    result.copies                = snapshot.copies;
    result.prepared_copies       = snapshot.prepared_copies;
    result.swaps                 = snapshot.swaps;
    result.frame_ends            = snapshot.frame_ends;
    result.completion_copy_id    = snapshot.completion_copy_id;
    result.completion_swap_id    = snapshot.completion_swap_id;
    result.completion_frame      = snapshot.completion_frame;
    result.completion_submission = snapshot.completion_submission;
    for (const auto& draw : result.draws)
    {
        if (draw.vertex_shader_words == fixture.vertex_shader_words &&
            draw.pixel_shader_words == fixture.pixel_shader_words)
        {
            result.source_registers = draw.registers;
            result.source_draw_issued =
                draw.original_host_issued &&
                draw.execution_status == static_cast<std::uint32_t>(DrawCaptureExecutionStatus::kIssued);
            if (result.source_registers.size() >=
                XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2U * 95U + 6U)
            {
                std::copy_n(result.source_registers.begin() + XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, 6, result.source_texture_fetch_words.begin());
                std::copy_n(result.source_registers.begin() + XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2U * 95U,
                            6,
                            result.source_vertex_fetch_words.begin());
            }
            break;
        }
    }
    if (result.source_draw_issued && result.source_registers.size() == kNativePixelLodRegisterCount)
    {
        result.source_fixture.register_image      = result.source_registers;
        result.source_fixture.texture.fetch_words = result.source_texture_fetch_words;
        result.source_fixture.vertex_fetch_words  = result.source_vertex_fetch_words;
    }
    const bool complete = result.progressed && result.completion_observed && result.terminal_capture &&
                          result.actual_readback_owner && result.source_draw_issued &&
                          result.terminal_pixels_match_expected &&
                          result.terminal_width == kNativePixelLodWidth &&
                          result.terminal_height == kNativePixelLodHeight &&
                          result.terminal_stride == kNativePixelLodWidth * 4U;
    if (!complete)
    {
        error          = "original pixel LOD GPU completion or expected pixels were incomplete";
        result.failure = error;
    }
    shutdown();
    return complete ? 0 : 1;
}

} // namespace rex::graphics::testing

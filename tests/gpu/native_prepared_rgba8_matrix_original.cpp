// Parameterized from native_prepared_bc2_rect_original.cpp. The matrix shares
// its PM4 producer, callbacks, overlapping writes and copy retirement path.
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

#include "native_prepared_rgba8_matrix_fixture.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
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

#if defined(_WIN32)
#include <windows.h>
#endif

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

using namespace rex::graphics::testing::rgba8_matrix_detail;
using rex::graphics::diagnostic::DrawCaptureExecutionStatus;
using rex::graphics::diagnostic::DrawCaptureFrameInputCopyView;
using rex::graphics::diagnostic::DrawCaptureFrameInputDrawView;
using rex::graphics::diagnostic::DrawCaptureFrameInputObserver;
using rex::graphics::diagnostic::DrawCaptureFrameInputSwapView;

void TraceOriginalPhase(std::string_view phase)
{
    std::cerr << "native_prepared_rgba8_matrix_original: " << phase << '\n'
              << std::flush;
}

constexpr std::uint32_t kMmioBaseAddress        = 0x7FC80000U;
constexpr std::uint32_t kCpRbWptr               = 0x01C5U;
constexpr std::uint32_t kWptrAddress            = kMmioBaseAddress + kCpRbWptr * 4U;
constexpr std::uint32_t kRingSizeLog2           = 12U;
constexpr std::uint32_t kRingWordCount          = 1U << (kRingSizeLog2 + 1U);
constexpr std::uint32_t kWritebackBlockSizeLog2 = 2U;

std::string LoadedModulePath(const wchar_t* module_name)
{
#if defined(_WIN32)
    const auto module = GetModuleHandleW(module_name);
    if (!module)
        return {};
    std::array<wchar_t, 32768> path{};
    const auto                 length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0U || length >= path.size())
        return {};
    return std::filesystem::path(std::wstring(path.data(), length)).string();
#else
    (void)module_name;
    return {};
#endif
}

} // namespace

namespace rex::graphics::testing
{

namespace
{

struct ObservationDraw
{
    NativePreparedRgba8MatrixDrawRecord record;
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
            prepared_draws_.push_back(MakeDraw(draw, ++event_ordinal_));
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
            prepared_copies_.push_back(MakeCopy(copy, ++event_ordinal_));
        }
        condition_.notify_all();
        return !forwarded_ || !forwarded_->WantsPreparedCopies() || forwarded_->OnCopyPrepared(copy);
    }

    bool WantsPreparedSwaps() const noexcept override
    {
        return true;
    }

    bool OnSwapPrepared(const DrawCaptureFrameInputSwapView& swap) override
    {
        {
            std::lock_guard lock(mutex_);
            prepared_swaps_.push_back(MakeSwap(swap, ++event_ordinal_));
        }
        condition_.notify_all();
        return !forwarded_ || !forwarded_->WantsPreparedSwaps() || forwarded_->OnSwapPrepared(swap);
    }

    bool WaitForCompletion(std::uint32_t terminal_address, std::uint64_t after_event_ordinal = 0)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(20), [&]
                                   {
                                       for (const auto& copy : copies_)
                                       {
                                           if (copy.written_address != terminal_address || copy.written_length == 0U ||
                                               !copy.original_succeeded || copy.event_ordinal <= after_event_ordinal)
                                           {
                                               continue;
                                           }
                                           for (const auto& swap : swaps_)
                                           {
                                               if (swap.frame != copy.frame || swap.submission != copy.submission ||
                                                   swap.event_ordinal <= copy.event_ordinal || swap.event_ordinal <= after_event_ordinal ||
                                                   swap.frontbuffer_ptr != terminal_address ||
                                                   swap.width != kNativePreparedRgba8MatrixOutputWidth ||
                                                   swap.height != kNativePreparedRgba8MatrixOutputHeight)
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
        std::size_t                                          begin_count = 0;
        std::vector<NativePreparedRgba8MatrixDrawRecord>     draws;
        std::vector<NativePreparedRgba8MatrixDrawRecord>     prepared_draws;
        std::vector<NativePreparedRgba8MatrixCopyRecord>     copies;
        std::vector<NativePreparedRgba8MatrixCopyRecord>     prepared_copies;
        std::vector<NativePreparedRgba8MatrixSwapRecord>     swaps;
        std::vector<NativePreparedRgba8MatrixSwapRecord>     prepared_swaps;
        std::vector<NativePreparedRgba8MatrixFrameEndRecord> frame_ends;
        std::uint64_t                                        completion_copy_id    = 0;
        std::uint64_t                                        completion_swap_id    = 0;
        std::uint64_t                                        completion_frame      = 0;
        std::uint64_t                                        completion_submission = 0;
    };

    Snapshot SnapshotData() const
    {
        std::lock_guard lock(mutex_);
        return { begin_count_, draws_, prepared_draws_, copies_, prepared_copies_, swaps_, prepared_swaps_, frame_ends_, completion_copy_id_, completion_swap_id_, completion_frame_, completion_submission_ };
    }

private:
    static NativePreparedRgba8MatrixDrawRecord MakeDraw(const DrawCaptureFrameInputDrawView& draw,
                                                        std::uint64_t                        event_ordinal)
    {
        NativePreparedRgba8MatrixDrawRecord result;
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
        for (const auto& texture : draw.textures)
        {
            NativePreparedRgba8MatrixDrawRecord::TextureRecord retained;
            retained.fetch_constant       = texture.fetch_constant;
            retained.format               = texture.format;
            retained.endianness           = texture.endianness;
            retained.dimension            = texture.dimension;
            retained.width                = texture.width;
            retained.height               = texture.height;
            retained.depth                = texture.depth;
            retained.pitch                = texture.pitch;
            retained.mip_min_level        = texture.mip_min_level;
            retained.mip_max_level        = texture.mip_max_level;
            retained.tiled                = texture.tiled;
            retained.packed_mips          = texture.packed_mips;
            retained.stage                = texture.stage;
            retained.shader_binding_index = texture.shader_binding_index;
            retained.sampler_value        = texture.sampler_value;
            retained.component_mapping    = texture.component_mapping;
            retained.swizzled_signs       = texture.swizzled_signs;
            retained.is_signed            = texture.is_signed;
            retained.required             = texture.required;
            retained.available            = texture.available;
            retained.gpu_written          = texture.gpu_written;
            retained.unavailable_reason   = static_cast<std::uint32_t>(texture.unavailable_reason);
            retained.base_address         = texture.base_address;
            retained.base_size            = texture.base_size;
            retained.mip_address          = texture.mip_address;
            retained.mip_size             = texture.mip_size;
            retained.fetch_words.assign(texture.fetch_words.begin(), texture.fetch_words.end());
            retained.memory_watch_supplied   = texture.memory_version.identity || texture.memory_version.generation ||
                                               texture.memory_version.context || texture.memory_version.check;
            retained.memory_watch_identity   = texture.memory_version.identity;
            retained.memory_watch_generation = texture.memory_version.generation;
            retained.memory_current_before   = texture.memory_version.IsCurrent();
            // Retain optional companion metadata without copying an unused
            // payload. Only a required, available CPU view owns raw ranges.
            if (retained.required && retained.available && !retained.gpu_written && retained.unavailable_reason == 0U)
            {
                retained.base_bytes.resize(texture.base_size);
                retained.mip_bytes.resize(texture.mip_size);
                retained.base_bytes_copied = texture.base_size == 0U ||
                                             texture.memory.Copy(texture.base_address, retained.base_bytes);
                retained.mip_bytes_copied  = texture.mip_size == 0U ||
                                             texture.memory.Copy(texture.mip_address, retained.mip_bytes);
            }
            retained.memory_current_after = texture.memory_version.IsCurrent();
            retained.memory_watch_stale   = retained.memory_watch_supplied &&
                                            (!retained.memory_current_before || !retained.memory_current_after);
            result.textures.push_back(std::move(retained));
        }
        return result;
    }

    static NativePreparedRgba8MatrixCopyRecord MakeCopy(const DrawCaptureFrameInputCopyView& copy,
                                                        std::uint64_t                        event_ordinal)
    {
        NativePreparedRgba8MatrixCopyRecord result;
        result.event_ordinal                  = event_ordinal;
        result.id                             = copy.id;
        result.frame                          = copy.frame;
        result.submission                     = copy.submission;
        result.mode                           = std::string(copy.copy_mode);
        result.original_succeeded             = copy.original_succeeded;
        result.cpu_data_complete              = copy.cpu_data_complete;
        result.unavailable_reason             = static_cast<std::uint32_t>(copy.unavailable_reason);
        result.source_rect_present            = copy.source_rect_present;
        result.source_x                       = copy.source_x;
        result.source_y                       = copy.source_y;
        result.source_width                   = copy.source_width;
        result.source_height                  = copy.source_height;
        result.source_geometry_fetch_constant = copy.source_geometry.fetch_constant;
        result.source_geometry_address        = copy.source_geometry.physical_address;
        result.source_geometry_size           = copy.source_geometry.size;
        result.written_address                = copy.written_address;
        result.written_length                 = copy.written_length;
        result.planned_address                = copy.planned_address;
        result.planned_length                 = copy.planned_length;
        result.registers.assign(copy.registers.begin(), copy.registers.end());
        return result;
    }

    static NativePreparedRgba8MatrixSwapRecord MakeSwap(const DrawCaptureFrameInputSwapView& swap,
                                                        std::uint64_t                        event_ordinal)
    {
        return { event_ordinal, swap.id, swap.frame, swap.submission, swap.frontbuffer_ptr, swap.frontbuffer_width, swap.frontbuffer_height, swap.original_succeeded };
    }

    mutable std::mutex                                   mutex_;
    std::condition_variable                              condition_;
    std::size_t                                          begin_count_   = 0;
    std::uint64_t                                        event_ordinal_ = 0;
    std::vector<NativePreparedRgba8MatrixDrawRecord>     draws_;
    std::vector<NativePreparedRgba8MatrixDrawRecord>     prepared_draws_;
    std::vector<NativePreparedRgba8MatrixCopyRecord>     copies_;
    std::vector<NativePreparedRgba8MatrixCopyRecord>     prepared_copies_;
    std::vector<NativePreparedRgba8MatrixSwapRecord>     swaps_;
    std::vector<NativePreparedRgba8MatrixSwapRecord>     prepared_swaps_;
    std::vector<NativePreparedRgba8MatrixFrameEndRecord> frame_ends_;
    std::uint64_t                                        completion_copy_id_    = 0;
    std::uint64_t                                        completion_swap_id_    = 0;
    std::uint64_t                                        completion_frame_      = 0;
    std::uint64_t                                        completion_submission_ = 0;
    std::shared_ptr<DrawCaptureFrameInputObserver>       forwarded_;
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

    std::uint8_t* virtual_bytes() const noexcept
    {
        return memory_ ? memory_->TranslateVirtual<std::uint8_t*>(virtual_address_) : nullptr;
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
    fetch.pitch          = kNativePreparedRgba8MatrixOutputWidth / 32U;
    fetch.format         = rex::graphics::xenos::TextureFormat::k_8_8_8_8;
    fetch.endianness     = rex::graphics::xenos::Endian::kNone;
    fetch.base_address   = address >> 12U;
    fetch.size_2d.width  = kNativePreparedRgba8MatrixOutputWidth - 1U;
    fetch.size_2d.height = kNativePreparedRgba8MatrixOutputHeight - 1U;
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
    for (const auto value : std::array<float, 6>{ -0.5F,
                                                  -0.5F,
                                                  static_cast<float>(kNativePreparedRgba8MatrixOutputWidth) - 0.5F,
                                                  -0.5F,
                                                  static_cast<float>(kNativePreparedRgba8MatrixOutputWidth) - 0.5F,
                                                  static_cast<float>(kNativePreparedRgba8MatrixOutputHeight) - 0.5F })
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

void PatchLods(NativePreparedRgba8MatrixFixture& fixture, std::uint32_t first_lod, std::uint32_t second_lod)
{
    for (std::uint32_t vertex = 0; vertex < 3U; ++vertex)
    {
        const auto base = static_cast<std::size_t>(vertex) * fixture.vertex_stride;
        PatchFloat(fixture.vertex_bytes, base + 24U, static_cast<float>(first_lod));
        PatchFloat(fixture.vertex_bytes, base + 28U, static_cast<float>(second_lod));
    }
}

bool RebuildTextureGuestBytes(NativePreparedRgba8MatrixFixture& fixture)
{
    if (fixture.texture.mips.size() != kNativePreparedRgba8MatrixMipCount ||
        fixture.texture.base_guest_bytes.size() < kNativePreparedRgba8MatrixBaseExtent ||
        fixture.texture.mip_guest_bytes.size() < kNativePreparedRgba8MatrixMipExtent)
        return false;
    const auto layout = texture_util::GetGuestTextureLayout(
        xenos::DataDimension::k2DOrStacked, rgba8MatrixForm->pitch, kNativePreparedRgba8MatrixWidth, kNativePreparedRgba8MatrixHeight, 1U, rgba8MatrixForm->tiled, Rgba8MatrixFormat(), rgba8MatrixForm->packed, true, kNativePreparedRgba8MatrixMipCount - 1U);
    fixture.texture.base_guest_bytes.assign(AlignToPage(layout.base.level_data_extent_bytes), 0xCDU);
    fixture.texture.mip_guest_bytes.assign(std::max(kPageSize, AlignToPage(layout.mips_total_extent_bytes)), 0xCDU);
    for (std::uint32_t level = 0; level < kNativePreparedRgba8MatrixMipCount; ++level)
    {
        const auto& mip = fixture.texture.mips[level];
        if (mip.width == 0U || mip.height == 0U ||
            mip.linear_rgba8.size() != static_cast<std::size_t>(mip.width) * mip.height * 4U)
        {
            return false;
        }
        if (level == 0U)
            WriteMipStorage(fixture.texture.base_guest_bytes, layout, level, mip);
        else
            WriteMipStorage(fixture.texture.mip_guest_bytes, layout, level, mip);
    }
    return true;
}

void RebuildExpectedPlanes(NativePreparedRgba8MatrixFixture& fixture)
{
    fixture.expected_by_lod_pair.resize(kNativePreparedRgba8MatrixMipCount * kNativePreparedRgba8MatrixMipCount);
    const auto selected_index                    = fixture.selected_lods[0] * kNativePreparedRgba8MatrixMipCount + fixture.selected_lods[1];
    fixture.expected_by_lod_pair[selected_index] = MakeExpectedPixels(fixture, fixture.selected_lods[0], fixture.selected_lods[1]);
    fixture.expected_selected_pixels             = fixture.expected_by_lod_pair[selected_index];
}

void ApplyCpuMipUpdate(NativePreparedRgba8MatrixFixture&                           fixture,
                       std::uint32_t                                               level,
                       rex::graphics::testing::NativePreparedRgba8MatrixUpdateKind update_kind)
{
    if (level >= fixture.texture.mips.size())
        return;
    // Zero the selected level only; every channel changes without saturation.
    auto& bytes = fixture.texture.mips[level].linear_rgba8;
    std::fill(bytes.begin(), bytes.end(), 0U);
    (void)update_kind;
}

void CopyAllocation(GuestAllocation& allocation, std::span<const std::uint8_t> bytes)
{
    std::memset(allocation.virtual_bytes(), 0, AlignToPage(static_cast<std::uint32_t>(bytes.size())));
    std::memcpy(allocation.virtual_bytes(), bytes.data(), bytes.size());
}

void CopyAllocationWithOverlap(GuestAllocation& allocation, std::span<const std::uint8_t> bytes, std::uint32_t overlap_anchor)
{
    // Model the producer's partial CPU writes while retaining one live guest
    // allocation. The second copy intentionally overlaps the preceding range.
    std::memset(allocation.virtual_bytes(), 0, AlignToPage(static_cast<std::uint32_t>(bytes.size())));
    if (bytes.empty())
        return;
    const auto anchor = std::min<std::size_t>(overlap_anchor, bytes.size());
    const auto first  = std::min<std::size_t>(bytes.size(), anchor + 8U);
    std::memcpy(allocation.virtual_bytes(), bytes.data(), first);
    const auto overlap       = anchor >= 8U ? anchor - 8U : 0U;
    const auto overlapLength = std::min<std::size_t>(bytes.size() - overlap, 16U);
    std::memcpy(allocation.virtual_bytes() + overlap, bytes.data() + overlap, overlapLength);
    if (overlap + overlapLength < bytes.size())
        std::memcpy(allocation.virtual_bytes() + overlap + overlapLength,
                    bytes.data() + overlap + overlapLength,
                    bytes.size() - overlap - overlapLength);
}

bool CopyChangedRgba8RangeWithOverlap(GuestAllocation&              allocation,
                                      std::span<const std::uint8_t> old_bytes,
                                      std::span<const std::uint8_t> new_bytes,
                                      std::uint32_t                 offset,
                                      std::uint32_t                 length,
                                      bool&                         outside_unchanged,
                                      bool&                         overlap_written)
{
    outside_unchanged = false;
    overlap_written   = false;
    if (old_bytes.size() != new_bytes.size() ||
        static_cast<std::size_t>(offset) + length > old_bytes.size())
        return false;
    if (std::memcmp(allocation.physical_bytes(), old_bytes.data(), old_bytes.size()) != 0)
        return false;
    bool changed = false;
    for (std::size_t index = 0; index < old_bytes.size(); ++index)
    {
        if (index >= offset && index < static_cast<std::size_t>(offset) + length &&
            old_bytes[index] != new_bytes[index])
            changed = true;
        if ((index < offset || index >= static_cast<std::size_t>(offset) + length) &&
            old_bytes[index] != new_bytes[index])
            return false;
    }
    if (!changed || length == 0U)
        return false;

    // CPU writes must use the guest virtual alias. The physical view is the
    // SDK data-provider path and bypasses the invalidation watches that make
    // an admitted texture version stale after a real CPU rewrite.
    auto*      destination  = allocation.virtual_bytes();
    const auto begin        = static_cast<std::size_t>(offset);
    const auto end          = begin + length;
    const auto first_length = std::min<std::size_t>(8U, length);
    std::memcpy(destination + begin, new_bytes.data() + begin, first_length);
    const auto overlap_begin  = begin + std::min<std::size_t>(4U, length - 1U);
    const auto overlap_length = std::min<std::size_t>(8U, end - overlap_begin);
    std::memcpy(destination + overlap_begin, new_bytes.data() + overlap_begin, overlap_length);
    overlap_written = overlap_begin < begin + first_length && overlap_length != 0U;
    if (overlap_begin + overlap_length < end)
        std::memcpy(destination + overlap_begin + overlap_length,
                    new_bytes.data() + overlap_begin + overlap_length,
                    end - overlap_begin - overlap_length);

    outside_unchanged = true;
    for (std::size_t index = 0; index < old_bytes.size(); ++index)
    {
        if (index < begin || index >= end)
            outside_unchanged = outside_unchanged && destination[index] == old_bytes[index];
    }
    return std::memcmp(destination, new_bytes.data(), new_bytes.size()) == 0;
}

} // namespace

int RunNativePreparedRgba8MatrixOriginal(
    NativePreparedRgba8MatrixOriginalParams&                                         params,
    const std::shared_ptr<rex::graphics::diagnostic::DrawCaptureFrameInputObserver>& forwarded,
    NativePreparedRgba8MatrixOriginalResult&                                         result,
    std::string&                                                                     error)
{
    result                = {};
    result.source_fixture = std::move(params.fixture);
    error.clear();
    result.cpu_update_level = params.first_lod;
    result.update_kind      = params.update_kind;
    TraceOriginalPhase("entry_reset");
    if (params.first_lod >= kNativePreparedRgba8MatrixMipCount || params.second_lod >= kNativePreparedRgba8MatrixMipCount)
    {
        error          = "selected LOD exceeds the effective matrix range";
        result.failure = error;
        return 1;
    }

    auto& fixture = result.source_fixture;
    TraceOriginalPhase("rebuild_texture_guest_bytes_begin");
    const bool texture_guest_bytes_rebuilt = RebuildTextureGuestBytes(fixture);
    TraceOriginalPhase(texture_guest_bytes_rebuilt ? "rebuild_texture_guest_bytes_end"
                                                   : "rebuild_texture_guest_bytes_failed");
    if (!texture_guest_bytes_rebuilt)
    {
        error          = "fixture does not contain every effective raw level";
        result.failure = error;
        return 1;
    }
    PatchLods(fixture, params.first_lod, params.second_lod);
    RebuildExpectedPlanes(fixture);
    result.expected_pixels = ExpectedNativePreparedRgba8MatrixPixels(fixture, params.first_lod, params.second_lod);
    if (result.expected_pixels.empty())
    {
        error          = "prepared RGBA8 fixture expected plane was empty";
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
    result.gpu_module_path     = LoadedModulePath(L"rexgpu-xenos.dll");
    result.runtime_module_path = LoadedModulePath(L"rexruntime.dll");
    if (!Require(!result.gpu_module_path.empty() && !result.runtime_module_path.empty(),
                 "actual rexruntime.dll and rexgpu-xenos.dll module identities were not loaded",
                 error))
    {
        result.failure = error;
        graphics->Shutdown();
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
    // PreparedCopy admits source resolves; resolve_readback copies are
    // rejected.
    if (!Require(rex::cvar::SetFlagByName("readback_resolve", "none"),
                 "readback_resolve=none was rejected",
                 error))
    {
        shutdown();
        result.failure = error;
        return 1;
    }
    // The oracle models pix_center D3D with the SDK half-pixel offset applied.
    if (!Require(rex::cvar::SetFlagByName("half_pixel_offset", "true"), "half_pixel_offset=true was rejected", error))
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

    const auto freezeObservation = [&]
    {
        const auto snapshot          = observation->SnapshotData();
        result.draws                 = snapshot.draws;
        result.prepared_draws        = snapshot.prepared_draws;
        result.copies                = snapshot.copies;
        result.prepared_copies       = snapshot.prepared_copies;
        result.swaps                 = snapshot.swaps;
        result.prepared_swaps        = snapshot.prepared_swaps;
        result.frame_ends            = snapshot.frame_ends;
        result.completion_copy_id    = snapshot.completion_copy_id;
        result.completion_swap_id    = snapshot.completion_swap_id;
        result.completion_frame      = snapshot.completion_frame;
        result.completion_submission = snapshot.completion_submission;
    };
    const auto unregisterFrameObserver = [&]
    {
        freezeObservation();
        mailbox->UnregisterFrameInputObserver(observer_token);
    };

    rex::memory::Memory& memory          = *runtime.memory();
    const auto           terminal_layout = texture_util::GetGuestTextureLayout(
        xenos::DataDimension::k2DOrStacked, kNativePreparedRgba8MatrixOutputWidth / 32U, kNativePreparedRgba8MatrixOutputWidth, kNativePreparedRgba8MatrixOutputHeight, 1U, true, xenos::TextureFormat::k_8_8_8_8, false, true, 0U);
    const auto terminal_extent = AlignToPage(terminal_layout.base.level_data_extent_bytes);
    TraceOriginalPhase("allocations_begin");
    GuestAllocation ring(memory, kRingWordCount * sizeof(std::uint32_t), kPageSize);
    GuestAllocation writeback(memory, kPageSize, kPageSize);
    // Prepared resolve plans retain their destination ranges. C needs four
    // disjoint destinations so later submissions never re-arm an earlier plan.
    const auto      terminal_count = rgba8MatrixForm->region_updates ? 4U : 1U;
    GuestAllocation terminal(memory, terminal_extent * terminal_count, kPageSize);
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
        unregisterFrameObserver();
        shutdown();
        result.failure = error;
        return 1;
    }
    result.source_index_guest_base = indices.physical_address();

    result.old_base_bytes           = fixture.texture.base_guest_bytes;
    result.old_mip_bytes            = fixture.texture.mip_guest_bytes;
    result.baseline_expected_pixels = result.expected_pixels;
    CopyAllocationWithOverlap(mip_base, fixture.texture.base_guest_bytes, 0x400U);
    CopyAllocationWithOverlap(mip_levels, fixture.texture.mip_guest_bytes, 0x2000U);
    CopyAllocationWithOverlap(mip_levels, fixture.texture.mip_guest_bytes, kNativePreparedRgba8MatrixTailOffset);
    result.cpu_partial_overlap_writes = true;
    result.source_raw_bytes_frozen =
        std::memcmp(mip_base.physical_bytes(), fixture.texture.base_guest_bytes.data(), fixture.texture.base_guest_bytes.size()) == 0 &&
        std::memcmp(mip_levels.physical_bytes(), fixture.texture.mip_guest_bytes.data(), fixture.texture.mip_guest_bytes.size()) == 0;
    if (!Require(result.source_raw_bytes_frozen, "CPU guest writes did not preserve the complete raw RGBA8 bytes", error))
    {
        unregisterFrameObserver();
        shutdown();
        result.failure = error;
        return 1;
    }
    CopyAllocation(vertex_bytes, fixture.vertex_bytes);
    CopyAllocation(indices, fixture.index_bytes);
    const auto resolve_bytes = MakeResolveGeometry();
    CopyAllocation(resolve_geometry, resolve_bytes);
    std::memset(terminal.physical_bytes(), 0, terminal_extent * terminal_count);
    WriteShaderWords(vertex_shader, fixture.vertex_shader_words);
    WriteShaderWords(pixel_shader, fixture.pixel_shader_words);
    graphics->InitializeRingBuffer(ring.physical_address(), kRingSizeLog2);
    graphics->EnableReadPointerWriteBack(writeback.physical_address(), kWritebackBlockSizeLog2);

    const auto source_texture         = MakeTextureFetch(mip_base.physical_address(), mip_levels.physical_address());
    const auto source_vertex_fetch    = MakeVertexFetch(vertex_bytes.physical_address(),
                                                        static_cast<std::uint32_t>(fixture.vertex_bytes.size()));
    const auto resolve_fetch          = MakeVertexFetch(resolve_geometry.physical_address(),
                                                        static_cast<std::uint32_t>(resolve_bytes.size()));
    result.source_texture_fetch_words = source_texture;
    result.source_vertex_fetch_words  = source_vertex_fetch;
    TraceOriginalPhase("command_stream_begin");
    const auto makeStream = [&](std::uint32_t terminal_address)
    {
        const auto   terminal_texture = MakeOutputFetch(terminal_address);
        PacketStream stream;
        stream.Type0(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, terminal_texture);
        stream.Swap(terminal_address, kNativePreparedRgba8MatrixOutputWidth, kNativePreparedRgba8MatrixOutputHeight);
        stream.LoadShader(vertex_shader.physical_address(), xenos::ShaderType::kVertex, static_cast<std::uint32_t>(fixture.vertex_shader_words.size()));
        stream.LoadShader(pixel_shader.physical_address(), xenos::ShaderType::kPixel, static_cast<std::uint32_t>(fixture.pixel_shader_words.size()));
        stream.Type0(0x2000U, { 0x14000500U, 0U, 0x530U });
        stream.Type0(0x200FU, { 0x20002000U });
        stream.Type0(0x2080U, { 0U, 0U });
        stream.Type0(0x2082U, { Rgba8MatrixViewportWidth() | (Rgba8MatrixViewportHeight() << 16U) });
        stream.Type0(0x2100U, { 2U });
        stream.Type0(0x2104U, { 0xFU });
        stream.Type0(0x210FU, { std::bit_cast<std::uint32_t>(static_cast<float>(Rgba8MatrixViewportWidth()) / 2.0F), std::bit_cast<std::uint32_t>(static_cast<float>(Rgba8MatrixViewportWidth()) / 2.0F), std::bit_cast<std::uint32_t>(-static_cast<float>(Rgba8MatrixViewportHeight()) / 2.0F), std::bit_cast<std::uint32_t>(static_cast<float>(Rgba8MatrixViewportHeight()) / 2.0F) });
        stream.Type0(0x2180U, { 0x10010002U });
        stream.Type0(0x2181U, { 4U });
        stream.Type0(0x2182U, { 1U });
        stream.Type0(0x2200U, { 0x24F00730U });
        stream.Type0(0x2201U, { 0x01000001U });
        stream.Type0(0x2202U, { 0x87000000U });
        stream.Type0(0x2204U, { 0x80000U });
        stream.Type0(0x2205U, { 0x18000U });
        stream.Type0(0x2206U, { 0x43FU });
        stream.Type0(0x2208U, { 4U });
        stream.Type0(0x2302U, { 4U });
        stream.Type0(0x2307U, { 0xFF000U });
        stream.Type0(0x2308U, { 0xFF100U });
        // The terminal copy leaves RB_COPY_* state live across submissions. Set
        // the complete copy register block explicitly before each source draw so
        // the issued draw has the fixture's authored zero state.
        stream.Type0(0x2318U, { 0U, 0U, 0U, 0U, 0U, 0U, 0U });
        stream.Type0(XE_GPU_REG_SHADER_CONSTANT_000_X, kVertexConstants);
        stream.Type0(XE_GPU_REG_SHADER_CONSTANT_256_X, kPixelConstant0);
        stream.Type0(XE_GPU_REG_SHADER_CONSTANT_256_X + 255U * 4U, kPixelConstant255);
        stream.Type0(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, source_texture);
        stream.Type0(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2U * 95U, source_vertex_fetch);
        stream.Type0(0x21FCU, { kNativePreparedRgba8MatrixDrawInitiator });
        stream.DrawDma(kNativePreparedRgba8MatrixDrawInitiator,
                       indices.physical_address(),
                       kNativePreparedRgba8MatrixIndexCount,
                       kNativePreparedRgba8MatrixIndexEndian);
        stream.Type0(0x2000U, { 0x14000500U, 0U, 0x530U });
        stream.Type0(0x200FU, { 0x20002000U });
        stream.Type0(0x2080U, { 0U, 0U, Rgba8MatrixViewportWidth() | (Rgba8MatrixViewportHeight() << 16U) });
        stream.Type0(0x2082U, { kNativePreparedRgba8MatrixOutputWidth | (kNativePreparedRgba8MatrixOutputHeight << 16U) });
        stream.Type0(0x2205U, { 0x10000U });
        stream.Type0(0x2208U, { 6U });
        stream.Type0(0x2318U, { 0x00100000U });
        stream.Type0(0x2319U, { terminal_address });
        stream.Type0(0x231AU, { kNativePreparedRgba8MatrixOutputWidth | (kNativePreparedRgba8MatrixOutputHeight << 16U) });
        stream.Type0(0x231BU, { 0x00000300U });
        stream.Type0(0x231CU, { 0xFFFU });
        stream.Type0(0x231DU, { 0xFFFFFF00U });
        stream.Type0(0x231EU, { 0U, 0U });
        stream.Type0(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, resolve_fetch);
        stream.Type0(0x21FCU, { static_cast<std::uint32_t>(xenos::PrimitiveType::kRectangleList) | (static_cast<std::uint32_t>(xenos::SourceSelect::kAutoIndex) << 6U) | (3U << 16U) });
        stream.DrawAuto(0x00030088U);
        stream.Type0(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, terminal_texture);
        stream.Swap(terminal_address, kNativePreparedRgba8MatrixOutputWidth, kNativePreparedRgba8MatrixOutputHeight);
        return stream;
    };
    auto stream = makeStream(terminal.physical_address());
    if (!Require(stream.words().size() < kRingWordCount, "PM4 stream exceeds ring capacity", error))
    {
        unregisterFrameObserver();
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
    const auto baseline_snapshot              = observation->SnapshotData();
    result.baseline_completion_observed       = result.completion_observed;
    result.baseline_copy_id                   = baseline_snapshot.completion_copy_id;
    result.baseline_swap_id                   = baseline_snapshot.completion_swap_id;
    result.baseline_frame                     = baseline_snapshot.completion_frame;
    result.baseline_submission                = baseline_snapshot.completion_submission;
    std::uint64_t baseline_swap_event_ordinal = 0;
    for (const auto& swap : baseline_snapshot.swaps)
    {
        if (swap.id == result.baseline_swap_id)
        {
            baseline_swap_event_ordinal = swap.event_ordinal;
            break;
        }
    }
    rex::ui::RawImage baseline_image;
    result.baseline_terminal_capture = result.baseline_completion_observed && baseline_swap_event_ordinal != 0 &&
                                       graphics->presenter() && graphics->presenter()->CaptureGuestOutput(baseline_image);
    if (result.baseline_terminal_capture)
        result.baseline_terminal_pixels = baseline_image.data;
    if (!Require(result.baseline_terminal_capture &&
                     result.baseline_terminal_pixels == result.baseline_expected_pixels,
                 "baseline Prepared RGBA8 completion did not capture the expected pixels",
                 error))
    {
        unregisterFrameObserver();
        shutdown();
        result.failure = error;
        return 1;
    }

    if (rgba8MatrixForm->region_updates)
        result.region_snapshots.push_back(result.old_base_bytes);
    const auto first_words = static_cast<std::uint32_t>(stream.words().size());
    const auto updateCount = rgba8MatrixForm->region_updates ? 3U : 1U;
    for (std::uint32_t updateIndex = 0; updateIndex < updateCount; ++updateIndex)
    {
        // Perform the guarded CPU update against the same live allocations after
        // the first completion. The resulting callback must publish a new owner.
        if (rgba8MatrixForm->region_updates)
        {
            auto model = fixture.texture.base_guest_bytes;
            ModelR8Region(model, updateIndex);
            const auto& r      = kR8RegionWrites[updateIndex];
            const auto  source = R8RegionSource(updateIndex);
            for (std::uint32_t row = 0; row < r.bottom - r.top; ++row)
                std::memcpy(mip_base.virtual_bytes() + std::size_t(r.destination_y + row) * 1024U + r.destination_x,
                            source.data() + std::size_t(r.top + row) * r.source_width + r.left,
                            r.right - r.left);
            if (!Require(std::memcmp(mip_base.physical_bytes(), model.data(), model.size()) == 0,
                         "ordered R8 region differs from whole-backing CPU model",
                         error))
            {
                unregisterFrameObserver();
                shutdown();
                result.failure = error;
                return 1;
            }
            result.region_snapshots.push_back(model);
            auto& pixels = fixture.texture.mips[0].linear_rgba8;
            for (std::size_t at = 0; at < model.size(); ++at)
                pixels[at * 4U + 3U] = model[at];
        }
        else
            ApplyCpuMipUpdate(fixture, params.first_lod, params.update_kind);
        if (!Require(RebuildTextureGuestBytes(fixture),
                     "CPU update could not rebuild every effective raw level",
                     error))
        {
            unregisterFrameObserver();
            shutdown();
            result.failure = error;
            return 1;
        }
        fixture.selected_lods = { params.first_lod, params.second_lod };
        RebuildExpectedPlanes(fixture);
        result.expected_pixels     = ExpectedNativePreparedRgba8MatrixPixels(fixture, params.first_lod, params.second_lod);
        result.new_base_bytes      = fixture.texture.base_guest_bytes;
        result.new_mip_bytes       = fixture.texture.mip_guest_bytes;
        result.cpu_update_observed = result.old_base_bytes != result.new_base_bytes || result.old_mip_bytes != result.new_mip_bytes;
        if (!Require(result.cpu_update_observed, "CPU RGBA8 update did not change retained raw guest bytes", error))
        {
            unregisterFrameObserver();
            shutdown();
            result.failure = error;
            return 1;
        }
        const auto update_width             = kNativePreparedRgba8MatrixWidths[params.first_lod];
        const auto update_height            = kNativePreparedRgba8MatrixHeights[params.first_lod];
        const auto update_blocks_wide       = update_width;
        const auto update_blocks_high       = update_height;
        const auto update_offset            = kNativePreparedRgba8MatrixGuestOffsets[params.first_lod];
        const auto update_length            = rgba8MatrixForm->tiled ? rgba8MatrixForm->base_data_extent : (update_blocks_high - 1U) * kNativePreparedRgba8MatrixRowPitches[params.first_lod] + update_blocks_wide * Rgba8MatrixBlockBytes();
        bool       update_outside_unchanged = false;
        bool       update_overlap_written   = false;
        const bool update_applied           = rgba8MatrixForm->region_updates || (params.first_lod == 0U
                                                                                      ? CopyChangedRgba8RangeWithOverlap(mip_base,
                                                                                                                         result.old_base_bytes,
                                                                                                                         result.new_base_bytes,
                                                                                                                         update_offset,
                                                                                                                         update_length,
                                                                                                                         update_outside_unchanged,
                                                                                                                         update_overlap_written)
                                                                                      : CopyChangedRgba8RangeWithOverlap(mip_levels,
                                                                                                                         result.old_mip_bytes,
                                                                                                                         result.new_mip_bytes,
                                                                                                                         update_offset,
                                                                                                                         update_length,
                                                                                                                         update_outside_unchanged,
                                                                                                                         update_overlap_written));
        if (!rgba8MatrixForm->region_updates)
        {
            result.cpu_update_guest_offset      = update_offset;
            result.cpu_update_guest_length      = update_length;
            result.cpu_update_overlap_written   = update_overlap_written;
            result.cpu_update_outside_unchanged = update_outside_unchanged;
        }
        result.cpu_update_observed = result.cpu_update_observed && update_applied;
        result.source_raw_bytes_frozen =
            std::memcmp(mip_base.physical_bytes(), fixture.texture.base_guest_bytes.data(), fixture.texture.base_guest_bytes.size()) == 0 &&
            std::memcmp(mip_levels.physical_bytes(), fixture.texture.mip_guest_bytes.data(), fixture.texture.mip_guest_bytes.size()) == 0;
        if (!Require(result.source_raw_bytes_frozen && (rgba8MatrixForm->region_updates || (result.cpu_update_overlap_written && result.cpu_update_outside_unchanged)),
                     "CPU RGBA8 update did not preserve complete raw bytes outside its selected overlap range",
                     error))
        {
            unregisterFrameObserver();
            shutdown();
            result.failure = error;
            return 1;
        }

        const auto terminal_address = terminal.physical_address() +
                                      (rgba8MatrixForm->region_updates ? (updateIndex + 1U) * terminal_extent : 0U);
        stream                      = makeStream(terminal_address);
        const auto total_words      = first_words * (updateIndex + 2U);
        if (!Require(total_words < kRingWordCount, "RGBA8/R8 submissions exceed ring capacity", error))
        {
            unregisterFrameObserver();
            shutdown();
            result.failure = error;
            return 1;
        }
        TraceOriginalPhase("publish_update_begin");
        WritePacketWords(memory, ring.physical_address() + first_words * (updateIndex + 1U) * sizeof(std::uint32_t), stream.words());
        result.published_word_count = total_words;
        PublishWritePointer(memory, total_words);
        TraceOriginalPhase("publish_update_end");
        TraceOriginalPhase("wait_writeback_update_begin");
        const bool update_progressed = WaitForWriteback(memory, writeback.physical_address(), total_words);
        result.progressed            = result.progressed && update_progressed;
        TraceOriginalPhase(update_progressed ? "wait_writeback_update_end" : "wait_writeback_update_failed");
        TraceOriginalPhase("wait_completion_update_begin");
        result.completion_observed = result.progressed &&
                                     observation->WaitForCompletion(terminal_address, baseline_swap_event_ordinal);
        TraceOriginalPhase(result.completion_observed ? "wait_completion_update_end" : "wait_completion_update_failed");
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
                                                    ? "presenter CaptureGuestOutput after matched completed update"
                                                    : "none";
        if (!Require(result.terminal_pixels_match_expected, "intermediate region output differs from oracle", error))
        {
            unregisterFrameObserver();
            shutdown();
            result.failure = error;
            return 1;
        }
        const auto update_snapshot = observation->SnapshotData();
        for (const auto& swap : update_snapshot.swaps)
            if (swap.id == update_snapshot.completion_swap_id)
                baseline_swap_event_ordinal = swap.event_ordinal;
    }
    unregisterFrameObserver();
    for (auto draw = result.draws.rbegin(); draw != result.draws.rend(); ++draw)
    {
        if (draw->vertex_shader_words == fixture.vertex_shader_words &&
            draw->pixel_shader_words == fixture.pixel_shader_words)
        {
            result.source_registers = draw->registers;
            result.source_draw_issued =
                draw->original_host_issued &&
                draw->execution_status == static_cast<std::uint32_t>(DrawCaptureExecutionStatus::kIssued);
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
    if (result.source_draw_issued && result.source_registers.size() == kNativePreparedRgba8MatrixRegisterCount)
    {
        result.source_fixture.register_image      = result.source_registers;
        result.source_fixture.texture.fetch_words = result.source_texture_fetch_words;
        result.source_fixture.vertex_fetch_words  = result.source_vertex_fetch_words;
    }
    const bool complete = result.progressed && result.baseline_completion_observed &&
                          result.baseline_terminal_capture && result.cpu_update_observed &&
                          result.completion_observed && result.terminal_capture &&
                          result.actual_readback_owner && result.source_draw_issued &&
                          result.terminal_pixels_match_expected &&
                          result.terminal_width == kNativePreparedRgba8MatrixOutputWidth &&
                          result.terminal_height == kNativePreparedRgba8MatrixOutputHeight &&
                          result.terminal_stride == kNativePreparedRgba8MatrixOutputWidth * 4U;
    if (!complete)
    {
        error          = "original prepared RGBA8 GPU completion or expected pixels were incomplete";
        result.failure = error;
    }
    shutdown();
    return complete ? 0 : 1;
}

} // namespace rex::graphics::testing

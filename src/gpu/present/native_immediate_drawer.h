#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <rex/ui/immediate_drawer.h>

namespace plume
{

struct RenderDevice;
struct RenderCommandList;

} // namespace plume

namespace rerevved::gpu
{

struct NativeOverlayTexture
{
    uint32_t                        width, height;
    rex::ui::ImmediateTextureFilter filter;
    bool                            repeated;
    std::vector<uint8_t>            rgba;
};

struct NativeOverlayCommand
{
    rex::ui::ImmediateDraw                      draw;
    std::shared_ptr<const NativeOverlayTexture> texture;
};

struct NativeOverlayBatch
{
    std::vector<rex::ui::ImmediateVertex> vertices;
    std::vector<uint16_t>                 indices;
    bool                                  indexed = false;
    std::vector<NativeOverlayCommand>     commands;
};

struct NativeOverlaySnapshot
{
    uint64_t                        sequence = 0;
    float                           width = 0, height = 0;
    std::vector<NativeOverlayBatch> batches;
};

// UI publishes complete copies; the host takes a shared immutable version.
// Release the mailbox lock before ImGui, GPU work, or entering the session.
// Close may take session -> drain -> mailbox; the mailbox never calls outward.
class NativeOverlayMailbox
{
public:
    void                                         SetReady(bool ready);
    bool                                         Ready() const;
    void                                         Publish(std::unique_ptr<NativeOverlaySnapshot> snapshot);
    std::shared_ptr<const NativeOverlaySnapshot> Latest() const;
    void                                         Fail();
    bool                                         Failed() const;

private:
    mutable std::mutex                           mutex_;
    bool                                         ready_    = false;
    uint64_t                                     sequence_ = 0;
    std::shared_ptr<const NativeOverlaySnapshot> latest_;
    std::atomic<bool>                            failed_{ false };
};

// SDK-owned facade. All its calls and texture handles belong to the UI thread;
// GPU objects belong exclusively to NativeOverlayCompositor.
class NativeImmediateDrawer final : public rex::ui::ImmediateDrawer
{
public:
    explicit NativeImmediateDrawer(std::shared_ptr<NativeOverlayMailbox> mailbox);
    std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t width, uint32_t height, rex::ui::ImmediateTextureFilter filter, bool repeated, const uint8_t* data) override;
    void                                       Begin(rex::ui::UIDrawContext& context, float width, float height) override;
    void                                       BeginDrawBatch(const rex::ui::ImmediateDrawBatch& batch) override;
    void                                       Draw(const rex::ui::ImmediateDraw& draw) override;
    void                                       EndDrawBatch() override;
    void                                       End() override;
    void                                       Clear();

private:
    std::shared_ptr<NativeOverlayMailbox>  mailbox_;
    std::unique_ptr<NativeOverlaySnapshot> recording_;
    bool                                   batchOpen_ = false;
};

// Lives in the session's GPU ownership set, including on an unproved drain.
// Record is called only after the same host slot has actually retired.
class NativeOverlayCompositor
{
public:
    NativeOverlayCompositor();
    ~NativeOverlayCompositor();
    bool   Record(plume::RenderDevice& device, plume::RenderCommandList& list, size_t slot, std::shared_ptr<const NativeOverlaySnapshot> snapshot, uint32_t width, uint32_t height);
    size_t LiveTextures() const;
    size_t GeometryAllocations() const;
    size_t PendingTextureUploads() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rerevved::gpu

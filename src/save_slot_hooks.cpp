#include "save_slots.h"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_INT32(save_slots, 99, "ReRevved", "Manual save slots in the Save and Load lists (10 to 99)");

REX_EXTERN(__imp__sub_82DDB818);
REX_EXTERN(__imp__sub_82DDB8F0);
REX_EXTERN(__imp__sub_821C08A0);
REX_EXTERN(sub_821C0978);
REX_EXTERN(sub_821C0780);

namespace
{

namespace slots = rerevved::save_slots;

// Read once: the record vector and buffer sizes must not change while a list exists.
int32_t slotCount()
{
    static const int32_t value = slots::ClampSlots(REXCVAR_GET(save_slots));
    return value;
}

bool extended()
{
    return slotCount() != slots::kNativeSlots;
}

const uint8_t* checkedBytes(uint64_t address, uint32_t size)
{
    auto* kernel = REX_KERNEL_STATE();
    auto* memory = kernel ? kernel->memory() : nullptr;
    if (!memory || !size || address < 0x10000 || address + size > 0xFFFFF000ull)
    {
        throw std::runtime_error("Save slots: unavailable guest memory");
    }
    const auto first = static_cast<uint32_t>(address);
    const auto last  = first + size - 1;
    auto*      heap  = memory->LookupHeap(first);
    if (!heap || memory->LookupHeap(last) != heap ||
        (static_cast<uint32_t>(heap->QueryRangeAccess(first, last)) & static_cast<uint32_t>(rex::memory::PageAccess::kReadOnly)) == 0)
    {
        throw std::runtime_error("Save slots: inaccessible guest range");
    }
    return memory->TranslateVirtual<const uint8_t*>(first);
}

uint32_t readWord(uint64_t address)
{
    const auto* bytes = checkedBytes(address, 4);
    return uint32_t{ bytes[0] } << 24 | uint32_t{ bytes[1] } << 16 | uint32_t{ bytes[2] } << 8 | bytes[3];
}

uint32_t recordCount(uint32_t picker)
{
    const uint32_t begin = readWord(uint64_t{ picker } + 0x450);
    const uint32_t end   = readWord(uint64_t{ picker } + 0x454);
    if (end < begin)
    {
        throw std::runtime_error("Save slots: inverted record vector");
    }
    return std::min((end - begin) / slots::kRecordStride, static_cast<uint32_t>(slotCount()));
}

// A null name pointer counts as empty; the native loop would read below the address space.
uint32_t usedSlots(uint32_t picker)
{
    const uint32_t begin = readWord(uint64_t{ picker } + 0x450);
    const uint32_t end   = readWord(uint64_t{ picker } + 0x454);
    if (end < begin)
    {
        throw std::runtime_error("Save slots: inverted record vector");
    }
    std::vector<uint32_t> lengths;
    for (uint64_t record = begin; record + slots::kRecordStride <= end; record += slots::kRecordStride)
    {
        const uint32_t name = readWord(record + 332);
        lengths.push_back(name ? readWord(uint64_t{ name } - 4) : 0);
    }
    return slots::UsedSlots(lengths);
}

} // namespace

// The buffer always holds the largest count, so lowering save_slots cannot hide saves already on disk.
void GrowSaveListObject(PPCRegister& size)
{
    if (size.u32 == slots::kNativeObjectBytes)
    {
        size.u64 = slots::ObjectBytes(slots::kMaxSlots);
    }
}

void SizeSaveEnumerator(PPCRegister& entries)
{
    entries.u64 = slots::EnumeratorEntries(slots::kMaxSlots);
}

bool FillMoreSavePlaceholders(PPCRegister& offset, PPCRegister& picker)
{
    try
    {
        return extended() && offset.u32 < recordCount(picker.u32) * slots::kRecordStride;
    }
    catch (...)
    {
        return false;
    }
}

bool NameExtraSaveSlot(PPCRegister& slot)
{
    return extended() && slot.u32 >= slots::kNativeSlots && slot.u32 < static_cast<uint32_t>(slotCount());
}

bool NameExtraSaveRecord(PPCRegister& slot)
{
    return slot.u32 >= static_cast<uint32_t>(slots::kNativeSlots);
}

bool KeepExtraSaveSlot(PPCRegister& slot, PPCRegister& entry, PPCRegister& picker)
{
    if (!extended())
    {
        return false;
    }
    try
    {
        std::string_view name;
        if (slot.u32 == 11)
        {
            name = { reinterpret_cast<const char*>(checkedBytes(uint64_t{ entry.u32 } + 264, 3)), 3 };
        }
        return slots::KeepsManualSlot(slot.u32, name, static_cast<int32_t>(recordCount(picker.u32)));
    }
    catch (...)
    {
        return false;
    }
}

// The native body has 10 records compiled in and ignores its count; insert the missing ones from the prototype.
REX_HOOK_RAW(sub_821C08A0)
{
    uint32_t end  = 0;
    uint32_t size = 0;
    try
    {
        const uint32_t begin = readWord(uint64_t{ ctx.r3.u32 } + 4);
        end                  = readWord(uint64_t{ ctx.r3.u32 } + 8);
        size                 = begin ? (end - begin) / slots::kRecordStride : 0;
    }
    catch (...)
    {
        size = UINT32_MAX;
    }
    if (!extended() || size >= static_cast<uint32_t>(slotCount()))
    {
        __imp__sub_821C08A0(ctx, base);
        return;
    }
    const uint32_t prototype = ctx.r5.u32;
    ctx.r4.u64               = end;
    ctx.r5.u64               = static_cast<uint32_t>(slotCount()) - size;
    ctx.r6.u64               = prototype;
    sub_821C0978(ctx, base);
    ctx.r3.u64 = prototype;
    sub_821C0780(ctx, base);
}

REX_HOOK_RAW(sub_82DDB818)
{
    if (!extended())
    {
        __imp__sub_82DDB818(ctx, base);
        return;
    }
    try
    {
        ctx.r3.u64 = slots::FullnessCount(usedSlots(ctx.r3.u32), static_cast<int32_t>(recordCount(ctx.r3.u32)));
    }
    catch (...)
    {
        __imp__sub_82DDB818(ctx, base);
    }
}

REX_HOOK_RAW(sub_82DDB8F0)
{
    if (!extended())
    {
        __imp__sub_82DDB8F0(ctx, base);
        return;
    }
    try
    {
        const uint32_t picker = ctx.r3.u32;
        const uint32_t used   = usedSlots(picker);
        ctx.r3.u64            = slots::RowCount(used, static_cast<int32_t>(recordCount(picker)), readWord(uint64_t{ picker } + 1080) == 1);
    }
    catch (...)
    {
        __imp__sub_82DDB8F0(ctx, base);
    }
}

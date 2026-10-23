#include "gpu/guest_d3d/solid_draw_adapter.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

using namespace rerevved::gpu::guest_d3d;

namespace
{

void store(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    for (unsigned c = 0; c < 4; ++c)
        bytes[offset + c] = static_cast<std::uint8_t>(value >> (24 - c * 8));
}

std::uint32_t word(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

} // namespace

TEST_CASE("solid_draw_adapter_abi", "[ctest][solid-draw-adapter]")
{
    std::string               error;
    constexpr std::uint32_t   base = 0x10000, g = base + 0x100;
    std::vector<std::uint8_t> memory(0x8000, 0xCD);
    IndexedUpArguments        a{ g, 4, 1, 2, 3, base + 0x5000, 1, base + 0x4000, 8, 0x82303E90 };
    IndexedUpStorage          storage{ base + 0x7000, base + 0x6000, base + 0x6100 };
    for (unsigned i = 0; i < 24; ++i)
        memory[0x4000 + i] = static_cast<std::uint8_t>(i + 1);
    // Three BE16 indices plus the original wrapper's rounded-copy padding.
    const std::array<std::uint8_t, 8> indices{ 0, 0, 0, 1, 0, 0, 0xDE, 0xAD };
    std::copy(indices.begin(), indices.end(), memory.begin() + 0x5000);
    const auto    original = memory;
    std::uint32_t result   = 0xBAD;
    UNSCOPED_INFO("valid indexed-UP ABI completion");
    REQUIRE(CompleteIndexedUpAbi(a, storage, memory, base, result, error));
    UNSCOPED_INFO("pending cursor/output/count and cursor publication");
    REQUIRE((word(memory, 0x100 + 0x3484) == storage.pendingCursor &&
             word(memory, 0x100 + 0x3488) == storage.vertexOutput &&
             word(memory, 0x100 + 0x348C) == storage.indexOutput &&
             word(memory, 0x100 + 0x3490) == 4 && word(memory, 0x100 + 0x3494) == 2 &&
             word(memory, 0x100 + 0x30) == storage.pendingCursor));
    UNSCOPED_INFO("r3 is final memcpy destination, not success zero");
    REQUIRE(result == storage.indexOutput);
    UNSCOPED_INFO("minimumVertex offset and vertexCount copy extent");
    REQUIRE(std::equal(original.begin() + 0x4008, original.begin() + 0x4018, memory.begin() + 0x6000));
    UNSCOPED_INFO("BE16 index copy includes rounded padding");
    REQUIRE(std::equal(indices.begin(), indices.end(), memory.begin() + 0x6100));
    UNSCOPED_INFO("source geometry unchanged");
    REQUIRE(std::equal(original.begin() + 0x4000, original.begin() + 0x5010, memory.begin() + 0x4000));
    const auto completed  = memory;
    const auto badStorage = IndexedUpStorage{ storage.pendingCursor, storage.vertexOutput, base + 0x7FFE };
    UNSCOPED_INFO("failed admission preserves memory and r3");
    REQUIRE((!CompleteIndexedUpAbi(a, badStorage, memory, base, result, error) && memory == completed &&
             result == storage.indexOutput));
    a.indexFormat = 6;
    a.indexCount  = 3;
    memory        = original;
    UNSCOPED_INFO("32-bit guest index ABI completion");
    REQUIRE(CompleteIndexedUpAbi(a, storage, memory, base, result, error));
    UNSCOPED_INFO("format bit 0x4 selects four-byte index elements");
    REQUIRE((word(memory, 0x100 + 0x3494) == 3 &&
             std::equal(original.begin() + 0x5000, original.begin() + 0x500C, memory.begin() + 0x6100)));

    SolidDrawInputs in;
    in.arguments.stride = 8;
    in.state.resize(0x3500);
    in.state[0x30F0] = 0xE1;
    in.vertexCode.resize(132, 0xA5);
    in.fetchTemplates.resize(24);
    in.fetchRecords.resize(8);
    in.declaration.resize(24);
    in.swizzleTable.resize(16);
    in.formatTable.resize(64);
    // The fetch count comes from vertexMetadata+28.
    in.vertexMetadata.resize(32);
    store(in.vertexMetadata, 28, 2);
    store(in.fetchRecords, 0, 1);
    store(in.fetchRecords, 4, 0x00001002);
    in.declaration[9]  = 0;
    in.declaration[10] = 0;
    in.declaration[21] = 1;
    in.declaration[22] = 0;
    store(in.declaration, 4, 25);
    store(in.declaration, 12, 4);
    store(in.declaration, 16, 6);
    std::vector<std::uint8_t> patched;
    UNSCOPED_INFO("two semantic fetch templates patched natively");
    REQUIRE(PatchVertexFetch(in, kSolidAttributeOffsets, patched, error));
    UNSCOPED_INFO("temporary stride/4 and declaration offsets override stale patch-state byte");
    REQUIRE((word(patched, 20) == 2 && word(patched, 32) == 0x102));
    UNSCOPED_INFO("position and color formats come from declaration");
    REQUIRE((((word(patched, 16) >> 16) & 63) == 25 && ((word(patched, 28) >> 16) & 63) == 6));
    UNSCOPED_INFO("only metadata-designated fetch instructions change");
    REQUIRE((std::equal(patched.begin(), patched.begin() + 12, in.vertexCode.begin()) &&
             std::equal(patched.begin() + 36, patched.end(), in.vertexCode.begin() + 36)));
    // The closing 0x300000 record excludes the last fetch from sorting.
    // Reverse offsets so an accidental swap is observable without coalescing.
    store(in.declaration, 0, 4);
    store(in.declaration, 12, 0);
    store(in.fetchRecords, 0, 0x100001);
    store(in.fetchRecords, 4, 0x301002);
    UNSCOPED_INFO("closing 0x300000 record preserves two-fetch order");
    REQUIRE((PatchVertexFetch(in, kSolidAttributeOffsets, patched, error) && word(patched, 20) == 0x102 && word(patched, 32) == 2));
    store(in.fetchRecords, 4, 0x101002);
    UNSCOPED_INFO("ordinary closing record sorts two-fetch offsets");
    REQUIRE((PatchVertexFetch(in, kSolidAttributeOffsets, patched, error) && word(patched, 20) == 2 && word(patched, 32) == 0x102));
    in.declaration[21] = 2;
    // 0x826AD228: a semantic without an element fetches k_8_8_8_8 at offset
    // 0 of fetch constant 95. An ungrouped record keeps its slot.
    store(in.fetchRecords, 4, 0x00001002);
    UNSCOPED_INFO("an unknown declaration semantic takes the guest's unmatched fetch");
    REQUIRE((PatchVertexFetch(in, kSolidAttributeOffsets, patched, error) && (word(patched, 24) >> 20 & 0x3FF) == 95 && (word(patched, 28) >> 16 & 63) == 6 &&
             !(word(patched, 28) & 0x40003000) && word(patched, 32) == 2));
    store(in.fetchRecords, 4, 0x101002);
    in.declaration[21] = 1;
    store(in.fetchRecords, 4, 0x00001FFF);
    UNSCOPED_INFO("fetch patch cannot escape shader extent");
    REQUIRE((!PatchVertexFetch(in, kSolidAttributeOffsets, patched, error) && error == "adapter_fetch_code_offset"));
    // 0x826AD8E8: word20 bits 5..9 count linkage records; bits 0..4
    // encode the PS input count. Lists use bit 0x1000 to terminate.
    SolidDrawInputs linkage;
    linkage.state.resize(0x3500);
    linkage.vertexCode.resize(132, 0xA5);
    linkage.vertexMetadata.resize(76);
    linkage.pixelMetadata.resize(36);
    store(linkage.vertexMetadata, 4, 132);
    store(linkage.pixelMetadata, 4, 36);
    store(linkage.vertexMetadata, 8, 0x10F00001);
    store(linkage.vertexMetadata, 12, 0x100);
    store(linkage.pixelMetadata, 8, 0x00010000);
    store(linkage.pixelMetadata, 12, 0x20);
    store(linkage.pixelMetadata, 24, 0x1234);
    store(linkage.vertexMetadata, 20, 3 * 32 + 3);
    store(linkage.pixelMetadata, 20, 1 * 32 + 2);
    store(linkage.vertexMetadata, 28, 2);
    // VS records start at (word24 + fetchCount + 9)*4 = 44.
    // Export 1 is unused, export 2 remaps from r4 to r7, export 3 is unused.
    store(linkage.vertexMetadata, 44, 0x00000401);
    store(linkage.vertexMetadata, 48, 0x00010402);
    store(linkage.vertexMetadata, 52, 0x00030403);
    store(linkage.pixelMetadata, 32, 0x00000702);
    store(linkage.vertexMetadata, 56, 0x1003);
    store(linkage.vertexMetadata, 60, 4);
    store(linkage.vertexMetadata, 64, 0x1005);
    store(linkage.vertexMetadata, 68, 0x1006);
    std::vector<std::uint8_t> folded;
    patched = linkage.vertexCode;
    UNSCOPED_INFO("mismatched metadata links the active pair");
    REQUIRE(FoldSolidShaderMetadata(linkage, patched, folded, error));
    UNSCOPED_INFO("unused exports receive 0x826AD760 no-op instruction");
    REQUIRE((word(patched, 36) == 0xC8000000 && word(patched, 40) == 0 && word(patched, 44) == 0x02000000 &&
             word(patched, 72) == 0xC8000000));
    UNSCOPED_INFO("0x826AD830 remaps only destination nibble across the terminated list");
    REQUIRE((word(patched, 48) == 0xA5A5A5A7 && word(patched, 60) == 0xA5A5A5A7 &&
             word(patched, 52) == 0xA5A5A5A5));
    UNSCOPED_INFO("PS input count minus one is inserted before loc_826AE1F0 OR fold");
    REQUIRE((word(folded, 0x2920) == 0x10110001 && word(folded, 0x2924) == 0x120 &&
             word(folded, 0x2928) == 0x1234 && word(folded, 0x2954) == 4));
    store(linkage.pixelMetadata, 20, 33);
    patched = linkage.vertexCode;
    UNSCOPED_INFO("one PS input folds to the solid catalog program control");
    REQUIRE((FoldSolidShaderMetadata(linkage, patched, folded, error) && word(folded, 0x2920) == 0x10010001));
    store(linkage.pixelMetadata, 20, 0);
    patched = linkage.vertexCode;
    UNSCOPED_INFO("zero PS inputs clear count field and disable every VS export");
    REQUIRE((FoldSolidShaderMetadata(linkage, patched, folded, error) && word(folded, 0x2920) == 0x10010001));
    for (auto flag : { 0x40000U, 0x20000U })
    {
        store(linkage.vertexMetadata, 20, 99 | (flag == 0x40000 ? flag : 0));
        store(linkage.pixelMetadata, 20, 34 | (flag == 0x20000 ? flag : 0));
        patched = linkage.vertexCode;
        UNSCOPED_INFO("metadata skip flags preserve code and program field");
        REQUIRE((FoldSolidShaderMetadata(linkage, patched, folded, error) && patched == linkage.vertexCode &&
                 word(folded, 0x2920) == 0x10F10001));
    }
    store(linkage.vertexMetadata, 20, 99);
    store(linkage.pixelMetadata, 20, 34);
    linkage.vertexMetadata[8] = 0x17;
    patched                   = linkage.vertexCode;
    UNSCOPED_INFO("VS program high byte low three bits equal seven skips linkage");
    REQUIRE((FoldSolidShaderMetadata(linkage, patched, folded, error) && patched == linkage.vertexCode));
    linkage.vertexMetadata[8] = 0x10;
    store(linkage.pixelMetadata, 20, 99);
    patched = linkage.vertexCode;
    UNSCOPED_INFO("matching word20 retains unmodified program fold");
    REQUIRE((FoldSolidShaderMetadata(linkage, patched, folded, error) && patched == linkage.vertexCode &&
             word(folded, 0x2920) == 0x10F10001));
    store(linkage.pixelMetadata, 20, 34);
    store(linkage.vertexMetadata, 60, 0x1FFF);
    patched = linkage.vertexCode;
    UNSCOPED_INFO("linkage cannot write outside the solid VS code span");
    REQUIRE((!FoldSolidShaderMetadata(linkage, patched, folded, error) && error == "adapter_linkage_code_offset"));

    // The solid PS catalog's numeric words serialize to the pinned LE digest.
    constexpr std::array<std::uint32_t, 9> solidPs{ 0, 0x1001C400, 0x22000000, 0xC80F8000, 0, 0x8B000203, 0, 0, 0 };
    linkage.pixelCode.resize(36);
    for (unsigned i = 0; i < solidPs.size(); ++i)
        store(linkage.pixelCode, i * 4, solidPs[i]);
    UNSCOPED_INFO("132/36 metadata and unpatched PS identity classify the solid pair");
    REQUIRE(IsSolidDrawCandidate(linkage));
    linkage.pixelCode[0] ^= 1;
    UNSCOPED_INFO("equal-sized foreign PS is not a solid candidate");
    REQUIRE(!IsSolidDrawCandidate(linkage));
    linkage.pixelCode[0] ^= 1;
    store(linkage.vertexMetadata, 4, 144);
    UNSCOPED_INFO("foreign VS extent is not a solid candidate");
    REQUIRE(!IsSolidDrawCandidate(linkage));
    store(linkage.vertexMetadata, 4, 132);
    store(linkage.pixelMetadata, 4, 48);
    UNSCOPED_INFO("foreign PS extent is not a solid candidate");
    REQUIRE(!IsSolidDrawCandidate(linkage));
    rerevved::gpu::NativeGameplayShaderInterface shader;
    SolidDrawRecipe                              foreignRecipe;
    UNSCOPED_INFO("foreign pair never receives an admission rejection reason");
    REQUIRE((!BuildSolidDrawRecipe(linkage, shader, foreignRecipe, error) && error.empty()));

    // A foreign stride-8 pair with differing word20 must leave admission open.
    std::vector<std::uint8_t> guest(0x8000);
    const auto                vsObject = base + 0x4000, psObject = base + 0x5000;
    store(guest, 0x100 + 0x3198, vsObject);
    store(guest, 0x100 + 0x3194, psObject);
    store(guest, 0x4000 + 896, 128);
    store(guest, 0x5000 + 64, 128);
    store(guest, 0x4000 + 872 + 128 + 4, 144);
    store(guest, 0x5000 + 40 + 128 + 4, 36);
    store(guest, 0x4000 + 872 + 128 + 20, 0x123);
    store(guest, 0x5000 + 40 + 128 + 20, 0x456);
    const auto guestRead = [&](std::uint64_t address, std::uint64_t size, bool physical)
    {
        UNSCOPED_INFO("foreign read stops before code or draw-family inputs");
        REQUIRE((!physical && address >= base && address + size <= base + guest.size()));
        return std::vector<std::uint8_t>(guest.begin() + (address - base), guest.begin() + (address - base + size));
    };
    SolidDrawInputs foreignInputs;
    a.stride = 8;
    UNSCOPED_INFO("foreign metadata extents precede linkage and unsupported argument admission");
    REQUIRE((!ReadSolidDrawInputs(a, guestRead, foreignInputs, error) && !foreignInputs.solidCandidate && error.empty()));

    unsigned        reads = 0;
    SolidDrawInputs rejected;
    a.stride = 12;
    UNSCOPED_INFO("foreign stride returns without guest reads or rejection reason");
    REQUIRE((!ReadSolidDrawInputs(a, [&](auto, auto, auto)
                                  {
                                      ++reads;
                                      return std::vector<std::uint8_t>{};
                                  },
                                  rejected,
                                  error) &&
             reads == 0 && error.empty() && !rejected.solidCandidate));
}

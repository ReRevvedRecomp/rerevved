#include "tiling_restore_consume.h"

#include "guest_memory.h"
#include "ring_stubs.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <exception>
#include <format>
#include <mutex>

REXCVAR_DEFINE_BOOL(native_consume_tiling_restore, false, "ReRevved", "Replay the exact B118 saved publication restore with validated ring stubs; tr_* timing fields")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_tiling_restore_exit_check, false, "ReRevved", "Force B118 Original and compare seven words and the full semantic register exit; observation only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rerevved::gpu::guest_d3d
{
namespace
{

constexpr std::array<std::uint32_t, 13> offsets{
    0x3460, 0x3464, 0x3468, 0x346C, 0x3470, 0x3474, 0x30, 0x34, 0x3A50, 0x3A4C, 0x3A44, 0x3A48, 0x38
};
std::mutex          countsMutex;
TilingRestoreCounts interval, lifetime;
std::uint64_t       nextCall = 0;

void increment(std::uint64_t TilingRestoreCounts::* field)
{
    std::lock_guard lock(countsMutex);
    ++(interval.*field);
    ++(lifetime.*field);
}

bool validDevice(std::uint32_t g)
{
    return g >= 0x10000 && (g & 3) == 0 && g <= UINT32_MAX - 0x3A53;
}

std::optional<TilingRestoreFallback> identities(const TilingRestoreMemory& memory, std::uint32_t g, std::array<std::uint64_t, 13>& ranges)
{
    if (!validDevice(g) || !memory.identity)
        return TilingRestoreFallback::Mapping;
    for (std::size_t i = 0; i < offsets.size(); ++i)
    {
        const auto identity = memory.identity(g + offsets[i], 4);
        if (!identity || *identity > UINT64_MAX - 3)
            return TilingRestoreFallback::Mapping;
        ranges[i] = *identity;
    }
    for (std::size_t i = 0; i < ranges.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (ranges[i] <= ranges[j] + 3 && ranges[j] <= ranges[i] + 3)
                return TilingRestoreFallback::Alias;
    return std::nullopt;
}

void applyRegisters(PPCContext& ctx, const TilingRestorePlan& plan)
{
    ctx.r5.u64  = plan.registers[0];
    ctx.r6.u64  = plan.registers[1];
    ctx.r7.u64  = plan.registers[2];
    ctx.r8.u64  = plan.registers[3];
    ctx.r9.u64  = plan.registers[4];
    ctx.r10.u64 = plan.registers[5];
    ctx.r11.u64 = plan.registers[6];
}

struct Field
{
    std::string_view name;
    std::uint64_t    value;
};

auto semanticFields(const PPCContext& ctx)
{
    return std::array{
        Field{ "r0", ctx.r0.u64 },
        Field{ "r1", ctx.r1.u64 },
        Field{ "r2", ctx.r2.u64 },
        Field{ "r3", ctx.r3.u64 },
        Field{ "r4", ctx.r4.u64 },
        Field{ "r5", ctx.r5.u64 },
        Field{ "r6", ctx.r6.u64 },
        Field{ "r7", ctx.r7.u64 },
        Field{ "r8", ctx.r8.u64 },
        Field{ "r9", ctx.r9.u64 },
        Field{ "r10", ctx.r10.u64 },
        Field{ "r11", ctx.r11.u64 },
        Field{ "r12", ctx.r12.u64 },
        Field{ "r13", ctx.r13.u64 },
        Field{ "r14", ctx.r14.u64 },
        Field{ "r15", ctx.r15.u64 },
        Field{ "r16", ctx.r16.u64 },
        Field{ "r17", ctx.r17.u64 },
        Field{ "r18", ctx.r18.u64 },
        Field{ "r19", ctx.r19.u64 },
        Field{ "r20", ctx.r20.u64 },
        Field{ "r21", ctx.r21.u64 },
        Field{ "r22", ctx.r22.u64 },
        Field{ "r23", ctx.r23.u64 },
        Field{ "r24", ctx.r24.u64 },
        Field{ "r25", ctx.r25.u64 },
        Field{ "r26", ctx.r26.u64 },
        Field{ "r27", ctx.r27.u64 },
        Field{ "r28", ctx.r28.u64 },
        Field{ "r29", ctx.r29.u64 },
        Field{ "r30", ctx.r30.u64 },
        Field{ "r31", ctx.r31.u64 },
        Field{ "f0", ctx.f0.u64 },
        Field{ "f1", ctx.f1.u64 },
        Field{ "f2", ctx.f2.u64 },
        Field{ "f3", ctx.f3.u64 },
        Field{ "f4", ctx.f4.u64 },
        Field{ "f5", ctx.f5.u64 },
        Field{ "f6", ctx.f6.u64 },
        Field{ "f7", ctx.f7.u64 },
        Field{ "f8", ctx.f8.u64 },
        Field{ "f9", ctx.f9.u64 },
        Field{ "f10", ctx.f10.u64 },
        Field{ "f11", ctx.f11.u64 },
        Field{ "f12", ctx.f12.u64 },
        Field{ "f13", ctx.f13.u64 },
        Field{ "f14", ctx.f14.u64 },
        Field{ "f15", ctx.f15.u64 },
        Field{ "f16", ctx.f16.u64 },
        Field{ "f17", ctx.f17.u64 },
        Field{ "f18", ctx.f18.u64 },
        Field{ "f19", ctx.f19.u64 },
        Field{ "f20", ctx.f20.u64 },
        Field{ "f21", ctx.f21.u64 },
        Field{ "f22", ctx.f22.u64 },
        Field{ "f23", ctx.f23.u64 },
        Field{ "f24", ctx.f24.u64 },
        Field{ "f25", ctx.f25.u64 },
        Field{ "f26", ctx.f26.u64 },
        Field{ "f27", ctx.f27.u64 },
        Field{ "f28", ctx.f28.u64 },
        Field{ "f29", ctx.f29.u64 },
        Field{ "f30", ctx.f30.u64 },
        Field{ "f31", ctx.f31.u64 },
        Field{ "v0[0]", ctx.v0.u64[0] },
        Field{ "v0[1]", ctx.v0.u64[1] },
        Field{ "v1[0]", ctx.v1.u64[0] },
        Field{ "v1[1]", ctx.v1.u64[1] },
        Field{ "v2[0]", ctx.v2.u64[0] },
        Field{ "v2[1]", ctx.v2.u64[1] },
        Field{ "v3[0]", ctx.v3.u64[0] },
        Field{ "v3[1]", ctx.v3.u64[1] },
        Field{ "v4[0]", ctx.v4.u64[0] },
        Field{ "v4[1]", ctx.v4.u64[1] },
        Field{ "v5[0]", ctx.v5.u64[0] },
        Field{ "v5[1]", ctx.v5.u64[1] },
        Field{ "v6[0]", ctx.v6.u64[0] },
        Field{ "v6[1]", ctx.v6.u64[1] },
        Field{ "v7[0]", ctx.v7.u64[0] },
        Field{ "v7[1]", ctx.v7.u64[1] },
        Field{ "v8[0]", ctx.v8.u64[0] },
        Field{ "v8[1]", ctx.v8.u64[1] },
        Field{ "v9[0]", ctx.v9.u64[0] },
        Field{ "v9[1]", ctx.v9.u64[1] },
        Field{ "v10[0]", ctx.v10.u64[0] },
        Field{ "v10[1]", ctx.v10.u64[1] },
        Field{ "v11[0]", ctx.v11.u64[0] },
        Field{ "v11[1]", ctx.v11.u64[1] },
        Field{ "v12[0]", ctx.v12.u64[0] },
        Field{ "v12[1]", ctx.v12.u64[1] },
        Field{ "v13[0]", ctx.v13.u64[0] },
        Field{ "v13[1]", ctx.v13.u64[1] },
        Field{ "v14[0]", ctx.v14.u64[0] },
        Field{ "v14[1]", ctx.v14.u64[1] },
        Field{ "v15[0]", ctx.v15.u64[0] },
        Field{ "v15[1]", ctx.v15.u64[1] },
        Field{ "v16[0]", ctx.v16.u64[0] },
        Field{ "v16[1]", ctx.v16.u64[1] },
        Field{ "v17[0]", ctx.v17.u64[0] },
        Field{ "v17[1]", ctx.v17.u64[1] },
        Field{ "v18[0]", ctx.v18.u64[0] },
        Field{ "v18[1]", ctx.v18.u64[1] },
        Field{ "v19[0]", ctx.v19.u64[0] },
        Field{ "v19[1]", ctx.v19.u64[1] },
        Field{ "v20[0]", ctx.v20.u64[0] },
        Field{ "v20[1]", ctx.v20.u64[1] },
        Field{ "v21[0]", ctx.v21.u64[0] },
        Field{ "v21[1]", ctx.v21.u64[1] },
        Field{ "v22[0]", ctx.v22.u64[0] },
        Field{ "v22[1]", ctx.v22.u64[1] },
        Field{ "v23[0]", ctx.v23.u64[0] },
        Field{ "v23[1]", ctx.v23.u64[1] },
        Field{ "v24[0]", ctx.v24.u64[0] },
        Field{ "v24[1]", ctx.v24.u64[1] },
        Field{ "v25[0]", ctx.v25.u64[0] },
        Field{ "v25[1]", ctx.v25.u64[1] },
        Field{ "v26[0]", ctx.v26.u64[0] },
        Field{ "v26[1]", ctx.v26.u64[1] },
        Field{ "v27[0]", ctx.v27.u64[0] },
        Field{ "v27[1]", ctx.v27.u64[1] },
        Field{ "v28[0]", ctx.v28.u64[0] },
        Field{ "v28[1]", ctx.v28.u64[1] },
        Field{ "v29[0]", ctx.v29.u64[0] },
        Field{ "v29[1]", ctx.v29.u64[1] },
        Field{ "v30[0]", ctx.v30.u64[0] },
        Field{ "v30[1]", ctx.v30.u64[1] },
        Field{ "v31[0]", ctx.v31.u64[0] },
        Field{ "v31[1]", ctx.v31.u64[1] },
        Field{ "v32[0]", ctx.v32.u64[0] },
        Field{ "v32[1]", ctx.v32.u64[1] },
        Field{ "v33[0]", ctx.v33.u64[0] },
        Field{ "v33[1]", ctx.v33.u64[1] },
        Field{ "v34[0]", ctx.v34.u64[0] },
        Field{ "v34[1]", ctx.v34.u64[1] },
        Field{ "v35[0]", ctx.v35.u64[0] },
        Field{ "v35[1]", ctx.v35.u64[1] },
        Field{ "v36[0]", ctx.v36.u64[0] },
        Field{ "v36[1]", ctx.v36.u64[1] },
        Field{ "v37[0]", ctx.v37.u64[0] },
        Field{ "v37[1]", ctx.v37.u64[1] },
        Field{ "v38[0]", ctx.v38.u64[0] },
        Field{ "v38[1]", ctx.v38.u64[1] },
        Field{ "v39[0]", ctx.v39.u64[0] },
        Field{ "v39[1]", ctx.v39.u64[1] },
        Field{ "v40[0]", ctx.v40.u64[0] },
        Field{ "v40[1]", ctx.v40.u64[1] },
        Field{ "v41[0]", ctx.v41.u64[0] },
        Field{ "v41[1]", ctx.v41.u64[1] },
        Field{ "v42[0]", ctx.v42.u64[0] },
        Field{ "v42[1]", ctx.v42.u64[1] },
        Field{ "v43[0]", ctx.v43.u64[0] },
        Field{ "v43[1]", ctx.v43.u64[1] },
        Field{ "v44[0]", ctx.v44.u64[0] },
        Field{ "v44[1]", ctx.v44.u64[1] },
        Field{ "v45[0]", ctx.v45.u64[0] },
        Field{ "v45[1]", ctx.v45.u64[1] },
        Field{ "v46[0]", ctx.v46.u64[0] },
        Field{ "v46[1]", ctx.v46.u64[1] },
        Field{ "v47[0]", ctx.v47.u64[0] },
        Field{ "v47[1]", ctx.v47.u64[1] },
        Field{ "v48[0]", ctx.v48.u64[0] },
        Field{ "v48[1]", ctx.v48.u64[1] },
        Field{ "v49[0]", ctx.v49.u64[0] },
        Field{ "v49[1]", ctx.v49.u64[1] },
        Field{ "v50[0]", ctx.v50.u64[0] },
        Field{ "v50[1]", ctx.v50.u64[1] },
        Field{ "v51[0]", ctx.v51.u64[0] },
        Field{ "v51[1]", ctx.v51.u64[1] },
        Field{ "v52[0]", ctx.v52.u64[0] },
        Field{ "v52[1]", ctx.v52.u64[1] },
        Field{ "v53[0]", ctx.v53.u64[0] },
        Field{ "v53[1]", ctx.v53.u64[1] },
        Field{ "v54[0]", ctx.v54.u64[0] },
        Field{ "v54[1]", ctx.v54.u64[1] },
        Field{ "v55[0]", ctx.v55.u64[0] },
        Field{ "v55[1]", ctx.v55.u64[1] },
        Field{ "v56[0]", ctx.v56.u64[0] },
        Field{ "v56[1]", ctx.v56.u64[1] },
        Field{ "v57[0]", ctx.v57.u64[0] },
        Field{ "v57[1]", ctx.v57.u64[1] },
        Field{ "v58[0]", ctx.v58.u64[0] },
        Field{ "v58[1]", ctx.v58.u64[1] },
        Field{ "v59[0]", ctx.v59.u64[0] },
        Field{ "v59[1]", ctx.v59.u64[1] },
        Field{ "v60[0]", ctx.v60.u64[0] },
        Field{ "v60[1]", ctx.v60.u64[1] },
        Field{ "v61[0]", ctx.v61.u64[0] },
        Field{ "v61[1]", ctx.v61.u64[1] },
        Field{ "v62[0]", ctx.v62.u64[0] },
        Field{ "v62[1]", ctx.v62.u64[1] },
        Field{ "v63[0]", ctx.v63.u64[0] },
        Field{ "v63[1]", ctx.v63.u64[1] },
        Field{ "v64[0]", ctx.v64.u64[0] },
        Field{ "v64[1]", ctx.v64.u64[1] },
        Field{ "v65[0]", ctx.v65.u64[0] },
        Field{ "v65[1]", ctx.v65.u64[1] },
        Field{ "v66[0]", ctx.v66.u64[0] },
        Field{ "v66[1]", ctx.v66.u64[1] },
        Field{ "v67[0]", ctx.v67.u64[0] },
        Field{ "v67[1]", ctx.v67.u64[1] },
        Field{ "v68[0]", ctx.v68.u64[0] },
        Field{ "v68[1]", ctx.v68.u64[1] },
        Field{ "v69[0]", ctx.v69.u64[0] },
        Field{ "v69[1]", ctx.v69.u64[1] },
        Field{ "v70[0]", ctx.v70.u64[0] },
        Field{ "v70[1]", ctx.v70.u64[1] },
        Field{ "v71[0]", ctx.v71.u64[0] },
        Field{ "v71[1]", ctx.v71.u64[1] },
        Field{ "v72[0]", ctx.v72.u64[0] },
        Field{ "v72[1]", ctx.v72.u64[1] },
        Field{ "v73[0]", ctx.v73.u64[0] },
        Field{ "v73[1]", ctx.v73.u64[1] },
        Field{ "v74[0]", ctx.v74.u64[0] },
        Field{ "v74[1]", ctx.v74.u64[1] },
        Field{ "v75[0]", ctx.v75.u64[0] },
        Field{ "v75[1]", ctx.v75.u64[1] },
        Field{ "v76[0]", ctx.v76.u64[0] },
        Field{ "v76[1]", ctx.v76.u64[1] },
        Field{ "v77[0]", ctx.v77.u64[0] },
        Field{ "v77[1]", ctx.v77.u64[1] },
        Field{ "v78[0]", ctx.v78.u64[0] },
        Field{ "v78[1]", ctx.v78.u64[1] },
        Field{ "v79[0]", ctx.v79.u64[0] },
        Field{ "v79[1]", ctx.v79.u64[1] },
        Field{ "v80[0]", ctx.v80.u64[0] },
        Field{ "v80[1]", ctx.v80.u64[1] },
        Field{ "v81[0]", ctx.v81.u64[0] },
        Field{ "v81[1]", ctx.v81.u64[1] },
        Field{ "v82[0]", ctx.v82.u64[0] },
        Field{ "v82[1]", ctx.v82.u64[1] },
        Field{ "v83[0]", ctx.v83.u64[0] },
        Field{ "v83[1]", ctx.v83.u64[1] },
        Field{ "v84[0]", ctx.v84.u64[0] },
        Field{ "v84[1]", ctx.v84.u64[1] },
        Field{ "v85[0]", ctx.v85.u64[0] },
        Field{ "v85[1]", ctx.v85.u64[1] },
        Field{ "v86[0]", ctx.v86.u64[0] },
        Field{ "v86[1]", ctx.v86.u64[1] },
        Field{ "v87[0]", ctx.v87.u64[0] },
        Field{ "v87[1]", ctx.v87.u64[1] },
        Field{ "v88[0]", ctx.v88.u64[0] },
        Field{ "v88[1]", ctx.v88.u64[1] },
        Field{ "v89[0]", ctx.v89.u64[0] },
        Field{ "v89[1]", ctx.v89.u64[1] },
        Field{ "v90[0]", ctx.v90.u64[0] },
        Field{ "v90[1]", ctx.v90.u64[1] },
        Field{ "v91[0]", ctx.v91.u64[0] },
        Field{ "v91[1]", ctx.v91.u64[1] },
        Field{ "v92[0]", ctx.v92.u64[0] },
        Field{ "v92[1]", ctx.v92.u64[1] },
        Field{ "v93[0]", ctx.v93.u64[0] },
        Field{ "v93[1]", ctx.v93.u64[1] },
        Field{ "v94[0]", ctx.v94.u64[0] },
        Field{ "v94[1]", ctx.v94.u64[1] },
        Field{ "v95[0]", ctx.v95.u64[0] },
        Field{ "v95[1]", ctx.v95.u64[1] },
        Field{ "v96[0]", ctx.v96.u64[0] },
        Field{ "v96[1]", ctx.v96.u64[1] },
        Field{ "v97[0]", ctx.v97.u64[0] },
        Field{ "v97[1]", ctx.v97.u64[1] },
        Field{ "v98[0]", ctx.v98.u64[0] },
        Field{ "v98[1]", ctx.v98.u64[1] },
        Field{ "v99[0]", ctx.v99.u64[0] },
        Field{ "v99[1]", ctx.v99.u64[1] },
        Field{ "v100[0]", ctx.v100.u64[0] },
        Field{ "v100[1]", ctx.v100.u64[1] },
        Field{ "v101[0]", ctx.v101.u64[0] },
        Field{ "v101[1]", ctx.v101.u64[1] },
        Field{ "v102[0]", ctx.v102.u64[0] },
        Field{ "v102[1]", ctx.v102.u64[1] },
        Field{ "v103[0]", ctx.v103.u64[0] },
        Field{ "v103[1]", ctx.v103.u64[1] },
        Field{ "v104[0]", ctx.v104.u64[0] },
        Field{ "v104[1]", ctx.v104.u64[1] },
        Field{ "v105[0]", ctx.v105.u64[0] },
        Field{ "v105[1]", ctx.v105.u64[1] },
        Field{ "v106[0]", ctx.v106.u64[0] },
        Field{ "v106[1]", ctx.v106.u64[1] },
        Field{ "v107[0]", ctx.v107.u64[0] },
        Field{ "v107[1]", ctx.v107.u64[1] },
        Field{ "v108[0]", ctx.v108.u64[0] },
        Field{ "v108[1]", ctx.v108.u64[1] },
        Field{ "v109[0]", ctx.v109.u64[0] },
        Field{ "v109[1]", ctx.v109.u64[1] },
        Field{ "v110[0]", ctx.v110.u64[0] },
        Field{ "v110[1]", ctx.v110.u64[1] },
        Field{ "v111[0]", ctx.v111.u64[0] },
        Field{ "v111[1]", ctx.v111.u64[1] },
        Field{ "v112[0]", ctx.v112.u64[0] },
        Field{ "v112[1]", ctx.v112.u64[1] },
        Field{ "v113[0]", ctx.v113.u64[0] },
        Field{ "v113[1]", ctx.v113.u64[1] },
        Field{ "v114[0]", ctx.v114.u64[0] },
        Field{ "v114[1]", ctx.v114.u64[1] },
        Field{ "v115[0]", ctx.v115.u64[0] },
        Field{ "v115[1]", ctx.v115.u64[1] },
        Field{ "v116[0]", ctx.v116.u64[0] },
        Field{ "v116[1]", ctx.v116.u64[1] },
        Field{ "v117[0]", ctx.v117.u64[0] },
        Field{ "v117[1]", ctx.v117.u64[1] },
        Field{ "v118[0]", ctx.v118.u64[0] },
        Field{ "v118[1]", ctx.v118.u64[1] },
        Field{ "v119[0]", ctx.v119.u64[0] },
        Field{ "v119[1]", ctx.v119.u64[1] },
        Field{ "v120[0]", ctx.v120.u64[0] },
        Field{ "v120[1]", ctx.v120.u64[1] },
        Field{ "v121[0]", ctx.v121.u64[0] },
        Field{ "v121[1]", ctx.v121.u64[1] },
        Field{ "v122[0]", ctx.v122.u64[0] },
        Field{ "v122[1]", ctx.v122.u64[1] },
        Field{ "v123[0]", ctx.v123.u64[0] },
        Field{ "v123[1]", ctx.v123.u64[1] },
        Field{ "v124[0]", ctx.v124.u64[0] },
        Field{ "v124[1]", ctx.v124.u64[1] },
        Field{ "v125[0]", ctx.v125.u64[0] },
        Field{ "v125[1]", ctx.v125.u64[1] },
        Field{ "v126[0]", ctx.v126.u64[0] },
        Field{ "v126[1]", ctx.v126.u64[1] },
        Field{ "v127[0]", ctx.v127.u64[0] },
        Field{ "v127[1]", ctx.v127.u64[1] },
        Field{ "cr0.lt", ctx.cr0.lt },
        Field{ "cr0.gt", ctx.cr0.gt },
        Field{ "cr0.eq", ctx.cr0.eq },
        Field{ "cr0.so", ctx.cr0.so },
        Field{ "cr1.lt", ctx.cr1.lt },
        Field{ "cr1.gt", ctx.cr1.gt },
        Field{ "cr1.eq", ctx.cr1.eq },
        Field{ "cr1.so", ctx.cr1.so },
        Field{ "cr2.lt", ctx.cr2.lt },
        Field{ "cr2.gt", ctx.cr2.gt },
        Field{ "cr2.eq", ctx.cr2.eq },
        Field{ "cr2.so", ctx.cr2.so },
        Field{ "cr3.lt", ctx.cr3.lt },
        Field{ "cr3.gt", ctx.cr3.gt },
        Field{ "cr3.eq", ctx.cr3.eq },
        Field{ "cr3.so", ctx.cr3.so },
        Field{ "cr4.lt", ctx.cr4.lt },
        Field{ "cr4.gt", ctx.cr4.gt },
        Field{ "cr4.eq", ctx.cr4.eq },
        Field{ "cr4.so", ctx.cr4.so },
        Field{ "cr5.lt", ctx.cr5.lt },
        Field{ "cr5.gt", ctx.cr5.gt },
        Field{ "cr5.eq", ctx.cr5.eq },
        Field{ "cr5.so", ctx.cr5.so },
        Field{ "cr6.lt", ctx.cr6.lt },
        Field{ "cr6.gt", ctx.cr6.gt },
        Field{ "cr6.eq", ctx.cr6.eq },
        Field{ "cr6.so", ctx.cr6.so },
        Field{ "cr7.lt", ctx.cr7.lt },
        Field{ "cr7.gt", ctx.cr7.gt },
        Field{ "cr7.eq", ctx.cr7.eq },
        Field{ "cr7.so", ctx.cr7.so },
        Field{ "lr", ctx.lr },
        Field{ "ctr", ctx.ctr.u64 },
        Field{ "xer.so", ctx.xer.so },
        Field{ "xer.ov", ctx.xer.ov },
        Field{ "xer.ca", ctx.xer.ca },
        Field{ "reserved", ctx.reserved.u64 },
        Field{ "msr", ctx.msr },
        Field{ "fpscr", ctx.fpscr.csr },
        Field{ "vscr_sat", ctx.vscr_sat },
        Field{ "last_indirect_target", ctx.last_indirect_target }
    };
}

rex::memory::Memory* kernelMemory()
{
    auto* kernel = rex::system::kernel_state();
    return kernel ? kernel->memory() : nullptr;
}

bool allows(std::uint32_t address, rex::memory::PageAccess needed)
{
    auto* memory = kernelMemory();
    if (!memory || address < 0x10000 || address > UINT32_MAX - 3 || (address & 3))
        return false;
    auto* heap = memory->LookupHeap(address);
    if (!heap || memory->LookupHeap(address + 3) != heap)
        return false;
    const auto access = static_cast<unsigned>(heap->QueryRangeAccess(address, address + 3));
    return (access & static_cast<unsigned>(needed)) == static_cast<unsigned>(needed);
}

TilingRestoreMemory productionMemory()
{
    return {
        [](std::uint32_t address, std::uint32_t& value)
        {
            if (!allows(address, rex::memory::PageAccess::kReadOnly))
                return false;
            std::array<std::uint8_t, 4> bytes{};
            if (!CopyGuestGuarded(bytes.data(), kernelMemory()->TranslateVirtual<const std::uint8_t*>(address), bytes.size()))
                return false;
            value = (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) | (std::uint32_t(bytes[2]) << 8) | bytes[3];
            return true;
        },
        [](std::uint32_t address, std::uint32_t bytes)
        {
            return bytes == 4 && allows(address, rex::memory::PageAccess::kReadWrite);
        },
        [](std::uint32_t address, std::uint32_t bytes) -> std::optional<std::uint64_t>
        {
            auto* memory = kernelMemory();
            if (!memory || bytes != 4 || address > UINT32_MAX - 3)
                return std::nullopt;
            auto* heap = memory->LookupHeap(address);
            if (!heap || memory->LookupHeap(address + 3) != heap)
                return std::nullopt;
            if (heap->heap_type() == rex::memory::HeapType::kGuestPhysical)
            {
                const auto physical = memory->GetPhysicalAddress(address);
                if (physical == UINT32_MAX || std::uint64_t{ physical } + 3 != memory->GetPhysicalAddress(address + 3))
                    return std::nullopt;
                return physical;
            }
            if (heap->heap_type() == rex::memory::HeapType::kGuestVirtual)
                return (std::uint64_t{ 1 } << 32) + address;
            return std::nullopt;
        },
        [](const TilingRestoreWrite& write)
        {
            if (!allows(write.address, rex::memory::PageAccess::kReadWrite))
                return false;
            const std::array<std::uint8_t, 4> bytes{
                static_cast<std::uint8_t>(write.value >> 24), static_cast<std::uint8_t>(write.value >> 16), static_cast<std::uint8_t>(write.value >> 8), static_cast<std::uint8_t>(write.value)
            };
            if (!CopyGuestGuarded(kernelMemory()->TranslateVirtual<std::uint8_t*>(write.address), bytes.data(), bytes.size()))
                return false;
            std::atomic_thread_fence(std::memory_order_release);
            return true;
        }
    };
}

struct Mismatch
{
    std::uint32_t       device = 0, caller = 0, address = 0;
    std::uint64_t       call = 0, sequence = 0, expected = 0, actual = 0;
    std::string_view    field;
    TilingRestoreInputs inputs;
};

std::array<Mismatch, 32> records{};
std::size_t              recordCount = 0;
std::uint64_t            sequence    = 0;

struct Active
{
    const void*                   entry = nullptr;
    std::array<std::uint64_t, 13> ranges{};
    std::atomic<bool>*            uncertain = nullptr;
    bool                          reported  = false;
    bool                          checking  = true;
};

std::array<Active, 64> active{};

class ReplayScope
{
public:
    explicit ReplayScope(const std::array<std::uint64_t, 13>& ranges)
    {
        std::lock_guard lock(countsMutex);
        for (auto& call : active)
            if (call.entry)
                for (const auto a : call.ranges)
                    for (const auto b : ranges)
                        if (a <= b + 3 && b <= a + 3)
                            *call.uncertain = true;
        for (auto& slot : active)
            if (!slot.entry)
            {
                slot = { this, ranges, &uncertain, false, false };
                return;
            }
        ++interval.eventDrop;
        ++lifetime.eventDrop;
    }

    ~ReplayScope()
    {
        std::lock_guard lock(countsMutex);
        for (auto& slot : active)
            if (slot.entry == this)
            {
                slot = {};
                return;
            }
    }

private:
    std::atomic<bool> uncertain{ false };
};

[[noreturn]] void replayFault() noexcept
{
    increment(&TilingRestoreCounts::replayFault);
    std::fputs("tr_replay_fault: B118 commit failed; Original forbidden\n", stderr);
    std::fflush(stderr);
    std::terminate();
}

} // namespace

TilingRestorePlan DeriveTilingRestore(std::uint32_t g, const TilingRestoreInputs& inputs) noexcept
{
    const auto& s     = inputs.saved;
    const auto  limit = std::uint64_t{ s[1] } - 160;
    return { { TilingRestoreWrite{ g + 0x30, s[0] }, { g + 0x34, s[1] }, { g + 0x3A50, s[2] }, { g + 0x3A4C, s[3] }, { g + 0x3A44, s[4] }, { g + 0x3A48, s[5] }, { g + 0x38, static_cast<std::uint32_t>(limit) } },
             { s[5], s[4], s[3], limit, s[2], s[0], s[1] } };
}

bool TilingRestoreConsumeRequested() noexcept
{
    return REXCVAR_GET(native_consume_tiling_restore);
}

bool TilingRestoreExitCheckRequested() noexcept
{
    return REXCVAR_GET(native_tiling_restore_exit_check);
}

bool TilingRestoreReportingRequested() noexcept
{
    return TilingRestoreConsumeRequested() || TilingRestoreExitCheckRequested();
}

TilingRestoreInputs ReadTilingRestoreInputs(const TilingRestoreMemory& memory, std::uint32_t g)
{
    TilingRestoreInputs inputs;
    // B118..B130: lwz r11,r10,r9; addi r8,r11,-160; lwz r7,r6,r5.
    for (const auto i : { 1U, 0U, 2U, 3U, 4U, 5U })
        if (memory.read && memory.read(g + offsets[i], inputs.saved[i]))
            inputs.valid |= static_cast<std::uint8_t>(1U << i);
    return inputs;
}

std::optional<TilingRestoreFallback> PreflightTilingRestore(const TilingRestoreState& state, const TilingRestoreMemory& memory, std::uint32_t g, TilingRestorePlan& plan)
{
    if (state.checking)
        return TilingRestoreFallback::ExitCheck;
    if (state.diagnostics)
        return TilingRestoreFallback::Diagnostics;
    if (!state.stubs)
        return TilingRestoreFallback::Stubs;
    if (!validDevice(g))
        return TilingRestoreFallback::Mapping;
    if (!state.device)
        return TilingRestoreFallback::Device;
    std::array<std::uint64_t, 13> ranges{};
    if (const auto refused = identities(memory, g, ranges))
        return refused;
    const auto inputs = ReadTilingRestoreInputs(memory, g);
    if (inputs.valid != 0x3F)
        return TilingRestoreFallback::Unreadable;
    plan = DeriveTilingRestore(g, inputs);
    for (const auto& write : plan.writes)
        if (!memory.writable || !memory.writable(write.address, 4))
            return TilingRestoreFallback::Unwritable;
    return std::nullopt;
}

bool ConsumeTilingRestoreWith(const TilingRestoreState& state, const TilingRestoreMemory& memory, PPCContext& ctx) noexcept
{
    if (!state.requested)
        return false;
    bool committing = false;
    try
    {
        TilingRestorePlan plan;
        if (const auto refused = PreflightTilingRestore(state, memory, ctx.r3.u32, plan))
        {
            std::lock_guard lock(countsMutex);
            ++interval.fallback[static_cast<std::size_t>(*refused)];
            ++lifetime.fallback[static_cast<std::size_t>(*refused)];
            return false;
        }
        std::array<std::uint64_t, 13> ranges{};
        if (identities(memory, ctx.r3.u32, ranges))
        {
            std::lock_guard lock(countsMutex);
            ++interval.fallback[static_cast<std::size_t>(TilingRestoreFallback::Mapping)];
            ++lifetime.fallback[static_cast<std::size_t>(TilingRestoreFallback::Mapping)];
            return false;
        }
        const ReplayScope replay(ranges);
        committing = true;
        for (std::size_t i = 0; i < plan.writes.size(); ++i)
        {
            const auto& write = plan.writes[i];
            if (memory.identity(write.address, 4) != std::optional<std::uint64_t>{ ranges[i + 6] } || !memory.write || !memory.write(write))
                replayFault();
        }
        applyRegisters(ctx, plan);
        increment(&TilingRestoreCounts::consumed);
        return true;
    }
    catch (...)
    {
        if (committing)
            replayFault();
        std::lock_guard lock(countsMutex);
        ++interval.fallback[static_cast<std::size_t>(TilingRestoreFallback::Mapping)];
        ++lifetime.fallback[static_cast<std::size_t>(TilingRestoreFallback::Mapping)];
        return false;
    }
}

bool ConsumeTilingRestore(PPCContext& ctx, bool diagnostics) noexcept
{
    if (!TilingRestoreConsumeRequested())
        return false;
    const bool checking = TilingRestoreExitCheckRequested();
    // Checking and diagnostics refuse before publication or guest access.
    if (checking || diagnostics)
        return ConsumeTilingRestoreWith({ true, checking, false, false, diagnostics }, {}, ctx);
    const RingStubLease lease;
    return ConsumeTilingRestoreWith({ true, false, static_cast<bool>(lease), lease && RingStubsReady(ctx.r3.u32), false }, productionMemory(), ctx);
}

void NoteTilingRestoreEnter() noexcept
{
    increment(&TilingRestoreCounts::enter);
}

void NoteTilingRestoreOriginal() noexcept
{
    increment(&TilingRestoreCounts::original);
}

struct TilingRestoreExitScope::Entry
{
    const PPCContext*             context = nullptr;
    TilingRestoreMemory           memory;
    PPCContext                    expected{};
    TilingRestoreInputs           inputs;
    TilingRestorePlan             plan;
    std::array<std::uint64_t, 13> ranges{};
    std::uint32_t                 device = 0, caller = 0;
    std::uint64_t                 call       = 0;
    int                           exceptions = 0;
    bool                          unreadable = false, registered = false;
    std::atomic<bool>             uncertain{ false };
};

TilingRestoreExitScope::TilingRestoreExitScope(const PPCContext& ctx) noexcept
: TilingRestoreExitScope(ctx, TilingRestoreExitCheckRequested() ? productionMemory() : TilingRestoreMemory{}, TilingRestoreExitCheckRequested())
{
}

TilingRestoreExitScope::TilingRestoreExitScope(const PPCContext& ctx, const TilingRestoreMemory& memory, bool enabled) noexcept
{
    if (!enabled)
        return;
    try
    {
        entry        = std::make_unique<Entry>();
        auto& e      = *entry;
        e.context    = &ctx;
        e.expected   = ctx;
        e.device     = ctx.r3.u32;
        e.caller     = static_cast<std::uint32_t>(ctx.lr);
        e.memory     = memory;
        e.exceptions = std::uncaught_exceptions();
        e.uncertain  = identities(memory, e.device, e.ranges).has_value();
        if (!e.uncertain)
        {
            e.inputs     = ReadTilingRestoreInputs(memory, e.device);
            e.unreadable = e.inputs.valid != 0x3F;
            e.plan       = DeriveTilingRestore(e.device, e.inputs);
            applyRegisters(e.expected, e.plan);
        }
        std::lock_guard lock(countsMutex);
        e.call = ++nextCall;
        for (auto& other : active)
            if (other.entry)
                for (const auto a : other.ranges)
                    for (const auto b : e.ranges)
                        if (a <= b + 3 && b <= a + 3)
                        {
                            e.uncertain      = true;
                            *other.uncertain = true;
                        }
        for (auto& slot : active)
            if (!slot.entry)
            {
                slot         = { &e, e.ranges, &e.uncertain, false };
                e.registered = true;
                break;
            }
        if (!e.registered)
        {
            ++interval.eventDrop;
            ++lifetime.eventDrop;
            e.uncertain = true;
        }
    }
    catch (...)
    {
        entry.reset();
        increment(&TilingRestoreCounts::eventDrop);
    }
}

TilingRestoreExitScope::~TilingRestoreExitScope()
{
    if (!entry)
        return;
    auto&    e = *entry;
    Mismatch mismatch;
    bool     wordBad = false, abiBad = false;
    bool     unmatched = std::uncaught_exceptions() != e.exceptions;
    try
    {
        std::array<std::uint64_t, 13> now{};
        if (identities(e.memory, e.device, now) || now != e.ranges)
            e.uncertain = true;
        if (!e.uncertain && !e.unreadable && !unmatched)
        {
            const auto saved = ReadTilingRestoreInputs(e.memory, e.device);
            e.unreadable     = saved.valid != 0x3F;
            if (!e.unreadable && saved.saved != e.inputs.saved)
                e.uncertain = true;
            for (const auto& write : e.plan.writes)
            {
                std::uint32_t value = 0;
                if (!e.memory.read(write.address, value))
                    e.unreadable = true;
                else if (value != write.value)
                {
                    if (!wordBad)
                        mismatch = { e.device, e.caller, write.address, e.call, 0, write.value, value, {}, e.inputs };
                    wordBad = true;
                }
            }
            const auto expected = semanticFields(e.expected), actual = semanticFields(*e.context);
            for (std::size_t i = 0; i < expected.size(); ++i)
                if (expected[i].value != actual[i].value)
                {
                    if (!wordBad && !abiBad)
                        mismatch = { e.device, e.caller, 0, e.call, 0, expected[i].value, actual[i].value, expected[i].name, e.inputs };
                    abiBad = true;
                }
        }
    }
    catch (...)
    {
        e.unreadable = true;
    }
    std::lock_guard lock(countsMutex);
    bool            alreadyReported = false;
    for (auto& slot : active)
        if (slot.entry == &e)
        {
            alreadyReported = slot.reported;
            slot            = {};
            break;
        }
    const auto count = [](std::uint64_t TilingRestoreCounts::* field)
    {
        ++(interval.*field);
        ++(lifetime.*field);
    };
    if (unmatched)
    {
        if (!alreadyReported)
            count(&TilingRestoreCounts::exitUnmatched);
    }
    else if (e.unreadable)
        count(&TilingRestoreCounts::exitUnreadable);
    else if (e.uncertain)
        count(&TilingRestoreCounts::exitIndeterminate);
    else
    {
        if (wordBad)
            count(&TilingRestoreCounts::exitMismatch);
        if (abiBad)
            count(&TilingRestoreCounts::exitAbiMismatch);
        if (!wordBad && !abiBad)
            count(&TilingRestoreCounts::exitChecked);
        else if (recordCount < records.size())
        {
            mismatch.sequence      = ++sequence;
            records[recordCount++] = mismatch;
        }
        else
            count(&TilingRestoreCounts::eventDrop);
    }
}

TilingRestoreCounts TakeTilingRestoreCounts(bool shutdown) noexcept
{
    std::lock_guard lock(countsMutex);
    if (shutdown)
        for (auto& slot : active)
            if (slot.entry && slot.checking && !slot.reported)
            {
                ++interval.exitUnmatched;
                ++lifetime.exitUnmatched;
                slot.reported = true;
            }
    const auto result = interval;
    interval          = {};
    return result;
}

TilingRestoreCounts TilingRestoreLifetimeCounts() noexcept
{
    std::lock_guard lock(countsMutex);
    return lifetime;
}

std::string FormatTilingRestoreCounts(const TilingRestoreCounts& c)
{
    std::string text = std::format(" tr_enter={} tr_consumed={} tr_original={}", c.enter, c.consumed, c.original);
    for (std::size_t i = 0; i < c.fallback.size(); ++i)
        text += std::format(" tr_fallback_{}={}", kTilingRestoreFallbackNames[i], c.fallback[i]);
    return text + std::format(" tr_exit_checked={} tr_exit_mismatch={} tr_exit_abi_mismatch={} tr_exit_unreadable={} tr_exit_indeterminate={} tr_exit_unmatched={} tr_event_drop={} tr_replay_fault={}",
                              c.exitChecked,
                              c.exitMismatch,
                              c.exitAbiMismatch,
                              c.exitUnreadable,
                              c.exitIndeterminate,
                              c.exitUnmatched,
                              c.eventDrop,
                              c.replayFault);
}

std::string ReportTilingRestore(bool shutdown) noexcept
{
    if (!TilingRestoreReportingRequested())
        return {};
    try
    {
        const auto               counts = TakeTilingRestoreCounts(shutdown);
        std::array<Mismatch, 32> drained{};
        std::size_t              size = 0;
        {
            std::lock_guard lock(countsMutex);
            size = recordCount;
            std::copy_n(records.begin(), size, drained.begin());
            recordCount = 0;
        }
        for (std::size_t i = 0; i < size; ++i)
        {
            const auto& r = drained[i];
            REXLOG_WARN("Native tiling restore: device={:08X} caller={:08X} call={} sequence={} valid={:02X} saved={:08X},{:08X},{:08X},{:08X},{:08X},{:08X} address={:08X} register={} expected={:016X} actual={:016X}",
                        r.device,
                        r.caller,
                        r.call,
                        r.sequence,
                        r.inputs.valid,
                        r.inputs.saved[0],
                        r.inputs.saved[1],
                        r.inputs.saved[2],
                        r.inputs.saved[3],
                        r.inputs.saved[4],
                        r.inputs.saved[5],
                        r.address,
                        r.field,
                        r.expected,
                        r.actual);
        }
        if (shutdown)
            REXLOG_INFO("Native tiling restore shutdown:{} endpoint_equivalence=retained_ownership", FormatTilingRestoreCounts(TilingRestoreLifetimeCounts()));
        return FormatTilingRestoreCounts(counts);
    }
    catch (...)
    {
        increment(&TilingRestoreCounts::eventDrop);
        return {};
    }
}

} // namespace rerevved::gpu::guest_d3d

"""Static contracts for the opt-in title-owned native renderer."""

from __future__ import annotations

import json
import os
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
APP_CPP = (ROOT / "src" / "app.cpp").read_text(encoding="ascii")
NATIVE_CPP = (
    ROOT / "src" / "gpu" / "d3d12" / "native_renderer_d3d12.cpp"
).read_text(
    encoding="ascii"
)
GUEST_SERVICE_CPP = (ROOT / "src" / "gpu" / "guest_gpu_service.cpp").read_text(
    encoding="ascii"
)
COMPAT_CPP = (ROOT / "src" / "compat_hooks.cpp").read_text(encoding="ascii")
PASSIVE_TRACE_CPP = (
    ROOT / "src" / "gpu" / "diagnostics" / "native_renderer_passive_trace.cpp"
).read_text(encoding="ascii")
PASSIVE_TRACE_H = (
    ROOT / "src" / "gpu" / "diagnostics" / "native_renderer_passive_trace.h"
).read_text(encoding="ascii")
HOOKS = (ROOT / "config" / "rerevved_hooks.toml").read_text(encoding="ascii")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="ascii")
LOCK = json.loads((ROOT / "rexglue-sdk.lock.json").read_text(encoding="ascii"))
GUEST_SERVICE_FILES = {
    ROOT / "src" / "gpu" / "guest_gpu_service.cpp",
    ROOT / "src" / "gpu" / "guest_gpu_service.h",
}


class NativeRendererIntegrationTests(unittest.TestCase):
    def test_renderer_is_init_only_opt_in(self) -> None:
        self.assertIn('REXCVAR_DEFINE_STRING(renderer, "xenos"', APP_CPP)
        self.assertIn('.allowed({ "xenos", "native" })', APP_CPP)
        self.assertIn("Lifecycle::kInitOnly", APP_CPP)
        self.assertIn("ParseRendererBackend(REXCVAR_GET(renderer))", APP_CPP)
        self.assertIn(
            "config.graphics = "
            "std::make_unique<rerevved::gpu::NativeGuestGpuService>();",
            APP_CPP,
        )
        self.assertIn("config.gpu_plugin.clear();", APP_CPP)

    def test_native_guest_gpu_service_is_native_only(self) -> None:
        consumers = []
        source_suffixes = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".inc"}
        for source in (ROOT / "src").rglob("*"):
            if source in GUEST_SERVICE_FILES or source.suffix not in source_suffixes:
                continue
            if "NativeGuestGpuService" in source.read_text(encoding="ascii"):
                consumers.append(source.relative_to(ROOT).as_posix())
        self.assertEqual(consumers, ["src/app.cpp"])

        pre_setup = APP_CPP[
            APP_CPP.index("void ReRevvedApp::OnPreSetup") :
            APP_CPP.index("void ReRevvedApp::OnConfigurePaths")
        ]
        self.assertIn("RendererBackend::Native", pre_setup)
        self.assertIn("std::make_unique<rerevved::gpu::NativeGuestGpuService>()", pre_setup)
        self.assertIn("config.gpu_plugin.clear();", pre_setup)

    def test_config_is_loaded_before_selection_and_detach(self) -> None:
        base_setup = APP_CPP.index("rex::ReXApp::SetupEnvironment()")
        selection = APP_CPP.index("ParseRendererBackend(REXCVAR_GET(renderer))")
        xenos_default = APP_CPP.index(
            'rex::cvar::SetFlagByName("gpu_plugin", "xenos")'
        )
        self.assertLess(base_setup, selection)
        self.assertLess(selection, xenos_default)

        sdk = os.environ.get("REREVVED_SDK_SOURCE")
        if not sdk:
            sibling = ROOT.parent / "rerevved-rexglue-sdk"
            if sibling.is_dir():
                sdk = str(sibling)
        if not sdk:
            self.skipTest("locked SDK source is not available")

        source = subprocess.run(
            [
                "git",
                "-C",
                sdk,
                "show",
                f'{LOCK["commit"]}:src/ui/rex_app.cpp',
            ],
            check=True,
            capture_output=True,
            text=True,
            encoding="utf-8",
        ).stdout
        setup_environment = source.index("bool ReXApp::SetupEnvironment()")
        load_config = source.index("rex::cvar::LoadConfig(config_path_)")
        setup_presentation = source.index("bool ReXApp::SetupPresentation()")
        pre_setup = source.index("OnPreSetup(config_);", setup_presentation)
        plugin_load = source.index("LoadGpuPlugin", pre_setup)
        self.assertLess(setup_environment, load_config)
        self.assertLess(load_config, setup_presentation)
        self.assertLess(setup_presentation, pre_setup)
        self.assertLess(pre_setup, plugin_load)

    def test_guest_gpu_service_holds_unconsumed_ring_work(self) -> None:
        thread_create = GUEST_SERVICE_CPP.index("vblank_worker_thread->Create()")
        mmio_registration = GUEST_SERVICE_CPP.index("AddVirtualMappedRange")
        self.assertLess(thread_create, mmio_registration)
        self.assertIn("kGpuMmioBaseAddress = 0x7FC80000", GUEST_SERVICE_CPP)
        self.assertIn("kGpuMmioAddressMask = 0xFFFF0000", GUEST_SERVICE_CPP)
        self.assertIn("kGpuMmioSize        = 0x0000FFFF", GUEST_SERVICE_CPP)
        self.assertIn("reg == kCpRbWptr", GUEST_SERVICE_CPP)
        self.assertIn("command consumption is unavailable", GUEST_SERVICE_CPP)
        self.assertNotIn("UpdateWritePointer", GUEST_SERVICE_CPP)
        self.assertNotIn("TranslatePhysical", GUEST_SERVICE_CPP)
        self.assertNotIn("store_and_swap", GUEST_SERVICE_CPP)

    def test_guest_gpu_service_vblank_abi_matches_sdk(self) -> None:
        self.assertIn("thread->SetActiveCpu(kDefaultCpu);", GUEST_SERVICE_CPP)
        self.assertIn("uint64_t args[] = { 0, user_data };", GUEST_SERVICE_CPP)
        self.assertIn("function_dispatcher->ExecuteInterrupt", GUEST_SERVICE_CPP)

    def test_guest_gpu_service_shutdown_bounds_concurrency(self) -> None:
        self.assertIn("kMaxVblankCatchUp = 4", GUEST_SERVICE_CPP)
        self.assertIn("catch_up_count < kMaxVblankCatchUp", GUEST_SERVICE_CPP)
        self.assertIn("shutdown_started", GUEST_SERVICE_CPP)

    def test_guest_gpu_service_mmio_after_shutdown_is_detached(self) -> None:
        self.assertIn("static MmioBridge bridge;", GUEST_SERVICE_CPP)
        self.assertIn("&GetMmioBridge()", GUEST_SERVICE_CPP)
        self.assertNotIn(
            "kGpuMmioSize,\n            this,",
            GUEST_SERVICE_CPP,
        )
        self.assertIn("bridge.service = nullptr;", GUEST_SERVICE_CPP)
        self.assertIn("bridge.callbacks_in_flight == 0", GUEST_SERVICE_CPP)
        self.assertIn("MmioCallbackLease lease(callback_context);", GUEST_SERVICE_CPP)
        self.assertIn("UnbindMmioBridge(this);", GUEST_SERVICE_CPP)

    def test_native_surface_is_created_after_sdk_window_open(self) -> None:
        presentation = APP_CPP.index("bool ReRevvedApp::SetupPresentation()")
        base_presentation = APP_CPP.index(
            "rex::ReXApp::SetupPresentation()", presentation
        )
        native_initialize = APP_CPP.index("native_renderer_.Initialize(*window())")
        self.assertLess(base_presentation, native_initialize)
        self.assertIn('"ReRevved - Native D3D12"', APP_CPP)

    def test_d3d12_frame_and_failure_contract(self) -> None:
        self.assertRegex(
            NATIVE_CPP,
            r"constexpr\s+std::uint32_t\s+kFrameCount\s*=\s*2;",
        )
        self.assertLess(
            NATIVE_CPP.index("EnableDred();"),
            NATIVE_CPP.index("D3D12CreateDevice"),
        )
        execute = NATIVE_CPP.index("ExecuteCommandLists")
        signal = NATIVE_CPP.index("queue->Signal", execute)
        present = NATIVE_CPP.index("swap_chain->Present", signal)
        self.assertLess(execute, signal)
        self.assertLess(signal, present)
        resize_failure = NATIVE_CPP.index('"swap chain resize"')
        self.assertNotIn(
            "ShutdownOnRendererThread", NATIVE_CPP[resize_failure : resize_failure + 250]
        )
        self.assertIn("GetDeviceRemovedReason", NATIVE_CPP)

    def test_d3d12_lifecycle_is_owned_by_one_renderer_thread(self) -> None:
        self.assertIn("std::thread             renderer_thread;", NATIVE_CPP)
        self.assertIn("NativeRendererD3D12::RendererThreadMain", NATIVE_CPP)
        self.assertIn("InitializeOnRendererThread", NATIVE_CPP)
        self.assertIn("PresentOnRendererThread", NATIVE_CPP)
        self.assertIn("ResizeOnRendererThread", NATIVE_CPP)
        self.assertIn("ShutdownOnRendererThread", NATIVE_CPP)

        resize = NATIVE_CPP[
            NATIVE_CPP.index("bool NativeRendererD3D12::Resize(") :
            NATIVE_CPP.index("void NativeRendererD3D12::Shutdown()")
        ]
        self.assertIn("requested_width", resize)
        self.assertIn("requested_height", resize)
        self.assertIn("resize_pending", resize)
        self.assertIn("state_cv.notify_one();", resize)
        self.assertNotIn("state_cv.wait", resize)
        self.assertNotIn("ResizeBuffers", resize)
        self.assertNotIn("WaitForGpu", resize)

        shutdown = NATIVE_CPP[NATIVE_CPP.index("void NativeRendererD3D12::Shutdown()") :]
        self.assertIn("stop_requested", shutdown)
        self.assertIn("renderer_thread.join();", shutdown)
        self.assertIn("state_cv.notify_all();", shutdown)
        self.assertNotIn("WaitForGpu", shutdown)

        worker = NATIVE_CPP[
            NATIVE_CPP.index("void NativeRendererD3D12::RendererThreadMain") :
            NATIVE_CPP.index("bool NativeRendererD3D12::Initialize(")
        ]
        self.assertLess(worker.index("state_cv.wait"), worker.index("ShutdownOnRendererThread"))
        self.assertLess(worker.index("stop_requested"), worker.index("ShutdownOnRendererThread"))
        self.assertIn("request_deferred_quit", NATIVE_CPP)
        self.assertIn("app_context->RequestDeferredQuit();", NATIVE_CPP)

    def test_fence_drain_is_bounded_and_fail_closed(self) -> None:
        self.assertIn("kFenceWaitTimeoutMs", NATIVE_CPP)
        self.assertIn("WAIT_TIMEOUT", NATIVE_CPP)
        self.assertIn("fence wait timed out after", NATIVE_CPP)
        self.assertNotIn("WaitForSingleObject(fence_event, INFINITE)", NATIVE_CPP)
        self.assertNotIn("ShutdownOnRendererThread(false);", NATIVE_CPP)
        self.assertIn("abandoning GPU objects until process exit", NATIVE_CPP)
        self.assertIn("gpu_objects_abandoned.store(true", NATIVE_CPP)
        self.assertIn("cannot reinitialize after abandoning", NATIVE_CPP)
        self.assertIn("app_context->RequestDeferredQuit();", NATIVE_CPP)

    def test_ui_resize_callback_only_latches_renderer_request(self) -> None:
        resize_callback = APP_CPP[
            APP_CPP.index("void ReRevvedApp::OnWindowPixelSizeChanged") :
            APP_CPP.index("void ReRevvedApp::OnKeyDown")
        ]
        self.assertIn("native_renderer_.Resize(pixel_width, pixel_height)", resize_callback)
        self.assertNotIn("ResizeBuffers", resize_callback)
        self.assertNotIn("WaitForGpu", resize_callback)

    def test_windows_links_are_explicit(self) -> None:
        self.assertIn(
            "target_link_libraries(rerevved PRIVATE d3d12 dxgi dxguid)", CMAKE
        )

    def test_passive_trace_is_default_off_and_xenos_only(self) -> None:
        self.assertIn(
            'REXCVAR_DEFINE_STRING(native_renderer_passive_trace_output, ""',
            APP_CPP,
        )
        self.assertIn("RendererBackend::Xenos", APP_CPP)
        self.assertIn('std::filesystem::absolute("out", error)', APP_CPP)
        self.assertIn("ContainsExistingReparsePoint", APP_CPP)
        self.assertIn("std::filesystem::weakly_canonical", APP_CPP)
        self.assertIn("cannot share a native-renderer coverage run", APP_CPP)
        self.assertLess(
            APP_CPP.index('REXLOG_INFO("NRD-COVERAGE-BEGIN")'),
            APP_CPP.index("GetPassiveTraceBuffer().Start"),
        )
        self.assertIn("GetPassiveTraceBuffer().Start", APP_CPP)
        self.assertIn("GetPassiveTraceBuffer().enabled()", COMPAT_CPP)
        self.assertIn("if (!PassiveTraceEnabled())", COMPAT_CPP)

    def test_passive_trace_is_scoped_to_the_known_swap_callers(self) -> None:
        reservation = COMPAT_CPP[
            COMPAT_CPP.index("void ReRevvedTraceReservationEnter") :
            COMPAT_CPP.index("void ReRevvedTraceReservationReturn")
        ]
        self.assertIn("traced_vdswap_owner_active", reservation)
        self.assertIn("r4.u32 != 64", reservation)
        self.assertIn("RecordCallerTracePoint", COMPAT_CPP)
        self.assertIn("traced_caller == TracedCaller::kOrdinary", COMPAT_CPP)
        self.assertIn("traced_caller == TracedCaller::kAlternate", COMPAT_CPP)

    def test_passive_guest_reads_hold_a_trace_capture_lease(self) -> None:
        for function in (
            "ReRevvedTraceReservationEnter",
            "ReRevvedTraceReservationReturn",
            "ReRevvedTraceVdSwapOwnerEnter",
            "ReRevvedTraceVdSwapOwnerReturn",
            "ReRevvedTraceVdSwapReturn",
            "ReRevvedTraceVdSwapPublished",
        ):
            start = COMPAT_CPP.index(f"void {function}")
            end = COMPAT_CPP.index("\nvoid ", start + 6)
            body = COMPAT_CPP[start:end]
            self.assertIn("BeginRecord", body)
            if "ReadRingObservation" in body:
                self.assertLess(body.index("BeginRecord"), body.index("ReadRingObservation"))

    def test_passive_trace_hooks_are_exact_and_non_replacing(self) -> None:
        exact_hooks = {
            "0x8269C7A8": "ReRevvedTraceReservationEnter",
            "0x8269C80C": "ReRevvedTraceReservationReturn",
            "0x826A4638": "ReRevvedTraceVdSwapOwnerEnter",
            "0x826A4C0C": "ReRevvedTraceVdSwapOwnerReturn",
            "0x8269E520": "ReRevvedObserveRendererResolve",
            "0x8269F360": "ReRevvedTraceResolveReturn",
            "0x826A4884": "ReRevvedObserveRendererSwapSource",
            "0x826A4888": "ReRevvedTraceVdSwapReturn",
            "0x826A4890": "ReRevvedTraceVdSwapPublished",
            "0x826A4150": "ReRevvedTracePreSwapEnter",
            "0x826A4324": "ReRevvedTracePreSwapReturn",
            "0x8269CD20": "ReRevvedTraceEmitterCd20Enter",
            "0x8269CE78": "ReRevvedTraceEmitterCd20Return",
            "0x8269BF40": "ReRevvedTraceEmitterBf40Enter",
            "0x8269C12C": "ReRevvedTraceEmitterBf40Return",
            "0x826A3FB8": "ReRevvedTraceCallbackEnter",
            "0x826A4148": "ReRevvedTraceCallbackReturn",
            "0x826ABEA8": "ReRevvedTraceOrdinaryCallerEnter",
            "0x826AC018": "ReRevvedTraceOrdinaryCallerReturn",
            "0x82517E38": "ReRevvedTraceAlternateCallerEnter",
            "0x82517EB4": "ReRevvedTraceAlternateCallerReturn",
        }
        for address, name in exact_hooks.items():
            hook = f'address = {address}\nname = "{name}"'
            self.assertIn(hook, HOOKS)
        self.assertNotIn("return_on_true", HOOKS)

    def test_passive_trace_has_no_guest_or_gpu_mutation_surface(self) -> None:
        for forbidden in (
            "REX_STORE",
            "WriteGuest",
            "UpdateWritePointer",
            "EnableReadPointerWriteBack",
            "ExecuteCommandLists",
            "Signal(",
            "Present(",
            "TranslatePhysical",
        ):
            self.assertNotIn(forbidden, PASSIVE_TRACE_CPP)
            self.assertNotIn(forbidden, PASSIVE_TRACE_H)
        self.assertIn("kGateClosed", PASSIVE_TRACE_H)
        self.assertIn("kPassiveTraceCapacity", PASSIVE_TRACE_H)
        self.assertIn("# overflow=", PASSIVE_TRACE_CPP)
        self.assertNotIn("std::mutex", PASSIVE_TRACE_CPP)
        self.assertNotIn("std::condition_variable", PASSIVE_TRACE_CPP)
        self.assertNotIn("StopAndFlush", COMPAT_CPP)
        record = PASSIVE_TRACE_CPP[
            PASSIVE_TRACE_CPP.index("bool PassiveTraceBuffer::Record") :
            PASSIVE_TRACE_CPP.index("bool PassiveTraceBuffer::BeginObservationEpoch")
        ]
        for forbidden in ("std::vector", "std::ofstream", "filesystem", "mutex"):
            self.assertNotIn(forbidden, record)

    def test_passive_snapshot_is_fixed_and_post_emission_only(self) -> None:
        self.assertIn("kPassiveTraceReservationDwords = 64", PASSIVE_TRACE_H)
        published = COMPAT_CPP[
            COMPAT_CPP.index("void ReRevvedTraceVdSwapPublished") :
            COMPAT_CPP.index("void ReRevvedTracePreSwapEnter")
        ]
        self.assertIn("event.reservation_words[index] = ReadGuestU32", published)
        for function in (
            "ReRevvedTraceReservationEnter",
            "ReRevvedTraceReservationReturn",
            "ReRevvedTraceVdSwapReturn",
        ):
            start = COMPAT_CPP.index(f"void {function}")
            end = COMPAT_CPP.index("\nvoid ", start + 6)
            self.assertNotIn("reservation_words[index]", COMPAT_CPP[start:end])
        for forbidden in (
            "filename",
            "framebuffer",
            "texture_payload",
            "save_bytes",
            "gamertag",
            "xuid",
        ):
            self.assertNotIn(forbidden, PASSIVE_TRACE_CPP.lower())
            self.assertNotIn(forbidden, PASSIVE_TRACE_H.lower())

    def test_fence_trace_is_default_off_xenos_only_and_separately_admitted(self) -> None:
        self.assertIn(
            'REXCVAR_DEFINE_STRING(native_renderer_fence_trace_output, ""',
            APP_CPP,
        )
        self.assertIn("GetXenosFenceTrace().Start", APP_CPP)
        self.assertIn("trace.FinishAndFlush()", APP_CPP)
        self.assertIn("NATIVE-FENCE-TRACE-INVALID", APP_CPP)
        self.assertIn("statistics.valid_for_promotion()", APP_CPP)
        self.assertIn("statistics.lock_waits", APP_CPP)
        self.assertIn("statistics.maximum_lock_wait_nanoseconds", APP_CPP)
        self.assertIn("statistics.dropped_callbacks", APP_CPP)
        self.assertIn("statistics.reentry_failures", APP_CPP)
        self.assertIn("statistics.watched", APP_CPP)
        self.assertNotIn("statistics.contention_drops", APP_CPP)
        self.assertIn(
            "Passive Resolve/VdSwap tracing and consumer/fence tracing cannot run together",
            APP_CPP,
        )
        self.assertIn("renderer_backend_ != rerevved::gpu::RendererBackend::Xenos", APP_CPP)
        self.assertIn("Native renderer diagnostic tracing cannot share", APP_CPP)
        self.assertIn(
            "ContainsExistingReparsePoint(scratch_root, candidate)",
            APP_CPP,
        )
        self.assertIn("std::filesystem::exists(candidate, error)", APP_CPP)

    def test_fence_trace_registers_only_exact_post_emission_reservations(self) -> None:
        returned = COMPAT_CPP[
            COMPAT_CPP.index("void ReRevvedTraceVdSwapReturn") :
            COMPAT_CPP.index("void ReRevvedTraceVdSwapPublished")
        ]
        self.assertIn("if (FenceTraceEnabled()", returned)
        self.assertIn("GetPhysicalAddress(r30.u32)", returned)
        self.assertIn("WatchSwapReservation(r30.u32, physical_address)", returned)
        self.assertNotIn("WriteGuest", returned)
        self.assertNotIn("store_and_swap", returned)
        self.assertNotIn("correlation_token", returned)

    def test_fence_trace_reset_occurs_only_after_xenos_pause_attempt(self) -> None:
        reset = COMPAT_CPP[
            COMPAT_CPP.index("void ReRevvedCompatRingInitializeBegin") :
            COMPAT_CPP.index("void ReRevvedCompatRingInitializeEnd")
        ]
        self.assertLess(
            reset.index("PauseAndResetGpuWritePointer"),
            reset.index("ResetObservationEpoch"),
        )
        self.assertNotIn("StopAndFlush", reset)


if __name__ == "__main__":
    unittest.main()

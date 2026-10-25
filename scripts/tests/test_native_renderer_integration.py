"""Static contracts for the opt-in title-owned native renderer."""

from __future__ import annotations

import json
import os
import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP_H = (ROOT / "src" / "app.h").read_text(encoding="ascii")
APP_CPP = (ROOT / "src" / "app.cpp").read_text(encoding="ascii")
GAME_CONTENT_H = (ROOT / "src" / "game_content.h").read_text(encoding="ascii")
GAME_CONTENT_CPP = (ROOT / "src" / "game_content.cpp").read_text(encoding="ascii")
NATIVE_CPP = (ROOT / "src" / "gpu" / "d3d12" / "native_renderer_d3d12.cpp").read_text(
    encoding="ascii"
)
GUEST_SERVICE_CPP = (ROOT / "src" / "gpu" / "guest_gpu_service.cpp").read_text(
    encoding="ascii"
)
GUEST_SERVICE_H = (ROOT / "src" / "gpu" / "guest_gpu_service.h").read_text(
    encoding="ascii"
)
LEDGER_CPP = (ROOT / "src" / "gpu" / "completion" / "completion_ledger.cpp").read_text(
    encoding="ascii"
)
PROGRESS_HOOKS_CPP = (
    ROOT / "src" / "gpu" / "completion" / "guest_progress_hooks.cpp"
).read_text(encoding="ascii")
COMPAT_CPP = (ROOT / "src" / "rerevved_hooks.cpp").read_text(encoding="ascii")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="ascii")
LOCK = json.loads((ROOT / "rexglue-sdk.lock.json").read_text(encoding="ascii"))
GUEST_SERVICE_FILES = {
    ROOT / "src" / "gpu" / "guest_gpu_service.cpp",
    ROOT / "src" / "gpu" / "guest_gpu_service.h",
}


class NativeRendererIntegrationTests(unittest.TestCase):
    def test_title_adapter_profile_copy_and_content_identity(self) -> None:
        self.assertIn("GetProfileCopySpecification() const override", APP_H)
        self.assertIn('.config_relative_path = "rerevved.toml"', APP_CPP)
        self.assertIn(".title_id             = rerevved::kTitleId", APP_CPP)
        self.assertIn("extern const std::uint32_t kTitleId;", GAME_CONTENT_H)
        self.assertRegex(
            GAME_CONTENT_CPP,
            r"const\s+std::uint32_t\s+kTitleId\s*=\s*0x545407E5u;",
        )

    def test_title_adapter_defaults_preserve_source_precedence(self) -> None:
        self.assertIn(
            'paths.config_path = paths.user_data_root / "rerevved.toml"',
            APP_CPP,
        )
        self.assertIn(
            'rex::cvar::SetFlagAsApplicationDefault("gpu_plugin", "xenos")',
            APP_CPP,
        )
        self.assertIn(
            'rex::cvar::SetFlagAsApplicationDefault("render_target_path_d3d12", "rov")',
            APP_CPP,
        )
        # Only player builds force the plugin; see the player-build test.
        start = APP_CPP.index("configuredRenderer = REXCVAR_GET(renderer)")
        player = APP_CPP[APP_CPP.index("#else", start) : APP_CPP.index("#endif", start)]
        forced = 'rex::cvar::SetFlagByName("gpu_plugin", "xenos")'
        self.assertEqual(APP_CPP.count(forced), player.count(forced))
        self.assertNotIn(
            'rex::cvar::SetFlagByName("render_target_path_d3d12", "rov")',
            APP_CPP,
        )

        environment = APP_CPP.index("bool App::SetupEnvironment()")
        base_environment = APP_CPP.index("rex::ReXApp::SetupEnvironment()", environment)
        selection = APP_CPP.index(
            "configuredRenderer = REXCVAR_GET(renderer)", environment
        )
        gpu_default = APP_CPP.index(
            'SetFlagAsApplicationDefault("gpu_plugin", "xenos")', environment
        )
        presentation = APP_CPP.index("bool App::SetupPresentation()")
        base_presentation = APP_CPP.index(
            "rex::ReXApp::SetupPresentation()", presentation
        )
        rov_default = APP_CPP.index(
            'SetFlagAsApplicationDefault("render_target_path_d3d12", "rov")',
            presentation,
        )
        self.assertLess(base_environment, selection)
        self.assertLess(selection, gpu_default)
        self.assertLess(gpu_default, presentation)
        self.assertLess(base_presentation, rov_default)
        # Preserve the SDK's automatic RTV path on AMD to avoid slow ROV.
        amd_guard = APP_CPP.index("preferredAdapterVendor() != 0x1002", presentation)
        self.assertLess(amd_guard, rov_default)

    def test_renderer_is_restart_scoped_opt_in(self) -> None:
        self.assertIn('REXCVAR_DEFINE_STRING(renderer, "xenos"', APP_CPP)
        self.assertIn('.allowed({ "xenos", "native" })', APP_CPP)
        renderer_declaration = APP_CPP[
            APP_CPP.index('REXCVAR_DEFINE_STRING(renderer, "xenos"') :
        ].split(";", 1)[0]
        self.assertIn("Lifecycle::kRequiresRestart", renderer_declaration)
        self.assertNotIn("Lifecycle::kInitOnly", renderer_declaration)
        self.assertIn("LatchRendererBackend(configuredRenderer)", APP_CPP)
        self.assertIn("config.graphics.reset();", APP_CPP)
        self.assertIn("config.gpu_plugin.clear();", APP_CPP)

    def test_player_builds_ignore_saved_native_settings(self) -> None:
        # Native test builds saved renderer and native_* paths to rerevved.toml.
        start = APP_CPP.index("configuredRenderer = REXCVAR_GET(renderer)")
        player = APP_CPP[APP_CPP.index("#else", start) : APP_CPP.index("#endif", start)]
        self.assertIn('LatchRendererBackend("xenos")', player)
        self.assertIn("NativeDrawFamilyRegistry()", player)
        # They also saved gpu_plugin = "", which outranks the Xenos default.
        self.assertIn('rex::cvar::SetFlagByName("gpu_plugin", "xenos")', player)
        self.assertNotIn("ShowSimpleMessageBox", player)
        self.assertNotIn("return false", player)

    def test_rerevved_cvars_share_one_settings_page(self) -> None:
        declarations = (
            'REXCVAR_DEFINE_STRING(combat_speed, "normal", "ReRevved",',
            'REXCVAR_DEFINE_STRING(renderer, "xenos", "ReRevved",',
        )
        for declaration in declarations:
            self.assertIn(declaration, APP_CPP)

        self.assertLess(APP_CPP.index(declarations[0]), APP_CPP.index(declarations[1]))
        self.assertIn("REXCVAR_DECLARE(std::string, combat_speed);", COMPAT_CPP)
        for category in ("ReRevved/Combat", "ReRevved/Diagnostics", "ReRevved/Video"):
            self.assertNotIn(category, APP_CPP)
            self.assertNotIn(category, COMPAT_CPP)

    def test_guest_video_services_are_native_only(self) -> None:
        consumers = []
        source_suffixes = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".inc"}
        for source in (ROOT / "src").rglob("*"):
            if source in GUEST_SERVICE_FILES or source.suffix not in source_suffixes:
                continue
            text = source.read_text(encoding="ascii")
            if "GuestVideoServices" in text or "NativeGuestGpuService" in text:
                consumers.append(source.relative_to(ROOT).as_posix())
        self.assertEqual(sorted(consumers), ["src/app.cpp", "src/app.h"])
        self.assertIn(
            "class GuestVideoServices final : public rex::system::IDetachedVideoDelegate",
            GUEST_SERVICE_H,
        )
        self.assertNotIn("IGraphicsSystem", GUEST_SERVICE_H + GUEST_SERVICE_CPP)

        construct = APP_CPP[
            APP_CPP.index("bool App::ConstructRuntime") : APP_CPP.index(
                "std::unique_ptr<rex::ui::ImmediateDrawer> App::OnCreateImmediateDrawer"
            )
        ]
        native = construct[
            : construct.index("    return rex::ReXApp::ConstructRuntime(paths);")
        ]
        self.assertIn("if (nativeFixture)\n            return false;", native)
        self.assertLess(
            native.index("rex::ReXApp::ConstructRuntime(paths)"),
            native.index("startGuestVideoServices()"),
        )
        start = APP_CPP[
            APP_CPP.index("bool App::startGuestVideoServices()") : APP_CPP.index(
                "void App::stopGuestVideoProducers()"
            )
        ]
        self.assertLess(
            start.index("guestVideo->Start(*runtime()"),
            start.index("SetDetachedVideoDelegate(guestVideo)"),
        )
        finalize = APP_CPP[
            APP_CPP.index("App::OnFinalizePaths") : APP_CPP.index(
                "void App::OnPostSetup()"
            )
        ]
        self.assertIn(
            "rendererBackend == gpu::RendererBackend::Native && nativeFixture",
            finalize,
        )

        pre_setup = APP_CPP[
            APP_CPP.index("void App::OnPreSetup") : APP_CPP.index(
                "void App::OnConfigurePaths"
            )
        ]
        self.assertIn("RendererBackend::Native", pre_setup)
        self.assertIn("config.graphics.reset();", pre_setup)
        self.assertIn("config.gpu_plugin.clear();", pre_setup)

    def test_config_is_loaded_before_selection_and_detach(self) -> None:
        base_setup = APP_CPP.index("rex::ReXApp::SetupEnvironment()")
        selection = APP_CPP.index("configuredRenderer = REXCVAR_GET(renderer)")
        xenos_default = APP_CPP.index(
            'rex::cvar::SetFlagAsApplicationDefault("gpu_plugin", "xenos")'
        )
        self.assertLess(base_setup, selection)
        self.assertLess(selection, xenos_default)

        sdk = os.environ.get("REREVVED_SDK_SOURCE")
        if not sdk:
            sibling = ROOT.parent / "rerevved-sdk"
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
                f"{LOCK['commit']}:src/ui/rex_app.cpp",
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

    def test_guest_video_services_complete_only_through_ledger(self) -> None:
        completion_thread = GUEST_SERVICE_CPP.index(
            "kCompletionThread, &State::runCompletionWorker"
        )
        vblank_thread = GUEST_SERVICE_CPP.index(
            "kVblankThread, &State::runVblankWorker"
        )
        mmio_registration = GUEST_SERVICE_CPP.index("AddVirtualMappedRange")
        self.assertLess(completion_thread, vblank_thread)
        self.assertLess(vblank_thread, mmio_registration)
        self.assertIn("kGpuMmioBaseAddress = 0x7FC80000", GUEST_SERVICE_CPP)
        self.assertIn("kGpuMmioAddressMask = 0xFFFF0000", GUEST_SERVICE_CPP)
        self.assertIn("kGpuMmioSize        = 0x0000FFFF", GUEST_SERVICE_CPP)
        self.assertIn("reg == kCpRbWptr", GUEST_SERVICE_CPP)
        self.assertIn("++writePointerPublications;", GUEST_SERVICE_CPP)
        self.assertIn("kicked->Kick(value);", GUEST_SERVICE_CPP)
        for forbidden in (
            "CommandProcessor",
            "RegisterFile",
            "PublishWritePointer",
            "admit_packet",
            "ExecutePacket",
            "GraphicsSystem::SetupGuestGpu",
            "UpdateWritePointer",
            "memory::store<",
        ):
            self.assertNotIn(forbidden, GUEST_SERVICE_CPP)
        # The only guest store is the ledger's sink; the debt monitor only reads.
        self.assertEqual(GUEST_SERVICE_CPP.count("store_and_swap"), 1)
        write = GUEST_SERVICE_CPP[
            GUEST_SERVICE_CPP.index(
                "bool ProgressSink::WritePhysical("
            ) : GUEST_SERVICE_CPP.index(
                "std::optional<uint32_t> ProgressSink::ReadPhysical("
            )
        ]
        self.assertIn("store_and_swap", write)
        self.assertEqual(GUEST_SERVICE_CPP.count("TranslatePhysical"), 3)
        image = GUEST_SERVICE_CPP[
            GUEST_SERVICE_CPP.index(
                "bool ProgressSink::WriteGuestImage("
            ) : GUEST_SERVICE_CPP.index("bool ProgressSink::WritePhysical(")
        ]
        self.assertIn("QueryRangeAccess(address, last)", image)
        self.assertEqual(image.count("std::memcpy("), 1)
        self.assertIn("std::atomic_thread_fence(std::memory_order_release)", image)
        self.assertIn(
            "TranslatePhysical<const uint8_t*>(writebackPointer)", GUEST_SERVICE_CPP
        )
        self.assertIn("QueryRangeAccess", GUEST_SERVICE_CPP)
        # Source 1 is raised only for a ledger request.
        self.assertEqual(GUEST_SERVICE_CPP.count("executeInterrupt(1,"), 1)
        service_request = GUEST_SERVICE_CPP[
            GUEST_SERVICE_CPP.index(
                "void GuestVideoServices::State::serviceCallbackInterrupt()"
            ) : GUEST_SERVICE_CPP.index(
                "void GuestVideoServices::State::checkCompletionDebt()"
            )
        ]
        self.assertIn("executeInterrupt(1,", service_request)
        # The ledger reaches guest memory only through its sink and decodes no PM4.
        for forbidden in (
            "TranslatePhysical",
            "TranslateVirtual",
            "store_and_swap",
            "ExecutePacket",
        ):
            self.assertNotIn(forbidden, LEDGER_CPP)
        for forbidden in ("store_and_swap", "TranslatePhysical", "memory::store"):
            self.assertNotIn(forbidden, PROGRESS_HOOKS_CPP)
        self.assertIn(
            "debtThreshold = std::chrono::seconds(2)",
            GUEST_SERVICE_H,
        )
        self.assertIn("kVblankJoinTimeout{ 1000 }", GUEST_SERVICE_H)
        self.assertIn("kMaxSwapEpochs = 64", GUEST_SERVICE_H)

    def test_guest_gpu_service_vblank_abi_matches_sdk(self) -> None:
        self.assertIn("executeInterrupt(0, kDefaultCpu,", GUEST_SERVICE_CPP)
        self.assertIn(
            "thread->SetActiveCpu(static_cast<uint8_t>(cpu));", GUEST_SERVICE_CPP
        )
        self.assertIn("uint64_t args[] = { source, userData };", GUEST_SERVICE_CPP)
        self.assertIn(
            "ExecuteInterrupt(thread->thread_state(), callback, args",
            GUEST_SERVICE_CPP,
        )

    def test_guest_gpu_service_shutdown_bounds_concurrency(self) -> None:
        self.assertRegex(GUEST_SERVICE_H, r"kMaxVblankCatchUp\s*=\s*4;")
        self.assertIn("step.due < kMaxVblankCatchUp", GUEST_SERVICE_CPP)
        self.assertIn("shutdownStarted", GUEST_SERVICE_CPP)
        self.assertIn("[shared = state, slot, body]", GUEST_SERVICE_CPP)
        stop = GUEST_SERVICE_CPP[
            GUEST_SERVICE_CPP.index(
                "bool GuestVideoServices::StopProducers("
            ) : GUEST_SERVICE_CPP.index("bool GuestVideoServices::Shutdown(")
        ]
        order = [
            stop.index("ledger->Close();"),
            stop.index("completionExit.wait_until(lock, deadline"),
            stop.index("vblankWorkerExit.wait_until(lock, deadline"),
        ]
        self.assertEqual(order, sorted(order))

    def test_guest_video_teardown_order(self) -> None:
        close = APP_CPP[
            APP_CPP.index("bool App::OnWindowCloseRequested()") : APP_CPP.index(
                "void App::OnWindowFocusChanged"
            )
        ]
        accept = close[
            close.index("Decision::Accept)") : close.index('Event("close_veto")')
        ]
        self.assertLess(
            accept.index("stopGuestVideoProducers();"), accept.index("return true;")
        )
        stop = APP_CPP[
            APP_CPP.index("void App::stopGuestVideoProducers()") : APP_CPP.index(
                "void App::nativePresentationTick"
            )
        ]
        self.assertIn("gpu::VblankJoinTimeout(", stop)
        self.assertIn("nativeDrain->deadline.Expiry()", stop)

        shutdown = APP_CPP[
            APP_CPP.index("void App::OnShutdown()") : APP_CPP.index(
                "bool App::OnWindowCloseRequested()"
            )
        ]
        order = [
            shutdown.index("guestVideo->StopProducers("),
            shutdown.index("removeNativeDraws();"),
            shutdown.index("SetDetachedVideoDelegate(nullptr)"),
            shutdown.index("guestVideo->Shutdown("),
            shutdown.index("nativeDrain->result.wait_until"),
            shutdown.index("hardExitAfterMissedVblankJoin();"),
            shutdown.index("ShutdownOverlays();"),
            shutdown.index("nativePresentation.reset();"),
        ]
        self.assertEqual(order, sorted(order))
        hard = APP_CPP[
            APP_CPP.index("void App::hardExitAfterMissedVblankJoin()") : APP_CPP.index(
                "void App::stopGuestVideoProducers()"
            )
        ]
        self.assertLess(hard.index("FlushTrace()"), hard.index("rex::FlushLogging();"))
        self.assertLess(
            hard.index("rex::FlushLogging();"), hard.index("std::_Exit(0);")
        )
        hard_exit = APP_CPP[APP_CPP.index("void App::OnBeforeHardExit()") :]
        self.assertNotIn("guestVideo", hard_exit)
        # Every hard exit joins the presence thread before rexruntime.dll detaches:
        # the SDK's exit hook and each title _Exit call.
        self.assertIn("rerevved::StopPresence();", hard_exit)
        exits = [m.start() for m in re.finditer(r"std::_Exit\(", APP_CPP)]
        self.assertTrue(exits)
        for exit_index in exits:
            body_start = APP_CPP.rindex("\n}\n", 0, exit_index)
            body = APP_CPP[body_start:exit_index]
            self.assertIn("rerevved::StopPresence();", body)
            self.assertLess(
                body.index("rerevved::StopPresence();"),
                body.index("rex::FlushLogging();"),
            )

    def test_guest_gpu_service_mmio_after_shutdown_is_detached(self) -> None:
        self.assertIn("static MmioBridge bridge;", GUEST_SERVICE_CPP)
        self.assertIn("&getMmioBridge()", GUEST_SERVICE_CPP)
        self.assertNotIn(
            "kGpuMmioSize,\n            this,",
            GUEST_SERVICE_CPP,
        )
        self.assertIn("bridge.service = nullptr;", GUEST_SERVICE_CPP)
        self.assertIn("bridge.callbacksInFlight == 0", GUEST_SERVICE_CPP)
        self.assertIn("MmioCallbackLease lease(callbackContext);", GUEST_SERVICE_CPP)
        self.assertIn("unbindMmioBridge(state.get());", GUEST_SERVICE_CPP)

    def test_native_surface_is_created_after_sdk_window_open(self) -> None:
        presentation = APP_CPP.index("bool App::SetupPresentation()")
        base_presentation = APP_CPP.index(
            "rex::ReXApp::SetupPresentation()", presentation
        )
        native_setup = APP_CPP.index("setupNativePresentation()", base_presentation)
        self.assertLess(base_presentation, native_setup)
        setup_start = APP_CPP.index("bool App::setupNativePresentation()")
        setup_end = APP_CPP.index("\n}\n", setup_start)
        setup_body = APP_CPP[setup_start:setup_end]
        self.assertIn("gpu::NativeAppSession::Create(", setup_body)
        self.assertIn("window()->GetNativeWindowHandle()", setup_body)
        self.assertNotIn("nativeRenderer.Initialize(*window())", APP_CPP)
        self.assertIn('"ReRevved - Native D3D12"', APP_CPP)

    def test_native_shutdown_waits_for_existing_close(self) -> None:
        shutdown = APP_CPP[
            APP_CPP.index("void App::OnShutdown()") : APP_CPP.index(
                "bool App::OnWindowCloseRequested()"
            )
        ]
        self.assertIn("if (!nativeDrain)", shutdown)
        self.assertNotIn("if (!nativeClosing)", shutdown)
        wait = shutdown.index(
            "nativeDrain->result.wait_until(nativeDrain->deadline.Expiry());"
        )
        self.assertLess(wait, shutdown.index("ShutdownOverlays();"))
        self.assertLess(wait, shutdown.index("nativePresentation.reset();"))

    def test_d3d12_frame_and_failure_contract(self) -> None:
        self.assertRegex(
            NATIVE_CPP,
            r"constexpr\s+std::uint32_t\s+kFrameCount\s*=\s*2;",
        )
        self.assertLess(
            NATIVE_CPP.index("enableDred();"),
            NATIVE_CPP.index("D3D12CreateDevice"),
        )
        execute = NATIVE_CPP.index("ExecuteCommandLists")
        signal = NATIVE_CPP.index("queue->Signal", execute)
        present = NATIVE_CPP.index("swapChain->Present", signal)
        self.assertLess(execute, signal)
        self.assertLess(signal, present)
        resize_failure = NATIVE_CPP.index('"swap chain resize"')
        self.assertNotIn(
            "shutdownOnRendererThread",
            NATIVE_CPP[resize_failure : resize_failure + 250],
        )
        self.assertIn("GetDeviceRemovedReason", NATIVE_CPP)

    def test_d3d12_lifecycle_is_owned_by_one_renderer_thread(self) -> None:
        self.assertRegex(NATIVE_CPP, r"std::thread\s+rendererThread;")
        self.assertIn("NativeRendererD3D12::rendererThreadMain", NATIVE_CPP)
        self.assertIn("initializeOnRendererThread", NATIVE_CPP)
        self.assertIn("presentOnRendererThread", NATIVE_CPP)
        self.assertIn("resizeOnRendererThread", NATIVE_CPP)
        self.assertIn("shutdownOnRendererThread", NATIVE_CPP)

        resize_start = NATIVE_CPP.index("bool NativeRendererD3D12::Resize(")
        resize = NATIVE_CPP[resize_start : NATIVE_CPP.index("\n}\n", resize_start)]
        self.assertIn("requestedWidth", resize)
        self.assertIn("requestedHeight", resize)
        self.assertIn("resizePending", resize)
        self.assertIn("stateCv.notify_one();", resize)
        self.assertNotIn("stateCv.wait", resize)
        self.assertNotIn("ResizeBuffers", resize)
        self.assertNotIn("waitForGpu", resize)

        stop_start = NATIVE_CPP.index(
            "void NativeRendererD3D12::RequestDrawStreamStop()"
        )
        stop = NATIVE_CPP[stop_start : NATIVE_CPP.index("\n}\n", stop_start)]
        self.assertIn("impl->stopRequested = true;", stop)
        self.assertIn("impl->notifyStream();", stop)
        self.assertNotIn("rendererThread.join", stop)
        self.assertNotIn("waitForGpu", stop)

        notify_start = NATIVE_CPP.index("void notifyStream()")
        notify = NATIVE_CPP[notify_start : NATIVE_CPP.index("\n    }\n", notify_start)]
        self.assertIn("stateCv.notify_all();", notify)

        shutdown_start = NATIVE_CPP.index("void NativeRendererD3D12::Shutdown()")
        shutdown = NATIVE_CPP[
            shutdown_start : NATIVE_CPP.index("\n}\n", shutdown_start)
        ]
        self.assertIn("RequestDrawStreamStop();", shutdown)
        self.assertIn("rendererThread.join();", shutdown)
        self.assertLess(
            shutdown.index("RequestDrawStreamStop();"),
            shutdown.index("rendererThread.join();"),
        )
        self.assertNotIn("waitForGpu", shutdown)

        worker = NATIVE_CPP[
            NATIVE_CPP.index(
                "void NativeRendererD3D12::rendererThreadMain"
            ) : NATIVE_CPP.index("bool NativeRendererD3D12::Initialize(")
        ]
        self.assertLess(
            worker.index("stateCv.wait"), worker.index("shutdownOnRendererThread")
        )
        self.assertLess(
            worker.index("stopRequested"), worker.index("shutdownOnRendererThread")
        )
        self.assertIn("requestDeferredQuit", NATIVE_CPP)
        self.assertIn("appContext->RequestDeferredQuit();", NATIVE_CPP)

    def test_fence_drain_is_bounded_and_fail_closed(self) -> None:
        self.assertIn("kFenceWaitTimeoutMs", NATIVE_CPP)
        self.assertIn("WAIT_TIMEOUT", NATIVE_CPP)
        self.assertIn("fence wait timed out after", NATIVE_CPP)
        self.assertNotIn("WaitForSingleObject(fence_event, INFINITE)", NATIVE_CPP)
        self.assertNotIn("shutdownOnRendererThread(false);", NATIVE_CPP)
        self.assertIn("abandoning GPU objects until process exit", NATIVE_CPP)
        self.assertIn("gpuObjectsAbandoned.store(true", NATIVE_CPP)
        self.assertIn("cannot reinitialize after abandoning", NATIVE_CPP)
        self.assertIn("appContext->RequestDeferredQuit();", NATIVE_CPP)

    def test_ui_resize_callback_only_latches_renderer_request(self) -> None:
        resize_callback = APP_CPP[
            APP_CPP.index("void App::OnWindowPixelSizeChanged") : APP_CPP.index(
                "bool App::setupNativePresentation()"
            )
        ]
        self.assertIn("nativeRenderer.Resize(pixelWidth, pixelHeight)", resize_callback)
        self.assertNotIn("ResizeBuffers", resize_callback)
        self.assertNotIn("WaitForGpu", resize_callback)

    def test_windows_links_are_explicit(self) -> None:
        self.assertIn(
            "target_link_libraries(rerevved PRIVATE d3d12 dxgi dxguid d3dcompiler)",
            CMAKE,
        )

    def test_runtime_code_reads_the_latch_not_the_renderer_cvar(self) -> None:
        # The renderer cvar changes in memory while the game runs; only the
        # value latched at startup selects the backend.
        self.assertNotIn("REXCVAR_GET(renderer)", COMPAT_CPP)
        self.assertNotIn("REXCVAR_DECLARE(std::string, renderer)", COMPAT_CPP)
        self.assertEqual(
            COMPAT_CPP.count("rerevved::gpu::SelectedRendererBackend()"), 4
        )
        reads = re.compile(
            r"REXCVAR_(?:GET|QUERY)\s*\([^)]*renderer\s*\)"
            r"|FLAGS_renderer"
            r"|(?:Query|GetFlag\w*)\s*(?:<[^>]*>)?\s*\(\s*\"renderer\""
        )
        for path in (ROOT / "src").rglob("*"):
            if (
                path.suffix not in (".cpp", ".h", ".inc")
                or path == ROOT / "src" / "app.cpp"
            ):
                continue
            text = path.read_text(encoding="utf-8")
            self.assertIsNone(reads.search(text), path.name)
        startup = APP_CPP[APP_CPP.index("bool App::SetupEnvironment()") :]
        self.assertEqual(
            startup.count("REXCVAR_GET(renderer)"), 1, "only startup reads the cvar"
        )
        after_startup = APP_CPP[APP_CPP.index("bool App::SetupPresentation()") :]
        self.assertNotIn("REXCVAR_GET(renderer)", after_startup)

    def test_overlay_lists_renderer_as_a_developer_row(self) -> None:
        overlay = (ROOT / "src" / "overlay_settings.cpp").read_text(encoding="ascii")
        display = overlay[overlay.index('{ "Display",') : overlay.index('{ "Audio",')]
        self.assertNotIn('"renderer"', display)
        developer = overlay[overlay.index('{ "Developer",') :].split("};", 1)[0]
        self.assertIn('{ "Renderer", { "renderer", "@Graphics" } }', developer)
        self.assertIn('{ "renderer", Audience::kDeveloper }', overlay)
        self.assertNotIn('{ "renderer", Audience::kPlayer }', overlay)
        self.assertIn('{ "@Graphics", Audience::kDeveloper }', overlay)
        self.assertIn('{ "renderer", "Renderer" }', overlay)
        self.assertIn(
            '{ "renderer", { { "xenos", "Xenos" }, '
            '{ "native", "Native (Experimental)" } } }',
            overlay,
        )
        self.assertIn(
            '{ "renderer", "Native is experimental." }',
            overlay,
        )


if __name__ == "__main__":
    unittest.main()

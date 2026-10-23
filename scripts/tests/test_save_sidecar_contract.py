from __future__ import annotations

import re
import tomllib
import unittest
from collections import deque
from pathlib import Path

from generated_scan import WindowCollector, chunk_lines, scan_once

ROOT = Path(__file__).resolve().parents[2]
EXPECTED_HOOKS = [
    {"address": 0x82D844C8, "name": "ReadGameplaySidecarBeforeDeserialize"},
    {
        "address": 0x82D829E8,
        "name": "WriteGameplaySidecarAfterSave",
        "registers": ["r25", "r29"],
    },
]
RAW_WRAPPERS = ("sub_82D82510", "sub_82D84238", "sub_82D387E0")


ANCHORS = {
    "save_result": "loc_82D829E8:",
    "deserialize": "ctx.lr = 0x82D844CC;",
    "load_reset": "ctx.lr = 0x82D86678;",
    "content_open": "ctx.lr = 0x82E83B3C;",
    "read_hook": "ReadGameplaySidecarBeforeDeserialize();",
    "write_hook": "WriteGameplaySidecarAfterSave(ctx.r25, ctx.r29);",
}


def scan_generated(paths):
    counts = dict.fromkeys([*ANCHORS, *RAW_WRAPPERS], 0)
    collector = WindowCollector()
    for lines in chunk_lines(paths):
        previous = deque(maxlen=32)
        for line in lines:
            collector.advance(line)
            stripped = line.strip()
            matches = [name for name, anchor in ANCHORS.items() if stripped == anchor]
            if line.startswith("DEFINE_REX_FUNC("):
                for name in RAW_WRAPPERS:
                    if re.match(rf"DEFINE_REX_FUNC\((?:__imp__)?{name}\)", line):
                        matches.append(name)
            for name in matches:
                counts[name] += 1
                collector.start(name, [*previous, line], 50)
            previous.append(line)
        collector.end_chunk()
    return counts, collector.windows


def generated_windows():
    paths, (counts, windows) = scan_once("save_sidecar", scan_generated)
    return paths, counts, windows


class SaveSidecarContractTests(unittest.TestCase):
    def test_legendary_uses_existing_record_schema(self):
        source = (ROOT / "src/save_sidecar.cpp").read_text(encoding="utf-8")
        self.assertIn(
            "state.resources == 1 || state.resources == 3 || state.overrides.empty()",
            source,
        )
        header = (ROOT / "src/resource_identity_overrides.h").read_text(
            encoding="utf-8"
        )
        self.assertRegex(header, r"uint8_t\s+ordinal")
        self.assertIn("count U16", header)
        self.assertRegex(header, r"kSchemaVersion\s*= 1")
        mapping = (ROOT / "src/resource_identity_overrides.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("count > 32 * 32", mapping)

    @classmethod
    def setUpClass(cls):
        cls.hooks = (ROOT / "src/save_sidecar_hooks.cpp").read_text(encoding="utf-8")
        cls.paths, cls.counts, cls.windows = generated_windows()

    def test_config_and_default_on(self):
        config = tomllib.loads(
            (ROOT / "config/save_sidecar_hooks.toml").read_text(encoding="utf-8")
        )
        self.assertEqual(config["midasm_hook"], EXPECTED_HOOKS)
        self.assertIn('REXCVAR_DEFINE_BOOL(save_sidecar, true, "ReRevved",', self.hooks)
        for name in RAW_WRAPPERS:
            self.assertEqual(self.hooks.count(f"REX_HOOK_RAW({name})"), 1)
            self.assertEqual(self.hooks.count(f"__imp__{name}(ctx, base);"), 1)
        self.assertIn("enabled(REXCVAR_GET(save_sidecar))", self.hooks)
        self.assertRegex(self.hooks, r"~Transaction\(\)\s*\{\s*active = previous;")

    def test_native_namespace_and_success_gate(self):
        self.assertIn("ctx.lr == 0x82E83B3C", self.hooks)
        self.assertIn("!capture || ctx.r3.u32 != 0", self.hooks)
        self.assertIn("readWord(uint64_t{ stream } + 0x10)", self.hooks)
        self.assertIn("fs->ResolvePath(filename)", self.hooks)
        self.assertIn("entry->host_path()", self.hooks)
        self.assertIn("uint64_t{ active->record } + 0x108", self.hooks)
        self.assertIn("readWord(active->record)", self.hooks)
        writer = self.hooks.split("void WriteGameplaySidecarAfterSave(", 1)[1].split(
            "namespace rerevved::save_sidecar", 1
        )[0]
        self.assertLess(writer.index("result.u32 == 0"), writer.index("sidecar::Save("))
        self.assertIn("readString(filename.u32, 43)", writer)
        payload = (ROOT / "src/save_sidecar.cpp").read_text(encoding="utf-8")
        save = payload.split("bool Save(", 1)[1]
        self.assertLess(save.index("if (!nativeSuccess)"), save.index("sha256_file("))
        self.assertIn("sha256_file(nativeFile)", payload)
        self.assertIn(
            "resource_identity_overrides::Serialize(state.overrides)", payload
        )
        self.assertIn("resource_identity_overrides::Deserialize(", payload)
        self.assertIn('temporary += ".tmp." + std::to_string(process)', payload)
        self.assertIn("counter.fetch_add(1, std::memory_order_relaxed)", payload)
        self.assertIn("error != EEXIST", payload)
        self.assertIn('L"wbx"', payload)
        self.assertIn("MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH", payload)
        self.assertIn("std::rename(temporary.c_str(), path.c_str())", payload)

    def test_restore_after_every_reset_and_freeze(self):
        reset = (ROOT / "src/ai_civilizations_hooks.cpp").read_text(encoding="utf-8")
        reset = reset.split("void ResetAiCivilizationsOnLoad()", 1)[1].split("}", 1)[0]
        restore = "rerevved::save_sidecar::RestoreAfterLoadReset();"
        for call in (
            "rerevved::start_resources::ResetOnLoad();",
            "removedMask.store(0);",
            "FreezeRagingBarbariansForGame();",
            "FreezeOccForGame();",
        ):
            self.assertLess(reset.index(call), reset.index(restore))
        self.assertTrue(reset.rstrip().endswith(restore))
        publish = self.hooks.split("void RestoreAfterLoadReset() noexcept", 1)[1]
        self.assertIn("!active || !active->enabled || !active->load", publish)
        self.assertIn("active->pending.AfterReset()", publish)
        for call in (
            "resource_identity_overrides::Publish(state->overrides)",
            "raging_barbarians::RestoreForGame(state->ragingBarbarians)",
            "occ::RestoreForGame({ state->occEnabled, state->occPopulation })",
            "start_resources::RestoreForGame(state->resources)",
        ):
            self.assertIn(call, publish)
        self.assertNotIn("REXCVAR_GET", publish)
        self.assertNotIn("Clear()", publish)
        self.assertIn("try", publish)
        self.assertIn("catch (...)", publish)
        self.assertIn("if (!active->restoreLogged)", publish)
        self.assertEqual(publish.count("REXLOG_INFO("), 1)
        capture = self.hooks.split("sidecar::State captureState()", 1)[1].split(
            "} // namespace", 1
        )[0]
        self.assertNotIn("REXCVAR_GET", capture)
        for value in (
            "occ::ForGame()",
            "raging_barbarians::Evaluate(0)",
            "start_resources::ForGame()",
        ):
            self.assertIn(value, capture)

    def test_fallback_and_exclusions(self):
        reader = self.hooks.split("void ReadGameplaySidecarBeforeDeserialize()", 1)[
            1
        ].split("void WriteGameplaySidecarAfterSave(", 1)[0]
        self.assertIn("!active || !active->enabled || !active->load", reader)
        self.assertIn("active->pending.Stage(std::nullopt)", reader)
        self.assertNotIn("REXLOG_", reader)
        for reason in (
            "no native file captured",
            "sidecar missing",
            "sidecar unreadable",
            "native digest unreadable",
            "native digest mismatch",
            "schema or identity reject",
        ):
            self.assertIn(reason, reader)
        transaction = self.hooks.split("~Transaction()", 1)[1].split(
            "const uint8_t* checkedBytes", 1
        )[0]
        self.assertIn("enabled && load && !restoreLogged", transaction)
        self.assertIn("staged state never restored", transaction)
        self.assertEqual(transaction.count("REXLOG_INFO("), 1)
        self.assertEqual(transaction.count("REXLOG_WARN("), 1)
        header = (ROOT / "src/save_sidecar.h").read_text(encoding="utf-8")
        self.assertNotIn("std::exchange", header)
        self.assertIn("const std::optional<State>& AfterReset() const noexcept", header)
        for excluded in ("gotw", "militia"):
            self.assertNotIn(excluded, self.hooks.lower())
            self.assertNotIn(excluded, header.lower())

    def test_sidecar_validation_precedes_native_hash(self):
        payload = (ROOT / "src/save_sidecar.cpp").read_text(encoding="utf-8")
        reader = payload.split("std::optional<State> Read(", 1)[1].split(
            "bool Save(", 1
        )[0]
        digest = reader.index("sha256_file(nativeFile)")
        for step in (
            "std::ifstream file(",
            "if (!file)",
            "Deserialize(bytes, identity)",
        ):
            self.assertLess(reader.index(step), digest)

    def test_native_sites_across_all_generated_chunks(self):
        if not self.paths:
            self.skipTest("No generated PPC chunks are available")
        for name in (
            *RAW_WRAPPERS,
            "save_result",
            "deserialize",
            "load_reset",
            "content_open",
        ):
            self.assertEqual(self.counts[name], 1, name)
        save = self.windows["save_result"]
        self.assertIn("ctx.r25.u64 = ctx.r31.u64;", save)
        self.assertIn("sub_82D31F48(ctx, base);", save)
        load = self.windows["deserialize"]
        self.assertIn("ctx.ctr.u64 = ctx.r10.u64;", load)
        self.assertIn("REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);", load)
        reset = self.windows["load_reset"]
        self.assertLess(
            reset.index("ResetAiCivilizationsOnLoad();"),
            reset.index("sub_82CC4040(ctx, base);"),
        )
        opener = self.windows["content_open"]
        self.assertIn("sub_82D387E0(ctx, base);", opener)
        self.assertIn("if (!ctx.cr6.eq) goto loc_82E83B4C;", opener)
        self.assertIn("ctx.r27.u64 = ctx.r4.u64;", self.windows["sub_82D82510"])
        self.assertIn("ctx.r29.s64 = ctx.r27.s64 + 264;", self.windows["sub_82D82510"])
        self.assertIn("ctx.r27.u64 = ctx.r4.u64;", self.windows["sub_82D84238"])
        self.assertIn("ctx.r31.s64 = ctx.r27.s64 + 264;", self.windows["sub_82D84238"])

    def test_emitted_hooks_preserve_native_calls(self):
        if not self.counts["read_hook"] and not self.counts["write_hook"]:
            self.skipTest("Save sidecar hooks have not been generated")
        self.assertEqual(self.counts["read_hook"], 1)
        self.assertEqual(self.counts["write_hook"], 1)
        load = self.windows["deserialize"]
        self.assertLess(
            load.index("ReadGameplaySidecarBeforeDeserialize();"),
            load.index("REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);"),
        )
        save = self.windows["save_result"]
        self.assertLess(
            save.index("loc_82D829E8:"),
            save.index("WriteGameplaySidecarAfterSave(ctx.r25, ctx.r29);"),
        )
        self.assertLess(
            save.index("WriteGameplaySidecarAfterSave(ctx.r25, ctx.r29);"),
            save.index("ctx.r3.u64 = REX_LOAD_U32(ctx.r28.u32 + -3456);"),
        )


if __name__ == "__main__":
    unittest.main()

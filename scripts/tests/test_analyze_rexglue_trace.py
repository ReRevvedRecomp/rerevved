"""Focused, asset-free tests for the ReXGlue trace analyzer."""

from __future__ import annotations

import importlib.util
import io
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ANALYZER_PATH = ROOT / "scripts" / "analyze-rexglue-trace.py"
SPEC = importlib.util.spec_from_file_location("analyze_rexglue_trace", ANALYZER_PATH)
assert SPEC and SPEC.loader
analyzer = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = analyzer
sys.dont_write_bytecode = True
SPEC.loader.exec_module(analyzer)


def packet_word(opcode: int) -> int:
    return (3 << 30) | (opcode << 8)


class TraceAnalyzerTests(unittest.TestCase):
    def test_payload_decompression_and_decoded_size_rejection(self) -> None:
        # Raw Snappy literal: decoded length 3, literal tag length 3, "abc".
        compressed = b"\x03\x08abc"
        self.assertEqual(analyzer.snappy_decompress(compressed, 3), b"abc")
        self.assertEqual(analyzer.decode_payload(0, b"raw", 3), b"raw")
        with self.assertRaisesRegex(ValueError, "size mismatch"):
            analyzer.snappy_decompress(compressed, 4)
        with self.assertRaisesRegex(ValueError, "raw payload size mismatch"):
            analyzer.decode_payload(0, b"raw", 4)
        with self.assertRaisesRegex(ValueError, "unknown memory encoding"):
            analyzer.decode_payload(7, b"", 0)

    def test_register_writes_cover_packet_types(self) -> None:
        registers = [0] * 0x5003

        type_zero = analyzer.Packet(0, 0, 0, [(1 << 16) | 5, 0x11, 0x22])
        analyzer.write_registers(registers, type_zero)
        self.assertEqual(registers[5:7], [0x11, 0x22])

        write_one = analyzer.Packet(
            1, 0, 0, [(1 << 16) | (1 << 15) | 9, 0x33, 0x44]
        )
        analyzer.write_registers(registers, write_one)
        self.assertEqual(registers[9], 0x44)

        type_one = analyzer.Packet(2, 0, 0, [(1 << 30) | (4 << 11) | 3, 0x55, 0x66])
        analyzer.write_registers(registers, type_one)
        self.assertEqual(registers[3], 0x55)
        self.assertEqual(registers[4], 0x66)

        set_constant = analyzer.Packet(
            3,
            0,
            0,
            [packet_word(analyzer.PM4_SET_CONSTANT), (1 << 16) | 2, 0x77, 0x88],
        )
        analyzer.write_registers(registers, set_constant)
        self.assertEqual(registers[0x4802 : 0x4804], [0x77, 0x88])

        set_constant2 = analyzer.Packet(
            4, 0, 0, [packet_word(analyzer.PM4_SET_CONSTANT2), 0x20, 0x99]
        )
        analyzer.write_registers(registers, set_constant2)
        self.assertEqual(registers[0x20], 0x99)

    def test_load_memory_registers_uses_traced_big_endian_data(self) -> None:
        registers = [0] * 0x5003
        packet = analyzer.Packet(
            0,
            0,
            0,
            [packet_word(analyzer.PM4_LOAD_ALU_CONSTANT), 0, (1 << 16) | 8, 2],
            [
                analyzer.MemoryRead(
                    base=0x1000,
                    size=8,
                    nonzero=8,
                    data=struct.pack(">II", 0xAABBCCDD, 0x11223344),
                )
            ],
        )
        constant_type, written = analyzer.load_memory_registers(registers, packet)
        self.assertEqual((constant_type, written), (1, 2))
        self.assertEqual(registers[0x4808 : 0x480A], [0xAABBCCDD, 0x11223344])

        packet.reads[-1].data = None
        self.assertEqual(analyzer.load_memory_registers(registers, packet), (1, 0))

    def test_texture_fetch_extraction_and_formatting(self) -> None:
        registers = [0] * 0x5003
        start = analyzer.FETCH_REGISTER_BASE + 3 * analyzer.FETCH_DWORDS
        registers[start : start + 6] = [
            (1 << 31) | (4 << 22) | 2,
            0x00123000 | 5,
            (19) | (9 << 13),
            0,
            0,
            0,
        ]
        fetches = analyzer.texture_fetches(registers)
        self.assertEqual(len(fetches), 1)
        self.assertEqual(
            analyzer.format_fetch(fetches[0]),
            "f3=00123000:20x10:pitch=128:fmt=5:tiled=1",
        )

    def test_draw_initiator_decoding(self) -> None:
        initiator = (0x1234 << 16) | (2 << 6) | 0x12
        indx = analyzer.Packet(0, 0, 0, [packet_word(analyzer.PM4_DRAW_INDX), 0, initiator])
        indx2 = analyzer.Packet(1, 0, 0, [packet_word(analyzer.PM4_DRAW_INDX_2), initiator])
        self.assertEqual(analyzer.draw_initiator(indx), (0x12, 2, 0x1234))
        self.assertEqual(analyzer.draw_initiator(indx2), (0x12, 2, 0x1234))

    def test_truncated_and_unsupported_trace_packets_fail(self) -> None:
        header = struct.pack("<I40sI", 1, b"test" + b"\0" * 36, 0x545354)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "truncated.trace"
            path.write_bytes(header + struct.pack("<III", analyzer.PACKET_START, 0, 1))
            with redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(ValueError, "truncated packet"):
                    analyzer.analyze(path, 4096, True, None)

            path.write_bytes(header + b"\x01")
            with redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(ValueError, "truncated command header"):
                    analyzer.analyze(path, 4096, True, None)

            path.write_bytes(header + struct.pack("<I", 0xFFFF))
            with redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(ValueError, "unknown command"):
                    analyzer.analyze(path, 4096, True, None)

    def test_summary_output_for_empty_trace_remains_compact(self) -> None:
        header = struct.pack("<I40sI", 1, b"test" + b"\0" * 36, 0x545354)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "empty.trace"
            path.write_bytes(header)
            output = io.StringIO()
            with redirect_stdout(output):
                analyzer.analyze(path, 4096, True, None)
        self.assertIn("trace=", output.getvalue())
        self.assertIn("packets=0 draws=0 memory_reads=0", output.getvalue())


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Print a compact draw and memory-read inventory for a ReXGlue GPU trace."""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass, field
from pathlib import Path


TRACE_HEADER_SIZE = 48

PRIMARY_BUFFER_START = 0
PRIMARY_BUFFER_END = 1
INDIRECT_BUFFER_START = 2
INDIRECT_BUFFER_END = 3
PACKET_START = 4
PACKET_END = 5
MEMORY_READ = 6
MEMORY_WRITE = 7
EDRAM_SNAPSHOT = 8
EVENT = 9
REGISTERS = 10
GAMMA_RAMP = 11

PM4_DRAW_INDX = 0x22
PM4_DRAW_INDX_2 = 0x36
PM4_SET_CONSTANT = 0x2D
PM4_LOAD_ALU_CONSTANT = 0x2F
PM4_SET_CONSTANT2 = 0x55
PM4_SET_SHADER_CONSTANTS = 0x56

FETCH_REGISTER_BASE = 0x4800
FETCH_COUNT = 32
FETCH_DWORDS = 6


def snappy_decompress(data: bytes, expected_size: int) -> bytes:
    """Decode the raw Snappy block format used by TraceWriter."""
    pos = 0
    decoded_size = 0
    shift = 0
    while True:
        if pos >= len(data) or shift >= 35:
            raise ValueError("invalid Snappy length")
        value = data[pos]
        pos += 1
        decoded_size |= (value & 0x7F) << shift
        if not value & 0x80:
            break
        shift += 7
    if decoded_size != expected_size:
        raise ValueError(
            f"Snappy size mismatch: stream={decoded_size}, expected={expected_size}"
        )

    output = bytearray()
    while pos < len(data) and len(output) < decoded_size:
        tag = data[pos]
        pos += 1
        tag_type = tag & 3
        if tag_type == 0:
            length_code = tag >> 2
            if length_code < 60:
                length = length_code + 1
            else:
                byte_count = length_code - 59
                if pos + byte_count > len(data):
                    raise ValueError("truncated Snappy literal length")
                length = int.from_bytes(data[pos : pos + byte_count], "little") + 1
                pos += byte_count
            if pos + length > len(data):
                raise ValueError("truncated Snappy literal")
            output.extend(data[pos : pos + length])
            pos += length
            continue

        if tag_type == 1:
            length = 4 + ((tag >> 2) & 7)
            if pos >= len(data):
                raise ValueError("truncated Snappy copy-1")
            offset = ((tag & 0xE0) << 3) | data[pos]
            pos += 1
        elif tag_type == 2:
            length = 1 + (tag >> 2)
            if pos + 2 > len(data):
                raise ValueError("truncated Snappy copy-2")
            offset = int.from_bytes(data[pos : pos + 2], "little")
            pos += 2
        else:
            length = 1 + (tag >> 2)
            if pos + 4 > len(data):
                raise ValueError("truncated Snappy copy-4")
            offset = int.from_bytes(data[pos : pos + 4], "little")
            pos += 4
        if offset == 0 or offset > len(output):
            raise ValueError("invalid Snappy copy offset")
        for _ in range(length):
            output.append(output[-offset])

    if len(output) != decoded_size or pos != len(data):
        raise ValueError(
            f"Snappy decode mismatch: output={len(output)}, input={pos}/{len(data)}"
        )
    return bytes(output)


def decode_payload(encoding: int, payload: bytes, decoded_size: int) -> bytes:
    if encoding == 0:
        if len(payload) != decoded_size:
            raise ValueError("raw payload size mismatch")
        return payload
    if encoding == 1:
        return snappy_decompress(payload, decoded_size)
    raise ValueError(f"unknown memory encoding {encoding}")


@dataclass
class MemoryRead:
    base: int
    size: int
    nonzero: int
    data: bytes | None = None


@dataclass
class Packet:
    ordinal: int
    frame: int
    base: int
    words: list[int]
    reads: list[MemoryRead] = field(default_factory=list)

    @property
    def packet_type(self) -> int:
        return self.words[0] >> 30 if self.words else -1

    @property
    def opcode(self) -> int:
        return (self.words[0] >> 8) & 0x7F if self.packet_type == 3 else -1


def write_registers(registers: list[int], packet: Packet) -> None:
    words = packet.words
    if not words:
        return
    packet_type = packet.packet_type
    if packet_type == 0:
        count = ((words[0] >> 16) & 0x3FFF) + 1
        base = words[0] & 0x7FFF
        write_one = (words[0] >> 15) & 1
        for offset, value in enumerate(words[1 : 1 + count]):
            index = base if write_one else base + offset
            if index < len(registers):
                registers[index] = value
        return
    if packet_type == 1 and len(words) >= 3:
        first = words[0] & 0x7FF
        second = (words[0] >> 11) & 0x7FF
        if first < len(registers):
            registers[first] = words[1]
        if second < len(registers):
            registers[second] = words[2]
        return
    if packet_type != 3 or len(words) < 2:
        return

    opcode = packet.opcode
    if opcode == PM4_SET_CONSTANT:
        offset_type = words[1]
        index = offset_type & 0x7FF
        constant_type = (offset_type >> 16) & 0xFF
        bases = {0: 0x4000, 1: 0x4800, 2: 0x4900, 3: 0x4908, 4: 0x2000}
        if constant_type not in bases:
            return
        index += bases[constant_type]
        values = words[2:]
    elif opcode in (PM4_SET_CONSTANT2, PM4_SET_SHADER_CONSTANTS):
        index = words[1] & 0xFFFF
        values = words[2:]
    else:
        return
    for offset, value in enumerate(values):
        target = index + offset
        if target < len(registers):
            registers[target] = value


def load_memory_registers(registers: list[int], packet: Packet) -> tuple[int, int]:
    """Apply a LOAD_ALU_CONSTANT packet after its traced memory read."""
    words = packet.words
    if packet.opcode != PM4_LOAD_ALU_CONSTANT or len(words) < 4:
        return -1, 0
    offset_type = words[2]
    index = offset_type & 0x7FF
    constant_type = (offset_type >> 16) & 0xFF
    size_dwords = words[3] & 0xFFF
    bases = {0: 0x4000, 1: 0x4800, 2: 0x4900, 3: 0x4908, 4: 0x2000}
    if constant_type not in bases or not packet.reads:
        return constant_type, 0
    data = packet.reads[-1].data
    if data is None:
        return constant_type, 0
    value_count = min(size_dwords, len(data) // 4)
    values = struct.unpack(f">{value_count}I", data[: value_count * 4])
    index += bases[constant_type]
    written = 0
    for offset, value in enumerate(values):
        target = index + offset
        if target >= len(registers):
            break
        registers[target] = value
        written += 1
    return constant_type, written


def texture_fetches(registers: list[int]) -> list[tuple[int, int, tuple[int, ...]]]:
    result = []
    for slot in range(FETCH_COUNT):
        start = FETCH_REGISTER_BASE + slot * FETCH_DWORDS
        words = tuple(registers[start : start + FETCH_DWORDS])
        if len(words) != FETCH_DWORDS or words[0] & 3 != 2:
            continue
        base = words[1] & 0xFFFFF000
        if base:
            result.append((slot, base, words))
    return result


def format_fetch(fetch: tuple[int, int, tuple[int, ...]]) -> str:
    slot, base, words = fetch
    pitch = ((words[0] >> 22) & 0x1FF) << 5
    tiled = words[0] >> 31
    texture_format = words[1] & 0x3F
    width = (words[2] & 0x1FFF) + 1
    height = ((words[2] >> 13) & 0x1FFF) + 1
    return (
        f"f{slot}={base:08X}:{width}x{height}:pitch={pitch}:"
        f"fmt={texture_format}:tiled={tiled}"
    )


def draw_initiator(packet: Packet) -> tuple[int, int, int]:
    words = packet.words
    if packet.opcode == PM4_DRAW_INDX:
        initiator = words[2] if len(words) > 2 else 0
    else:
        initiator = words[1] if len(words) > 1 else 0
    return initiator & 0x3F, (initiator >> 6) & 3, initiator >> 16


def analyze(
    path: Path, minimum_read: int, summary_only: bool, selected_frame: int | None
) -> None:
    data = path.read_bytes()
    if len(data) < TRACE_HEADER_SIZE:
        raise ValueError("trace is smaller than its header")
    version, commit, title_id = struct.unpack_from("<I40sI", data, 0)
    if version != 1:
        raise ValueError(f"unsupported trace version {version}")
    commit_text = commit.rstrip(b"\0").decode("ascii", errors="replace")
    print(
        f"trace={path} bytes={len(data)} version={version} "
        f"commit={commit_text} title={title_id:08X}"
    )

    registers = [0] * 0x5003
    packets: list[Packet] = []
    packet_stack: list[Packet] = []
    draws: list[tuple[Packet, list[tuple[int, int, tuple[int, ...]]]]] = []
    large_memory_reads: list[tuple[int, int, MemoryRead]] = []
    memory_reads = 0
    load_constant_packets = 0
    load_fetch_dwords = 0
    current_frame = 0
    pos = TRACE_HEADER_SIZE
    while pos < len(data):
        if pos + 4 > len(data):
            raise ValueError(f"truncated command header at file offset {pos:#x}")
        command_type = struct.unpack_from("<I", data, pos)[0]
        if command_type in (PRIMARY_BUFFER_START, INDIRECT_BUFFER_START):
            if pos + 12 > len(data):
                raise ValueError(f"truncated buffer header at file offset {pos:#x}")
            _, _, count = struct.unpack_from("<III", data, pos)
            end = pos + 12 + count * 4
            if end > len(data):
                raise ValueError(f"truncated buffer at file offset {pos:#x}")
            pos = end
        elif command_type in (PRIMARY_BUFFER_END, INDIRECT_BUFFER_END):
            pos += 4
        elif command_type == PACKET_START:
            if pos + 12 > len(data):
                raise ValueError(f"truncated packet header at file offset {pos:#x}")
            _, base, count = struct.unpack_from("<III", data, pos)
            end = pos + 12 + count * 4
            raw = data[pos + 12 : end]
            if len(raw) != count * 4:
                raise ValueError("truncated packet")
            words = list(struct.unpack(f">{count}I", raw)) if count else []
            packet = Packet(len(packets), current_frame, base, words)
            packets.append(packet)
            packet_stack.append(packet)
            write_registers(registers, packet)
            pos += 12 + count * 4
        elif command_type == PACKET_END:
            if packet_stack:
                packet = packet_stack.pop()
                constant_type, written = load_memory_registers(registers, packet)
                if constant_type >= 0:
                    load_constant_packets += 1
                    if constant_type == 1:
                        load_fetch_dwords += written
                if packet.opcode in (PM4_DRAW_INDX, PM4_DRAW_INDX_2):
                    draws.append((packet, texture_fetches(registers)))
            pos += 4
        elif command_type in (MEMORY_READ, MEMORY_WRITE):
            if pos + 20 > len(data):
                raise ValueError(f"truncated memory header at file offset {pos:#x}")
            _, base, encoding, encoded_size, decoded_size = struct.unpack_from(
                "<IIIII", data, pos
            )
            payload_start = pos + 20
            payload_end = payload_start + encoded_size
            if payload_end > len(data):
                raise ValueError(f"truncated memory payload at file offset {pos:#x}")
            payload = data[payload_start:payload_end]
            decoded = decode_payload(encoding, payload, decoded_size)
            if command_type == MEMORY_READ:
                memory_reads += 1
                if packet_stack:
                    packet = packet_stack[-1]
                    read = MemoryRead(
                        base,
                        decoded_size,
                        sum(value != 0 for value in decoded),
                        decoded if packet.opcode == PM4_LOAD_ALU_CONSTANT else None,
                    )
                    packet.reads.append(read)
                    if decoded_size >= minimum_read:
                        large_memory_reads.append((packet.ordinal, packet.opcode, read))
            pos = payload_end
        elif command_type == EDRAM_SNAPSHOT:
            if pos + 12 > len(data):
                raise ValueError(f"truncated EDRAM header at file offset {pos:#x}")
            _, _, encoded_size = struct.unpack_from("<III", data, pos)
            end = pos + 12 + encoded_size
            if end > len(data):
                raise ValueError(f"truncated EDRAM payload at file offset {pos:#x}")
            pos = end
        elif command_type == EVENT:
            if pos + 8 > len(data):
                raise ValueError(f"truncated event at file offset {pos:#x}")
            _, event_type = struct.unpack_from("<II", data, pos)
            pos += 8
            if event_type == 0:
                current_frame += 1
        elif command_type == REGISTERS:
            if pos + 24 > len(data):
                raise ValueError(f"truncated registers header at file offset {pos:#x}")
            _, first, count = struct.unpack_from("<III", data, pos)
            encoding, encoded_size = struct.unpack_from("<II", data, pos + 16)
            payload_start = pos + 24
            payload_end = payload_start + encoded_size
            if payload_end > len(data):
                raise ValueError(f"truncated registers payload at file offset {pos:#x}")
            payload = data[payload_start:payload_end]
            decoded = decode_payload(encoding, payload, count * 4)
            values = struct.unpack(f"<{count}I", decoded)
            registers[first : first + count] = values
            pos = payload_end
        elif command_type == GAMMA_RAMP:
            if pos + 16 > len(data):
                raise ValueError(f"truncated gamma header at file offset {pos:#x}")
            _, encoded_size = struct.unpack_from("<II", data, pos + 8)
            end = pos + 16 + encoded_size
            if end > len(data):
                raise ValueError(f"truncated gamma payload at file offset {pos:#x}")
            pos = end
        else:
            raise ValueError(f"unknown command {command_type} at file offset {pos:#x}")

    print(
        f"packets={len(packets)} draws={len(draws)} memory_reads={memory_reads} "
        f"frames={current_frame} open_packet_depth={len(packet_stack)} "
        f"load_constant_packets={load_constant_packets} "
        f"load_fetch_dwords={load_fetch_dwords}"
    )
    frame_draws: dict[int, list[tuple[Packet, list[tuple[int, int, tuple[int, ...]]]]]] = {}
    for draw in draws:
        frame_draws.setdefault(draw[0].frame, []).append(draw)
    for frame in sorted(frame_draws):
        frame_fetches = {
            format_fetch(fetch)
            for _, fetches in frame_draws[frame]
            for fetch in fetches
        }
        fetch_text = ",".join(sorted(frame_fetches)) or "-"
        print(f"frame={frame:04d} draws={len(frame_draws[frame])} fetches={fetch_text}")

    if summary_only:
        return
    for draw_index, (packet, fetches) in enumerate(draws):
        if selected_frame is not None and packet.frame != selected_frame:
            continue
        prim, source, count = draw_initiator(packet)
        large_reads = [read for read in packet.reads if read.size >= minimum_read]
        read_text = ",".join(
            f"{read.base:08X}+{read.size:X}:nz={read.nonzero}"
            for read in large_reads
        ) or "-"
        fetch_text = ",".join(format_fetch(fetch) for fetch in fetches) or "-"
        print(
            f"draw={draw_index:03d} frame={packet.frame:04d} "
            f"packet={packet.ordinal:04d} pm4={packet.base:08X} "
            f"op={packet.opcode:02X} prim={prim:02X} src={source} count={count} "
            f"reads={read_text} fetches={fetch_text}"
        )
    print(f"memory_reads_at_least_{minimum_read:#x}={len(large_memory_reads)}")
    for packet_ordinal, opcode, read in large_memory_reads:
        packet = packets[packet_ordinal]
        if selected_frame is not None and packet.frame != selected_frame:
            continue
        print(
            f"read frame={packet.frame:04d} packet={packet_ordinal:04d} op={opcode:02X} "
            f"base={read.base:08X} size={read.size:X} nonzero={read.nonzero}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--minimum-read", type=lambda value: int(value, 0), default=4096)
    parser.add_argument("--summary-only", action="store_true")
    parser.add_argument("--frame", type=int)
    args = parser.parse_args()
    analyze(args.trace, args.minimum_read, args.summary_only, args.frame)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

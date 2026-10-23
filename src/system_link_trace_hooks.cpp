// Default-off System Link trace. Hooks observe guest socket wrappers and the
// GameSpy LAN browse and QR2 host entry points without changing guest state.
// Logs carry direction, kind, framing type, size and ports, never payloads.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "system_link_trace.h"

REXCVAR_DECLARE(bool, system_link_trace);

REX_EXTERN(__imp__sub_827F1368);
REX_EXTERN(__imp__sub_827F1330);

namespace
{

using rerevved::system_link::DatagramKey;
using rerevved::system_link::Direction;

rerevved::system_link::DatagramCounters& counters()
{
    static rerevved::system_link::DatagramCounters instance;
    return instance;
}

bool traceEnabled()
{
    return REXCVAR_GET(system_link_trace);
}

bool readGuestBytes(uint32_t address, uint8_t* out, uint32_t size)
{
    if (!size || address < 0x10000 || address > 0xFFFFF000u - size)
    {
        return false;
    }
    auto* memory = REX_KERNEL_MEMORY();
    auto* heap   = memory->LookupHeap(address);
    if (!heap || memory->LookupHeap(address + size - 1) != heap ||
        heap->QueryRangeAccess(address, address + size - 1) == rex::memory::PageAccess::kNoAccess)
    {
        return false;
    }
    std::copy_n(memory->TranslateVirtual<const uint8_t*>(address), size, out);
    return true;
}

// Big-endian guest word, or 0xFFFFFFFF when the address is unreadable.
uint32_t readGuestWord(uint32_t address)
{
    std::array<uint8_t, 4> raw{};
    if (!readGuestBytes(address, raw.data(), raw.size()))
    {
        return 0xFFFFFFFFu;
    }
    return static_cast<uint32_t>(raw[0]) << 24 | static_cast<uint32_t>(raw[1]) << 16 |
           static_cast<uint32_t>(raw[2]) << 8 | raw[3];
}

// Guest XSOCKADDR_IN: big-endian family, port and IPv4 address.
bool readGuestEndpoint(uint32_t address, uint32_t& ipv4, uint16_t& port)
{
    std::array<uint8_t, 8> raw{};
    if (!readGuestBytes(address, raw.data(), raw.size()))
    {
        return false;
    }
    port = static_cast<uint16_t>(raw[2] << 8 | raw[3]);
    ipv4 = static_cast<uint32_t>(raw[4]) << 24 | static_cast<uint32_t>(raw[5]) << 16 |
           static_cast<uint32_t>(raw[6]) << 8 | raw[7];
    return true;
}

void record(Direction direction,
            uint32_t  buffer,
            uint32_t  size,
            uint32_t  endpoint,
            int32_t   result)
{
    std::array<uint8_t, rerevved::system_link::kClassifiedHeadSize> head{};

    const uint32_t headSize = std::min<uint32_t>(size, head.size());
    const bool     haveHead = readGuestBytes(buffer, head.data(), headSize);
    uint32_t       ipv4     = 0;
    uint16_t       port     = 0;
    if (endpoint)
    {
        readGuestEndpoint(endpoint, ipv4, port);
    }

    const auto key = rerevved::system_link::MakeDatagramKey(
        direction,
        std::span<const uint8_t>(head.data(), haveHead ? headSize : 0),
        size,
        ipv4,
        port,
        result < 0);
    const uint64_t count = counters().Record(key);
    if (!rerevved::system_link::ShouldLogCount(count))
    {
        return;
    }
    REXLOG_INFO("System Link trace: {} {} type=0x{:02X} size={} port={} failed={} count={}",
                direction == Direction::Send ? "send" : "recv",
                rerevved::system_link::KindName(key.kind),
                key.type,
                key.size,
                key.port == 0 && key.kind == rerevved::system_link::DatagramKind::Echo
                    ? std::string("26900-27000")
                    : std::to_string(key.port),
                key.failed,
                count);
}

} // namespace

// sendto(socket, buffer, length, flags, to, to_length)
REX_HOOK_RAW(sub_827F1368)
{
    const uint32_t buffer   = ctx.r4.u32;
    const auto     length   = static_cast<int32_t>(ctx.r5.u32);
    const uint32_t size     = length < 0 ? 0 : static_cast<uint32_t>(length);
    const uint32_t endpoint = ctx.r7.u32 != 0 && static_cast<int32_t>(ctx.r8.u32) >= 8 ? ctx.r7.u32 : 0;
    __imp__sub_827F1368(ctx, base);
    if (traceEnabled())
    {
        record(Direction::Send, buffer, size, endpoint, static_cast<int32_t>(ctx.r3.u32));
    }
}

// recvfrom(socket, buffer, length, flags, from, from_length)
REX_HOOK_RAW(sub_827F1330)
{
    const uint32_t buffer   = ctx.r4.u32;
    const uint32_t endpoint = ctx.r7.u32;
    __imp__sub_827F1330(ctx, base);
    const auto received = static_cast<int32_t>(ctx.r3.u32);
    if (traceEnabled() && received >= 0)
    {
        record(Direction::Receive, buffer, static_cast<uint32_t>(received), endpoint, received);
    }
}

// ServerBrowserLANUpdate(browser, async, first port, last port).
void TraceSystemLinkLanUpdate(PPCRegister& r5, PPCRegister& r6)
{
    if (traceEnabled())
    {
        REXLOG_INFO("System Link trace: LAN browse update ports={}-{}", r5.u32, r6.u32);
    }
}

void TraceSystemLinkLanUpdateResult(PPCRegister& r3)
{
    if (traceEnabled())
    {
        REXLOG_INFO("System Link trace: LAN browse update result={}", r3.u32);
    }
}

// qr2_initA: r5 base port; r8 and r9 are the session-info blob pointer and
// length that the host advertises in its echo replies.
void TraceSystemLinkQr2Init(PPCRegister& r5, PPCRegister& r8, PPCRegister& r9)
{
    if (traceEnabled())
    {
        REXLOG_INFO("System Link trace: QR2 init base_port={} session_info=0x{:08X} length={}",
                    r5.u32,
                    r8.u32,
                    r9.u32);
    }
}

void TraceSystemLinkQr2InitResult(PPCRegister& r3)
{
    if (traceEnabled())
    {
        REXLOG_INFO("System Link trace: QR2 init result={}", r3.u32);
    }
}

// Server browser callback(browser, reason, server, instance). The guest
// switch accepts reasons 0 through 5.
void TraceSystemLinkBrowserCallback(PPCRegister& r4)
{
    static std::array<std::atomic<uint64_t>, 7> reasonCounts{};
    if (!traceEnabled())
    {
        return;
    }
    const uint32_t reason = std::min<uint32_t>(r4.u32, reasonCounts.size() - 1);
    const uint64_t count  = ++reasonCounts[reason];
    if (rerevved::system_link::ShouldLogCount(count))
    {
        REXLOG_INFO("System Link trace: browser callback reason={} count={}", r4.u32, count);
    }
}

// GT2 connect-attempt admission 0x82E87950: the virtual predicate it calls
// (r10) with the client's initial message length (r6), and its verdict.
void TraceSystemLinkAdmissionCall(PPCRegister& r6, PPCRegister& r10)
{
    if (traceEnabled())
    {
        REXLOG_INFO("System Link trace: GT2 admission predicate=0x{:08X} message_length={}",
                    r10.u32,
                    r6.u32);
    }
}

void TraceSystemLinkAdmissionResult(PPCRegister& r3)
{
    if (traceEnabled())
    {
        REXLOG_INFO("System Link trace: GT2 admission accepted={}", r3.u32 & 0xFF);
    }
}

// Candidate predicate 0x82CA09B8: the peer table's +8 and +12 fields decide
// the host-role branch (equal and not -1).
void TraceSystemLinkAdmissionRole(PPCRegister& r3)
{
    if (traceEnabled())
    {
        REXLOG_INFO("System Link trace: GT2 admission role table=0x{:08X} +8=0x{:08X} +12=0x{:08X}",
                    r3.u32,
                    readGuestWord(r3.u32 + 8),
                    readGuestWord(r3.u32 + 12));
    }
}

void TraceSystemLinkAdmissionHostResult(PPCRegister& r3)
{
    if (traceEnabled())
    {
        REXLOG_INFO("System Link trace: GT2 admission host branch result={}", r3.u32 & 0xFF);
    }
}

// Non-host branch: expected version (r3) against the client's 4-byte version
// at r1+80, which stays 0 unless the message is exactly 4 bytes.
void TraceSystemLinkAdmissionVersion(PPCRegister& r1, PPCRegister& r3)
{
    if (traceEnabled())
    {
        REXLOG_INFO("System Link trace: GT2 admission version expected=0x{:08X} client=0x{:08X}",
                    r3.u32,
                    readGuestWord(r1.u32 + 80));
    }
}

// Host branch of 0x82CA09B8: the manager admission target (r10) and the
// deserialized client descriptor (r4), whose user count is at +36.
void TraceSystemLinkAdmissionHostCall(PPCRegister& r4, PPCRegister& r10)
{
    if (traceEnabled())
    {
        REXLOG_INFO("System Link trace: GT2 admission manager=0x{:08X} descriptor=0x{:08X} users={}",
                    r10.u32,
                    r4.u32,
                    r4.u32 ? readGuestWord(r4.u32 + 36) : 0);
    }
}

// Candidate manager admission 0x82CA69F0: peer count (must be below 8), XGI
// session admission result, application admission target and result.
void TraceSystemLinkAdmissionPeers(PPCRegister& r30)
{
    if (traceEnabled())
    {
        REXLOG_INFO("System Link trace: GT2 admission peers={}", r30.u32);
    }
}

void TraceSystemLinkAdmissionSession(PPCRegister& r3)
{
    if (traceEnabled())
    {
        REXLOG_INFO("System Link trace: GT2 admission session result={}", r3.u32 & 0xFF);
    }
}

void TraceSystemLinkAdmissionAppCall(PPCRegister& r10)
{
    if (traceEnabled())
    {
        REXLOG_INFO("System Link trace: GT2 admission application=0x{:08X}", r10.u32);
    }
}

void TraceSystemLinkAdmissionAppResult(PPCRegister& r3)
{
    if (traceEnabled())
    {
        REXLOG_INFO("System Link trace: GT2 admission application result={}", r3.u32 & 0xFF);
    }
}

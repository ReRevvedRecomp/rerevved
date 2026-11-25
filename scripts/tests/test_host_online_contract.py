"""Contract tests for the one-click online hosting script."""

import re
import shutil
import socket
import subprocess
import sys
import threading
import unittest
from pathlib import Path

PACKAGING = Path(__file__).resolve().parents[1] / "packaging"
SCRIPT = PACKAGING / "host-online.ps1"
LAUNCHER = PACKAGING / "host-online.cmd"
MANUAL = PACKAGING / "host-manual.cmd"


class HostOnlineSourceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = SCRIPT.read_text(encoding="ascii")

    def test_launcher_runs_the_script_and_pauses_on_error(self):
        text = LAUNCHER.read_text(encoding="ascii")
        self.assertIn(
            'powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0host-online.ps1"',
            text,
        )
        self.assertRegex(text, r"(?m)^if errorlevel 1 pause\s*$")

    def test_upnp_mapping_uses_the_builtin_com_object(self):
        self.assertIn("HNetCfg.NATUPnP", self.source)
        self.assertIn("StaticPortMappingCollection", self.source)
        self.assertIn("'ReRevved online server'", self.source)
        self.assertRegex(self.source, r"\.Add\(\$Port, 'UDP', \$Port, \$local,")
        self.assertIn(".ExternalIPAddress", self.source)

    def test_router_lookup_retries_and_settings_open_only_for_a_public_network(self):
        lookup = re.search(
            r"function Get-RouterMappings \{(.*?)\n\}\n", self.source, re.S
        ).group(1)
        self.assertIn("foreach ($attempt in 1..3)", lookup)
        self.assertIn("if ($found) { return $found }", lookup)
        self.assertEqual(self.source.count("HNetCfg.NATUPnP"), 1)
        # A disabled discovery service is reported instead of the network type.
        opened = re.search(
            r"if \(\$cause -eq 'NoRouter'\) \{.*?\$cause = 'NoDiscovery'\s+"
            r"\} elseif \(\$category -eq 'Public'\) \{(.*?)\n                \}",
            self.source,
            re.S,
        )
        self.assertIsNotNone(opened)
        self.assertIn("Start-Process $page", opened.group(1))
        self.assertEqual(self.source.count("Start-Process $page"), 1)
        self.assertNotIn("Set-NetConnectionProfile", self.source)
        self.assertNotIn("Set-Service", self.source)

    def test_fallback_prompt_and_pointer_to_help(self):
        self.assertIn("CAN'T HOST?", self.source)
        self.assertIn("ONLINE.txt", self.source)
        self.assertIn(
            "Address friends can reach (for example a Tailscale or ZeroTier address), "
            "or press Enter to quit",
            self.source,
        )

    def test_password_is_created_only_when_missing(self):
        self.assertEqual(self.source.count("--create-password"), 1)
        self.assertRegex(
            self.source,
            r"if \(-not \(Test-Path -LiteralPath \$passwordFile\)\) "
            r"\{ \$passwordArgs \+= '--create-password' \}",
        )
        self.assertNotIn("Write-Host $password", self.source)

    def test_codes_go_to_the_file_and_clipboard_not_the_console(self):
        self.assertIn("online-invite --address", self.source)
        self.assertIn('"127.0.0.1:$Port"', self.source)
        self.assertIn("invite-codes.txt", self.source)
        self.assertIn("Set-Clipboard -Value $friendsCode", self.source)
        for line in self.source.splitlines():
            if re.search(r"Write-(Host|Output)", line):
                self.assertNotRegex(line, r"\$(friendsCode|thisPcCode|password)\b")

    def test_game_start_flags(self):
        self.assertIn('"--system_link_invite=$thisPcCode"', self.source)
        self.assertIn("'--system_link_lan_enabled=false'", self.source)
        # Friends' code and Discord Join carry the same outside address.
        self.assertIn("New-Invite $rexglue $friendsAddress $passwordArgs", self.source)
        self.assertIn("$advertised = $friendsAddress", self.source)
        self.assertIn(
            '$gameArgs += "--system_link_advertised_address=$advertised"', self.source
        )
        self.assertIn("if ($advertised.Length -gt 35)", self.source)
        self.assertIn("Discord Join won't work with this address", self.source)
        self.assertIn("Get-Process -Name rerevved", self.source)

    def test_server_starts_before_the_game_and_is_waited_on(self):
        for text in (
            "'--log-file'",
            "'online-server', '--password-file'",
            "'--port', $Port, '--max-members', 8",
            "Start-Process -FilePath $rexglue -NoNewWindow -PassThru",
            "-RedirectStandardOutput (Join-Path $state 'online-server-console.txt')",
            "-RedirectStandardError (Join-Path $state 'online-server-console-err.txt')",
            "Get-NetUDPEndpoint -LocalPort $Port -OwningProcess $server.Id",
            "AddSeconds(10)",
            "Wait-Process -Id $server.Id -Timeout ($NatPmpLifetime / 2)",
        ):
            self.assertIn(text, self.source)
        self.assertIn("online-server.log", self.source)
        self.assertLess(
            self.source.index("Get-NetUDPEndpoint"),
            self.source.index("Start-Process -FilePath $game"),
        )
        self.assertLess(
            self.source.index("Start-Process -FilePath $game"),
            self.source.index("Wait-Process"),
        )

    def test_server_failure_returns_nonzero(self):
        self.assertIn("if ($server.ExitCode -ne 0)", self.source)
        self.assertRegex(
            self.source,
            r"The online server stopped with exit code[^\n]*\n\s+return 1",
        )
        self.assertRegex(
            self.source,
            r"did not start listening on UDP port[\s\S]*?return 1",
        )

    def test_mapping_is_removed_only_in_finally(self):
        # The last finally is Start-Hosting's; a helper has its own.
        cleanup = self.source.rsplit("} finally {", 1)[1]
        self.assertNotIn("\nfunction ", cleanup)
        self.assertIn("if ($created)", cleanup)
        self.assertIn("$mappings.Remove($Port, 'UDP')", cleanup)
        self.assertIn("Port mapping removed", cleanup)
        self.assertEqual(self.source.count(".Remove("), 1)

    def test_existing_mapping_is_found_by_enumeration(self):
        self.assertNotIn(".Item(", self.source)
        self.assertIn("foreach ($item in $mappings)", self.source)
        self.assertIn("if ($item.Protocol -eq 'UDP') { $item }", self.source)
        self.assertIn(
            "$mapped | Where-Object { $_.ExternalPort -eq $Port }", self.source
        )

    def test_only_our_own_mapping_is_ever_owned(self):
        # Another program's mapping is never touched: its port is skipped before
        # any add. Ownership is set only by a fresh add or by reclaiming a
        # matching mapping of ours.
        self.assertIn(
            "$mapped | Where-Object { $_.Description -ne $MappingName }", self.source
        )
        self.assertLess(
            self.source.index("Select-FreePort $Port $taken"),
            self.source.index("$mappings.Add("),
        )
        self.assertEqual(self.source.count("$created = $true"), 2)
        reclaim = re.search(
            r"elseif \(\$mapping\.Enabled -and \$mapping\.InternalPort -eq \$Port -and"
            r"\s+\$mapping\.InternalClient -eq \$local\) \{[^}]*\$created = \$true",
            self.source,
        )
        self.assertIsNotNone(reclaim)
        fresh = re.search(
            r"if \(-not \$mapping\) \{\s+\$mapping = \$mappings\.Add\([^\n]*\n\s+"
            r"\$created = \$true",
            self.source,
        )
        self.assertIsNotNone(fresh)
        # A stale mapping of ours is repointed without taking ownership.
        stale = re.search(r"\} else \{[^}]*\$mappings\.Add\([^}]*\}", self.source)
        self.assertIsNotNone(stale)
        self.assertNotIn("$created", stale.group(0))

    def test_no_kills_elevation_firewall_or_network_lookups(self):
        for forbidden in (
            "Stop-Process",
            "taskkill",
            "Kill(",
            "netsh",
            "RunAs",
            "Invoke-WebRequest",
            "Invoke-RestMethod",
            "WebClient",
            "curl",
            # The firewall is read, never changed.
            "New-NetFirewallRule",
            "Set-NetFirewallRule",
            "Remove-NetFirewallRule",
            "Enable-NetFirewallRule",
            "Disable-NetFirewallRule",
        ):
            self.assertNotIn(forbidden, self.source)
        self.assertNotRegex(self.source, r"(?i)https?://")

    def test_script_avoids_powershell_7_syntax(self):
        for forbidden in ("??", "?.", "&&", "||", "ForEach-Object -Parallel"):
            self.assertNotIn(forbidden, self.source)
        self.assertNotRegex(self.source, r"\$\w+\s*\?\s*[^\s]+\s*:")


class HostManualSourceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = MANUAL.read_text(encoding="ascii")

    def test_missing_executable_message_names_the_recovery_step(self):
        block = self.text.split(":missing_exe\n", 1)[1]
        block = block.split("\n:state_error", 1)[0]
        self.assertEqual(
            block.strip().splitlines(),
            [
                "echo ERROR: rexglue.exe is missing. Extract the entire ReRevved "
                "release archive, then run host-manual.cmd again.",
                "goto done",
            ],
        )

    def test_server_console_goes_to_the_log_only(self):
        self.assertIn(
            "The online server is running on UDP !server_port!. Leave this window open; "
            "press Ctrl+C to stop. Details: !server_log!",
            self.text,
        )
        self.assertRegex(
            self.text,
            r'(?m)^rexglue\.exe --log-file "!server_log!" online-server .*--max-members 8 '
            r">nul 2>&1\s*$",
        )

    def test_invite_calls_capture_only_the_code(self):
        calls = [line for line in self.text.splitlines() if "online-invite" in line]
        self.assertEqual(len(calls), 2)
        for line in calls:
            self.assertRegex(line, r'>> "!invites!" 2>>? "!invite_errors!"$')
        self.assertIn('start "" notepad.exe "!invites!"', self.text)

    def test_no_process_kills_and_crlf(self):
        for forbidden in ("Stop-Process", "taskkill", "netsh"):
            self.assertNotIn(forbidden, self.text)
        data = MANUAL.read_bytes()
        data.decode("ascii")
        self.assertNotIn(b"\n", data.replace(b"\r\n", b""))


@unittest.skipUnless(
    sys.platform == "win32" and shutil.which("powershell"),
    "needs Windows PowerShell",
)
class HostOnlineHelperTest(unittest.TestCase):
    """Runs the pure helpers with the script dot-sourced in helper-only mode."""

    def run_helpers(self, body):
        command = f". '{SCRIPT}' -LoadHelpersOnly; {body}"
        result = subprocess.run(
            [
                "powershell",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-Command",
                command,
            ],
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.split()

    def test_address_ranges(self):
        cases = {
            "10.1.2.3": True,
            "172.15.255.255": False,
            "172.16.0.1": True,
            "172.31.255.255": True,
            "172.32.0.1": False,
            "192.168.1.1": True,
            "192.167.255.255": False,
            "100.63.255.255": False,
            "100.64.0.1": True,
            "100.127.255.255": True,
            "100.128.0.1": False,
            "127.0.0.1": True,
            "169.254.1.1": True,
            "8.8.8.8": False,
            "203.0.113.5": False,
            "0.0.0.0": True,
            "1.2.3": True,
            "300.1.1.1": True,
            "not-an-address": True,
        }
        names = ",".join(f"'{address}'" for address in cases)
        output = self.run_helpers(
            f"foreach ($a in {names}) {{ Test-UnreachableAddress $a }}"
        )
        self.assertEqual(output, ["True" if v else "False" for v in cases.values()])

    def test_default_route_pick_uses_lowest_metric(self):
        output = self.run_helpers(
            "$r = @(1..3 | ForEach-Object { [pscustomobject]@{InterfaceIndex=$_; "
            "Metric=(50, 10, 20)[$_ - 1]; Physical=$true} }); "
            "(Select-DefaultRoute $r).InterfaceIndex; "
            "$null -eq (Select-DefaultRoute @())"
        )
        self.assertEqual(output, ["2", "True"])

    def test_default_route_pick_prefers_a_physical_adapter_over_a_vpn(self):
        output = self.run_helpers(
            "$r = @([pscustomobject]@{InterfaceIndex=7; Metric=5; Physical=$false}, "
            "[pscustomobject]@{InterfaceIndex=3; Metric=35; Physical=$true}); "
            "(Select-DefaultRoute $r).InterfaceIndex; "
            "(Select-DefaultRoute @($r[0])).InterfaceIndex"
        )
        self.assertEqual(output, ["3", "7"])

    def test_typed_address_keeps_its_own_port_or_takes_the_server_port(self):
        cases = {
            "100.101.102.103": "100.101.102.103:30100",
            "example.net:41234": "example.net:41234",
            "example.net:65535": "example.net:65535",
            "example.net:0": "None",
            "example.net:65536": "None",
            "example.net:": "None",
            "example.net:12:34": "None",
            "bad name": "None",
        }
        body = "; ".join(
            f"$a = ConvertTo-FriendsAddress '{text}' 30100; "
            "if ($null -eq $a) { 'None' } else { $a }"
            for text in cases
        )
        self.assertEqual(self.run_helpers(body), list(cases.values()))

    def serve_nat_pmp(self, answer):
        """Runs a loopback gateway; answer(request) returns the reply or None."""
        gateway = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        gateway.bind(("127.0.0.1", 0))
        gateway.settimeout(0.2)
        requests = []
        stop = threading.Event()

        def serve():
            while not stop.is_set():
                try:
                    data, peer = gateway.recvfrom(64)
                except socket.timeout:
                    continue
                requests.append(data)
                reply = answer(data)
                if reply:
                    gateway.sendto(reply, peer)

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()

        def close():
            stop.set()
            thread.join()
            gateway.close()

        self.addCleanup(close)
        return gateway.getsockname()[1], requests

    def test_nat_pmp_opens_renews_and_removes_a_mapping(self):
        def answer(data):
            if data[1] == 0:
                return bytes([0, 128, 0, 0, 0, 0, 0, 1, 203, 0, 113, 5])
            # The router grants its own outside port, and none for a removal.
            outside = bytes(2) if data[8:12] == bytes(4) else bytes([0xA1, 0x12])
            return bytes([0, 129, 0, 0, 0, 0, 0, 1]) + data[4:6] + outside + data[8:12]

        port, requests = self.serve_nat_pmp(answer)
        output = self.run_helpers(
            f"$g = Open-NatPmpPort '127.0.0.1' 30100 {port}; $g.Address; $g.Port; "
            f"Request-NatPmpPort '127.0.0.1' 30100 $g.Port $NatPmpLifetime {port}; "
            f"Request-NatPmpPort '127.0.0.1' 30100 0 0 {port}"
        )
        self.assertEqual(output, ["203.0.113.5", "41234", "41234", "0"])
        mapping = bytes([0, 1, 0, 0, 0x75, 0x94])
        hour = bytes([0, 0, 0x0E, 0x10])
        self.assertEqual(
            requests,
            [
                bytes(2),
                mapping + bytes([0x75, 0x94]) + hour,
                mapping + bytes([0xA1, 0x12]) + hour,
                mapping + bytes(2) + bytes(4),
            ],
        )

    def test_nat_pmp_gives_up_on_a_refusing_or_silent_router(self):
        # Result code 2 is "not authorized"; most routers without NAT-PMP stay silent.
        refusing, _ = self.serve_nat_pmp(
            lambda data: bytes([0, data[1] + 128, 0, 2, 0, 0, 0, 1, 0, 0, 0, 0])
        )
        silent, asked = self.serve_nat_pmp(lambda data: None)
        output = self.run_helpers(
            f"$null -eq (Open-NatPmpPort '127.0.0.1' 30100 {refusing}); "
            f"$null -eq (Open-NatPmpPort '127.0.0.1' 30100 {silent})"
        )
        self.assertEqual(output, ["True", "True"])
        self.assertEqual(asked, [bytes(2), bytes(2)])

    def test_free_port_skips_ports_other_programs_mapped(self):
        output = self.run_helpers(
            "Select-FreePort 30100 @(); "
            "Select-FreePort 30100 @(30100, 30101, 40000); "
            "$null -eq (Select-FreePort 30100 @(30100..30109)); "
            "$null -eq (Select-FreePort 65535 @(65535))"
        )
        self.assertEqual(output, ["30100", "30102", "True", "True"])

    def test_firewall_block_must_apply_to_this_network_type(self):
        def rule(action, profile, enabled="True", direction="Inbound"):
            return (
                f"[pscustomobject]@{{Enabled='{enabled}'; Direction='{direction}'; "
                f"Action='{action}'; Profile='{profile}'}}"
            )

        cases = [
            (rule("Block", "Public"), "Public", True),
            (rule("Block", "Public"), "Private", False),
            (rule("Block", "Domain, Private"), "Private", True),
            (rule("Block", "Domain, Private"), "DomainAuthenticated", True),
            (rule("Block", "Any"), "Private", True),
            (rule("Allow", "Any"), "Private", False),
            (rule("Block", "Any", enabled="False"), "Private", False),
            (rule("Block", "Any", direction="Outbound"), "Private", False),
        ]
        body = "; ".join(
            f"Test-FirewallBlocked @({rules}) '{category}'"
            for rules, category, _ in cases
        )
        output = self.run_helpers(body + "; Test-FirewallBlocked @() 'Public'")
        expected = ["True" if blocked else "False" for _, _, blocked in cases]
        self.assertEqual(output, expected + ["False"])

    def test_failure_text_names_the_public_network_only_when_it_is_the_cause(self):
        output = self.run_helpers(
            "(Get-FailureText 'NoRouter' 'Public' 30100) -match 'Public'; "
            "(Get-FailureText 'NoRouter' 'Private' 30100) -match 'Public'; "
            "(Get-FailureText 'SharedAddress' 'Public' 30100) -match 'Public'; "
            "(Get-FailureText 'PortsTaken' 'Private' 30100) -match '-Port 30200'"
        )
        self.assertEqual(output, ["True", "False", "False", "True"])


if __name__ == "__main__":
    unittest.main()

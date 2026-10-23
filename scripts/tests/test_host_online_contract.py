"""Contract tests for the one-click online hosting script."""

import re
import shutil
import subprocess
import sys
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
        self.assertIn("'ReRevved relay'", self.source)
        self.assertRegex(self.source, r"\.Add\(\$Port, 'UDP', \$Port, \$local,")
        self.assertIn(".ExternalIPAddress", self.source)

    def test_fallback_prompt_and_pointer_to_help(self):
        self.assertIn("CAN'T HOST?", self.source)
        self.assertIn("ONLINE.txt", self.source)
        self.assertIn(
            "Address friends can reach (for example a Tailscale or ZeroTier address), "
            "or press Enter to quit",
            self.source,
        )

    def test_secret_is_created_only_when_missing(self):
        self.assertEqual(self.source.count("--create-secret"), 1)
        self.assertRegex(
            self.source,
            r"if \(-not \(Test-Path -LiteralPath \$secretFile\)\) "
            r"\{ \$secretArgs \+= '--create-secret' \}",
        )
        self.assertNotIn("Write-Host $secret", self.source)

    def test_codes_go_to_the_file_and_clipboard_not_the_console(self):
        self.assertIn("relay-invite --address", self.source)
        self.assertIn('"127.0.0.1:$Port"', self.source)
        self.assertIn("invite-codes.txt", self.source)
        self.assertIn("Set-Clipboard -Value $friendsCode", self.source)
        for line in self.source.splitlines():
            if re.search(r"Write-(Host|Output)", line):
                self.assertNotRegex(line, r"\$(friendsCode|thisPcCode|secret)\b")

    def test_game_start_flags(self):
        self.assertIn('"--system_link_invite=$thisPcCode"', self.source)
        self.assertIn("'--system_link_lan_enabled=false'", self.source)
        self.assertIn('$advertised = "${external}:$Port"', self.source)
        self.assertIn(
            '$gameArgs += "--system_link_advertised_address=$advertised"', self.source
        )
        self.assertIn("if ($advertised.Length -gt 35)", self.source)
        self.assertIn("Discord Join won't work with this address", self.source)
        self.assertIn("Get-Process -Name rerevved", self.source)

    def test_relay_starts_before_the_game_and_is_waited_on(self):
        for text in (
            "'--log-file'",
            "'relay', '--secret-file'",
            "'--port', $Port, '--max-members', 8",
            "Start-Process -FilePath $rexglue -NoNewWindow -PassThru",
            "-RedirectStandardOutput (Join-Path $state 'relay-console.txt')",
            "-RedirectStandardError (Join-Path $state 'relay-console-err.txt')",
            "Get-NetUDPEndpoint -LocalPort $Port -OwningProcess $relay.Id",
            "AddSeconds(10)",
            "Wait-Process -Id $relay.Id",
        ):
            self.assertIn(text, self.source)
        self.assertIn("relay.log", self.source)
        self.assertLess(
            self.source.index("Get-NetUDPEndpoint"),
            self.source.index("Start-Process -FilePath $game"),
        )
        self.assertLess(
            self.source.index("Start-Process -FilePath $game"),
            self.source.index("Wait-Process"),
        )

    def test_relay_failure_returns_nonzero(self):
        self.assertIn("if ($relay.ExitCode -ne 0)", self.source)
        self.assertRegex(
            self.source,
            r"The relay stopped with exit code[^\n]*\n\s+return 1",
        )
        self.assertRegex(
            self.source,
            r"did not start listening on UDP port[\s\S]*?return 1",
        )

    def test_mapping_is_removed_only_in_finally(self):
        match = re.search(r"\} finally \{(.*)\n    \}\n\}", self.source, re.S)
        self.assertIsNotNone(match)
        cleanup = match.group(1)
        self.assertIn("if ($created)", cleanup)
        self.assertIn("$mappings.Remove($Port, 'UDP')", cleanup)
        self.assertIn("Port mapping removed", cleanup)
        self.assertEqual(self.source.count(".Remove("), 1)

    def test_existing_mapping_is_found_by_enumeration(self):
        self.assertNotIn(".Item(", self.source)
        self.assertIn("foreach ($item in $mappings)", self.source)
        self.assertIn(
            "$item.ExternalPort -eq $Port -and $item.Protocol -eq 'UDP'", self.source
        )

    def test_only_our_own_mapping_is_ever_owned(self):
        # Another program's mapping is unusable; ownership is set only by a fresh
        # add or by reclaiming a matching mapping of ours.
        self.assertIn("$mapping.Description -ne $MappingName", self.source)
        self.assertIn("throw 'The port is mapped by another program.'", self.source)
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
        block = block.split("\n:documents_error", 1)[0]
        self.assertEqual(
            block.strip().splitlines(),
            [
                "echo ERROR: rexglue.exe is missing. Extract the entire ReRevved "
                "release archive, then run host-manual.cmd again.",
                "goto done",
            ],
        )

    def test_relay_console_goes_to_the_log_only(self):
        self.assertIn(
            "Relay running on UDP !relay_port!. Leave this window open; "
            "press Ctrl+C to stop. Details: !relay_log!",
            self.text,
        )
        self.assertRegex(
            self.text,
            r'(?m)^rexglue\.exe --log-file "!relay_log!" relay .*--max-members 8 '
            r">nul 2>&1\s*$",
        )

    def test_invite_calls_capture_only_the_code(self):
        calls = [line for line in self.text.splitlines() if "relay-invite" in line]
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
            "Metric=(50, 10, 20)[$_ - 1]} }); "
            "(Select-DefaultRoute $r).InterfaceIndex; "
            "$null -eq (Select-DefaultRoute @())"
        )
        self.assertEqual(output, ["2", "True"])


if __name__ == "__main__":
    unittest.main()

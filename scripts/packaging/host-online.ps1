# Hosts a private ReRevved online game: opens the UDP port with UPnP, makes the
# invite codes, starts the game on this PC's own relay, and runs the relay.
# Runs on Windows PowerShell 5.1.
param(
    [ValidateRange(1, 65535)][int]$Port = 30100,
    # Defines the helper functions and stops; the contract tests use it.
    [switch]$LoadHelpersOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$MappingName = 'ReRevved relay'

# Addresses that cannot be reached from the Internet: private, CGNAT, loopback,
# link-local and "this network".
$UnreachableRanges = @(
    @('10.0.0.0', 8), @('172.16.0.0', 12), @('192.168.0.0', 16),
    @('100.64.0.0', 10), @('127.0.0.0', 8), @('169.254.0.0', 16), @('0.0.0.0', 8)
)

function ConvertTo-IPv4Number([string]$Text) {
    if ($Text -notmatch '^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$') { return $null }
    $number = [long]0
    foreach ($part in $Matches[1], $Matches[2], $Matches[3], $Matches[4]) {
        if ([int]$part -gt 255) { return $null }
        $number = $number * 256 + [int]$part
    }
    return $number
}

# True for anything that is not a usable public IPv4 address.
function Test-UnreachableAddress([string]$Text) {
    $number = ConvertTo-IPv4Number $Text
    if ($null -eq $number) { return $true }
    foreach ($range in $UnreachableRanges) {
        $size = [math]::Pow(2, 32 - $range[1])
        $base = ConvertTo-IPv4Number $range[0]
        if ([math]::Floor($number / $size) -eq [math]::Floor($base / $size)) { return $true }
    }
    return $false
}

# Routes carry Metric (route metric plus interface metric); the lowest wins.
function Select-DefaultRoute($Routes) {
    $Routes | Sort-Object -Property Metric | Select-Object -First 1
}

function Get-LocalIPv4 {
    $routes = foreach ($route in Get-NetRoute -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0') {
        $nic = Get-NetIPInterface -AddressFamily IPv4 -InterfaceIndex $route.InterfaceIndex
        [pscustomobject]@{
            InterfaceIndex = $route.InterfaceIndex
            Metric         = $route.RouteMetric + $nic.InterfaceMetric
        }
    }
    $best = Select-DefaultRoute $routes
    if (-not $best) { return $null }
    $address = Get-NetIPAddress -AddressFamily IPv4 -InterfaceIndex $best.InterfaceIndex |
        Where-Object { $_.IPAddress -notlike '169.254.*' } |
        Select-Object -First 1
    if ($address) { return $address.IPAddress }
    return $null
}

function New-Invite([string]$Rexglue, [string]$Address, [string[]]$SecretArgs) {
    $output = & $Rexglue relay-invite --address $Address @SecretArgs
    if ($LASTEXITCODE -ne 0) { throw "Could not create an invite code for $Address." }
    $code = @($output) | Where-Object { $_ -match '^RXR1-' } | Select-Object -First 1
    if (-not $code) { throw "relay-invite printed no invite code for $Address." }
    return $code.Trim()
}

function Start-Hosting {
    $rexglue = Join-Path $PSScriptRoot 'rexglue.exe'
    $game = Join-Path $PSScriptRoot 'rerevved.exe'
    foreach ($exe in $rexglue, $game) {
        if (-not (Test-Path -LiteralPath $exe)) {
            Write-Host "ERROR: $(Split-Path -Leaf $exe) is missing. Extract the whole package and run this from its folder."
            return 1
        }
    }
    # Checked before touching the router so a stop leaves nothing to undo.
    if (Get-Process -Name rerevved -ErrorAction SilentlyContinue) {
        Write-Host 'ReRevved is already running. Close it, then run this again.'
        return 1
    }

    $state = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\ReRevved\relay'
    $null = New-Item -ItemType Directory -Force -Path $state
    $secretFile = Join-Path $state 'relay-secret.txt'
    $invites = Join-Path $state 'invite-codes.txt'
    $secretArgs = @('--secret-file', $secretFile)
    if (-not (Test-Path -LiteralPath $secretFile)) { $secretArgs += '--create-secret' }

    $mappings = $null
    $created = $false   # True only for a mapping this run added or reclaimed.
    try {
        $external = $null
        try {
            $local = Get-LocalIPv4
            if ($local) {
                $mappings = (New-Object -ComObject HNetCfg.NATUPnP).StaticPortMappingCollection
            }
            if ($mappings) {
                # If listing fails, ownership is unknown: the catch below leaves
                # $created false and the host gets the manual prompt.
                $mapping = $null
                foreach ($item in $mappings) {
                    if ($item.ExternalPort -eq $Port -and $item.Protocol -eq 'UDP') {
                        $mapping = $item
                        break
                    }
                }
                if ($mapping -and $mapping.Description -ne $MappingName) {
                    throw 'The port is mapped by another program.'
                }
                if (-not $mapping) {
                    $mapping = $mappings.Add($Port, 'UDP', $Port, $local, $true, $MappingName)
                    $created = $true
                } elseif ($mapping.Enabled -and $mapping.InternalPort -eq $Port -and
                    $mapping.InternalClient -eq $local) {
                    # Left by an earlier run (closed window, crash): reclaim it, so
                    # finally removes it this time.
                    $created = $true
                } else {
                    # Ours but stale: repoint it, and leave removal to a later run.
                    $mapping = $mappings.Add($Port, 'UDP', $Port, $local, $true, $MappingName)
                }
                $external = $mapping.ExternalIPAddress
            }
        } catch {
            $external = $null
        }
        # Behind another NAT layer the mapping is no use; finally removes it if new.
        if ($external -and (Test-UnreachableAddress $external)) { $external = $null }
        if (-not $external) {
            Write-Host "This PC cannot host automatically. See `"CAN'T HOST?`" in ONLINE.txt."
            $external = (Read-Host 'Address friends can reach (for example a Tailscale or ZeroTier address), or press Enter to quit').Trim()
            if (-not $external) { return 0 }
            if ($external -notmatch '^[A-Za-z0-9.-]{1,253}$') {
                Write-Host 'ERROR: Enter an IPv4 address or DNS name without a port.'
                return 1
            }
        } else {
            Write-Host "Opened UDP port $Port on your router."
        }

        $friendsCode = New-Invite $rexglue "${external}:$Port" $secretArgs
        # The secret file exists after the first call.
        $thisPcCode = New-Invite $rexglue "127.0.0.1:$Port" @('--secret-file', $secretFile)
        Set-Content -LiteralPath $invites -Encoding ASCII -Value @(
            "Friends' code:", $friendsCode, '', "This PC's code:", $thisPcCode)
        Set-Clipboard -Value $friendsCode
        Write-Host 'The friends'' code is copied. Paste it to your friends privately.'
        Write-Host "Both codes are in $invites. Keep them private."

        # The game's relay client gives up after 5 seconds, so the relay goes first.
        # It shares this console, so Ctrl+C reaches it as well.
        $log = Join-Path $state 'relay.log'
        # Console output goes to files so the window stays quiet; details are in the log.
        $relay = Start-Process -FilePath $rexglue -NoNewWindow -PassThru `
            -RedirectStandardOutput (Join-Path $state 'relay-console.txt') `
            -RedirectStandardError (Join-Path $state 'relay-console-err.txt') `
            -ArgumentList @(
            '--log-file', "`"$log`"", 'relay', '--secret-file', "`"$secretFile`"",
            '--port', $Port, '--max-members', 8)
        $null = $relay.Handle   # Keeps the exit code readable.
        $deadline = (Get-Date).AddSeconds(10)
        while (-not (Get-NetUDPEndpoint -LocalPort $Port -OwningProcess $relay.Id -ErrorAction SilentlyContinue)) {
            if ($relay.HasExited -or (Get-Date) -gt $deadline) {
                Write-Host "ERROR: The relay did not start listening on UDP port $Port. See $log."
                if (-not $relay.HasExited) {
                    Write-Host 'rexglue.exe is still running; close it in Task Manager.'
                }
                return 1
            }
            Start-Sleep -Milliseconds 200
        }

        # Command-line values win over the saved config and are not saved to it. The
        # advertised address makes Discord join codes carry the friends' address
        # instead of this PC's 127.0.0.1 relay address; the SDK accepts 35 characters.
        $gameArgs = @("--system_link_invite=$thisPcCode", '--system_link_lan_enabled=false')
        $advertised = "${external}:$Port"
        if ($advertised.Length -gt 35) {
            Write-Host "Discord Join won't work with this address. Friends should paste the copied code instead."
        } else {
            $gameArgs += "--system_link_advertised_address=$advertised"
        }
        Start-Process -FilePath $game -WorkingDirectory $PSScriptRoot -ArgumentList $gameArgs
        Write-Host 'Started ReRevved. Leave this window open while you play; press Ctrl+C when done.'

        Wait-Process -Id $relay.Id
        $relay.Refresh()
        if ($relay.ExitCode -ne 0) {
            Write-Host "ERROR: The relay stopped with exit code $($relay.ExitCode). See $log."
            return 1
        }
        Write-Host 'Relay stopped.'
        return 0
    } catch [System.Management.Automation.PipelineStoppedException] {
        throw
    } catch {
        Write-Host "ERROR: $($_.Exception.Message)"
        return 1
    } finally {
        if ($created) {
            try {
                $mappings.Remove($Port, 'UDP')
                Write-Host "Port mapping removed (UDP $Port)."
            } catch {
                Write-Host "Could not remove the UDP $Port port mapping. Remove it in your router."
            }
        }
    }
}

if ($LoadHelpersOnly) { return }
exit (Start-Hosting)

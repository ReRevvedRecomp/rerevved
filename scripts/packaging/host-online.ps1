# Hosts a private ReRevved online game: opens the UDP port with UPnP, makes the
# invite codes, starts the game on this PC's own online server, and runs it.
# Runs on Windows PowerShell 5.1.
param(
    [ValidateRange(1, 65535)][int]$Port = 30100,
    # Defines the helper functions and stops; the contract tests use it.
    [switch]$LoadHelpersOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$MappingName = 'ReRevved online server'

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

# Routes carry Metric (route metric plus interface metric) and Physical. The
# router is on a physical adapter, so one wins over a VPN adapter's lower metric.
function Select-DefaultRoute($Routes) {
    $Routes | Sort-Object -Property @{ Expression = { -not $_.Physical } }, Metric |
        Select-Object -First 1
}

function Get-LocalNetwork {
    $physical = @(Get-NetAdapter -Physical | ForEach-Object { $_.InterfaceIndex })
    $routes = @(foreach ($route in Get-NetRoute -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0') {
            $nic = Get-NetIPInterface -AddressFamily IPv4 -InterfaceIndex $route.InterfaceIndex
            [pscustomobject]@{
                InterfaceIndex = $route.InterfaceIndex
                Metric         = $route.RouteMetric + $nic.InterfaceMetric
                Physical       = $physical -contains $route.InterfaceIndex
            }
        })
    $best = Select-DefaultRoute $routes
    if (-not $best) { return $null }
    $address = Get-NetIPAddress -AddressFamily IPv4 -InterfaceIndex $best.InterfaceIndex |
        Where-Object { $_.IPAddress -notlike '169.254.*' } |
        Select-Object -First 1
    if (-not $address) { return $null }
    $vpn = $null
    $lowest = $routes | Sort-Object -Property Metric | Select-Object -First 1
    if ($lowest.InterfaceIndex -ne $best.InterfaceIndex) {
        $vpn = (Get-NetAdapter -InterfaceIndex $lowest.InterfaceIndex).Name
    }
    $category = $null
    $connection = Get-NetConnectionProfile -InterfaceIndex $best.InterfaceIndex -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($connection) { $category = "$($connection.NetworkCategory)" }
    $wireless = (Get-NetAdapter -InterfaceIndex $best.InterfaceIndex).PhysicalMediaType -like '*802.11*'
    [pscustomobject]@{
        Address = $address.IPAddress; Category = $category; VpnAdapter = $vpn; Wireless = $wireless
    }
}

# Windows can answer before it has found the router, so an empty answer is asked again.
function Get-RouterMappings {
    foreach ($attempt in 1..3) {
        $found = (New-Object -ComObject HNetCfg.NATUPnP).StaticPortMappingCollection
        if ($found) { return $found }
        if ($attempt -lt 3) { Start-Sleep -Seconds 2 }
    }
    return $null
}

function Select-FreePort([int]$Port, [int[]]$Taken) {
    foreach ($candidate in $Port..([math]::Min($Port + 9, 65535))) {
        if ($Taken -notcontains $candidate) { return $candidate }
    }
    return $null
}

# A typed port is the outside one, which can differ from the port the online
# server listens on.
function ConvertTo-FriendsAddress([string]$Text, [int]$Port) {
    if ($Text -notmatch '^([A-Za-z0-9.-]{1,253})(:(\d{1,5}))?$') { return $null }
    $name = $Matches[1]
    if ($Matches.ContainsKey(3)) {
        $Port = [int]$Matches[3]
        if ($Port -lt 1 -or $Port -gt 65535) { return $null }
    }
    return "${name}:$Port"
}

function Get-FailureText([string]$Cause, [string]$Category, [int]$Port) {
    switch ($Cause) {
        'NoNetwork' { return 'No Internet connection was found on this PC.' }
        'NoDiscovery' { return 'The Windows service "SSDP Discovery" is disabled, so this PC cannot find your router. Set it to Manual in the Services app, then run this again.' }
        'NoRouter' {
            if ($Category -eq 'Public') {
                return 'Windows treats this network as Public, which hides your router from this script. Set it to Private in the Settings page that just opened, then run this again.'
            }
            return 'Your router did not answer. UPnP may be turned off in the router, or this network does not offer it.'
        }
        'PortsTaken' { return "Other programs already use UDP ports $Port to $($Port + 9) on your router. Run this again with another port, for example: host-online.cmd -Port $($Port + 100)" }
        'SharedAddress' { return "Your router is behind another router or your Internet provider's shared address, so this PC cannot open a port." }
        default { return "Your router refused to open UDP port $Port." }
    }
}

# Windows adds a Block rule when the firewall prompt is dismissed or denied,
# and never asks again.
function Test-FirewallBlocked($Rules, [string]$Category) {
    $name = $Category -replace '^DomainAuthenticated$', 'Domain'
    foreach ($rule in $Rules) {
        if ("$($rule.Enabled)" -ne 'True' -or "$($rule.Direction)" -ne 'Inbound' -or
            "$($rule.Action)" -ne 'Block') { continue }
        $profiles = "$($rule.Profile)" -split ',\s*'
        if ($profiles -contains 'Any' -or $profiles -contains $name) { return $true }
    }
    return $false
}

function New-Invite([string]$Rexglue, [string]$Address, [string[]]$PasswordArgs) {
    $output = & $Rexglue online-invite --address $Address @PasswordArgs
    if ($LASTEXITCODE -ne 0) { throw "Could not create an invite code for $Address." }
    $code = @($output) | Where-Object { $_ -match '^RXR1-' } | Select-Object -First 1
    if (-not $code) { throw "No invite code was found for $Address." }
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

    $state = Join-Path $PSScriptRoot 'userdata\online-server'
    $null = New-Item -ItemType Directory -Force -Path $state
    $passwordFile = Join-Path $state 'server-password.txt'
    $invites = Join-Path $state 'invite-codes.txt'
    $passwordArgs = @('--password-file', $passwordFile)
    if (-not (Test-Path -LiteralPath $passwordFile)) { $passwordArgs += '--create-password' }

    $mappings = $null
    $created = $false   # True only for a mapping this run added or reclaimed.
    try {
        $external = $null
        $local = $null
        $category = $null
        $vpn = $null
        $wireless = $false
        $cause = 'NoNetwork'   # Names the step that failed; see Get-FailureText.
        try {
            $network = Get-LocalNetwork
            if ($network) {
                $local = $network.Address
                $category = $network.Category
                $vpn = $network.VpnAdapter
                $wireless = $network.Wireless
                $cause = 'NoRouter'
                Write-Host 'Looking for your router...'
                $mappings = Get-RouterMappings
            }
            if ($mappings) {
                $cause = 'Refused'
                # If listing fails, ownership is unknown: the catch below leaves
                # $created false and the host gets the manual prompt.
                $mapped = @(foreach ($item in $mappings) {
                        if ($item.Protocol -eq 'UDP') { $item }
                    })
                $taken = @($mapped | Where-Object { $_.Description -ne $MappingName } |
                        ForEach-Object { $_.ExternalPort })
                $free = Select-FreePort $Port $taken
                if (-not $free) {
                    $cause = 'PortsTaken'
                    throw 'No free port.'
                }
                if ($free -ne $Port) {
                    Write-Host "Another program uses UDP port $Port on your router; using $free instead."
                    $Port = $free
                }
                $mapping = $mapped | Where-Object { $_.ExternalPort -eq $Port } | Select-Object -First 1
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
        if ($external -and (Test-UnreachableAddress $external)) {
            $cause = 'SharedAddress'
            $external = $null
        }
        if (-not $external) {
            if ($cause -eq 'NoRouter') {
                $discovery = Get-Service -Name SSDPSRV -ErrorAction SilentlyContinue
                if ($discovery -and "$($discovery.StartType)" -eq 'Disabled') {
                    $cause = 'NoDiscovery'
                } elseif ($category -eq 'Public') {
                    # Changing the network type needs administrator rights, so the
                    # host does it on the page that holds the setting.
                    $page = 'ms-settings:network-ethernet'
                    if ($wireless) { $page = 'ms-settings:network-wifi' }
                    Start-Process $page
                }
            }
            Write-Host (Get-FailureText $cause $category $Port)
            Write-Host "This PC cannot host automatically. See `"CAN'T HOST?`" in ONLINE.txt."
            Write-Host "If friends must use a port other than $Port, add it after the address, like example.net:41234."
            $external = (Read-Host 'Address friends can reach (for example a Tailscale or ZeroTier address), or press Enter to quit').Trim()
            if (-not $external) { return 0 }
            $friendsAddress = ConvertTo-FriendsAddress $external $Port
            if (-not $friendsAddress) {
                Write-Host 'ERROR: Enter an IPv4 address or DNS name, with :port after it if needed.'
                return 1
            }
        } else {
            $friendsAddress = "${external}:$Port"
            Write-Host "Opened UDP port $Port on your router."
            if ($vpn) {
                Write-Host "A VPN ($vpn) carries this PC's Internet traffic. If friends cannot connect, turn it off and run this again."
            }
        }

        # Read before the server starts: its first start raises the firewall prompt,
        # which holds a Block rule until the host answers.
        $rules = @()
        try {
            $rules = @(Get-NetFirewallApplicationFilter -Program $rexglue | Get-NetFirewallRule)
        } catch {
            $rules = @()
        }
        if (Test-FirewallBlocked $rules $category) {
            Write-Host 'WARNING: Windows Firewall blocks rexglue.exe, so friends cannot connect.'
            Write-Host 'Open "Allow an app through Windows Firewall" from the Start menu, tick rexglue.exe for this network type, then run this again.'
        }

        $friendsCode = New-Invite $rexglue $friendsAddress $passwordArgs
        # The password file exists after the first call.
        $thisPcCode = New-Invite $rexglue "127.0.0.1:$Port" @('--password-file', $passwordFile)
        Set-Content -LiteralPath $invites -Encoding ASCII -Value @(
            "Friends' code:", $friendsCode, '', "This PC's code:", $thisPcCode)
        Set-Clipboard -Value $friendsCode
        Write-Host 'The friends'' code is copied. Paste it to your friends privately.'
        Write-Host "Both codes are in $invites. Keep them private."

        # The game gives up on the online server after 5 seconds, so the server goes first.
        # It shares this console, so Ctrl+C reaches it as well.
        $log = Join-Path $state 'online-server.log'
        # Console output goes to files so the window stays quiet; details are in the log.
        $server = Start-Process -FilePath $rexglue -NoNewWindow -PassThru `
            -RedirectStandardOutput (Join-Path $state 'online-server-console.txt') `
            -RedirectStandardError (Join-Path $state 'online-server-console-err.txt') `
            -ArgumentList @(
            '--log-file', "`"$log`"", 'online-server', '--password-file', "`"$passwordFile`"",
            '--port', $Port, '--max-members', 8)
        $null = $server.Handle   # Keeps the exit code readable.
        $deadline = (Get-Date).AddSeconds(10)
        while (-not (Get-NetUDPEndpoint -LocalPort $Port -OwningProcess $server.Id -ErrorAction SilentlyContinue)) {
            if ($server.HasExited -or (Get-Date) -gt $deadline) {
                Write-Host "ERROR: The online server did not start listening on UDP port $Port. See $log."
                if (-not $server.HasExited) {
                    Write-Host 'rexglue.exe is still running; close it in Task Manager.'
                }
                return 1
            }
            Start-Sleep -Milliseconds 200
        }

        # Command-line values win over the saved config and are not saved to it. The
        # advertised address makes Discord join codes carry the friends' address
        # instead of this PC's 127.0.0.1 server address; the SDK accepts 35 characters.
        $gameArgs = @("--system_link_invite=$thisPcCode", '--system_link_lan_enabled=false')
        $advertised = $friendsAddress
        if ($advertised.Length -gt 35) {
            Write-Host "Discord Join won't work with this address. Friends should paste the copied code instead."
        } else {
            $gameArgs += "--system_link_advertised_address=$advertised"
        }
        Start-Process -FilePath $game -WorkingDirectory $PSScriptRoot -ArgumentList $gameArgs
        Write-Host 'Started ReRevved. Leave this window open while you play; press Ctrl+C when done.'

        Wait-Process -Id $server.Id
        $server.Refresh()
        if ($server.ExitCode -ne 0) {
            Write-Host "ERROR: The online server stopped with exit code $($server.ExitCode). See $log."
            return 1
        }
        Write-Host 'The online server stopped.'
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

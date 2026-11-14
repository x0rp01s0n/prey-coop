param(
    [Parameter(Mandatory=$true)][string]$ProfilesRoot,
    [Parameter(Mandatory=$true)][string]$GameRoot,
    [Parameter(Mandatory=$true)][string]$ManifestPath,
    [Parameter(Mandatory=$true)][string]$ResultRoot,
    [string[]]$DoorNames = @('Door.Door_Sliding_Double_Large7', 'LaserLabDoor'),
    [ValidateSet('client','host','both_hold','client_reentry','host_reentry','client_after_unrelated_event','empty_remote_open','occupied_close','script_open_client','script_open_host','script_open_held','boundary_client','boundary_host','both_handover','client_disconnect','incoming_close_client','incoming_close_host','incoming_proximity_close_client','incoming_proximity_close_host','floor_root_client','floor_root_host','expired_timer_exit','burst_crossings')]
    [string[]]$Cases = @('client','host','both_hold','client_reentry','host_reentry','client_after_unrelated_event','empty_remote_open','occupied_close'),
    [ValidateRange(1,100)][int]$Cycles = 3,
    [int]$Seed = 247,
    [ValidateRange(-1,10000)][int]$ReentryDelayMilliseconds = -1,
    [ValidateRange(1,65535)][int]$JoinPort = 27015,
    [string]$JoinAddress = '127.0.0.1',
    [ValidateRange(0,10000)][int]$ExpectedPoseDelayMilliseconds = 0,
    [string]$NetworkCondition = 'direct',
    [string]$ExpectedDllHash,
    [switch]$AllowHostMenu,
    [switch]$TraceDecisions,
    [switch]$PreflightOnly
)
$ErrorActionPreference = 'Stop'
$inv = [Globalization.CultureInfo]::InvariantCulture
$random = New-Object Random $Seed
$run = Join-Path $ResultRoot ('door-stress-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $run | Out-Null
$commandTimes = New-Object 'Collections.Generic.List[double]'
$rows = New-Object 'Collections.Generic.List[object]'
$runnerCompleted = $false
$runnerError = ''
$sequence = 0
$mutex = New-Object Threading.Mutex($false, 'Local\PreyCoopDoorStress')
if (!$mutex.WaitOne(0)) { throw 'Another door stress runner is active' }

function Invoke-Control($role, $command, $expected, [bool]$expectRejected=$false, [bool]$allowEitherResult=$false) {
    $script:sequence++
    $dir = Join-Path (Join-Path $ProfilesRoot $role) 'CoopRuntimeExtractor'
    $statusPath = Join-Path $dir 'status.txt'
    $readClock = [Diagnostics.Stopwatch]::StartNew()
    do {
        $oldRaw = [string](Get-Content $statusPath -Raw -ErrorAction SilentlyContinue)
        if ([string]::IsNullOrWhiteSpace($oldRaw)) { Start-Sleep -Milliseconds 100; continue }
        $oldBatch = [regex]::Match($oldRaw, '(?m)^command=(?:OK|FAILED) ([0-9]+) command\(s\)\r?$')
        if ($oldBatch.Success) { break }
        Start-Sleep -Milliseconds 100
    } while ($readClock.Elapsed.TotalSeconds -lt 15)
    if (!$oldBatch.Success) { throw "$role status file has no complete batch response" }
    $commandCount = @($command -split '\r?\n' | Where-Object { $_.Trim() -and !$_.Trim().StartsWith('#') }).Count
    $prefixCount = if ([int]$oldBatch.Groups[1].Value -eq ($commandCount + 1)) { 2 } else { 1 }
    $batchSize = $prefixCount + $commandCount
    $before = (Get-Item $statusPath).LastWriteTimeUtc.Ticks
    $clock = [Diagnostics.Stopwatch]::StartNew()
    # The batch count is published only after all handlers return; alternate it
    # so an intermediate status write cannot acknowledge this batch.
    $payload = (('coop_status' + "`r`n") * $prefixCount) + $command + "`r`n# " + [guid]::NewGuid()
    $temporary = Join-Path $dir ('door-command-' + [guid]::NewGuid() + '.tmp')
    [IO.File]::WriteAllText($temporary, $payload, [Text.Encoding]::ASCII)
    # Windows PowerShell coerces a null backup path to an invalid empty path.
    $backup = $temporary + '.bak'
    [IO.File]::Replace($temporary, (Join-Path $dir 'command.txt'), $backup)
    [IO.File]::Delete($backup)
    $raw = ''
    do {
        Start-Sleep -Milliseconds 100
        $raw = Get-Content $statusPath -Raw -ErrorAction SilentlyContinue
        if ([string]::IsNullOrWhiteSpace($raw)) { continue }
        $completed = [regex]::Match([string]$raw, '(?m)^command=(OK|FAILED) ' + $batchSize + ' command\(s\)\r?$')
        $response = [regex]::Match([string]$raw, 'cmd=([^\s]+) ok=([01])')
        $actionMatches = $response.Success -and ($response.Groups[1].Value -eq $expected -or
            ($expectRejected -and $expected.EndsWith('_failed') -and
                $response.Groups[1].Value -eq $expected.Substring(0,$expected.Length-7)) -or
            ($allowEitherResult -and $response.Groups[1].Value -eq ($expected + '_failed')))
        if ($completed.Success -and $actionMatches -and
            $raw -match '(?m)^export=[^\r\n]*\r?\n\z' -and
            (Get-Item $statusPath).LastWriteTimeUtc.Ticks -gt $before) {
            $action = $response.Groups[1].Value; $ok = $response.Groups[2].Value
            $file = '{0:D6}-{1}-{2}.txt' -f $script:sequence,$role,$action
            [IO.File]::WriteAllText((Join-Path $run $file), $raw)
            $script:commandTimes.Add($clock.Elapsed.TotalMilliseconds)
            if ($allowEitherResult) { return $raw }
            if (!$expectRejected -and ($ok -ne '1' -or $completed.Groups[1].Value -ne 'OK')) { throw "$role rejected '$command': $action (see $file)" }
            if ($expectRejected -and ($ok -ne '0' -or $completed.Groups[1].Value -ne 'FAILED')) { throw "Unexpected command acceptance: $role accepted '$command' (see $file)" }
            if ($action -ne $expected) { throw "$role returned unexpected action '$action' for '$command'" }
            return $raw
        }
    } while ($clock.Elapsed.TotalSeconds -lt 15)
    [IO.File]::WriteAllText((Join-Path $run ('timeout-' + $role + '.txt')), $raw)
    throw "$role control timeout: $command"
}

function Token($raw, $name) {
    $match = [regex]::Match($raw, '(?:^|\s)' + [regex]::Escape($name) + '=([^\s]+)')
    if (!$match.Success) { throw "Missing status token $name" }
    return $match.Groups[1].Value
}

function Door-Field($network, $key) {
    $match = [regex]::Match($network, '_' + $key + '_(-?[0-9]+(?:\.[0-9]+)?)(?:_|$)')
    if (!$match.Success) { throw "Missing native door field $key" }
    return [double]::Parse($match.Groups[1].Value, $inv)
}

function Assert-Pair {
    $epochs = @()
    foreach ($role in @('host','client')) {
        $s = Invoke-Control $role 'coop_status' 'status'
        foreach ($entry in @(@('gameReady','1'), @('sessionReady','1'), @('session','connected_same_level'),
            @('localLevel','campaign/research/prototype'), @('remoteLevel','campaign/research/prototype'),
            @('loadGuard','0'))) {
            if ((Token $s $entry[0]) -ne $entry[1]) { throw "$role invalid fixture: $($entry[0])=$(Token $s $entry[0])" }
        }
        $interactive = Token $s 'localInteractive'
        if ($interactive -ne '1/interactive' -and !($AllowHostMenu -and $role -eq 'host' -and $interactive -eq '0/input_mode_3')) {
            throw "$role invalid fixture: localInteractive=$interactive"
        }
        $epochs += Token $s 'localEpoch'
        if ((Token $s 'localEpoch') -ne (Token $s 'remoteEpoch')) { throw "$role world epoch mismatch" }
    }
    if ($epochs[0] -ne $epochs[1]) { throw 'Pair world epochs differ' }
}

function Read-Pair($door, $phase, $roles = @('host','client'), [bool]$allowDisconnected=$false) {
    $pair = @{}
    foreach ($role in $roles) {
        $raw = Invoke-Control $role ("coop_area_door_probe guid:" + $door.guid) 'area_door_probe'
        if ((Token $raw 'gameReady') -ne '1' -or (Token $raw 'loadGuard') -ne '0' -or
            (!$allowDisconnected -and (Token $raw 'sessionReady') -ne '1')) { throw "$role lost gameplay session during $phase" }
        $network = Token $raw 'network'
        $sample = [ordered]@{utc=[DateTime]::UtcNow.ToString('o'); role=$role; door=$door.name; guid=$door.guid; phase=$phase}
        foreach ($field in @('open','locked','powered','area_triggered','area_entities','range_controlled','range_occupied','range_depth','explicit_open','remote_revision','remote_open','remote_accepted','update_marked','animating','close_timer','close_duration','collision_probe','center_blocked')) {
            $sample[$field] = Door-Field $network $field
        }
        $sample.areaObjectEvents = Token $raw 'areaObjectEvents'
        $sample.areaObjectLast = Token $raw 'areaObjectLast'
        $sample.localLocation = Token $raw 'localLocation'
        $sample.remoteLocation = Token $raw 'remoteLocation'
        $sample.poseSequence = Token $raw 'poseSequence'
        $pair[$role] = [pscustomobject]$sample
        $pair[$role] | Export-Csv (Join-Path $run 'samples.csv') -Append -NoTypeInformation
    }
    return $pair
}

function Matches-State($pair, $open) {
    foreach ($role in @('host','client')) {
        $p = $pair[$role]
        if ($p.open -ne $open -or $p.collision_probe -ne 1 -or $p.center_blocked -ne (1-$open)) { return $false }
    }
    return $true
}

function Wait-State($door, $open, $seconds, $phase) {
    $clock = [Diagnostics.Stopwatch]::StartNew()
    do {
        $pair = Read-Pair $door $phase
        if (Matches-State $pair $open) { return $pair }
        Start-Sleep -Milliseconds 200
    } while ($clock.Elapsed.TotalSeconds -lt $seconds)
    throw "Native state/collision did not converge: door=$($door.name) phase=$phase expectedOpen=$open host=$($pair.host.open)/$($pair.host.center_blocked) client=$($pair.client.open)/$($pair.client.center_blocked)"
}

function Place($role, $door, $distance) {
    if ($distance -eq 0) { $distance = -$door.clearDistance }
    $x = ($door.x + $door.nx*$distance).ToString('F3',$inv)
    $y = ($door.y + $door.ny*$distance).ToString('F3',$inv)
    $z = ($door.z + 0.05).ToString('F3',$inv)
    $xyz = "$x $y $z"
    $null = Invoke-Control $role "coop_player_place_aim $xyz guid:$($door.guid) 1.0" 'player_place_aim'
}

function Clear-Players($door) {
    foreach ($role in @('host','client')) { Place $role $door 0 }
}

function Rejoin-Client {
    $null = Invoke-Control client "coop_start_client $JoinAddress $JoinPort" start_client
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $continued = $false
    do {
        $s = Invoke-Control client 'coop_status' status
        if ((Token $s 'sessionReady') -eq '1' -and
            (Token $s 'session') -eq 'connected_same_level' -and
            (Token $s 'loadGuard') -eq '0' -and
            (Token $s 'localInteractive') -eq '1/interactive') {
            Assert-Pair
            $null = Invoke-Control client 'coop_player_health 3000 3000' player_health
            return
        }
        if (!$continued -and (Token $s 'waitContinue') -eq '1') {
            $null = Invoke-Control client 'coop_autoskip_loading_screen' continue_after_load_queued
            $null = Invoke-Control client 'coop_input_tap enter' input_tap
            $continued = $true
        }
        Start-Sleep -Seconds 1
    } while ($clock.Elapsed.TotalSeconds -lt 180)
    throw 'Client did not return to interactive same-world gameplay after rejoin'
}

function Capture-Failure($label, $message) {
    $failure = Join-Path $run ('failure-' + $label)
    New-Item -ItemType Directory -Force $failure | Out-Null
    [IO.File]::WriteAllText((Join-Path $failure 'error.txt'), $message)
    $Error[0] | Format-List * -Force | Out-File (Join-Path $failure 'error-record.txt')
    foreach ($role in @('host','client')) {
        $profile = Join-Path $ProfilesRoot $role
        foreach ($relative in @('CoopRuntimeExtractor\status.txt','coop_runtime_log.txt')) {
            $path = Join-Path $profile $relative
            if (Test-Path $path) { Copy-Item $path (Join-Path $failure ($role + '-' + [IO.Path]::GetFileName($path))) }
        }
    }
}

function Save-DoorTrace($door, $label, $role) {
    $null = Invoke-Control $role "coop_area_door_trace guid:$($door.guid) dump" area_door_trace
    $trace = Join-Path (Join-Path (Join-Path $ProfilesRoot $role) 'CoopRuntimeExtractor') 'door_decisions.log'
    Copy-Item $trace (Join-Path $run ("trace-$label-$role.log"))
    $firstTrace = Get-Content $trace -TotalCount 1
    if (!$firstTrace -or $firstTrace -notmatch '^seq=1 ' -or $firstTrace -notmatch ' reason=trace_start ') {
        throw 'Decision trace is empty or its bounded ring lost the start of this case'
    }
    return @(Get-Content $trace | Where-Object {
        $_ -match 'reason=OnOpen_enter remoteApply=0 commandDepth=0 nativeUpdateScope=1 occupied=0 ' -and $_ -match ' open=0 '
    }).Count
}

try {
    $builds = @()
    foreach ($role in @('host','client')) {
        $exe = Join-Path $GameRoot "$role\Binaries\Danielle\x64\Release\Prey.exe"
        $processes = @(Get-CimInstance Win32_Process | Where-Object ExecutablePath -eq $exe)
        if ($processes.Count -ne 1) { throw "Expected exactly one isolated $role process" }
        $hash = (Get-FileHash (Join-Path $GameRoot "$role\Mods\CoopPrototype\CoopPrototype.dll") -Algorithm SHA256).Hash
        if ($ExpectedDllHash -and $hash -ne $ExpectedDllHash) { throw "$role DLL hash mismatch" }
        $builds += [pscustomobject]@{role=$role;pid=$processes[0].ProcessId;dllSha256=$hash}
    }
    if ($builds[0].dllSha256 -ne $builds[1].dllSha256) { throw 'Peer DLLs differ' }
    Copy-Item $PSCommandPath (Join-Path $run 'runner.ps1')
    [pscustomobject]@{seed=$Seed;cycles=$Cycles;cases=$Cases;traceDecisions=[bool]$TraceDecisions;reentryDelayMilliseconds=$ReentryDelayMilliseconds;joinAddress=$JoinAddress;joinPort=$JoinPort;expectedPoseDelayMilliseconds=$ExpectedPoseDelayMilliseconds;networkCondition=$NetworkCondition;allowHostMenu=[bool]$AllowHostMenu;doors=$DoorNames;builds=$builds;runnerSha256=(Get-FileHash $PSCommandPath -Algorithm SHA256).Hash;manifestSha256=(Get-FileHash $ManifestPath -Algorithm SHA256).Hash} |
        ConvertTo-Json -Depth 5 | Set-Content (Join-Path $run 'run.json') -Encoding UTF8
    Assert-Pair
    if ($PreflightOnly) { $runnerCompleted = $true; Write-Output "PREFLIGHT PASS $run"; return }
    foreach ($role in @('host','client')) { $null = Invoke-Control $role 'coop_player_health 3000 3000' 'player_health' }
    [xml]$manifest = Get-Content $ManifestPath -Raw
    $doors = @()
    foreach ($name in $DoorNames) {
        $e = @($manifest.Entities.Entity | Where-Object Name -eq $name)
        if ($e.Count -ne 1) { throw "Authored door not unique: $name" }
        $p = $e[0].Pos.Split(',') | ForEach-Object { [double]::Parse($_,$inv) }
        $q = $e[0].Rotate.Split(',') | ForEach-Object { [double]::Parse($_,$inv) }
        $doors += [pscustomobject]@{name=$name;guid=[Convert]::ToUInt64($e[0].EntityGuid,16).ToString();x=$p[0];y=$p[1];z=$p[2];nx=2*($q[1]*$q[2]-$q[0]*$q[3]);ny=1-2*($q[1]*$q[1]+$q[3]*$q[3])}
    }
    if ($doors.Count -lt 2 -and $Cases -contains 'client_after_unrelated_event') { throw 'Need a second authored door for the unrelated-event race' }
    foreach ($door in $doors) {
        Assert-Pair
        $initial = Read-Pair $door 'initial'
        # Keep the closed door visible: native offscreen skeletal animation can
        # defer its collision pose even after m_bOpened has become false.
        $door | Add-Member -NotePropertyName clearDistance -NotePropertyValue ($initial.host.range_depth/2 + 0.75) -Force
        Clear-Players $door
        foreach ($role in @('host','client')) {
            if ($initial[$role].powered -ne 1 -or $initial[$role].area_triggered -ne 1 -or $initial[$role].range_controlled -ne 1) { throw "$($door.name) is not a range-controlled powered automatic door" }
        }
        if ($initial.host.locked -ne $initial.client.locked) { throw 'Initial lock disagreement; do not mask invalid fixture with setup mutation' }
        $originalLock = $initial.host.locked
        $closeBudget = [Math]::Max($initial.host.close_duration,$initial.client.close_duration) + 4
        try {
            if ($originalLock) { $null = Invoke-Control host "coop_area_door_set guid:$($door.guid) locked 0" area_door_set }
            for ($cycle=1; $cycle -le $Cycles; $cycle++) {
                foreach ($case in $Cases) {
                    $label = "$($door.guid)-$cycle-$case"
                    $row = [ordered]@{door=$door.name;guid=$door.guid;cycle=$cycle;case=$case;result='FAIL';error='';seconds=0}
                    $caseClock = [Diagnostics.Stopwatch]::StartNew()
                    $noiseDoor = $null; $noiseLock = 0
                    $baselineReady = $false
                    try {
                        Assert-Pair
                        if ($TraceDecisions) {
                            foreach ($role in @('host','client')) {
                                $null = Invoke-Control $role "coop_area_door_trace guid:$($door.guid) start" area_door_trace
                            }
                        }
                        Clear-Players $door
                        $null = Wait-State $door 0 $closeBudget "$label-baseline"
                        $baselineReady = $true
                        Start-Sleep -Milliseconds ($random.Next(0,251))
                        $origin = if ($case -like 'host*' -or $case -like '*_host') { 'host' } else { 'client' }
                        if ($case -like 'floor_root_*') {
                            # Deliberately reproduce a root 7 mm below the box
                            # floor before physics gets another frame to settle.
                            Place $origin $door -0.9
                            $null = Wait-State $door 1 4 "$label-open"
                            $x = ($door.x - $door.nx*0.9).ToString('F3',$inv)
                            $y = ($door.y - $door.ny*0.9).ToString('F3',$inv)
                            $z = ($door.z - 0.007).ToString('F3',$inv)
                            $batch = "coop_player_place_aim $x $y $z guid:$($door.guid) 1.0`r`ncoop_area_door_set guid:$($door.guid) open 0"
                            $result = Invoke-Control $origin $batch area_door_set $false $true
                            if ($result -match 'cmd=area_door_set ok=1') {
                                throw 'Door closed/blocked: below-floor root allowed native Close'
                            }
                            $held = Read-Pair $door "$label-after-floor-root-close"
                            if (!(Matches-State $held 1)) { throw 'Door closed/blocked after below-floor root despite player body in trigger' }
                        } elseif ($case -eq 'empty_remote_open') {
                            $beforeRemote = (Read-Pair $door "$label-before-replay").host.remote_revision
                            $null = Invoke-Control client "coop_area_door_set guid:$($door.guid) proximity_open 1" area_door_set
                            Start-Sleep -Milliseconds 500
                            $empty = Read-Pair $door "$label-empty-replay"
                            if ($empty.host.open -ne 0) { throw 'Door reopened from network without any occupant' }
                            if ($empty.client.open -ne 1 -or $empty.host.range_occupied -ne 0 -or $empty.client.range_occupied -ne 0) {
                                throw 'Empty-open fixture did not preserve local native Open with no occupant'
                            }
                            if ($empty.host.remote_open -ne 1 -or $empty.host.remote_accepted -ne 0 -or $empty.host.remote_revision -le $beforeRemote) {
                                throw 'Missing evidence that the empty network edge was received and rejected'
                            }
                            $null = Wait-State $door 0 $closeBudget "$label-native-closed"
                        } elseif ($case -eq 'script_open_held') {
                            Place client $door -0.9
                            $null = Wait-State $door 1 4 "$label-spatial-open"
                            $null = Invoke-Control host "coop_area_door_set guid:$($door.guid) open 1" area_door_set
                            Start-Sleep -Milliseconds 300
                            Clear-Players $door
                            for ($sample=0; $sample -lt 3; $sample++) {
                                $held = Read-Pair $door "$label-explicit-after-leave-$sample"
                                if (!(Matches-State $held 1) -or $held.host.explicit_open -ne 1 -or $held.client.explicit_open -ne 1) {
                                    throw 'Door open/collision or explicit cause lost after button opened an already-open door'
                                }
                            }
                            $null = Wait-State $door 0 $closeBudget "$label-native-closed"
                        } elseif ($case -like 'script_open_*') {
                            $null = Invoke-Control $origin "coop_area_door_set guid:$($door.guid) open 1" area_door_set
                            $opened = Wait-State $door 1 4 "$label-explicit-open"
                            foreach ($role in @('host','client')) {
                                if ($opened[$role].range_occupied -ne 0) { throw 'Script-open fixture unexpectedly has an occupant' }
                                if ($opened[$role].explicit_open -ne 1) { throw 'Door open/collision converged without preserving explicit opening cause' }
                            }
                            $null = Wait-State $door 0 $closeBudget "$label-native-closed"
                        } elseif ($case -like 'boundary_*') {
                            foreach ($side in @(-1,1)) {
                                Place $origin $door ($side * ($initial.host.range_depth/2 + 0.15))
                                $null = Wait-State $door 0 $closeBudget "$label-outside-$side"
                                $outside = Read-Pair $door "$label-outside-occupancy-$side"
                                foreach ($role in @('host','client')) {
                                    if ($outside[$role].range_occupied -ne 0) { throw 'Door boundary outside point counted as occupied' }
                                }
                                Place $origin $door ($side * ($initial.host.range_depth/2 - 0.05))
                                $inside = Wait-State $door 1 4 "$label-inside-$side"
                                foreach ($role in @('host','client')) {
                                    if ($inside[$role].range_occupied -ne 1) { throw 'Door boundary inside point lost occupancy' }
                                }
                                Clear-Players $door
                                $null = Wait-State $door 0 $closeBudget "$label-boundary-cleared-$side"
                            }
                        } elseif ($case -eq 'burst_crossings') {
                            # Do not wait for convergence between reversals: newer native
                            # events must supersede in-flight edges without a custom tick.
                            for ($crossing=0; $crossing -lt 12; $crossing++) {
                                $mover = if ($crossing%2 -eq 0) { 'client' } else { 'host' }
                                Place $mover $door -0.9
                                Place $mover $door 0
                            }
                            Place client $door -0.9
                            $null = Wait-State $door 1 4 "$label-final-entry"
                            for ($sample=0; $sample -lt 5; $sample++) {
                                $held = Read-Pair $door "$label-final-held-$sample"
                                if (!(Matches-State $held 1)) { throw 'Door closed/blocked after burst reversals while client remained' }
                            }
                            Clear-Players $door
                            $null = Wait-State $door 0 $closeBudget "$label-final-empty"
                        } elseif ($case -eq 'client_disconnect') {
                            Place client $door -0.9
                            $null = Wait-State $door 1 4 "$label-open"
                            if ($TraceDecisions) {
                                if ((Save-DoorTrace $door "$label-pre-disconnect" client) -gt 0) { throw 'Door reopened without occupancy before disconnect' }
                                $null = Invoke-Control client "coop_area_door_trace guid:$($door.guid) stop" area_door_trace
                            }
                            $null = Invoke-Control client "coop_stop_network`r`ncoop_status" status
                            $clock = [Diagnostics.Stopwatch]::StartNew()
                            do {
                                $s = Invoke-Control client 'coop_status' status
                                if ((Token $s 'mode') -eq 'off') { break }
                                Start-Sleep -Milliseconds 250
                            } while ($clock.Elapsed.TotalSeconds -lt 30)
                            if ((Token $s 'mode') -ne 'off') { throw 'Client disconnect did not complete' }
                            $clock.Restart()
                            do {
                                $alone = (Read-Pair $door "$label-peer-left" @('host') $true).host
                                if ($alone.open -eq 0 -and $alone.range_occupied -eq 0 -and
                                    $alone.collision_probe -eq 1 -and $alone.center_blocked -eq 1) { break }
                                Start-Sleep -Milliseconds 250
                            } while ($clock.Elapsed.TotalSeconds -lt ($closeBudget+10))
                            if ($alone.open -ne 0 -or $alone.range_occupied -ne 0 -or $alone.center_blocked -ne 1) {
                                throw 'Door remained open/occupied after its only player disconnected'
                            }
                            Rejoin-Client
                            if ($TraceDecisions) {
                                $null = Invoke-Control client "coop_area_door_trace guid:$($door.guid) start" area_door_trace
                            }
                            Clear-Players $door
                            $null = Wait-State $door 0 $closeBudget "$label-after-rejoin"
                            Place client $door -0.9
                            $null = Wait-State $door 1 4 "$label-rejoin-reentered"
                            Clear-Players $door
                            $null = Wait-State $door 0 $closeBudget "$label-rejoin-cleared"
                        } elseif ($case -like 'incoming*close_*') {
                            if ($ExpectedPoseDelayMilliseconds -lt 1500) { throw 'Incoming-close race requires externally verified pose delay >=1500 ms' }
                            $sender = if ($origin -eq 'client') { 'host' } else { 'client' }
                            if ($case -like 'incoming_proximity_*') {
                                Place $origin $door -0.9
                                $opened = Wait-State $door 1 4 "$label-proximity-baseline"
                                if ($opened.host.explicit_open -ne 0 -or $opened.client.explicit_open -ne 0 -or $opened[$sender].range_occupied -ne 0) {
                                    throw 'Proximity-close race not established before delayed pose; no Close issued'
                                }
                            } else {
                                $null = Invoke-Control $sender "coop_area_door_set guid:$($door.guid) open 1" area_door_set
                                $null = Wait-State $door 1 4 "$label-explicit-baseline"
                            }
                            $beforeRemote = (Read-Pair $door "$label-before-close" @($origin))[$origin].remote_revision
                            if ($case -notlike 'incoming_proximity_*') { Place $origin $door -0.9 }
                            $null = Invoke-Control $sender "coop_area_door_set guid:$($door.guid) open 0" area_door_set
                            # The occupant knows it is near, but its delayed pose has not
                            # reached the sender. This creates a real incoming stale Close.
                            $sawRejectedEdge = $false
                            for ($sample=0; $sample -lt 5; $sample++) {
                                $p = (Read-Pair $door "$label-close-race-$sample" @($origin))[$origin]
                                $sawRejectedEdge = $sawRejectedEdge -or ($p.remote_open -eq 0 -and
                                    $p.remote_accepted -eq 0 -and $p.remote_revision -gt $beforeRemote)
                                if ($p.open -ne 1 -or $p.range_occupied -ne 1 -or $p.center_blocked -ne 0) {
                                    throw 'Door incoming Close overrode the current local occupant or its collision state'
                                }
                            }
                            if (!$sawRejectedEdge) { throw 'Missing received-and-rejected network Close evidence; race not established' }
                            $null = Wait-State $door 1 8 "$label-pose-caught-up"
                            Clear-Players $door
                            $null = Wait-State $door 0 ($closeBudget+8) "$label-empty-again"
                        } else {
                        if ($case -eq 'client_after_unrelated_event') {
                            $noiseDoor = $doors | Where-Object guid -ne $door.guid | Select-Object -First 1
                            $noise = Read-Pair $noiseDoor "$label-noise-before"
                            $noiseLock = [int]$noise.host.locked
                            $null = Invoke-Control host "coop_area_door_set guid:$($noiseDoor.guid) locked $(1-$noiseLock)" area_door_set
                        }
                        Place $origin $door -0.9
                        if ($case -eq 'both_hold' -or $case -eq 'both_handover') { Place host $door 0.9 }
                        $null = Wait-State $door 1 4 "$label-open"
                        if ($noiseDoor) {
                            $null = Invoke-Control host "coop_area_door_set guid:$($noiseDoor.guid) locked $noiseLock" area_door_set
                            $noiseDoor = $null
                        }
                        if ($case -eq 'expired_timer_exit') {
                            Start-Sleep -Seconds ([Math]::Ceiling($closeBudget-4)+1)
                        } elseif ($case -eq 'both_handover') {
                            for ($handoff=0; $handoff -lt 6; $handoff++) {
                                $leaver = if ($handoff%2 -eq 0) { 'client' } else { 'host' }
                                $holder = if ($leaver -eq 'client') { 'host' } else { 'client' }
                                Place $leaver $door 0
                                $null = Invoke-Control $leaver "coop_area_door_set guid:$($door.guid) open 0" area_door_set_failed $true
                                $held = Read-Pair $door "$label-handoff-$handoff"
                                if (!(Matches-State $held 1) -or $held.host.range_occupied -ne 1 -or $held.client.range_occupied -ne 1) {
                                    # Retain the failure even if delayed poses later converge.
                                    for ($after=0; $after -lt 6; $after++) {
                                        Start-Sleep -Milliseconds 150
                                        $null = Read-Pair $door "$label-handoff-$handoff-after-failure-$after"
                                    }
                                    throw 'Door closed/blocked or lost occupancy during player handover'
                                }
                                Place $leaver $door -0.9
                                $null = Wait-State $door 1 4 "$label-handoff-return-$handoff"
                            }
                        } elseif ($case -eq 'occupied_close') {
                            $null = Invoke-Control host "coop_area_door_set guid:$($door.guid) open 0" area_door_set_failed $true
                            Start-Sleep -Milliseconds 500
                            $held = Read-Pair $door "$label-close-while-client-near"
                            if (!(Matches-State $held 1)) { throw 'Door closed/blocked while client remained in trigger' }
                        } elseif ($case -eq 'both_hold') {
                            Place client $door 0
                            Start-Sleep -Seconds ([Math]::Ceiling($closeBudget-4)+1)
                            $held = Read-Pair $door "$label-host-still-near"
                            if (!(Matches-State $held 1)) { throw 'Door closed/blocked while host remained in trigger' }
                        } elseif ($case -like '*_reentry') {
                            Place $origin $door 0
                            if ($ReentryDelayMilliseconds -ge 0) {
                                $closeClock = [Diagnostics.Stopwatch]::StartNew()
                                do {
                                    $closing = (Read-Pair $door "$label-await-native-close" @($origin))[$origin]
                                    if ($closing.open -eq 0) { break }
                                    Start-Sleep -Milliseconds 100
                                } while ($closeClock.Elapsed.TotalSeconds -lt $closeBudget)
                                if ($closing.open -ne 0) { throw 'Native state/collision did not begin closing before rapid re-entry' }
                            }
                            $reentryDelay = if ($ReentryDelayMilliseconds -ge 0) { $ReentryDelayMilliseconds } else { [int]([Math]::Max(0.1,$closeBudget-4.5)*1000) }
                            Start-Sleep -Milliseconds $reentryDelay
                            Place $origin $door -0.9
                            $null = Wait-State $door 1 4 "$label-reentered"
                        } else {
                            Start-Sleep -Milliseconds 500
                            $held = Read-Pair $door "$label-held"
                            if (!(Matches-State $held 1)) { throw 'Door open/collision state did not remain stable' }
                        }
                        Clear-Players $door
                        $null = Wait-State $door 0 $closeBudget "$label-closed"
                        Start-Sleep -Milliseconds 500
                        $closed = Read-Pair $door "$label-settled"
                        if (!(Matches-State $closed 0)) { throw 'Door reopened or collider changed after clearing players' }
                        }
                        $row.result = 'PASS'
                    } catch {
                        $row.error = $_.ToString()
                        if ($row.error -notmatch 'Native state/collision|Door closed/blocked|Door open/collision|Door reopened|Door boundary|Door incoming|Door remained|Unexpected command acceptance') { $row.result = 'TEST_ERROR' }
                        elseif (!$baselineReady) { $row.result = 'BLOCKED_BY_BASELINE' }
                        Capture-Failure $label $row.error
                    } finally {
                        if ($TraceDecisions) {
                            foreach ($role in @('host','client')) {
                                try {
                                    $emptyReopens = Save-DoorTrace $door $label $role
                                    if ($emptyReopens -gt 0 -and $row.result -eq 'PASS') {
                                        $row.result = 'FAIL'
                                        $row.error = "Door native proximity reopen with zero current occupancy ($role, $emptyReopens trace event(s))"
                                    }
                                    $null = Invoke-Control $role "coop_area_door_trace guid:$($door.guid) stop" area_door_trace
                                } catch {
                                    Add-Content (Join-Path $run 'trace-errors.txt') "${label}/${role}: $_"
                                    if ($row.result -eq 'PASS') {
                                        $row.result = 'TEST_ERROR'
                                        $row.error = "Missing decision-trace evidence ($role): $_"
                                    }
                                }
                            }
                        }
                        if ($noiseDoor) { $null = Invoke-Control host "coop_area_door_set guid:$($noiseDoor.guid) locked $noiseLock" area_door_set }
                        Clear-Players $door
                    }
                    $row.seconds = [Math]::Round($caseClock.Elapsed.TotalSeconds,3)
                    $rows.Add([pscustomobject]$row)
                    [pscustomobject]$row | Export-Csv (Join-Path $run 'results.csv') -Append -NoTypeInformation
                    [pscustomobject]$row | ConvertTo-Json -Compress
                }
            }
        } finally {
            Clear-Players $door
            if ($originalLock) { $null = Invoke-Control host "coop_area_door_set guid:$($door.guid) locked $originalLock" area_door_set }
        }
    }
    $runnerCompleted = $true
} catch {
    $runnerError = $_.ToString()
    Capture-Failure 'runner' $_.ToString()
    throw
} finally {
    if ($commandTimes.Count) {
        $sorted = @($commandTimes | Sort-Object)
        [pscustomobject]@{runnerCompleted=$runnerCompleted;runnerError=$runnerError;commands=$sorted.Count;medianMs=$sorted[[int]($sorted.Count/2)];maxMs=$sorted[-1];cases=$rows.Count;passed=@($rows | Where-Object result -eq PASS).Count;failed=@($rows | Where-Object result -eq FAIL).Count;blocked=@($rows | Where-Object result -eq BLOCKED_BY_BASELINE).Count;testErrors=@($rows | Where-Object result -eq TEST_ERROR).Count;resultDirectory=$run} |
            ConvertTo-Json | Set-Content (Join-Path $run 'summary.json') -Encoding UTF8
    } else {
        [pscustomobject]@{runnerCompleted=$runnerCompleted;runnerError=$runnerError;commands=0;cases=$rows.Count;resultDirectory=$run} |
            ConvertTo-Json | Set-Content (Join-Path $run 'summary.json') -Encoding UTF8
    }
    $mutex.ReleaseMutex()
    $mutex.Dispose()
    Write-Output "DOOR STRESS RESULTS $run"
}
if (@($rows | Where-Object result -ne PASS).Count) { exit 1 }
exit 0

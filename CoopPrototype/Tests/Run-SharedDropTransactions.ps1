# Requires an already running, isolated native pair. This script never launches,
# kills, rejoins, resets inventory, or imports a fixture.
[CmdletBinding()]
param(
    [ValidateSet('preflight','success','full_inventory','hold_after_grant','observe_after_disconnect','observe_after_reconnect')]
    [string]$Phase = 'preflight',
    [string]$ProfilesRoot = (Join-Path $env:USERPROFILE 'Saved Games\Arkane Studios\PreyCoop-Repro-20261103'),
    [string]$GameRoot = 'D:\PreyCoop-Repro-20261103',
    [Parameter(Mandatory=$true)][string]$ResultRoot,
    [Parameter(Mandatory=$true)][ValidatePattern('^[0-9A-Fa-f]{64}$')][string]$ExpectedDllSha256,
    [ValidatePattern('^[0-9]+$')][string]$ArchetypeId = '10739735956144611856',
    [ValidateSet('host','client')][string]$DropRole = 'host',
    [ValidateSet('host','client')][string]$PickupRole = 'client',
    [string]$StatePath,
    [string]$HostPickupEvidencePath,
    [ValidateRange(1,120)][int]$WaitSeconds = 20,
    [switch]$FunctionsOnly
)
$ErrorActionPreference = 'Stop'
$inv = [Globalization.CultureInfo]::InvariantCulture
$run = Join-Path $ResultRoot ('shared-drop-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + $Phase)
New-Item -ItemType Directory -Force $run | Out-Null
$script:sequence = 0
$script:pauseTouched = @{}
$script:leaveClientPaused = $false
$script:state = $null
$script:builds = @{}
$script:pairScope = $null
$mutex = $null
$mutexOwned = $false
$outcome = 'TEST_ERROR'
$detail = ''

function Write-Json($path, $value) {
    $value | ConvertTo-Json -Depth 12 | Set-Content $path -Encoding ASCII
}

function UInt64-Text([string]$value) {
    [uint64]$parsed = 0
    if ($value -notmatch '^[0-9]+$' -or ![uint64]::TryParse($value, [ref]$parsed) -or $parsed -eq 0) {
        throw "Invalid positive uint64: $value"
    }
    return $parsed.ToString($inv)
}

function Token([string]$raw, [string]$name) {
    $m = [regex]::Match($raw, '(?:^|\s)' + [regex]::Escape($name) + '=([^\s]+)')
    if (!$m.Success) { throw "Missing status token $name" }
    return $m.Groups[1].Value
}

function Invoke-Control([string]$role, [string]$command, [string]$expected, [bool]$allowRejected=$false) {
    $script:sequence++
    $prefix = '{0:D6}-{1}' -f $script:sequence,$role
    $dir = Join-Path (Join-Path $ProfilesRoot $role) 'CoopRuntimeExtractor'
    $statusPath = Join-Path $dir 'status.txt'
    $oldRaw = [string](Get-Content $statusPath -Raw)
    $oldBatch = [regex]::Match($oldRaw, '(?m)^command=(?:OK|FAILED) ([0-9]+) command\(s\)\r?$')
    if (!$oldBatch.Success) { throw "BLOCKED: $role has no completed previous command batch" }
    $commandCount = @($command -split '\r?\n' | Where-Object { $_.Trim() -and !$_.Trim().StartsWith('#') }).Count
    $prefixCount = if ([int]$oldBatch.Groups[1].Value -eq ($commandCount+1)) { 2 } else { 1 }
    $batchSize = $prefixCount + $commandCount
    $before = (Get-Item $statusPath).LastWriteTimeUtc.Ticks
    $marker = [guid]::NewGuid().ToString()
    Write-Json (Join-Path $run ($prefix+'-command.json')) @{
        command=$command; expected=$expected; marker=$marker; batchSize=$batchSize; utc=[DateTime]::UtcNow.ToString('o')
    }
    # The comment is an audit marker, not an echoed ACK. Freshness comes from
    # changed completed-batch size, exact final action, final export line and mtime.
    $payload = (('coop_status'+"`r`n")*$prefixCount)+$command+"`r`n# "+$marker
    $temp = Join-Path $dir ('shared-drop-'+$marker+'.tmp')
    [IO.File]::WriteAllText($temp, $payload, [Text.Encoding]::ASCII)
    $backup = $temp+'.bak'
    [IO.File]::Replace($temp, (Join-Path $dir 'command.txt'), $backup)
    [IO.File]::Delete($backup)
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $raw = ''
    do {
        Start-Sleep -Milliseconds 100
        $raw = [string](Get-Content $statusPath -Raw -ErrorAction SilentlyContinue)
        $completed = [regex]::Match($raw, '(?m)^command=(OK|FAILED) '+$batchSize+' command\(s\)\r?$')
        $action = [regex]::Match($raw, 'cmd=([^\s]+) ok=([01])')
        if ($completed.Success -and $action.Success -and
            ($action.Groups[1].Value -eq $expected -or ($allowRejected -and $action.Groups[1].Value -eq ($expected+'_failed'))) -and
            $raw -match '(?m)^export=[^\r\n]*\r?\n\z' -and
            (Get-Item $statusPath).LastWriteTimeUtc.Ticks -gt $before) {
            [IO.File]::WriteAllText((Join-Path $run ($prefix+'-status.txt')), $raw)
            if (!$allowRejected -and ($action.Groups[2].Value -ne '1' -or $completed.Groups[1].Value -ne 'OK')) {
                throw "$role command rejected: $command (artifact $prefix)"
            }
            return [pscustomobject]@{Raw=$raw; Prefix=$prefix; Ok=($action.Groups[2].Value -eq '1')}
        }
    } while ($clock.Elapsed.TotalSeconds -lt 20)
    [IO.File]::WriteAllText((Join-Path $run ($prefix+'-timeout.txt')), $raw)
    throw "$role command timed out: $command"
}

function Read-Build([string]$role) {
    $exe = Join-Path $GameRoot "$role\Binaries\Danielle\x64\Release\Prey.exe"
    $p = @(Get-CimInstance Win32_Process | Where-Object ExecutablePath -eq $exe)
    if ($p.Count -ne 1) { throw "BLOCKED: expected one isolated $role process, found $($p.Count)" }
    $dll = Join-Path $GameRoot "$role\Mods\CoopPrototype\CoopPrototype.dll"
    $hash = (Get-FileHash $dll -Algorithm SHA256).Hash
    if ($hash -ne $ExpectedDllSha256) { throw "BLOCKED: $role DLL file hash $hash does not match $ExpectedDllSha256" }
    return [pscustomobject]@{Role=$role; Pid=[string]$p[0].ProcessId; Created=$p[0].CreationDate.ToString('o'); Exe=$exe; Dll=$dll; Hash=$hash}
}

function Assert-SameHost($saved) {
    $now = $script:builds.host
    if ($now.Pid -ne [string]$saved.Pid -or $now.Created -ne [string]$saved.Created) {
        throw 'BLOCKED: host process changed since the saved transaction boundary'
    }
}

function Assert-Ready([string]$role, [string]$raw, [bool]$requirePair=$true) {
    foreach ($entry in @(@('mode',$role),@('gameReady','1'),@('loadGuard','0'),@('localInteractive','1/interactive'))) {
        if ((Token $raw $entry[0]) -ne $entry[1]) { throw "BLOCKED: $role $($entry[0])=$(Token $raw $entry[0])" }
    }
    if ($requirePair) {
        if ((Token $raw 'sessionReady') -ne '1' -or (Token $raw 'session') -ne 'connected_same_level' -or
            (Token $raw 'localLevel') -ne (Token $raw 'remoteLevel') -or
            (Token $raw 'localEpoch') -ne (Token $raw 'remoteEpoch') -or
            (Token $raw 'remotePeerCount') -ne '1') { throw "BLOCKED: $role is not an isolated same-level two-player fixture" }
    }
    if ($script:pairScope) {
        if ((Token $raw 'localLevel') -ne $script:pairScope.Level -or
            (Token $raw 'localEpoch') -ne $script:pairScope.Epoch -or
            (Token $raw 'hostSaveKey') -ne $script:pairScope.SaveKey) {
            throw "BLOCKED: $role transaction world/save scope changed"
        }
    }
}

function Assert-Pair {
    $statuses = @{}
    foreach ($role in @('host','client')) {
        $s = Invoke-Control $role 'coop_status' 'status'
        Assert-Ready $role $s.Raw
        $statuses[$role] = $s.Raw
    }
    if ((Token $statuses.host 'localEpoch') -ne (Token $statuses.client 'localEpoch') -or
        (Token $statuses.host 'localLevel') -ne (Token $statuses.client 'localLevel') -or
        (Token $statuses.host 'hostSaveKey') -ne (Token $statuses.client 'hostSaveKey')) { throw 'BLOCKED: pair world/save scopes differ' }
    if (!$script:pairScope) {
        $script:pairScope = [pscustomobject]@{Level=(Token $statuses.host 'localLevel'); Epoch=(Token $statuses.host 'localEpoch'); SaveKey=(Token $statuses.host 'hostSaveKey')}
        if ($script:pairScope.Epoch -eq '0' -or $script:pairScope.SaveKey -in @('-','unknown_save')) { throw 'BLOCKED: invalid transaction scope' }
    }
    return $statuses
}

function Inventory-Count([string]$role) {
    $s = Invoke-Control $role "coop_inventory_count $ArchetypeId" 'inventory_count'
    Assert-Ready $role $s.Raw ($Phase -ne 'observe_after_disconnect')
    $v = (Token $s.Raw 'debugInv').Split('/')
    if ($v.Count -lt 2 -or (UInt64-Text $v[0]) -ne $ArchetypeId -or $v[1] -notmatch '^[0-9]+$') { throw "$role invalid exact-archetype inventory count" }
    return [int64]$v[1]
}

function Read-Drop([string]$role, [string]$stable, [bool]$allowAbsent=$false) {
    $s = Invoke-Control $role "coop_shared_drop_probe $stable" 'coop_shared_drop_probe' $allowAbsent
    Assert-Ready $role $s.Raw ($Phase -ne 'observe_after_disconnect')
    $network = Token $s.Raw 'network'
    if (!$s.Ok) {
        if ($allowAbsent -and $network -eq 'coop_shared_drop_probe_failed_detail_shared_drop_record_not_found') {
            Write-Json (Join-Path $run ($s.Prefix+'-absent-record.json')) @{
                Role=$role; StableId=$stable; Observation='record_not_found'; WorldEntityAbsenceProved=$false; Utc=[DateTime]::UtcNow.ToString('o')
            }
            return $null
        }
        throw "$role exact stable probe failed for another reason: $network"
    }
    $sample = [ordered]@{Utc=[DateTime]::UtcNow.ToString('o'); Role=$role}
    foreach ($key in @('stable','entity','exists','hidden','live','pending','granted','version','arch','count','nativeCount','owner','winner')) {
        $m = [regex]::Match($network, '_'+$key+'_([0-9]+)(?:_|$)')
        if (!$m.Success) { throw "$role probe lacks $key" }
        $sample[$key] = $m.Groups[1].Value
    }
    if ((UInt64-Text $sample.stable) -ne $stable -or (UInt64-Text $sample.arch) -ne $ArchetypeId -or $sample.count -ne '1') {
        throw "$role probe identity/count mismatch"
    }
    $row = [pscustomobject]$sample
    $row | Export-Csv (Join-Path $run 'samples.csv') -Append -NoTypeInformation
    return $row
}

function Capture-Inventory([string]$role, [string]$label) {
    $path = Join-Path (Join-Path (Join-Path $ProfilesRoot $role) 'CoopRuntimeExtractor') 'latest.jsonl'
    $before = if (Test-Path $path) { (Get-Item $path).LastWriteTimeUtc.Ticks } else { 0 }
    $s = Invoke-Control $role 'coop_inventory_dump' 'inventory_dump'
    if (!(Test-Path $path) -or (Get-Item $path).LastWriteTimeUtc.Ticks -le $before) { throw "$role inventory export is not fresh" }
    $destination = Join-Path $run ($s.Prefix+'-'+$label+'-inventory.jsonl')
    Copy-Item $path $destination
    $items = @(); $meta = @()
    foreach ($line in Get-Content $destination) {
        if (!$line.Trim()) { continue }
        $row = $line | ConvertFrom-Json
        if ($row.kind -eq 'inventory_meta') { $meta += $row }
        if ($row.kind -eq 'inventory_item') {
            # Preserve uint64 identity lexically; PS5 JSON numbers may round it.
            $m = [regex]::Match($line, '"archetypeId"\s*:\s*([0-9]+)')
            if (!$m.Success) { throw 'Inventory item has no integer archetypeId' }
            $row | Add-Member -NotePropertyName ExactArchetypeId -NotePropertyValue $m.Groups[1].Value
            $items += $row
        }
    }
    if ($meta.Count -ne 1 -or !$meta[0].readCells -or $meta[0].itemIdSource -ne 'stored_cells' -or
        [int]$meta[0].itemIds -ne $items.Count -or [int]$meta[0].storedCells -lt $items.Count -or
        ($items.Count -eq 0 -and [int]$meta[0].storedCells -ne 0)) { throw "$role inventory export is incomplete" }
    return [pscustomobject]@{Meta=$meta[0]; Items=$items; Path=$destination}
}

function Assert-FullGrid($dump) {
    $m = $dump.Meta
    if ($m.itemIdSource -ne 'stored_cells' -or !$m.readCells -or [int]$m.width -le 0 -or [int]$m.height -le 0 -or
        [int]$m.storedCells -ne $dump.Items.Count) { throw 'BLOCKED: full-grid fixture metadata is not trustworthy' }
    $cells = @{}
    foreach ($item in $dump.Items) {
        if (!$item.resolved -or !$item.hasCell) { throw 'BLOCKED: full-grid fixture contains an unresolved or unplaced item' }
        if ($item.ExactArchetypeId -eq $ArchetypeId) { throw 'BLOCKED: receiver already has target archetype; stack merging could bypass the full grid' }
        $x=[int]$item.x; $y=[int]$item.y; $w=[int]$item.cellW; $h=[int]$item.cellH
        # The dump exports native StorageCell coordinates, which are one-based.
        if ($x -lt 1 -or $y -lt 1 -or $w -le 0 -or $h -le 0 -or $x+$w-1 -gt [int]$m.width -or $y+$h-1 -gt [int]$m.height) {
            throw 'BLOCKED: invalid inventory cell bounds'
        }
        for ($yy=$y; $yy -lt $y+$h; $yy++) {
            for ($xx=$x; $xx -lt $x+$w; $xx++) {
                $key="$xx,$yy"
                if ($cells.ContainsKey($key)) { throw 'BLOCKED: overlapping inventory cells' }
                $cells[$key]=$true
            }
        }
    }
    if ($cells.Count -ne [int]$m.width*[int]$m.height) { throw 'BLOCKED: receiver grid is not completely full' }
}

function Inventory-ContainsEntity($dump, [string]$entityId) {
    return @($dump.Items | Where-Object { [string]$_.itemId -eq $entityId }).Count -gt 0
}

function Assert-WorldNotStored($dump, $drop) {
    if ($drop.entity -eq '0' -or (Inventory-ContainsEntity $dump $drop.entity)) {
        throw "$($drop.Role) live world record is still stored in the native inventory: entity=$($drop.entity)"
    }
}

function Capture-Sidecars([string]$role, [string]$label) {
    $source = Join-Path (Join-Path (Join-Path $ProfilesRoot $role) 'CoopPrototype') 'PlayerState'
    if (!(Test-Path $source)) { return }
    $target = Join-Path $run ($label+'-'+$role+'-player-state')
    New-Item -ItemType Directory -Force $target | Out-Null
    $files = @(Get-ChildItem $source -Filter '*.state' -File | Where-Object { $_.Name -match '^player_(?:account|recovery_account)_' })
    $manifest = @()
    foreach ($file in $files) {
        Copy-Item $file.FullName (Join-Path $target $file.Name)
        $manifest += @{Name=$file.Name; Length=$file.Length; LastWriteUtc=$file.LastWriteTimeUtc.ToString('o'); Sha256=(Get-FileHash (Join-Path $target $file.Name) -Algorithm SHA256).Hash}
    }
    Write-Json (Join-Path $target 'files.json') $manifest
}

function Set-Pause([string]$role, [bool]$paused) {
    $script:pauseTouched[$role]=$true
    $value = if ($paused) { '1' } else { '0' }
    $action = if ($paused) { 'reliable_test_paused' } else { 'reliable_test_resumed' }
    $s = Invoke-Control $role "coop_reliable_test_pause $value" $action
    if ((Token $s.Raw 'reliableTest').Split('/')[0] -ne $value) { throw "$role reliable pause did not take effect" }
}

function Save-State {
    Write-Json (Join-Path $run 'transaction-state.json') $script:state
}

function Wait-LivePair([string]$stable) {
    $clock = [Diagnostics.Stopwatch]::StartNew()
    do {
        $h = Read-Drop host $stable; $c = Read-Drop client $stable
        if ($h.live -eq '1' -and $c.live -eq '1' -and $h.pending -eq '0' -and $c.pending -eq '0' -and
            $h.exists -eq '1' -and $c.exists -eq '1' -and $h.hidden -eq '0' -and $c.hidden -eq '0' -and
            $h.nativeCount -eq '1' -and $c.nativeCount -eq '1' -and $h.version -eq $c.version) {
            foreach ($role in @('host','client')) {
                $dump=Capture-Inventory $role 'eligible-world'
                $drop=if ($role -eq 'host') {$h} else {$c}
                Assert-WorldNotStored $dump $drop
            }
            return @{host=$h;client=$c}
        }
        Start-Sleep -Milliseconds 100
    } while ($clock.Elapsed.TotalSeconds -lt $WaitSeconds)
    throw 'Shared-drop world replicas did not converge to the same eligible live lifetime'
}

if ($FunctionsOnly) { return }

try {
    $mutex = New-Object Threading.Mutex($false,'Local\PreyCoopDoorStress')
    $mutexOwned = $mutex.WaitOne(0)
    if (!$mutexOwned) { throw 'BLOCKED: another native command runner is active' }
    $ArchetypeId = UInt64-Text $ArchetypeId
    $observe = $Phase -in @('observe_after_disconnect','observe_after_reconnect')
    if ($observe) {
        if (!$StatePath) { throw 'BLOCKED: observation requires -StatePath from hold_after_grant' }
        $script:state = Get-Content $StatePath -Raw | ConvertFrom-Json
        if ($script:state.Boundary -ne 'native_success_result_held' -or $script:state.ExpectedDllSha256 -ne $ExpectedDllSha256) {
            throw 'BLOCKED: state does not describe the expected held native-success boundary'
        }
        $ArchetypeId = UInt64-Text ([string]$script:state.ArchetypeId)
        $stable = UInt64-Text ([string]$script:state.StableId)
        $script:pairScope = $script:state.Scope
        $script:builds.host = Read-Build host
        Assert-SameHost $script:state.Builds.host
        if ($Phase -eq 'observe_after_disconnect') {
            $oldClient = $script:state.Builds.client
            $p = @(Get-CimInstance Win32_Process | Where-Object { [string]$_.ProcessId -eq [string]$oldClient.Pid -and $_.CreationDate.ToString('o') -eq $oldClient.Created })
            if ($p.Count) { throw 'BLOCKED: recorded client process is still alive; root must terminate it externally' }
            $clock = [Diagnostics.Stopwatch]::StartNew()
            do {
                $s = Invoke-Control host 'coop_status' 'status'
                Assert-Ready host $s.Raw $false
                if ((Token $s.Raw 'remotePeerCount') -eq '0') { break }
                Start-Sleep -Milliseconds 200
            } while ($clock.Elapsed.TotalSeconds -lt $WaitSeconds)
            if ((Token $s.Raw 'remotePeerCount') -ne '0') { throw 'BLOCKED: host has not removed the disconnected peer' }
            $h = Read-Drop host $stable
            $hostInventory = Capture-Inventory host 'after-disconnect'
            Capture-Sidecars host 'after-disconnect'
            if ($h.live -eq '1' -and $h.pending -eq '0' -and $h.exists -eq '1' -and $h.hidden -eq '0' -and
                !(Inventory-ContainsEntity $hostInventory $h.entity)) {
                throw 'VIOLATION: host re-exposes an eligible world lifetime after the recorded successful client native acquisition; reconnect is required to measure durable duplication'
            }
            if ($h.live -ne '0') { throw 'UNRESOLVED: host has no proven terminal outcome for the recorded successful native acquisition' }
            $outcome='SAFE_TERMINAL_OBSERVED'
            $detail='No eligible live world copy; this alone does not prove winner recovery or absence of item loss.'
        } else {
            $script:builds.client = Read-Build client
            $null = Assert-Pair
            $hc = Inventory-Count host; $cc = Inventory-Count client
            $hostInventory = Capture-Inventory host 'after-reconnect'; $clientInventory = Capture-Inventory client 'after-reconnect'
            Capture-Sidecars client 'after-reconnect'
            Capture-Sidecars host 'after-reconnect'
            $h = Read-Drop host $stable; $c = Read-Drop client $stable $true
            $originalHost = [int64]$script:state.BaselineCounts.host
            $originalClient = [int64]$script:state.BaselineCounts.client
            $heldClient = $originalClient + 1
            $hostPickupProved = $false
            if ($HostPickupEvidencePath) {
                $evidence = Get-Content $HostPickupEvidencePath -Raw
                Copy-Item $HostPickupEvidencePath (Join-Path $run 'external-host-pickup-status.txt')
                $hostPickupProved = $evidence -match 'cmd=coop_shared_drop_pickup ok=1' -and
                    $evidence -match ('_nativeSuccesses_[1-5]_stable_'+[regex]::Escape($stable)+'_') -and
                    $evidence -match '(?m)^command=OK [0-9]+ command\(s\)\r?$'
                if (!$hostPickupProved) { throw 'BLOCKED: supplied host pickup evidence is not a completed native-success response for this exact stable ID' }
            }
            $observation = @{
                StableId=$stable; OriginalCounts=@{host=$originalHost;client=$originalClient}; HeldClientCount=$heldClient
                CurrentCounts=@{host=$hc;client=$cc}; InventoryExcess=($hc+$cc-$originalHost-$originalClient)
                HostReturnedToPreDropCount=($hc -eq $originalHost); ExternalHostNativePickupProved=$hostPickupProved
                ClientStableRecordAbsent=($null -eq $c); ClientWorldEntityAbsenceProved=$false; HostProbe=$h; ClientProbe=$c
            }
            Write-Json (Join-Path $run 'reconnect-observation.json') $observation
            if ($cc -ne $heldClient) { throw "VIOLATION: winner inventory recovery is $cc, expected recorded held count $heldClient" }
            if ($hostPickupProved -and $hc -ne $originalHost) { throw 'VIOLATION: externally proved host native pickup was not retained in the host inventory' }
            if ($hc+$cc -gt $originalHost+$originalClient) {
                throw "VIOLATION: inventory alone contains $($hc+$cc-$originalHost-$originalClient) extra unit(s); host=$hc client=$cc originalHost=$originalHost originalClient=$originalClient; missing client registry coverage cannot erase this excess"
            }
            if (($h.live -eq '1' -and $h.pending -eq '0' -and $h.exists -eq '1' -and $h.hidden -eq '0' -and
                    !(Inventory-ContainsEntity $hostInventory $h.entity)) -or
                ($c -and $c.live -eq '1' -and $c.pending -eq '0' -and $c.exists -eq '1' -and $c.hidden -eq '0' -and
                    !(Inventory-ContainsEntity $clientInventory $c.entity))) {
                throw 'VIOLATION: recovered client acquisition coexists with an eligible live shared-drop world lifetime'
            }
            if ($hc -ne ($originalHost-1) -or $h.live -ne '0' -or $h.pending -ne '0' -or
                ($c -and ($c.live -ne '0' -or $c.pending -ne '0'))) { throw 'UNRESOLVED: inventory and transaction state do not establish a conserved terminal outcome' }
            if ($null -eq $c) {
                $outcome='CONSERVATION_OBSERVED'; $detail='Recorded client acquisition recovered; inventory total conserved and host record terminal. Fresh client has no old stable record, so its world-entity absence is unproved.'
            } else {
                $outcome='PASS'; $detail='Recorded client acquisition recovered exactly once; inventory total conserved and both known shared-drop records terminal.'
            }
        }
    } else {
        foreach ($role in @('host','client')) { $script:builds[$role]=Read-Build $role }
        $statuses = Assert-Pair
        foreach ($role in @('host','client')) {
            if ((Token $statuses[$role] 'reliableTest') -notmatch '^0/0/') { throw "BLOCKED: $role already has reliable impairment enabled" }
        }
        Write-Json (Join-Path $run 'preflight.json') @{Builds=$script:builds; Scope=$script:pairScope; ExpectedDllSha256=$ExpectedDllSha256}
        if ($Phase -eq 'preflight') {
            foreach ($role in @('host','client')) { $null=Capture-Inventory $role 'preflight'; Capture-Sidecars $role 'preflight' }
            $outcome='PASS'; $detail='Existing isolated pair is ready; no inventory/network mutation performed.'
        } else {
            if ($Phase -in @('hold_after_grant','full_inventory') -and ($DropRole -ne 'host' -or $PickupRole -ne 'client')) {
                throw 'BLOCKED: held-grant/full-inventory phases require host drop and client pickup'
            }
            $baseline=@{}; $dumps=@{}
            foreach ($role in @('host','client')) { $baseline[$role]=Inventory-Count $role; $dumps[$role]=Capture-Inventory $role 'before'; Capture-Sidecars $role 'before' }
            if ($baseline[$DropRole] -lt 1) { throw "BLOCKED: $DropRole has no unit of archetype $ArchetypeId to drop" }
            if ($Phase -eq 'full_inventory') {
                Assert-FullGrid $dumps.client
                if ($baseline.client -ne 0 -or !@($dumps.host.Items | Where-Object { $_.ExactArchetypeId -eq $ArchetypeId -and $_.hasCell -and [int]$_.dimW -gt 0 -and [int]$_.dimH -gt 0 }).Count) {
                    throw 'BLOCKED: target must be an absent receiver archetype and a native grid item on the source'
                }
            }
            $s=Invoke-Control $DropRole "coop_shared_drop_drop $ArchetypeId 1" 'coop_shared_drop_drop'
            $match=[regex]::Match((Token $s.Raw 'network'),'_stable_([0-9]+)(?:_|$)')
            if (!$match.Success) { throw 'Native drop response has no stable ID' }
            $stable=UInt64-Text $match.Groups[1].Value
            $script:state=[ordered]@{Schema=1; Phase=$Phase; Boundary='own_drop_created'; CreatedUtc=[DateTime]::UtcNow.ToString('o'); ExpectedDllSha256=$ExpectedDllSha256; ArchetypeId=$ArchetypeId; StableId=$stable; DropRole=$DropRole; PickupRole=$PickupRole; BaselineCounts=$baseline; PostDropCounts=@{host=($baseline.host-[int]($DropRole -eq 'host'));client=($baseline.client-[int]($DropRole -eq 'client'))}; Builds=$script:builds; Scope=$script:pairScope}
            Save-State
            $live=Wait-LivePair $stable
            foreach ($role in @('host','client')) {
                $expected=$baseline[$role]-[int]($role -eq $DropRole)
                if ((Inventory-Count $role) -ne $expected) { throw "$role inventory changed unexpectedly during native drop" }
            }
            if ($Phase -in @('hold_after_grant','full_inventory')) {
                Set-Pause host $true
                $null=Invoke-Control client "coop_shared_drop_pickup $stable 1" 'coop_shared_drop_pickup' $true
                $clock=[Diagnostics.Stopwatch]::StartNew()
                do {
                    $h=Read-Drop host $stable
                    if ($h.pending -eq '1' -and $h.live -eq '1' -and $h.winner -ne '0') { break }
                    Start-Sleep -Milliseconds 100
                } while ($clock.Elapsed.TotalSeconds -lt $WaitSeconds)
                if ($h.pending -ne '1' -or $h.live -ne '1' -or $h.winner -eq '0' -or (Inventory-Count client) -ne $baseline.client) {
                    throw 'Could not establish host-reserved, grant-held boundary before native acquisition'
                }
                Set-Pause client $true
                Set-Pause host $false
                $clock=[Diagnostics.Stopwatch]::StartNew()
                $nativeFailure=$false; $c=$null; $cc=-1
                do {
                    $s=Invoke-Control client 'coop_status' 'status'
                    $last=Token $s.Raw 'sharedDropLast'
                    $nativeFailure=($last -match ('^(?:native_pickup_failed_id_|sent_pickup_failure_id_)'+[regex]::Escape($stable)+'_'))
                    # Probe rewrites sharedDropLast. Preserve the failure event
                    # before issuing probes, rather than erasing it while polling.
                    if ($Phase -eq 'full_inventory' -and !$nativeFailure) { Start-Sleep -Milliseconds 100; continue }
                    $c=Read-Drop client $stable
                    $cc=Inventory-Count client
                    if ($Phase -eq 'hold_after_grant' -and $cc -eq $baseline.client+1 -and $c.granted -eq '1' -and $c.live -eq '0') { break }
                    if ($Phase -eq 'full_inventory' -and $nativeFailure -and $cc -eq $baseline.client -and $c.granted -eq '0' -and $c.live -eq '1' -and
                        $c.exists -eq '1' -and $c.nativeCount -eq '1') { break }
                    Start-Sleep -Milliseconds 100
                } while ($clock.Elapsed.TotalSeconds -lt $WaitSeconds)
                if (!$c) { throw 'BLOCKED: no actual failed native pickup event was observed in the full-grid fixture' }
                $h=Read-Drop host $stable
                if ($h.pending -ne '1' -or $h.live -ne '1' -or $h.version -ne $c.version -or $h.winner -ne $c.winner) { throw 'Result-held boundary was not stable on both peers' }
                if ($Phase -eq 'hold_after_grant') {
                    if ($cc -ne $baseline.client+1 -or $c.granted -ne '1' -or $c.live -ne '0') { throw 'BLOCKED: native acquisition did not succeed at the held-result boundary' }
                    $null=Capture-Inventory client 'native-success-result-held'
                    Capture-Sidecars client 'native-success-result-held'
                    $h=Read-Drop host $stable
                    if ($h.pending -ne '1' -or $h.live -ne '1' -or (Inventory-Count client) -ne $baseline.client+1) { throw 'Held result escaped while preserving evidence' }
                    $script:state.Boundary='native_success_result_held'
                    $script:state.HeldUtc=[DateTime]::UtcNow.ToString('o')
                    $script:state.HeldClientCount=$cc
                    $script:state.HostProbe=$h; $script:state.ClientProbe=$c
                    Save-State
                    $script:leaveClientPaused=$true
                    $outcome='BOUNDARY_HELD'; $detail='Client outgoing reliable sends intentionally remain paused. Root must externally terminate the exact recorded client PID, then run observe_after_disconnect.'
                } else {
                    if (!$nativeFailure -or $cc -ne $baseline.client -or $c.granted -ne '0' -or $c.live -ne '1' -or
                        $c.exists -ne '1' -or $c.nativeCount -ne '1') { throw 'BLOCKED: no actual failed native pickup with a retained world item was observed in the full-grid fixture' }
                    $failedInventory=Capture-Inventory client 'native-failure-result-held'
                    Assert-WorldNotStored $failedInventory $c
                    Set-Pause client $false
                    $released=Wait-LivePair $stable
                    if ([uint64]$released.host.version -le [uint64]$live.host.version -or (Inventory-Count client) -ne $baseline.client -or (Inventory-Count host) -ne $baseline.host-1) {
                        throw 'Failed native pickup did not release one unchanged eligible lifetime'
                    }
                    $outcome='PASS'; $detail='Actual native pickup failed; no receiver count increase; same stable lifetime released on both peers. Retry after freeing space is a separate test.'
                }
            } else {
                $null=Invoke-Control $PickupRole "coop_shared_drop_pickup $stable 1" 'coop_shared_drop_pickup' $true
                $clock=[Diagnostics.Stopwatch]::StartNew()
                do {
                    $h=Read-Drop host $stable; $c=Read-Drop client $stable
                    $counts=@{host=(Inventory-Count host);client=(Inventory-Count client)}
                    $matched=$true
                    foreach ($role in @('host','client')) {
                        if ($counts[$role] -ne $baseline[$role]-[int]($role -eq $DropRole)+[int]($role -eq $PickupRole)) { $matched=$false }
                    }
                    if ($matched -and $h.live -eq '0' -and $c.live -eq '0' -and $h.pending -eq '0' -and $c.pending -eq '0') { break }
                    Start-Sleep -Milliseconds 100
                } while ($clock.Elapsed.TotalSeconds -lt $WaitSeconds)
                if (!$matched -or $h.live -ne '0' -or $c.live -ne '0' -or $h.pending -ne '0' -or $c.pending -ne '0') { throw 'Successful pickup did not converge to exactly one acquired unit and a terminal shared lifetime' }
                foreach ($role in @('host','client')) { $null=Capture-Inventory $role 'after-success'; Capture-Sidecars $role 'after-success' }
                $script:state.Boundary='committed_success'; Save-State
                $outcome='PASS'; $detail='Exactly one native unit acquired; both shared-drop records terminal and not pending.'
            }
        }
    }
} catch {
    $detail=$_.Exception.Message
    if ($detail.StartsWith('BLOCKED:')) { $outcome='BLOCKED' }
    elseif ($detail.StartsWith('VIOLATION:')) { $outcome='VIOLATION' }
    elseif ($detail.StartsWith('UNRESOLVED:')) { $outcome='UNRESOLVED' }
    else { $outcome='TEST_ERROR' }
} finally {
    $cleanupErrors=@()
    foreach ($role in @('host','client')) {
        if ($script:pauseTouched.ContainsKey($role) -and !($role -eq 'client' -and $script:leaveClientPaused)) {
            try { Set-Pause $role $false } catch { $cleanupErrors += "$role pause restore: $($_.Exception.Message)" }
        }
    }
    if ($cleanupErrors.Count) { $outcome='TEST_ERROR'; $detail += '; '+($cleanupErrors -join '; ') }
    Write-Json (Join-Path $run 'result.json') @{Outcome=$outcome; Detail=$detail; Phase=$Phase; Run=$run; StableId=if ($script:state) {$script:state.StableId} else {$null}; ClientSendsIntentionallyPaused=$script:leaveClientPaused; CleanupErrors=$cleanupErrors; Utc=[DateTime]::UtcNow.ToString('o')}
    if ($mutexOwned) { $mutex.ReleaseMutex() }
    if ($mutex) { $mutex.Dispose() }
}
Write-Output "$outcome $detail"
Write-Output "RESULT $run"
if ($outcome -in @('BLOCKED','VIOLATION','UNRESOLVED','TEST_ERROR')) { exit 1 }

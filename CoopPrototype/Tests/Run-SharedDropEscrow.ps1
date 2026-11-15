# Runs against an existing disposable pair. Held phases leave reliable delivery
# paused intentionally; killing and restarting the exact Client is external.
[CmdletBinding()]
param(
    [ValidateSet('prepare_timeout','success','commit_held','result_held','after_rejoin')]
    [Parameter(Mandatory=$true)][string]$Case,
    [Parameter(Mandatory=$true)][string]$ExpectedDllSha256,
    [string]$ResultRoot = 'D:/PreyCoop-PR60-20261109/escrow-results',
    [string]$StatePath,
    [string]$ArchetypeId = '10739735956144611856'
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Run-SharedDropTransactions.ps1" -FunctionsOnly -Phase preflight `
    -ResultRoot $ResultRoot -ExpectedDllSha256 $ExpectedDllSha256 -ArchetypeId $ArchetypeId -StatePath $StatePath
$outcome = 'TEST_ERROR'
$detail = ''
$leavePaused = $false
$testState = $null
$escrowMutex = New-Object Threading.Mutex($false,'Local\PreyCoopDoorStress')
$escrowMutexOwned = $false

function Count-Item([string]$role) {
    return Inventory-Count $role
}

function Probe([string]$role, [string]$stable) {
    $s = Invoke-Control $role "coop_shared_drop_probe $stable" 'coop_shared_drop_probe' $true
    Assert-Ready $role $s.Raw
    if (!$s.Ok) { return $null }
    $network = Token $s.Raw 'network'
    $row = [ordered]@{Role=$role;Utc=[DateTime]::UtcNow.ToString('o')}
    foreach ($key in @('stable','entity','exists','hidden','live','pending','granted','committed','stored','attempted','received','version','arch','count','owner','winner')) {
        $m = [regex]::Match($network, '_'+$key+'_([0-9]+)(?:_|$)')
        if (!$m.Success) { throw "Probe lacks $key" }
        $row[$key] = $m.Groups[1].Value
    }
    if ($row.stable -ne $stable -or $row.arch -ne $ArchetypeId) { throw 'Wrong probed lifetime' }
    $r = [pscustomobject]$row
    $r | Export-Csv (Join-Path $run 'escrow-samples.csv') -Append -NoTypeInformation
    return $r
}

function Capture-Entitlement([string]$stable) {
    $source = Join-Path $ProfilesRoot 'host/CoopPrototype/ServerSaves'
    $found = @()
    foreach ($hostFolder in Get-ChildItem $source -Directory) {
        $scope = Join-Path $hostFolder.FullName $script:pairScope.SaveKey
        if (!(Test-Path -LiteralPath $scope)) { continue }
        foreach ($file in Get-ChildItem -LiteralPath $scope -Filter 'player_account_*.state' -File) {
            if ($file.Length -gt 10485760) { throw 'Current scoped state unexpectedly large' }
            $text = [IO.File]::ReadAllText($file.FullName)
            $row = [regex]::Match($text, '(?m)^sharedDropDelivery=[0-9]+ '+$stable+' '+$ArchetypeId+' 1 0 [0-9]+ [0-9]+\r?$')
            if ($row.Success) {
                $destination = Join-Path $run ('host-entitlement-'+$found.Count+'.state')
                Copy-Item $file.FullName $destination
                $found += @{Source=$file.FullName;Copy=$destination;Sha256=(Get-FileHash $destination -Algorithm SHA256).Hash;Rows=@($row.Value)}
            }
        }
    }
    if ($found.Count -ne 1) { throw 'Durable pending Host assignment missing or ambiguous in the current save scope' }
    Write-Json (Join-Path $run 'host-entitlements.json') $found
}

function Wait-Probe([string]$role, [string]$stable, [scriptblock]$predicate, [int]$seconds=15) {
    $clock = [Diagnostics.Stopwatch]::StartNew()
    do {
        $r = Probe $role $stable
        if ($r -and (& $predicate $r)) { return $r }
        Start-Sleep -Milliseconds 150
    } while ($clock.Elapsed.TotalSeconds -lt $seconds)
    throw "$role lifetime did not reach the requested $Case boundary"
}

function Save-EscrowState {
    Write-Json (Join-Path $run 'transaction-state.json') $testState
    Write-Json (Join-Path $ResultRoot 'latest-transaction-state.json') $testState
}

try {
    $escrowMutexOwned = $escrowMutex.WaitOne(0)
    if (!$escrowMutexOwned) { throw 'Another native command runner is active' }
    foreach ($role in @('host','client')) { $script:builds[$role] = Read-Build $role }
    $statuses = Assert-Pair
    if ($Case -eq 'after_rejoin') {
        $testState = Get-Content $StatePath -Raw | ConvertFrom-Json
        if ($testState.ExpectedDllSha256 -ne $ExpectedDllSha256 -or $testState.Case -notin @('commit_held','result_held')) { throw 'Wrong held state' }
        Assert-SameHost $testState.Builds.host
        if ($script:builds.client.Pid -eq [string]$testState.Builds.client.Pid -and
            $script:builds.client.Created -eq [string]$testState.Builds.client.Created) { throw 'Client was not replaced; recovery is untested' }
        if ($script:pairScope.Level -ne $testState.Scope.Level -or $script:pairScope.Epoch -ne $testState.Scope.Epoch -or $script:pairScope.SaveKey -ne $testState.Scope.SaveKey) { throw 'Rejoin changed the timeline' }
        $stable = [string]$testState.StableId
        $hc = Count-Item host; $cc = Count-Item client
        if ($hc -ne ([int64]$testState.BaselineCounts.host-1) -or $cc -ne ([int64]$testState.BaselineCounts.client+1)) { throw "Inventory not conserved after recovery: host=$hc client=$cc" }
        $h = Probe host $stable
        if (!$h -or $h.live -ne '0' -or $h.exists -ne '0' -or $h.committed -ne '1') { throw 'Host re-exposed committed world item' }
        Capture-Sidecars host 'after-rejoin'; Capture-Sidecars client 'after-rejoin'
        $null = Capture-Inventory host 'after-rejoin'; $null = Capture-Inventory client 'after-rejoin'
        $outcome = 'PASS'
        $detail = 'Winner recovered one unit; unchanged Host retained no eligible world copy and total inventory was conserved.'
    } else {
        foreach ($role in @('host','client')) {
            if ((Token $statuses[$role] 'reliableTest') -notmatch '^0/0/') { throw "$role already has reliable impairment enabled" }
        }
        $baseline = @{host=(Count-Item host);client=(Count-Item client)}
        if ($baseline.host -lt 1) { throw 'Host has no target item' }
        foreach ($role in @('host','client')) { $null = Capture-Inventory $role 'before'; Capture-Sidecars $role 'before' }
        $s = Invoke-Control host "coop_shared_drop_drop $ArchetypeId 1" 'coop_shared_drop_drop'
        $m = [regex]::Match((Token $s.Raw 'network'), '_stable_([0-9]+)(?:_|$)')
        if (!$m.Success) { throw 'Drop response lacks stable identity' }
        $stable = $m.Groups[1].Value
        $null = Wait-LivePair $stable
        $testState = [ordered]@{
            Schema=2;Case=$Case;Boundary='world';ExpectedDllSha256=$ExpectedDllSha256
            ArchetypeId=$ArchetypeId;StableId=$stable;BaselineCounts=$baseline;Builds=$script:builds;Scope=$script:pairScope
        }
        Save-EscrowState
        if ((Count-Item host) -ne ($baseline.host-1) -or (Count-Item client) -ne $baseline.client) { throw 'Native drop changed unexpected inventory' }
        if ($Case -eq 'success') {
            $null = Invoke-Control client "coop_shared_drop_pickup $stable 1" 'coop_shared_drop_pickup'
            $c = Wait-Probe client $stable {param($r) $r.committed -eq '1' -and $r.granted -eq '1' -and $r.received -eq '1' -and $r.stored -eq '1'}
            if ((Count-Item host) -ne ($baseline.host-1) -or (Count-Item client) -ne ($baseline.client+1)) { throw 'Successful pickup was not conserved' }
            $h = Probe host $stable
            if ($h.live -ne '0' -or $h.exists -ne '0' -or $h.committed -ne '1') { throw 'Committed source remained available' }
            $outcome = 'PASS'; $detail = 'Native acquisition followed durable commit; one receipt and conserved inventory observed.'
        } else {
            Set-Pause host $true
            $null = Invoke-Control client "coop_shared_drop_pickup $stable 1" 'coop_shared_drop_pickup'
            $null = Wait-Probe host $stable {param($r) $r.pending -eq '1' -and $r.committed -eq '0'}
            Set-Pause client $true
            Set-Pause host $false
            $null = Wait-Probe client $stable {param($r) $r.pending -eq '1' -and $r.committed -eq '0' -and $r.hidden -eq '1'}
            if ((Count-Item client) -ne $baseline.client) { throw 'Prepare gave usable inventory' }
            if ($Case -eq 'prepare_timeout') {
                $h = Wait-Probe host $stable {param($r) $r.live -eq '1' -and $r.pending -eq '0' -and $r.hidden -eq '0' -and $r.committed -eq '0'} 40
                Set-Pause client $false
                $null = Wait-LivePair $stable
                $drain = [Diagnostics.Stopwatch]::StartNew()
                do {
                    $s = Invoke-Control client 'coop_status' 'status'
                    if ((Token $s.Raw 'reliable').Split('/')[0] -eq '0') { break }
                    Start-Sleep -Milliseconds 200
                } while ($drain.Elapsed.TotalSeconds -lt 15)
                if ((Token $s.Raw 'reliable').Split('/')[0] -ne '0') { throw 'Delayed Ready queue did not drain' }
                $h = Probe host $stable
                if ($h.live -ne '1' -or $h.committed -ne '0' -or $h.pending -ne '0') { throw 'Delayed Ready changed cancelled ownership' }
                if ((Count-Item client) -ne $baseline.client) { throw 'Cancelled preparation gave an item' }
                $outcome = 'PASS'; $detail = 'Preparation gave nothing; wall-clock timeout restored the shared item and delayed Ready did not commit it.'
            } else {
                Set-Pause host $true
                Set-Pause client $false
                $h = Wait-Probe host $stable {param($r) $r.committed -eq '1' -and $r.live -eq '0' -and $r.exists -eq '0'}
                if ((Count-Item client) -ne $baseline.client) { throw 'Client acquired before final commit delivery' }
                if ($Case -eq 'result_held') {
                    Set-Pause client $true
                    Set-Pause host $false
                    $c = Wait-Probe client $stable {param($r) $r.committed -eq '1' -and $r.granted -eq '1' -and $r.received -eq '1' -and $r.stored -eq '1'}
                    if ((Count-Item client) -ne ($baseline.client+1)) { throw 'Native delivery not observed before result cutoff' }
                    $testState.Boundary = 'native_success_result_held'
                } else { $testState.Boundary = 'durable_commit_delivery_held' }
                Capture-Sidecars host 'held'; Capture-Sidecars client 'held'
                Capture-Entitlement $stable
                $testState.HeldUtc = [DateTime]::UtcNow.ToString('o')
                Save-EscrowState
                $leavePaused = $true
                $outcome = 'HELD'; $detail = $testState.Boundary
            }
        }
        Save-EscrowState
    }
} catch {
    $detail = $_.Exception.Message
} finally {
    if (!$leavePaused) {
        foreach ($role in @('host','client')) {
            if ($script:pauseTouched.ContainsKey($role)) {
                try { Set-Pause $role $false } catch { Write-Warning $_.Exception.Message }
            }
        }
    }
    Write-Json (Join-Path $run 'escrow-result.json') @{Outcome=$outcome;Detail=$detail;Case=$Case;Utc=[DateTime]::UtcNow.ToString('o')}
    if ($escrowMutexOwned) { $escrowMutex.ReleaseMutex() }
    $escrowMutex.Dispose()
}
Write-Output "$outcome $detail"
Write-Output "RESULT $run"
if ($outcome -notin @('PASS','HELD')) { exit 1 }

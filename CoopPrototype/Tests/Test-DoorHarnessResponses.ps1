param([string]$HarnessPath = (Join-Path $PSScriptRoot 'Run-DoorStress.ps1'))
$ErrorActionPreference='Stop'
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($HarnessPath,[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw 'Harness parse failed' }
$assignment=@($ast.FindAll({param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and
    $node.Left.Extent.Text -eq '$actionMatches'
},$true))
if ($assignment.Count -ne 1) { throw 'Expected one response classifier' }
$classify=[scriptblock]::Create($assignment[0].Right.Extent.Text)
$cases=@(
    @('area_door_set_failed',$true,$false,'area_door_set',1,$true),
    @('area_door_set_failed',$true,$false,'area_door_set_failed',0,$true),
    @('area_door_set',$false,$false,'area_door_set',1,$true),
    @('area_door_set',$false,$false,'area_door_set_failed',0,$false),
    @('area_door_set',$false,$true,'area_door_set_failed',0,$true),
    @('area_door_set_failed',$true,$false,'player_place_aim',1,$false)
)
foreach ($case in $cases) {
    $expected,$expectRejected,$allowEitherResult,$action,$ok,$want=$case
    $response=[regex]::Match("cmd=$action ok=$ok",'cmd=([^\s]+) ok=([01])')
    $actual=& $classify
    if ($actual -ne $want) { throw "Wrong response classification: $($case -join ',')" }
}
Write-Output "PASS: $($cases.Count) production harness response-classifier checks"

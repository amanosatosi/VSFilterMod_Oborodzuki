$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$results = New-Item -ItemType Directory -Force benchmark-results
$renderers = [ordered]@{}
foreach ($renderer in ($env:BENCH_RENDERERS | ConvertFrom-Json)) {
    $dir = "benchmark-bundles\renderer-$($renderer.kind)-$($renderer.ref)"
    $dll = Join-Path $dir 'renderer.dll'
    $info = Join-Path $dir 'build.json'
    if (!(Test-Path -LiteralPath $dll -PathType Leaf) -or !(Test-Path -LiteralPath $info -PathType Leaf)) {
        throw "Missing benchmark artifact for $($renderer.kind) at $($renderer.ref)"
    }
    $metadata = Get-Content -LiteralPath $info -Raw | ConvertFrom-Json
    if ($metadata.component -ne $renderer.kind -or $metadata.revision -ne $renderer.ref -or
        $metadata.platform -ne 'x64' -or $metadata.instrumented) {
        throw "Unexpected build metadata for $($renderer.kind)"
    }
    $renderers[$renderer.kind] = $metadata
    $metadata | Add-Member -NotePropertyName dll_sha256 -NotePropertyValue (Get-FileHash $dll).Hash.ToLowerInvariant()
    $metadata | Add-Member -NotePropertyName dll_path -NotePropertyValue (Resolve-Path $dll).Path
}
$harness = 'benchmark-harness\render-bench.exe'
$harnessInfo = 'benchmark-harness\build.json'
if (!(Test-Path $harness -PathType Leaf) -or !(Test-Path $harnessInfo -PathType Leaf)) {
    throw 'Missing compiled benchmark harness'
}
$harnessMetadata = Get-Content $harnessInfo -Raw | ConvertFrom-Json
if ($harnessMetadata.revision -ne $env:GITHUB_SHA) { throw 'Harness revision mismatch' }
$fontKeys = @(
    'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts',
    'HKCU:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts'
)
$fontNames = foreach ($key in $fontKeys) {
    if (Test-Path -LiteralPath $key) {
        (Get-Item -LiteralPath $key).GetValueNames() |
            ForEach-Object { $_ -replace '\s+\([^)]*\)$', '' }
    }
}
@{
    image = $env:ImageOS
    image_version = $env:ImageVersion
    cpu = Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors
    ram_bytes = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
    registered_fonts = @($fontNames | Sort-Object -Unique)
    renderers = $renderers
    harness = $harnessMetadata
    suite = $env:BENCH_SUITE
    primary_input = 'op.fx.4testing.ass'
    primary_input_sha256 = $env:BENCH_INPUT_SHA256
} | ConvertTo-Json -Depth 8 | Set-Content "$($results.FullName)\environment.json"
python .github\ci\run-bench.py $results.FullName $renderers.original.dll_path $renderers.baseline.dll_path $renderers.candidate.dll_path $renderers.mangetsu.dll_path

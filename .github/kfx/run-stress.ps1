param([switch]$Profile)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
. "$PSScriptRoot\bundles.ps1"
$root = (New-Item -ItemType Directory -Force $(if ($Profile) { 'kfx-profile' } else { 'kfx-raw' })).FullName
$manifest = $env:KFX_RENDERERS | ConvertFrom-Json
$renderers = [ordered]@{}
if ($Profile) {
    $renderers.profile = Read-KfxBundle bundle 'profile' $env:GITHUB_SHA
} else {
    foreach ($renderer in $manifest) {
        $dir = "kfx-bundles\kfx-renderer-$($renderer.kind)-$($renderer.ref)"
        if (!(Test-Path -LiteralPath $dir -PathType Container)) { throw "Missing artifact for $($renderer.kind)" }
        $renderers[$renderer.kind] = Read-KfxBundle $dir $renderer.kind $renderer.ref
    }
}
$harness = Read-KfxBundle 'kfx-harness' 'harness' $env:GITHUB_SHA
$fontKeys = @('HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts',
    'HKCU:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts')
$fonts = foreach ($key in $fontKeys) {
    if (Test-Path -LiteralPath $key) {
        (Get-Item -LiteralPath $key).GetValueNames() |
            ForEach-Object { $_ -replace '\s+\([^)]*\)$', '' }
    }
}
@{
    run_id = $env:GITHUB_RUN_ID
    image = $env:ImageOS
    image_version = $env:ImageVersion
    cpu = @(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors)
    ram_bytes = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
    registered_fonts = @($fonts | Sort-Object -Unique)
    renderers = $renderers
    harness = $harness
} | ConvertTo-Json -Depth 12 | Set-Content "$root\environment.json"
if ($Profile) {
    python .github\kfx\run-playback.py --profile
} else {
    python .github\kfx\run-playback.py
}

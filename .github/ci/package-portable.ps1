$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
. "$PSScriptRoot/../kfx/bundles.ps1"

$package = (New-Item -ItemType Directory portable-stage).FullName
$dist = (New-Item -ItemType Directory portable-dist).FullName
$binaries = New-Item -ItemType Directory "$package/binaries"
$fixtures = New-Item -ItemType Directory "$package/fixtures"
$runtime = New-Item -ItemType Directory "$package/runtime"
$runner = New-Item -ItemType Directory "$package/.github/ci"
Copy-Item Run-Benchmark.bat, LICENSE "$package/"
Copy-Item "$PSScriptRoot/Run-PortableBenchmark.ps1", "$PSScriptRoot/portable_benchmark.py", "$PSScriptRoot/bench_fixtures.py" $runner
Copy-Item "$PSScriptRoot/../parallel/scale_fixtures.py", "$PSScriptRoot/../kfx/fixture.py" $runner
Copy-Item "$PSScriptRoot/shared.ass", "$PSScriptRoot/op.fx.4testing.ass" $fixtures

foreach ($renderer in ($env:BENCH_RENDERERS | ConvertFrom-Json)) {
    $src = "benchmark-bundles/renderer-$($renderer.kind)-$($renderer.ref)"
    $null = Assert-KfxBundle $src $renderer.kind $renderer.ref
    Copy-Item $src "$binaries/$($renderer.kind)" -Recurse
}
$null = Read-KfxBundle benchmark-harness harness $env:GITHUB_SHA
Assert-KfxPe benchmark-harness/render-bench.exe $false
Copy-Item benchmark-harness "$binaries/hosts" -Recurse

$pythonVersion = '3.13.11'
$pythonZip = Join-Path $dist 'python-runtime.tmp'
Invoke-WebRequest "https://www.python.org/ftp/python/$pythonVersion/python-$pythonVersion-embed-amd64.zip" -OutFile $pythonZip
if ((Get-FileHash $pythonZip).Hash -cne '1EC066FB61BA5E8C73E29E048CD07C26850F74585E3A116005135B31B8004890') {
    throw 'Embedded Python checksum mismatch'
}
$pythonArchive = "$pythonZip.zip"
Move-Item $pythonZip $pythonArchive
Expand-Archive $pythonArchive $runtime
Get-ChildItem $runtime -Filter '*._pth' | ForEach-Object {
    Add-Content -LiteralPath $_.FullName -Value '../.github/ci' -Encoding ascii
}
Copy-Item "$PSScriptRoot/portable-benchmark.md" "$package/README.md"

$files = [ordered]@{}
Get-ChildItem $package -Recurse -File -Force | Sort-Object FullName | ForEach-Object {
    $path = [IO.Path]::GetRelativePath($package, $_.FullName).Replace('\', '/')
    $files[$path] = (Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()
}
@{
    schema = 1
    repository = $env:GITHUB_REPOSITORY
    branch = $env:GITHUB_REF_NAME
    revision = $env:GITHUB_SHA
    run_id = $env:GITHUB_RUN_ID
    run_attempt = $env:GITHUB_RUN_ATTEMPT
    renderers = @($env:BENCH_RENDERERS | ConvertFrom-Json)
    python_version = $pythonVersion
    files = $files
} | ConvertTo-Json -Depth 8 | Set-Content "$package/manifest.json" -Encoding utf8

Add-Type -AssemblyName System.IO.Compression.FileSystem
$name = "Oborodzuki-Benchmark-$env:GITHUB_SHA-windows-x64.zip"
$zip = Join-Path $dist $name
[IO.Compression.ZipFile]::CreateFromDirectory($package, $zip, 'Optimal', $false)
$check = (New-Item -ItemType Directory portable-check).FullName
[IO.Compression.ZipFile]::ExtractToDirectory($zip, $check)
& "$check/runtime/python.exe" -B "$check/.github/ci/portable_benchmark.py" --check-package
(Get-FileHash $zip).Hash.ToLowerInvariant() + "  $name" | Set-Content "$zip.sha256" -Encoding ascii
"Portable ZIP: $name" >> $env:GITHUB_STEP_SUMMARY

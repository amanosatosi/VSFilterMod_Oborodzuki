param([Parameter(Mandatory)][ValidatePattern('^[a-f0-9]{40}$')][string]$Revision)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
. "$PSScriptRoot\bundles.ps1"
$headers = (Resolve-Path benchmark-source\libass).Path
$bundle = (New-Item -ItemType Directory -Force bundle).FullName
$native = @('/nologo','/std:c++17','/EHsc','/O2','/MT',"/I$headers")
# Link the sampled harness's CPU timer, RGB hash and legacy compositor
# into this host. Its own entry point remains exclusive to Renderer Benchmarks.
cl @native /c /Dwmain=sampled_benchmark_main .github\ci\render-bench.cpp /Fo:sampled-benchmark.obj
cl @native .github\kfx\playback.cpp sampled-benchmark.obj "/Fe:$bundle\kfx-playback.exe"
@{
    component = 'harness'
    revision = $Revision
    headers_revision = git -C benchmark-source rev-parse HEAD
    platform = 'x64'
    configuration = 'MSVC /O2 /MT'
    options = $native
    instrumented = $false
    image = $env:ImageOS
    image_version = $env:ImageVersion
    compiler_tools = $env:VCToolsVersion
    windows_sdk = $env:WindowsSDKVersion
} | ConvertTo-Json -Depth 4 | Set-Content "$bundle\build.json"
$verified = Assert-KfxBundle $bundle 'harness' $Revision
Write-KfxSeal $verified $bundle @{ kind = 'this-run'; run_id = $env:GITHUB_RUN_ID }

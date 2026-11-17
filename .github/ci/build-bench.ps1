$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$root = (Get-Location).Path
$build = Join-Path $root 'benchmark-build'
$results = New-Item -ItemType Directory -Force 'benchmark-results'
$env:VCPKG_DISABLE_METRICS = '1'
$env:VCPKG_BINARY_SOURCES = "clear;files,$root\vcpkg-binaries,readwrite"
$null = New-Item -ItemType Directory -Force "$root\vcpkg-binaries"
$vcpkg = "$build\vcpkg"
$source = "$build\mangetsu"

foreach ($tree in @($root, "$build\original", "$build\baseline")) {
    Push-Location $tree
    try {
        msbuild VSFilterMod.sln /m /p:PlatformToolset=v143 "/p:WindowsTargetPlatformVersion=$($env:WindowsSDKVersion.TrimEnd('\'))" '/p:Configuration=Release (MOD)' /p:Platform=x64
    } finally {
        Pop-Location
    }
}

python -m pip install meson==1.8.2 ninja==1.11.1.4
& "$vcpkg\bootstrap-vcpkg.bat" -disableMetrics
$triplet = 'x64-windows-static'
& "$vcpkg\vcpkg.exe" install freetype fribidi harfbuzz pkgconf --triplet $triplet
$prefix = "$vcpkg\installed\$triplet"
$env:PKG_CONFIG = "$prefix\tools\pkgconf\pkgconf.exe"
$env:PKG_CONFIG_PATH = "$prefix\lib\pkgconfig;$prefix\share\pkgconfig"
$env:PKG_CONFIG_LIBDIR = $env:PKG_CONFIG_PATH
$includes = @('include','include\freetype2','include\fribidi','include\harfbuzz','include\libpng16') |
    ForEach-Object { '/I"' + (Join-Path $prefix $_) + '"' }
$mesonBuild = "$build\mangetsu-msvc"
$options = @('--buildtype=release', '-Ddefault_library=shared', '-Db_vscrt=mt',
    '-Dfontconfig=disabled', '-Ddirectwrite=enabled', '-Dlibunibreak=disabled', '-Dasm=enabled',
    '-Dtest=disabled', '-Dcompare=disabled', '-Dprofile=disabled', '-Dfuzz=disabled', '-Dcheckasm=disabled',
    "-Dc_args=$($includes -join ' ')", "-Dc_link_args=/LIBPATH:$prefix\lib")
python -m mesonbuild.mesonmain setup $mesonBuild $source @options
python -m mesonbuild.mesonmain compile -C $mesonBuild
$dll = @(Get-ChildItem $mesonBuild -Recurse -File -Filter '*.dll' | Where-Object Name -Match '^(ass|libass)([-.].+)?\.dll$')
if ($dll.Count -ne 1) { throw 'Expected one Mangetsu DLL' }
cl /nologo /std:c++17 /EHsc /O2 /MT "/I$source\libass" .github\ci\render-bench.cpp /Fe:render-bench.exe

@{
    image = $env:ImageOS
    image_version = $env:ImageVersion
    cpu = Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors
    ram_bytes = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
    original = git -C "$build\original" rev-parse HEAD
    baseline = git -C "$build\baseline" rev-parse HEAD
    candidate = git rev-parse HEAD
    mangetsu = git -C $source rev-parse HEAD
    vcpkg = git -C $vcpkg rev-parse HEAD
    compiler_tools = $env:VCToolsVersion
    windows_sdk = $env:WindowsSDKVersion
} | ConvertTo-Json -Depth 4 | Set-Content "$($results.FullName)\environment.json"

$original = "$build\original\bin\x64\VSFilter\Release (MOD)\VSFilterMod.dll"
$baseline = "$build\baseline\bin\x64\VSFilter\Release (MOD)\VSFilterMod.dll"
$candidate = "$root\bin\x64\VSFilter\Release (MOD)\VSFilterModOborodzuki.dll"
python .github\ci\run-bench.py $results.FullName $original $baseline $candidate $dll[0].FullName

# Profile a separate build after collecting the uninstrumented timings.
$env:_CL_ = "$env:_CL_ /DOBORODZUKI_PROFILE"
msbuild VSFilterMod.sln /t:Rebuild /m /p:PlatformToolset=v143 "/p:WindowsTargetPlatformVersion=$($env:WindowsSDKVersion.TrimEnd('\'))" '/p:Configuration=Release (MOD)' /p:Platform=x64
$profiles = [ordered]@{}
$stages = @('Frame bookkeeping and effects', 'Subtitle parsing and layout', 'Font lookup and text measurement',
    'Glyph path construction', 'Transform', 'Scan conversion', 'Outline widening',
    'Rasterization', 'Blur', 'Compositing')
foreach ($workload in @('idle', 'static', 'animation', 'seek', 'heavy')) {
    $fixture = switch ($workload) {
        { $_ -in 'idle', 'static' } { "$root\.github\ci\shared.ass" }
        { $_ -in 'animation', 'seek' } { "$($results.FullName)\animated.ass" }
        'heavy' { "$($results.FullName)\heavy.ass" }
    }
    $measurement = .\render-bench.exe csri $candidate $fixture $workload | ConvertFrom-Json
    if ($measurement.profile.Count -ne $stages.Count -or !$measurement.profile[0].calls) {
        throw "Missing render stage timings for $workload"
    }
    $profiles[$workload] = for ($i = 0; $i -lt $stages.Count; ++$i) {
        [pscustomobject]@{
            stage = $stages[$i]
            exclusive_ms = $measurement.profile[$i].ns / 1e6
            calls = $measurement.profile[$i].calls
        }
    }
}
$profiles | ConvertTo-Json -Depth 5 | Set-Content "$($results.FullName)\profile.json"
$lines = @('', '### Instrumented Oborodzuki profile', '',
    'Exclusive wall time across 600 warm frames. Timing scopes add overhead.', '',
    '| Workload | Stage | Exclusive ms | Calls |', '|---|---|---:|---:|')
foreach ($workload in $profiles.Keys) {
    foreach ($entry in ($profiles[$workload] | Sort-Object exclusive_ms -Descending)) {
        $lines += '| {0} | {1} | {2:F3} | {3} |' -f $workload, $entry.stage, $entry.exclusive_ms, $entry.calls
    }
}
$lines | Add-Content $env:GITHUB_STEP_SUMMARY
$lines | Set-Content "$($results.FullName)\profile.md"

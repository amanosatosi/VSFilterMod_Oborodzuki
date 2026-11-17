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

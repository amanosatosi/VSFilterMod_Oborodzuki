param(
    [Parameter(Mandatory)][ValidateSet('candidate','baseline','original','mangetsu','harness','profile')]
    [string]$Component,
    [Parameter(Mandatory)][string]$Revision
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$root = (Get-Location).Path
$bundle = New-Item -ItemType Directory -Force bundle
$source = Join-Path $root 'benchmark-source'
if ($Component -in 'candidate','profile') { $source = $root }
$configuration = 'Release (MOD)'
$options = @()

switch ($Component) {
    'mangetsu' {
        python -m pip install meson==1.8.2 ninja==1.11.1.4
        $vcpkg = Join-Path $root 'benchmark-vcpkg'
        $triplet = 'x64-windows-static'
        $env:VCPKG_DISABLE_METRICS = '1'
        $env:VCPKG_BINARY_SOURCES = "clear;files,$root\vcpkg-binaries,readwrite"
        $null = New-Item -ItemType Directory -Force vcpkg-binaries
        & "$vcpkg\bootstrap-vcpkg.bat" -disableMetrics
        & "$vcpkg\vcpkg.exe" install freetype fribidi harfbuzz pkgconf --triplet $triplet
        $prefix = "$vcpkg\installed\$triplet"
        $env:PKG_CONFIG = "$prefix\tools\pkgconf\pkgconf.exe"
        $env:PKG_CONFIG_PATH = "$prefix\lib\pkgconfig;$prefix\share\pkgconfig"
        $env:PKG_CONFIG_LIBDIR = $env:PKG_CONFIG_PATH
        $includes = @('include','include\freetype2','include\fribidi','include\harfbuzz','include\libpng16') |
            ForEach-Object { '/I"' + (Join-Path $prefix $_) + '"' }
        $mesonBuild = Join-Path $root 'benchmark-meson'
        $configuration = 'Meson release'
        $options = @('--buildtype=release', '-Ddefault_library=shared', '-Db_vscrt=mt',
            '-Dfontconfig=disabled', '-Ddirectwrite=enabled', '-Dlibunibreak=disabled', '-Dasm=enabled',
            '-Dtest=disabled', '-Dcompare=disabled', '-Dprofile=disabled', '-Dfuzz=disabled', '-Dcheckasm=disabled',
            "-Dc_args=$($includes -join ' ')", "-Dc_link_args=/LIBPATH:$prefix\lib")
        python -m mesonbuild.mesonmain setup $mesonBuild $source @options
        python -m mesonbuild.mesonmain compile -C $mesonBuild
        $dll = @(Get-ChildItem $mesonBuild -Recurse -File -Filter '*.dll' | Where-Object Name -Match '^(ass|libass)([-.].+)?\.dll$')
        if ($dll.Count -ne 1) { throw 'Expected one Mangetsu DLL' }
        Copy-Item $dll[0].FullName "$bundle\renderer.dll"
    }
    'harness' {
        $configuration = 'MSVC /O2 /MT'
        cl /nologo /std:c++17 /EHsc /O2 /MT "/I$source\libass" .github\ci\render-bench.cpp "/Fe:$bundle\render-bench.exe"
    }
    default {
        if ($Component -eq 'profile') {
            $env:_CL_ = "$env:_CL_ /DOBORODZUKI_PROFILE"
        }
        $options = @('/p:PlatformToolset=v143',
            "/p:WindowsTargetPlatformVersion=$($env:WindowsSDKVersion.TrimEnd('\'))",
            "/p:Configuration=$configuration", '/p:Platform=x64')
        Push-Location $source
        try {
            msbuild VSFilterMod.sln /m @options
        } finally {
            Pop-Location
        }
        $name = if ($Component -eq 'original' -or !(Test-Path "$source\bin\x64\VSFilter\Release (MOD)\VSFilterModOborodzuki.dll")) { 'VSFilterMod.dll' } else { 'VSFilterModOborodzuki.dll' }
        Copy-Item "$source\bin\x64\VSFilter\Release (MOD)\$name" "$bundle\renderer.dll"
    }
}

$actualRevision = git -C $source rev-parse HEAD
if ($Component -ne 'harness' -and $actualRevision -ne $Revision) {
    throw "Revision mismatch for $Component"
}
@{
    component = $Component
    revision = $Revision
    headers_revision = if ($Component -eq 'harness') { $actualRevision } else { $null }
    platform = 'x64'
    configuration = $configuration
    options = $options
    vcpkg_revision = if ($Component -eq 'mangetsu') { git -C $vcpkg rev-parse HEAD } else { $null }
    instrumented = $Component -eq 'profile'
    image = $env:ImageOS
    image_version = $env:ImageVersion
    compiler_tools = $env:VCToolsVersion
    windows_sdk = $env:WindowsSDKVersion
} | ConvertTo-Json -Depth 4 | Set-Content "$bundle\build.json"

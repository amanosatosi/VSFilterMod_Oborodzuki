function Assert-KfxPe([string]$Path, [bool]$IsDll) {
    $stream = [IO.File]::OpenRead($Path)
    $pe = [IO.BinaryReader]::new($stream)
    try {
        if ($pe.ReadUInt16() -ne 0x5a4d) { throw "Not a Windows executable: $Path" }
        $stream.Position = 0x3c
        $header = $pe.ReadUInt32()
        $stream.Position = $header
        if ($pe.ReadUInt32() -ne 0x4550 -or $pe.ReadUInt16() -ne 0x8664) {
            throw "Expected an x64 PE: $Path"
        }
        $stream.Position = $header + 22
        $flags = $pe.ReadUInt16()
        if ([bool]($flags -band 0x2000) -ne $IsDll -or $pe.ReadUInt16() -ne 0x20b) {
            throw "Unexpected PE type for $Path"
        }
    } finally {
        $pe.Dispose()
    }
}

function Assert-KfxBundle([string]$Directory, [string]$Kind, [string]$Revision) {
    $metadata = Get-Content -LiteralPath (Join-Path $Directory 'build.json') -Raw | ConvertFrom-Json
    $file = if ($Kind -eq 'harness') { 'kfx-playback.exe' } else { 'renderer.dll' }
    if ($metadata.component -ne $Kind -or $metadata.revision -cne $Revision -or
        $metadata.platform -ne 'x64' -or [bool]$metadata.instrumented -ne ($Kind -eq 'profile') -or
        !$metadata.compiler_tools -or !$metadata.windows_sdk) {
        throw "Invalid $Kind build identity for revision $Revision"
    }
    if ($Kind -eq 'mangetsu') {
        if ($metadata.configuration -ne 'Meson release' -or $metadata.vcpkg_revision -ne $env:VCPKG_REVISION) {
            throw 'Unexpected Mangetsu build configuration'
        }
        $options = @('--buildtype=release', '-Ddefault_library=shared', '-Db_vscrt=mt',
            '-Dfontconfig=disabled', '-Ddirectwrite=enabled', '-Dlibunibreak=disabled', '-Dasm=enabled')
    } elseif ($Kind -eq 'harness') {
        if ($metadata.configuration -ne 'MSVC /O2 /MT' -or
            $metadata.headers_revision -ne $env:MANGETSU_REVISION) { throw 'Unexpected harness toolchain or headers' }
        $options = @('/O2', '/MT')
    } else {
        if ($metadata.configuration -ne 'Release (MOD)') { throw "Unexpected $Kind build configuration" }
        $options = @('/p:PlatformToolset=v143', '/p:Platform=x64', '/p:Configuration=Release (MOD)')
    }
    foreach ($option in $options) {
        if ($option -notin $metadata.options) { throw "Missing build option $option for $Kind" }
    }
    $binary = Join-Path $Directory $file
    Assert-KfxPe $binary ($Kind -ne 'harness')
    $sha = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLowerInvariant()
    [pscustomobject]@{ path = (Resolve-Path $binary).Path; file = $file; sha256 = $sha; build = $metadata }
}

function Write-KfxSeal($Bundle, [string]$Directory, $Source) {
    @{
        sha256 = $Bundle.sha256
        build_sha256 = (Get-FileHash -LiteralPath (Join-Path $Directory 'build.json')).Hash.ToLowerInvariant()
        file = $Bundle.file
        source = $Source
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $Directory 'seal.json')
}

function Read-KfxBundle([string]$Directory, [string]$Kind, [string]$Revision) {
    $bundle = Assert-KfxBundle $Directory $Kind $Revision
    $seal = Get-Content -LiteralPath (Join-Path $Directory 'seal.json') -Raw | ConvertFrom-Json
    $buildHash = (Get-FileHash -LiteralPath (Join-Path $Directory 'build.json')).Hash.ToLowerInvariant()
    if ($seal.file -ne $bundle.file -or $seal.sha256 -cne $bundle.sha256 -or $seal.build_sha256 -cne $buildHash) {
        throw "Artifact SHA-256 mismatch for $Kind"
    }
    $bundle | Add-Member -NotePropertyName provenance -NotePropertyValue $seal.source
    $bundle
}

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$repo = 'amanosatosi/VSFilterMod_Oborodzuki'
$branch = 'astra/extra-optimization-JustInCase'
$api = "https://api.github.com/repos/$repo"
$headers = @{
    Accept = 'application/vnd.github+json'
    'X-GitHub-Api-Version' = '2022-11-28'
}
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Write-RunLog($message) {
    Write-Host $message
    Add-Content -LiteralPath "$results/benchmark.log" -Value $message -Encoding UTF8
}

function Set-GitHubToken($token) {
    if ($token) { $headers['Authorization'] = "Bearer $token" }
}

function Read-GitHub($suffix) {
    Invoke-RestMethod -Uri "$api/$suffix" -Headers $headers -UseBasicParsing -TimeoutSec 30
}

function Save-Artifact($artifact, $file) {
    Add-Type -AssemblyName System.Net.Http
    $handler = [Net.Http.HttpClientHandler]::new()
    $handler.AllowAutoRedirect = $false
    $client = [Net.Http.HttpClient]::new($handler)
    $client.Timeout = [TimeSpan]::FromSeconds(30)
    $client.DefaultRequestHeaders.UserAgent.ParseAdd('Oborodzuki-Portable-Benchmark/1')
    foreach ($key in $headers.Keys) {
        $null = $client.DefaultRequestHeaders.TryAddWithoutValidation($key, $headers[$key])
    }
    try {
        $response = $client.GetAsync("$api/actions/artifacts/$($artifact.id)/zip").GetAwaiter().GetResult()
        try {
            if ([int]$response.StatusCode -in 401,403,404) { return $false }
            if ([int]$response.StatusCode -ne 302 -or $response.Headers.Location.Scheme -ne 'https') {
                throw "GitHub artifact download returned HTTP $([int]$response.StatusCode)."
            }
            try {
                Invoke-WebRequest -Uri $response.Headers.Location -OutFile $file -UseBasicParsing -TimeoutSec 300
            } catch {
                throw 'The temporary artifact download failed. Run the launcher again to request a fresh link.'
            }
            return $true
        } finally { $response.Dispose() }
    } finally { $client.Dispose() }
}

function Get-Package {
    $token = $env:GH_TOKEN
    if (!$token) { $token = $env:GITHUB_TOKEN }
    if (!$token -and (Get-Command gh -ErrorAction SilentlyContinue)) {
        $token = & gh auth token --hostname github.com 2>$null
    }
    Set-GitHubToken $token
    $encodedBranch = [Uri]::EscapeDataString($branch)
    Write-RunLog "Finding the portable build for $branch..."
    $ref = Read-GitHub "git/ref/heads/$branch"
    $revision = $ref.object.sha
    if ($revision -cnotmatch '^[a-f0-9]{40}$') { throw 'GitHub did not return a valid branch revision.' }
    $runs = Read-GitHub "actions/workflows/benchmark.yml/runs?branch=$encodedBranch&head_sha=$revision&status=success&per_page=30"
    $name = "oborodzuki-benchmark-windows-x64-$revision"
    $artifact = $null
    foreach ($run in $runs.workflow_runs) {
        if ($run.head_sha -cne $revision -or $run.conclusion -ne 'success') { continue }
        $found = Read-GitHub "actions/runs/$($run.id)/artifacts?name=$name"
        $artifact = $found.artifacts | Where-Object { $_.name -ceq $name -and !$_.expired } | Select-Object -First 1
        if ($artifact) { break }
    }
    if (!$artifact) {
        throw "No successful, unexpired portable artifact exists for current branch HEAD $revision. Check https://github.com/$repo/actions/workflows/benchmark.yml and retry after packaging succeeds."
    }
    if ($artifact.digest -cnotmatch '^sha256:[a-f0-9]{64}$') { throw 'GitHub did not provide an artifact SHA-256.' }
    $outer = Join-Path $results 'github-artifact.zip'
    Write-RunLog "Downloading $name from Actions run $($run.id), artifact $($artifact.id)..."
    if (!(Save-Artifact $artifact $outer)) {
        if (![Environment]::UserInteractive -or [Console]::IsInputRedirected) {
            throw 'GitHub denied artifact download. Set GH_TOKEN to a GitHub token with Actions read access and retry.'
        }
        Write-Host "GitHub requires artifact access. Use a token with Actions read access to $repo, or sign into GitHub and download the ZIP from $($run.html_url)."
        $secret = Read-Host 'GitHub token (hidden, Enter to cancel)' -AsSecureString
        if (!$secret.Length) { throw 'Artifact download requires GitHub access.' }
        $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secret)
        try { Set-GitHubToken ([Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr)) }
        finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr); $secret.Dispose() }
        if (!(Save-Artifact $artifact $outer)) { throw 'GitHub denied the token access to this Actions artifact.' }
    }
    if ((Get-FileHash -LiteralPath $outer).Hash.ToLowerInvariant() -cne $artifact.digest.Substring(7)) {
        throw 'GitHub artifact SHA-256 mismatch.'
    }
    Write-RunLog 'Verifying and extracting the portable ZIP...'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $download = Join-Path $results 'download'
    [IO.Compression.ZipFile]::ExtractToDirectory($outer, $download)
    $zipName = "Oborodzuki-Benchmark-$revision-windows-x64.zip"
    $zip = Join-Path $download $zipName
    $checksum = Get-Content -LiteralPath "$zip.sha256" -Raw
    if ($checksum.Trim() -cne ((Get-FileHash -LiteralPath $zip).Hash.ToLowerInvariant() + "  $zipName")) {
        throw 'Portable ZIP SHA-256 mismatch.'
    }
    $destination = Join-Path $results 'package'
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $destination)
    $manifest = Get-Content -LiteralPath "$destination/manifest.json" -Raw | ConvertFrom-Json
    if ($manifest.revision -cne $revision -or "$($manifest.run_id)" -cne "$($run.id)") {
        throw 'Portable ZIP does not match the chosen Actions run.'
    }
    @{ url = $run.html_url; run_id = $run.id; artifact_id = $artifact.id; name = $name
       sha256 = $artifact.digest; portable_zip = $zipName } | ConvertTo-Json |
        Set-Content -LiteralPath "$results/artifact.json" -Encoding UTF8
    # The verified ZIP and unpacked binaries live in the temporary package
    # directory for this run. The provenance remains in the results.
    Remove-Item -LiteralPath $outer
    $destination
}

$exitCode = 1
$results = Join-Path $root ("Results\{0}_{1}" -f (Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'), [Guid]::NewGuid().ToString('N').Substring(0,8))
$mutex = [Threading.Mutex]::new($false, 'Local\OborodzukiPortableBenchmark')
$locked = $false
try {
    try { $locked = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (!$locked) { throw 'An Oborodzuki benchmark is already running in this session.' }
    $null = New-Item -ItemType Directory -Path $results
    if ($env:PROCESSOR_ARCHITECTURE -ne 'AMD64' -or $env:PROCESSOR_ARCHITEW6432 -eq 'ARM64') {
        throw 'This package requires native Windows x64.'
    }
    $cpu = @(Get-CimInstance Win32_Processor -ErrorAction SilentlyContinue |
        Select-Object Name,NumberOfCores,NumberOfLogicalProcessors)
    $registryCpu = (Get-ItemProperty 'HKLM:\HARDWARE\DESCRIPTION\System\CentralProcessor\0' -ErrorAction SilentlyContinue).ProcessorNameString
    @{ machine = $env:COMPUTERNAME; cpu = $cpu; cpu_registry = $registryCpu
       logical_processors = [Environment]::ProcessorCount; windows = [Environment]::OSVersion.VersionString
       architecture = $env:PROCESSOR_ARCHITECTURE; started_at = (Get-Date).ToString('o') } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$results/system-info.json" -Encoding UTF8
    Write-RunLog "Windows x64 on $env:COMPUTERNAME, $registryCpu. Detecting available worker counts..."
    $package = if (Test-Path -LiteralPath "$root/manifest.json" -PathType Leaf) {
        Write-RunLog 'Using the exact revision in this extracted portable ZIP.'
        $root
    } else { Get-Package }
    & "$package/runtime/python.exe" -B "$package/.github/ci/portable_benchmark.py" $results
    $exitCode = $LASTEXITCODE
    if ($exitCode) { Write-RunLog "Benchmark ended with exit code $exitCode. See the reports and log." }
} catch {
    if (Test-Path -LiteralPath $results) { Write-RunLog $_.Exception.Message }
    else { Write-Host $_.Exception.Message }
} finally {
    if ($locked) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
    if (Test-Path -LiteralPath $results -PathType Container) {
        Write-RunLog "Results folder: $results"
        try { Start-Process -FilePath "$env:WINDIR/explorer.exe" -ArgumentList ('"{0}"' -f $results) -WindowStyle Normal }
        catch { Write-RunLog "File Explorer could not be opened: $($_.Exception.Message)"; $exitCode = 1 }
    }
}
exit $exitCode

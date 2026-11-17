param(
    [Parameter(Mandatory)][ValidateSet('candidate','baseline','original','mangetsu')][string]$Kind,
    [Parameter(Mandatory)][ValidatePattern('^[a-f0-9]{40}$')][string]$Revision
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\bundles.ps1"
$endpoint = "$env:GITHUB_API_URL/repos/$env:GITHUB_REPOSITORY"
$headers = @{ Authorization = "Bearer $env:GH_TOKEN"; Accept = 'application/vnd.github+json' }
$validWorkflows = @('.github/workflows/benchmark.yml', '.github/workflows/kfx-stress-test.yml')

foreach ($name in @("kfx-renderer-$Kind-$Revision", "renderer-$Kind-$Revision")) {
    try {
        $artifacts = (Invoke-RestMethod "$endpoint/actions/artifacts?name=$name&per_page=20" -Headers $headers).artifacts
    } catch {
        Write-Warning "Could not look up artifacts for $Kind. Will build if no verified artifact is found."
        continue
    }
    foreach ($artifact in $artifacts) {
        if ($artifact.expired -or $artifact.digest -notmatch '^sha256:[a-f0-9]{64}$') { continue }
        if ($artifact.workflow_run.head_branch -ne 'develop/oborodzuki') { continue }
        $runId = $artifact.workflow_run.id
        try {
            $run = Invoke-RestMethod "$endpoint/actions/runs/$runId" -Headers $headers
            if ($run.path -notin $validWorkflows -or $run.conclusion -ne 'success') { continue }
            $archive = Join-Path $env:RUNNER_TEMP "$($artifact.id).zip"
            Invoke-WebRequest -Uri "$endpoint/actions/artifacts/$($artifact.id)/zip" -Headers $headers -OutFile $archive
            $digest = (Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant()
            if ("sha256:$digest" -cne $artifact.digest) { throw 'GitHub artifact ZIP digest mismatch' }

            $unpacked = Join-Path $env:RUNNER_TEMP ([guid]::NewGuid().ToString())
            $zip = [IO.Compression.ZipFile]::OpenRead($archive)
            try {
                $null = New-Item -ItemType Directory $unpacked
                foreach ($entry in $zip.Entries) {
                    if ($entry.FullName -notin 'renderer.dll','build.json','seal.json') {
                        throw "Unexpected file in renderer artifact: $($entry.FullName)"
                    }
                    [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $unpacked $entry.FullName))
                }
            } finally {
                $zip.Dispose()
            }
            $bundle = if ($name.StartsWith('kfx-')) {
                Read-KfxBundle $unpacked $Kind $Revision
            } else {
                Assert-KfxBundle $unpacked $Kind $Revision
            }
            if ($Kind -eq 'candidate' -and $run.head_sha -ne $Revision) { throw 'Candidate run revision mismatch' }
            $target = New-Item -ItemType Directory -Force bundle
            Copy-Item -LiteralPath $bundle.path, (Join-Path $unpacked 'build.json') -Destination $target.FullName
            Write-KfxSeal $bundle $target.FullName @{
                kind = 'github-artifact'
                run_id = $runId
                artifact_id = $artifact.id
                archive_sha256 = $digest
            }
            "reused=true" >> $env:GITHUB_OUTPUT
            Write-Host "Reused validated $Kind artifact $($artifact.id) from run $runId."
            exit 0
        } catch {
            Write-Warning "Rejected $Kind artifact $($artifact.id): $($_.Exception.Message)"
        }
    }
}
"reused=false" >> $env:GITHUB_OUTPUT

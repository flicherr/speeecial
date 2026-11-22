# Exercise the actual draft step with an in-memory GitHub API; no network calls.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$workflow = Get-Content -LiteralPath "$projectRoot/.github/workflows/release.yml" -Raw
$step = [regex]::Match($workflow, '(?ms)^      - name: Create or update draft and verify release assets\r?\n.*?^        run: \|\r?\n(?<code>.*)\z')
if (-not $step.Success) { throw 'Cannot find the release draft step.' }
$code = [regex]::Replace($step.Groups['code'].Value, '(?m)^ {10}', '')
$draftStep = [scriptblock]::Create($code)
$testRoot = Join-Path $projectRoot ('build/release-workflow-tests/' + [guid]::NewGuid().ToString('N'))

function Assert($condition, [string]$message) {
    if (-not $condition) { throw $message }
}

function Copy-Response($value) {
    return $value | ConvertTo-Json -Depth 20 | ConvertFrom-Json
}

function Throw-HttpError([int]$status) {
    $response = [System.Net.Http.HttpResponseMessage]::new([System.Net.HttpStatusCode]$status)
    throw [Microsoft.PowerShell.Commands.HttpResponseException]::new("HTTP $status", $response)
}

function Invoke-Case([hashtable]$case) {
    $directory = Join-Path $testRoot $case.Name
    New-Item -ItemType Directory -Force -Path "$directory/release", "$directory/notes" | Out-Null
    $env:GH_TOKEN = 'offline-test-token'
    $env:RELEASE_TAG = 'v1.0.13'
    $env:RELEASE_VERSION = '1.0.13'
    $env:RELEASE_COMMIT = '0123456789012345678901234567890123456789'
    $env:BUILT_VERSION = $env:RELEASE_VERSION
    $env:BUILT_COMMIT = $env:RELEASE_COMMIT
    $env:GITHUB_REPOSITORY = 'flicherr/speeecial'
    $env:GITHUB_API_URL = 'https://api.github.com'
    $env:GITHUB_SERVER_URL = 'https://github.com'
    $env:GITHUB_STEP_SUMMARY = "$directory/summary.md"
    $notes = "**English**`n`n- Update available.`n`n**Русский**`n`n- Доступно обновление."
    $noteFile = if ($case.EmptyNotes) { ' ' } else { $notes }
    [System.IO.File]::WriteAllText("$directory/notes/release-notes.md", $noteFile)
    [System.IO.File]::WriteAllBytes("$directory/release/speeecial-windows-x64.exe", [byte[]]::new(2048))
    [System.IO.File]::WriteAllBytes("$directory/release/speeecial-1.0.13-windows-x64.zip", [byte[]]::new(128))
    $pageRef = if ($case.Untagged) { 'untagged-a1b2c3' } else { 'v1.0.13' }
    $expectedAssets = @(Get-ChildItem -LiteralPath "$directory/release" | ForEach-Object {
        [pscustomobject]@{
            id = if ($_.Extension -eq '.exe') { 101 } else { 102 }
            name = $_.Name
            state = 'uploaded'
            size = $_.Length
            digest = 'sha256:' + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            browser_download_url = "https://github.com/flicherr/speeecial/releases/download/$pageRef/$($_.Name)"
        }
    })
    $state = @{
        Release = $null; Lists = 0; Reads = 0; Created = 0; Updated = 0
        Uploaded = 0; Deleted = 0; Attested = 0; Sleeps = 0
        NextAssetId = 200
    }
    $template = [pscustomobject]@{
        id = 777; tag_name = 'v1.0.13'; draft = $true; prerelease = $false
        name = 'speeecial 1.0.13'; body = $notes; assets = @()
        html_url = "https://github.com/flicherr/speeecial/releases/tag/$pageRef"
        upload_url = 'https://uploads.github.com/repos/flicherr/speeecial/releases/777/assets{?name,label}'
    }
    if ($case.Existing) {
        $state.Release = Copy-Response $template
        if (-not $case.CurrentMetadata) { $state.Release.name = 'Old title'; $state.Release.body = 'Old notes' }
        if ($case.Published) { $state.Release.draft = $false }
        if ($case.Prerelease) { $state.Release.prerelease = $true }
        switch ($case.Assets) {
            'all' { $state.Release.assets = @(Copy-Response $expectedAssets) }
            'exe' { $state.Release.assets = @(Copy-Response ($expectedAssets | Where-Object name -Like '*.exe')) }
            'stale' {
                $state.Release.assets = @(Copy-Response $expectedAssets)
                foreach ($asset in $state.Release.assets) { $asset.digest = 'sha256:old' }
            }
            'extra' { $state.Release.assets = @([pscustomobject]@{ id = 999; name = 'extra.txt' }) }
            'duplicate' {
                $exe = $expectedAssets | Where-Object name -Like '*.exe'
                $state.Release.assets = @((Copy-Response $exe), (Copy-Response $exe))
            }
        }
    }

    # These functions shadow every network operation in the workflow step.
    function gh {
        if ($args[0] -ceq 'api' -and $args[1] -ceq 'repos/flicherr/speeecial/commits/v1.0.13') {
            $global:LASTEXITCODE = 0
            if ($case.MovedTag) { return 'ffffffffffffffffffffffffffffffffffffffff' }
            return $env:RELEASE_COMMIT
        }
        if ($args[0] -ceq 'attestation' -and $args[1] -ceq 'verify') {
            $global:LASTEXITCODE = if ($case.BadAttestation) { 1 } else { 0 }
            if (-not $case.BadAttestation) { ++$state.Attested }
            return
        }
        throw "Unexpected gh invocation: $args"
    }

    function Start-Sleep { param($Seconds) ++$state.Sleeps }

    function Invoke-RestMethod {
        param([uri]$Uri, [string]$Method = 'Get', $Headers, $ContentType, $Body, $InFile)
        $path = $Uri.AbsolutePath
        if ($Method -eq 'Get' -and $path -eq '/repos/flicherr/speeecial/releases') {
            ++$state.Lists
            # A newly created release deliberately never appears in this list.
            if ($case.PageTwo -and $Uri.Query -match 'page=1$') {
                return @(1..100 | ForEach-Object { [pscustomobject]@{ id = 1000 + $_; tag_name = "v0.0.$_" } })
            }
            if ($case.DuplicateReleases) { return @($state.Release, $state.Release) }
            if ($case.Existing) { return @(Copy-Response $state.Release) }
            return @()
        }
        if ($Method -eq 'Get' -and $path -eq '/repos/flicherr/speeecial/releases/777') {
            ++$state.Reads
            if ($case.Read403) { Throw-HttpError 403 }
            if ($state.Reads -le $case.Read404) { Throw-HttpError 404 }
            if ($case.PublishAtRead -and $state.Reads -eq $case.PublishAtRead) { $state.Release.draft = $false }
            return Copy-Response $state.Release
        }
        Assert ($state.Attested -eq 2) 'Mutation before both attestations were verified.'
        Assert (-not $state.Release -or $state.Release.draft) 'Mutation of a published release.'
        if ($Method -eq 'Post' -and $path -eq '/repos/flicherr/speeecial/releases') {
            Assert (-not $state.Release) 'A second draft was created.'
            $payload = $Body | ConvertFrom-Json
            Assert ($payload.tag_name -ceq 'v1.0.13' -and $payload.draft -and -not $payload.prerelease) 'Wrong release creation payload.'
            Assert ($payload.target_commitish -ceq $env:RELEASE_COMMIT) 'Wrong source commit in the release creation payload.'
            Assert ($payload.name -ceq $template.name -and $payload.body -ceq $notes) 'Wrong release metadata.'
            ++$state.Created
            $state.Release = Copy-Response $template
            return Copy-Response $state.Release
        }
        if ($Method -eq 'Patch' -and $path -eq '/repos/flicherr/speeecial/releases/777') {
            $payload = $Body | ConvertFrom-Json
            Assert (@($payload.PSObject.Properties.Name).Count -eq 2 -and $payload.name -and $payload.body) 'PATCH must contain only name and body.'
            ++$state.Updated
            $state.Release.name = $payload.name
            $state.Release.body = $payload.body
            return Copy-Response $state.Release
        }
        if ($Method -eq 'Delete' -and $path -match '^/repos/flicherr/speeecial/releases/assets/(\d+)$') {
            $assetId = [int]$Matches[1]
            Assert (@($state.Release.assets | Where-Object id -EQ $assetId).Count -eq 1) 'Deleting an unknown asset.'
            $state.Release.assets = @($state.Release.assets | Where-Object id -NE $assetId)
            ++$state.Deleted
            return
        }
        if ($Method -eq 'Post' -and $path -eq '/repos/flicherr/speeecial/releases/777/assets') {
            Assert ($Uri.Host -ceq 'uploads.github.com') 'Wrong upload host.'
            $name = [uri]::UnescapeDataString(($Uri.Query -split '=', 2)[1])
            Assert (@($state.Release.assets | Where-Object name -CEQ $name).Count -eq 0) 'Upload did not replace the old asset first.'
            if ($case.FailZip -and $name.EndsWith('.zip')) { Throw-HttpError 502 }
            $asset = Copy-Response ($expectedAssets | Where-Object name -CEQ $name)
            Assert ($asset -and (Get-FileHash -LiteralPath $InFile -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $asset.digest.Substring(7)) 'Uploaded bytes differ from the attested file.'
            $asset.id = ++$state.NextAssetId
            if ($case.BadDigest) { $asset.digest = 'sha256:wrong' }
            if ($case.BadUrl) { $asset.browser_download_url = 'https://example.invalid/file.exe' }
            $state.Release.assets += $asset
            ++$state.Uploaded
            return Copy-Response $asset
        }
        throw "Unexpected API request: $Method $Uri"
    }

    $problem = $null
    Push-Location -LiteralPath $directory
    try { & $draftStep } catch { $problem = $_.Exception.Message } finally { Pop-Location }
    if ($case.Error) {
        Assert ($problem -and $problem.Contains($case.Error)) "Wrong failure for $($case.Name): $problem"
        Assert (-not (Test-Path -LiteralPath $env:GITHUB_STEP_SUMMARY)) 'A failed run reported a verified draft.'
    } else {
        Assert (-not $problem) "Unexpected failure for $($case.Name): $problem"
        Assert ($state.Release.id -eq 777 -and $state.Release.draft) 'Draft identity or state changed.'
        Assert ($state.Release.name -ceq $template.name -and $state.Release.body -ceq $notes) 'Draft metadata was not refreshed.'
        Assert (@($state.Release.assets).Count -eq 2) 'Draft assets are incomplete.'
        Assert (Test-Path -LiteralPath $env:GITHUB_STEP_SUMMARY) 'Successful run has no summary.'
    }
    foreach ($counter in @('Created', 'Updated', 'Uploaded', 'Deleted', 'Reads', 'Lists')) {
        if ($case.ContainsKey($counter)) { Assert ($state[$counter] -eq $case[$counter]) "Wrong $counter count for $($case.Name): $($state[$counter])" }
    }
    Write-Output "PASS: $($case.Name)"
}

$cases = @(
    @{ Name = 'create-despite-stale-list-and-two-404s'; Read404 = 2; Lists = 1; Created = 1; Uploaded = 2 }
    @{ Name = 'refresh-existing-draft-metadata'; Existing = $true; Created = 0; Updated = 1; Uploaded = 2 }
    @{ Name = 'reuse-identical-files'; Existing = $true; CurrentMetadata = $true; Assets = 'all'; Created = 0; Updated = 0; Uploaded = 0; Deleted = 0 }
    @{ Name = 'resume-partial-upload'; Existing = $true; CurrentMetadata = $true; Assets = 'exe'; Uploaded = 1; Deleted = 0 }
    @{ Name = 'replace-different-files'; Existing = $true; CurrentMetadata = $true; Assets = 'stale'; Created = 0; Uploaded = 2; Deleted = 2 }
    @{ Name = 'accept-untagged-draft-urls'; Untagged = $true; Created = 1; Uploaded = 2 }
    @{ Name = 'find-existing-draft-on-second-page'; Existing = $true; PageTwo = $true; Assets = 'all'; Lists = 2; Created = 0; Updated = 1; Uploaded = 0 }
    @{ Name = 'reject-published-release'; Existing = $true; Published = $true; Error = 'already published'; Created = 0; Updated = 0; Uploaded = 0; Deleted = 0 }
    @{ Name = 'reject-prerelease'; Existing = $true; Prerelease = $true; Error = 'not a prerelease'; Updated = 0; Uploaded = 0 }
    @{ Name = 'reject-unexpected-assets'; Existing = $true; Assets = 'extra'; Error = 'unexpected assets'; Updated = 0; Uploaded = 0; Deleted = 0 }
    @{ Name = 'reject-duplicate-releases'; Existing = $true; DuplicateReleases = $true; Error = 'Multiple releases'; Created = 0; Updated = 0; Uploaded = 0 }
    @{ Name = 'reject-duplicate-assets'; Existing = $true; CurrentMetadata = $true; Assets = 'duplicate'; Error = 'Duplicate draft asset'; Uploaded = 0; Deleted = 0 }
    @{ Name = 'report-persistent-404'; Read404 = 100; Error = 'Cannot read release 777 after 6 attempts'; Created = 1; Reads = 6; Uploaded = 0 }
    @{ Name = 'fail-immediately-on-forbidden-read'; Existing = $true; Read403 = $true; Error = 'Cannot read release 777: HTTP 403'; Reads = 1; Updated = 0; Uploaded = 0 }
    @{ Name = 'stop-after-concurrent-publication'; PublishAtRead = 2; Error = 'already published'; Created = 1; Updated = 0; Uploaded = 0; Deleted = 0 }
    @{ Name = 'reject-moved-tag'; MovedTag = $true; Error = 'release tag moved'; Created = 0; Updated = 0; Uploaded = 0; Lists = 0 }
    @{ Name = 'reject-unverified-build'; BadAttestation = $true; Error = 'Attestation verification failed'; Created = 0; Updated = 0; Uploaded = 0; Lists = 0 }
    @{ Name = 'leave-partial-draft-after-upload-error'; FailZip = $true; Error = 'Cannot upload speeecial-1.0.13-windows-x64.zip'; Created = 1; Uploaded = 1 }
    @{ Name = 'report-digest-mismatch'; BadDigest = $true; Error = 'verification failed'; Created = 1; Uploaded = 2 }
    @{ Name = 'reject-unexpected-download-url'; BadUrl = $true; Error = 'verification failed'; Created = 1; Uploaded = 2 }
    @{ Name = 'reject-empty-notes'; Existing = $true; EmptyNotes = $true; Error = 'Release notes are empty'; Created = 0; Updated = 0; Uploaded = 0 }
)
$environment = @{}
foreach ($name in @('GH_TOKEN', 'RELEASE_TAG', 'RELEASE_VERSION', 'RELEASE_COMMIT', 'BUILT_VERSION', 'BUILT_COMMIT', 'GITHUB_REPOSITORY', 'GITHUB_API_URL', 'GITHUB_SERVER_URL', 'GITHUB_STEP_SUMMARY')) {
    $environment[$name] = [System.Environment]::GetEnvironmentVariable($name)
}
try {
    foreach ($case in $cases) { Invoke-Case $case }
} finally {
    foreach ($name in $environment.Keys) {
        [System.Environment]::SetEnvironmentVariable($name, $environment[$name])
    }
}
Write-Output "$($cases.Count) release workflow scenarios passed."

param(
    [Parameter(Mandatory)][string]$PackageDirectory,
    [Parameter(Mandatory)][string]$CabPath,
    [string]$WdkRoot = (Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Windows Kits\10'),
    [ValidatePattern('^\d+\.\d+\.\d+\.\d+$')][string]$WdkVersion = '10.0.28000.0',
    [ValidatePattern('^[A-Za-z0-9]{1,39}$')][string]$CabFolder = 'Cableloom'
)
$ErrorActionPreference = 'Stop'
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
$cab = (Resolve-Path -LiteralPath $CabPath).Path
$infverif = Join-Path $WdkRoot "Tools\$WdkVersion\x64\InfVerif.exe"
if (-not (Test-Path -LiteralPath $infverif -PathType Leaf)) {
    throw "InfVerif is missing: $infverif"
}
$required = 'cableloom.inf','cableloom.sys','cableloom.pdb','cableloom.cat'
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $package $name) -PathType Leaf)) {
        throw "Submission input is missing: $name"
    }
}
foreach ($mode in '/w','/u','/h') {
    & $infverif $mode /v (Join-Path $package 'cableloom.inf')
    if ($LASTEXITCODE -ne 0) { throw "InfVerif failed: $mode" }
}
$scratchRoot = [IO.Path]::GetFullPath((Split-Path -Parent $cab))
$scratch = Join-Path $scratchRoot ("cableloom-attestation-" + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $scratch
try {
    & expand.exe -F:* $cab $scratch | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'CAB extraction failed.' }
    $actual = @(Get-ChildItem -LiteralPath $scratch -File -Recurse | ForEach-Object {
        [IO.Path]::GetRelativePath($scratch, $_.FullName)
    } | Sort-Object)
    $expected = @($required | ForEach-Object { Join-Path $CabFolder $_ } | Sort-Object)
    if (Compare-Object -ReferenceObject $expected -DifferenceObject $actual) {
        throw 'CAB layout differs from the required single-folder four-file package.'
    }
    $files = foreach ($name in $required) {
        $source = Join-Path $package $name
        $expanded = Join-Path (Join-Path $scratch $CabFolder) $name
        $sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash
        $expandedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $expanded).Hash
        if ($sourceHash -ne $expandedHash) {
            throw "CAB content hash mismatch: $name"
        }
        [ordered]@{ name = $name; sha256 = $sourceHash }
    }
    [ordered]@{
        schema = 1
        status = 'ready-for-ev-signing-and-partner-center-submission'
        wdk_version = $WdkVersion
        cab_folder = $CabFolder
        cab_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $cab).Hash
        cab_signature_status = (Get-AuthenticodeSignature -LiteralPath $cab).Status.ToString()
        files = $files
    } | ConvertTo-Json -Depth 4
} finally {
    $resolvedScratch = [IO.Path]::GetFullPath($scratch)
    if ($resolvedScratch.StartsWith($scratchRoot, [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolvedScratch).StartsWith('cableloom-attestation-', [StringComparison]::Ordinal)) {
        Remove-Item -LiteralPath $resolvedScratch -Recurse -Force -ErrorAction SilentlyContinue
    }
}

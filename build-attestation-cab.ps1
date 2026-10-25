param(
    [Parameter(Mandatory)][string]$PackageDirectory,
    [string]$Output = (Join-Path $PSScriptRoot 'build\attestation')
)
$ErrorActionPreference = 'Stop'
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
$required = 'cableloom.inf','cableloom.sys','cableloom.pdb','cableloom.cat'
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $package $name) -PathType Leaf)) {
        throw "Attestation input is missing: $name"
    }
}
if (Test-Path -LiteralPath $Output) {
    throw "Output already exists; choose a new empty path: $Output"
}
$null = New-Item -ItemType Directory -Path $Output
$outputPath = (Resolve-Path -LiteralPath $Output).Path
$ddf = Join-Path $outputPath 'Cableloom.ddf'
$cab = Join-Path $outputPath 'Cableloom-attestation.cab'
$lines = @(
    '.OPTION EXPLICIT',
    '.Set CabinetFileCountThreshold=0',
    '.Set FolderFileCountThreshold=0',
    '.Set FolderSizeThreshold=0',
    '.Set MaxCabinetSize=0',
    '.Set MaxDiskFileCount=0',
    '.Set MaxDiskSize=0',
    '.Set CompressionType=MSZIP',
    '.Set Cabinet=on',
    '.Set Compress=on',
    '.Set CabinetNameTemplate=Cableloom-attestation.cab',
    '.Set DiskDirectoryTemplate=.',
    '.Set InfFileName=NUL',
    '.Set RptFileName=NUL',
    '.Set DestinationDir=Cableloom'
)
foreach ($name in $required) {
    $source = Join-Path $package $name
    $lines += '"' + $source + '" ' + $name
}
[IO.File]::WriteAllLines($ddf, $lines, [Text.UTF8Encoding]::new($false))
Push-Location $outputPath
try {
    & makecab.exe /F $ddf
    if ($LASTEXITCODE -ne 0) { throw 'MakeCab failed.' }
} finally {
    Pop-Location
}
if (-not (Test-Path -LiteralPath $cab -PathType Leaf)) {
    throw "MakeCab did not create the expected file: $cab"
}
Write-Output "Unsigned Partner Center submission CAB: $cab"
Write-Output 'Sign this CAB with a Partner Center-registered EV certificate using SHA-256, then submit it for Microsoft attestation signing.'

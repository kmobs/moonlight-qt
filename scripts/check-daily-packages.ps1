param([Parameter(Mandatory=$true)][string]$CiVersion)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/windows-product-version.ps1"
$expected = & "$PSScriptRoot/vrr-pe-version.ps1" -CiVersion $CiVersion
if ($LASTEXITCODE -ne 0 -or !$expected) { throw 'Missing daily product version' }
$installer = New-Object -ComObject WindowsInstaller.Installer
foreach ($arch in @('x64', 'arm64')) {
    $exe = (Resolve-Path "build/build-$arch-release/app/release/Moonlight.exe").Path
    $version = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($exe).FileVersion
    Assert-WindowsProductVersion -Actual $version -Expected $expected -Label "$arch PE"
    $msi = (Resolve-Path "build/build-$arch-release/Moonlight.msi").Path
    $database = $installer.OpenDatabase($msi, 0)
    $view = $database.OpenView("SELECT ``Value`` FROM ``Property`` WHERE ``Property`` = 'ProductVersion'")
    $view.Execute()
    $record = $view.Fetch()
    if (!$record) { throw "$arch MSI is missing ProductVersion" }
    $msiVersion = $record.StringData(1)
    Assert-WindowsProductVersion -Actual $msiVersion -Expected $expected -Label "$arch MSI"
    Write-Host "$arch MSI ProductVersion $msiVersion (expected $expected)"
    $view.Close()
}
$bundle = (Resolve-Path "build/installer-release/MoonlightSetup-$CiVersion.exe").Path
$bundleVersion = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($bundle).FileVersion
Assert-WindowsProductVersion -Actual $bundleVersion -Expected $expected -Label 'Bundle'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path "build/installer-x64-release/MoonlightPortable-x64-$CiVersion.zip"))
try {
    $names = @($zip.Entries.FullName)
    foreach ($file in @('Moonlight.exe', 'vrrreplay.exe', 'decode-vrr-trace.py', 'portable.dat.inactive')) {
        if ($names -notcontains $file) { throw "Portable ZIP is missing $file" }
    }
    if ($names -contains 'portable.dat') { throw 'Portable ZIP has active settings marker' }
} finally { $zip.Dispose() }
Write-Host "Daily PE, MSI, Burn and portable package gates passed: $expected"

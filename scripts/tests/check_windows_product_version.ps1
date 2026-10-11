param([Parameter(Mandatory=$true)][string]$FixtureDirectory)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../windows-product-version.ps1"
$installer = New-Object -ComObject WindowsInstaller.Installer
$expected = '6.2.181'
$cases = @(
    @{Actual='6.2.181'; Pass=$true},
    @{Actual='6.2.181.0'; Pass=$true},
    @{Actual='6.2.182.0'; Pass=$false},
    @{Actual='6.2.18.0'; Pass=$false},
    @{Actual='6.3.181.0'; Pass=$false},
    @{Actual='6.2.181.1'; Pass=$false},
    @{Actual='6.2.181.0.extra'; Pass=$false},
    @{Actual=''; Pass=$false}
)
$i = 0
foreach ($case in $cases) {
    $path = Join-Path $FixtureDirectory "version-$i.msi"
    $database = $installer.OpenDatabase($path, 3)
    $create = $database.OpenView('CREATE TABLE `Property` (`Property` CHAR(72) NOT NULL, `Value` CHAR(0) LOCALIZABLE PRIMARY KEY `Property`)')
    $create.Execute()
    $create.Close()
    [void][System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($create)
    if ($case.Actual) {
        $insert = $database.OpenView("INSERT INTO ``Property`` (``Property``, ``Value``) VALUES ('ProductVersion', '$($case.Actual)')")
        $insert.Execute()
        $insert.Close()
        [void][System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($insert)
    }
    $database.Commit()
    [void][System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($database)
    $database = $null
    # Read-only open only. No install, package action, custom action or GPU use.
    $readOnly = $installer.OpenDatabase($path, 0)
    $query = $readOnly.OpenView("SELECT ``Value`` FROM ``Property`` WHERE ``Property`` = 'ProductVersion'")
    $query.Execute()
    $record = $query.Fetch()
    $value = if ($record) { $record.StringData(1) } else { '' }
    if ($value -eq '6.2.181.0' -and [version]$value -eq [version]$expected) {
        throw 'Fixture did not reproduce the original System.Version mismatch'
    }
    $accepted = $true
    try {
        if (!$record) { throw 'Missing ProductVersion' }
        Assert-WindowsProductVersion -Actual $value -Expected $expected -Label "Fixture $i MSI"
    } catch { $accepted = $false }
    if ($accepted -ne $case.Pass) { throw "Unexpected result for '$value': accepted=$accepted expected=$($case.Pass)" }
    Write-Host "Verified MSI ProductVersion '$value': accepted=$accepted"
    $query.Close()
    [void][System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($query)
    if ($record) { [void][System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($record) }
    [void][System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($readOnly)
    $query = $null
    $record = $null
    $readOnly = $null
    $i++
}

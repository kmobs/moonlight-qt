function Assert-WindowsProductVersion {
    param(
        [Parameter(Mandatory=$true)][string]$Actual,
        [Parameter(Mandatory=$true)][string]$Expected,
        [Parameter(Mandatory=$true)][string]$Label
    )
    # PE-bound WiX versions include a zero fourth field. MSI orders the first
    # three fields, while System.Version distinguishes missing revision (-1)
    # from explicit zero. Permit only that equivalent representation.
    if ($Expected -notmatch '^\d+\.\d+\.\d+$' -or
        $Actual -notmatch '^\d+\.\d+\.\d+(\.0)?$') {
        throw "$Label version '$Actual' is not the expected three-field product version '$Expected' with optional zero revision"
    }
    $actualVersion = [version]$Actual
    $expectedVersion = [version]$Expected
    if ($actualVersion.Major -ne $expectedVersion.Major -or
        $actualVersion.Minor -ne $expectedVersion.Minor -or
        $actualVersion.Build -ne $expectedVersion.Build) {
        throw "$Label version '$Actual' does not match '$Expected'"
    }
}

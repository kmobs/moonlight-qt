# Map a GitHub CI_VERSION such as 6.1.0-vrr17 or 6.1.0-vrr17.1 to the
# three-field Windows PE/MSI ProductVersion. MSI compares only major.minor.build,
# so patch tags cannot reuse 6.2.N.
#
#   6.1.0-vrrN     (N <= 17)  ->  6.2.N
#   6.1.0-vrrN.P              ->  6.2.(N * 10 + P)   # vrr17.1 -> 6.2.171
#   6.1.0-vrrN     (N >= 18)  ->  6.2.(N * 10)       # vrr18   -> 6.2.180
param(
    [Parameter(Mandatory = $true)]
    [string]$CiVersion
)

# Daily versions share the existing 6.2 MSI sequence. The allocator chooses
# a build higher than every previously tagged VRR and daily version.
if ($CiVersion -match '^6\.[12]\.0-daily\.([1-9][0-9]*)$') {
    $build = [long]$Matches[1]
    if ($build -lt 1 -or $build -gt 65535) {
        Write-Error "Daily Windows build must be between 1 and 65535"
        exit 1
    }
    Write-Output "6.2.$build"
    exit 0
}

if ($CiVersion -match '-vrr(\d+)\.(\d+)$') {
    Write-Output ('6.2.' + ([int]$Matches[1] * 10 + [int]$Matches[2]))
    exit 0
}

if ($CiVersion -match '-vrr(\d+)$') {
    $n = [int]$Matches[1]
    if ($n -le 17) {
        Write-Output "6.2.$n"
    } else {
        Write-Output ('6.2.' + ($n * 10))
    }
    exit 0
}

Write-Error "Cannot map CI_VERSION '$CiVersion' to a Windows PE/MSI version"
exit 1

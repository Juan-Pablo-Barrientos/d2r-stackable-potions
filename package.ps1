# Builds dist\StackablePotions.zip. The DLL and INSTALL.txt sit at the top of
# the zip, next to the StackablePotions mod folder, because each part is
# installed in a different place.
param(
	[string]$Dll
)
$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
if (-not $Dll) {
	& "$root\build.ps1"
	$Dll = Join-Path $root "build\d2rl-stackable-potions.dll"
}

$dist  = Join-Path $root "dist"
$stage = Join-Path $dist "stage"
Remove-Item -Recurse -Force $dist -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\StackablePotions" | Out-Null

Copy-Item "$root\StackablePotions\*" "$stage\StackablePotions"
Copy-Item $Dll $stage
Copy-Item "$root\INSTALL.txt" $stage
Compress-Archive -Path "$stage\*" -DestinationPath "$dist\StackablePotions.zip"
Remove-Item -Recurse -Force $stage
Write-Host "Built $dist\StackablePotions.zip"

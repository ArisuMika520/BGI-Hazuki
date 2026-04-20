param(
    [string]$OutputDir = "dist"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$rootPath = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$binDir = Join-Path $rootPath "bin"
$guiPath = Join-Path $binDir "BGI_Hazuki_GUI.exe"
$corePath = Join-Path $binDir "BGI_Hazuki_Core.dll"

foreach ($path in @($guiPath, $corePath)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Missing required build output: $path"
    }
}

if (-not ("HazukiNativeResource" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class HazukiNativeResource
{
    public const uint LOAD_LIBRARY_AS_DATAFILE = 0x00000002;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr LoadLibraryEx(string lpFileName, IntPtr hFile, uint dwFlags);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "FindResourceW")]
    public static extern IntPtr FindResource(IntPtr hModule, IntPtr lpName, IntPtr lpType);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint SizeofResource(IntPtr hModule, IntPtr hResInfo);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool FreeLibrary(IntPtr hModule);
}
"@
}

function Test-GuiResources {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $module = [HazukiNativeResource]::LoadLibraryEx($Path, [IntPtr]::Zero, [HazukiNativeResource]::LOAD_LIBRARY_AS_DATAFILE)
    if ($module -eq [IntPtr]::Zero) {
        throw "Failed to inspect GUI resources: $Path"
    }

    try {
        $checks = @(
            @{ Id = 101; Type = [IntPtr]10; Name = "background" },
            @{ Id = 102; Type = [IntPtr]10; Name = "icon-png" },
            @{ Id = 1;   Type = [IntPtr]14; Name = "app-icon" }
        )

        foreach ($check in $checks) {
            $res = [HazukiNativeResource]::FindResource($module, [IntPtr]$check.Id, $check.Type)
            $size = if ($res -ne [IntPtr]::Zero) { [HazukiNativeResource]::SizeofResource($module, $res) } else { 0 }
            [pscustomobject]@{
                Id = $check.Id
                Name = $check.Name
                Found = ($res -ne [IntPtr]::Zero)
                Size = $size
            }
        }
    }
    finally {
        [HazukiNativeResource]::FreeLibrary($module) | Out-Null
    }
}

$resourceState = @(Test-GuiResources -Path $guiPath)
$missing = @($resourceState | Where-Object { -not $_.Found })
if ($missing.Count -gt 0) {
    $names = ($missing | ForEach-Object { "$($_.Name)[$($_.Id)]" }) -join ", "
    throw "GUI build is missing embedded resources: $names"
}

$resolvedOutputDir = if ([System.IO.Path]::IsPathRooted($OutputDir)) {
    [System.IO.Path]::GetFullPath($OutputDir)
}
else {
    [System.IO.Path]::GetFullPath((Join-Path $rootPath $OutputDir))
}

$stageDir = Join-Path $resolvedOutputDir "BGI_Hazuki_GUI_package"
$zipPath = Join-Path $resolvedOutputDir "BGI_Hazuki_GUI_package.zip"
$hashPath = Join-Path $resolvedOutputDir "BGI_Hazuki_GUI_package.sha256.txt"

New-Item -ItemType Directory -Path $resolvedOutputDir -Force | Out-Null
if (Test-Path -LiteralPath $stageDir) {
    Remove-Item -LiteralPath $stageDir -Recurse -Force
}
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}

New-Item -ItemType Directory -Path $stageDir | Out-Null
Copy-Item -LiteralPath $guiPath -Destination (Join-Path $stageDir "BGI_Hazuki_GUI.exe") -Force
Copy-Item -LiteralPath $corePath -Destination (Join-Path $stageDir "BGI_Hazuki_Core.dll") -Force

Compress-Archive -LiteralPath @(
    (Join-Path $stageDir "BGI_Hazuki_GUI.exe"),
    (Join-Path $stageDir "BGI_Hazuki_Core.dll")
) -DestinationPath $zipPath -CompressionLevel Optimal

$hashLines = @(
    (Get-FileHash -Algorithm SHA256 -LiteralPath $guiPath),
    (Get-FileHash -Algorithm SHA256 -LiteralPath $corePath),
    (Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath)
) | ForEach-Object {
    "{0}  {1}" -f $_.Hash, [System.IO.Path]::GetFileName($_.Path)
}

Set-Content -LiteralPath $hashPath -Value $hashLines -Encoding ASCII

Write-Host "Packaged GUI files from:" $binDir
Write-Host "Created zip:" $zipPath
Write-Host "Created hashes:" $hashPath
Write-Host "GUI size:" (Get-Item -LiteralPath $guiPath).Length
Write-Host "GUI resources:" (($resourceState | ForEach-Object { "$($_.Name)=$($_.Found)" }) -join ", ")

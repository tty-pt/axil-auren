# Installs axil-auren as a user logon task or Windows service
param(
    [string]$Prefix = "$HOME\.local",
    [switch]$AsService
)

$AxilBin = Join-Path $Prefix "bin\axil.exe"
if (-not (Test-Path $AxilBin)) {
    $AxilBin = (Get-Command axil.exe -ErrorAction SilentlyContinue)?.Source
}
$AurenDll = Join-Path $Prefix "bin\libaxil-auren.dll"

if ($AsService) {
    Write-Host "Registering axil-auren Windows Service..."
    New-Service -Name "axil-auren" -BinaryPathName "`"$AxilBin`" -A -d -p 4242 -m `"$AurenDll`"" -DisplayName "Axil Auren Neural Inference Server" -StartupType Automatic
    Start-Service -Name "axil-auren"
} else {
    Write-Host "Registering axil-auren Scheduled Task at logon..."
    $XmlTemplate = Get-Content (Join-Path $PSScriptRoot "axil-auren.xml") -Raw
    $XmlContent = $XmlTemplate.Replace("@PREFIX@", $Prefix)
    $XmlPath = [System.IO.Path]::GetTempFileName() + ".xml"
    [System.IO.File]::WriteAllText($XmlPath, $XmlContent)
    schtasks.exe /Create /TN "Axil\axil-auren" /XML $XmlPath /F
    Remove-Item $XmlPath
    schtasks.exe /Run /TN "Axil\axil-auren"
}
Write-Host "axil-auren service registered successfully."

# Dev cycle: close Echo, install the freshly built loader and runtime, relaunch, show the runtime log.
#   powershell -File tools\dev-run.ps1 [-Seconds 15] [-Version 0.2.0]
param([int]$Seconds = 15, [string]$Version = "0.2.0",
      [string]$Game = "H:\EVR\ready-at-dawn-echo-arena\bin\win10")
$repo = Split-Path $PSScriptRoot -Parent
Get-Process echovr -ErrorAction SilentlyContinue | Stop-Process -Force
while (Get-Process echovr -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 300 }
Copy-Item "$repo\target\release\hid.dll" "$Game\hid.dll" -Force
New-Item -ItemType Directory -Force "$Game\plugins\versions\$Version" | Out-Null
Copy-Item "$repo\runtime\build\evr-runtime.dll" "$Game\plugins\versions\$Version\evr-runtime.dll" -Force
Remove-Item "$Game\plugins\evr-runtime.log" -ErrorAction SilentlyContinue
Start-Process -FilePath "$Game\echovr.exe" -ArgumentList '-noovr', '-windowed' -WorkingDirectory $Game
Start-Sleep $Seconds
Get-Content "$Game\plugins\evr-runtime.log" -ErrorAction SilentlyContinue

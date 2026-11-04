# Puts a "Lewd Vault" shortcut on your desktop that starts dist\Image Vault\Lewd Vault.exe
# (with the app's own icon). Run by `npm run shortcut`; running it again just updates it.
$exe = Join-Path $PSScriptRoot "..\dist\Image Vault\Lewd Vault.exe" | Resolve-Path -ErrorAction Stop
$desktop = [Environment]::GetFolderPath("Desktop")
$link = Join-Path $desktop "Lewd Vault.lnk"
$shell = New-Object -ComObject WScript.Shell
$s = $shell.CreateShortcut($link)
$s.TargetPath = "$exe"
$s.WorkingDirectory = Split-Path "$exe"
$s.IconLocation = "$exe,0"
$s.Description = "Lewd Vault"
$s.Save()
# the shortcut under the app's old name
Remove-Item (Join-Path $desktop "Image Vault.lnk") -ErrorAction SilentlyContinue
"Shortcut: $link -> $exe"

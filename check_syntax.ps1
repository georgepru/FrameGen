$errs = $null; $tokens = $null
[System.Management.Automation.Language.Parser]::ParseFile(
    (Resolve-Path '.\launcher.ps1').Path, [ref]$tokens, [ref]$errs) | Out-Null
foreach ($e in $errs) { Write-Host "Line $($e.Extent.StartLineNumber): $($e.Message)" }
if ($errs.Count -eq 0) { Write-Host "No syntax errors" }

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$exePath = Join-Path $PSScriptRoot "build\Release\framegen_mvp.exe"

# ── Form ──────────────────────────────────────────────────────────────────────
$form               = New-Object Windows.Forms.Form
$form.Text          = "FrameGen Launcher"
$form.StartPosition = "CenterScreen"
$form.FormBorderStyle = "FixedDialog"
$form.MaximizeBox   = $false
$form.Font          = New-Object Drawing.Font("Segoe UI", 9)

# Update this URL when you upload the ONNX files as GitHub release assets.
$MODELS_BASE_URL = "https://github.com/georgepru/FrameGen/releases/download/v1.0/"

# ── Tab control ───────────────────────────────────────────────────────────────
$tab      = New-Object Windows.Forms.TabControl
$tab.Dock = "Fill"
$form.Controls.Add($tab)

$tabMain  = New-Object Windows.Forms.TabPage ; $tabMain.Text  = "Main"
$tabSetup = New-Object Windows.Forms.TabPage ; $tabSetup.Text = "Setup"
$tab.TabPages.AddRange(@($tabMain, $tabSetup))

$pad = 10
$y   = $pad

function New-Group($text, $height) {
    $g          = New-Object Windows.Forms.GroupBox
    $g.Text     = $text
    $g.Location = New-Object Drawing.Point($pad, $script:y)
    $g.Size     = New-Object Drawing.Size(414, $height)
    $tabMain.Controls.Add($g)
    $script:y  += $height + 6
    return $g
}

# ── Input ─────────────────────────────────────────────────────────────────────
$grpInput = New-Group "Input" 82

$lblOnnx          = New-Object Windows.Forms.Label
$lblOnnx.Text     = "ONNX Model:"
$lblOnnx.Location = New-Object Drawing.Point(8, 22)
$lblOnnx.AutoSize = $true

$cmbOnnx               = New-Object Windows.Forms.ComboBox
$cmbOnnx.DropDownStyle = "DropDownList"
$cmbOnnx.Location      = New-Object Drawing.Point(92, 19)
$cmbOnnx.Size          = New-Object Drawing.Size(290, 22)
# Populate from *.onnx files next to the exe
$onnxDir = Split-Path $exePath
Get-ChildItem $onnxDir -Filter "*.onnx" -ErrorAction SilentlyContinue |
    Sort-Object Name | ForEach-Object { [void]$cmbOnnx.Items.Add($_.Name) }
if ($cmbOnnx.Items.Count -eq 0) { [void]$cmbOnnx.Items.Add("rife_1080p.onnx") }
# Default to rife_1080p.onnx if present, else first item
$def = $cmbOnnx.Items | Where-Object { $_ -eq "rife_1080p.onnx" } | Select-Object -First 1
$cmbOnnx.SelectedIndex = if ($def) { $cmbOnnx.Items.IndexOf($def) } else { 0 }

$lblDev          = New-Object Windows.Forms.Label
$lblDev.Text     = "Capture Device:"
$lblDev.Location = New-Object Drawing.Point(8, 53)
$lblDev.AutoSize = $true

$cmbDev          = New-Object Windows.Forms.ComboBox
$cmbDev.DropDownStyle = "DropDownList"
$cmbDev.Location = New-Object Drawing.Point(112, 50)
$cmbDev.Size     = New-Object Drawing.Size(174, 22)

# Device index → friendly name map
$script:deviceMap = @{}

function Refresh-Devices {
    $cmbDev.Items.Clear()
    $script:deviceMap.Clear()
    try {
        $lines = & $exePath --list-devices 2>$null
        foreach ($line in $lines) {
            if ($line -match '^(\d+):\s+(.+)$') {
                $idx  = [int]$Matches[1]
                $name = $Matches[2].Trim()
                $script:deviceMap[$cmbDev.Items.Count] = $idx
                [void]$cmbDev.Items.Add("[$idx] $name")
            }
        }
    } catch {}
    if ($cmbDev.Items.Count -gt 0) { $cmbDev.SelectedIndex = 0 }
    else { [void]$cmbDev.Items.Add("(no devices found)") ; $cmbDev.SelectedIndex = 0 }
}

$btnRefresh          = New-Object Windows.Forms.Button
$btnRefresh.Text     = "Refresh List"
$btnRefresh.Location = New-Object Drawing.Point(292, 49)
$btnRefresh.Size     = New-Object Drawing.Size(90, 24)
$btnRefresh.Add_Click({ Refresh-Devices })

$grpInput.Controls.AddRange(@($lblOnnx, $cmbOnnx, $lblDev, $cmbDev, $btnRefresh))

# ── Options ───────────────────────────────────────────────────────────────────
$grpOpts = New-Group "Options" 56

$chkNoAudio          = New-Object Windows.Forms.CheckBox
$chkNoAudio.Text     = "No Audio"
$chkNoAudio.Location = New-Object Drawing.Point(8, 20)
$chkNoAudio.AutoSize = $true

$grpOpts.Controls.AddRange(@($chkNoAudio))

# ── Upscaling ─────────────────────────────────────────────────────────────────
$grpScale = New-Group "Upscaling" 160

$radNone          = New-Object Windows.Forms.RadioButton
$radNone.Text     = "None  (native resolution)"
$radNone.Location = New-Object Drawing.Point(8, 20)
$radNone.AutoSize = $true
$radNone.Checked  = $true

$rad720to1080          = New-Object Windows.Forms.RadioButton
$rad720to1080.Text     = "720p to 1080p"
$rad720to1080.Location = New-Object Drawing.Point(8, 46)
$rad720to1080.AutoSize = $true

$rad720to1440          = New-Object Windows.Forms.RadioButton
$rad720to1440.Text     = "720p to 1440p"
$rad720to1440.Location = New-Object Drawing.Point(8, 72)
$rad720to1440.AutoSize = $true

$rad1440          = New-Object Windows.Forms.RadioButton
$rad1440.Text     = "1080p to 1440p"
$rad1440.Location = New-Object Drawing.Point(8, 98)
$rad1440.AutoSize = $true

$rad4K          = New-Object Windows.Forms.RadioButton
$rad4K.Text     = "1080p to 4K"
$rad4K.Location = New-Object Drawing.Point(8, 124)
$rad4K.AutoSize = $true

$chkFSR          = New-Object Windows.Forms.CheckBox
$chkFSR.Text     = "FSR 1.0"
$chkFSR.Location = New-Object Drawing.Point(220, 110)
$chkFSR.AutoSize = $true
$chkFSR.Enabled  = $false

$updateFSR = {
    $up             = $rad720to1080.Checked -or $rad720to1440.Checked -or $rad1440.Checked -or $rad4K.Checked
    $chkFSR.Enabled = $up
    if (-not $up) { $chkFSR.Checked = $false }
}
$radNone.Add_CheckedChanged($updateFSR)
$rad720to1080.Add_CheckedChanged($updateFSR)
$rad720to1440.Add_CheckedChanged($updateFSR)
$rad1440.Add_CheckedChanged($updateFSR)
$rad4K.Add_CheckedChanged($updateFSR)

$grpScale.Controls.AddRange(@($radNone, $rad720to1080, $rad720to1440, $rad1440, $rad4K, $chkFSR))

# ── Benchmark button ──────────────────────────────────────────────────────────
$btnBenchmark           = New-Object Windows.Forms.Button
$btnBenchmark.Text      = "Run Benchmark  (auto-detect best settings)"
$btnBenchmark.Location  = New-Object Drawing.Point(10, 228)
$btnBenchmark.Size      = New-Object Drawing.Size(414, 30)
$btnBenchmark.Font      = New-Object Drawing.Font("Segoe UI", 9)
$btnBenchmark.BackColor = [Drawing.Color]::FromArgb(60, 60, 80)
$btnBenchmark.ForeColor = [Drawing.Color]::FromArgb(200, 220, 255)
$btnBenchmark.FlatStyle = "Flat"
$tabSetup.Controls.Add($btnBenchmark)
$y += 38

# ── Launch button ─────────────────────────────────────────────────────────────
$btnLaunch           = New-Object Windows.Forms.Button
$btnLaunch.Text      = "Launch"
$btnLaunch.Location  = New-Object Drawing.Point($pad, $y)
$btnLaunch.Size      = New-Object Drawing.Size(414, 38)
$btnLaunch.Font      = New-Object Drawing.Font("Segoe UI", 11, [Drawing.FontStyle]::Bold)
$btnLaunch.BackColor = [Drawing.Color]::FromArgb(0, 120, 212)
$btnLaunch.ForeColor = [Drawing.Color]::White
$btnLaunch.FlatStyle = "Flat"
$tabMain.Controls.Add($btnLaunch)
$y += 46

# ── Log box ───────────────────────────────────────────────────────────────────
$grpLog = New-Group "Output / Errors" 160

$txtLog            = New-Object Windows.Forms.TextBox
$txtLog.Multiline  = $true
$txtLog.ScrollBars = "Vertical"
$txtLog.ReadOnly   = $true
$txtLog.Location   = New-Object Drawing.Point(5, 18)
$txtLog.Size       = New-Object Drawing.Size(402, 133)
$txtLog.Font       = New-Object Drawing.Font("Consolas", 8)
$txtLog.BackColor  = [Drawing.Color]::FromArgb(20, 20, 20)
$txtLog.ForeColor  = [Drawing.Color]::FromArgb(180, 255, 180)
$grpLog.Controls.Add($txtLog)

$tabContentH = $y + 10
$formH = $tabContentH + 30
$form.ClientSize = New-Object Drawing.Size(434, $formH)

# ── State ─────────────────────────────────────────────────────────────────────
$script:proc   = $null
$script:timer  = $null
$script:outBuf = $null
$script:errBuf = $null

function Set-LaunchButton($running) {
    if ($running) {
        $btnLaunch.Text      = "Stop"
        $btnLaunch.BackColor = [Drawing.Color]::FromArgb(200, 50, 50)
    } else {
        $btnLaunch.Text      = "Launch"
        $btnLaunch.BackColor = [Drawing.Color]::FromArgb(0, 120, 212)
    }
}

# ── Benchmark logic ───────────────────────────────────────────────────────────
function Apply-RecommendedSettings($rec) {
    # rec is a hashtable: upscaleFlag, fsr, label
    $radNone.Checked     = $true  # reset first
    $chkFSR.Checked      = $false
    $chkNoAudio.Checked  = $false   # audio ON

    switch ($rec.upscaleFlag) {
        "--upscaled-720-to-1080"  { $rad720to1080.Checked = $true }
        "--upscaled-720-to-1440"  { $rad720to1440.Checked = $true }
        "--upscaled-1080-to-1440" { $rad1440.Checked      = $true }
        "--upscaled-1080-to-4k"   { $rad4K.Checked        = $true }
        default                   { $radNone.Checked       = $true }
    }
    if ($rec.fsr -and $chkFSR.Enabled) { $chkFSR.Checked = $true }
}

$script:benchJob     = $null
$script:benchTimer   = $null
$script:benchDots    = 0
$script:benchPhase   = ""
$script:benchMs1080  = [double]-2
$script:benchMs720   = [double]-2
$script:benchOut1080 = ""
$script:benchOut720  = ""
$script:benchOutFile = ""
$script:benchLogPos  = 0

$btnBenchmark.Add_Click({
    $testDir  = Join-Path $PSScriptRoot "testdata"
    $test1080 = Join-Path $testDir "1080p_test.mp4"
    $test720  = Join-Path $testDir "720p_test.mp4"

    foreach ($f in @($test1080, $test720)) {
        if (-not (Test-Path $f)) {
            [Windows.Forms.MessageBox]::Show(
                "Benchmark video not found:`n$f`n`nPlace 720p_test.mp4 and 1080p_test.mp4 in the testdata folder.",
                "Benchmark", [Windows.Forms.MessageBoxButtons]::OK,
                [Windows.Forms.MessageBoxIcon]::Warning) | Out-Null
            return
        }
    }

    $btnBenchmark.Enabled = $false
    $btnBenchmark.Text    = "Benchmarking 1080p."
    $script:benchDots     = 0
    $script:benchPhase    = "1080p"
    $script:benchMs1080   = [double]-2
    $script:benchMs720    = [double]-2

    $txtLog.ForeColor = [Drawing.Color]::FromArgb(180,255,180)
    $txtLog.Clear()
    $txtLog.AppendText("=== Benchmark: 1080p test ===`r`n")

    $exeP   = $exePath
    $onnxP  = Join-Path (Split-Path $exePath) $cmbOnnx.SelectedItem
    $wdPath = Split-Path $exePath
    $myPid  = [Diagnostics.Process]::GetCurrentProcess().Id
    $o1080  = Join-Path $env:TEMP "fg_bench1080_$myPid.txt"
    $o720   = Join-Path $env:TEMP "fg_bench720_$myPid.txt"
    Remove-Item $o1080,$o720 -ErrorAction SilentlyContinue

    $script:benchOut1080 = $o1080
    $script:benchOut720  = $o720
    $script:benchOutFile = $o1080
    $script:benchLogPos  = 0

    $script:benchJob = Start-Job -ScriptBlock {
        param($exeP, $onnxP, $vid, $wdPath, $outFile)
        $errFile = $outFile + ".err"
        Remove-Item $outFile,$errFile -ErrorAction SilentlyContinue
        $a = "--benchmark --gpu-dedupe --dedupe-threshold 1 --no-audio `"$onnxP`" --file `"$vid`""
        Start-Process -FilePath $exeP -ArgumentList $a `
            -WorkingDirectory $wdPath -Wait `
            -RedirectStandardOutput $outFile -RedirectStandardError $errFile | Out-Null
        $raw = Get-Content $outFile -Raw -ErrorAction SilentlyContinue
        if ($raw -match 'BENCHMARK_RESULT avg_ms=([0-9.]+)') { return [double]$Matches[1] }
        return [double]-1
    } -ArgumentList $exeP, $onnxP, $test1080, $wdPath, $o1080

    if ($script:benchTimer) { $script:benchTimer.Stop(); $script:benchTimer.Dispose() }
    $script:benchTimer = New-Object Windows.Forms.Timer
    $script:benchTimer.Interval = 500
    $script:benchTimer.Add_Tick({
        # Animate button
        $script:benchDots = ($script:benchDots + 1) % 4
        $btnBenchmark.Text = "Benchmarking $($script:benchPhase)" + ('.' * ($script:benchDots + 1))

        # Stream exe stdout from active temp file into log box
        if ($script:benchOutFile -and (Test-Path $script:benchOutFile)) {
            try {
                $fs = [IO.File]::Open($script:benchOutFile, 'Open', 'Read', 'ReadWrite')
                if ($fs.Length -gt $script:benchLogPos) {
                    $fs.Position = $script:benchLogPos
                    $count = [int]($fs.Length - $script:benchLogPos)
                    $buf   = [byte[]]::new($count)
                    $read  = $fs.Read($buf, 0, $count)
                    $script:benchLogPos += $read
                    $newText = [Text.Encoding]::UTF8.GetString($buf, 0, $read)
                    $newText = $newText.Replace("`r`n","`n").Replace("`r","`n").Replace("`n","`r`n")
                    $txtLog.AppendText($newText)
                    $txtLog.ScrollToCaret()
                }
                $fs.Close()
            } catch {}
        }

        if ($script:benchJob -and $script:benchJob.State -eq 'Running') { return }
        if (-not $script:benchJob) { return }

        # Job done — harvest result
        $rawMs = Receive-Job $script:benchJob -ErrorAction SilentlyContinue
        Remove-Job $script:benchJob -Force
        $script:benchJob = $null
        $ms = try { [double]($rawMs | Select-Object -Last 1) } catch { [double]-1 }
        if ($null -eq $rawMs) { $ms = [double]-1 }

        if ($script:benchPhase -eq "1080p") {
            $script:benchMs1080 = $ms
            $txtLog.AppendText("`r`n--- 1080p avg: $([Math]::Round($ms,2)) ms ---`r`n")

            if ($ms -gt 30) {
                # Slow on 1080p — need 720p test
                $script:benchPhase   = "720p"
                $script:benchOutFile = $script:benchOut720
                $script:benchLogPos  = 0
                Remove-Item $script:benchOut720 -ErrorAction SilentlyContinue
                $txtLog.AppendText("`r`n=== Benchmark: 720p test ===`r`n")

                $script:benchJob = Start-Job -ScriptBlock {
                    param($exeP, $onnxP, $vid, $wdPath, $outFile)
                    $errFile = $outFile + ".err"
                    Remove-Item $outFile,$errFile -ErrorAction SilentlyContinue
                    $a = "--benchmark --gpu-dedupe --dedupe-threshold 1 --no-audio `"$onnxP`" --file `"$vid`""
                    Start-Process -FilePath $exeP -ArgumentList $a `
                        -WorkingDirectory $wdPath -Wait `
                        -RedirectStandardOutput $outFile -RedirectStandardError $errFile | Out-Null
                    $raw = Get-Content $outFile -Raw -ErrorAction SilentlyContinue
                    if ($raw -match 'BENCHMARK_RESULT avg_ms=([0-9.]+)') { return [double]$Matches[1] }
                    return [double]-1
                } -ArgumentList $exeP, $onnxP, $test720, $wdPath, $script:benchOut720
                return
            }
            $script:benchMs720 = [double]-1
        } elseif ($script:benchPhase -eq "720p") {
            $script:benchMs720 = $ms
            $txtLog.AppendText("`r`n--- 720p avg: $([Math]::Round($ms,2)) ms ---`r`n")
        }

        # All phases done
        $script:benchTimer.Stop()
        $btnBenchmark.Enabled = $true
        $btnBenchmark.Text    = "Run Benchmark  (auto-detect best settings)"
        $txtLog.AppendText("`r`n=== Benchmark complete ===`r`n")

        $ms1080 = $script:benchMs1080
        $ms720  = $script:benchMs720
        $screenW = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Width
        $screenH = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Height
        # Use WMI physical resolution to avoid DPI-scaling misdetection
        $mon     = Get-CimInstance -ClassName Win32_VideoController | Where-Object { $_.CurrentHorizontalResolution -gt 0 } | Select-Object -First 1
        $physW   = if ($mon.CurrentHorizontalResolution) { $mon.CurrentHorizontalResolution } else { $screenW }
        $physH   = if ($mon.CurrentVerticalResolution)   { $mon.CurrentVerticalResolution   } else { $screenH }
        $is4K    = ($physW -ge 3840 -and $physH -ge 2160)
        $rec     = $null
        $summary = ""

        if ($ms1080 -lt 0) {
            $summary = "Benchmark failed - could not read RIFE timing from 1080p run."
        } elseif ($ms1080 -lt 17) {
            $upFlag  = if ($is4K) { "--upscaled-1080-to-4k" } else { "--upscaled-1080-to-1440" }
            $upLabel = if ($is4K) { "1080p -> 4K" } else { "1080p -> 1440p" }
            $rec     = @{ upscaleFlag = $upFlag; fsr = $true }
            $summary = "1080p avg RIFE: $([Math]::Round($ms1080,1)) ms  ->  HIGH-END GPU`n`nRecommended settings:`n  Upscaling: $upLabel`n  FSR 1.0: ON`n  GPU Dedupe: ON (threshold 1)`n  Audio: ON"
        } elseif ($ms1080 -le 30) {
            $rec     = @{ upscaleFlag = "none"; fsr = $false }
            $summary = "1080p avg RIFE: $([Math]::Round($ms1080,1)) ms  ->  MID-RANGE GPU`n`nRecommended settings:`n  Upscaling: None`n  FSR 1.0: OFF`n  GPU Dedupe: ON (threshold 1)`n  Audio: ON"
        } else {
            if ($ms720 -lt 0) {
                $summary = "1080p avg RIFE: $([Math]::Round($ms1080,1)) ms`n720p benchmark failed - could not read RIFE timing."
            } elseif ($ms720 -lt 17) {
                $upFlag  = if ($is4K) { "--upscaled-720-to-1440" } else { "--upscaled-720-to-1080" }
                $upLabel = if ($is4K) { "720p -> 1440p" } else { "720p -> 1080p" }
                $rec     = @{ upscaleFlag = $upFlag; fsr = $true }
                $summary = "1080p avg RIFE: $([Math]::Round($ms1080,1)) ms`n720p avg RIFE:  $([Math]::Round($ms720,1)) ms  ->  MID-RANGE GPU (720p capable)`n`nRecommended settings:`n  Upscaling: $upLabel`n  FSR 1.0: ON`n  GPU Dedupe: ON (threshold 1)`n  Audio: ON"
            } else {
                $rec     = @{ upscaleFlag = "none"; fsr = $false }
                $tier    = if ($ms720 -le 30) { "MID-RANGE" } else { "ENTRY-LEVEL" }
                $summary = "1080p avg RIFE: $([Math]::Round($ms1080,1)) ms`n720p avg RIFE:  $([Math]::Round($ms720,1)) ms  ->  $tier GPU`n`nRecommended settings:`n  Upscaling: None`n  FSR 1.0: OFF`n  GPU Dedupe: ON (threshold 1)`n  Audio: ON"
            }
        }

        $dlg = New-Object Windows.Forms.Form
        $dlg.Text            = "Benchmark Results"
        $dlg.StartPosition   = "CenterParent"
        $dlg.FormBorderStyle = "FixedDialog"
        $dlg.MaximizeBox     = $false
        $dlg.MinimizeBox     = $false
        $dlg.ClientSize      = New-Object Drawing.Size(420, 220)
        $dlg.Font            = New-Object Drawing.Font("Segoe UI", 9)

        $lbl          = New-Object Windows.Forms.Label
        $lbl.Text     = $summary
        $lbl.Location = New-Object Drawing.Point(14, 14)
        $lbl.Size     = New-Object Drawing.Size(392, 150)
        $lbl.AutoSize = $false
        $dlg.Controls.Add($lbl)

        $btnApply           = New-Object Windows.Forms.Button
        $btnApply.Text      = "Apply These Settings"
        $btnApply.Location  = New-Object Drawing.Point(14, 174)
        $btnApply.Size      = New-Object Drawing.Size(180, 30)
        $btnApply.BackColor = [Drawing.Color]::FromArgb(0, 120, 212)
        $btnApply.ForeColor = [Drawing.Color]::White
        $btnApply.FlatStyle = "Flat"
        $btnApply.Enabled   = ($rec -ne $null)
        $btnApply.Add_Click({ Apply-RecommendedSettings $rec; $dlg.Close() })
        $dlg.Controls.Add($btnApply)

        $btnClose          = New-Object Windows.Forms.Button
        $btnClose.Text     = "Close"
        $btnClose.Location = New-Object Drawing.Point(204, 174)
        $btnClose.Size     = New-Object Drawing.Size(80, 30)
        $btnClose.Add_Click({ $dlg.Close() })
        $dlg.Controls.Add($btnClose)

        $dlg.ShowDialog($form) | Out-Null
    })
    $script:benchTimer.Start()
})

# ── Launch logic ──────────────────────────────────────────────────────────────
$btnLaunch.Add_Click({

    # If running, kill it
    if ($script:proc -and -not $script:proc.HasExited) {
        $script:proc.Kill()
        return
    }

    Save-Settings

    # Build arg list
    $args = [System.Collections.Generic.List[string]]::new()
    if ($chkNoAudio.Checked)  { $args.Add("--no-audio") }
    $args.Add("--gpu-dedupe")
    $args.Add("--dedupe-threshold")
    $args.Add("1")
    if ($rad720to1080.Checked) { $args.Add("--upscaled-720-to-1080") }
    if ($rad720to1440.Checked) { $args.Add("--upscaled-720-to-1440") }
    if ($rad1440.Checked)      { $args.Add("--upscaled-1080-to-1440") }
    if ($rad4K.Checked)        { $args.Add("--upscaled-1080-to-4k") }
    if ($chkFSR.Checked)      { $args.Add("--fsr") }
    $args.Add((Join-Path (Split-Path $exePath) $cmbOnnx.SelectedItem))
    # Resolve selected dropdown item to device index
    $selIdx = $cmbDev.SelectedIndex
    $devIdx = if ($script:deviceMap.ContainsKey($selIdx)) { $script:deviceMap[$selIdx] } else { 0 }
    $args.Add([string]$devIdx)

    $argStr = $args -join ' '

    $txtLog.ForeColor = [Drawing.Color]::FromArgb(180, 255, 180)
    $txtLog.Clear()
    $txtLog.AppendText("$ framegen_mvp.exe $argStr`r`n`r`n")

    $psi                        = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName               = $exePath
    $psi.Arguments              = $argStr
    $psi.WorkingDirectory       = Split-Path $exePath
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true
    $psi.UseShellExecute        = $false
    $psi.CreateNoWindow         = $true   # suppress extra console; D3D window still appears

    $script:outBuf = New-Object Text.StringBuilder
    $script:errBuf = New-Object Text.StringBuilder

    $p = New-Object Diagnostics.Process
    $p.StartInfo           = $psi
    $p.EnableRaisingEvents = $true
    $p.add_OutputDataReceived({ param($s,$e); if ($e.Data) { [void]$script:outBuf.AppendLine($e.Data) } })
    $p.add_ErrorDataReceived({  param($s,$e); if ($e.Data) { [void]$script:errBuf.AppendLine($e.Data) } })

    $p.Start()             | Out-Null
    $p.BeginOutputReadLine()
    $p.BeginErrorReadLine()
    $script:proc = $p

    Set-LaunchButton $true

    # Poll for exit on UI thread
    $script:timer          = New-Object Windows.Forms.Timer
    $script:timer.Interval = 300
    $script:timer.Add_Tick({
        if (-not $script:proc.HasExited) { return }

        $script:timer.Stop()
        $script:proc.WaitForExit()   # flush async reads

        $out  = $script:outBuf.ToString().Trim()
        $err  = $script:errBuf.ToString().Trim()
        $code = $script:proc.ExitCode

        if ($code -ne 0) {
            $txtLog.ForeColor = [Drawing.Color]::FromArgb(255, 110, 110)
            $txtLog.AppendText("=== CRASHED  (exit code $code) ===`r`n`r`n")
        } else {
            $txtLog.ForeColor = [Drawing.Color]::FromArgb(180, 255, 180)
            $txtLog.AppendText("=== Exited cleanly ===`r`n`r`n")
        }
        if ($out) { $txtLog.AppendText($out + "`r`n") }
        if ($err) { $txtLog.AppendText($err + "`r`n") }
        $txtLog.SelectionStart = $txtLog.TextLength
        $txtLog.ScrollToCaret()

        Set-LaunchButton $false
    })
    $script:timer.Start()
})

# ── Setup tab ─────────────────────────────────────────────────────────────────
$grpModels          = New-Object Windows.Forms.GroupBox
$grpModels.Text     = "RIFE Models"
$grpModels.Location = New-Object Drawing.Point(10, 10)
$grpModels.Size     = New-Object Drawing.Size(414, 270)
$tabSetup.Controls.Add($grpModels)

$lblModelInfo          = New-Object Windows.Forms.Label
$lblModelInfo.Text     = "Download RIFE interpolation models to build\Release\."
$lblModelInfo.Location = New-Object Drawing.Point(8, 22)
$lblModelInfo.Size     = New-Object Drawing.Size(398, 18)
$grpModels.Controls.Add($lblModelInfo)

# 720p row
$lbl720           = New-Object Windows.Forms.Label
$lbl720.Text      = "rife_720p.onnx"
$lbl720.Location  = New-Object Drawing.Point(8, 52)
$lbl720.Size      = New-Object Drawing.Size(158, 18)
$grpModels.Controls.Add($lbl720)

$lbl720Status          = New-Object Windows.Forms.Label
$lbl720Status.Location = New-Object Drawing.Point(170, 52)
$lbl720Status.Size     = New-Object Drawing.Size(102, 18)
$grpModels.Controls.Add($lbl720Status)

$btn720           = New-Object Windows.Forms.Button
$btn720.Text      = "Download"
$btn720.Location  = New-Object Drawing.Point(278, 48)
$btn720.Size      = New-Object Drawing.Size(128, 26)
$btn720.FlatStyle = "Flat"
$grpModels.Controls.Add($btn720)

# 1080p row
$lbl1080           = New-Object Windows.Forms.Label
$lbl1080.Text      = "rife_1080p.onnx"
$lbl1080.Location  = New-Object Drawing.Point(8, 84)
$lbl1080.Size      = New-Object Drawing.Size(158, 18)
$grpModels.Controls.Add($lbl1080)

$lbl1080Status          = New-Object Windows.Forms.Label
$lbl1080Status.Location = New-Object Drawing.Point(170, 84)
$lbl1080Status.Size     = New-Object Drawing.Size(102, 18)
$grpModels.Controls.Add($lbl1080Status)

$btn1080           = New-Object Windows.Forms.Button
$btn1080.Text      = "Download"
$btn1080.Location  = New-Object Drawing.Point(278, 80)
$btn1080.Size      = New-Object Drawing.Size(128, 26)
$btn1080.FlatStyle = "Flat"
$grpModels.Controls.Add($btn1080)

$progressDl          = New-Object Windows.Forms.ProgressBar
$progressDl.Location = New-Object Drawing.Point(8, 120)
$progressDl.Size     = New-Object Drawing.Size(398, 18)
$progressDl.Visible  = $false
$grpModels.Controls.Add($progressDl)

$lblDlStatus          = New-Object Windows.Forms.Label
$lblDlStatus.Location = New-Object Drawing.Point(8, 144)
$lblDlStatus.Size     = New-Object Drawing.Size(398, 18)
$lblDlStatus.Text     = ""
$grpModels.Controls.Add($lblDlStatus)

$lblModelNote           = New-Object Windows.Forms.Label
$lblModelNote.Text      = "Models are ~19 MB each. An internet connection is required."
$lblModelNote.Location  = New-Object Drawing.Point(8, 192)
$lblModelNote.Size      = New-Object Drawing.Size(398, 18)
$lblModelNote.ForeColor = [Drawing.Color]::Gray
$grpModels.Controls.Add($lblModelNote)

function Update-ModelStatus {
    $relDir = Split-Path $exePath
    $minSz  = 10 * 1024 * 1024
    foreach ($pair in @(
        @("rife_720p.onnx",  $lbl720Status),
        @("rife_1080p.onnx", $lbl1080Status)
    )) {
        $onnx = Join-Path $relDir $pair[0]
        $data = $onnx + ".data"
        $lbl  = $pair[1]
        # Valid = .onnx exists AND (.data exists with real weight data OR .onnx itself is large)
        $onnxOk = (Test-Path $onnx)
        $dataOk = (Test-Path $data) -and (Get-Item $data).Length -gt $minSz
        $selfOk = $onnxOk -and (Get-Item $onnx).Length -gt $minSz
        if ($onnxOk -and ($dataOk -or $selfOk)) {
            $mb = [math]::Round(((Get-Item $onnx).Length + $(if (Test-Path $data) { (Get-Item $data).Length } else { 0 })) / 1MB, 0)
            $lbl.Text      = "Present  ($mb MB)"
            $lbl.ForeColor = [Drawing.Color]::FromArgb(80, 200, 80)
        } else {
            $lbl.Text      = "Missing"
            $lbl.ForeColor = [Drawing.Color]::FromArgb(220, 80, 80)
        }
    }
}

# Downloads a queue of [url, destPath] pairs sequentially, one at a time.
$script:dlWc       = $null
$script:dlQueue    = @()
$script:dlLabel    = ""

function Invoke-NextDownload {
    if ($script:dlQueue.Count -eq 0) {
        # All done
        $progressDl.Visible = $false
        $lblDlStatus.Text   = "Download complete."
        Update-ModelStatus
        $cmbOnnx.Items.Clear()
        Get-ChildItem (Split-Path $exePath) -Filter "*.onnx" -ErrorAction SilentlyContinue |
            Sort-Object Name | ForEach-Object { [void]$cmbOnnx.Items.Add($_.Name) }
        $defItem = $cmbOnnx.Items | Where-Object { $_ -eq "rife_1080p.onnx" } | Select-Object -First 1
        $cmbOnnx.SelectedIndex = if ($defItem) { $cmbOnnx.Items.IndexOf($defItem) } else { 0 }
        $btn720.Enabled  = $true
        $btn1080.Enabled = $true
        $script:dlWc     = $null
        return
    }

    $pair   = $script:dlQueue[0]
    $script:dlQueue = $script:dlQueue[1..($script:dlQueue.Count - 1)]
    $url    = $pair[0]
    $dest   = $pair[1]
    $fname  = Split-Path $url -Leaf
    $lblDlStatus.Text   = "Downloading $fname..."
    $progressDl.Value   = 0
    $progressDl.Visible = $true

    $script:dlWc = New-Object Net.WebClient
    $script:dlWc.add_DownloadProgressChanged({
        param($s, $e)
        $form.Invoke([Action]{
            $progressDl.Value = [Math]::Min($e.ProgressPercentage, 100)
            $total = if ($e.TotalBytesToReceive -gt 0) {
                "$([Math]::Round($e.TotalBytesToReceive / 1MB, 1)) MB"
            } else { "?" }
            $lblDlStatus.Text = "$(Split-Path $s.BaseAddress.AbsolutePath -Leaf): " +
                "$([Math]::Round($e.BytesReceived / 1MB, 1)) / $total  ($($e.ProgressPercentage)%)"
        })
    })
    $script:dlWc.add_DownloadFileCompleted({
        param($s, $e)
        $form.Invoke([Action]{
            if ($e.Cancelled -or $e.Error) {
                $progressDl.Visible = $false
                $lblDlStatus.Text   = if ($e.Error) { "Error: $($e.Error.Message)" } else { "Cancelled." }
                $btn720.Enabled  = $true
                $btn1080.Enabled = $true
                $script:dlQueue  = @()
                $script:dlWc     = $null
            } else {
                Invoke-NextDownload
            }
        })
    })
    try {
        $script:dlWc.DownloadFileAsync([Uri]$url, $dest)
    } catch {
        $lblDlStatus.Text   = "Failed to start: $_"
        $progressDl.Visible = $false
        $btn720.Enabled     = $true
        $btn1080.Enabled    = $true
        $script:dlQueue     = @()
        $script:dlWc        = $null
    }
}

function Start-ModelDownload($basename) {
    if ($script:dlWc -ne $null) { return }
    $relDir = Split-Path $exePath
    $btn720.Enabled     = $false
    $btn1080.Enabled    = $false
    # Queue both the .onnx graph and the .onnx.data weights file
    $script:dlQueue = @(
        @($MODELS_BASE_URL + $basename,         (Join-Path $relDir $basename)),
        @($MODELS_BASE_URL + $basename + ".data", (Join-Path $relDir ($basename + ".data")))
    )
    Invoke-NextDownload
}

$btn720.Add_Click({  Start-ModelDownload "rife_720p.onnx"  })
$btn1080.Add_Click({ Start-ModelDownload "rife_1080p.onnx" })

$settingsPath = Join-Path $PSScriptRoot "launcher_settings.json"

function Save-Settings {
    $s = @{
        onnx    = $cmbOnnx.SelectedItem
        devIdx  = $cmbDev.SelectedIndex
        noAudio = $chkNoAudio.Checked
        fsr     = $chkFSR.Checked
        scale   = if ($rad720to1080.Checked) { "720to1080" }
                  elseif ($rad720to1440.Checked) { "720to1440" }
                  elseif ($rad1440.Checked) { "1440" }
                  elseif ($rad4K.Checked) { "4K" }
                  else { "none" }
    }
    $s | ConvertTo-Json | Set-Content $settingsPath -Encoding UTF8
}

function Load-Settings {
    if (-not (Test-Path $settingsPath)) { return }
    try {
        $s = Get-Content $settingsPath -Raw | ConvertFrom-Json
        if ($s.onnx -and $cmbOnnx.Items.Contains($s.onnx)) {
            $cmbOnnx.SelectedIndex = $cmbOnnx.Items.IndexOf($s.onnx)
        }
        if ($s.devIdx -ge 0 -and $s.devIdx -lt $cmbDev.Items.Count) {
            $cmbDev.SelectedIndex = $s.devIdx
        }
        $chkNoAudio.Checked = [bool]$s.noAudio
        switch ($s.scale) {
            "720to1080" { $rad720to1080.Checked = $true }
            "720to1440" { $rad720to1440.Checked = $true }
            "1440"      { $rad1440.Checked = $true }
            "4K"        { $rad4K.Checked = $true }
            default     { $radNone.Checked = $true }
        }
        if ($s.fsr -and $chkFSR.Enabled) { $chkFSR.Checked = $true }
    } catch {}
}

$form.Add_FormClosing({ Save-Settings })

[Windows.Forms.Application]::EnableVisualStyles()
Refresh-Devices
Update-ModelStatus
Load-Settings
# First launch (no settings file) → open Setup tab
if (-not (Test-Path $settingsPath)) { $tab.SelectedTab = $tabSetup }
[Windows.Forms.Application]::Run($form)

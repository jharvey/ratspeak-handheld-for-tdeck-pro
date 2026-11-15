<#
How-to videos from the simulator (Windows PowerShell 5.1).

  powershell -ExecutionPolicy Bypass -File tools\howto\make_videos.ps1 [-Only send_message,wifi]
      [-SimExe <path to ratspeak-sim.exe>] [-Out docs\videos] [-Setup]

Each video is a script in sim\scripts\howto (it records with "rec <name>"), run headless on
its own simulated radio channel, sometimes with a second radio (SAM) running a companion
script. tools\howto\make_video.py then adds the pointing hand, key names, captions and click
sounds and writes <Out>\<name>.mp4. Needs ffmpeg (PATH or $env:FFMPEG) and Python + Pillow.

Devices (folders in <sim exe dir>\simdata): ht_alex (ALEX, the one filmed), ht_sam (SAM),
ht_jo (JO, never runs, only a contact), ht_new (fresh each time, for "add a contact"),
ht_map (map tiles on its SD card, for the Atlas). -Setup (or a missing ht_alex) creates
ALEX / SAM / JO and reads their addresses; they are kept in <simdata>\ht_ids.txt.
#>
param(
    [string[]]$Only = @(),
    [string]$SimExe = "",
    [string]$Out = "",
    [switch]$Setup
)
$ErrorActionPreference = "Stop"
$Only = @($Only | ForEach-Object { $_ -split "," } | Where-Object { $_ })   # "-File" passes "a,b" as one string
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $SimExe) { $SimExe = "$repo\sim\build\ratspeak-sim.exe" }
if (-not $Out) { $Out = "$repo\docs\videos" }
$simData = Join-Path (Split-Path $SimExe) "simdata"
$scripts = "$repo\sim\scripts\howto"
$work = Join-Path $env:TEMP "ratspeak_howto"
New-Item -ItemType Directory -Force $work, $Out | Out-Null

# name, title, main device, companion script ("" = none), fresh main device
$videos = @(
    @("send_message",  "Send a message",         "ht_alex", "companion_sam.txt",          $false),
    @("read_message",  "Read a message",         "ht_alex", "companion_sam_send.txt",     $false),
    @("add_contact",   "Add a contact",          "ht_new",  "companion_sam_announce.txt", $true),
    @("share_contact", "Send (share) a contact", "ht_alex", "companion_sam.txt",          $false),
    @("wifi",          "Connect to WiFi",        "ht_alex", "",                           $false),
    @("if_found",      "Set the If-found details", "ht_alex", "",                         $false),
    @("mode_touch",    "Operator mode and touch", "ht_alex", "",                          $false),
    @("atlas",         "Open the Atlas (map)",   "ht_map",  "",                           $false)
)

function Start-Sim([string]$script, [string]$device) {
    $log = Join-Path $work "log_$device.txt"
    $cmd = "set QT_QPA_PLATFORM=offscreen&& set QT_QPA_FONTDIR=%WINDIR%\Fonts&& set RS_SIM_AIR_PORT=48333&& " +
           "`"$SimExe`" --name $device --fast --script `"$script`" --quit > `"$log`" 2>&1"
    return Start-Process cmd.exe -ArgumentList "/c", $cmd -PassThru -WindowStyle Hidden
}

function Fill([string]$name, [hashtable]$ids) {
    $text = Get-Content -Raw "$scripts\$name"
    foreach ($k in $ids.Keys) { $text = $text.Replace("{$k}", $ids[$k]) }
    $dst = Join-Path $work $name
    [IO.File]::WriteAllText($dst, $text)
    return $dst
}

# ---- devices ------------------------------------------------------------------------
$idFile = Join-Path $simData "ht_ids.txt"
if ($Setup -or -not (Test-Path $idFile) -or -not (Test-Path "$simData\ht_alex")) {
    $procs = @()
    foreach ($n in "ALEX", "SAM", "JO") {
        $dev = "ht_" + $n.ToLower()
        Remove-Item -Recurse -Force "$simData\$dev" -ErrorAction SilentlyContinue
        $s = Fill "setup.txt" @{ NAME = $n }
        $s2 = Join-Path $work "setup_$n.txt"
        Move-Item -Force $s $s2
        $procs += Start-Sim $s2 $dev
    }
    $procs | Wait-Process -Timeout 180
    $lines = @()
    foreach ($n in "ALEX", "SAM", "JO") {
        $m = Select-String -Path (Join-Path $work ("log_ht_" + $n.ToLower() + ".txt")) -Pattern '\[ID\] dest=([0-9a-f]{32})'
        if (-not $m) { throw "setup of $n failed (no [ID] line)" }
        $lines += "$n=" + $m.Matches[0].Groups[1].Value
    }
    Set-Content -Encoding ascii $idFile $lines
}
$ids = @{}
foreach ($l in Get-Content $idFile) { $p = $l.Split("="); $ids[$p[0]] = $p[1] }
$ids["SAM16"] = $ids["SAM"].Substring(0, 16)

# ---- videos ------------------------------------------------------------------------
$failed = @()
foreach ($v in $videos) {
    $name, $title, $dev, $comp, $fresh = $v
    if ($Only.Count -and -not ($Only -contains $name)) { continue }
    Write-Host "== $name"
    if ($fresh) { Remove-Item -Recurse -Force "$simData\$dev" -ErrorAction SilentlyContinue }
    $c = $null
    if ($comp) { $c = Start-Sim (Fill $comp $ids) "ht_sam"; Start-Sleep 2 }
    $m = Start-Sim (Fill "$name.txt" $ids) $dev
    $m | Wait-Process -Timeout 400
    if ($c -and -not $c.HasExited) {          # the companion's simulator: stop it before the next video
        Get-CimInstance Win32_Process -Filter "Name='ratspeak-sim.exe'" |
            Where-Object { $_.CommandLine -like "*--name ht_sam *" } |
            ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
        $c | Wait-Process -Timeout 20 -ErrorAction SilentlyContinue
    }
    $log = Join-Path $work "log_$dev.txt"
    $res = Select-String -Path $log -Pattern '\[SCRIPT\] (PASS|FAIL.*)' | Select-Object -Last 1
    Write-Host ("   script: " + $(if ($res) { $res.Line } else { "no result" }))
    if (-not $res -or $res.Line -notmatch 'PASS') { $failed += $name; continue }
    & python "$PSScriptRoot\make_video.py" "$simData\$dev\shots\$name" "$Out\$name.mp4" --title $title
    if ($LASTEXITCODE) { $failed += $name }
}
if ($failed.Count) { Write-Host "FAILED: $($failed -join ', ')"; exit 1 }
Write-Host "done: $Out"

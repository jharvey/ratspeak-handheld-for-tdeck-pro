<#
How-to videos from the simulator (Windows PowerShell 5.1).

  powershell -ExecutionPolicy Bypass -File tools\howto\make_videos.ps1 [-Only atlas,peer_search]
      [-SimExe <path to ratspeak-sim.exe>] [-Out docs\videos] [-Seed <folder>]

Each video is a script in sim\scripts\howto\<name>.txt (it records with "rec <name>"), run
headless on its own simulated radio channel, with the other radios of the story running
companion scripts at the same time. tools\howto\make_video.py then adds the pointing hands,
key names, captions, notes and click sounds and writes <Out>\<name>.mp4 (two devices side by
side when the video records both). Needs ffmpeg (PATH or $env:FFMPEG) and Python + Pillow.

Every run starts the video's radios from scratch (folders <sim exe dir>\simdata\<device>):
  1. delete the folder, write sim.ini (GPS position, WiFi networks ...), copy the SD seed
     (<Seed>\<seed>\* -> the device's sd folder: map tiles, place names; default
     <sim exe dir>\howto_seed),
  2. name each radio (setup.txt) and read its address,
  3. run the scripts with {NAME} / {NAME16} replaced by the addresses (NAME = the device
     name without its ht_ / hv_ prefix, in capitals: hv_jack -> {JACK}).
#>
param(
    [string[]]$Only = @(),
    [string]$SimExe = "",
    [string]$Out = "",
    [string]$Seed = ""
)
$ErrorActionPreference = "Stop"
$Only = @($Only | ForEach-Object { $_ -split "," } | Where-Object { $_ })   # "-File" passes "a,b" as one string
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $SimExe) { $SimExe = "$repo\sim\build\ratspeak-sim.exe" }
if (-not $Out) { $Out = "$repo\docs\videos" }
if (-not $Seed) { $Seed = Join-Path (Split-Path $SimExe) "howto_seed" }
$simData = Join-Path (Split-Path $SimExe) "simdata"
$scripts = "$repo\sim\scripts\howto"
$work = Join-Path $env:TEMP "ratspeak_howto"
New-Item -ItemType Directory -Force $work, $Out | Out-Null

$alamo = "gps = 29.42600,-98.48610"
# name, title, devices (device = display name, "" = default name), main device,
# companions "script@device[+start delay s]", second recorded device + labels,
# sim.ini lines per device, SD seed folder per device
$videos = @(
    @{ name = "send_message"; title = "Send a message"; main = "ht_alex"
       devices = [ordered]@{ ht_alex = "ALEX"; ht_sam = "SAM"; ht_jo = "JO" }
       comps = @("companion_sam.txt@ht_sam") },
    @{ name = "read_message"; title = "Read a message"; main = "ht_alex"
       devices = [ordered]@{ ht_alex = "ALEX"; ht_sam = "SAM" }
       comps = @("companion_sam_send.txt@ht_sam") },
    @{ name = "add_contact"; title = "Add a contact"; main = "ht_new"
       devices = [ordered]@{ ht_new = ""; ht_sam = "SAM" }
       comps = @("companion_sam_announce.txt@ht_sam") },
    @{ name = "wifi"; title = "Connect to WiFi"; main = "ht_alex"; devices = [ordered]@{ ht_alex = "ALEX" }
       ini = @{ ht_alex = @("wifi = HomeNet/-48/TOWELDAY; Coffee-Shop/-67; Neighbour_5G/-79/SECRETPASS") } },
    @{ name = "if_found"; title = "Set the If-found details"; main = "ht_alex"; devices = [ordered]@{ ht_alex = "ALEX" } },
    @{ name = "mode_touch"; title = "Operator mode and touch"; main = "ht_alex"; devices = [ordered]@{ ht_alex = "ALEX" } },
    @{ name = "atlas"; title = "The Atlas: map, places, contacts"; main = "hv_davy"
       devices = [ordered]@{ hv_davy = "Davy" }
       ini = @{ hv_davy = @($alamo) }; seed = @{ hv_davy = "atlas" } },
    @{ name = "peer_search"; title = "Find someone: announce and Peers"; main = "hv_columbo"
       devices = [ordered]@{ hv_columbo = "Lt Columbo"; hv_franklin = "KF" }
       comps = @("peer_search_franklin.txt@hv_franklin+45") },
    @{ name = "typical_checkup"; title = "A typical check-up"; main = "hv_deep"
       devices = [ordered]@{ hv_deep = "Deep Thought"; hv_fook = "Fook"; hv_ford = "Ford"; hv_trillian = "Trillian" }
       comps = @("typical_checkup_fook.txt@hv_fook", "typical_checkup_ford.txt@hv_ford",
                 "typical_checkup_trillian.txt@hv_trillian+8") },
    @{ name = "contact_exchange"; title = "Jack and Jill swap contacts"; main = "hv_jack"
       devices = [ordered]@{ hv_jack = "Jack"; hv_jill = "Jill" }
       comps = @("contact_exchange_jill.txt@hv_jill"); rec2 = "hv_jill"; labels = @("Jack", "Jill")
       ini = @{ hv_jack = @($alamo); hv_jill = @("gps = 29.42580,-98.48590") }
       seed = @{ hv_jack = "atlas"; hv_jill = "atlas" } },
    @{ name = "add_wifi"; title = "Add a WiFi network"; main = "hv_w1aw"
       devices = [ordered]@{ hv_w1aw = "W1AW" }
       ini = @{ hv_w1aw = @("wifi = W1AW-Guest/-52; ARRL-HQ/-58/HIRAMMAXIM; QRZ-Home/-71/CQDX; Hiram-Net/-80/SPARKGAP") } }
)

function Start-Sim([string]$script, [string]$device, [int]$delay = 0) {
    $log = Join-Path $work "log_$device.txt"
    $wait = if ($delay) { "timeout /t $delay /nobreak > nul&& " } else { "" }
    $cmd = "$wait" + "set QT_QPA_PLATFORM=offscreen&& set QT_QPA_FONTDIR=%WINDIR%\Fonts&& set RS_SIM_AIR_PORT=48333&& " +
           "`"$SimExe`" --name $device --fast --script `"$script`" --quit > `"$log`" 2>&1"
    return Start-Process cmd.exe -ArgumentList "/c", $cmd -PassThru -WindowStyle Hidden
}

function Fill([string]$name, [hashtable]$ids, [string]$dst) {
    $text = Get-Content -Raw "$scripts\$name"
    foreach ($k in $ids.Keys) { $text = $text.Replace("{$k}", $ids[$k]) }
    [IO.File]::WriteAllText($dst, $text)
    return $dst
}

function Stop-Sims([string[]]$devices) {
    Get-CimInstance Win32_Process -Filter "Name='ratspeak-sim.exe'" |
        Where-Object { $cl = $_.CommandLine; @($devices | Where-Object { $cl -like "*--name $_ *" }).Count -gt 0 } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

$failed = @()
foreach ($v in $videos) {
    $name = $v.name
    if ($Only.Count -and -not ($Only -contains $name)) { continue }
    Write-Host "== $name"
    $devs = @($v.devices.Keys)
    Stop-Sims $devs
    # 1. fresh devices
    foreach ($dev in $devs) {
        $dir = "$simData\$dev"
        Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force "$dir\sd" | Out-Null
        if ($v.ini -and $v.ini[$dev]) { [IO.File]::WriteAllLines("$dir\sim.ini", [string[]]$v.ini[$dev]) }
        if ($v.seed -and $v.seed[$dev]) { Copy-Item -Recurse -Force "$Seed\$($v.seed[$dev])\*" "$dir\sd\" }
    }
    # 2. names and addresses
    $procs = @()
    foreach ($dev in $devs) {
        $procs += Start-Sim (Fill "setup.txt" @{ NAME = $v.devices[$dev] } (Join-Path $work "setup_$dev.txt")) $dev
    }
    $procs | Wait-Process -Timeout 180
    $ids = @{}
    foreach ($dev in $devs) {
        $m = Select-String -Path (Join-Path $work "log_$dev.txt") -Pattern '\[ID\] dest=([0-9a-f]{32})'
        if (-not $m) { throw "setup of $dev failed (no [ID] line)" }
        $key = ($dev -replace '^h[tv]_', '').ToUpper()
        $ids[$key] = $m.Matches[0].Groups[1].Value
        $ids[$key + "16"] = $ids[$key].Substring(0, 16)
    }
    # 3. the story
    $cs = @()
    foreach ($c in @($v.comps)) {
        if (-not $c) { continue }
        $script, $rest = $c -split "@"
        $dev, $delay = $rest -split "\+"
        $cs += Start-Sim (Fill $script $ids (Join-Path $work "$dev.$script")) $dev ([int]$delay)
    }
    if ($cs.Count) { Start-Sleep 2 }
    $m = Start-Sim (Fill "$name.txt" $ids (Join-Path $work "$name.txt")) $v.main
    $m | Wait-Process -Timeout 600
    if ($v.rec2) { $cs | Wait-Process -Timeout 120 -ErrorAction SilentlyContinue }
    Stop-Sims ($devs | Where-Object { $_ -ne $v.main })
    $res = Select-String -Path (Join-Path $work "log_$($v.main).txt") -Pattern '\[SCRIPT\] (PASS|FAIL.*)' | Select-Object -Last 1
    Write-Host ("   script: " + $(if ($res) { $res.Line } else { "no result" }))
    if (-not $res -or $res.Line -notmatch 'PASS') { $failed += $name; continue }
    $pyArgs = @("$PSScriptRoot\make_video.py", "$simData\$($v.main)\shots\$name", "$Out\$name.mp4", "--title", $v.title)
    if ($v.rec2) { $pyArgs += @("--rec2", "$simData\$($v.rec2)\shots\$name", "--labels") + $v.labels }
    & python @pyArgs
    if ($LASTEXITCODE) { $failed += $name }
}
if ($failed.Count) { Write-Host "FAILED: $($failed -join ', ')"; exit 1 }
Write-Host "done: $Out"

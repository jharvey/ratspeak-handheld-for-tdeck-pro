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
    [string]$Seed = "",
    [string]$DataRoot = "",    # the radios' folders go here instead of <sim exe dir>\simdata (e.g. off a full C:)
    [string]$Uptime = "1d2h"   # the uptime every radio starts with (sim.ini "uptime"): the idle screen shows it
)
$ErrorActionPreference = "Stop"
$Only = @($Only | ForEach-Object { $_ -split "," } | Where-Object { $_ })   # "-File" passes "a,b" as one string
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $SimExe) { $SimExe = "$repo\sim\build\ratspeak-sim.exe" }
if (-not $Out) { $Out = "$repo\docs\videos" }
if (-not $Seed) { $Seed = Join-Path (Split-Path $SimExe) "howto_seed" }
$simData = if ($DataRoot) { $DataRoot } else { Join-Path (Split-Path $SimExe) "simdata" }
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
    @{ name = "voice_message"; title = "Send a voice message"; main = "hv_jack"
       devices = [ordered]@{ hv_jack = "Jack"; hv_jill = "Jill" }
       comps = @("voice_message_jill.txt@hv_jill"); rec2 = "hv_jill"; labels = @("Jack", "Jill"); mic = $true },
    @{ name = "walkie_talkie"; title = "Walkie-talkie: talk live"; main = "hv_jack"
       devices = [ordered]@{ hv_jack = "Jack"; hv_jill = "Jill" }
       comps = @("walkie_talkie_jill.txt@hv_jill"); rec2 = "hv_jill"; labels = @("Jack", "Jill") },
    @{ name = "add_wifi"; title = "Add a WiFi network"; main = "hv_w1aw"
       devices = [ordered]@{ hv_w1aw = "W1AW" }
       ini = @{ hv_w1aw = @("wifi = W1AW-SHACK/-52; ARRL-HQ/-58/HIRAMMAXIM; QST-GUEST/-71; NEWINGTON-HAMS/-80/SPARKGAP") } },
    # --- added 22 Nov 2026 (named as in docs\videos\Video_Descriptions.txt) ---
    @{ name = "Password_lock_and_unlock"; title = "Password lock and unlock"; main = "hv_robin"
       devices = [ordered]@{ hv_robin = "ROBIN" } },
    @{ name = "Charging_and_battery"; title = "Charging and the Battery page"; main = "hv_robin"
       devices = [ordered]@{ hv_robin = "ROBIN" } },
    @{ name = "Announce_your_unit"; title = "Announce your unit"; main = "hv_robin"
       devices = [ordered]@{ hv_robin = "ROBIN"; hv_sage = "SAGE" }
       comps = @("Announce_your_unit_sage.txt@hv_sage") },
    @{ name = "Energy_modes"; title = "Energy modes"; main = "hv_robin"
       devices = [ordered]@{ hv_robin = "ROBIN" } },
    @{ name = "Low_battery_notice"; title = "Low-battery notice"; main = "hv_robin"
       devices = [ordered]@{ hv_robin = "ROBIN"; hv_sage = "SAGE" }
       comps = @("Low_battery_notice_sage.txt@hv_sage"); rec2 = "hv_sage"; labels = @("Robin", "Sage") },
    @{ name = "Battery_empty_wont_turn_on"; title = "Battery empty: it will not turn on"; main = "hv_robin"
       devices = [ordered]@{ hv_robin = "ROBIN" } },
    @{ name = "Share_your_location"; title = "Share your location"; main = "hv_robin"
       devices = [ordered]@{ hv_robin = "ROBIN"; hv_sage = "SAGE" }
       comps = @("Share_your_location_sage.txt@hv_sage"); rec2 = "hv_sage"; labels = @("Robin", "Sage")
       ini = @{ hv_robin = @($alamo); hv_sage = @("gps = 29.43500,-98.48000") }
       seed = @{ hv_robin = "atlas"; hv_sage = "atlas" } },
    @{ name = "Atlas_waypoints"; title = "Atlas: waypoints"; main = "hv_robin"
       devices = [ordered]@{ hv_robin = "ROBIN" }; ini = @{ hv_robin = @($alamo) }; seed = @{ hv_robin = "atlas" } },
    @{ name = "Atlas_breadcrumb_track"; title = "Atlas: the breadcrumb track"; main = "hv_robin"
       devices = [ordered]@{ hv_robin = "ROBIN" }; ini = @{ hv_robin = @($alamo) }; seed = @{ hv_robin = "atlas" } },
    @{ name = "Navigate_to_a_contact"; title = "Navigate to a contact"; main = "hv_robin"
       devices = [ordered]@{ hv_robin = "ROBIN" }; ini = @{ hv_robin = @($alamo) }; seed = @{ hv_robin = "atlas" } },
    @{ name = "Arrival_alerts"; title = "Arrival alerts"; main = "hv_robin"
       devices = [ordered]@{ hv_robin = "ROBIN" }; ini = @{ hv_robin = @($alamo) }; seed = @{ hv_robin = "atlas" } },
    # seed "library" = tools\sdcard\library\*.txt in <seed>\library\ratdeckpro\library;
    # seed "fw" = a firmware .bin (firmware\tdeck_pro_standalone.bin) in <seed>\fw\firmware
    @{ name = "SD_backup_and_restore"; title = "SD card: back up and restore"; main = "hv_robin"
       devices = [ordered]@{ hv_robin = "ROBIN" }; seed = @{ hv_robin = "library" } },
    @{ name = "Firmware_update_from_SD"; title = "Firmware update from the SD card"; main = "hv_robin"
       devices = [ordered]@{ hv_robin = "ROBIN" }; seed = @{ hv_robin = "fw" } },
    @{ name = "Range_test"; title = "Range test"; main = "hv_robin"
       devices = [ordered]@{ hv_robin = "ROBIN"; hv_sage = "SAGE" }
       comps = @("Range_test_sage.txt@hv_sage"); seed = @{ hv_robin = "library" } },
    @{ name = "Library_and_games"; title = "Library and games"; main = "hv_robin"
       devices = [ordered]@{ hv_robin = "ROBIN" }; seed = @{ hv_robin = "library" } },
    @{ name = "Settings_tour"; title = "A tour of Settings"; main = "hv_robin"
       devices = [ordered]@{ hv_robin = "ROBIN" }; seed = @{ hv_robin = "library" } },
    # --- the instructional series (docs\instructional\Instructional_videos.txt): run these with
    #     -Out docs\instructional ---
    @{ name = "02_Everyday_messaging_Minimal_mode"; title = "Everyday messaging in Minimal mode"; main = "hv_granny"
       devices = [ordered]@{ hv_pip = "PIP"; hv_granny = "GRANNY GOUDA" }
       comps = @("02_Everyday_messaging_Minimal_mode_pip.txt@hv_pip"); rec2 = "hv_pip"; labels = @("Granny Gouda", "Pip"); colours = @(3, 0) },
    @{ name = "20_Out_of_range_and_back"; title = "Out of range and back"; main = "hv_mabel"
       devices = [ordered]@{ hv_mabel = "MABEL"; hv_pip = "PIP" }
       comps = @("20_Out_of_range_and_back_pip.txt@hv_pip+75"); rec2 = "hv_pip"; labels = @("Mabel", "Pip"); colours = @(1, 0) },
    @{ name = "31_USB_power_and_charging"; title = "USB power and charging"; main = "hv_nibbles"
       devices = [ordered]@{ hv_nibbles = "PROF NIBBLES"; hv_mabel = "MABEL" }
       comps = @("31_USB_power_and_charging_mabel.txt@hv_mabel"); rec2 = "hv_mabel"
       labels = @("Professor Nibbles", "Mabel"); colours = @(2, 1)
       ini = @{ hv_nibbles = @("gps = 51.28260,-2.76750"); hv_mabel = @("gps = 51.28300,-2.76500") } }
)

function Start-Sim([string]$script, [string]$device, [int]$delay = 0) {
    $log = Join-Path $work "log_$device.txt"
    # (ping as the delay: "timeout" refuses to run without a console, e.g. from a scheduled task)
    $wait = if ($delay) { "ping -n $($delay + 1) 127.0.0.1 > nul& " } else { "" }
    $cmd = "$wait" + "set QT_QPA_PLATFORM=offscreen&& set QT_QPA_FONTDIR=%WINDIR%\Fonts&& set RS_SIM_AIR_PORT=48333&& set SIM_AUDIO_DIR=$scripts\audio&& " +
           "`"$SimExe`" --name $device --data `"$simData\$device`" --fast --script `"$script`" --quit > `"$log`" 2>&1"
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
        # (on a network share a delete can lag: wait until the folder is gone, and try the copy again)
        for ($try = 0; $try -lt 20 -and (Test-Path $dir); $try++) {
            Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
            if (Test-Path $dir) { Start-Sleep -Milliseconds 500 }
        }
        New-Item -ItemType Directory -Force "$dir\sd" | Out-Null
        # Every radio has been up about a day (the idle screen shows the uptime), plus the video's own lines.
        $ini = @("uptime = $Uptime")
        if ($v.ini -and $v.ini[$dev]) { $ini += $v.ini[$dev] }
        [IO.File]::WriteAllLines("$dir\sim.ini", [string[]]$ini)
        if ($v.seed -and $v.seed[$dev]) {
            for ($try = 0; ; $try++) {
                try { Copy-Item -Recurse -Force "$Seed\$($v.seed[$dev])\*" "$dir\sd\"; break }
                catch { if ($try -ge 3) { throw }; Start-Sleep 2 }
            }
        }
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
    if ($v.mic) { $pyArgs += "--mic" }      # the clean voice too (not on calls: it echoes)
    if ($v.rec2) { $pyArgs += @("--rec2", "$simData\$($v.rec2)\shots\$name", "--labels") + $v.labels }
    if ($v.colours) { $pyArgs += @("--label-colours") + $v.colours }   # each person keeps a colour
    # the scripts give the size of each "point" box: the main one, then the second recorded device's
    $sc2 = @($v.comps | Where-Object { $_ -and $v.rec2 -and ($_ -split "@")[1] -replace "\+.*", "" -eq $v.rec2 } |
             ForEach-Object { "$scripts\" + ($_ -split "@")[0] })
    $pyArgs += @("--scripts", "$scripts\$name.txt") + $sc2
    & python @pyArgs
    if ($LASTEXITCODE) { $failed += $name }
}
if ($failed.Count) { Write-Host "FAILED: $($failed -join ', ')"; exit 1 }
Write-Host "done: $Out"

<#
Spoken clips for the how-to videos, with the Windows built-in text-to-speech voices
(no download): every line of sim\scripts\howto\audio\lines.txt ("file | voice | text")
becomes sim\scripts\howto\audio\<file>.wav, 16 kHz mono 16-bit (what the simulated
microphone plays: script command "mic <file>.wav").

  powershell -ExecutionPolicy Bypass -File tools\howto\make_speech.ps1

Voices: "David" and "Zira" (Microsoft David / Zira Desktop, en-US) are on every Windows 10.
#>
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Speech
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$dir = "$repo\sim\scripts\howto\audio"
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
$voices = (New-Object System.Speech.Synthesis.SpeechSynthesizer).GetInstalledVoices() | ForEach-Object { $_.VoiceInfo.Name }
foreach ($line in Get-Content "$dir\lines.txt") {
    if ($line -match '^\s*#' -or -not $line.Trim()) { continue }
    $p = $line -split '\|', 3
    if ($p.Count -lt 3) { continue }
    $file, $voice, $text = $p[0].Trim(), $p[1].Trim(), $p[2].Trim()
    $name = $voices | Where-Object { $_ -like "*$voice*" } | Select-Object -First 1
    if (-not $name) { throw "no voice like '$voice' (installed: $($voices -join ', '))" }
    $s = New-Object System.Speech.Synthesis.SpeechSynthesizer
    $s.SelectVoice($name)
    $s.Rate = 0
    $s.SetOutputToWaveFile("$dir\$file.wav", $fmt)
    $s.Speak($text)
    $s.Dispose()
    Write-Host ("{0}.wav  {1}  {2}" -f $file, $voice, $text)
}

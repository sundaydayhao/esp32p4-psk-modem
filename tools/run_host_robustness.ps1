param(
    [string]$WavDir = "C:\projects\psk-script\generated-wav-48000",
    [string]$Expected = "C:\projects\psk-script\input.txt"
)

$ErrorActionPreference = "Stop"
$runner = Join-Path $PSScriptRoot "run_host_regression.ps1"
$saved = @{
    PSK_INPUT_GAIN = $env:PSK_INPUT_GAIN
    PSK_NOISE_PEAK = $env:PSK_NOISE_PEAK
    PSK_RX_TONE_HZ = $env:PSK_RX_TONE_HZ
    PSK_AFC = $env:PSK_AFC
}
$scenarios = @(
    @{ Name = "low-level"; Gain = "0.20"; Noise = "0"; Tone = "1500" },
    @{ Name = "noisy"; Gain = "0.35"; Noise = "600"; Tone = "1500" },
    @{ Name = "frequency-offset"; Gain = "1.0"; Noise = "0"; Tone = "1497" }
)

$failed = 0
try {
    foreach ($scenario in $scenarios) {
        $env:PSK_INPUT_GAIN = $scenario.Gain
        $env:PSK_NOISE_PEAK = $scenario.Noise
        $env:PSK_RX_TONE_HZ = $scenario.Tone
        $env:PSK_AFC = "1"
        Write-Output ""
        Write-Output "SCENARIO=$($scenario.Name) gain=$($scenario.Gain) noise_peak=$($scenario.Noise) tone=$($scenario.Tone)"
        & powershell -ExecutionPolicy Bypass -File $runner -WavDir $WavDir -Expected $Expected
        if ($LASTEXITCODE -ne 0) {
            $failed++
        }
    }
} finally {
    foreach ($name in $saved.Keys) {
        if ($null -eq $saved[$name]) {
            Remove-Item "Env:$name" -ErrorAction SilentlyContinue
        } else {
            Set-Item "Env:$name" $saved[$name]
        }
    }
}

Write-Output ""
Write-Output "ROBUSTNESS_SCENARIOS=$($scenarios.Count) FAILED=$failed"
if ($failed -ne 0) {
    exit 1
}

#Requires -Version 7.2
param([string]$OutputRoot = 'logs/scheduler-investigation-new')
$ErrorActionPreference = 'Stop'
$env:EMSDK_QUIET = '1'
. C:/Env/clion_env.ps1
$env:PATH = "$env:UCRT64_HOME/bin;$env:PATH"
$env:IGAME_DATACODEC_SCHEDULER_TRACE = '1'
$benchmark = './cmake-build-release-dev/iGameCore/iGameDataCodecFileBenchmark.exe'
$inputFile = 'logs/resource-parameters-20260913/memory_reserve20_r1.igc'
if (!(Test-Path -LiteralPath $inputFile)) { throw '调查使用的完整 IGC 不存在' }
if (Test-Path -LiteralPath $OutputRoot) { throw '输出目录已存在，请使用新目录' }
New-Item -ItemType Directory -Path $OutputRoot | Out-Null
& $benchmark --investigate-scheduler *> "$OutputRoot/mechanism.log"
Get-Content "$OutputRoot/mechanism.log"
if ($LASTEXITCODE -ne 0) { throw '调度机制核验失败' }
foreach ($case in @(
    @{ Name = 'fixed1024'; Option = '--investigate-decode-fixed'; Value = '1024' },
    @{ Name = 'adaptive25'; Option = '--investigate-decode'; Value = '25' },
    @{ Name = 'fixed1024_final_gate'; Option = '--investigate-decode-gated'; Value = '1024' }
)) {
    $log = Join-Path $OutputRoot "$($case.Name).log"
    & $benchmark $case.Option $inputFile $case.Value *> $log
    $code = $LASTEXITCODE
    Write-Output "END $($case.Name) exit=$code"
    $lines = Get-Content -LiteralPath $log
    $lines | Where-Object { $_ -match '^(RESULT|MEMTRACE_END|SHAPE)' } | Write-Output
    if ($code -ne 0 -and !($lines -match '^RESULT decode_success=0 ')) { throw '调查程序异常中断' }
}

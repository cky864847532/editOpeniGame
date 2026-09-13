#Requires -Version 7.2
param([string]$OutputRoot = 'logs/block-response-20260913')
$ErrorActionPreference = 'Stop'
$env:EMSDK_QUIET = '1'
. C:/Env/clion_env.ps1
$env:PATH = "$env:UCRT64_HOME/bin;$env:PATH"
$env:IGAME_DATACODEC_SCHEDULER_TRACE = '1'
$benchmark = './cmake-build-release-dev/iGameCore/iGameDataCodecFileBenchmark.exe'
$inputFile = 'logs/resource-parameters-20260913/memory_reserve20_r1.igc'
if (!(Test-Path -LiteralPath $benchmark) -or !(Test-Path -LiteralPath $inputFile)) { throw '调查程序或完整 IGC 不存在' }
if (Test-Path -LiteralPath $OutputRoot) { throw '输出目录已存在，请使用新目录' }
New-Item -ItemType Directory -Path $OutputRoot | Out-Null
foreach ($entry in @('scheduler', 'memory-controller')) {
    $log = Join-Path $OutputRoot "$entry.log"
    & $benchmark "--investigate-$entry" *> $log
    if ($LASTEXITCODE -ne 0) { throw "机制检查失败：$entry" }
    Write-Output "CHECK $entry passed"
}
# 各组顺序运行，避免调查进程之间争用内存
foreach ($case in @(
    @{Name='fixed2048_t1_r1'; Mode='fixed'; Value=2048; Threads=1},
    @{Name='adaptive20_t1_r1'; Mode='adaptive'; Value=20; Threads=1},
    @{Name='probe20_t1_r1'; Mode='probe'; Value=20; Threads=1},
    @{Name='adaptive25_t1_r1'; Mode='adaptive'; Value=25; Threads=1},
    @{Name='probe25_t1_r1'; Mode='probe'; Value=25; Threads=1},
    @{Name='fixed2048_t16_r1'; Mode='fixed'; Value=2048; Threads=16},
    @{Name='adaptive20_t16_r1'; Mode='adaptive'; Value=20; Threads=16},
    @{Name='adaptive25_t16_r1'; Mode='adaptive'; Value=25; Threads=16},
    @{Name='fixed1024_t1_r1'; Mode='fixed'; Value=1024; Threads=1},
    @{Name='fixed2048_t1_r2'; Mode='fixed'; Value=2048; Threads=1},
    @{Name='probe20_t1_r2'; Mode='probe'; Value=20; Threads=1},
    @{Name='fixed2048_stream_t1_r1'; Mode='fixed'; Value=2048; Threads=1; Reader='stream'},
    @{Name='adaptive20_stream_t1_r1'; Mode='adaptive'; Value=20; Threads=1; Reader='stream'},
    @{Name='adaptive20_mapped_t1_r2'; Mode='adaptive'; Value=20; Threads=1},
    @{Name='fixed2048_stream_t1_r2'; Mode='fixed'; Value=2048; Threads=1; Reader='stream'},
    @{Name='probe22_t1_r1'; Mode='probe'; Value=22; Threads=1},
    @{Name='adaptive20_stream_t1_r2'; Mode='adaptive'; Value=20; Threads=1; Reader='stream'}
)) {
    $log = Join-Path $OutputRoot "$($case.Name).log"
    Write-Output "START $($case.Name)"
    $arguments = @('--investigate-response', $inputFile, $case.Mode, $case.Value, $case.Threads)
    if ($case.Reader) { $arguments += $case.Reader }
    & $benchmark @arguments *> $log
    $code = $LASTEXITCODE
    Write-Output "END $($case.Name) exit=$code"
    $lines = Get-Content -LiteralPath $log -Encoding UTF8
    $lines | Where-Object { $_ -match 'RESPONSE_END|^RESULT|^SHAPE' } | Write-Output
    if ($code -ne 0 -and !($lines -match '^RESULT decode_success=')) { throw '调查程序异常中断' }
}

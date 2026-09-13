#Requires -Version 7.2
param([string]$OutputRoot = 'logs/memory-investigation-20260913')
$ErrorActionPreference = 'Stop'
$env:EMSDK_QUIET = '1'
. C:/Env/clion_env.ps1
$env:PATH = "$env:UCRT64_HOME/bin;$env:PATH"
$benchmark = './cmake-build-release-dev/iGameCore/iGameDataCodecFileBenchmark.exe'
$inputFile = 'E:/CAE_data/Driver/10gb_csgn/car_3800W_Fluent_node_HDF5-0001.cgns'
if (Test-Path -LiteralPath $OutputRoot) { throw '调查输出目录已存在，请使用新目录' }
New-Item -ItemType Directory -Path $OutputRoot | Out-Null
& $benchmark --investigate-memory-controller *> "$OutputRoot/controller.log"
Get-Content "$OutputRoot/controller.log"
if ($LASTEXITCODE -ne 0) { throw '控制器行为核验未通过' }
foreach ($target in @(20, 25)) {
    Write-Output "START reserve$target"
    $arguments = @($inputFile, "$OutputRoot/reserve$target.igc", '--memory', 'adaptive',
        '--threads', '16', '--memory-reserve', "$target", '--memory-trace', 'on')
    & $benchmark @arguments *> "$OutputRoot/reserve$target.log"
    $code = $LASTEXITCODE
    Write-Output "END reserve$target exit=$code"
    $lines = Get-Content "$OutputRoot/reserve$target.log"
    $lines | Where-Object { $_ -match '^(RESULT|MEMTRACE_END)' } | Write-Output
    if ($code -ne 0 -and -not ($lines -match '^RESULT (encode|decode)_success=0 ')) {
        throw '程序异常中断，停止调查'
    }
}
# 编码可能在读取宿主源对象后被目标阻止，独立解码使用已经成功写出的文件
$decodeInput = "$OutputRoot/reserve20.igc"
$encode20Succeeded = (Get-Content "$OutputRoot/reserve20.log") -match '^RESULT encode_success=1 '
if ($encode20Succeeded) {
    & $benchmark --investigate-decode $decodeInput 25 *> "$OutputRoot/reserve25_decode.log"
    $code = $LASTEXITCODE
    Write-Output "END reserve25_decode exit=$code"
    $lines = Get-Content "$OutputRoot/reserve25_decode.log"
    $lines | Where-Object { $_ -match '^(RESULT|MEMTRACE_END|SHAPE)' } | Write-Output
    if ($code -ne 0 -and -not ($lines -match '^RESULT decode_success=0 ')) { throw '独立解码异常中断' }
}

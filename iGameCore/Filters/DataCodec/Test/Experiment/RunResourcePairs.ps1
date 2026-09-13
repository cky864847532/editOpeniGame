#Requires -Version 7.2
param(
    [ValidateSet('Warmup', 'CPU', 'Memory')]
    [string]$Phase = 'CPU',
    [int]$Repetitions = 3,
    [double[]]$MemoryTargets = @(20, 25),
    [string]$OutputRoot = 'logs/resource-parameters-20260913',
    [string]$InputFile = 'E:/CAE_data/Driver/10gb_csgn/car_3800W_Fluent_node_HDF5-0001.cgns'
)
$ErrorActionPreference = 'Stop'
$env:EMSDK_QUIET = '1'
. C:/Env/clion_env.ps1
$env:PATH = "$env:UCRT64_HOME/bin;$env:PATH"
$benchmark = (Resolve-Path 'cmake-build-release-dev/iGameCore/iGameDataCodecFileBenchmark.exe').Path
$root = [System.IO.Path]::GetFullPath($OutputRoot)
[System.IO.Directory]::CreateDirectory($root) | Out-Null
$groups = @(switch ($Phase) {
    'Warmup' { @{ Name = 'warmup'; Arguments = @('--memory', 'unlimited', '--threads', '16') } }
    'CPU' {
        @{ Name = 'cpu_fixed4'; Arguments = @('--memory', 'unlimited', '--threads', '4') }
        @{ Name = 'cpu_fixed16'; Arguments = @('--memory', 'unlimited', '--threads', '16') }
        foreach ($target in @(20, 50, 80)) {
            @{ Name = "cpu_idle$target"; Arguments = @('--memory', 'unlimited', '--cpu-idle', "$target") }
        }
    }
    'Memory' {
        foreach ($target in $MemoryTargets) {
            @{ Name = "memory_reserve$target"; Arguments = @('--memory', 'adaptive', '--memory-reserve', "$target", '--threads', '16') }
        }
    }
})
for ($round = 1; $round -le $Repetitions; ++$round) {
    for ($index = 0; $index -lt $groups.Count; ++$index) {
        $group = $groups[($index + $round - 1) % $groups.Count]
        $name = "$($group.Name)_r$round"
        $output = Join-Path $root "$name.igc"
        $log = Join-Path $root "$name.log"
        $resultFile = Join-Path $root "$name.run.json"
        if (Test-Path -LiteralPath $resultFile) { Write-Output "SKIP $name"; continue }
        if (Test-Path -LiteralPath $output) { throw "实验输出已存在：$name" }
        if (-not (Test-Path -LiteralPath $benchmark)) { throw '测速程序已不存在，停止实验' }
        Write-Output "START $name"
        $started = Get-Date
        $arguments = @($InputFile, $output) + $group.Arguments
        & $benchmark @arguments *> $log
        $code = $LASTEXITCODE
        [ordered]@{ name = $name; phase = $Phase; round = $round; arguments = $arguments;
            started = $started.ToString('o'); ended = (Get-Date).ToString('o'); exitCode = $code } |
            ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $resultFile -Encoding utf8
        Write-Output "END $name exit=$code"
        $lines = Get-Content -LiteralPath $log
        $lines | Where-Object { $_ -match '^RESULT ' } | Write-Output
        if ($code -ne 0 -and -not ($lines -match '^RESULT (encode|decode)_success=0 ')) {
            throw "测试异常中断：$name，退出码 $code"
        }
    }
}

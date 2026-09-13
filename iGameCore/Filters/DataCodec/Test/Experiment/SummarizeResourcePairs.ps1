#Requires -Version 7.2
param([string]$OutputRoot = 'logs/resource-parameters')
$ErrorActionPreference = 'Stop'
$rows = @(Get-ChildItem -LiteralPath $OutputRoot -Filter '*.run.json' | ForEach-Object {
    $run = Get-Content -LiteralPath $_.FullName -Raw -Encoding utf8 | ConvertFrom-Json
    $log = Get-Content -LiteralPath (Join-Path $OutputRoot "$($run.name).log") -Encoding utf8
    $encode = $log | Where-Object { $_ -match '^RESULT encode_success=' } | Select-Object -Last 1
    $decode = $log | Where-Object { $_ -match '^RESULT decode_success=' } | Select-Object -Last 1
    $shape = $log | Where-Object { $_ -match '^RESULT shape_matches=' } | Select-Object -Last 1
    $field = { param($text, $key)
        if ($text -match "(?:^| )$key=([^ ]+)") { return $Matches[1] }
        return $null
    }
    [pscustomobject][ordered]@{
        name = $run.name
        group = $run.name -replace '_r\d+$', ''
        round = $run.round
        exit_code = $run.exitCode
        encode_success = & $field $encode 'encode_success'
        decode_success = & $field $decode 'decode_success'
        encode_seconds = & $field $encode 'encode_seconds'
        decode_seconds = & $field $decode 'decode_seconds'
        encode_cpu_seconds = & $field $encode 'encode_cpu_seconds'
        decoded_cache_hit = & $field $decode 'decoded_cache_hit'
        shape_matches = & $field $shape 'shape_matches'
        output_bytes = & $field $encode 'encoded_bytes'
    }
})
$rows | Export-Csv -LiteralPath (Join-Path $OutputRoot 'results.csv') -NoTypeInformation -Encoding utf8
$rows | Group-Object group | ForEach-Object {
    $successful = @($_.Group | Where-Object { $_.exit_code -eq 0 -and $_.shape_matches -eq '1' })
    $encode = @($_.Group | Where-Object { $_.encode_success -eq '1' } | ForEach-Object { [double]$_.encode_seconds } | Sort-Object)
    $decode = @($_.Group | Where-Object { $_.decode_success -eq '1' } | ForEach-Object { [double]$_.decode_seconds } | Sort-Object)
    $median = { param($values)
        if ($values.Count -eq 0) { return $null }
        if ($values.Count % 2) { return $values[[int][math]::Floor($values.Count / 2)] }
        return ($values[$values.Count / 2 - 1] + $values[$values.Count / 2]) / 2
    }
    [pscustomobject][ordered]@{
        group = $_.Name
        completed = $successful.Count
        attempts = $_.Count
        encode_completed = $encode.Count
        decode_completed = $decode.Count
        encode_median = & $median $encode
        encode_min = ($encode | Select-Object -First 1)
        encode_max = ($encode | Select-Object -Last 1)
        decode_median = & $median $decode
        decode_min = ($decode | Select-Object -First 1)
        decode_max = ($decode | Select-Object -Last 1)
    }
} | ConvertTo-Json

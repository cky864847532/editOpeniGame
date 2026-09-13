#Requires -Version 7.2
param([string]$OutputRoot = 'logs/memory-investigation-20260913')
$ErrorActionPreference = 'Stop'
$summary = foreach ($file in Get-ChildItem -LiteralPath $OutputRoot -Filter 'reserve*.log') {
    $rows = @(Get-Content -LiteralPath $file.FullName | Where-Object { $_.StartsWith('MEMTRACE ') } | ForEach-Object {
        $row = [ordered]@{}
        foreach ($part in $_.Substring(9).Split(' ')) {
            $pair = $part.Split('=', 2)
            $number = 0.0
            if ($pair[1] -eq 'null') { $row[$pair[0]] = $null }
            elseif ([double]::TryParse($pair[1], [Globalization.NumberStyles]::Float,
                    [Globalization.CultureInfo]::InvariantCulture, [ref]$number)) { $row[$pair[0]] = $number }
            else { $row[$pair[0]] = $pair[1] }
        }
        [pscustomobject]$row
    })
    foreach ($stage in $rows | Group-Object stage) {
        $data = @($stage.Group)
        $closed = 0.0; $idle = 0.0; $band = 0.0; $transitions = 0
        $segments = [Collections.Generic.List[object]]::new()
        $segmentStart = 0
        for ($i = 0; $i -lt $data.Count; ++$i) {
            $r = $data[$i]
            if ($i + 1 -lt $data.Count) {
                $dt = $data[$i + 1].t - $r.t
                if ($r.gate -eq 0) {
                    $closed += $dt
                    if ($r.admitted -eq 0 -and $r.computing -eq 0 -and $r.queued -eq 0) { $idle += $dt }
                    if ($null -ne $r.available -and $r.available -ge $r.reserve -and $r.available -lt $r.recovery) { $band += $dt }
                }
            }
            if ($i -gt 0 -and $r.gate -ne $data[$i - 1].gate) { ++$transitions }
            if ($i + 1 -eq $data.Count -or $data[$i + 1].phase -ne $r.phase -or $data[$i + 1].gate -ne $r.gate) {
                $chunk = @($data[$segmentStart..$i])
                $segments.Add([pscustomobject]@{
                    start = $chunk[0].t; end = $r.t; seconds = $r.t - $chunk[0].t
                    phase = $r.phase; gate = $r.gate
                    available_min = ($chunk.available | Measure-Object -Minimum).Minimum
                    available_max = ($chunk.available | Measure-Object -Maximum).Maximum
                    reserved_min = ($chunk.reserved | Measure-Object -Minimum).Minimum
                    reserved_max = ($chunk.reserved | Measure-Object -Maximum).Maximum
                    max_admitted = ($chunk.admitted | Measure-Object -Maximum).Maximum
                    max_computing = ($chunk.computing | Measure-Object -Maximum).Maximum
                    max_queued = ($chunk.queued | Measure-Object -Maximum).Maximum
                    last = $r
                })
                $segmentStart = $i + 1
            }
        }
        $lastUpdate = -1.0
        $gains = @($data | ForEach-Object {
            if ($_.gain_updates -ne $lastUpdate) {
                $lastUpdate = $_.gain_updates
                $_ | Select-Object t,gain,gain_updates,response,baseline,response_available,measured,calibration,calibration_ms
            }
        })
        $lastCalibration = ''
        $calibrations = @($data | ForEach-Object {
            if ($_.calibration -ne $lastCalibration) {
                $lastCalibration = $_.calibration
                $_ | Select-Object t,calibration,calibration_ms,measured,grant,gain,gain_updates
            }
        })
        [pscustomobject]@{
            file = $file.Name; stage = $stage.Name; samples = $data.Count
            observed_seconds = $data[-1].t - $data[0].t
            gate_transitions = $transitions; closed_seconds_approx = $closed
            closed_idle_seconds_approx = $idle; closed_deadband_seconds_approx = $band
            gain_min = ($data.gain | Measure-Object -Minimum).Minimum
            gain_max = ($data.gain | Measure-Object -Maximum).Maximum
            gains = $gains; calibrations = $calibrations; segments = @($segments)
        }
    }
}
$summary | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath "$OutputRoot/summary.json" -Encoding utf8
$summary | Select-Object file,stage,samples,observed_seconds,gate_transitions,closed_seconds_approx,
    closed_idle_seconds_approx,closed_deadband_seconds_approx,gain_min,gain_max | ConvertTo-Json

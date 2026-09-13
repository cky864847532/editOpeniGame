#Requires -Version 7.2
param([string]$InputRoot = 'logs/block-response-20260913')
$ErrorActionPreference = 'Stop'
function Convert-Fields([string]$Line) {
    $row = @{}
    foreach ($item in [regex]::Matches($Line, '(\w+)=([^ ]+)')) {
        $value = $item.Groups[2].Value
        $number = 0.0
        if ([double]::TryParse($value, [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$number)) { $row[$item.Groups[1].Value] = $number }
        else { $row[$item.Groups[1].Value] = $value }
    }
    return [pscustomobject]$row
}
function Stats($Values) {
    $ordered = @($Values | Sort-Object)
    if ($ordered.Count -eq 0) { return $null }
    return @{Min=$ordered[0]; Median=$ordered[[int][math]::Floor(($ordered.Count - 1)*0.5)];
        P95=$ordered[[int][math]::Floor(($ordered.Count - 1)*0.95)]; Max=$ordered[-1]}
}
$results = foreach ($file in Get-ChildItem -LiteralPath $InputRoot -Filter '*.log') {
    $lines = Get-Content -LiteralPath $file.FullName -Encoding UTF8
    $endLine = $lines | Where-Object { $_ -match 'RESPONSE_END ' } | Select-Object -Last 1
    if (!$endLine) { continue }
    $end = Convert-Fields $endLine
    $rows = @($lines | Where-Object { $_ -match ' RESPONSE point=' } | ForEach-Object { Convert-Fields $_ })
    $samples = @($rows | Where-Object { $_.point -eq 'sample' -and $_.valid -eq 1 })
    $blocks = foreach ($group in ($rows | Where-Object plan -GT 0 | Group-Object flow,block)) {
        $before = $group.Group | Where-Object point -EQ 'plan' | Select-Object -First 1
        $allocated = $group.Group | Where-Object point -EQ 'allocated' | Select-Object -First 1
        $committed = $group.Group | Where-Object point -EQ 'commit.end' | Select-Object -First 1
        $retired = $group.Group | Where-Object point -EQ 'retired' | Select-Object -First 1
        if (!$before -or !$allocated -or !$retired) { continue }
        $peak = ($group.Group.ws | Measure-Object -Maximum).Maximum
        $computeBegin = $group.Group | Where-Object point -EQ 'compute.begin' | Select-Object -First 1
        $computeEnd = $group.Group | Where-Object point -EQ 'compute.end' | Select-Object -First 1
        $commitBegin = $group.Group | Where-Object point -EQ 'commit.begin' | Select-Object -First 1
        if (!$computeBegin -or !$computeEnd -or !$commitBegin -or !$committed) { continue }
        [pscustomobject]@{Flow=$before.flow; Block=$before.block; Plan=$before.plan; Missing=$allocated.missing;
            Acquired=$allocated.acquired-$before.acquired; PeakWsDelta=$peak-$before.ws;
            EndWsDelta=$retired.ws-$before.ws; PrivateDelta=$retired.private-$before.private;
            ComputeWsDelta=$computeEnd.ws-$computeBegin.ws; CommitWsDelta=$committed.ws-$commitBegin.ws;
            PlanMs=$before.ms; RetiredMs=$retired.ms}
    }
    $cold = @($blocks | Where-Object Missing -GT 0)
    $reuse = @($blocks | Where-Object Missing -EQ 0)
    $partialReuse = @($blocks | Where-Object { $_.Missing -lt $_.Plan })
    $lowSeconds=0.0; $closedSeconds=0.0; $noCapacitySeconds=0.0
    $waitLowSeconds=0.0; $waitDeadbandSeconds=0.0; $waitHighSeconds=0.0; $gateTransitions=0
    for ($i=1; $i -lt $samples.Count; ++$i) {
        $a=$samples[$i-1]; $b=$samples[$i]; $dt=($b.ms-$a.ms)/1000
        if ($a.reserve -gt 0 -and $a.available -lt $a.reserve) { $lowSeconds += $dt }
        if ($a.gate -eq 0) { $closedSeconds += $dt }
        if ($a.gate -ne $b.gate) { ++$gateTransitions }
        if ($a.next -gt 0 -and $a.admitted -eq 0) {
            $noCapacitySeconds += $dt
            if ($a.available -lt $a.reserve) { $waitLowSeconds += $dt }
            elseif ($a.available -lt $a.recovery) { $waitDeadbandSeconds += $dt }
            else { $waitHighSeconds += $dt }
        }
    }
    $probeRows = @($rows | Where-Object { $_.point -like 'probe.*' })
    $hostBegin = $rows | Where-Object point -EQ 'host.prepare.begin' | Select-Object -First 1
    $hostEnd = $rows | Where-Object point -EQ 'host.prepare.end' | Select-Object -First 1
    $lastRetired = $rows | Where-Object point -EQ 'retired' | Select-Object -Last 1
    # 阶段边界使用实际记录，不把失败清理时释放的数 GiB 当作计算期采样偏差
    $phaseStats = foreach ($phase in @(
        @{Name='host.prepare'; Begin=$hostBegin; End=$hostEnd},
        @{Name='blocks.through.last.observed.retired'; Begin=$hostEnd; End=$lastRetired}
    )) {
        if (!$phase.Begin -or !$phase.End -or $phase.End.ms -lt $phase.Begin.ms) { continue }
        $phaseSamples = @($samples | Where-Object { $_.ms -ge $phase.Begin.ms -and $_.ms -le $phase.End.ms })
        [pscustomobject]@{Name=$phase.Name; BeginMs=$phase.Begin.ms; EndMs=$phase.End.ms; Samples=$phaseSamples.Count;
            AvailableSampleError=Stats @($phaseSamples | ForEach-Object { $_.sample_available-$_.available });
            SampleAgeMs=Stats $phaseSamples.sample_age_ms}
    }
    [pscustomobject]@{
        Name=$file.BaseName; End=$end; Result=($lines | Where-Object { $_ -like 'RESULT *' } | Select-Object -Last 1);
        Samples=$samples.Count; Available=Stats $samples.available; WorkingSet=Stats $samples.ws;
        Private=Stats $samples.private; Controlled=Stats $samples.reserved;
        AvailableSampleError=Stats @($samples | ForEach-Object { $_.sample_available-$_.available });
        SampleAgeMs=Stats $samples.sample_age_ms; MaxAdmitted=($samples.admitted | Measure-Object -Maximum).Maximum;
        BelowReserveSeconds=$lowSeconds; GateClosedSeconds=$closedSeconds; IdleByteWaitSeconds=$noCapacitySeconds;
        GateTransitions=$gateTransitions; WaitBelowReserveSeconds=$waitLowSeconds;
        WaitBetweenWatermarksSeconds=$waitDeadbandSeconds; WaitAboveRecoverySeconds=$waitHighSeconds;
        PhaseStats=@($phaseStats);
        PreviewRatio=Stats @($samples | Where-Object { $null -ne $_.preview_ratio } | ForEach-Object preview_ratio);
        Gain=Stats @($samples | Where-Object { $null -ne $_.gain } | ForEach-Object gain);
        GainUpdates=($samples.gain_updates | Measure-Object -Maximum).Maximum;
        Blocks=@($blocks).Count; PositiveMissing=$cold.Count; ZeroMissing=$reuse.Count;
        PartialReuseBlocks=$partialReuse.Count; PartialReusePeakWsDelta=Stats $partialReuse.PeakWsDelta;
        AllocationMismatch=@($blocks | Where-Object { $_.Acquired -ne $_.Missing }).Count;
        ColdPeakWsDelta=Stats $cold.PeakWsDelta; ReusePeakWsDelta=Stats $reuse.PeakWsDelta;
        ColdWsToMissing=Stats @($cold | ForEach-Object { $_.PeakWsDelta/$_.Missing });
        MaxBlocks=@($blocks | Sort-Object PeakWsDelta -Descending | Select-Object -First 4);
        StageRows=@($rows | Where-Object { $_.point -match '^(host\.|attributes.begin|geometry.commit|attributes.commit)' });
        ProbeRows=$probeRows;
        ProbeBlockRows=@(if ($probeRows.Count -gt 0) { $rows | Where-Object { $_.flow -eq $end.flow -and $_.block -eq $end.block -and $_.point -ne 'sample' } })
    }
}
$results | ConvertTo-Json -Depth 6

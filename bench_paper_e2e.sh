#!/bin/bash
# ============================================================================
# ANVIL Paper E2E Data Collection — Single 30-minute run
#
# Collects ALL e2e data needed for paper Table e2e_latency + Sustained Perf:
#   1. ANVIL per-frame timing (from logcat; set log_interval=1 in vf_anvil.c for full-frame)
#   2. System telemetry: battery, thermal, CPU/GPU freq (every 10s via dumpsys)
#   3. Meta: device info, start/end battery, duration
#
# Prerequisites:
#   - Device connected via wireless ADB
#   - ANVIL Demo app playing video with VFI enabled (loop=on)
#   - Screen on, brightness fixed, USB disconnected
#
# Usage:
#   ./bench_paper_e2e.sh [DEVICE] [DURATION_MIN]
#   ./bench_paper_e2e.sh 10.246.105.24:5555 30
#   ./bench_paper_e2e.sh 10.246.105.24:5555 1    # smoke test
# ============================================================================
set -euo pipefail

DEVICE="${1:-10.246.105.24:5555}"
DURATION_MIN="${2:-30}"
SAMPLE_INTERVAL=10
ADB="adb -s $DEVICE"
OUT_DIR="$(pwd)/bench_paper_e2e"

mkdir -p "$OUT_DIR"

# ---- Verify device ----
$ADB shell echo "ok" > /dev/null 2>&1 || { echo "ERROR: device not reachable at $DEVICE"; exit 1; }

# ---- Verify ANVIL is playing (check recent logcat) ----
ANVIL_CHECK=$($ADB logcat -d -t 500 2>&1 | grep 'anvil:info.*total=' | tail -1)
if [ -z "$ANVIL_CHECK" ]; then
    echo "WARNING: No ANVIL timing in recent logcat. Is the video playing?"
    echo "  Last check: $(date)"
    read -p "Press Enter to continue anyway, or Ctrl-C to abort..."
fi

# ---- Device info ----
SOC=$($ADB shell getprop ro.soc.model 2>/dev/null || echo "unknown")
MODEL=$($ADB shell getprop ro.product.model 2>/dev/null || echo "unknown")
ANDROID=$($ADB shell getprop ro.build.version.release 2>/dev/null || echo "unknown")
BRIGHT=$($ADB shell settings get system screen_brightness 2>/dev/null || echo "unknown")
BRIGHT_MODE=$($ADB shell settings get system screen_brightness_mode 2>/dev/null || echo "unknown")

# ---- Battery baseline ----
get_battery() {
    $ADB shell dumpsys battery 2>/dev/null | awk '
        /^  level:/ {level=$2}
        /^  voltage:/ {voltage=$2}
        /^  temperature:/ {temp=$2}
        /^  Charge counter:/ {charge=$3}
        /^  status:/ {status=$2}
        END {printf "%s,%s,%s,%s,%s", level, voltage, temp, charge, status}
    '
}

get_thermals() {
    $ADB shell dumpsys thermalservice 2>/dev/null | grep 'Temperature{' | awk -F'[,=}]' '
        /mName=CPU0[^-9]/ && !c0{c0=$2}
        /mName=CPU7[^-9]/ && !c7{c7=$2}
        /mName=GPU0[^-9]/ && !g0{g0=$2}
        /mName=nsp0[^-9]/ && !n0{n0=$2}
        /mName=nsp1[^-9]/ && !n1{n1=$2}
        /mName=skin[^_]/ && !sk{sk=$2}
        /mName=shell_skin/ && !sh{sh=$2}
        /mName=battery[^-]/ && !bt{bt=$2}
        END {printf "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f", c0,c7,g0,n0,n1,sk,sh,bt}
    '
}

get_cpu_freq() {
    local c0=$($ADB shell cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo 0)
    local c4=$($ADB shell cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq 2>/dev/null || echo 0)
    local c7=$($ADB shell cat /sys/devices/system/cpu/cpu7/cpufreq/scaling_cur_freq 2>/dev/null || echo 0)
    echo "${c0},${c4},${c7}"
}

BASELINE_BAT=$(get_battery)
BASELINE_LEVEL=$(echo "$BASELINE_BAT" | cut -d, -f1)
BASELINE_CHARGE=$(echo "$BASELINE_BAT" | cut -d, -f4)
START_TIME=$(date '+%Y-%m-%d %H:%M:%S')

# ---- Write meta ----
cat > "$OUT_DIR/meta.json" << EOF
{
    "device": "$MODEL",
    "soc": "$SOC",
    "android": "$ANDROID",
    "brightness": $BRIGHT,
    "brightness_mode": $BRIGHT_MODE,
    "duration_min": $DURATION_MIN,
    "sample_interval_s": $SAMPLE_INTERVAL,
    "start_time": "$START_TIME",
    "start_battery_level": $BASELINE_LEVEL,
    "start_battery_charge_uah": $BASELINE_CHARGE,
    "video": "old_town_cross_30fps.mp4 (1080p H.264 30fps bframes=0 Baseline CRF23)",
    "video_note": "HARDCODED — update this field if the test video changes"
}
EOF

echo "============================================"
echo "  ANVIL Paper E2E Data Collection"
echo "============================================"
echo "Device: $MODEL ($SOC)"
echo "Duration: ${DURATION_MIN}min, sample every ${SAMPLE_INTERVAL}s"
echo "Battery: ${BASELINE_LEVEL}%, ${BASELINE_CHARGE} µAh"
echo "Brightness: ${BRIGHT} (mode=${BRIGHT_MODE})"
echo ""

# ---- Clear logcat, start capture ----
$ADB logcat -c 2>/dev/null || true
$ADB logcat -v time > "$OUT_DIR/logcat_raw.txt" 2>/dev/null &
LOGCAT_PID=$!

# ---- Telemetry CSV ----
TELEM="$OUT_DIR/telemetry.csv"
echo "timestamp,elapsed_s,bat_level,bat_voltage,bat_temp,charge_uAh,bat_status,cpu0_khz,cpu4_khz,cpu7_khz,t_cpu0,t_cpu7,t_gpu0,t_nsp0,t_nsp1,t_skin,t_shell,t_battery" > "$TELEM"

# ---- Sampling loop ----
TOTAL_SAMPLES=$((DURATION_MIN * 60 / SAMPLE_INTERVAL))
echo "Collecting ${TOTAL_SAMPLES} telemetry samples + logcat..."

for i in $(seq 1 $TOTAL_SAMPLES); do
    TS=$(date '+%H:%M:%S')
    ELAPSED=$((i * SAMPLE_INTERVAL))

    BAT=$(get_battery)
    CPUFREQ=$(get_cpu_freq)
    THERM=$(get_thermals)

    echo "$TS,$ELAPSED,$BAT,$CPUFREQ,$THERM" >> "$TELEM"

    if (( ELAPSED % 120 == 0 )); then
        LVL=$(echo "$BAT" | cut -d, -f1)
        echo "  [$(( ELAPSED / 60 ))/${DURATION_MIN}min] level=${LVL}%"
    fi

    sleep $SAMPLE_INTERVAL
done

# ---- End ----
END_BAT=$(get_battery)
END_TIME=$(date '+%Y-%m-%d %H:%M:%S')
END_LEVEL=$(echo "$END_BAT" | cut -d, -f1)
END_CHARGE=$(echo "$END_BAT" | cut -d, -f4)

# ---- Stop logcat ----
kill $LOGCAT_PID 2>/dev/null || true
wait $LOGCAT_PID 2>/dev/null || true

# ---- Extract ANVIL timing ----
grep 'anvil:info' "$OUT_DIR/logcat_raw.txt" > "$OUT_DIR/anvil_log.txt" 2>/dev/null || true
grep 'total=' "$OUT_DIR/anvil_log.txt" > "$OUT_DIR/anvil_timing.txt" 2>/dev/null || true

# ---- Parse timing into CSV ----
python3 -c "
import re, json, statistics

timing_lines = open('$OUT_DIR/anvil_timing.txt').readlines()

records = []
for line in timing_lines:
    m_ts = re.match(r'(\d+-\d+)\s+(\d+:\d+:\d+\.\d+)', line)
    m_total = re.search(r'total=([\d.]+)ms', line)
    m_p1a = re.search(r'P1a=([\d.]+)', line)
    m_gpu = re.search(r'GPU=([\d.]+)', line)
    m_copy = re.search(r'copy=([\d.]+)', line)
    m_p3 = re.search(r'P3=([\d.]+)', line)
    m_p4 = re.search(r'P4.*?=([\d.]+)', line)
    if all([m_ts, m_total]):
        records.append({
            'timestamp': m_ts.group(2),
            'total': float(m_total.group(1)),
            'p1a': float(m_p1a.group(1)) if m_p1a else 0,
            'gpu': float(m_gpu.group(1)) if m_gpu else 0,
            'copy': float(m_copy.group(1)) if m_copy else 0,
            'p3': float(m_p3.group(1)) if m_p3 else 0,
            'p4': float(m_p4.group(1)) if m_p4 else 0,
        })

# Write timing CSV
with open('$OUT_DIR/anvil_timing.csv', 'w') as f:
    f.write('timestamp,total_ms,p1a_ms,gpu_ms,copy_ms,p3_ms,p4_ms\n')
    for r in records:
        f.write(f\"{r['timestamp']},{r['total']},{r['p1a']},{r['gpu']},{r['copy']},{r['p3']},{r['p4']}\n\")

# Compute summary
if records:
    totals = [r['total'] for r in records]
    p3s = [r['p3'] for r in records]
    p1as = [r['p1a'] for r in records]
    gpus = [r['gpu'] for r in records]
    copies = [r['copy'] for r in records]
    p4s = [r['p4'] for r in records]

    summary = {
        'n_samples': len(records),
        'total': {
            'median': round(statistics.median(totals), 1),
            'mean': round(statistics.mean(totals), 1),
            'p5': round(sorted(totals)[int(0.05*len(totals))], 1),
            'p95': round(sorted(totals)[int(0.95*len(totals))], 1),
            'min': round(min(totals), 1),
            'max': round(max(totals), 1),
        },
        'p1a':  {'median': round(statistics.median(p1as), 1)},
        'gpu':  {'median': round(statistics.median(gpus), 1)},
        'copy': {'median': round(statistics.median(copies), 1)},
        'p3':   {'median': round(statistics.median(p3s), 1),
                 'p95': round(sorted(p3s)[int(0.95*len(p3s))], 1)},
        'p4':   {'median': round(statistics.median(p4s), 1)},
    }
    with open('$OUT_DIR/timing_summary.json', 'w') as f:
        json.dump(summary, f, indent=2)

    print(f'  ANVIL timing: {len(records)} samples')
    print(f'  Total:  median={summary[\"total\"][\"median\"]}ms  p95={summary[\"total\"][\"p95\"]}ms  range=[{summary[\"total\"][\"min\"]}, {summary[\"total\"][\"max\"]}]')
    print(f'  P1a={summary[\"p1a\"][\"median\"]}  GPU={summary[\"gpu\"][\"median\"]}  copy={summary[\"copy\"][\"median\"]}  P3={summary[\"p3\"][\"median\"]}  P4={summary[\"p4\"][\"median\"]}')
else:
    print('  WARNING: No ANVIL timing data found!')
    with open('$OUT_DIR/timing_summary.json', 'w') as f:
        json.dump({'error': 'no timing data'}, f)
" 2>&1

# ---- Update meta with end stats ----
python3 -c "
import json
meta = json.load(open('$OUT_DIR/meta.json'))
meta['end_time'] = '$END_TIME'
meta['end_battery_level'] = $END_LEVEL
meta['end_battery_charge_uah'] = $END_CHARGE
meta['battery_drop_pct'] = $BASELINE_LEVEL - $END_LEVEL
json.dump(meta, open('$OUT_DIR/meta.json', 'w'), indent=2)
"

echo ""
echo "============================================"
echo "  Done"
echo "============================================"
echo "$START_TIME → $END_TIME"
echo "Battery: ${BASELINE_LEVEL}% → ${END_LEVEL}% (Δ=$(( BASELINE_LEVEL - END_LEVEL ))%)"
echo ""
echo "Output: $OUT_DIR/"
echo "  meta.json           — device info + battery"
echo "  telemetry.csv       — system telemetry (${TOTAL_SAMPLES} samples)"
echo "  anvil_timing.txt    — raw ANVIL log lines"
echo "  anvil_timing.csv    — parsed per-stage timing"
echo "  timing_summary.json — paper-ready statistics"
echo "  logcat_raw.txt      — full logcat"

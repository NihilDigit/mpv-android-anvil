#!/bin/bash
# 30fps e2e stress test — 30 minutes, burst profile, wireless ADB
# Uses dumpsys (works over wireless ADB without root)
set -euo pipefail

DEVICE="${1:-10.246.105.24:5555}"
ADB="adb -s $DEVICE"
DURATION_MIN=30
SAMPLE_INTERVAL=10
OUT_DIR="$(pwd)/bench_stress_30fps"

mkdir -p "$OUT_DIR"

# ---- Verify connection ----
$ADB shell echo "ok" > /dev/null 2>&1 || { echo "ERROR: device $DEVICE not reachable"; exit 1; }
echo "Device connected: $DEVICE"

# ---- Helper: parse dumpsys battery ----
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

# ---- Helper: parse dumpsys thermalservice ----
get_thermals() {
  $ADB shell dumpsys thermalservice 2>/dev/null | grep 'Temperature{' | awk -F'[,=}]' '
    /mName=CPU0/  {cpu0=$2}
    /mName=CPU7/  {cpu7=$2}
    /mName=GPU0/  {gpu0=$2}
    /mName=nsp0/  {nsp0=$2}
    /mName=nsp1/  {nsp1=$2}
    /mName=skin[^_]/ {skin=$2}
    /mName=shell_skin/ {shell=$2}
    /mName=battery/ {bat=$2}
    END {printf "%s,%s,%s,%s,%s,%s,%s,%s", cpu0, cpu7, gpu0, nsp0, nsp1, skin, shell, bat}
  '
}

# ---- Helper: CPU frequencies ----
get_cpu_freq() {
  local c0=$($ADB shell cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo 0)
  local c4=$($ADB shell cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq 2>/dev/null || echo 0)
  local c7=$($ADB shell cat /sys/devices/system/cpu/cpu7/cpufreq/scaling_cur_freq 2>/dev/null || echo 0)
  echo "${c0},${c4},${c7}"
}

# ---- Baseline ----
BASELINE_BAT=$(get_battery)
START_TIME=$(date '+%Y-%m-%d %H:%M:%S')
LEVEL=$(echo "$BASELINE_BAT" | cut -d, -f1)
CHARGE=$(echo "$BASELINE_BAT" | cut -d, -f4)

cat > "$OUT_DIR/meta.txt" << EOF
=== 30fps E2E Stress Test ===
Device: OnePlus Ace5 (SM8650, HTP V75)
Video: old_town_cross_30fps.mp4, 1080p H.264 30fps, loop=inf
Brightness: 50% (2048 manual), WiFi on, USB disconnected
Profile: burst (default)
Start: $START_TIME
Baseline: level=${LEVEL}%, charge=${CHARGE} µAh
EOF

echo "=== 30fps E2E Stress Test ==="
echo "Duration: ${DURATION_MIN}min, sample every ${SAMPLE_INTERVAL}s"
echo "Baseline: ${LEVEL}%, ${CHARGE} µAh"
echo ""

# ---- Clear logcat and start capture ----
$ADB logcat -c 2>/dev/null || true
$ADB logcat -v time > "$OUT_DIR/logcat_raw.txt" 2>/dev/null &
LOGCAT_PID=$!

# ---- Telemetry CSV ----
TELEM="$OUT_DIR/telemetry.csv"
echo "timestamp,elapsed_s,bat_level,bat_voltage,bat_temp,charge_uAh,bat_status,cpu0_khz,cpu4_khz,cpu7_khz,t_cpu0,t_cpu7,t_gpu0,t_nsp0,t_nsp1,t_skin,t_shell,t_battery" > "$TELEM"

# ---- Sampling loop ----
TOTAL_SAMPLES=$((DURATION_MIN * 60 / SAMPLE_INTERVAL))
echo "Collecting ${TOTAL_SAMPLES} samples..."

for i in $(seq 1 $TOTAL_SAMPLES); do
  TS=$(date '+%H:%M:%S')
  ELAPSED=$((i * SAMPLE_INTERVAL))

  BAT=$(get_battery)
  CPUFREQ=$(get_cpu_freq)
  THERM=$(get_thermals)

  echo "$TS,$ELAPSED,$BAT,$CPUFREQ,$THERM" >> "$TELEM"

  # Progress every 2 minutes
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

cat >> "$OUT_DIR/meta.txt" << EOF
End: $END_TIME
End: level=${END_LEVEL}%, charge=${END_CHARGE} µAh
Duration: ${DURATION_MIN} minutes
Samples: ${TOTAL_SAMPLES}
EOF

kill $LOGCAT_PID 2>/dev/null || true
wait $LOGCAT_PID 2>/dev/null || true

# ---- Extract ANVIL timing from logcat ----
grep -i 'anvil\|vf_anvil\|ANVIL' "$OUT_DIR/logcat_raw.txt" > "$OUT_DIR/anvil_timing.txt" 2>/dev/null || true

echo ""
echo "=== Done ==="
echo "$START_TIME → $END_TIME"
echo "Battery: ${LEVEL}% → ${END_LEVEL}% (Δ=$(( LEVEL - END_LEVEL ))%)"
echo "ANVIL log lines: $(wc -l < "$OUT_DIR/anvil_timing.txt")"
echo "Telemetry: $(( $(wc -l < "$TELEM") - 1 )) samples"
echo "Output: $OUT_DIR/"

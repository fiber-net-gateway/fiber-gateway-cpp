#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: collect_cgroup.sh <user-unit> <output-prefix>" >&2
    exit 2
fi

unit="$1"
output_prefix="$2"
cgroup="$(systemctl --user show "$unit" --property=ControlGroup --value)"
if [[ -z "$cgroup" ]]; then
    echo "unit has no active cgroup: $unit" >&2
    exit 1
fi

cgroup_path="/sys/fs/cgroup$cgroup"
for metric in cpu.stat memory.current memory.peak memory.events pids.current; do
    metric_path="$cgroup_path/$metric"
    if [[ -r "$metric_path" ]]; then
        cp "$metric_path" "$output_prefix.$metric"
    fi
done

systemctl --user show "$unit" \
    --property=ActiveState \
    --property=SubState \
    --property=ControlGroup \
    --property=AllowedCPUs \
    --property=CPUQuotaPerSecUSec \
    --property=MemoryCurrent \
    --property=MemoryPeak \
    >"$output_prefix.unit"

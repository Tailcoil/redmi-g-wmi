#!/bin/sh
# 接口探测：EC 字段变化和驱动报告的未知事件写入同一日志。需要 root。
# 用法：probe.sh <DSDT.dsl> <日志> [秒数]
DIR=$(dirname "$(readlink -f "$0")")
DSL=$1 LOG=$2 SECS=${3:-900}
modprobe ec_sys
timeout "$SECS" journalctl -kf -n0 -o short-precise | grep --line-buffered 'redmi-g-wmi' |
    while read -r l; do echo "$(date +%T) wmi ${l#*: }" >> "$LOG"; sync "$LOG"; done &
timeout "$SECS" python3 -I "$DIR/ecwatch.py" "$DSL" "$LOG"
wait
U=${PKEXEC_UID:-$SUDO_UID}
[ -n "$U" ] && chown "$U" "$LOG"

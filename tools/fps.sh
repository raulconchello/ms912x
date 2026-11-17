#!/bin/sh
F=/sys/kernel/debug/dri/0/ms912x_stats
cat $F >/dev/null; t0=$(date +%s.%N); sleep ${1:-3}; out=$(cat $F); t1=$(date +%s.%N)
echo "$out"
echo "$out" | awk -v t0=$t0 -v t1=$t1 '/^frames=/{split($1,a,"="); printf "fps=%.1f over %.2fs\n", a[2]/(t1-t0), t1-t0}'

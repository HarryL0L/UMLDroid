#!/bin/bash
# Console commands for run.sh: print facts about the running Android between
# PROBE markers (where UWB/C2 are declared, why media.swcodec dies, what keeps
# the CPUs busy while idle).
#   UML_CONSOLE_FEED=host/probe-feed.sh ./run.sh
sleep "${PROBE_AT:-200}"
cat <<'X'
su 0 sh -c 'echo PROBE-BEGIN; for d in /system/etc/permissions /vendor/etc/permissions /odm/etc/permissions /product/etc/permissions /system_ext/etc/permissions; do grep -l -i uwb $d/*.xml 2>/dev/null; done; echo PROBE-c2; grep -rl "media.c2" /vendor/etc/vintf /system/etc/vintf /odm/etc/vintf /apex/*/etc/vintf 2>/dev/null; ls /apex | head -80 | tr "\n" " "; echo; echo PROBE-swcodec; logcat -d 2>/dev/null | grep -iE "swcodec|mediaswcodec|minijail|seccomp" | tail -40; echo PROBE-top; top -b -n 1 -m 25 2>/dev/null; echo PROBE-END'
X
sleep 60
cat <<'X'
su 0 sh -c 'echo PROBE2-BEGIN; top -b -n 1 -d 10 -m 20 2>/dev/null; for f in $(grep -l -i uwb /vendor/etc/permissions/*.xml /system/etc/permissions/*.xml 2>/dev/null); do echo == $f; cat $f; done; dumpsys -l | grep -i -E "uwb|c2"; echo PROBE2-END'
X

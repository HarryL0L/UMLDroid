#!/bin/bash
# Console commands for run.sh (UML_CONSOLE_FEED=host/diag-feed.sh ./run.sh):
# every DIAG_EVERY seconds (default 120) write Android's crash, watchdog and
# fatal-error log lines into android/console.log, between UMLDIAG markers.
sleep "${DIAG_FIRST:-120}"
while :; do
  printf "echo UMLDIAG-BEGIN; logcat -d -b crash -b system -b events 2>&1 | grep -iE 'FATAL|Caused by|WATCHDOG|am_crash|am_anr|Blocked in|Abort message|>>> |Killing.*system|died' | tail -60; echo UMLDIAG-END\n"
  sleep "${DIAG_EVERY:-120}"
done

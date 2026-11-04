#!/bin/bash
# Console commands for run.sh: AOT-compile all apps once (the result stays in
# /data on disk.img), writing COMPILE markers into android/console.log.
#   COMPILE_MODE=speed-profile UML_CONSOLE_FEED=host/compile-feed.sh ./run.sh
# (COMPILE_MODE: speed or speed-profile; "reset" undoes it.)
# COMPILE_PKGS="pkg ..." compiles only those packages (all of them, -a, needs
# a lot of memory: on a phone with little free RAM Android killed UML).
sleep "${COMPILE_AT:-180}"
M=${COMPILE_MODE:-speed-profile}
if [ "$M" = reset ]; then
  printf '%s\n' "su 0 sh -c 'echo COMPILE begin reset; time cmd package compile --reset -a 2>&1 | tail -3; echo COMPILE end'"
else
  if [ -n "${COMPILE_PKGS:-}" ]; then
    printf '%s\n' "su 0 sh -c 'echo COMPILE begin $M; for p in $COMPILE_PKGS; do cmd package compile -m $M -f \$p 2>&1 | tail -1; done; echo COMPILE end'"
  else
    printf '%s\n' "su 0 sh -c 'echo COMPILE begin $M; time cmd package compile -m $M -f -a 2>&1 | tail -3; echo COMPILE end'"
  fi
fi

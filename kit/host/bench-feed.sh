#!/bin/bash
# Console commands for run.sh: run the UML benchmark inside Android once the
# boot has settled, writing its results into android/console.log.
#   UML_CONSOLE_FEED=host/bench-feed.sh ./run.sh
# then: grep BENCH android/console.log
sleep "${BENCH_AT:-180}"
printf "su 0 /dev/uml/umlbench\n"

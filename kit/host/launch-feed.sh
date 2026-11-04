#!/bin/bash
# Console commands for run.sh: time app launches inside Android once the boot
# has settled (cold = app process started fresh, warm = app already running).
#   UML_CONSOLE_FEED=host/launch-feed.sh ./run.sh
# then: grep -a "^LAUNCH" android/console.log
sleep "${LAUNCH_AT:-180}"
printf '%s\n' 'for i in 1 2 3; do for c in com.android.settings/.Settings com.android.deskclock/.DeskClock; do echo "LAUNCH cold $c $(am start -W -S -n $c | grep TotalTime)"; sleep 4; input keyevent 3; sleep 4; echo "LAUNCH warm $c $(am start -W -n $c | grep TotalTime)"; sleep 4; input keyevent 3; sleep 4; done; done; echo "LAUNCH" "end"'

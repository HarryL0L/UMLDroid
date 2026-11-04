#!/system/bin/sh
# "Daily use" trim, run by init.rc once Android has booted:
#   lite.sh on   turn off system apps that have nothing to do on UML (no
#                radio, Bluetooth, NFC, printers, ...) but start at boot or
#                on every boot-completed broadcast, then compile the apps
#                that always run ahead of time (one at a time, low priority).
#   lite.sh off  turn those apps back on.
# Both run once: a marker in /data remembers the state.
M=/data/local/tmp/.uml-lite
APPS="com.android.bluetooth com.android.bluetoothmidiservice com.android.nfc com.android.se
com.android.devicelockcontroller com.android.printspooler com.android.bips
com.android.printservice.recommendation com.android.statementservice
com.android.cellbroadcastreceiver com.android.cellbroadcastreceiver.module com.android.ons
com.android.stk com.android.imsserviceentitlement com.android.telephony.qns
com.google.android.telephony.satellite com.android.gbaservice com.android.hotspot2.osulogin
com.android.federatedcompute.services com.android.ondevicepersonalization.services
com.android.traceur com.android.dreams.basic com.android.dreams.phototable com.android.egg
com.android.quicksearchbox com.android.threadnetwork.demoapp com.android.DeviceAsWebcam
com.android.devicediagnostics com.android.google.gce.gceservice org.chromium.webview_shell
com.android.wallpaper.livepicker com.android.emergency"
COMPILE="com.android.systemui com.android.launcher3 com.android.settings com.android.inputmethod.latin"
log() { echo "uml-lite: $*" > /dev/kmsg; }

case "$1" in
on)
  [ "$(cat $M 2>/dev/null)" = on ] && exit 0
  settings put global bluetooth_on 0
  settings put global ble_scan_always_enabled 0
  settings put global wifi_scan_always_enabled 0
  n=0
  for p in $APPS; do
    pm disable-user --user 0 "$p" > /dev/null 2>&1 && n=$((n + 1))
  done
  log "turned off $n system apps"
  echo on > $M
  for p in $COMPILE; do
    nice -n 15 cmd package compile -m speed -f "$p" > /dev/null 2>&1
    log "compiled $p ahead of time"
  done
  ;;
off)
  [ "$(cat $M 2>/dev/null)" = on ] || exit 0
  for p in $APPS; do
    pm enable --user 0 "$p" > /dev/null 2>&1
  done
  log "turned the system apps back on"
  echo off > $M
  ;;
esac

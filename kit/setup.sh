#!/usr/bin/env bash
# Prepare Android 17 (Cuttlefish userspace) for User-Mode Linux.
#
#   ./setup.sh [cuttlefish-img.zip]
#
# Without an argument the Android 17 Cuttlefish image matching this machine
# (arm64 or x86_64) is downloaded (about 1.1 GB). Everything ends up in
# ./android: a GPT disk with Android's partitions (sparse, about 2 GB used)
# and the initramfs. Needs: python3, unzip, mke2fs (Termux: pkg install python unzip
# e2fsprogs curl). About 5 GB of free space while setting up, 3 GB after.
# Rerunning it later only rebuilds the initramfs (updating the UML helpers)
# and keeps Android's data.
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)
A=$D/android
PY=$(command -v python3 || command -v python)
BUILD=16644040
case "$(uname -m)" in
  aarch64|arm64) ARCH=arm64; TARGET=aosp_cf_arm64_only_phone ;;
  x86_64)        ARCH=x86_64; TARGET=aosp_cf_x86_64_only_phone ;;
  *) echo "setup: unsupported machine $(uname -m)" >&2; exit 1 ;;
esac
ZIP=${1:-$A/$TARGET-img-$BUILD.zip}
URL=https://ci.android.com/builds/submitted/$BUILD/$TARGET-userdebug/latest/raw/$TARGET-img-$BUILD.zip
mkdir -p "$A/img" "$A/unpack"

if [ ! -f "$ZIP" ] && [ ! -f "$A/disk.img" ]; then
  echo "== downloading $TARGET (Android 17, build $BUILD)"
  curl -fL --retry 3 -o "$ZIP.part" "$URL" && mv "$ZIP.part" "$ZIP"
fi

echo "== unpacking images"
if [ -f "$A/disk.img" ]; then
  # Only the boot images are needed to rebuild the initramfs.
  [ -f "$A/img/vendor_boot.img" ] && [ -f "$A/img/init_boot.img" ] ||
    unzip -o -q "$ZIP" -d "$A/img" init_boot.img vendor_boot.img
else
  unzip -o -q "$ZIP" -d "$A/img" boot.img init_boot.img vendor_boot.img vbmeta.img \
    vbmeta_system.img vbmeta_system_dlkm.img vbmeta_vendor_dlkm.img \
    cuttlefish_example_custom.img super.img userdata.img
fi

echo "== building the initramfs"
"$PY" "$D/host/bootimg.py" "$A/img/vendor_boot.img" "$A/unpack/vendor_boot" >/dev/null
"$PY" "$D/host/bootimg.py" "$A/img/init_boot.img" "$A/unpack/init_boot" >/dev/null
# Our first_stage.sh and the UML boot script and helpers (/uml, copied to
# /dev/uml at boot). Rebuilt on every setup.sh run, so rerunning it updates
# them without touching Android's data.
"$PY" "$D/host/mkoverlay.py" "$A/unpack/overlay.cpio" \
  first_stage.sh="$D/guest/first_stage.sh:755" uml/init.rc="$D/guest/init.rc" \
  uml/fixup="$D/guest/bin-$ARCH/fixup:755" uml/bpfstub="$D/guest/bin-$ARCH/bpfstub:755" \
  uml/umlbench="$D/guest/bin-$ARCH/umlbench:755" uml/lite.sh="$D/guest/lite.sh:755"   $( [ -f "$D/guest/bin-$ARCH/shmtest" ] && echo uml/shmtest="$D/guest/bin-$ARCH/shmtest:755" )
# The kernel unpacks concatenated archives in order: ours, then the vendor
# ramdisk, then the generic ramdisk (as an Android bootloader would).
cat "$A/unpack/overlay.cpio" "$A/unpack/vendor_boot/vendor_ramdisk_all" \
    "$A/unpack/init_boot/ramdisk" > "$A/initrd.img"

if [ ! -f "$A/disk.img" ]; then
  echo "== building the disk (one GPT disk with the partitions Android expects)"
  # An empty ext4 metadata partition (Android keeps its encryption keys there).
  rm -f "$A/unpack/metadata.img"; truncate -s 64M "$A/unpack/metadata.img"
  mke2fs -q -F -t ext4 -E root_owner=0:0 "$A/unpack/metadata.img"
  I=$A/img
  "$PY" "$D/host/mkdisk.py" "$A/disk.img.part" misc:1 custom:1:$I/cuttlefish_example_custom.img \
    metadata:64:$A/unpack/metadata.img boot_a:64:$I/boot.img init_boot_a:8:$I/init_boot.img \
    vendor_boot_a:64:$I/vendor_boot.img vbmeta_a:1:$I/vbmeta.img vbmeta_system_a:1:$I/vbmeta_system.img \
    vbmeta_vendor_dlkm_a:1:$I/vbmeta_vendor_dlkm.img vbmeta_system_dlkm_a:1:$I/vbmeta_system_dlkm.img \
    super:8192:$I/super.img userdata:8192:$I/userdata.img frp:1
  mv "$A/disk.img.part" "$A/disk.img"
  rm -f "$I/super.img" "$I/userdata.img"
else
  echo "== keeping the existing disk (Android's data lives there; delete android/disk.img to start over)"
  [ -f "$ZIP" ] && echo "   ($(basename "$ZIP") is no longer needed and can be deleted)"
fi
echo "== done: ./run.sh starts Android (the first boot takes several minutes)"

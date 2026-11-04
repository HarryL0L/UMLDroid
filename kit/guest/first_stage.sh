#!/system/bin/sh
# Runs from Android's first-stage init (androidboot.first_stage_console=1)
# before the first-stage mount.
#
# 1. UML's ubd disk is a "virtual" block device, which init does not consider
#    a boot device, so it never creates the /dev/block/by-name links the
#    fstab relies on. Create them; the order matches the GPT that setup.sh
#    builds.
# 2. Copy the UML boot script and helpers from the initramfs to /dev/uml:
#    /dev is carried into the second stage, and androidboot.init_rc points
#    there.
mkdir -p /dev/block/by-name
i=1
for n in misc custom metadata boot_a init_boot_a vendor_boot_a vbmeta_a \
         vbmeta_system_a vbmeta_vendor_dlkm_a vbmeta_system_dlkm_a super userdata frp; do
  ln -sf /dev/block/ubda$i /dev/block/by-name/$n
  i=$((i+1))
done
mkdir -p /dev/uml
for f in /uml/*; do
  cp $f /dev/uml/
done
chmod 0755 /dev/uml/fixup /dev/uml/bpfstub /dev/uml/umlbench /dev/uml/lite.sh
echo "first_stage.sh: created $((i-1)) by-name links, UML helpers in /dev/uml" > /dev/kmsg

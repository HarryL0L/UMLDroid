/*
 * Guest-side fixups for running the Cuttlefish Android userspace on UML
 * without Cuttlefish's host services. Static binary, copied from the
 * initramfs to /dev/uml by first_stage.sh and run from /dev/uml/init.rc in
 * early-init: this is before
 * apexd-bootstrap, so the bionic APEX is not active yet and no dynamically
 * linked program (not even sh) can run. vendor is verity-protected, so files
 * are overridden with bind mounts.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#define U "/dev/uml"

/* HALs that only work with Cuttlefish's host services (virtio consoles /
 * vsock to the host): hide their APEXes from apexd so they are neither
 * started nor declared. Some would block system_server (oemlock, uwb,
 * threadnetwork); the others crash and restart every few seconds, which
 * costs a lot of CPU on UML (Bluetooth, radio, NFC). */
static const char *hidden_apexes[] = {
	"com.google.cf.oemlock.apex",
	"com.android.hardware.uwb.apex",
	"com.android.hardware.threadnetwork.apex",
	"com.google.cf.bt.apex",
	"com.google.cf.rild.apex",
	"com.google.cf.nfc.apex",
	NULL
};

static void kmsg(const char *msg)
{
	int fd = open("/dev/kmsg", O_WRONLY);
	if (fd >= 0) {
		if (write(fd, msg, strlen(msg)) < 0)
			;
		close(fd);
	}
}

static int hidden(const char *name)
{
	for (int i = 0; hidden_apexes[i]; i++)
		if (!strcmp(name, hidden_apexes[i]))
			return 1;
	return 0;
}

/*
 * Build a view of /vendor/apex without the hidden APEXes: a tmpfs under
 * /dev with one bind mount per kept APEX, then bind it over /vendor/apex.
 */
static int filter_vendor_apexes(void)
{
	const char *view = "/dev/uml_vendor_apex";
	DIR *d = opendir("/vendor/apex");
	struct dirent *e;
	char src[300], dst[300];
	int n = 0, fd;

	if (!d)
		return -1;
	mkdir(view, 0755);
	if (mount("tmpfs", view, "tmpfs", 0, "mode=0755") < 0) {
		closedir(d);
		return -1;
	}
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' || hidden(e->d_name))
			continue;
		snprintf(src, sizeof(src), "/vendor/apex/%s", e->d_name);
		snprintf(dst, sizeof(dst), "%s/%s", view, e->d_name);
		fd = open(dst, O_WRONLY | O_CREAT, 0644);
		if (fd < 0)
			continue;
		close(fd);
		if (mount(src, dst, NULL, MS_BIND, NULL) == 0)
			n++;
	}
	closedir(d);
	if (mount(view, "/vendor/apex", NULL, MS_BIND | MS_REC, NULL) < 0)
		return -1;
	return n;
}

/*
 * ubd partitions get ueventd's default root-only mode. Cuttlefish's rules
 * match its virtio-blk by-name paths, so add the ubd node of partitions
 * that system services open directly (frp: PersistentDataBlockService).
 */
static void extend_ueventd_rules(void)
{
	static const char extra[] =
		"\n# UML: ubd partitions opened directly by system services\n"
		"/dev/block/ubda13         0660   system     system\n";
	char buf[65536];
	int in, out;
	ssize_t r;

	in = open("/vendor/etc/ueventd.rc", O_RDONLY);
	if (in < 0)
		return;
	out = open(U "/etc/ueventd.rc", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) {
		close(in);
		return;
	}
	while ((r = read(in, buf, sizeof(buf))) > 0)
		if (write(out, buf, r) != r)
			break;
	if (write(out, extra, sizeof(extra) - 1) < 0)
		;
	close(in);
	close(out);
	if (mount(U "/etc/ueventd.rc", "/vendor/etc/ueventd.rc", NULL, MS_BIND, NULL) < 0)
		kmsg("fixup: bind mount of ueventd.rc failed\n");
}

/*
 * Replace a read-only (verity-protected) file with @content: write it to
 * /dev/uml/etc/@name and bind mount it over @path.
 */
static void override_mode(const char *path, const char *name, const char *content, int mode,
			  const char *label)
{
	char tmp[300], msg[400];
	int fd;

	snprintf(tmp, sizeof(tmp), U "/etc/%s", name);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0)
		return;
	if (write(fd, content, strlen(content)) < 0)
		;
	fchmod(fd, mode);
	close(fd);
	/* init only starts services whose binary has the right label */
	if (label)
		setxattr(tmp, "security.selinux", label, strlen(label) + 1, 0);
	if (mount(tmp, path, NULL, MS_BIND, NULL) < 0) {
		snprintf(msg, sizeof(msg), "fixup: bind mount over %s failed\n", path);
		kmsg(msg);
	}
}

static void override_file(const char *path, const char *name, const char *content)
{
	override_mode(path, name, content, 0644, NULL);
}

/*
 * odsign checks the signatures of ART code compiled on the device and waits
 * for its key from keystore first (about 9 s on UML, longer on a phone). The
 * system image's own precompiled code is used without it, so report "done"
 * at once and stop, as odsign does when it finishes.
 */
static const char odsign_stub[] =
	"#!/system/bin/sh\n"
	"setprop odsign.key.done 1\n"
	"setprop odsign.verification.done 1\n"
	"setprop ctl.stop odsign\n"
	"sleep 10\n";

/* Whether the kernel command line has androidboot.uml_lite=1 */
static int lite_mode(void)
{
	char cmd[8192];
	int fd = open("/proc/cmdline", O_RDONLY), n;

	if (fd < 0)
		return 0;
	n = read(fd, cmd, sizeof(cmd) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	cmd[n] = 0;
	return strstr(cmd, "androidboot.uml_lite=1") != NULL;
}

/*
 * With the "daily use" trim (lite.sh) the Bluetooth, NFC and secure element
 * apps are turned off. system_server refuses to boot when a feature is
 * declared but its app is missing, so drop those features from the file
 * declaring them (Cuttlefish: handheld_core_hardware.xml).
 */
static void hide_features(const char *path, const char *name)
{
	static const char *const hide[] = {
		"android.hardware.bluetooth", "android.hardware.nfc", "android.hardware.se.omapi",
	};
	static char in[65536], out[65536];
	char msg[400];
	int fd = open(path, O_RDONLY), n, len = 0, dropped = 0;

	if (fd < 0)
		return;
	n = read(fd, in, sizeof(in) - 1);
	close(fd);
	if (n <= 0)
		return;
	in[n] = 0;
	for (char *l = in; *l; ) {
		char *e = strchr(l, '\n');
		int ll = e ? e - l + 1 : (int)strlen(l), skip = 0;

		for (unsigned i = 0; i < sizeof(hide) / sizeof(hide[0]); i++) {
			char save = l[ll];

			l[ll] = 0;
			if (strstr(l, hide[i]) && strstr(l, "<feature"))
				skip = 1;
			l[ll] = save;
		}
		if (skip)
			dropped++;
		else {
			memcpy(out + len, l, ll);
			len += ll;
		}
		l += ll;
	}
	out[len] = 0;
	if (!dropped)
		return;
	override_file(path, name, out);
	snprintf(msg, sizeof(msg), "fixup: dropped %d features from %s\n", dropped, path);
	kmsg(msg);
}

int main(void)
{
	char buf[256];
	int n, fd;

	mkdir(U "/etc", 0755);
	/* Overrides are read by system services too, not only by root */
	chmod(U, 0755);
	chmod(U "/etc", 0755);

	extend_ueventd_rules();
	n = filter_vendor_apexes();
	if (n < 0)
		kmsg("fixup: could not filter /vendor/apex\n");

	/* Sensors: the Cuttlefish sub-HAL talks to the host over /dev/hvc13 and
	 * aborts without it; an empty sub-HAL list gives a sensor-less HAL. */
	fd = open(U "/etc/hals.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0)
		close(fd);
	if (mount(U "/etc/hals.conf", "/vendor/etc/sensors/hals.conf", NULL, MS_BIND, NULL) < 0)
		kmsg("fixup: bind mount of hals.conf failed\n");

	/*
	 * Features and services that Cuttlefish declares but that have nothing
	 * behind them here: their clients keep asking servicemanager (and so
	 * init) for them every second or so.
	 * - UWB: the HAL APEX is hidden above; without the feature
	 *   system_server stops polling android.hardware.uwb.IUwb/default.
	 * - media.c2 software store: media.swcodec crashes on UML (and init.rc
	 *   stops it); undeclared, codec clients stop waiting for it.
	 */
	override_file("/vendor/etc/permissions/android.hardware.uwb.xml",
		      "uwb-permissions.xml", "<permissions/>\n");
	override_file("/system/etc/vintf/manifest/manifest_media_c2_software.xml",
		      "manifest_media_c2_software.xml",
		      "<manifest version=\"1.0\" type=\"framework\"/>\n");

	override_mode("/system/bin/odsign", "odsign", odsign_stub, 0755, "u:object_r:odsign_exec:s0");
	if (lite_mode())
		hide_features("/vendor/etc/permissions/handheld_core_hardware.xml",
			      "handheld_core_hardware.xml");

	snprintf(buf, sizeof(buf), "fixup: done (%d vendor apexes kept)\n", n);
	kmsg(buf);
	return 0;
}

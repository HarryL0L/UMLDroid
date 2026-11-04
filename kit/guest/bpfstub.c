/*
 * Create empty stand-ins for BPF maps whose programs cannot load on UML
 * (tracepoint programs need perf events, which UML lacks) but that system
 * services open unconditionally and abort without. Run once after the BPF
 * loaders (property bpf.progs_loaded=1). Static binary on /metadata/uml.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

struct stub_map {
	const char *path;
	const char *name;
	uint32_t type, key, value, entries;
	uid_t uid;
	gid_t gid;
	mode_t mode;
};

#define AID_GRAPHICS 1003

static const struct stub_map maps[] = {
	/* frameworks/native/services/gpuservice/bpfprogs/gpuMem.c; read by
	 * system_server through libmeminfo (ReadProcessGpuUsageKb). */
	{ "/sys/fs/bpf/map_gpuMem_gpu_mem_total_map", "gpu_mem_total_m",
	  BPF_MAP_TYPE_HASH, 8, 8, 1024, 0, AID_GRAPHICS, 0440 },
};

static int sys_bpf(int cmd, union bpf_attr *attr)
{
	return syscall(__NR_bpf, cmd, attr, sizeof(*attr));
}

static void kmsg(const char *msg)
{
	int fd = open("/dev/kmsg", O_WRONLY);

	if (fd >= 0) {
		if (write(fd, msg, strlen(msg)) < 0)
			;
		close(fd);
	}
}

int main(void)
{
	char buf[256];

	for (size_t i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) {
		const struct stub_map *m = &maps[i];
		union bpf_attr attr;
		int fd;

		if (access(m->path, F_OK) == 0)
			continue;
		memset(&attr, 0, sizeof(attr));
		attr.map_type = m->type;
		attr.key_size = m->key;
		attr.value_size = m->value;
		attr.max_entries = m->entries;
		memcpy(attr.map_name, m->name, strnlen(m->name, sizeof(attr.map_name) - 1));
		fd = sys_bpf(BPF_MAP_CREATE, &attr);
		if (fd < 0) {
			snprintf(buf, sizeof(buf), "bpfstub: create %s: errno %d\n", m->path, errno);
			kmsg(buf);
			continue;
		}
		memset(&attr, 0, sizeof(attr));
		attr.pathname = (uint64_t)(uintptr_t)m->path;
		attr.bpf_fd = fd;
		if (sys_bpf(BPF_OBJ_PIN, &attr) < 0) {
			snprintf(buf, sizeof(buf), "bpfstub: pin %s: errno %d\n", m->path, errno);
			kmsg(buf);
		} else {
			if (chown(m->path, m->uid, m->gid) < 0 || chmod(m->path, m->mode) < 0)
				;
			snprintf(buf, sizeof(buf), "bpfstub: created %s\n", m->path);
			kmsg(buf);
		}
		close(fd);
	}
	return 0;
}

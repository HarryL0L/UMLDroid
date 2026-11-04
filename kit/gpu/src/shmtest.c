// SPDX-License-Identifier: GPL-2.0
/*
 * shmtest: check virtio-gpu host-visible memory on UML (umlgpu UMLGPU_BLOBS=1).
 * Creates a HOST3D blob, maps it, writes a pattern from userspace, then lets
 * the kernel copy into and out of it (read() into the mapping, write() from
 * it), as apps do with GPU memory. Prints SHMTEST lines.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm/virtgpu_drm.h>

#define SIZE (1 << 20)

int main(void)
{
	int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("SHMTEST FAIL open: %s\n", strerror(errno));
		return 1;
	}

	struct drm_virtgpu_context_set_param p[] = {
		{ VIRTGPU_CONTEXT_PARAM_CAPSET_ID, 3 },
	};
	struct drm_virtgpu_context_init ci = { .num_params = 1, .ctx_set_params = (uintptr_t)p };
	if (ioctl(fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &ci)) {
		printf("SHMTEST FAIL context init: %s\n", strerror(errno));
		return 1;
	}

	struct drm_virtgpu_resource_create_blob cb = {
		.blob_mem = VIRTGPU_BLOB_MEM_HOST3D,
		.blob_flags = VIRTGPU_BLOB_FLAG_USE_MAPPABLE,
		.size = SIZE,
		.blob_id = 1,
	};
	if (ioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &cb)) {
		printf("SHMTEST FAIL create blob: %s\n", strerror(errno));
		return 1;
	}
	struct drm_virtgpu_map mp = { .handle = cb.bo_handle };
	if (ioctl(fd, DRM_IOCTL_VIRTGPU_MAP, &mp)) {
		printf("SHMTEST FAIL map ioctl: %s\n", strerror(errno));
		return 1;
	}
	uint8_t *m = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mp.offset);
	if (m == MAP_FAILED) {
		printf("SHMTEST FAIL mmap: %s\n", strerror(errno));
		return 1;
	}

	/* 1. userspace writes, reads back */
	for (int i = 0; i < SIZE; i++)
		m[i] = (uint8_t)(i * 7 + 1);
	int bad = 0;
	for (int i = 0; i < SIZE; i++)
		bad += m[i] != (uint8_t)(i * 7 + 1);
	printf("SHMTEST user write/read: %s\n", bad ? "FAIL" : "ok");

	/* 2. kernel copies out of the mapping (write() from it) */
	int pf[2];
	pipe(pf);
	char buf[4096];
	ssize_t w = write(pf[1], m + 8192, sizeof(buf));
	ssize_t r = read(pf[0], buf, sizeof(buf));
	printf("SHMTEST kernel copy from blob: %s (%zd/%zd)\n",
	       w == sizeof(buf) && r == sizeof(buf) && !memcmp(buf, m + 8192, sizeof(buf)) ?
	       "ok" : "FAIL", w, r);

	/* 3. kernel copies into the mapping (read() into it) */
	memset(buf, 0x5a, sizeof(buf));
	write(pf[1], buf, sizeof(buf));
	r = read(pf[0], m + 4096 * 3, sizeof(buf));
	bad = 0;
	for (int i = 0; i < 4096; i++)
		bad += m[4096 * 3 + i] != 0x5a;
	printf("SHMTEST kernel copy into blob: %s (%zd)\n", r == sizeof(buf) && !bad ? "ok" : "FAIL", r);

	/* marker the host checks: first bytes of the blob */
	memcpy(m, "UMLGPU-SHM-OK", 13);
	msync(m, SIZE, MS_SYNC);
	printf("SHMTEST done\n");
	return 0;
}

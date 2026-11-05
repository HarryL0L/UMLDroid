// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * umlgpu: a virtio-gpu device for User-Mode Linux, as a vhost-user backend.
 *
 *   umlgpu SOCKET FB_FILE WIDTH HEIGHT
 *
 * UML connects with virtio_uml.device=SOCKET:16. This first version is a 2D
 * device (no 3D contexts): it keeps the guest's resources, copies their
 * contents on TRANSFER_TO_HOST_2D and, on RESOURCE_FLUSH of the scanout,
 * writes the frame as RGBA into FB_FILE in the format of the kernel's vkms
 * export (4 KiB header "UMFB", then the pixels), so the existing viewers work
 * unchanged.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "libvhost-user.h"
#include "standard-headers/linux/virtio_gpu.h"

#define UMFB_MAGIC	0x42464d55
#define UMFB_HDR	4096
#define UMFB_SIZE	(32u << 20)
#define DRM_FORMAT_ABGR8888 0x34324241

struct umfb {
	uint32_t magic, version, width, height, stride, format;
	uint64_t seq, frames;
};

static int verbose;
/*
 * UMLGPU_BLOBS=1: test mode for host-visible memory. The device offers 3D,
 * context init and blob resources and a host-visible shared memory region;
 * HOST3D blobs are memfds mapped into the guest with SHMEM_MAP.
 */
static int blobs;
#define SHM_ID_HOST_VISIBLE	1
#define SHM_SIZE		(4ull << 30)	/* guest address space only */
#define TEST_CAPSET_ID		3	/* VIRTIO_GPU_CAPSET_GFXSTREAM_VULKAN */
#define VHOST_USER_PROTOCOL_F_SHMEM	20
#define VHOST_USER_GET_SHMEM_CONFIG	44
#define VHOST_USER_BACKEND_SHMEM_MAP	9
#define VHOST_USER_BACKEND_SHMEM_UNMAP	10
static VuDev *gdev;
#define dbg(...) do { if (verbose) fprintf(stderr, "umlgpu: " __VA_ARGS__); } while (0)

/* ---------------- resources ---------------- */

struct resource {
	uint32_t id, format, width, height;
	int blob_fd;			/* host memory of a blob resource, or -1 */
	uint64_t blob_size;
	int64_t map_offset;		/* where it is mapped in the region, or -1 */
	uint8_t *data;			/* host copy, width * 4 per row */
	struct iovec *iov;		/* guest backing, mapped */
	unsigned int niov;
	struct resource *next;
};

static struct resource *resources;
static uint32_t scanout_res;		/* resource shown on scanout 0 */
static struct virtio_gpu_rect scanout_rect;	/* the part of it that is shown */
static uint32_t disp_w, disp_h;
static struct umfb *fb;

static struct resource *find_res(uint32_t id)
{
	for (struct resource *r = resources; r; r = r->next)
		if (r->id == id)
			return r;
	return NULL;
}

static void free_backing(struct resource *r)
{
	free(r->iov);
	r->iov = NULL;
	r->niov = 0;
}

/* Copy @len bytes at @off of the guest backing to @dst */
static void iov_read(struct resource *r, size_t off, uint8_t *dst, size_t len)
{
	for (unsigned int i = 0; i < r->niov && len; i++) {
		struct iovec *v = &r->iov[i];

		if (off >= v->iov_len) {
			off -= v->iov_len;
			continue;
		}
		size_t n = v->iov_len - off < len ? v->iov_len - off : len;
		memcpy(dst, (uint8_t *)v->iov_base + off, n);
		dst += n;
		len -= n;
		off = 0;
	}
}

/* Byte positions of R, G, B in a pixel of a virtio-gpu format, -1: unsupported */
static int format_order(uint32_t f, int *r, int *g, int *b)
{
	switch (f) {
	case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
	case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
		*b = 0; *g = 1; *r = 2; return 0;
	case VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM:
	case VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM:
		*r = 1; *g = 2; *b = 3; return 0;
	case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
	case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
		*r = 0; *g = 1; *b = 2; return 0;
	case VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM:
	case VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM:
		*b = 1; *g = 2; *r = 3; return 0;
	}
	return -1;
}

/* Present: the scanout resource, as RGBA, into the frame file */
static void present(struct resource *r)
{
	int ri, gi, bi;

	if (!fb || !r || format_order(r->format, &ri, &gi, &bi) < 0)
		return;
	/* the scanout rectangle: buffers are often wider than the display */
	uint32_t x0 = scanout_rect.x, y0 = scanout_rect.y;
	uint32_t w = scanout_rect.width, h = scanout_rect.height;
	if (!w || !h || (uint64_t)x0 + w > r->width || (uint64_t)y0 + h > r->height) {
		x0 = y0 = 0;
		w = r->width;
		h = r->height;
	}
	if (UMFB_HDR + (size_t)w * h * 4 > UMFB_SIZE)
		return;
	__atomic_store_n(&fb->seq, fb->seq + 1, __ATOMIC_RELEASE);
	fb->width = w;
	fb->height = h;
	fb->stride = w * 4;
	uint8_t *dst = (uint8_t *)fb + UMFB_HDR;
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *src = r->data + ((size_t)(y0 + y) * r->width + x0) * 4;

		for (uint32_t x = 0; x < w; x++, src += 4, dst += 4) {
			dst[0] = src[ri];
			dst[1] = src[gi];
			dst[2] = src[bi];
			dst[3] = 0xff;
		}
	}
	fb->frames++;
	__atomic_store_n(&fb->seq, fb->seq + 1, __ATOMIC_RELEASE);
}

/* ---------------- shared memory (backend requests) ---------------- */

struct shm_req {
	uint32_t request, flags, size;
	uint8_t shmid, pad[7];
	uint64_t fd_offset, shm_offset, len, flags2;
} __attribute__((packed));

/* Ask the frontend to map (fd >= 0) or unmap a range of the shared memory region */
static int shm_request(int fd, uint64_t shm_offset, uint64_t len)
{
	struct shm_req r = {
		.request = fd >= 0 ? VHOST_USER_BACKEND_SHMEM_MAP : VHOST_USER_BACKEND_SHMEM_UNMAP,
		.flags = 1 | (1 << 3),		/* version 1, need reply */
		.size = 40,
		.shmid = SHM_ID_HOST_VISIBLE,
		.shm_offset = shm_offset,
		.len = len,
		.flags2 = 1,			/* read-write */
	};
	char cbuf[CMSG_SPACE(sizeof(int))] = {};
	struct iovec iov = { &r, sizeof(r) };
	struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1 };
	struct { uint32_t request, flags, size; uint64_t value; } __attribute__((packed)) reply;

	if (fd >= 0) {
		m.msg_control = cbuf;
		m.msg_controllen = sizeof(cbuf);
		struct cmsghdr *c = CMSG_FIRSTHDR(&m);
		c->cmsg_level = SOL_SOCKET;
		c->cmsg_type = SCM_RIGHTS;
		c->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(c), &fd, sizeof(int));
	}
	if (gdev->backend_fd < 0 || sendmsg(gdev->backend_fd, &m, 0) != sizeof(r))
		return -1;
	if (read(gdev->backend_fd, &reply, sizeof(reply)) != sizeof(reply))
		return -1;
	dbg("shmem %s at 0x%llx len 0x%llx -> %llu\n", fd >= 0 ? "map" : "unmap",
	    (unsigned long long)shm_offset, (unsigned long long)len,
	    (unsigned long long)reply.value);
	return reply.value ? -1 : 0;
}

/* ---------------- commands ---------------- */

static size_t iov_copy_out(const struct iovec *iov, unsigned int n, void *buf, size_t len)
{
	size_t done = 0;

	for (unsigned int i = 0; i < n && done < len; i++) {
		size_t c = iov[i].iov_len < len - done ? iov[i].iov_len : len - done;
		memcpy((uint8_t *)buf + done, iov[i].iov_base, c);
		done += c;
	}
	return done;
}

static size_t iov_copy_in(const struct iovec *iov, unsigned int n, const void *buf, size_t len)
{
	size_t done = 0;

	for (unsigned int i = 0; i < n && done < len; i++) {
		size_t c = iov[i].iov_len < len - done ? iov[i].iov_len : len - done;
		memcpy(iov[i].iov_base, (const uint8_t *)buf + done, c);
		done += c;
	}
	return done;
}

static uint32_t cmd_attach_backing(VuDev *dev, const uint8_t *req, size_t len)
{
	const struct virtio_gpu_resource_attach_backing *ab = (const void *)req;
	const struct virtio_gpu_mem_entry *e = (const void *)(ab + 1);
	struct resource *r = find_res(ab->resource_id);

	if (!r)
		return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
	if (len < sizeof(*ab) + (size_t)ab->nr_entries * sizeof(*e) || ab->nr_entries > 16384)
		return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
	free_backing(r);
	r->iov = calloc(ab->nr_entries * 4 + 1, sizeof(*r->iov));
	for (uint32_t i = 0; i < ab->nr_entries; i++) {
		uint64_t addr = e[i].addr, left = e[i].length;

		/* a guest-contiguous entry may span host mappings */
		while (left) {
			uint64_t l = left;
			void *va = vu_gpa_to_va(dev, &l, addr);

			if (!va || !l) {
				free_backing(r);
				return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
			}
			if (r->niov >= ab->nr_entries * 4 + 1)
				r->iov = realloc(r->iov, (r->niov + 16) * sizeof(*r->iov));
			r->iov[r->niov].iov_base = va;
			r->iov[r->niov].iov_len = l;
			r->niov++;
			addr += l;
			left -= l;
		}
	}
	return VIRTIO_GPU_RESP_OK_NODATA;
}

#ifdef HAVE_GFXSTREAM
#include "umlgpu-gfx.inc"
#else
#define gfx 0
#endif

static uint32_t handle(VuDev *dev, const uint8_t *req, size_t len, uint8_t *resp, size_t *rlen)
{
	const struct virtio_gpu_ctrl_hdr *h = (const void *)req;

	*rlen = sizeof(struct virtio_gpu_ctrl_hdr);
#ifdef HAVE_GFXSTREAM
	if (gfx) {
		uint32_t type;

		if (gfx_handle(dev, req, len, resp, rlen, &type))
			return type;
	}
#endif
	switch (h->type) {
	case VIRTIO_GPU_CMD_GET_DISPLAY_INFO: {
		struct virtio_gpu_resp_display_info *di = (void *)resp;

		memset(di, 0, sizeof(*di));
		di->pmodes[0].r.width = disp_w;
		di->pmodes[0].r.height = disp_h;
		di->pmodes[0].enabled = 1;
		*rlen = sizeof(*di);
		return VIRTIO_GPU_RESP_OK_DISPLAY_INFO;
	}
	case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D: {
		const struct virtio_gpu_resource_create_2d *c = (const void *)req;
		int a, b, cc;

		if (len < sizeof(*c))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
		if (!c->resource_id || find_res(c->resource_id))
			return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
		if (format_order(c->format, &a, &b, &cc) < 0 || !c->width || !c->height ||
		    c->width > 8192 || c->height > 8192)
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
		struct resource *r = calloc(1, sizeof(*r));
		r->id = c->resource_id;
		r->format = c->format;
		r->width = c->width;
		r->height = c->height;
		r->data = calloc((size_t)c->width * c->height, 4);
		r->blob_fd = -1;
		r->next = resources;
		resources = r;
		dbg("create %u %ux%u fmt %u\n", r->id, r->width, r->height, r->format);
		return VIRTIO_GPU_RESP_OK_NODATA;
	}
	case VIRTIO_GPU_CMD_RESOURCE_UNREF: {
		const struct virtio_gpu_resource_unref *u = (const void *)req;

		for (struct resource **p = &resources; *p; p = &(*p)->next) {
			if ((*p)->id == u->resource_id) {
				struct resource *r = *p;

				*p = r->next;
				free_backing(r);
				if (r->blob_fd >= 0) {
					char head[16] = {};

					pread(r->blob_fd, head, 15, 0);
					fprintf(stderr, "umlgpu: blob %u freed, starts with '%s'\n", r->id, head);
					close(r->blob_fd);
				}
				free(r->data);
				free(r);
				if (scanout_res == u->resource_id)
					scanout_res = 0;
				return VIRTIO_GPU_RESP_OK_NODATA;
			}
		}
		return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
	}
	case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
		return cmd_attach_backing(dev, req, len);
	case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING: {
		const struct virtio_gpu_resource_detach_backing *d = (const void *)req;
		struct resource *r = find_res(d->resource_id);

		if (!r)
			return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
		free_backing(r);
		return VIRTIO_GPU_RESP_OK_NODATA;
	}
	case VIRTIO_GPU_CMD_SET_SCANOUT: {
		const struct virtio_gpu_set_scanout *s = (const void *)req;

		if (s->scanout_id != 0)
			return VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
		if (s->resource_id && !find_res(s->resource_id))
			return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
		scanout_res = s->resource_id;
		scanout_rect = s->r;
		dbg("scanout %u\n", scanout_res);
		return VIRTIO_GPU_RESP_OK_NODATA;
	}
	case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D: {
		const struct virtio_gpu_transfer_to_host_2d *t = (const void *)req;
		struct resource *r = find_res(t->resource_id);

		if (!r)
			return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
		if (!r->iov)
			return VIRTIO_GPU_RESP_ERR_UNSPEC;
		if ((uint64_t)t->r.x + t->r.width > r->width || (uint64_t)t->r.y + t->r.height > r->height)
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
		size_t stride = (size_t)r->width * 4;
		/* as QEMU: the backing has the resource's layout, offset = first byte */
		for (uint32_t y = 0; y < t->r.height; y++)
			iov_read(r, t->offset + stride * y,
				 r->data + (t->r.y + y) * stride + t->r.x * 4, (size_t)t->r.width * 4);
		return VIRTIO_GPU_RESP_OK_NODATA;
	}
	case VIRTIO_GPU_CMD_RESOURCE_FLUSH: {
		const struct virtio_gpu_resource_flush *f = (const void *)req;

		if (f->resource_id == scanout_res)
			present(find_res(scanout_res));
		return VIRTIO_GPU_RESP_OK_NODATA;
	}
	case VIRTIO_GPU_CMD_GET_CAPSET_INFO: {
		const struct virtio_gpu_get_capset_info *ci = (const void *)req;
		struct virtio_gpu_resp_capset_info *ri = (void *)resp;

		if (!blobs || ci->capset_index != 0)
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
		ri->capset_id = TEST_CAPSET_ID;
		ri->capset_max_version = 1;
		ri->capset_max_size = 64;
		*rlen = sizeof(*ri);
		return VIRTIO_GPU_RESP_OK_CAPSET_INFO;
	}
	case VIRTIO_GPU_CMD_GET_CAPSET: {
		if (!blobs)
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
		memset(resp + sizeof(struct virtio_gpu_ctrl_hdr), 0, 64);
		*rlen = sizeof(struct virtio_gpu_ctrl_hdr) + 64;
		return VIRTIO_GPU_RESP_OK_CAPSET;
	}
	case VIRTIO_GPU_CMD_CTX_CREATE:
	case VIRTIO_GPU_CMD_CTX_DESTROY:
	case VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE:
	case VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE:
	case VIRTIO_GPU_CMD_SUBMIT_3D:
		return blobs ? VIRTIO_GPU_RESP_OK_NODATA : VIRTIO_GPU_RESP_ERR_UNSPEC;
	case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB: {
		const struct virtio_gpu_resource_create_blob *cb = (const void *)req;

		if (!blobs || cb->blob_mem != VIRTIO_GPU_BLOB_MEM_HOST3D || !cb->size ||
		    cb->size > SHM_SIZE || find_res(cb->resource_id))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
		int fd = memfd_create("umlgpu-blob", MFD_CLOEXEC);
		if (fd < 0 || ftruncate(fd, cb->size) < 0) {
			if (fd >= 0)
				close(fd);
			return VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
		}
		struct resource *r = calloc(1, sizeof(*r));
		r->id = cb->resource_id;
		r->blob_fd = fd;
		r->blob_size = cb->size;
		r->map_offset = -1;
		r->next = resources;
		resources = r;
		dbg("blob %u size %llu\n", r->id, (unsigned long long)cb->size);
		return VIRTIO_GPU_RESP_OK_NODATA;
	}
	case VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB: {
		const struct virtio_gpu_resource_map_blob *mb = (const void *)req;
		struct virtio_gpu_resp_map_info *mi = (void *)resp;
		struct resource *r = find_res(mb->resource_id);

		if (!r || r->blob_fd < 0 || mb->offset + r->blob_size > SHM_SIZE)
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
		/* the frontend takes over the fd it receives: pass a duplicate */
		int fd = dup(r->blob_fd);
		if (shm_request(fd, mb->offset, (r->blob_size + 4095) & ~4095ull) < 0) {
			close(fd);
			return VIRTIO_GPU_RESP_ERR_UNSPEC;
		}
		close(fd);	/* sendmsg duplicated it into the frontend */
		r->map_offset = mb->offset;
		mi->map_info = VIRTIO_GPU_MAP_CACHE_CACHED;
		*rlen = sizeof(*mi);
		return VIRTIO_GPU_RESP_OK_MAP_INFO;
	}
	case VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB: {
		const struct virtio_gpu_resource_unmap_blob *ub = (const void *)req;
		struct resource *r = find_res(ub->resource_id);

		if (!r || r->map_offset < 0)
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
		if (shm_request(-1, r->map_offset, (r->blob_size + 4095) & ~4095ull) < 0)
			return VIRTIO_GPU_RESP_ERR_UNSPEC;
		r->map_offset = -1;
		return VIRTIO_GPU_RESP_OK_NODATA;
	}
	case VIRTIO_GPU_CMD_UPDATE_CURSOR:
	case VIRTIO_GPU_CMD_MOVE_CURSOR:
		return VIRTIO_GPU_RESP_OK_NODATA;
	default:
		dbg("unsupported command 0x%x\n", h->type);
		return VIRTIO_GPU_RESP_ERR_UNSPEC;
	}
}

static void queue_handler(VuDev *dev, int qidx)
{
	VuVirtq *vq = vu_get_queue(dev, qidx);
	VuVirtqElement *elem;
	static uint8_t req[1 << 20];
	uint8_t resp[1024];

	while ((elem = vu_queue_pop(dev, vq, sizeof(VuVirtqElement)))) {
		size_t len = iov_copy_out(elem->out_sg, elem->out_num, req, sizeof(req)), rlen;
		size_t written = 0;

		if (len >= sizeof(struct virtio_gpu_ctrl_hdr)) {
			const struct virtio_gpu_ctrl_hdr *h = (const void *)req;
			struct virtio_gpu_ctrl_hdr *rh = (void *)resp;

			memset(resp, 0, sizeof(struct virtio_gpu_ctrl_hdr));
			uint32_t type = handle(dev, req, len, resp, &rlen);
			static unsigned long count[64];
			unsigned idx = h->type & 63;
			if (verbose && (++count[idx] <= 3 || (count[idx] & 1023) == 0 || type >= 0x1200))
				fprintf(stderr, "umlgpu: q%d cmd 0x%x res %u -> 0x%x (#%lu)\n", qidx, h->type,
					len >= 28 ? *(uint32_t *)(req + 24) : 0, type, count[idx]);

			rh->type = type;
			/* 2D: everything completes at once, fences are signalled right away */
			if (h->flags & VIRTIO_GPU_FLAG_FENCE) {
				rh->flags = h->flags & (VIRTIO_GPU_FLAG_FENCE | VIRTIO_GPU_FLAG_INFO_RING_IDX);
				rh->fence_id = h->fence_id;
				rh->ctx_id = h->ctx_id;
				rh->ring_idx = h->ring_idx;
			}
			/* the cursor queue expects no response data */
			if (qidx == 0)
				written = iov_copy_in(elem->in_sg, elem->in_num, resp, rlen);
#ifdef HAVE_GFXSTREAM
			/* gfxstream: answer when the fence signals */
			if (gfx && (h->flags & VIRTIO_GPU_FLAG_FENCE) && type < 0x1200) {
				struct virtio_gpu_ctrl_hdr hdr = *h;

				gfx_defer(dev, qidx, elem, written, &hdr);
				continue;
			}
#endif
		}
		vu_queue_push(dev, vq, elem, written);
		vu_queue_notify(dev, vq);
		free(elem);
	}
}

/* ---------------- vhost-user plumbing ---------------- */

static uint64_t get_features(VuDev *dev)
{
	uint64_t f = 1ULL << VIRTIO_F_VERSION_1;

	if (blobs || gfx)
		f |= 1ULL << VIRTIO_GPU_F_VIRGL | 1ULL << VIRTIO_GPU_F_RESOURCE_BLOB |
		     1ULL << VIRTIO_GPU_F_CONTEXT_INIT;
	return f;
}

static void set_features(VuDev *dev, uint64_t features)
{
}

static uint64_t get_protocol_features(VuDev *dev)
{
	uint64_t f = 1ULL << VHOST_USER_PROTOCOL_F_CONFIG;

	if (blobs || gfx)
		f |= 1ULL << VHOST_USER_PROTOCOL_F_SHMEM;
	return f;
}

/* VHOST_USER_GET_SHMEM_CONFIG: region 1 (host visible) only */
static int process_msg(VuDev *dev, VhostUserMsg *m, int *do_reply)
{
	dbg("vhost-user request %d flags 0x%x size %u\n", m->request, m->flags, m->size);
	if (m->request != VHOST_USER_GET_SHMEM_CONFIG)
		return 0;
	uint32_t *n = (void *)&m->payload;
	uint64_t *sizes = (void *)(n + 2);

	n[0] = 2;
	n[1] = 0;
	sizes[0] = 0;
	sizes[1] = SHM_SIZE;
	m->size = 8 + 2 * 8;
	*do_reply = 1;
	return 1;
}

static int get_config(VuDev *dev, uint8_t *config, uint32_t len)
{
	struct virtio_gpu_config c = { .num_scanouts = 1, .num_capsets = blobs ? 1 : 0 };

#ifdef HAVE_GFXSTREAM
	if (gfx)
		c.num_capsets = ncapsets;
#endif

	memcpy(config, &c, len < sizeof(c) ? len : sizeof(c));
	return 0;
}

static void queue_set_started(VuDev *dev, int qidx, bool started)
{
	vu_set_queue_handler(dev, vu_get_queue(dev, qidx), started ? queue_handler : NULL);
}

static const VuDevIface iface = {
	.get_features = get_features,
	.set_features = set_features,
	.get_protocol_features = get_protocol_features,
	.process_msg = process_msg,
	.get_config = get_config,
	.queue_set_started = queue_set_started,
};

#define MAX_WATCH 16
static struct {
	int fd;
	int cond;
	vu_watch_cb cb;
	void *data;
} watches[MAX_WATCH];
static int nwatch;

static void set_watch(VuDev *dev, int fd, int condition, vu_watch_cb cb, void *data)
{
	for (int i = 0; i < nwatch; i++)
		if (watches[i].fd == fd) {
			watches[i].cond = condition;
			watches[i].cb = cb;
			watches[i].data = data;
			return;
		}
	if (nwatch < MAX_WATCH)
		watches[nwatch++] = (typeof(watches[0])){ fd, condition, cb, data };
}

static void remove_watch(VuDev *dev, int fd)
{
	for (int i = 0; i < nwatch; i++)
		if (watches[i].fd == fd) {
			watches[i] = watches[--nwatch];
			return;
		}
}

static void panic_cb(VuDev *dev, const char *msg)
{
	fprintf(stderr, "umlgpu: vhost-user error: %s\n", msg);
	exit(1);
}

/*
 * A crash in gfxstream or the GPU driver otherwise leaves no trace (the app
 * cannot read Android's tombstones): print the signal, fault address and a
 * symbolized backtrace to stderr, i.e. umlgpu.log.
 */
#include <dlfcn.h>
#include <ucontext.h>
#include <unwind.h>

static void crash_line(const char *what, uintptr_t pc)
{
	Dl_info di;
	char b[512];
	int n;

	if (dladdr((void *)pc, &di) && di.dli_fname)
		n = snprintf(b, sizeof(b), "umlgpu:   %s %#lx %s+%#lx %s\n", what, (unsigned long)pc,
			     strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname,
			     (unsigned long)(pc - (uintptr_t)di.dli_fbase),
			     di.dli_sname ? di.dli_sname : "");
	else
		n = snprintf(b, sizeof(b), "umlgpu:   %s %#lx\n", what, (unsigned long)pc);
	if (n > 0)
		write(2, b, n < (int)sizeof(b) ? n : (int)sizeof(b) - 1);
}

static _Unwind_Reason_Code crash_frame(struct _Unwind_Context *c, void *arg)
{
	int *depth = arg;
	uintptr_t pc = _Unwind_GetIP(c);

	if (pc)
		crash_line("at", pc);
	return ++*depth < 40 ? _URC_NO_REASON : _URC_END_OF_STACK;
}

static void crash_handler(int sig, siginfo_t *si, void *uc)
{
	char b[160];
	int depth = 0, n;

	n = snprintf(b, sizeof(b), "umlgpu: crashed: signal %d (%s), code %d, address %p\n", sig,
		     strsignal(sig), si->si_code, si->si_addr);
	write(2, b, n);
#if defined(__aarch64__)
	crash_line("pc", ((ucontext_t *)uc)->uc_mcontext.pc);
	crash_line("lr", ((ucontext_t *)uc)->uc_mcontext.regs[30]);
#elif defined(__x86_64__)
	crash_line("pc", ((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP]);
#endif
	_Unwind_Backtrace(crash_frame, &depth);
	signal(sig, SIG_DFL);
	raise(sig);
}

static void crash_setup(void)
{
	static char altstack[64 * 1024];
	stack_t ss = { .ss_sp = altstack, .ss_size = sizeof(altstack) };
	struct sigaction sa = { .sa_sigaction = crash_handler, .sa_flags = SA_SIGINFO | SA_ONSTACK };
	const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };

	sigaltstack(&ss, NULL);
	sigemptyset(&sa.sa_mask);
	for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
		sigaction(sigs[i], &sa, NULL);
}

int main(int argc, char **argv)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	VuDev dev;
	int ls, cs, fd;

	if (argc < 5) {
		fprintf(stderr, "usage: umlgpu SOCKET FB_FILE WIDTH HEIGHT\n");
		return 2;
	}
	verbose = getenv("UMLGPU_DEBUG") != NULL;
	blobs = getenv("UMLGPU_BLOBS") != NULL;
#ifdef HAVE_GFXSTREAM
	gfx = getenv("UMLGPU_GFXSTREAM") != NULL;
	blobs = blobs && !gfx;
#endif
	disp_w = atoi(argv[3]);
	disp_h = atoi(argv[4]);
	signal(SIGPIPE, SIG_IGN);
	crash_setup();

	fd = open(argv[2], O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || ftruncate(fd, UMFB_SIZE) < 0) {
		perror("umlgpu: frame file");
		return 1;
	}
	fb = mmap(NULL, UMFB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		perror("umlgpu: mmap");
		return 1;
	}
	fb->magic = UMFB_MAGIC;
	fb->version = 1;
	fb->format = DRM_FORMAT_ABGR8888;

	unlink(argv[1]);
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", argv[1]);
	ls = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ls < 0 || bind(ls, (void *)&sa, sizeof(sa)) < 0 || listen(ls, 1) < 0) {
		perror("umlgpu: socket");
		return 1;
	}
#ifdef HAVE_GFXSTREAM
	if (gfx && gfx_init(disp_w, disp_h) < 0)
		return 1;
#endif
	fprintf(stderr, "umlgpu: waiting on %s (%ux%u)\n", argv[1], disp_w, disp_h);
	cs = accept(ls, NULL, NULL);
	if (cs < 0) {
		perror("umlgpu: accept");
		return 1;
	}
	if (!vu_init(&dev, 2, cs, panic_cb, NULL, set_watch, remove_watch, &iface)) {
		fprintf(stderr, "umlgpu: vu_init failed\n");
		return 1;
	}
	fprintf(stderr, "umlgpu: connected\n");
	gdev = &dev;

	for (;;) {
		struct pollfd p[MAX_WATCH + 2];

		p[0].fd = cs;
		p[0].events = POLLIN;
		for (int i = 0; i < nwatch; i++) {
			p[i + 1].fd = watches[i].fd;
			p[i + 1].events = watches[i].cond;
		}
		int n = nwatch, extra = 0;
#ifdef HAVE_GFXSTREAM
		if (gfx) {
			p[n + 1].fd = fence_efd;
			p[n + 1].events = POLLIN;
			extra = 1;
		}
#endif
		if (poll(p, n + 1 + extra, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (p[0].revents & (POLLHUP | POLLERR)) {
			fprintf(stderr, "umlgpu: UML disconnected\n");
			break;
		}
		if (p[0].revents & POLLIN && !vu_dispatch(&dev)) {
			fprintf(stderr, "umlgpu: dispatch failed\n");
			break;
		}
		for (int i = 0; i < n; i++)
			if (p[i + 1].revents && i < nwatch && watches[i].fd == p[i + 1].fd)
				watches[i].cb(&dev, p[i + 1].revents, watches[i].data);
#ifdef HAVE_GFXSTREAM
		if (extra && p[n + 1].revents & POLLIN)
			gfx_retire(&dev);
#endif
	}
	vu_deinit(&dev);
	return 0;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * umlgpu: gfxstream mode (UMLGPU_GFXSTREAM=1, built with -DHAVE_GFXSTREAM).
 *
 * The 3D side of virtio-gpu goes to gfxstream's host renderer (the same
 * library crosvm and the Android emulator use): contexts, command streams,
 * resources, blobs and fences. Android's guest drivers (vulkan.ranchu, and
 * ANGLE on top of it for GLES) then run on the host GPU.
 *
 * Host-visible blobs are exported by gfxstream as FDs (dma-buf on Android
 * hosts) and mapped into the guest with SHMEM_MAP. Fences signal
 * asynchronously: fenced commands are answered when gfxstream reports them
 * done. Displayed frames are read back from the scanout resource and written
 * into the frame file, like the 2D mode does.
 *
 * Included from umlgpu.c.
 */
#include <pthread.h>
#include <sys/eventfd.h>
#include "gfxstream/virtio-gpu-gfxstream-renderer.h"

static int gfx;

#define CAPSET_GFXSTREAM_VULKAN		3
#define CAPSET_GFXSTREAM_GLES		8
#define CAPSET_GFXSTREAM_COMPOSER	9

static struct {
	uint32_t id, max_version, max_size;
} capsets[8];
static unsigned int ncapsets;

/* ---- fences ---- */

static int fence_efd = -1;
static pthread_mutex_t fence_lock = PTHREAD_MUTEX_INITIALIZER;
static struct stream_renderer_fence signalled[1024];
static unsigned int nsignalled;

struct pending {
	VuVirtqElement *elem;
	int qidx;
	size_t written;
	uint64_t timeline, fence_id;
	struct pending *next;
};
static struct pending *pending;

/* A fence's timeline: a context's ring, or the global one */
static uint64_t timeline(uint32_t flags, uint32_t ctx_id, uint8_t ring_idx)
{
	return flags ? (1ULL << 63) | (uint64_t)ctx_id << 8 | ring_idx : 0;
}

/* gfxstream's threads: queue the fence, wake the main loop */
static void fence_cb(void *user_data, struct stream_renderer_fence *f)
{
	uint64_t one = 1;

	pthread_mutex_lock(&fence_lock);
	if (nsignalled < sizeof(signalled) / sizeof(signalled[0]))
		signalled[nsignalled++] = *f;
	pthread_mutex_unlock(&fence_lock);
	if (write(fence_efd, &one, sizeof(one)) < 0)
		;
}

/* Hold a fenced command's answer until gfxstream signals the fence */
static void gfx_defer(VuDev *dev, int qidx, VuVirtqElement *elem, size_t written,
		      const struct virtio_gpu_ctrl_hdr *h)
{
	bool ring = h->flags & VIRTIO_GPU_FLAG_INFO_RING_IDX;
	struct stream_renderer_fence f = {
		.flags = STREAM_RENDERER_FLAG_FENCE | (ring ? STREAM_RENDERER_FLAG_FENCE_RING_IDX : 0),
		.fence_id = h->fence_id,
		.ctx_id = h->ctx_id,
		.ring_idx = h->ring_idx,
	};
	struct pending *p = calloc(1, sizeof(*p)), **t;

	p->elem = elem;
	p->qidx = qidx;
	p->written = written;
	p->timeline = timeline(ring, h->ctx_id, h->ring_idx);
	p->fence_id = h->fence_id;
	for (t = &pending; *t; t = &(*t)->next)
		;
	*t = p;
	stream_renderer_create_fence(&f);
}

/* Main loop: answer the commands whose fences have signalled */
static void gfx_retire(VuDev *dev)
{
	struct stream_renderer_fence done[1024];
	unsigned int n;
	uint64_t v;

	if (read(fence_efd, &v, sizeof(v)) < 0)
		;
	pthread_mutex_lock(&fence_lock);
	n = nsignalled;
	memcpy(done, signalled, n * sizeof(done[0]));
	nsignalled = 0;
	pthread_mutex_unlock(&fence_lock);

	for (unsigned int i = 0; i < n; i++) {
		uint64_t tl = timeline(done[i].flags & STREAM_RENDERER_FLAG_FENCE_RING_IDX,
				       done[i].ctx_id, done[i].ring_idx);

		for (struct pending **p = &pending; *p; ) {
			struct pending *q = *p;

			if (q->timeline != tl || q->fence_id > done[i].fence_id) {
				p = &q->next;
				continue;
			}
			*p = q->next;
			vu_queue_push(dev, vu_get_queue(dev, q->qidx), q->elem, q->written);
			vu_queue_notify(dev, vu_get_queue(dev, q->qidx));
			free(q->elem);
			free(q);
		}
	}
}

/* ---- setup ---- */

static void debug_cb(void *user_data, struct stream_renderer_debug *d)
{
	if (verbose || d->debug_type <= STREAM_RENDERER_DEBUG_WARN)
		fprintf(stderr, "gfxstream: %s\n", d->message);
}

static int gfx_init(uint32_t w, uint32_t h)
{
	const char *env = getenv("UMLGPU_FLAGS");
	/* Vulkan only (guest GLES is ANGLE on Vulkan); blobs exportable as FDs */
	uint64_t flags = env ? strtoull(env, NULL, 0) :
		STREAM_RENDERER_FLAGS_USE_VK_BIT | STREAM_RENDERER_FLAGS_USE_EXTERNAL_BLOB |
		STREAM_RENDERER_FLAGS_USE_SYSTEM_BLOB;
	struct stream_renderer_param p[] = {
		{ STREAM_RENDERER_PARAM_USER_DATA, 0 },
		{ STREAM_RENDERER_PARAM_RENDERER_FLAGS, flags },
		{ STREAM_RENDERER_PARAM_FENCE_CALLBACK, (uintptr_t)fence_cb },
		{ STREAM_RENDERER_PARAM_WIN0_WIDTH, w },
		{ STREAM_RENDERER_PARAM_WIN0_HEIGHT, h },
		{ STREAM_RENDERER_PARAM_DEBUG_CALLBACK, (uintptr_t)debug_cb },
	};
	static const uint32_t ids[] = {
		CAPSET_GFXSTREAM_VULKAN, CAPSET_GFXSTREAM_GLES, CAPSET_GFXSTREAM_COMPOSER,
	};

	fence_efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (fence_efd < 0)
		return -1;
	int err = stream_renderer_init(p, sizeof(p) / sizeof(p[0]));
	if (err) {
		fprintf(stderr, "umlgpu: stream_renderer_init failed: %d\n", err);
		return -1;
	}
	for (unsigned int i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		uint32_t ver = 0, size = 0;

		stream_renderer_get_cap_set(ids[i], &ver, &size);
		if (!size)
			continue;
		capsets[ncapsets].id = ids[i];
		capsets[ncapsets].max_version = ver;
		capsets[ncapsets].max_size = size;
		ncapsets++;
		fprintf(stderr, "umlgpu: capset %u: version %u, %u bytes\n", ids[i], ver, size);
	}
	fprintf(stderr, "umlgpu: gfxstream ready (flags 0x%llx)\n", (unsigned long long)flags);
	return 0;
}

/* ---- display ---- */

static struct {
	bool blob;
	uint32_t width, height, format, stride, offset;
} scanout_blob;

/* Read the scanout resource back from gfxstream and show it */
static void gfx_present(struct resource *r)
{
	static uint8_t *buf;
	static size_t bufsize;
	uint32_t w = r->width, h = r->height, fmt = r->format;

	if (scanout_blob.blob) {
		w = scanout_blob.width;
		h = scanout_blob.height;
		fmt = scanout_blob.format;
	}
	static unsigned long presented, failed;

	if (!w || !h || w > 8192 || h > 8192) {
		if (failed++ < 10)
			fprintf(stderr, "umlgpu: scanout %u has no size (%ux%u)\n", r->id, w, h);
		return;
	}
	size_t need = (size_t)w * h * 4;
	if (need > bufsize) {
		free(buf);
		buf = malloc(need);
		bufsize = need;
	}

	if (scanout_blob.blob) {
		void *hva;
		uint64_t size;

		if (stream_renderer_resource_map(r->id, &hva, &size)) {
			if (failed++ < 10)
				fprintf(stderr, "umlgpu: cannot map scanout blob %u\n", r->id);
			return;
		}
		for (uint32_t y = 0; y < h; y++) {
			uint64_t off = scanout_blob.offset + (uint64_t)y * scanout_blob.stride;

			if (off + (size_t)w * 4 > size)
				break;
			memcpy(buf + (size_t)y * w * 4, (uint8_t *)hva + off, (size_t)w * 4);
		}
		stream_renderer_resource_unmap(r->id);
	} else {
		struct stream_renderer_box box = { 0, 0, 0, w, h, 1 };
		struct iovec iov = { buf, need };

		if (stream_renderer_transfer_read_iov(r->id, 0, 0, w * 4, 0, &box, 0, &iov, 1)) {
			if (failed++ < 10)
				fprintf(stderr, "umlgpu: cannot read back scanout %u (%ux%u fmt %u)\n",
					r->id, w, h, fmt);
			return;
		}
	}

	struct resource view = { .id = r->id, .format = fmt, .width = w, .height = h, .data = buf };
	present(&view);
	if (++presented == 1 || presented % 600 == 0)
		fprintf(stderr, "umlgpu: %lu frames shown (%ux%u fmt %u)\n", presented, w, h, fmt);
}

/* ---- commands ---- */

static int gfx_handle(VuDev *dev, const uint8_t *req, size_t len, uint8_t *resp,
		      size_t *rlen, uint32_t *type)
{
	const struct virtio_gpu_ctrl_hdr *h = (const void *)req;

#define OK(t) do { *type = (t); return 1; } while (0)
	switch (h->type) {
	case VIRTIO_GPU_CMD_GET_CAPSET_INFO: {
		const struct virtio_gpu_get_capset_info *ci = (const void *)req;
		struct virtio_gpu_resp_capset_info *ri = (void *)resp;

		if (ci->capset_index >= ncapsets)
			OK(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		ri->capset_id = capsets[ci->capset_index].id;
		ri->capset_max_version = capsets[ci->capset_index].max_version;
		ri->capset_max_size = capsets[ci->capset_index].max_size;
		*rlen = sizeof(*ri);
		OK(VIRTIO_GPU_RESP_OK_CAPSET_INFO);
	}
	case VIRTIO_GPU_CMD_GET_CAPSET: {
		const struct virtio_gpu_get_capset *gc = (const void *)req;
		uint32_t ver = 0, size = 0;

		stream_renderer_get_cap_set(gc->capset_id, &ver, &size);
		if (!size || size > 1000 - sizeof(struct virtio_gpu_ctrl_hdr))
			OK(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		stream_renderer_fill_caps(gc->capset_id, gc->capset_version,
					  resp + sizeof(struct virtio_gpu_ctrl_hdr));
		*rlen = sizeof(struct virtio_gpu_ctrl_hdr) + size;
		OK(VIRTIO_GPU_RESP_OK_CAPSET);
	}
	case VIRTIO_GPU_CMD_CTX_CREATE: {
		const struct virtio_gpu_ctx_create *cc = (const void *)req;
		uint32_t nlen = cc->nlen < 64 ? cc->nlen : 64;

		if (stream_renderer_context_create(h->ctx_id, nlen, cc->debug_name, cc->context_init))
			OK(VIRTIO_GPU_RESP_ERR_UNSPEC);
		OK(VIRTIO_GPU_RESP_OK_NODATA);
	}
	case VIRTIO_GPU_CMD_CTX_DESTROY:
		stream_renderer_context_destroy(h->ctx_id);
		OK(VIRTIO_GPU_RESP_OK_NODATA);
	case VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE:
	case VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE: {
		const struct virtio_gpu_ctx_resource *cr = (const void *)req;

		if (h->type == VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE)
			stream_renderer_ctx_attach_resource(h->ctx_id, cr->resource_id);
		else
			stream_renderer_ctx_detach_resource(h->ctx_id, cr->resource_id);
		OK(VIRTIO_GPU_RESP_OK_NODATA);
	}
	case VIRTIO_GPU_CMD_SUBMIT_3D: {
		const struct virtio_gpu_cmd_submit *cs = (const void *)req;
		struct stream_renderer_command cmd = {
			.ctx_id = h->ctx_id,
			.cmd_size = cs->size,
			.cmd = (uint8_t *)(cs + 1),
		};

		if (len < sizeof(*cs) + cs->size)
			OK(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		if (stream_renderer_submit_cmd(&cmd))
			OK(VIRTIO_GPU_RESP_ERR_UNSPEC);
		OK(VIRTIO_GPU_RESP_OK_NODATA);
	}
	case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
	case VIRTIO_GPU_CMD_RESOURCE_CREATE_3D: {
		struct stream_renderer_resource_create_args a = { 0 };
		uint32_t id;

		if (h->type == VIRTIO_GPU_CMD_RESOURCE_CREATE_2D) {
			const struct virtio_gpu_resource_create_2d *c = (const void *)req;

			id = c->resource_id;
			a = (struct stream_renderer_resource_create_args){
				.handle = id, .target = 2 /* PIPE_TEXTURE_2D */, .format = c->format,
				.bind = 1 << 1 /* render target */, .width = c->width,
				.height = c->height, .depth = 1, .array_size = 1,
			};
		} else {
			const struct virtio_gpu_resource_create_3d *c = (const void *)req;

			id = c->resource_id;
			a = (struct stream_renderer_resource_create_args){
				.handle = id, .target = c->target, .format = c->format,
				.bind = c->bind, .width = c->width, .height = c->height,
				.depth = c->depth, .array_size = c->array_size,
				.last_level = c->last_level, .nr_samples = c->nr_samples,
				.flags = c->flags,
			};
		}
		if (!id || find_res(id))
			OK(VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
		if (stream_renderer_resource_create(&a, NULL, 0))
			OK(VIRTIO_GPU_RESP_ERR_UNSPEC);
		struct resource *r = calloc(1, sizeof(*r));
		r->id = id;
		r->format = a.format;
		r->width = a.width;
		r->height = a.height;
		r->blob_fd = -1;
		r->map_offset = -1;
		r->next = resources;
		resources = r;
		OK(VIRTIO_GPU_RESP_OK_NODATA);
	}
	case VIRTIO_GPU_CMD_RESOURCE_UNREF: {
		const struct virtio_gpu_resource_unref *u = (const void *)req;

		for (struct resource **p = &resources; *p; p = &(*p)->next) {
			if ((*p)->id != u->resource_id)
				continue;
			struct resource *r = *p;
			*p = r->next;
			stream_renderer_resource_unref(r->id);
			free_backing(r);
			free(r);
			if (scanout_res == u->resource_id)
				scanout_res = 0;
			OK(VIRTIO_GPU_RESP_OK_NODATA);
		}
		OK(VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
	}
	case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING: {
		const struct virtio_gpu_resource_attach_backing *ab = (const void *)req;
		uint32_t t = cmd_attach_backing(dev, req, len);
		struct resource *r = find_res(ab->resource_id);

		if (t != VIRTIO_GPU_RESP_OK_NODATA || !r)
			OK(t);
		if (stream_renderer_resource_attach_iov(r->id, r->iov, r->niov))
			OK(VIRTIO_GPU_RESP_ERR_UNSPEC);
		OK(VIRTIO_GPU_RESP_OK_NODATA);
	}
	case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING: {
		const struct virtio_gpu_resource_detach_backing *d = (const void *)req;
		struct resource *r = find_res(d->resource_id);
		struct iovec *iov = NULL;
		int n = 0;

		if (!r)
			OK(VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
		stream_renderer_resource_detach_iov(r->id, &iov, &n);
		free_backing(r);
		OK(VIRTIO_GPU_RESP_OK_NODATA);
	}
	case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D: {
		const struct virtio_gpu_transfer_to_host_2d *t = (const void *)req;
		struct stream_renderer_box box = { t->r.x, t->r.y, 0, t->r.width, t->r.height, 1 };

		if (stream_renderer_transfer_write_iov(t->resource_id, 0, 0, 0, 0, &box,
						       t->offset, NULL, 0))
			OK(VIRTIO_GPU_RESP_ERR_UNSPEC);
		OK(VIRTIO_GPU_RESP_OK_NODATA);
	}
	case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D:
	case VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D: {
		const struct virtio_gpu_transfer_host_3d *t = (const void *)req;
		struct stream_renderer_box box = {
			t->box.x, t->box.y, t->box.z, t->box.w, t->box.h, t->box.d,
		};
		int err = h->type == VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D ?
			stream_renderer_transfer_write_iov(t->resource_id, h->ctx_id, t->level,
							   t->stride, t->layer_stride, &box,
							   t->offset, NULL, 0) :
			stream_renderer_transfer_read_iov(t->resource_id, h->ctx_id, t->level,
							  t->stride, t->layer_stride, &box,
							  t->offset, NULL, 0);
		OK(err ? VIRTIO_GPU_RESP_ERR_UNSPEC : VIRTIO_GPU_RESP_OK_NODATA);
	}
	case VIRTIO_GPU_CMD_SET_SCANOUT: {
		const struct virtio_gpu_set_scanout *s = (const void *)req;

		if (s->scanout_id != 0)
			OK(VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID);
		static int n;
		if (n++ < 5)
			fprintf(stderr, "umlgpu: set scanout %u %ux%u\n", s->resource_id, s->r.width, s->r.height);
		scanout_res = s->resource_id;
		scanout_rect = s->r;
		scanout_blob.blob = false;
		OK(VIRTIO_GPU_RESP_OK_NODATA);
	}
	case VIRTIO_GPU_CMD_SET_SCANOUT_BLOB: {
		const struct virtio_gpu_set_scanout_blob *s = (const void *)req;

		if (s->scanout_id != 0)
			OK(VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID);
		static int n;
		if (n++ < 5)
			fprintf(stderr, "umlgpu: set scanout blob %u %ux%u fmt %u stride %u\n", s->resource_id,
				s->width, s->height, s->format, s->strides[0]);
		scanout_res = s->resource_id;
		scanout_rect = s->r;
		scanout_blob.blob = s->resource_id != 0;
		scanout_blob.width = s->width;
		scanout_blob.height = s->height;
		scanout_blob.format = s->format;
		scanout_blob.stride = s->strides[0];
		scanout_blob.offset = s->offsets[0];
		OK(VIRTIO_GPU_RESP_OK_NODATA);
	}
	case VIRTIO_GPU_CMD_RESOURCE_FLUSH: {
		const struct virtio_gpu_resource_flush *f = (const void *)req;
		struct resource *r = find_res(f->resource_id);

		if (r && f->resource_id == scanout_res)
			gfx_present(r);
		else {
			static int n;
			if (n++ < 5)
				fprintf(stderr, "umlgpu: flush of %u, scanout is %u%s\n", f->resource_id,
					scanout_res, r ? "" : " (unknown resource)");
		}
		OK(VIRTIO_GPU_RESP_OK_NODATA);
	}
	case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB: {
		const struct virtio_gpu_resource_create_blob *cb = (const void *)req;
		const struct virtio_gpu_mem_entry *e = (const void *)(cb + 1);
		struct stream_renderer_create_blob b = {
			.blob_mem = cb->blob_mem,
			.blob_flags = cb->blob_flags,
			.blob_id = cb->blob_id,
			.size = cb->size,
		};
		struct resource *r;

		if (!cb->resource_id || find_res(cb->resource_id) ||
		    len < sizeof(*cb) + (size_t)cb->nr_entries * sizeof(*e))
			OK(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		r = calloc(1, sizeof(*r));
		r->id = cb->resource_id;
		r->blob_fd = -1;
		r->blob_size = cb->size;
		r->map_offset = -1;
		/* guest memory backing (GUEST, HOST3D_GUEST): as ATTACH_BACKING */
		if (cb->nr_entries) {
			r->iov = calloc(cb->nr_entries * 4 + 1, sizeof(*r->iov));
			for (uint32_t i = 0; i < cb->nr_entries; i++) {
				uint64_t addr = e[i].addr, left = e[i].length;

				while (left) {
					uint64_t l = left;
					void *va = vu_gpa_to_va(dev, &l, addr);

					if (!va || !l)
						break;
					if (r->niov >= cb->nr_entries * 4 + 1)
						r->iov = realloc(r->iov, (r->niov + 16) * sizeof(*r->iov));
					r->iov[r->niov].iov_base = va;
					r->iov[r->niov].iov_len = l;
					r->niov++;
					addr += l;
					left -= l;
				}
			}
		}
		if (stream_renderer_create_blob(h->ctx_id, r->id, &b, r->iov, r->niov, NULL)) {
			free_backing(r);
			free(r);
			OK(VIRTIO_GPU_RESP_ERR_UNSPEC);
		}
		r->next = resources;
		resources = r;
		OK(VIRTIO_GPU_RESP_OK_NODATA);
	}
	case VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB: {
		const struct virtio_gpu_resource_map_blob *mb = (const void *)req;
		struct virtio_gpu_resp_map_info *mi = (void *)resp;
		struct resource *r = find_res(mb->resource_id);
		struct stream_renderer_handle hd = { .os_handle = -1 };
		uint32_t map_info = STREAM_RENDERER_MAP_CACHE_CACHED;

		if (!r || mb->offset + r->blob_size > SHM_SIZE)
			OK(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		/* host memory as an FD the frontend can map: dma-buf, memfd, opaque fd */
		{
			static int n;
			if (n++ < 3)
				fprintf(stderr, "umlgpu: map blob %u (%llu bytes)\n", r->id,
					(unsigned long long)r->blob_size);
		}
		if (stream_renderer_export_blob(r->id, &hd) || hd.os_handle < 0) {
			fprintf(stderr, "umlgpu: blob %u cannot be exported\n", r->id);
			OK(VIRTIO_GPU_RESP_ERR_UNSPEC);
		}
		dbg("map blob %u (handle type 0x%x) at 0x%llx\n", r->id, hd.handle_type,
		    (unsigned long long)mb->offset);
		stream_renderer_resource_map_info(r->id, &map_info);
		int err = shm_request((int)hd.os_handle, mb->offset, (r->blob_size + 4095) & ~4095ull);
		close((int)hd.os_handle);
		if (err)
			OK(VIRTIO_GPU_RESP_ERR_UNSPEC);
		r->map_offset = mb->offset;
		mi->map_info = map_info;
		*rlen = sizeof(*mi);
		OK(VIRTIO_GPU_RESP_OK_MAP_INFO);
	}
	case VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB: {
		const struct virtio_gpu_resource_unmap_blob *ub = (const void *)req;
		struct resource *r = find_res(ub->resource_id);

		if (!r || r->map_offset < 0)
			OK(VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		if (shm_request(-1, r->map_offset, (r->blob_size + 4095) & ~4095ull) < 0)
			OK(VIRTIO_GPU_RESP_ERR_UNSPEC);
		r->map_offset = -1;
		OK(VIRTIO_GPU_RESP_OK_NODATA);
	}
	}
#undef OK
	return 0;	/* not ours: the 2D code handles it */
}

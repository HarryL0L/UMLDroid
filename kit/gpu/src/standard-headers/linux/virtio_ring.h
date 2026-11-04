/* The system's UAPI header (QEMU keeps its own copy; two copies clash) */
#pragma once
#include <linux/virtio_ring.h>

/* Older UAPI headers (the NDK's) lack it: the virtio spec's event index check */
static inline int umlgpu_vring_need_event(__u16 event_idx, __u16 new_idx, __u16 old)
{
	return (__u16)(new_idx - event_idx - 1) < (__u16)(new_idx - old);
}
#define vring_need_event umlgpu_vring_need_event

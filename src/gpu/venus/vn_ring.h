#ifndef UEFI_VENUS_RING_DECLARATIONS_H
#define UEFI_VENUS_RING_DECLARATIONS_H
#include "vn_cs.h"
#define VN_TRACE_FUNC() ((void)0)
/* The synchronous probe uses direct encoders. Ring wrappers remain unlinked. */
struct vn_ring;
struct vn_ring_submit_command { struct vn_cs_encoder encoder; struct vn_cs_decoder decoder; };
struct vn_cs_encoder *vn_ring_submit_command_init(struct vn_ring *, struct vn_ring_submit_command *, void *, size_t, size_t);
void vn_ring_submit_command(struct vn_ring *, struct vn_ring_submit_command *);
struct vn_cs_decoder *vn_ring_get_command_reply(struct vn_ring *, struct vn_ring_submit_command *);
void vn_ring_free_command_reply(struct vn_ring *, struct vn_ring_submit_command *);
#endif

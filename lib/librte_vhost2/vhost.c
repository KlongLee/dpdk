/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2018 Intel Corporation
 */

/* Security model
 * --------------
 * The vhost-user protocol connection is an external interface, so it must be
 * robust against invalid inputs.
 *
 * This is important because the vhost-user master is only one step removed
 * from the guest.  Malicious guests that have escaped will then launch further
 * attacks from the vhost-user master.
 *
 * Even in deployments where guests are trusted, a bug in the vhost-user master
 * can still cause invalid messages to be sent.  Such messages must not
 * compromise the stability of the DPDK application by causing crashes, memory
 * corruption, or other problematic behavior.
 *
 * Do not assume received vhost_user_msg fields contain sensible values!
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include <rte_malloc.h>
#include <rte_log.h>
#include <rte_atomic.h>

#include "vhost.h"

static rte_atomic32_t g_vhost_dev_count = RTE_ATOMIC32_INIT(0);

static const char *vhost_message_str[VHOST_USER_MAX] = {
	[VHOST_USER_NONE] = "VHOST_USER_NONE",
	[VHOST_USER_GET_FEATURES] = "VHOST_USER_GET_FEATURES",
	[VHOST_USER_SET_FEATURES] = "VHOST_USER_SET_FEATURES",
	[VHOST_USER_SET_OWNER] = "VHOST_USER_SET_OWNER",
	[VHOST_USER_RESET_OWNER] = "VHOST_USER_RESET_OWNER",
	[VHOST_USER_SET_MEM_TABLE] = "VHOST_USER_SET_MEM_TABLE",
	[VHOST_USER_SET_LOG_BASE] = "VHOST_USER_SET_LOG_BASE",
	[VHOST_USER_SET_LOG_FD] = "VHOST_USER_SET_LOG_FD",
	[VHOST_USER_SET_VRING_NUM] = "VHOST_USER_SET_VRING_NUM",
	[VHOST_USER_SET_VRING_ADDR] = "VHOST_USER_SET_VRING_ADDR",
	[VHOST_USER_SET_VRING_BASE] = "VHOST_USER_SET_VRING_BASE",
	[VHOST_USER_GET_VRING_BASE] = "VHOST_USER_GET_VRING_BASE",
	[VHOST_USER_SET_VRING_KICK] = "VHOST_USER_SET_VRING_KICK",
	[VHOST_USER_SET_VRING_CALL] = "VHOST_USER_SET_VRING_CALL",
	[VHOST_USER_SET_VRING_ERR]  = "VHOST_USER_SET_VRING_ERR",
	[VHOST_USER_GET_PROTOCOL_FEATURES] = "VHOST_USER_GET_PROTOCOL_FEATURES",
	[VHOST_USER_SET_PROTOCOL_FEATURES] = "VHOST_USER_SET_PROTOCOL_FEATURES",
	[VHOST_USER_GET_QUEUE_NUM]  = "VHOST_USER_GET_QUEUE_NUM",
	[VHOST_USER_SET_VRING_ENABLE]  = "VHOST_USER_SET_VRING_ENABLE",
	[VHOST_USER_SET_SLAVE_REQ_FD]  = "VHOST_USER_SET_SLAVE_REQ_FD",
	[VHOST_USER_IOTLB_MSG]  = "VHOST_USER_IOTLB_MSG",
};

#define SUPPORTED_PROTOCOL_FEATURES \
	((1ULL << VHOST_USER_PROTOCOL_F_MQ) | \
	 (1ULL << VHOST_USER_PROTOCOL_F_REPLY_ACK) | \
	 (1ULL << VHOST_USER_PROTOCOL_F_CONFIG))

static void handle_msg(struct vhost_dev *vdev);
static void _vhost_dev_destroy_continue(struct vhost_dev *vdev);
static void _msg_handler_stop_vq(struct vhost_dev *vdev, struct vhost_vq *vq);
static void _msg_handler_stop_all_vqs(struct vhost_dev *vdev);
static void _msg_handler_start_all_vqs(struct vhost_dev *vdev, uint16_t starting_idx);

int
vhost_dev_init(struct vhost_dev *vdev, uint64_t features,
	       const struct vhost_transport_ops *transport,
	       const struct vhost_dev_ops *dev_ops,
	       const struct rte_vhost2_tgt_ops *ops)
{
	memset(vdev, 0, sizeof(*vdev));

	vdev->dev.transport = transport;
	/* this might be later unset if the driver doesn't support it */
	vdev->dev.iommu = features & (1ULL << VIRTIO_F_IOMMU_PLATFORM);

	vdev->id = rte_atomic32_add_return(&g_vhost_dev_count, 1);

	vdev->dev_ops = dev_ops;
	vdev->ops = ops;
	vdev->supported_features = features;
	vdev->features = 0;
	vdev->ops = ops;

	return 0;
}

void
vhost_dev_set_ops_cb(struct vhost_dev *vdev,
		vhost_dev_ops_cb cb_fn, void *ctx)
{
	assert(vdev->op_cpl_fn == NULL);

	assert(vdev->op_pending_cnt < UINT_MAX);
	vdev->op_pending_cnt++;
	vdev->op_cpl_fn = cb_fn;
	vdev->op_cpl_ctx = ctx;
	vdev->op_failed_flag = false;
}

void
vhost_dev_ops_complete(struct vhost_dev *vdev, int rc)
{
	vhost_dev_ops_cb op_cpl_fn = vdev->op_cpl_fn;
	void *op_cpl_ctx = vdev->op_cpl_ctx;

	vdev->op_cpl_fn = NULL;
	vdev->op_cpl_ctx = NULL;
	vdev->op_failed_flag = !!rc;
	if (op_cpl_fn)
		op_cpl_fn(vdev, rc, op_cpl_ctx);

	assert(vdev->op_pending_cnt > 0);
	vdev->op_pending_cnt--;

	/* continue any deferred device destruction. some ops may be fired
	 * and completed from inside the op_cpl_fn, so make sure we only
	 * call this once from the top-level op. */
	if (vdev->removed && vdev->op_pending_cnt == 0)
		_msg_handler_stop_all_vqs(vdev);
}

static void
_stop_vq(struct vhost_vq *vq)
{
	vq->started = false;
}

static void
_msg_handler_stop_all_vqs_cpl(struct vhost_dev *vdev, int rc, void *ctx)
{
	struct vhost_vq *vq = ctx;

	if (rc) {
		if (!vdev->op_completed_inline)
			_msg_handler_stop_all_vqs(vdev);
		/* unwind the stack */
		return;
	}

	_stop_vq(vq);
	/* Stop the next one. */
	_msg_handler_stop_all_vqs(vdev);
}

static void
_msg_handler_stop_all_vqs(struct vhost_dev *vdev)
{
	struct vhost_vq *vq;
	uint32_t vq_idx;

	for (vq_idx = 0; vq_idx < VHOST_MAX_VIRTQUEUES; vq_idx++) {
		vq = vdev->vq[vq_idx];

		if (!vq || !vq->started)
			continue;

		if (!vdev->ops->queue_stop) {
			_stop_vq(vq);
			continue;
		}

		do {
			vdev->op_completed_inline = true;
			vhost_dev_set_ops_cb(vdev,
					_msg_handler_stop_all_vqs_cpl, vq);
			vdev->ops->queue_stop(&vdev->dev, &vq->q);
			vdev->op_completed_inline = false;
		} while (vdev->op_failed_flag);
		return;
	}

	if (vdev->removed)
		_vhost_dev_destroy_continue(vdev);
	else
		handle_msg(vdev);
}

static void
_msg_handler_stop_vq_cpl(struct vhost_dev *vdev, int rc, void *ctx)
{
	struct vhost_vq *vq = ctx;

	if (rc) {
		if (!vdev->op_completed_inline)
			_msg_handler_stop_vq(vdev, vq);
		/* unwind the stack */
		return;
	}

	_stop_vq(vq);
}

static void
_msg_handler_stop_vq(struct vhost_dev *vdev, struct vhost_vq *vq)
{
	if (!vdev->ops->queue_stop) {
		_msg_handler_stop_vq_cpl(vdev, 0, vq);
		return;
	}

	do {
		vdev->op_completed_inline = true;
		vhost_dev_set_ops_cb(vdev, _msg_handler_stop_vq_cpl, vq);
		vdev->ops->queue_stop(&vdev->dev, &vq->q);
		vdev->op_completed_inline = false;
	} while (vdev->op_failed_flag);
}

static int
_msg_handler_alloc_stop_vq(struct vhost_dev *vdev, uint16_t vq_idx)
{
	struct vhost_vq *vq;

	if (vq_idx >= VHOST_MAX_VIRTQUEUES) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"invalid vring index: %u\n", vq_idx);
		return -1;
	}

	vq = vdev->vq[vq_idx];
	if (vq) {
		if (vq->started)
			_msg_handler_stop_vq(vdev, vq);
		else
			handle_msg(vdev);
		return 0;
	}

	vq = rte_zmalloc(NULL, sizeof(struct vhost_vq), 0);
	if (vq == NULL) {
		RTE_LOG(ERR, VHOST_CONFIG,
			"Failed to allocate memory for vring:%u.\n", vq_idx);
		return -1;
	}

	vq->q.state = VHOST_VQ_DEFAULT_STATE;
	vq->idx = vq_idx;
	vq->callfd = -1;
	vq->kickfd = -1;
	vq->kicked = false;
	vq->started = false;

	vdev->vq[vq_idx] = vq;
	handle_msg(vdev);
	return 0;
}


static void
_start_all_vqs_cpl(struct vhost_dev *vdev, int rc, void *ctx)
{
	struct vhost_vq *vq = ctx;

	/* if user failed to start the queue then just continue without it */
	if (rc == 0)
		vq->started = true;

	if (vdev->removed) {
		/* device is pending destruction; don't start queues anymore */
		return;
	}

	if (vq->idx < UINT16_MAX) {
		/* Start the next queue. */
		_msg_handler_start_all_vqs(vdev, vq->idx + 1);
		return;
	}

	handle_msg(vdev);
}

static void
_msg_handler_start_all_vqs(struct vhost_dev *vdev, uint16_t starting_idx)
{
	struct vhost_vq *vq;
	uint16_t vq_idx;

	for (vq_idx = starting_idx; vq_idx < VHOST_MAX_VIRTQUEUES; vq_idx++) {
		vq = vdev->vq[vq_idx];

		if (!vq || !vq->kicked || vq->started)
			continue;

		if (!vdev->ops->queue_start) {
			_start_all_vqs_cpl(vdev, 0, vq);
			continue;
		}

		vhost_dev_set_ops_cb(vdev, _start_all_vqs_cpl, vq);
		vdev->ops->queue_start(&vdev->dev, &vq->q);
		return;
	}

	handle_msg(vdev);
}

static void
_destroy_device_cpl(struct vhost_dev *vdev, int rc, void *ctx __rte_unused)
{
	if (rc) {
		if (!vdev->op_completed_inline)
			_vhost_dev_destroy_continue(vdev);
		/* unwind the stack */
		return;
	}

	if (vdev->del_cb_fn)
		vdev->del_cb_fn(vdev->del_cb_arg);
}


static void
_vhost_dev_destroy_continue(struct vhost_dev *vdev)
{
	if (!vdev->ops->device_destroy) {
		_destroy_device_cpl(vdev, 0, NULL);
		return;
	}

	do {
		vdev->op_completed_inline = true;
		vhost_dev_set_ops_cb(vdev, _destroy_device_cpl, NULL);
		vdev->ops->device_destroy(&vdev->dev);
		vdev->op_completed_inline = false;
	} while (vdev->op_failed_flag);
}

void
vhost_dev_destroy(struct vhost_dev *vdev, void (*cb_fn)(void *arg),
		       void *cb_arg)
{
	vdev->removed = true;
	vdev->del_cb_fn = cb_fn;
	vdev->del_cb_arg = cb_arg;

	if (vdev->op_pending_cnt) {
		/* we'll resume once it's finished */
		return;
	}

	_msg_handler_stop_all_vqs(vdev);
}

static void *
uva_to_vva(struct vhost_dev *vdev, uint64_t qva, uint64_t len)
{
	struct rte_vhost2_mem_region *r;
	uint32_t i;

	/* Find the region where the address lives. */
	for (i = 0; i < vdev->dev.mem->nregions; i++) {
		r = &vdev->dev.mem->regions[i];

		if (qva >= r->guest_user_addr &&
		    qva <  r->guest_user_addr + r->size) {

			if (unlikely(len > r->guest_user_addr + r->size - qva))
				return NULL;

			return (void *)(uintptr_t)(qva - r->guest_user_addr +
			       r->host_user_addr);
		}
	}

	return NULL;
}

/*
 * Converts ring address to Vhost virtual address.
 * If IOMMU is enabled, the ring address is a guest IO virtual address,
 * else it is a QEMU virtual address.
 */
static void *
ring_addr_to_vva(struct vhost_dev *vdev, struct vhost_vq *vq,
		uint64_t ra, uint64_t size)
{
	(void) vq;
	if (vdev->dev.iommu) {
		/* FIXME */
		abort();
	}

	return uva_to_vva(vdev, ra, size);
}

static int
translate_ring_addresses(struct vhost_dev *dev, struct vhost_vq *vq,
		struct vhost_user_msg *msg)
{
	struct vhost_vring_addr addr = msg->payload.addr;
	uint64_t len;

	len = sizeof(struct vring_desc) * vq->q.size;
	vq->q.desc = ring_addr_to_vva(dev, vq, addr.desc_user_addr, len);
	if (vq->q.desc == NULL) {
		RTE_LOG(DEBUG, VHOST_CONFIG,
			"(%d) failed to map desc ring.\n",
			dev->id);
		return -1;
	}

	len = sizeof(struct vring_avail) + sizeof(uint16_t) * vq->q.size;
	vq->q.avail = ring_addr_to_vva(dev, vq, addr.avail_user_addr, len);
	if (vq->q.avail == NULL) {
		RTE_LOG(DEBUG, VHOST_CONFIG,
			"(%d) failed to map avail ring.\n",
			dev->id);
		return -1;
	}

	len = sizeof(struct vring_used) +
		sizeof(struct vring_used_elem) * vq->q.size;
	vq->q.used = ring_addr_to_vva(dev, vq, addr.used_user_addr, len);
	if (vq->q.used == NULL) {
		RTE_LOG(DEBUG, VHOST_CONFIG,
			"(%d) failed to map used ring.\n",
			dev->id);
		return -1;
	}

	if (vq->q.last_used_idx != vq->q.used->idx) {
		RTE_LOG(WARNING, VHOST_CONFIG,
			"last_used_idx (%u) and vq->used->idx (%u) mismatches; "
			"some packets maybe resent for Tx and dropped for Rx\n",
			vq->q.last_used_idx, vq->q.used->idx);
		vq->q.last_used_idx  = vq->q.used->idx;
		vq->q.last_avail_idx = vq->q.used->idx;
	}

	vq->q.log_guest_addr = addr.log_guest_addr;

	RTE_LOG(DEBUG, VHOST_CONFIG, "(%d) mapped address desc: %p\n",
			dev->id, vq->q.desc);
	RTE_LOG(DEBUG, VHOST_CONFIG, "(%d) mapped address avail: %p\n",
			dev->id, vq->q.avail);
	RTE_LOG(DEBUG, VHOST_CONFIG, "(%d) mapped address used: %p\n",
			dev->id, vq->q.used);
	RTE_LOG(DEBUG, VHOST_CONFIG, "(%d) log_guest_addr: %lu\n",
			dev->id, vq->q.log_guest_addr);

	return 0;
}

static int
send_vhost_reply(struct vhost_dev *vdev, struct vhost_user_msg *msg)
{
	msg->flags &= ~VHOST_USER_VERSION_MASK;
	msg->flags &= ~VHOST_USER_NEED_REPLY;
	msg->flags |= VHOST_USER_VERSION;
	msg->flags |= VHOST_USER_REPLY_MASK;

	return vdev->dev_ops->send_reply(vdev, msg);
}

static int
_parse_msg(struct vhost_dev *vdev)
{
	struct vhost_user_msg *msg = vdev->msg;
	struct vhost_vq *vq;
	struct vhost_vring_file file;

	switch (msg->type) {
	case VHOST_USER_GET_FEATURES:
		msg->payload.u64 = vdev->supported_features;
		msg->size = sizeof(msg->payload.u64);
		return send_vhost_reply(vdev, msg);

	case VHOST_USER_SET_FEATURES:
		if (msg->payload.u64 & ~vdev->supported_features) {
			RTE_LOG(ERR, VHOST_CONFIG,
				"received invalid negotiated features.\n");
			return -1;
		}

		vdev->features = msg->payload.u64;

		if (vdev->ops->device_features_changed) {
			vhost_dev_set_ops_cb(vdev, NULL, NULL);
			vdev->ops->device_features_changed(&vdev->dev,
				vdev->features);
		}
		return 0;

	case VHOST_USER_SET_OWNER:
		/* FIXME */
		return 0;

	case VHOST_USER_RESET_OWNER:
		/* FIXME */
		return 0;

	case VHOST_USER_SET_MEM_TABLE:
		/* mem table is managed by the transport layer */
		return 0;

	case VHOST_USER_SET_VRING_NUM:
		vq = vdev->vq[msg->payload.state.index];
		vq->q.size = msg->payload.state.num;

		/* VIRTIO 1.0, 2.4 Virtqueues says:
		 *
		 *   Queue Size value is always a power of 2.
		 *   The maximum Queue Size value is 32768.
		 */
		if ((vq->q.size & (vq->q.size - 1)) || vq->q.size > 32768) {
			RTE_LOG(ERR, VHOST_CONFIG,
				"invalid virtqueue size %u\n", vq->q.size);
			return -1;
		}
		return 0;

	case VHOST_USER_SET_VRING_ADDR:
		if (vdev->dev.mem == NULL)
			return -1;

		vq = vdev->vq[msg->payload.addr.index];
		translate_ring_addresses(vdev, vq, msg);
		return 0;

	case VHOST_USER_SET_VRING_BASE:
		vq = vdev->vq[msg->payload.state.index];
		vq->q.last_used_idx = msg->payload.state.num;
		vq->q.last_avail_idx = msg->payload.state.num;
		return 0;

	case VHOST_USER_GET_VRING_BASE:
		vq = vdev->vq[msg->payload.state.index];
		msg->payload.state.num = vq->q.last_avail_idx;

		RTE_LOG(INFO, VHOST_CONFIG,
			"vring base idx:%d file:%d\n",
			msg->payload.state.index, msg->payload.state.num);

		if (vq->kickfd >= 0)
			close(vq->kickfd);

		vq->kickfd = -1;
		vq->kicked = false;

		msg->size = sizeof(msg->payload.state);
		return send_vhost_reply(vdev, msg);

	case VHOST_USER_SET_VRING_KICK:
		file.index = msg->payload.u64 & VHOST_USER_VRING_IDX_MASK;
		if (msg->payload.u64 & VHOST_USER_VRING_NOFD_MASK)
			file.fd = -1;
		else
			file.fd = msg->fds[0];

		RTE_LOG(INFO, VHOST_CONFIG,
			"vring kick idx:%d file:%d\n", file.index, file.fd);

		vq = vdev->vq[file.index];
		if (vq->kickfd >= 0)
			close(vq->kickfd);

		vq->kickfd = file.fd;
		vq->kicked = true;

		if ((vdev->features &
				(1ULL << VHOST_USER_F_PROTOCOL_FEATURES)) == 0)
			vq->q.state = VHOST_VQ_ENABLED;

		return 0;

	case VHOST_USER_SET_VRING_CALL:
		file.index = msg->payload.u64 & VHOST_USER_VRING_IDX_MASK;
		if (msg->payload.u64 & VHOST_USER_VRING_NOFD_MASK)
			file.fd = -1;
		else
			file.fd = msg->fds[0];

		RTE_LOG(INFO, VHOST_CONFIG,
			"vring call idx:%d file:%d\n", file.index, file.fd);

		vq = vdev->vq[file.index];
		if (vq->callfd >= 0)
			close(vq->callfd);

		vq->callfd = file.fd;
		return 0;

	case VHOST_USER_SET_VRING_ERR:
		if (!(msg->payload.u64 & VHOST_USER_VRING_NOFD_MASK))
			close(msg->fds[0]);
		RTE_LOG(INFO, VHOST_CONFIG, "not implemented\n");
		return 0;

	case VHOST_USER_GET_PROTOCOL_FEATURES:
		if ((vdev->features &
				(1ULL << VHOST_USER_F_PROTOCOL_FEATURES)) == 0)
			return -1;

		msg->payload.u64 = SUPPORTED_PROTOCOL_FEATURES;

		/*
		 * REPLY_ACK protocol feature is only mandatory for now
		 * for IOMMU feature. If IOMMU is explicitly disabled by the
		 * application, disable also REPLY_ACK feature for older buggy
		 * Qemu versions (from v2.7.0 to v2.9.0).
		 */
		if (!vdev->dev.iommu)
			msg->payload.u64 &=
				~(1ULL << VHOST_USER_PROTOCOL_F_REPLY_ACK);

		if (!vdev->ops->get_config || !vdev->ops->set_config)
			msg->payload.u64 &=
				~(1ULL << VHOST_USER_PROTOCOL_F_CONFIG);

		return send_vhost_reply(vdev, msg);

	case VHOST_USER_SET_PROTOCOL_FEATURES:
		if ((vdev->features &
				(1ULL << VHOST_USER_F_PROTOCOL_FEATURES)) == 0)
			return -1;

		vdev->protocol_features = msg->payload.u64;
		if (msg->payload.u64 & ~SUPPORTED_PROTOCOL_FEATURES) {
			RTE_LOG(ERR, VHOST_CONFIG,
				"received invalid negotiated protocol features.\n");
			return -1;
		}
		return 0;

	case VHOST_USER_GET_QUEUE_NUM:
		msg->payload.u64 = (uint64_t)VHOST_MAX_VIRTQUEUES;
		msg->size = sizeof(msg->payload.u64);
		return send_vhost_reply(vdev, msg);

	case VHOST_USER_SET_VRING_ENABLE:
		if ((vdev->features &
				(1ULL << VHOST_USER_F_PROTOCOL_FEATURES)) == 0)
			return -1;

		vq = vdev->vq[msg->payload.state.index];
		if (msg->payload.state.num & 1)
			vq->q.state = VHOST_VQ_ENABLED;
		else
			vq->q.state = VHOST_VQ_DISABLED;

		return 0;

	case VHOST_USER_GET_CONFIG:
		if ((vdev->protocol_features &
				(1ULL << VHOST_USER_PROTOCOL_F_CONFIG)) == 0)
				return -1;

		if (msg->payload.config.size > VHOST_USER_MAX_CONFIG_SIZE)
			return -1;

		if (vdev->ops->get_config(&vdev->dev,
				msg->payload.config.region,
				msg->payload.config.size)) {
			/* zero length payload indicates an error */
			msg->size = 0;
		}

		return send_vhost_reply(vdev, msg);

	case VHOST_USER_SET_CONFIG:
		if ((vdev->protocol_features &
				(1ULL << VHOST_USER_PROTOCOL_F_CONFIG)) == 0)
				return -1;

		if (msg->payload.config.size > VHOST_USER_MAX_CONFIG_SIZE)
			return -1;

		return vdev->ops->set_config(&vdev->dev,
				msg->payload.config.region,
				msg->payload.config.offset,
				msg->payload.config.size,
				msg->payload.config.flags);

	case VHOST_USER_SET_SLAVE_REQ_FD:
		/* managed by the transport layer */
		return 0;

	default:
		/* FIXME */
		abort();
	}

	return -1;
}

static void
handle_msg(struct vhost_dev *vdev)
{
	struct vhost_user_msg *msg = vdev->msg;
	int ret = 0;

	if (vdev->msg_state < VHOST_DEV_MSG_COMPLETE)
		vdev->msg_state += 1;

	switch (vdev->msg_state) {
	case VHOST_DEV_MSG_PREPARE:
		switch (msg->type) {
		case VHOST_USER_SET_VRING_KICK:
		case VHOST_USER_SET_VRING_CALL:
		case VHOST_USER_SET_VRING_ERR:
			ret = _msg_handler_alloc_stop_vq(vdev,
					msg->payload.u64 &
						VHOST_USER_VRING_IDX_MASK);
			break;
		case VHOST_USER_SET_VRING_NUM:
		case VHOST_USER_SET_VRING_BASE:
		case VHOST_USER_SET_VRING_ENABLE:
			ret = _msg_handler_alloc_stop_vq(vdev,
					msg->payload.state.index);
			break;
		case VHOST_USER_SET_VRING_ADDR:
			ret = _msg_handler_alloc_stop_vq(vdev,
					msg->payload.addr.index);
			break;
		case VHOST_USER_SET_MEM_TABLE:
		case VHOST_USER_SET_PROTOCOL_FEATURES:
			_msg_handler_stop_all_vqs(vdev);
			break;

		default:
			break;
		}

		if (ret)
			break;

		return;

	case VHOST_DEV_MSG_PARSE:
		if (vdev->dev_ops->handle_msg) {
			ret = vdev->dev_ops->handle_msg(vdev, msg);
			if (ret < 0) {
				break;
			}
		}

		ret = _parse_msg(vdev);
		if (ret < 0)
			break;

		if (ret == 0 || vdev->op_pending_cnt)
			return;

		/* break through */

	case VHOST_DEV_MSG_POSTPROCESS:
		/* messages that already sent a reply won't have the
		 * VHOST_USER_NEED_REPLY bit set.
		 */
		if ((vdev->protocol_features &
				(1ULL << VHOST_USER_PROTOCOL_F_REPLY_ACK)) &&
		    (msg->flags & VHOST_USER_NEED_REPLY)) {
			msg->payload.u64 = !!ret;
			msg->size = sizeof(msg->payload.u64);
			ret = send_vhost_reply(vdev, msg);
			if (ret) {
				RTE_LOG(ERR, VHOST_CONFIG,
					"failed to send reply\n");
				break;
			}
		}

		_msg_handler_start_all_vqs(vdev, 0);
		return;

	case VHOST_DEV_MSG_COMPLETE:
		break;

	default:
		abort();
		break;
	}

	if (ret)
		vdev->msg_state = VHOST_DEV_MSG_COMPLETE;

	if (vdev->msg_state == VHOST_DEV_MSG_COMPLETE &&
			vdev->dev_ops->msg_cpl &&
			vdev->op_pending_cnt == 0)
		vdev->dev_ops->msg_cpl(vdev, ret, msg);
}

int
vhost_dev_msg_handler(struct vhost_dev *vdev, struct vhost_user_msg *msg)
{
	if (msg->type >= VHOST_USER_MAX)
		return -1;

	if (vdev->removed)
		return -1;

	RTE_LOG(ERR, VHOST_CONFIG, "read message (%d) %s\n",
		msg->type, vhost_message_str[msg->type]);

	vdev->msg = msg;
	vdev->msg_state = VHOST_DEV_MSG_RECEIVE;
	handle_msg(vdev);
	return 0;
}

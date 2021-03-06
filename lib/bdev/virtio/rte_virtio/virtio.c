/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2016 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include <linux/virtio_scsi.h>
#include <linux/virtio_pci.h>
#include <linux/virtio_config.h>

#include <rte_config.h>
#include <rte_memcpy.h>
#include <rte_string_fns.h>
#include <rte_memzone.h>
#include <rte_malloc.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_pci.h>
#include <rte_common.h>
#include <rte_errno.h>

#include <rte_memory.h>
#include <rte_eal.h>
#include <rte_dev.h>
#include <rte_prefetch.h>

#include "spdk/env.h"

#include "virtio.h"

struct virtio_driver g_virtio_driver = {
	.init_ctrlrs = TAILQ_HEAD_INITIALIZER(g_virtio_driver.init_ctrlrs),
	.attached_ctrlrs = TAILQ_HEAD_INITIALIZER(g_virtio_driver.attached_ctrlrs),
	.ctrlr_counter = 0,
};

static void
virtio_init_vring(struct virtqueue *vq)
{
	int size = vq->vq_nentries;
	struct vring *vr = &vq->vq_ring;
	uint8_t *ring_mem = vq->vq_ring_virt_mem;

	/*
	 * Reinitialise since virtio port might have been stopped and restarted
	 */
	memset(ring_mem, 0, vq->vq_ring_size);
	vring_init(vr, size, ring_mem, VIRTIO_PCI_VRING_ALIGN);
	vq->vq_used_cons_idx = 0;
	vq->vq_desc_head_idx = 0;
	vq->vq_avail_idx = 0;
	vq->vq_desc_tail_idx = (uint16_t)(vq->vq_nentries - 1);
	vq->vq_free_cnt = vq->vq_nentries;
	memset(vq->vq_descx, 0, sizeof(struct vq_desc_extra) * vq->vq_nentries);

	vring_desc_init(vr->desc, size);

	/*
	 * Disable device(host) interrupting guest
	 */
	virtqueue_disable_intr(vq);
}

static int
virtio_init_queue(struct virtio_dev *dev, uint16_t vtpci_queue_idx)
{
	char vq_name[VIRTQUEUE_MAX_NAME_SZ];
	const struct rte_memzone *mz = NULL;
	unsigned int vq_size, size;
	struct virtqueue *vq;
	int ret;

	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_DEV, "setting up queue: %"PRIu16"\n", vtpci_queue_idx);

	/*
	 * Read the virtqueue size from the Queue Size field
	 * Always power of 2 and if 0 virtqueue does not exist
	 */
	vq_size = virtio_dev_backend_ops(dev)->get_queue_num(dev, vtpci_queue_idx);
	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_DEV, "vq_size: %u\n", vq_size);
	if (vq_size == 0) {
		SPDK_ERRLOG("virtqueue %"PRIu16" does not exist\n", vtpci_queue_idx);
		return -EINVAL;
	}

	if (!rte_is_power_of_2(vq_size)) {
		SPDK_ERRLOG("virtqueue %"PRIu16" size (%u) is not powerof 2\n",
			    vtpci_queue_idx, vq_size);
		return -EINVAL;
	}

	snprintf(vq_name, sizeof(vq_name), "dev%d_vq%d",
		 dev->id, vtpci_queue_idx);

	size = RTE_ALIGN_CEIL(sizeof(*vq) +
			      vq_size * sizeof(struct vq_desc_extra),
			      RTE_CACHE_LINE_SIZE);

	vq = rte_zmalloc_socket(vq_name, size, RTE_CACHE_LINE_SIZE,
				SOCKET_ID_ANY);
	if (vq == NULL) {
		SPDK_ERRLOG("can not allocate vq\n");
		return -ENOMEM;
	}
	dev->vqs[vtpci_queue_idx] = vq;

	vq->vdev = dev;
	vq->vq_queue_index = vtpci_queue_idx;
	vq->vq_nentries = vq_size;

	/*
	 * Reserve a memzone for vring elements
	 */
	size = vring_size(vq_size, VIRTIO_PCI_VRING_ALIGN);
	vq->vq_ring_size = RTE_ALIGN_CEIL(size, VIRTIO_PCI_VRING_ALIGN);
	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_DEV, "vring_size: %u, rounded_vring_size: %u\n",
		      size, vq->vq_ring_size);

	mz = rte_memzone_reserve_aligned(vq_name, vq->vq_ring_size,
					 SOCKET_ID_ANY,
					 0, VIRTIO_PCI_VRING_ALIGN);
	if (mz == NULL) {
		if (rte_errno == EEXIST)
			mz = rte_memzone_lookup(vq_name);
		if (mz == NULL) {
			ret = -ENOMEM;
			goto fail_q_alloc;
		}
	}

	memset(mz->addr, 0, sizeof(mz->len));

	vq->vq_ring_mem = mz->phys_addr;
	vq->vq_ring_virt_mem = mz->addr;
	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_DEV, "vq->vq_ring_mem:      0x%" PRIx64 "\n",
		      (uint64_t)mz->phys_addr);
	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_DEV, "vq->vq_ring_virt_mem: 0x%" PRIx64 "\n",
		      (uint64_t)(uintptr_t)mz->addr);

	virtio_init_vring(vq);

	vq->mz = mz;

	vq->owner_thread = NULL;
	vq->poller = NULL;

	if (virtio_dev_backend_ops(dev)->setup_queue(dev, vq) < 0) {
		SPDK_ERRLOG("setup_queue failed\n");
		return -EINVAL;
	}

	return 0;

fail_q_alloc:
	rte_memzone_free(mz);
	rte_free(vq);

	return ret;
}

static void
virtio_free_queues(struct virtio_dev *dev)
{
	uint16_t nr_vq = dev->max_queues;
	struct virtqueue *vq;
	uint16_t i;

	if (dev->vqs == NULL)
		return;

	for (i = 0; i < nr_vq; i++) {
		vq = dev->vqs[i];
		if (!vq)
			continue;

		rte_memzone_free(vq->mz);

		rte_free(vq);
		dev->vqs[i] = NULL;
	}

	rte_free(dev->vqs);
	dev->vqs = NULL;
}

static int
virtio_alloc_queues(struct virtio_dev *dev)
{
	uint16_t nr_vq = dev->max_queues;
	uint16_t i;
	int ret;

	dev->vqs = rte_zmalloc(NULL, sizeof(struct virtqueue *) * nr_vq, 0);
	if (!dev->vqs) {
		SPDK_ERRLOG("failed to allocate %"PRIu16" vqs\n", nr_vq);
		return -ENOMEM;
	}

	for (i = 0; i < nr_vq; i++) {
		ret = virtio_init_queue(dev, i);
		if (ret < 0) {
			virtio_free_queues(dev);
			return ret;
		}
	}

	return 0;
}

/**
 * Negotiate virtio features. For virtio_user this will also set
 * dev->modern flag if VIRTIO_F_VERSION_1 flag is negotiated.
 */
static int
virtio_negotiate_features(struct virtio_dev *dev, uint64_t req_features)
{
	uint64_t host_features = virtio_dev_backend_ops(dev)->get_features(dev);
	int rc;

	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_DEV, "guest features = %" PRIx64 "\n", req_features);
	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_DEV, "device features = %" PRIx64 "\n", host_features);

	rc = virtio_dev_backend_ops(dev)->set_features(dev, req_features & host_features);
	if (rc != 0) {
		SPDK_ERRLOG("failed to negotiate device features.\n");
		return -1;
	}

	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_DEV, "negotiated features = %" PRIx64 "\n",
		      dev->negotiated_features);

	virtio_dev_set_status(dev, VIRTIO_CONFIG_S_FEATURES_OK);
	if (!(virtio_dev_get_status(dev) & VIRTIO_CONFIG_S_FEATURES_OK)) {
		SPDK_ERRLOG("failed to set FEATURES_OK status!\n");
		return -1;
	}

	return 0;
}

struct virtio_dev *
	virtio_dev_construct(const struct virtio_dev_ops *ops, void *ctx)
{
	struct virtio_dev *vdev;
	unsigned vdev_num;

	vdev = calloc(1, sizeof(*vdev));
	if (vdev == NULL) {
		SPDK_ERRLOG("virtio device calloc failed\n");
		return NULL;
	}

	vdev_num = __sync_add_and_fetch(&g_virtio_driver.ctrlr_counter, 1);
	vdev->id = vdev_num;
	pthread_mutex_init(&vdev->mutex, NULL);
	vdev->backend_ops = ops;
	vdev->ctx = ctx;

	return vdev;
}

/* reset device and renegotiate features if needed */
int
virtio_dev_init(struct virtio_dev *dev, uint64_t req_features)
{
	int ret;

	/* Reset the device although not necessary at startup */
	virtio_dev_reset(dev);

	/* Tell the host we've noticed this device. */
	virtio_dev_set_status(dev, VIRTIO_CONFIG_S_ACKNOWLEDGE);

	/* Tell the host we've known how to drive the device. */
	virtio_dev_set_status(dev, VIRTIO_CONFIG_S_DRIVER);
	if (virtio_negotiate_features(dev, req_features) < 0)
		return -1;

	ret = virtio_alloc_queues(dev);
	if (ret < 0)
		return ret;

	virtio_dev_set_status(dev, VIRTIO_CONFIG_S_DRIVER_OK);
	return 0;
}

void
virtio_dev_free(struct virtio_dev *dev)
{
	virtio_free_queues(dev);
	virtio_dev_backend_ops(dev)->free_vdev(dev);
	pthread_mutex_destroy(&dev->mutex);
	free(dev);
}

int
virtio_dev_start(struct virtio_dev *vdev)
{
	/* Enable uio/vfio intr/eventfd mapping: althrough we already did that
	 * in device configure, but it could be unmapped  when device is
	 * stopped.
	 */
	/** TODO: interrupt handling for virtio_scsi */
#if 0
	if (dev->data->dev_conf.intr_conf.lsc ||
	    dev->data->dev_conf.intr_conf.rxq) {
		rte_intr_disable(dev->intr_handle);

		if (rte_intr_enable(dev->intr_handle) < 0) {
			PMD_DRV_LOG(ERR, "interrupt enable failed");
			return -EIO;
		}
	}
#endif

	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_DEV, "Notified backend at initialization\n");

	vdev->started = 1;

	return 0;
}

static void
vq_ring_free_chain(struct virtqueue *vq, uint16_t desc_idx)
{
	struct vring_desc *dp, *dp_tail;
	struct vq_desc_extra *dxp;
	uint16_t desc_idx_last = desc_idx;

	dp  = &vq->vq_ring.desc[desc_idx];
	dxp = &vq->vq_descx[desc_idx];
	vq->vq_free_cnt = (uint16_t)(vq->vq_free_cnt + dxp->ndescs);
	if ((dp->flags & VRING_DESC_F_INDIRECT) == 0) {
		while (dp->flags & VRING_DESC_F_NEXT) {
			desc_idx_last = dp->next;
			dp = &vq->vq_ring.desc[dp->next];
		}
	}
	dxp->ndescs = 0;

	/*
	 * We must append the existing free chain, if any, to the end of
	 * newly freed chain. If the virtqueue was completely used, then
	 * head would be VQ_RING_DESC_CHAIN_END (ASSERTed above).
	 */
	if (vq->vq_desc_tail_idx == VQ_RING_DESC_CHAIN_END) {
		vq->vq_desc_head_idx = desc_idx;
	} else {
		dp_tail = &vq->vq_ring.desc[vq->vq_desc_tail_idx];
		dp_tail->next = desc_idx;
	}

	vq->vq_desc_tail_idx = desc_idx_last;
	dp->next = VQ_RING_DESC_CHAIN_END;
}

static uint16_t
virtqueue_dequeue_burst_rx(struct virtqueue *vq, struct virtio_req **rx_pkts,
			   uint32_t *len, uint16_t num)
{
	struct vring_used_elem *uep;
	struct virtio_req *cookie;
	uint16_t used_idx, desc_idx;
	uint16_t i;

	/*  Caller does the check */
	for (i = 0; i < num ; i++) {
		used_idx = (uint16_t)(vq->vq_used_cons_idx & (vq->vq_nentries - 1));
		uep = &vq->vq_ring.used->ring[used_idx];
		desc_idx = (uint16_t) uep->id;
		len[i] = uep->len;
		cookie = (struct virtio_req *)vq->vq_descx[desc_idx].cookie;

		if (spdk_unlikely(cookie == NULL)) {
			SPDK_WARNLOG("vring descriptor with no mbuf cookie at %"PRIu16"\n",
				     vq->vq_used_cons_idx);
			break;
		}

		rte_prefetch0(cookie);
		rx_pkts[i]  = cookie;
		vq->vq_used_cons_idx++;
		vq_ring_free_chain(vq, desc_idx);
		vq->vq_descx[desc_idx].cookie = NULL;
	}

	return i;
}

static inline void
virtqueue_iov_to_desc(struct virtqueue *vq, uint16_t desc_idx, struct iovec *iov)
{
	if (!vq->vdev->is_hw) {
		vq->vq_ring.desc[desc_idx].addr  = (uintptr_t)iov->iov_base;
	} else {
		vq->vq_ring.desc[desc_idx].addr = spdk_vtophys(iov->iov_base);
	}

	vq->vq_ring.desc[desc_idx].len = iov->iov_len;
}

static int
virtqueue_enqueue_xmit(struct virtqueue *vq, struct virtio_req *req)
{
	struct vq_desc_extra *dxp;
	struct vring_desc *descs;
	uint32_t i;
	uint16_t head_idx, idx;
	uint32_t total_iovs = req->iovcnt + 2;
	struct iovec *iov = req->iov;

	if (total_iovs > vq->vq_free_cnt) {
		SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_DEV,
			      "not enough free descriptors. requested %"PRIu32", got %"PRIu16"\n",
			      total_iovs, vq->vq_free_cnt);
		return -ENOMEM;
	}

	head_idx = vq->vq_desc_head_idx;
	idx = head_idx;
	dxp = &vq->vq_descx[idx];
	dxp->cookie = (void *)req;
	dxp->ndescs = total_iovs;

	descs = vq->vq_ring.desc;

	virtqueue_iov_to_desc(vq, idx, &req->iov_req);
	descs[idx].flags = VRING_DESC_F_NEXT;
	idx = descs[idx].next;

	if (req->is_write || req->iovcnt == 0) {
		for (i = 0; i < req->iovcnt; i++) {
			virtqueue_iov_to_desc(vq, idx, &iov[i]);
			descs[idx].flags = VRING_DESC_F_NEXT;
			idx = descs[idx].next;
		}

		virtqueue_iov_to_desc(vq, idx, &req->iov_resp);
		descs[idx].flags = VRING_DESC_F_WRITE;
		idx = descs[idx].next;
	} else {
		virtqueue_iov_to_desc(vq, idx, &req->iov_resp);
		descs[idx].flags = VRING_DESC_F_WRITE | VRING_DESC_F_NEXT;
		idx = descs[idx].next;

		for (i = 0; i < req->iovcnt; i++) {
			virtqueue_iov_to_desc(vq, idx, &iov[i]);
			descs[idx].flags = VRING_DESC_F_WRITE;
			descs[idx].flags |= (i + 1) != req->iovcnt ? VRING_DESC_F_NEXT : 0;
			idx = descs[idx].next;
		}
	}

	vq->vq_desc_head_idx = idx;
	if (vq->vq_desc_head_idx == VQ_RING_DESC_CHAIN_END) {
		assert(vq->vq_free_cnt == 0);
		vq->vq_desc_tail_idx = VQ_RING_DESC_CHAIN_END;
	}
	vq->vq_free_cnt = (uint16_t)(vq->vq_free_cnt - total_iovs);
	vq_update_avail_ring(vq, head_idx);
	return 0;
}

#define VIRTIO_MBUF_BURST_SZ 64
#define DESC_PER_CACHELINE (RTE_CACHE_LINE_SIZE / sizeof(struct vring_desc))
uint16_t
virtio_recv_pkts(struct virtqueue *vq, struct virtio_req **reqs, uint16_t nb_pkts)
{
	struct virtio_dev *vdev = vq->vdev;
	struct virtio_req *rxm;
	uint16_t nb_used, num, nb_rx;
	uint32_t len[VIRTIO_MBUF_BURST_SZ];
	struct virtio_req *rcv_pkts[VIRTIO_MBUF_BURST_SZ];
	uint32_t i;

	nb_rx = 0;
	if (spdk_unlikely(vdev->started == 0))
		return nb_rx;

	nb_used = VIRTQUEUE_NUSED(vq);

	virtio_rmb();

	num = (uint16_t)(spdk_likely(nb_used <= nb_pkts) ? nb_used : nb_pkts);
	num = (uint16_t)(spdk_likely(num <= VIRTIO_MBUF_BURST_SZ) ? num : VIRTIO_MBUF_BURST_SZ);
	if (spdk_likely(num > DESC_PER_CACHELINE))
		num = num - ((vq->vq_used_cons_idx + num) % DESC_PER_CACHELINE);

	num = virtqueue_dequeue_burst_rx(vq, rcv_pkts, len, num);
	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_DEV, "used:%"PRIu16" dequeue:%"PRIu16"\n", nb_used, num);

	for (i = 0; i < num ; i++) {
		rxm = rcv_pkts[i];

		SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_DEV, "packet len:%"PRIu32"\n", len[i]);

		rxm->data_transferred = (uint16_t)(len[i]);

		reqs[nb_rx++] = rxm;
	}

	return nb_rx;
}

int
virtio_xmit_pkt(struct virtqueue *vq, struct virtio_req *req)
{
	struct virtio_dev *vdev = vq->vdev;
	int rc;

	if (spdk_unlikely(vdev->started == 0))
		return -EIO;

	virtio_rmb();

	rc = virtqueue_enqueue_xmit(vq, req);
	if (spdk_unlikely(rc != 0)) {
		return rc;
	}

	vq_update_avail_idx(vq);

	if (spdk_unlikely(virtqueue_kick_prepare(vq))) {
		virtio_dev_backend_ops(vdev)->notify_queue(vdev, vq);
		SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_DEV, "Notified backend after xmit\n");
	}

	return 0;
}

int
virtio_dev_acquire_queue(struct virtio_dev *vdev, uint16_t index)
{
	struct virtqueue *vq = NULL;

	if (index >= vdev->max_queues) {
		SPDK_ERRLOG("requested vq index %"PRIu16" exceeds max queue count %"PRIu16".\n",
			    index, vdev->max_queues);
		return -1;
	}

	pthread_mutex_lock(&vdev->mutex);
	vq = vdev->vqs[index];
	if (vq == NULL || vq->owner_thread != NULL) {
		pthread_mutex_unlock(&vdev->mutex);
		return -1;
	}

	assert(vq->poller == NULL);
	vq->owner_thread = spdk_get_thread();
	pthread_mutex_unlock(&vdev->mutex);
	return 0;
}

int32_t
virtio_dev_find_and_acquire_queue(struct virtio_dev *vdev, uint16_t start_index)
{
	struct virtqueue *vq = NULL;
	uint16_t i;

	pthread_mutex_lock(&vdev->mutex);
	for (i = start_index; i < vdev->max_queues; ++i) {
		vq = vdev->vqs[i];
		if (vq != NULL && vq->owner_thread == NULL) {
			break;
		}
	}

	if (vq == NULL || i == vdev->max_queues) {
		SPDK_ERRLOG("no more unused virtio queues with idx >= %"PRIu16".\n", start_index);
		pthread_mutex_unlock(&vdev->mutex);
		return -1;
	}

	assert(vq->poller == NULL);
	vq->owner_thread = spdk_get_thread();
	pthread_mutex_unlock(&vdev->mutex);
	return i;
}

struct spdk_thread *
virtio_dev_queue_get_thread(struct virtio_dev *vdev, uint16_t index)
{
	struct virtqueue *vq;
	struct spdk_thread *thread = NULL;

	if (index >= vdev->max_queues) {
		SPDK_ERRLOG("given vq index %"PRIu16" exceeds max queue count %"PRIu16"\n",
			    index, vdev->max_queues);
		return NULL;
	}

	pthread_mutex_lock(&vdev->mutex);
	vq = vdev->vqs[index];
	if (vq != NULL) {
		thread = vq->owner_thread;
	}
	pthread_mutex_unlock(&vdev->mutex);

	return thread;
}

bool
virtio_dev_queue_is_acquired(struct virtio_dev *vdev, uint16_t index)
{
	return virtio_dev_queue_get_thread(vdev, index) != NULL;
}

void
virtio_dev_release_queue(struct virtio_dev *vdev, uint16_t index)
{
	struct virtqueue *vq = NULL;

	if (index >= vdev->max_queues) {
		SPDK_ERRLOG("given vq index %"PRIu16" exceeds max queue count %"PRIu16".\n",
			    index, vdev->max_queues);
		return;
	}

	pthread_mutex_lock(&vdev->mutex);
	vq = vdev->vqs[index];
	if (vq == NULL) {
		SPDK_ERRLOG("virtqueue at index %"PRIu16" is not initialized.\n", index);
		pthread_mutex_unlock(&vdev->mutex);
		return;
	}

	assert(vq->poller == NULL);
	assert(vq->owner_thread == spdk_get_thread());
	vq->owner_thread = NULL;
	pthread_mutex_unlock(&vdev->mutex);
}

void
virtio_dev_read_dev_config(struct virtio_dev *dev, size_t offset,
			   void *dst, int length)
{
	virtio_dev_backend_ops(dev)->read_dev_cfg(dev, offset, dst, length);
}

void
virtio_dev_write_dev_config(struct virtio_dev *dev, size_t offset,
			    const void *src, int length)
{
	virtio_dev_backend_ops(dev)->write_dev_cfg(dev, offset, src, length);
}

void
virtio_dev_reset(struct virtio_dev *dev)
{
	virtio_dev_backend_ops(dev)->set_status(dev, VIRTIO_CONFIG_S_RESET);
	/* flush status write */
	virtio_dev_backend_ops(dev)->get_status(dev);
}

void
virtio_dev_set_status(struct virtio_dev *dev, uint8_t status)
{
	if (status != VIRTIO_CONFIG_S_RESET)
		status |= virtio_dev_backend_ops(dev)->get_status(dev);

	virtio_dev_backend_ops(dev)->set_status(dev, status);
}

uint8_t
virtio_dev_get_status(struct virtio_dev *dev)
{
	return virtio_dev_backend_ops(dev)->get_status(dev);
}

const struct virtio_dev_ops *
virtio_dev_backend_ops(struct virtio_dev *dev)
{
	return dev->backend_ops;
}

void
virtio_dev_dump_json_config(struct virtio_dev *hw, struct spdk_json_write_ctx *w)
{
	spdk_json_write_name(w, "virtio");
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "vq_count");
	spdk_json_write_uint32(w, hw->max_queues);

	spdk_json_write_name(w, "vq_size");
	spdk_json_write_uint32(w, virtio_dev_backend_ops(hw)->get_queue_num(hw, 0));

	virtio_dev_backend_ops(hw)->dump_json_config(hw, w);

	spdk_json_write_object_end(w);
}

SPDK_LOG_REGISTER_TRACE_FLAG("virtio_dev", SPDK_TRACE_VIRTIO_DEV)

#include "kvm/virtio-blk.h"

#include "kvm/virtio-pci-dev.h"
#include "kvm/disk-image.h"
#include "kvm/mutex.h"
#include "kvm/util.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"
#include "kvm/threadpool.h"
#include "kvm/ioeventfd.h"
#include "kvm/guest_compat.h"
#include "kvm/virtio-pci.h"
#include "kvm/virtio.h"
#include "kvm/virtio-trans.h"

#include <linux/virtio_ring.h>
#include <linux/virtio_blk.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/types.h>
#include <pthread.h>

#define VIRTIO_BLK_MAX_DEV		4

/*
 * the header and status consume too entries
 */
#define DISK_SEG_MAX			(VIRTIO_BLK_QUEUE_SIZE - 2)
#define VIRTIO_BLK_QUEUE_SIZE		128
#define NUM_VIRT_QUEUES			1

struct blk_dev_req {
	struct virt_queue		*vq;
	struct blk_dev			*bdev;
	struct iovec			iov[VIRTIO_BLK_QUEUE_SIZE];
	u16				out, in, head;
	struct kvm			*kvm;
};

struct blk_dev {
	pthread_mutex_t			mutex;
	pthread_mutex_t			req_mutex;

	struct list_head		list;
	struct list_head		req_list;

	struct virtio_trans		vtrans;
	struct virtio_blk_config	blk_config;
	struct disk_image		*disk;
	u32				features;

	struct virt_queue		vqs[NUM_VIRT_QUEUES];
	struct blk_dev_req		reqs[VIRTIO_BLK_QUEUE_SIZE];
};

static LIST_HEAD(bdevs);
static int compat_id = -1;

void virtio_blk_complete(void *param, long len)
{
	struct blk_dev_req *req = param;
	struct blk_dev *bdev = req->bdev;
	int queueid = req->vq - bdev->vqs;
	u8 *status;

	/* status */
	status	= req->iov[req->out + req->in - 1].iov_base;
	*status	= (len < 0) ? VIRTIO_BLK_S_IOERR : VIRTIO_BLK_S_OK;

	mutex_lock(&bdev->mutex);
	virt_queue__set_used_elem(req->vq, req->head, len);
	mutex_unlock(&bdev->mutex);

	if (virtio_queue__should_signal(&bdev->vqs[queueid]))
		bdev->vtrans.trans_ops->signal_vq(req->kvm, &bdev->vtrans, queueid);
}

static void virtio_blk_do_io_request(struct kvm *kvm, struct blk_dev_req *req)
{
	struct virtio_blk_outhdr *req_hdr;
	ssize_t block_cnt;
	struct blk_dev *bdev;
	struct iovec *iov;
	u16 out, in;

	block_cnt	= -1;
	bdev		= req->bdev;
	iov		= req->iov;
	out		= req->out;
	in		= req->in;
	req_hdr		= iov[0].iov_base;

	switch (req_hdr->type) {
	case VIRTIO_BLK_T_IN:
		block_cnt	= disk_image__read(bdev->disk, req_hdr->sector, iov + 1,
					in + out - 2, req);
		break;
	case VIRTIO_BLK_T_OUT:
		block_cnt	= disk_image__write(bdev->disk, req_hdr->sector, iov + 1,
					in + out - 2, req);
		break;
	case VIRTIO_BLK_T_FLUSH:
		block_cnt       = disk_image__flush(bdev->disk);
		virtio_blk_complete(req, block_cnt);
		break;
	case VIRTIO_BLK_T_GET_ID:
		block_cnt	= VIRTIO_BLK_ID_BYTES;
		disk_image__get_serial(bdev->disk, (iov + 1)->iov_base, &block_cnt);
		virtio_blk_complete(req, block_cnt);
		break;
	default:
		pr_warning("request type %d", req_hdr->type);
		block_cnt	= -1;
		break;
	}
}

static void virtio_blk_do_io(struct kvm *kvm, struct virt_queue *vq, struct blk_dev *bdev)
{
	struct blk_dev_req *req;
	u16 head;

	while (virt_queue__available(vq)) {
		head		= virt_queue__pop(vq);
		req		= &bdev->reqs[head];
		req->head	= virt_queue__get_head_iov(vq, req->iov, &req->out, &req->in, head, kvm);
		req->vq		= vq;

		virtio_blk_do_io_request(kvm, req);
	}
}

static void set_config(struct kvm *kvm, void *dev, u8 data, u32 offset)
{
	struct blk_dev *bdev = dev;

	((u8 *)(&bdev->blk_config))[offset] = data;
}

static u8 get_config(struct kvm *kvm, void *dev, u32 offset)
{
	struct blk_dev *bdev = dev;

	return ((u8 *)(&bdev->blk_config))[offset];
}

static u32 get_host_features(struct kvm *kvm, void *dev)
{
	return	1UL << VIRTIO_BLK_F_SEG_MAX
		| 1UL << VIRTIO_BLK_F_FLUSH
		| 1UL << VIRTIO_RING_F_EVENT_IDX
		| 1UL << VIRTIO_RING_F_INDIRECT_DESC;
}

static void set_guest_features(struct kvm *kvm, void *dev, u32 features)
{
	struct blk_dev *bdev = dev;

	bdev->features = features;
}

static int init_vq(struct kvm *kvm, void *dev, u32 vq, u32 pfn)
{
	struct blk_dev *bdev = dev;
	struct virt_queue *queue;
	void *p;

	compat__remove_message(compat_id);

	queue			= &bdev->vqs[vq];
	queue->pfn		= pfn;
	p			= guest_pfn_to_host(kvm, queue->pfn);

	vring_init(&queue->vring, VIRTIO_BLK_QUEUE_SIZE, p, VIRTIO_PCI_VRING_ALIGN);

	return 0;
}

static int notify_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct blk_dev *bdev = dev;

	virtio_blk_do_io(kvm, &bdev->vqs[vq], bdev);

	return 0;
}

static int get_pfn_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct blk_dev *bdev = dev;

	return bdev->vqs[vq].pfn;
}

static int get_size_vq(struct kvm *kvm, void *dev, u32 vq)
{
	return VIRTIO_BLK_QUEUE_SIZE;
}

static struct virtio_ops blk_dev_virtio_ops = (struct virtio_ops) {
	.set_config		= set_config,
	.get_config		= get_config,
	.get_host_features	= get_host_features,
	.set_guest_features	= set_guest_features,
	.init_vq		= init_vq,
	.notify_vq		= notify_vq,
	.get_pfn_vq		= get_pfn_vq,
	.get_size_vq		= get_size_vq,
};

void virtio_blk__init(struct kvm *kvm, struct disk_image *disk)
{
	struct blk_dev *bdev;
	unsigned int i;

	if (!disk)
		return;

	bdev = calloc(1, sizeof(struct blk_dev));
	if (bdev == NULL)
		die("Failed allocating bdev");

	*bdev = (struct blk_dev) {
		.mutex			= PTHREAD_MUTEX_INITIALIZER,
		.req_mutex		= PTHREAD_MUTEX_INITIALIZER,
		.disk			= disk,
		.blk_config		= (struct virtio_blk_config) {
			.capacity	= disk->size / SECTOR_SIZE,
			.seg_max	= DISK_SEG_MAX,
		},
	};

	virtio_trans_init(&bdev->vtrans, VIRTIO_PCI);
	bdev->vtrans.trans_ops->init(kvm, &bdev->vtrans, bdev, PCI_DEVICE_ID_VIRTIO_BLK,
					VIRTIO_ID_BLOCK, PCI_CLASS_BLK);
	bdev->vtrans.virtio_ops = &blk_dev_virtio_ops;

	list_add_tail(&bdev->list, &bdevs);

	for (i = 0; i < ARRAY_SIZE(bdev->reqs); i++) {
		bdev->reqs[i].bdev	= bdev;
		bdev->reqs[i].kvm	= kvm;
	}

	disk_image__set_callback(bdev->disk, virtio_blk_complete);

	if (compat_id != -1)
		compat_id = compat__add_message("virtio-blk device was not detected",
						"While you have requested a virtio-blk device, "
						"the guest kernel did not initialize it.\n"
						"Please make sure that the guest kernel was "
						"compiled with CONFIG_VIRTIO_BLK=y enabled "
						"in its .config");
}

void virtio_blk__init_all(struct kvm *kvm)
{
	int i;

	for (i = 0; i < kvm->nr_disks; i++)
		virtio_blk__init(kvm, kvm->disks[i]);
}

void virtio_blk__delete_all(struct kvm *kvm)
{
	while (!list_empty(&bdevs)) {
		struct blk_dev *bdev;

		bdev = list_first_entry(&bdevs, struct blk_dev, list);
		list_del(&bdev->list);
		free(bdev);
	}
}

#ifndef KVM__VIRTIO_PCI_H
#define KVM__VIRTIO_PCI_H

#include "kvm/pci.h"
#include "kvm/virtio-trans.h"

#include <linux/types.h>

#define VIRTIO_PCI_MAX_VQ	3
#define VIRTIO_PCI_MAX_CONFIG	1

struct kvm;

struct virtio_pci_ioevent_param {
	struct virtio_trans	*vtrans;
	u32			vq;
};

struct virtio_pci {
	struct pci_device_header pci_hdr;
	void			*dev;

	u16			base_addr;
	u8			status;
	u8			isr;

	/* MSI-X */
	u16			config_vector;
	u32			config_gsi;
	u32			vq_vector[VIRTIO_PCI_MAX_VQ];
	u32			gsis[VIRTIO_PCI_MAX_VQ];
	u32			msix_io_block;
	u64			msix_pba;
	struct msix_table	msix_table[VIRTIO_PCI_MAX_VQ + VIRTIO_PCI_MAX_CONFIG];

	/* virtio queue */
	u16			queue_selector;
	struct virtio_pci_ioevent_param ioeventfds[VIRTIO_PCI_MAX_VQ];
};

int virtio_pci__init(struct kvm *kvm, struct virtio_trans *vtrans, void *dev,
			int device_id, int subsys_id, int class);
int virtio_pci__signal_vq(struct kvm *kvm, struct virtio_trans *vtrans, u32 vq);
int virtio_pci__signal_config(struct kvm *kvm, struct virtio_trans *vtrans);

struct virtio_trans_ops *virtio_pci__get_trans_ops(void);

#endif

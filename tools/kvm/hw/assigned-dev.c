#include "kvm/assigned-dev.h"

#include "kvm/util.h"

#include <stddef.h>
#include <sys/ioctl.h>
#include <linux/kernel.h>

#define ASSIGNED_DEV_MAX_DEVICES 16

static u32 assigned_dev_ids;
struct assigned_dev devs[ASSIGNED_DEV_MAX_DEVICES];

static int assigned_dev__set_param(struct assigned_dev *dev, const char *param, const char *val)
{
	/* TODO: Support MSI/MSI-X */

	/* Parse command line options */
	if (strcmp("seg", param) == 0) {
		dev->kvm_assigned_dev.segnr = strtoul(val, NULL, 0);
	} else if (strcmp("bus", param) == 0) {
		dev->kvm_assigned_dev.busnr = strtoul(val, NULL, 0);
	} else if (strcmp("dev", param) == 0) {
		dev->kvm_assigned_dev.devfn = strtoul(val, NULL, 0);
	} else if (strcmp("iommu", param) == 0) {
		u32 iommu = strtoul(val, NULL, 0);

		if (iommu == 1)
			dev->kvm_assigned_dev.flags = 1UL << KVM_DEV_ASSIGN_ENABLE_IOMMU;
	} else if (strcmp("guest_int", param) == 0) {

		/* Clear guest flags */
		dev->kvm_assigned_irq.flags &= KVM_DEV_IRQ_HOST_MASK;
	
		dev->kvm_assigned_irq.flags |= KVM_DEV_IRQ_GUEST_INTX;
		dev->kvm_assigned_irq.guest_irq = strtoul(val, NULL, 0);
	} else if (strcmp("host_int", param) == 0) {
		/* Clear host flags */
		dev->kvm_assigned_irq.flags &= KVM_DEV_IRQ_GUEST_MASK;

		dev->kvm_assigned_irq.flags |= KVM_DEV_IRQ_HOST_INTX;
		dev->kvm_assigned_irq.host_irq = strtoul(val, NULL, 0);
	} else {
		pr_warning("Unknown parameter: %s\n", param);
		return -EINVAL;
	}
	
	return 0;
}

int assigned_dev__parser(const struct option *opt, const char *arg, int unset)
{
	struct assigned_dev *dev = NULL;
	char *buff = NULL, *cmd = NULL, *cur = NULL;
	bool on_cmd = true;
	int r;

	if (assigned_dev_ids >= ASSIGNED_DEV_MAX_DEVICES)
		return -ENOMEM;

	dev = &devs[assigned_dev_ids];
	dev->kvm_assigned_dev.assigned_dev_id = dev->kvm_assigned_irq.assigned_dev_id = assigned_dev_ids;
	

	r = -ENOMEM;
	buff = strdup(arg);
	if (buff == NULL)
		goto fail;

	cur = strtok(buff, ",=");

	while (cur) {
		if (on_cmd) {
			cmd = cur;
		} else {
			r = assigned_dev__set_param(dev, cmd, cur);
			if (r < 0)
				goto fail;
		}
		on_cmd = !on_cmd;

		cur = strtok(NULL, ",=");
	};

	r = 0;
	assigned_dev_ids++;

fail:
	free(buff);

	return r;
}

static int assigned_dev__assign_device(struct kvm *kvm, struct assigned_dev *dev)
{
	int r, err;

	r = ioctl(kvm->vm_fd, KVM_ASSIGN_PCI_DEVICE, &dev->kvm_assigned_dev);
	if (r < 0) {
		err = errno;
		perror("KVM_ASSIGN_PCI_DEVICE failed");
		goto fail;
	}

	r = ioctl(kvm->vm_fd, KVM_ASSIGN_DEV_IRQ, &dev->kvm_assigned_irq);
	if (r < 0) {
		err = errno;
		perror("KVM_ASSIGN_DEV_IRQ failed");
		goto fail_deassign;
	}

	return 0;

fail_deassign:
	ioctl(kvm->vm_fd, KVM_DEASSIGN_PCI_DEVICE, &dev->kvm_assigned_dev);
fail:
	return err;
}

int assigned_dev__init(struct kvm *kvm)
{
	u32 i;

	for (i = 0; i < assigned_dev_ids; i++) {
		int r;

		r = assigned_dev__assign_device(kvm, &devs[i]);
		die("Failed assigning device. Bus: %u Seg: %u Dev: %u",
			devs[i].kvm_assigned_dev.busnr,
			devs[i].kvm_assigned_dev.segnr,
			devs[i].kvm_assigned_dev.devfn);
	}

	return 0;
}

static int assigned_dev__deassign_device(struct kvm *kvm, struct assigned_dev *dev)
{
	int r;

	r = ioctl(kvm->vm_fd, KVM_DEASSIGN_DEV_IRQ, &dev->kvm_assigned_irq);
	if (r)
		perror("KVM_DEASSIGN_DEV_IRQ failed");

	r = ioctl(kvm->vm_fd, KVM_DEASSIGN_PCI_DEVICE, &dev->kvm_assigned_dev);
	if (r)
		perror("KVM_DEASSIGN_DEV_IRQ failed");

	return 0;
}

int assigned_dev__free(struct kvm *kvm)
{
	u32 i;

	for (i = 0; i < assigned_dev_ids; i++)
		assigned_dev__deassign_device(kvm, &devs[i]);

	return 0;
}
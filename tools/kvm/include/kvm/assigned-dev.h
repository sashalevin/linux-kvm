#ifndef KVM__ASSIGNED_H_
#define KVM__ASSIGNED_H_

#include "kvm/kvm.h"
#include "kvm/parse-options.h"

#include <linux/list.h>
#include <linux/kvm.h>

struct assigned_dev {
	struct list_head		list;

	struct kvm_assigned_pci_dev	kvm_assigned_dev;
	struct kvm_assigned_irq		kvm_assigned_irq;
};

int assigned_dev__parser(const struct option *opt, const char *arg, int unset);
int assigned_dev__init(struct kvm *kvm);
int assigned_dev__free(struct kvm *kvm);

#endif
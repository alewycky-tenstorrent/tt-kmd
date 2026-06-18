// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

/*
 * Custom vfio-pci variant driver for Tenstorrent Wormhole and Blackhole.
 *
 * This behaves exactly like the generic vfio-pci driver, with one difference:
 * it advertises PCIe Function Level Reset (FLR) capability. Config-space reads
 * of the PCIe Device Capabilities register report PCI_EXP_DEVCAP_FLR (bit 28)
 * as set, even when the hardware reports it as clear.
 *
 * Binding: the PCI IDs are declared with PCI_DRIVER_OVERRIDE_DEVICE_VFIO(),
 * which marks them override-only. This driver therefore never auto-claims a
 * device away from the native "tenstorrent" driver; an administrator opts a
 * device in explicitly via its driver_override sysfs attribute.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/vfio.h>
#include <linux/vfio_pci_core.h>
#include <linux/version.h>
#include <linux/uaccess.h>

#include "../enumerate.h"

static ssize_t tt_vfio_pci_read(struct vfio_device *core_vdev, char __user *buf,
				size_t count, loff_t *ppos)
{
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);
	unsigned int index = VFIO_PCI_OFFSET_TO_INDEX(*ppos);
	loff_t pos = *ppos & VFIO_PCI_OFFSET_MASK;
	ssize_t ret;

	/* vfio_pci_core_read() advances *ppos, so capture index/pos first. */
	ret = vfio_pci_core_read(core_vdev, buf, count, ppos);
	if (ret <= 0)
		return ret;

	/*
	 * Force the FLR-capable bit (PCI_EXP_DEVCAP_FLR, bit 28) to read as 1.
	 * It lives in the most-significant byte (byte 3) of the PCIe Device
	 * Capabilities register, so we only have to touch that single byte, and
	 * only when the completed read actually covered it.
	 */
	if (index == VFIO_PCI_CONFIG_REGION_INDEX && vdev->pdev->pcie_cap) {
		loff_t flr_byte = vdev->pdev->pcie_cap + PCI_EXP_DEVCAP + 3;

		if (pos <= flr_byte && flr_byte < pos + ret) {
			u8 __user *p = (u8 __user *)buf + (flr_byte - pos);
			u8 val;

			if (get_user(val, p))
				return -EFAULT;
			val |= (u8)(PCI_EXP_DEVCAP_FLR >> 24);	/* 0x10 */
			if (put_user(val, p))
				return -EFAULT;
		}
	}

	return ret;
}

static int tt_vfio_pci_open_device(struct vfio_device *core_vdev)
{
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);
	int ret;

	ret = vfio_pci_core_enable(vdev);
	if (ret)
		return ret;

	vfio_pci_core_finish_enable(vdev);

	return 0;
}

/*
 * Every vfio_device_ops member that postdates the v5.15 baseline is guarded by
 * its introducing kernel version, so this single source builds across the
 * supported range. Guarding the assignment also guards the symbol reference, so
 * helpers that only exist on newer kernels are never named on older ones.
 */
static const struct vfio_device_ops tt_vfio_pci_ops = {
	/* present since v5.15 (no guard) */
	.name		= KBUILD_MODNAME,
	.open_device	= tt_vfio_pci_open_device,
	.close_device	= vfio_pci_core_close_device,
	.ioctl		= vfio_pci_core_ioctl,
	.read		= tt_vfio_pci_read,		/* FLR-bit override */
	.write		= vfio_pci_core_write,
	.mmap		= vfio_pci_core_mmap,
	.request	= vfio_pci_core_request,
	.match		= vfio_pci_core_match,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
	.device_feature	= vfio_pci_core_ioctl_feature,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	.init		= vfio_pci_core_init_dev,
	.release	= vfio_pci_core_release_dev,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
	.bind_iommufd	= vfio_iommufd_physical_bind,
	.unbind_iommufd	= vfio_iommufd_physical_unbind,
	.attach_ioas	= vfio_iommufd_physical_attach_ioas,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
	.detach_ioas	= vfio_iommufd_physical_detach_ioas,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
	.pasid_attach_ioas = vfio_iommufd_physical_pasid_attach_ioas,
	.pasid_detach_ioas = vfio_iommufd_physical_pasid_detach_ioas,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
	.match_token_uuid = vfio_pci_core_match_token_uuid,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
	.get_region_info_caps = vfio_pci_ioctl_get_region_info,
#endif
};

static int tt_vfio_pci_probe(struct pci_dev *pdev,
			     const struct pci_device_id *id)
{
	struct vfio_pci_core_device *vdev;
	int ret;

	vdev = vfio_alloc_device(vfio_pci_core_device, vdev, &pdev->dev,
				 &tt_vfio_pci_ops);
	if (IS_ERR(vdev))
		return PTR_ERR(vdev);

	dev_set_drvdata(&pdev->dev, vdev);
	ret = vfio_pci_core_register_device(vdev);
	if (ret)
		goto out_put_vdev;

	return 0;

out_put_vdev:
	vfio_put_device(&vdev->vdev);
	return ret;
}

static void tt_vfio_pci_remove(struct pci_dev *pdev)
{
	struct vfio_pci_core_device *vdev = dev_get_drvdata(&pdev->dev);

	vfio_pci_core_unregister_device(vdev);
	vfio_put_device(&vdev->vdev);
}

/*
 * Reset hooks. Empty for now: this driver only advertises FLR capability. A
 * later change fills these in to drive the real Tenstorrent reset sequence.
 *
 * reset_prepare/reset_done are members of struct pci_error_handlers (not
 * struct pci_driver), so we wrap them together with vfio-core's AER handler in
 * our own error-handler table rather than reusing vfio_pci_core_err_handlers.
 */
static void tt_vfio_pci_reset_prepare(struct pci_dev *pdev)
{
}

static void tt_vfio_pci_reset_done(struct pci_dev *pdev)
{
}

static const struct pci_error_handlers tt_vfio_pci_err_handlers = {
	.error_detected	= vfio_pci_core_aer_err_detected,
	.reset_prepare	= tt_vfio_pci_reset_prepare,
	.reset_done	= tt_vfio_pci_reset_done,
};

static const struct pci_device_id tt_vfio_pci_table[] = {
	{ PCI_DRIVER_OVERRIDE_DEVICE_VFIO(PCI_VENDOR_ID_TENSTORRENT,
					  PCI_DEVICE_ID_WORMHOLE) },
	{ PCI_DRIVER_OVERRIDE_DEVICE_VFIO(PCI_VENDOR_ID_TENSTORRENT,
					  PCI_DEVICE_ID_BLACKHOLE) },
	{}
};
MODULE_DEVICE_TABLE(pci, tt_vfio_pci_table);

static struct pci_driver tt_vfio_pci_driver = {
	.name			= KBUILD_MODNAME,
	.id_table		= tt_vfio_pci_table,
	.probe			= tt_vfio_pci_probe,
	.remove			= tt_vfio_pci_remove,
	.err_handler		= &tt_vfio_pci_err_handlers,
	.driver_managed_dma	= true,
};

module_pci_driver(tt_vfio_pci_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Tenstorrent Wormhole/Blackhole VFIO PCI variant driver (advertises FLR)");

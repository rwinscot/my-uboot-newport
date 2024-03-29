/*
 * Copyright (c) 2014 Google, Inc
 * Written by Simon Glass <sjg@chromium.org>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <dm.h>
#include <errno.h>
#include <inttypes.h>
#include <pci.h>
#include <asm/io.h>
#include <dm/device-internal.h>
#include <dm/lists.h>
#if defined(CONFIG_X86) && defined(CONFIG_HAVE_FSP)
#include <asm/fsp/fsp_support.h>
#endif
#include "pci_internal.h"

DECLARE_GLOBAL_DATA_PTR;

int pci_get_bus(int busnum, struct udevice **busp)
{
	int ret;

	ret = uclass_get_device_by_seq(UCLASS_PCI, busnum, busp);

	/* Since buses may not be numbered yet try a little harder with bus 0 */
	if (ret == -ENODEV) {
		ret = uclass_first_device_err(UCLASS_PCI, busp);
		if (ret)
			return ret;
		ret = uclass_get_device_by_seq(UCLASS_PCI, busnum, busp);
	}

	return ret;
}

struct udevice *pci_get_controller(struct udevice *dev)
{
	while (device_is_on_pci_bus(dev))
		dev = dev->parent;

	return dev;
}

pci_dev_t dm_pci_get_bdf(struct udevice *dev)
{
	struct pci_child_platdata *pplat = dev_get_parent_platdata(dev);
	struct udevice *bus = dev->parent;

	return PCI_ADD_BUS(bus->seq, pplat->devfn);
}

/**
 * pci_get_bus_max() - returns the bus number of the last active bus
 *
 * @return last bus number, or -1 if no active buses
 */
static int pci_get_bus_max(void)
{
	struct udevice *bus;
	struct uclass *uc;
	int ret = -1;

	ret = uclass_get(UCLASS_PCI, &uc);
	uclass_foreach_dev(bus, uc) {
		if (bus->seq > ret)
			ret = bus->seq;
	}

	debug("%s: ret=%d\n", __func__, ret);

	return ret;
}

int pci_last_busno(void)
{
	return pci_get_bus_max();
}

int pci_get_ff(enum pci_size_t size)
{
	switch (size) {
	case PCI_SIZE_8:
		return 0xff;
	case PCI_SIZE_16:
		return 0xffff;
	default:
		return 0xffffffff;
	}
}

int pci_bus_find_devfn(struct udevice *bus, pci_dev_t find_devfn,
		       struct udevice **devp)
{
	struct udevice *dev;

	for (device_find_first_child(bus, &dev);
	     dev;
	     device_find_next_child(&dev)) {
		struct pci_child_platdata *pplat;

		pplat = dev_get_parent_platdata(dev);
		if (pplat && pplat->devfn == find_devfn) {
			*devp = dev;
			return 0;
		}
	}

	return -ENODEV;
}

int dm_pci_bus_find_bdf(pci_dev_t bdf, struct udevice **devp)
{
	struct udevice *bus;
	int ret;

	ret = pci_get_bus(PCI_BUS(bdf), &bus);
	if (ret)
		return ret;
	return pci_bus_find_devfn(bus, PCI_MASK_BUS(bdf), devp);
}

static int pci_device_matches_ids(struct udevice *dev,
				  struct pci_device_id *ids)
{
	struct pci_child_platdata *pplat;
	int i;

	pplat = dev_get_parent_platdata(dev);
	if (!pplat)
		return -EINVAL;
	for (i = 0; ids[i].vendor != 0; i++) {
		if (pplat->vendor == ids[i].vendor &&
		    pplat->device == ids[i].device)
			return i;
	}

	return -EINVAL;
}

int pci_bus_find_devices(struct udevice *bus, struct pci_device_id *ids,
			 int *indexp, struct udevice **devp)
{
	struct udevice *dev;

	/* Scan all devices on this bus */
	for (device_find_first_child(bus, &dev);
	     dev;
	     device_find_next_child(&dev)) {
		if (pci_device_matches_ids(dev, ids) >= 0) {
			if ((*indexp)-- <= 0) {
				*devp = dev;
				return 0;
			}
		}
	}

	return -ENODEV;
}

int pci_find_device_id(struct pci_device_id *ids, int index,
		       struct udevice **devp)
{
	struct udevice *bus;

	/* Scan all known buses */
	for (uclass_first_device(UCLASS_PCI, &bus);
	     bus;
	     uclass_next_device(&bus)) {
		if (!pci_bus_find_devices(bus, ids, &index, devp))
			return 0;
	}
	*devp = NULL;

	return -ENODEV;
}

static int dm_pci_bus_find_device(struct udevice *bus, unsigned int vendor,
				  unsigned int device, int *indexp,
				  struct udevice **devp)
{
	struct pci_child_platdata *pplat;
	struct udevice *dev;

	for (device_find_first_child(bus, &dev);
	     dev;
	     device_find_next_child(&dev)) {
		pplat = dev_get_parent_platdata(dev);
		if (pplat->vendor == vendor && pplat->device == device) {
			if (!(*indexp)--) {
				*devp = dev;
				return 0;
			}
		}
	}

	return -ENODEV;
}

int dm_pci_find_device(unsigned int vendor, unsigned int device, int index,
		       struct udevice **devp)
{
	struct udevice *bus;

	/* Scan all known buses */
	for (uclass_first_device(UCLASS_PCI, &bus);
	     bus;
	     uclass_next_device(&bus)) {
		if (!dm_pci_bus_find_device(bus, vendor, device, &index, devp))
			return device_probe(*devp);
	}
	*devp = NULL;

	return -ENODEV;
}

int dm_pci_find_class(uint find_class, int index, struct udevice **devp)
{
	struct udevice *dev;

	/* Scan all known buses */
	for (pci_find_first_device(&dev);
	     dev;
	     pci_find_next_device(&dev)) {
		struct pci_child_platdata *pplat = dev_get_parent_platdata(dev);

		if (pplat->class == find_class && !index--) {
			*devp = dev;
			return device_probe(*devp);
		}
	}
	*devp = NULL;

	return -ENODEV;
}

int pci_bus_write_config(struct udevice *bus, pci_dev_t bdf, int offset,
			 unsigned long value, enum pci_size_t size)
{
	struct dm_pci_ops *ops;

	ops = pci_get_ops(bus);
	if (!ops->write_config)
		return -ENOSYS;
	return ops->write_config(bus, bdf, offset, value, size);
}

int pci_bus_clrset_config32(struct udevice *bus, pci_dev_t bdf, int offset,
			    u32 clr, u32 set)
{
	ulong val;
	int ret;

	ret = pci_bus_read_config(bus, bdf, offset, &val, PCI_SIZE_32);
	if (ret)
		return ret;
	val &= ~clr;
	val |= set;

	return pci_bus_write_config(bus, bdf, offset, val, PCI_SIZE_32);
}

int pci_write_config(pci_dev_t bdf, int offset, unsigned long value,
		     enum pci_size_t size)
{
	struct udevice *bus;
	int ret;

	ret = pci_get_bus(PCI_BUS(bdf), &bus);
	if (ret)
		return ret;

	return pci_bus_write_config(bus, bdf, offset, value, size);
}

int dm_pci_write_config(struct udevice *dev, int offset, unsigned long value,
			enum pci_size_t size)
{
	struct udevice *bus;

	for (bus = dev; device_is_on_pci_bus(bus);)
		bus = bus->parent;
	return pci_bus_write_config(bus, dm_pci_get_bdf(dev), offset, value,
				    size);
}

int pci_write_config32(pci_dev_t bdf, int offset, u32 value)
{
	return pci_write_config(bdf, offset, value, PCI_SIZE_32);
}

int pci_write_config16(pci_dev_t bdf, int offset, u16 value)
{
	return pci_write_config(bdf, offset, value, PCI_SIZE_16);
}

int pci_write_config8(pci_dev_t bdf, int offset, u8 value)
{
	return pci_write_config(bdf, offset, value, PCI_SIZE_8);
}

int dm_pci_write_config8(struct udevice *dev, int offset, u8 value)
{
	return dm_pci_write_config(dev, offset, value, PCI_SIZE_8);
}

int dm_pci_write_config16(struct udevice *dev, int offset, u16 value)
{
	return dm_pci_write_config(dev, offset, value, PCI_SIZE_16);
}

int dm_pci_write_config32(struct udevice *dev, int offset, u32 value)
{
	return dm_pci_write_config(dev, offset, value, PCI_SIZE_32);
}

int pci_bus_read_config(struct udevice *bus, pci_dev_t bdf, int offset,
			unsigned long *valuep, enum pci_size_t size)
{
	struct dm_pci_ops *ops;

	ops = pci_get_ops(bus);
	if (!ops->read_config)
		return -ENOSYS;
	return ops->read_config(bus, bdf, offset, valuep, size);
}

int pci_read_config(pci_dev_t bdf, int offset, unsigned long *valuep,
		    enum pci_size_t size)
{
	struct udevice *bus;
	int ret;

	ret = pci_get_bus(PCI_BUS(bdf), &bus);
	if (ret)
		return ret;

	return pci_bus_read_config(bus, bdf, offset, valuep, size);
}

int dm_pci_read_config(struct udevice *dev, int offset, unsigned long *valuep,
		       enum pci_size_t size)
{
	struct udevice *bus;

	for (bus = dev; device_is_on_pci_bus(bus);)
		bus = bus->parent;
	return pci_bus_read_config(bus, dm_pci_get_bdf(dev), offset, valuep,
				   size);
}

int pci_read_config32(pci_dev_t bdf, int offset, u32 *valuep)
{
	unsigned long value;
	int ret;

	ret = pci_read_config(bdf, offset, &value, PCI_SIZE_32);
	if (ret)
		return ret;
	*valuep = value;

	return 0;
}

int pci_read_config16(pci_dev_t bdf, int offset, u16 *valuep)
{
	unsigned long value;
	int ret;

	ret = pci_read_config(bdf, offset, &value, PCI_SIZE_16);
	if (ret)
		return ret;
	*valuep = value;

	return 0;
}

int pci_read_config8(pci_dev_t bdf, int offset, u8 *valuep)
{
	unsigned long value;
	int ret;

	ret = pci_read_config(bdf, offset, &value, PCI_SIZE_8);
	if (ret)
		return ret;
	*valuep = value;

	return 0;
}

int dm_pci_read_config8(struct udevice *dev, int offset, u8 *valuep)
{
	unsigned long value;
	int ret;

	ret = dm_pci_read_config(dev, offset, &value, PCI_SIZE_8);
	if (ret)
		return ret;
	*valuep = value;

	return 0;
}

int dm_pci_read_config16(struct udevice *dev, int offset, u16 *valuep)
{
	unsigned long value;
	int ret;

	ret = dm_pci_read_config(dev, offset, &value, PCI_SIZE_16);
	if (ret)
		return ret;
	*valuep = value;

	return 0;
}

int dm_pci_read_config32(struct udevice *dev, int offset, u32 *valuep)
{
	unsigned long value;
	int ret;

	ret = dm_pci_read_config(dev, offset, &value, PCI_SIZE_32);
	if (ret)
		return ret;
	*valuep = value;

	return 0;
}

int dm_pci_clrset_config8(struct udevice *dev, int offset, u32 clr, u32 set)
{
	u8 val;
	int ret;

	ret = dm_pci_read_config8(dev, offset, &val);
	if (ret)
		return ret;
	val &= ~clr;
	val |= set;

	return dm_pci_write_config8(dev, offset, val);
}

int dm_pci_clrset_config16(struct udevice *dev, int offset, u32 clr, u32 set)
{
	u16 val;
	int ret;

	ret = dm_pci_read_config16(dev, offset, &val);
	if (ret)
		return ret;
	val &= ~clr;
	val |= set;

	return dm_pci_write_config16(dev, offset, val);
}

int dm_pci_clrset_config32(struct udevice *dev, int offset, u32 clr, u32 set)
{
	u32 val;
	int ret;

	ret = dm_pci_read_config32(dev, offset, &val);
	if (ret)
		return ret;
	val &= ~clr;
	val |= set;

	return dm_pci_write_config32(dev, offset, val);
}

static void set_vga_bridge_bits(struct udevice *dev)
{
	struct udevice *parent = dev->parent;
	u16 bc;

	while (parent->seq != 0) {
		dm_pci_read_config16(parent, PCI_BRIDGE_CONTROL, &bc);
		bc |= PCI_BRIDGE_CTL_VGA;
		dm_pci_write_config16(parent, PCI_BRIDGE_CONTROL, bc);
		parent = parent->parent;
	}
}

int pci_auto_config_devices(struct udevice *bus)
{
	struct pci_controller *hose = bus->uclass_priv;
	struct pci_child_platdata *pplat;
	unsigned int sub_bus;
	struct udevice *dev;
	int ret;

	sub_bus = bus->seq;
	debug("%s: start\n", __func__);
	pciauto_config_init(hose);
	for (ret = device_find_first_child(bus, &dev);
	     !ret && dev;
	     ret = device_find_next_child(&dev)) {
		unsigned int max_bus;
		int ret;

		debug("%s: device %s\n", __func__, dev->name);
		ret = dm_pciauto_config_device(dev);
		if (ret < 0)
			return ret;
		max_bus = ret;
		sub_bus = max(sub_bus, max_bus);

		pplat = dev_get_parent_platdata(dev);
		if (pplat->class == (PCI_CLASS_DISPLAY_VGA << 8))
			set_vga_bridge_bits(dev);
	}
	debug("%s: done\n", __func__);

	return sub_bus;
}

int dm_pci_hose_probe_bus(struct udevice *bus)
{
	int sub_bus;
	int ret;
	int ea_pos;
	u8 reg;

	debug("%s\n", __func__);

	ea_pos = dm_pci_find_capability(bus, PCI_CAP_ID_EA);

	if (ea_pos) {
		dm_pci_read_config8(bus, ea_pos + sizeof(u32) + sizeof(u8), &reg);
		sub_bus = reg;
		debug("%s: bus = %d/%s\n", __func__, sub_bus, bus->name);
	} else {
		sub_bus = pci_get_bus_max() + 1;
		debug("%s: bus = %d/%s\n", __func__, sub_bus, bus->name);
		dm_pciauto_prescan_setup_bridge(bus, sub_bus);
	}

	ret = device_probe(bus);
	if (ret) {
		debug("%s: Cannot probe bus %s: %d\n", __func__, bus->name,
		      ret);
		return ret;
	}

	if (!ea_pos) {
		if (sub_bus != bus->seq) {
			printf("%s: Internal error, bus '%s' got seq %d, expected %d\n",
			       __func__, bus->name, bus->seq, sub_bus);
			return -EPIPE;
		}
		sub_bus = pci_get_bus_max();
		dm_pciauto_postscan_setup_bridge(bus, sub_bus);
	}

	return sub_bus;
}

/**
 * pci_match_one_device - Tell if a PCI device structure has a matching
 *                        PCI device id structure
 * @id: single PCI device id structure to match
 * @find: the PCI device id structure to match against
 *
 * Returns true if the finding pci_device_id structure matched or false if
 * there is no match.
 */
static bool pci_match_one_id(const struct pci_device_id *id,
			     const struct pci_device_id *find)
{
	if ((id->vendor == PCI_ANY_ID || id->vendor == find->vendor) &&
	    (id->device == PCI_ANY_ID || id->device == find->device) &&
	    (id->subvendor == PCI_ANY_ID || id->subvendor == find->subvendor) &&
	    (id->subdevice == PCI_ANY_ID || id->subdevice == find->subdevice) &&
	    !((id->class ^ find->class) & id->class_mask))
		return true;

	return false;
}

/**
 * pci_find_and_bind_driver() - Find and bind the right PCI driver
 *
 * This only looks at certain fields in the descriptor.
 *
 * @parent:	Parent bus
 * @find_id:	Specification of the driver to find
 * @bdf:	Bus/device/function addreess - see PCI_BDF()
 * @devp:	Returns a pointer to the device created
 * @return 0 if OK, -EPERM if the device is not needed before relocation and
 *	   therefore was not created, other -ve value on error
 */
static int pci_find_and_bind_driver(struct udevice *parent,
				    struct pci_device_id *find_id,
				    pci_dev_t bdf, struct udevice **devp)
{
	struct pci_driver_entry *start, *entry;
	const char *drv;
	int n_ents;
	int ret;
	char name[30], *str;
	bool bridge;

	*devp = NULL;

	debug("%s: Searching for driver: vendor=%x, device=%x\n", __func__,
	      find_id->vendor, find_id->device);
	start = ll_entry_start(struct pci_driver_entry, pci_driver_entry);
	n_ents = ll_entry_count(struct pci_driver_entry, pci_driver_entry);
	for (entry = start; entry != start + n_ents; entry++) {
		const struct pci_device_id *id;
		struct udevice *dev;
		const struct driver *drv;

		for (id = entry->match;
		     id->vendor || id->subvendor || id->class_mask;
		     id++) {
			if (!pci_match_one_id(id, find_id))
				continue;

			drv = entry->driver;

			/*
			 * In the pre-relocation phase, we only bind devices
			 * whose driver has the DM_FLAG_PRE_RELOC set, to save
			 * precious memory space as on some platforms as that
			 * space is pretty limited (ie: using Cache As RAM).
			 */
			if (!(gd->flags & GD_FLG_RELOC) &&
			    !(drv->flags & DM_FLAG_PRE_RELOC))
				return -EPERM;

			/*
			 * We could pass the descriptor to the driver as
			 * platdata (instead of NULL) and allow its bind()
			 * method to return -ENOENT if it doesn't support this
			 * device. That way we could continue the search to
			 * find another driver. For now this doesn't seem
			 * necesssary, so just bind the first match.
			 */
			ret = device_bind(parent, drv, drv->name, NULL, -1,
					  &dev);
			if (ret)
				goto error;
			debug("%s: Match found: %s\n", __func__, drv->name);
			dev->driver_data = find_id->driver_data;
			*devp = dev;
			return 0;
		}
	}

	bridge = (find_id->class >> 8) == PCI_CLASS_BRIDGE_PCI;
	/*
	 * In the pre-relocation phase, we only bind bridge devices to save
	 * precious memory space as on some platforms as that space is pretty
	 * limited (ie: using Cache As RAM).
	 */
	if (!(gd->flags & GD_FLG_RELOC) && !bridge)
		return -EPERM;

	/* Bind a generic driver so that the device can be used */
	sprintf(name, "pci_%x:%x.%x", parent->seq, PCI_DEV(bdf),
		PCI_FUNC(bdf));
	str = strdup(name);
	if (!str)
		return -ENOMEM;
	drv = bridge ? "pci_bridge_drv" : "pci_generic_drv";

	ret = device_bind_driver(parent, drv, str, devp);
	if (ret) {
		debug("%s: Failed to bind generic driver: %d\n", __func__, ret);
		free(str);
		return ret;
	}
	debug("%s: No match found: bound generic driver instead\n", __func__);

	return 0;

error:
	debug("%s: No match found: error %d\n", __func__, ret);
	return ret;
}

int pci_sriov_init(struct udevice *pdev, int vf_en)
{
	u16 vendor, device;
	struct udevice *bus;
	struct udevice *dev;
	pci_dev_t bdf;
	u16 ctrl;
	u16 num_vfs;
	u16 total_vf;
	u16 vf_offset;
	u16 vf_stride;
	int vf, ret;
	int pos;

	pos = dm_pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_SRIOV);
	if (!pos) {
		printf("Error: SRIOV capability not found\n");
		return -ENODEV;
	}

	dm_pci_read_config16(pdev, pos + PCI_SRIOV_CTRL, &ctrl);

	dm_pci_read_config16(pdev, pos + PCI_SRIOV_TOTAL_VF, &total_vf);

	if (vf_en > total_vf)
		vf_en = total_vf;

	dm_pci_write_config16(pdev, pos + PCI_SRIOV_NUM_VF, vf_en);

	ctrl |= PCI_SRIOV_CTRL_VFE | PCI_SRIOV_CTRL_MSE;
	dm_pci_write_config16(pdev, pos + PCI_SRIOV_CTRL, ctrl);

	dm_pci_read_config16(pdev, pos + PCI_SRIOV_NUM_VF, &num_vfs);

	dm_pci_read_config16(pdev, pos + PCI_SRIOV_VF_OFFSET, &vf_offset);
	dm_pci_read_config16(pdev, pos + PCI_SRIOV_VF_STRIDE, &vf_stride);

	dm_pci_read_config16(pdev, PCI_VENDOR_ID, &vendor);
	dm_pci_read_config16(pdev, pos + PCI_SRIOV_VF_DID, &device);

	bdf = dm_pci_get_bdf(pdev);

	pci_get_bus(PCI_BUS(bdf), &bus);

	if (!bus)
		return -ENODEV;

	bdf += PCI_BDF(0, 0, vf_offset);

	for (vf = 0; vf < num_vfs; vf++) {
		struct pci_child_platdata *pplat;
		ulong class;

		pci_bus_read_config(bus, bdf, PCI_CLASS_REVISION,
				    &class, PCI_SIZE_32);

		class >>= 8;

		debug("%s: bus %d/%s: found VF %x:%x\n", __func__,
		      bus->seq, bus->name, PCI_DEV(bdf), PCI_FUNC(bdf));

		/* Find this device in the device tree */
		ret = pci_bus_find_devfn(bus, PCI_MASK_BUS(bdf), &dev);

		if (ret == -ENODEV) {
			struct pci_device_id find_id;

			memset(&find_id, 0, sizeof(find_id));

			find_id.vendor = vendor;
			find_id.device = device;
			find_id.class = class >> 8;

			ret = pci_find_and_bind_driver(bus, &find_id,
						       bdf, &dev);

			if (ret)
				return ret;
		}

		/* Update the platform data */
		pplat = dev_get_parent_platdata(dev);
		pplat->devfn = PCI_MASK_BUS(bdf);
		pplat->vendor = vendor;
		pplat->device = device;
		pplat->class = class;
		pplat->is_phys = false;
		pplat->pdev = pdev;
		pplat->vf_id = vf * vf_stride + vf_offset;

		bdf += PCI_BDF(0, 0, vf_stride);
	}

	return 0;

}

int pci_sriov_get_totalvfs(struct udevice *pdev)
{
	u16 total_vf;
	int pos;

	pos = dm_pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_SRIOV);
	if (!pos) {
		printf("Error: SRIOV capability not found\n");
		return -ENODEV;
	}

	dm_pci_read_config16(pdev, pos + PCI_SRIOV_TOTAL_VF, &total_vf);

	return total_vf;
}

int pci_bind_bus_devices(struct udevice *bus)
{
	ulong vendor, device;
	ulong header_type;
	pci_dev_t bdf, end;
	bool found_multi;
	int ret;

	found_multi = false;
	end = PCI_BDF(bus->seq, PCI_MAX_PCI_DEVICES - 1,
		      PCI_MAX_PCI_FUNCTIONS - 1);
	for (bdf = PCI_BDF(bus->seq, 0, 0); bdf <= end;
	     bdf += PCI_BDF(0, 0, 1)) {
		struct pci_child_platdata *pplat;
		struct udevice *dev;
		ulong class;

		if (PCI_FUNC(bdf) && !found_multi)
			continue;
		/* Check only the first access, we don't expect problems */
		ret = pci_bus_read_config(bus, bdf, PCI_HEADER_TYPE,
					  &header_type, PCI_SIZE_8);
		if (ret)
			goto error;
		pci_bus_read_config(bus, bdf, PCI_VENDOR_ID, &vendor,
				    PCI_SIZE_16);
		if (vendor == 0xffff || vendor == 0x0000)
			continue;

		if (!PCI_FUNC(bdf))
			found_multi = header_type & 0x80;

		debug("%s: bus %d/%s: found device %x, function %d\n", __func__,
		      bus->seq, bus->name, PCI_DEV(bdf), PCI_FUNC(bdf));
		pci_bus_read_config(bus, bdf, PCI_DEVICE_ID, &device,
				    PCI_SIZE_16);
		pci_bus_read_config(bus, bdf, PCI_CLASS_REVISION, &class,
				    PCI_SIZE_32);
		class >>= 8;

		/* Find this device in the device tree */
		ret = pci_bus_find_devfn(bus, PCI_MASK_BUS(bdf), &dev);

		/* If nothing in the device tree, bind a device */
		if (ret == -ENODEV) {
			struct pci_device_id find_id;
			ulong val;

			memset(&find_id, '\0', sizeof(find_id));
			find_id.vendor = vendor;
			find_id.device = device;
			find_id.class = class;
			if ((header_type & 0x7f) == PCI_HEADER_TYPE_NORMAL) {
				pci_bus_read_config(bus, bdf,
						    PCI_SUBSYSTEM_VENDOR_ID,
						    &val, PCI_SIZE_32);
				find_id.subvendor = val & 0xffff;
				find_id.subdevice = val >> 16;
			}
			ret = pci_find_and_bind_driver(bus, &find_id, bdf,
						       &dev);
		}
		if (ret == -EPERM)
			continue;
		else if (ret)
			return ret;

		/* Update the platform data */
		pplat = dev_get_parent_platdata(dev);
		pplat->devfn = PCI_MASK_BUS(bdf);
		pplat->vendor = vendor;
		pplat->device = device;
		pplat->class = class;
		pplat->is_phys = true;
	}

	return 0;
error:
	printf("Cannot read bus configuration: %d\n", ret);

	return ret;
}

static int decode_regions(struct pci_controller *hose, ofnode parent_node,
			  ofnode node)
{
	int pci_addr_cells, addr_cells, size_cells;
	phys_addr_t base = 0, size;
	int cells_per_record;
	const u32 *prop;
	int len;
	int i;

	prop = ofnode_get_property(node, "ranges", &len);
	if (!prop)
		return -EINVAL;
	pci_addr_cells = ofnode_read_simple_addr_cells(node);
	addr_cells = ofnode_read_simple_addr_cells(parent_node);
	size_cells = ofnode_read_simple_size_cells(node);

	/* PCI addresses are always 3-cells */
	len /= sizeof(u32);
	cells_per_record = pci_addr_cells + addr_cells + size_cells;
	hose->region_count = 0;
	debug("%s: len=%d, cells_per_record=%d\n", __func__, len,
	      cells_per_record);
	for (i = 0; i < MAX_PCI_REGIONS; i++, len -= cells_per_record) {
		u64 pci_addr, addr, size;
		int space_code;
		u32 flags;
		int type;
		int pos;

		if (len < cells_per_record)
			break;
		flags = fdt32_to_cpu(prop[0]);
		space_code = (flags >> 24) & 3;
		pci_addr = fdtdec_get_number(prop + 1, 2);
		prop += pci_addr_cells;
		addr = fdtdec_get_number(prop, addr_cells);
		prop += addr_cells;
		size = fdtdec_get_number(prop, size_cells);
		prop += size_cells;
		debug("%s: region %d, pci_addr=%" PRIx64 ", addr=%" PRIx64
		      ", size=%" PRIx64 ", space_code=%d\n", __func__,
		      hose->region_count, pci_addr, addr, size, space_code);
		if (space_code & 2) {
			type = flags & (1U << 30) ? PCI_REGION_PREFETCH :
					PCI_REGION_MEM;
		} else if (space_code & 1) {
			type = PCI_REGION_IO;
		} else {
			continue;
		}

		pos = hose->region_count++;
		debug(" - type=%d, pos=%d\n", type, pos);
		pci_set_region(hose->regions + pos, pci_addr, addr, size, type);
	}

	if(hose->region_count == MAX_PCI_REGIONS) {
		printf("PCI region count reached limit, cannot add local memory region");
		return 1;
	}

	/* Add a region for our local memory */
	size = gd->ram_size;
#ifdef CONFIG_SYS_SDRAM_BASE
	base = CONFIG_SYS_SDRAM_BASE;
#endif
	if (gd->pci_ram_top && gd->pci_ram_top < base + size)
		size = gd->pci_ram_top - base;

	pci_set_region(hose->regions + hose->region_count++, base, base,
		       size, PCI_REGION_MEM | PCI_REGION_SYS_MEMORY);

	return 0;
}

static int pci_uclass_pre_probe(struct udevice *bus)
{
	struct pci_controller *hose;
	int ret;

	debug("%s, bus=%d/%s, parent=%s\n", __func__, bus->seq, bus->name,
	      bus->parent->name);
	hose = bus->uclass_priv;

	/* For bridges, use the top-level PCI controller */
	if (!device_is_on_pci_bus(bus)) {
		hose->ctlr = bus;
		ret = decode_regions(hose, dev_ofnode(bus->parent),
				     dev_ofnode(bus));
		if (ret) {
			debug("%s: Cannot decode regions\n", __func__);
			return ret;
		}
	} else {
		struct pci_controller *parent_hose;

		parent_hose = dev_get_uclass_priv(bus->parent);
		hose->ctlr = parent_hose->bus;
	}
	hose->bus = bus;
	hose->first_busno = bus->seq;
	hose->last_busno = bus->seq;

	return 0;
}

static int pci_uclass_post_probe(struct udevice *bus)
{
	int ret;

	debug("%s: probing bus %d\n", __func__, bus->seq);
	ret = pci_bind_bus_devices(bus);
	if (ret)
		return ret;

#ifdef CONFIG_PCI_PNP
	ret = pci_auto_config_devices(bus);
	if (ret < 0)
		return ret;
#endif

#if defined(CONFIG_X86) && defined(CONFIG_HAVE_FSP)
	/*
	 * Per Intel FSP specification, we should call FSP notify API to
	 * inform FSP that PCI enumeration has been done so that FSP will
	 * do any necessary initialization as required by the chipset's
	 * BIOS Writer's Guide (BWG).
	 *
	 * Unfortunately we have to put this call here as with driver model,
	 * the enumeration is all done on a lazy basis as needed, so until
	 * something is touched on PCI it won't happen.
	 *
	 * Note we only call this 1) after U-Boot is relocated, and 2)
	 * root bus has finished probing.
	 */
	if ((gd->flags & GD_FLG_RELOC) && (bus->seq == 0)) {
		ret = fsp_init_phase_pci();
		if (ret)
			return ret;
	}
#endif

	return 0;
}

static int pci_uclass_child_post_bind(struct udevice *dev)
{
	struct pci_child_platdata *pplat;
	struct fdt_pci_addr addr;
	int ret;

	if (!dev_of_valid(dev))
		return 0;

	/*
	 * We could read vendor, device, class if available. But for now we
	 * just check the address.
	 */
	pplat = dev_get_parent_platdata(dev);
	ret = ofnode_read_pci_addr(dev_ofnode(dev), FDT_PCI_SPACE_CONFIG, "reg",
				   &addr);

	if (ret) {
		if (ret != -ENOENT)
			return -EINVAL;
	} else {
		/* extract the devfn from fdt_pci_addr */
		pplat->devfn = addr.phys_hi & 0xff00;
	}

	return 0;
}

static int pci_bridge_read_config(struct udevice *bus, pci_dev_t bdf,
				  uint offset, ulong *valuep,
				  enum pci_size_t size)
{
	struct pci_controller *hose = bus->uclass_priv;

	return pci_bus_read_config(hose->ctlr, bdf, offset, valuep, size);
}

static int pci_bridge_write_config(struct udevice *bus, pci_dev_t bdf,
				   uint offset, ulong value,
				   enum pci_size_t size)
{
	struct pci_controller *hose = bus->uclass_priv;

	return pci_bus_write_config(hose->ctlr, bdf, offset, value, size);
}

static int skip_to_next_device(struct udevice *bus, struct udevice **devp)
{
	struct udevice *dev;
	int ret = 0;

	/*
	 * Scan through all the PCI controllers. On x86 there will only be one
	 * but that is not necessarily true on other hardware.
	 */
	do {
		device_find_first_child(bus, &dev);
		if (dev) {
			*devp = dev;
			return 0;
		}
		ret = uclass_next_device(&bus);
		if (ret)
			return ret;
	} while (bus);

	return 0;
}

int pci_find_next_device(struct udevice **devp)
{
	struct udevice *child = *devp;
	struct udevice *bus = child->parent;
	int ret;

	/* First try all the siblings */
	*devp = NULL;
	while (child) {
		device_find_next_child(&child);
		if (child) {
			*devp = child;
			return 0;
		}
	}

	/* We ran out of siblings. Try the next bus */
	ret = uclass_next_device(&bus);
	if (ret)
		return ret;

	return bus ? skip_to_next_device(bus, devp) : 0;
}

int pci_find_first_device(struct udevice **devp)
{
	struct udevice *bus;
	int ret;

	*devp = NULL;
	ret = uclass_first_device(UCLASS_PCI, &bus);
	if (ret)
		return ret;

	return skip_to_next_device(bus, devp);
}

ulong pci_conv_32_to_size(ulong value, uint offset, enum pci_size_t size)
{
	switch (size) {
	case PCI_SIZE_8:
		return (value >> ((offset & 3) * 8)) & 0xff;
	case PCI_SIZE_16:
		return (value >> ((offset & 2) * 8)) & 0xffff;
	default:
		return value;
	}
}

ulong pci_conv_size_to_32(ulong old, ulong value, uint offset,
			  enum pci_size_t size)
{
	uint off_mask;
	uint val_mask, shift;
	ulong ldata, mask;

	switch (size) {
	case PCI_SIZE_8:
		off_mask = 3;
		val_mask = 0xff;
		break;
	case PCI_SIZE_16:
		off_mask = 2;
		val_mask = 0xffff;
		break;
	default:
		return value;
	}
	shift = (offset & off_mask) * 8;
	ldata = (value & val_mask) << shift;
	mask = val_mask << shift;
	value = (old & ~mask) | ldata;

	return value;
}

int pci_get_regions(struct udevice *dev, struct pci_region **iop,
		    struct pci_region **memp, struct pci_region **prefp)
{
	struct udevice *bus = pci_get_controller(dev);
	struct pci_controller *hose = dev_get_uclass_priv(bus);
	int i;

	*iop = NULL;
	*memp = NULL;
	*prefp = NULL;
	for (i = 0; i < hose->region_count; i++) {
		switch (hose->regions[i].flags) {
		case PCI_REGION_IO:
			if (!*iop || (*iop)->size < hose->regions[i].size)
				*iop = hose->regions + i;
			break;
		case PCI_REGION_MEM:
			if (!*memp || (*memp)->size < hose->regions[i].size)
				*memp = hose->regions + i;
			break;
		case (PCI_REGION_MEM | PCI_REGION_PREFETCH):
			if (!*prefp || (*prefp)->size < hose->regions[i].size)
				*prefp = hose->regions + i;
			break;
		}
	}

	return (*iop != NULL) + (*memp != NULL) + (*prefp != NULL);
}

u32 dm_pci_read_bar32(struct udevice *dev, int barnum)
{
	u32 addr;
	int bar;

	bar = PCI_BASE_ADDRESS_0 + barnum * 4;
	dm_pci_read_config32(dev, bar, &addr);
	if (addr & PCI_BASE_ADDRESS_SPACE_IO)
		return addr & PCI_BASE_ADDRESS_IO_MASK;
	else
		return addr & PCI_BASE_ADDRESS_MEM_MASK;
}

void dm_pci_write_bar32(struct udevice *dev, int barnum, u32 addr)
{
	int bar;

	bar = PCI_BASE_ADDRESS_0 + barnum * 4;
	dm_pci_write_config32(dev, bar, addr);
}

static int _dm_pci_bus_to_phys(struct udevice *ctlr,
			       pci_addr_t bus_addr, unsigned long flags,
			       unsigned long skip_mask, phys_addr_t *pa)
{
	struct pci_controller *hose = dev_get_uclass_priv(ctlr);
	struct pci_region *res;
	int i;

	for (i = 0; i < hose->region_count; i++) {
		res = &hose->regions[i];

		if (((res->flags ^ flags) & PCI_REGION_TYPE) != 0)
			continue;

		if (res->flags & skip_mask)
			continue;

		if (bus_addr >= res->bus_start &&
		    (bus_addr - res->bus_start) < res->size) {
			*pa = (bus_addr - res->bus_start + res->phys_start);
			return 0;
		}
	}

	return 1;
}

phys_addr_t dm_pci_bus_to_phys(struct udevice *dev, pci_addr_t bus_addr,
			       unsigned long flags)
{
	phys_addr_t phys_addr = 0;
	struct udevice *ctlr;
	int ret;

	/* The root controller has the region information */
	ctlr = pci_get_controller(dev);

	/*
	 * if PCI_REGION_MEM is set we do a two pass search with preference
	 * on matches that don't have PCI_REGION_SYS_MEMORY set
	 */
	if ((flags & PCI_REGION_TYPE) == PCI_REGION_MEM) {
		ret = _dm_pci_bus_to_phys(ctlr, bus_addr,
					  flags, PCI_REGION_SYS_MEMORY,
					  &phys_addr);
		if (!ret)
			return phys_addr;
	}

	ret = _dm_pci_bus_to_phys(ctlr, bus_addr, flags, 0, &phys_addr);

	if (ret)
		puts("pci_hose_bus_to_phys: invalid physical address\n");

	return phys_addr;
}

int _dm_pci_phys_to_bus(struct udevice *dev, phys_addr_t phys_addr,
			unsigned long flags, unsigned long skip_mask,
			pci_addr_t *ba)
{
	struct pci_region *res;
	struct udevice *ctlr;
	pci_addr_t bus_addr;
	int i;
	struct pci_controller *hose;

	/* The root controller has the region information */
	ctlr = pci_get_controller(dev);
	hose = dev_get_uclass_priv(ctlr);

	for (i = 0; i < hose->region_count; i++) {
		res = &hose->regions[i];

		if (((res->flags ^ flags) & PCI_REGION_TYPE) != 0)
			continue;

		if (res->flags & skip_mask)
			continue;

		bus_addr = phys_addr - res->phys_start + res->bus_start;

		if (bus_addr >= res->bus_start &&
		    (bus_addr - res->bus_start) < res->size) {
			*ba = bus_addr;
			return 0;
		}
	}

	return 1;
}

pci_addr_t dm_pci_phys_to_bus(struct udevice *dev, phys_addr_t phys_addr,
			      unsigned long flags)
{
	pci_addr_t bus_addr = 0;
	int ret;

	/*
	 * if PCI_REGION_MEM is set we do a two pass search with preference
	 * on matches that don't have PCI_REGION_SYS_MEMORY set
	 */
	if ((flags & PCI_REGION_TYPE) == PCI_REGION_MEM) {
		ret = _dm_pci_phys_to_bus(dev, phys_addr, flags,
					  PCI_REGION_SYS_MEMORY, &bus_addr);
		if (!ret)
			return bus_addr;
	}

	ret = _dm_pci_phys_to_bus(dev, phys_addr, flags, 0, &bus_addr);

	if (ret)
		puts("pci_hose_phys_to_bus: invalid physical address\n");

	return bus_addr;
}

int dm_pci_ea_bar_read(struct udevice *dev, int bar,
		       pci_addr_t *start, size_t *size);

void *dm_pci_map_bar(struct udevice *dev, int bar, size_t *size, int flags)
{
	int pos;
	pci_addr_t pci_bus_start;
	u32 bar_response;
	struct pci_child_platdata *pdata = dev_get_parent_platdata(dev);

	if (!pdata->is_phys) {
		if (bar < 9 || bar > 14)
			return NULL;
		dev = pdata->pdev;
	}

	pos = dm_pci_find_capability(dev, PCI_CAP_ID_EA);

	if (pos) {
		dm_pci_ea_bar_read(dev, bar, &pci_bus_start, size);
	} else {
		/* read BAR address */
		if (bar >= 0 && bar <= 5) {
			bar = PCI_BASE_ADDRESS_0 + bar * 4;
		} else if (bar >= 9 && bar <= 14) {
			pos = dm_pci_find_ext_capability(dev, PCI_EXT_CAP_ID_SRIOV);
			bar = pos + PCI_SRIOV_BAR + bar * 4;
			//TODO: Get BAR size
		}
		dm_pci_read_config32(dev, bar,
				     &bar_response);
		pci_bus_start = (pci_addr_t)(bar_response & ~0xf);

		if ((bar_response & PCI_BASE_ADDRESS_MEM_TYPE_MASK) ==
				PCI_BASE_ADDRESS_MEM_TYPE_64) {
			dm_pci_read_config32(dev, bar + 4, &bar_response);
		}
		pci_bus_start |= (pci_addr_t)bar_response << 32;
	}

	if (!pdata->is_phys) {
		pci_bus_start += (pdata->vf_id - 1) * (*size);
	}

	/*
	 * Pass "0" as the length argument to pci_bus_to_virt.  The arg
	 * isn't actualy used on any platform because u-boot assumes a static
	 * linear mapping.  In the future, this could read the BAR size
	 * and pass that as the size if needed.
	 */
	return dm_pci_bus_to_virt(dev, pci_bus_start, flags, 0, MAP_NOCACHE);
}

/* Find the header pointer to the Capabilities*/
int dm_pci_find_cap_start(struct udevice *dev, u8 hdr_type)
{
	u16 status;

	dm_pci_read_config16(dev, PCI_STATUS, &status);

	if (!(status & PCI_STATUS_CAP_LIST))
		return 0;

	switch (hdr_type) {
	case PCI_HEADER_TYPE_NORMAL:
	case PCI_HEADER_TYPE_BRIDGE:
		return PCI_CAPABILITY_LIST;
	case PCI_HEADER_TYPE_CARDBUS:
		return PCI_CB_CAPABILITY_LIST;
	default:
		return 0;
	}
}

int dm_pci_find_cap(struct udevice *dev, int pos, int cap)
{
	int ttl = PCI_FIND_CAP_TTL;
	u8 id;
	u8 next_pos;

	while (ttl--) {
		dm_pci_read_config8(dev, pos, &next_pos);
		if (next_pos < CAP_START_POS)
			break;
		next_pos &= ~3;
		pos = (int) next_pos;
		dm_pci_read_config8(dev, pos + PCI_CAP_LIST_ID, &id);
		if (id == 0xff)
			break;
		if (id == cap)
			return pos;
		pos += PCI_CAP_LIST_NEXT;
	}
	return 0;
}

/* Returns the address of the requested capability structure within the
 * device's PCI configuration space or 0 in case the device does not
 * support it.
 * */
int dm_pci_find_capability(struct udevice *dev, int cap)
{
	int pos;
	u8 hdr_type;

	dm_pci_read_config8(dev, PCI_HEADER_TYPE, &hdr_type);

	pos = dm_pci_find_cap_start(dev, hdr_type & 0x7F);

	if (pos)
		pos = dm_pci_find_cap(dev, pos, cap);

	return pos;
}

/* Read an Enhanced Allocation (EA) entry */
static int dm_pci_ea_entry_read(struct udevice *dev, int offset, int *bei, pci_addr_t *start, size_t *size)
{
	u32 base;
	u32 max_offset;
	u8  prop;
	int ent_offset = offset;
	int ent_size;
	u32 dw0;

	dm_pci_read_config32(dev, ent_offset, &dw0);

	debug("%s: %d: dw0: %lx\n", __FUNCTION__, __LINE__, (unsigned long)dw0);

	ent_offset += sizeof(u32);

	/* Entry size field indicates DWORDs after 1st */
	ent_size = ((dw0 & PCI_EA_ES) + 1) * sizeof(u32);

	if (!(dw0 & PCI_EA_ENABLE))
		goto out;
	*bei = PCI_EA_BEI(dw0);

	prop = PCI_EA_PP(dw0);

	debug("EA property: %x\n", prop);

	/*
	* If the Property is in the reserved range, try the Secondary
	* Property instead.
	*/
	if (prop > PCI_EA_P_BRIDGE_IO && prop < PCI_EA_P_MEM_RESERVED)
		prop = PCI_EA_SP(dw0);
	if (prop > PCI_EA_P_BRIDGE_IO)
		goto out;

	debug("EA property: %x\n", prop);

	/* Read Base */
	dm_pci_read_config32(dev, ent_offset, &base);
	ent_offset += sizeof(u32);
	*start = (pci_addr_t)base & PCI_EA_FIELD_MASK;

	/* Read MaxOffset */
	dm_pci_read_config32(dev, ent_offset, &max_offset);
	ent_offset += sizeof(u32);

	/* Read Base MSBs (if 64-bit entry) */
	if (base & PCI_EA_IS_64) {
		dm_pci_read_config32(dev, ent_offset, &base);
		ent_offset += sizeof(u32);

		*start |= (pci_addr_t)base << 32;
	}

	debug("EA (%u,%u) start = %lx\n", PCI_EA_BEI(dw0), prop, (unsigned long)*start);

	*size = ((size_t)max_offset | 0x03) + 1;

	/* Read MaxOffset MSBs (if 64-bit entry) */
	if (max_offset & PCI_EA_IS_64) {
		dm_pci_read_config32(dev, ent_offset, &max_offset);
		ent_offset += sizeof(u32);

		*size |= (size_t)max_offset << 32;
	}

	debug("EA (%u,%u) size = %lx\n", PCI_EA_BEI(dw0), prop, (unsigned long)*size);

	if (*start + *size < *start) {
		*size = 0;
		*start = 0;
		printf("EA Entry crosses address boundary\n");
		goto out;
	}

	if (ent_size != ent_offset - offset) {
		printf("EA Entry Size (%d) does not match length read (%d)\n",
			ent_size, ent_offset - offset);
		goto out;
	}

out:
	return offset + ent_size;
}

/* Read an Enhanced Allocation (EA) BAR */
int dm_pci_ea_bar_read(struct udevice *dev, int bar, pci_addr_t *start, size_t *size)
{
	int ea;
	int offset;
	u8  num_ent;
	u8  hdr_type;
	int i, bei = -1;

	ea = dm_pci_find_capability(dev, PCI_CAP_ID_EA);

	dm_pci_read_config8(dev, ea + PCI_EA_NUM_ENT, &num_ent);
	num_ent &= PCI_EA_NUM_ENT_MASK;

	offset = ea + PCI_EA_FIRST_ENT;

	dm_pci_read_config8(dev, PCI_HEADER_TYPE, &hdr_type);

	/* Skip DWORD 2 for type 1 functions */
	if (hdr_type == PCI_HEADER_TYPE_BRIDGE)
		offset += sizeof(u32);

	for (i = 0; (i < num_ent) && (bar != bei); i++) {
		offset = dm_pci_ea_entry_read(dev, offset, &bei, start, size);
	}

	return (bar == bei);
}

/**
 * dm_pci_find_next_ext_capability - Find an extended capability
 *
 * Returns the address of the next matching extended capability structure
 * within the device's PCI configuration space or 0 if the device does
 * not support it.  Some capabilities can occur several times, e.g., the
 * vendor-specific capability, and this provides a way to find them all.
 */
int dm_pci_find_next_ext_capability(struct udevice *dev,
				    int start, int cap)
{
	u32 header;
	int ttl, pos = PCI_CFG_SPACE_SIZE;

	/* minimum 8 bytes per capability */
	ttl = (PCI_CFG_SPACE_EXP_SIZE - PCI_CFG_SPACE_SIZE) / 8;

	if (start)
		pos = start;

	dm_pci_read_config32(dev, pos, &header);
	if (header == 0xffffffff || header == 0)
		return 0;

	while (ttl-- > 0) {
		if (PCI_EXT_CAP_ID(header) == cap && pos != start)
			return pos;

		pos = PCI_EXT_CAP_NEXT(header);
		if (pos < PCI_CFG_SPACE_SIZE)
			break;

		dm_pci_read_config32(dev, pos, &header);
		if (header == 0xffffffff || header == 0)
			break;
	}

	return 0;
}

/**
 * dm_pci_find_ext_capability - Find an extended capability
 *
 * Returns the address of the requested extended capability structure
 * within the device's PCI configuration space or 0 if the device does
 * not support it.
 */
int dm_pci_find_ext_capability(struct udevice *dev, int cap)
{
	return dm_pci_find_next_ext_capability(dev, 0, cap);
}

/**
 * pci_bus_find_next_ext_capability - Find an extended capability
 *
 * Returns the address of the next matching extended capability structure
 * within the device's PCI configuration space or 0 if the device does
 * not support it.  Some capabilities can occur several times, e.g., the
 * vendor-specific capability, and this provides a way to find them all.
 */
static int pci_bus_find_next_ext_capability(struct udevice *bus,
					    pci_dev_t bdf, int start, int cap)
{
	ulong header;
	int ttl, pos = PCI_CFG_SPACE_SIZE;

	/* minimum 8 bytes per capability */
	ttl = (PCI_CFG_SPACE_EXP_SIZE - PCI_CFG_SPACE_SIZE) / 8;

	if (start)
		pos = start;

	pci_bus_read_config(bus, bdf, pos, &header, PCI_SIZE_32);
	if (header == 0xffffffff || header == 0)
		return 0;

	while (ttl-- > 0) {
		if (PCI_EXT_CAP_ID(header) == cap && pos != start)
			return pos;

		pos = PCI_EXT_CAP_NEXT(header);
		if (pos < PCI_CFG_SPACE_SIZE)
			break;

		pci_bus_read_config(bus, bdf, pos, &header, PCI_SIZE_32);
		if (header == 0xffffffff || header == 0)
			break;
	}

	return 0;
}

/**
 * pci_find_ext_capability - Find an extended capability
 *
 * Returns the address of the requested extended capability structure
 * within the device's PCI configuration space or 0 if the device does
 * not support it.
 */
int pci_bus_find_ext_capability(struct udevice *bus, pci_dev_t bdf, int cap)
{
	return pci_bus_find_next_ext_capability(bus, bdf, 0, cap);
}

UCLASS_DRIVER(pci) = {
	.id		= UCLASS_PCI,
	.name		= "pci",
	.flags		= DM_UC_FLAG_SEQ_ALIAS,
	.post_bind	= dm_scan_fdt_dev,
	.pre_probe	= pci_uclass_pre_probe,
	.post_probe	= pci_uclass_post_probe,
	.child_post_bind = pci_uclass_child_post_bind,
	.per_device_auto_alloc_size = sizeof(struct pci_controller),
	.per_child_platdata_auto_alloc_size =
			sizeof(struct pci_child_platdata),
};

static const struct dm_pci_ops pci_bridge_ops = {
	.read_config	= pci_bridge_read_config,
	.write_config	= pci_bridge_write_config,
};

static const struct udevice_id pci_bridge_ids[] = {
	{ .compatible = "pci-bridge" },
	{ }
};

U_BOOT_DRIVER(pci_bridge_drv) = {
	.name		= "pci_bridge_drv",
	.id		= UCLASS_PCI,
	.of_match	= pci_bridge_ids,
	.ops		= &pci_bridge_ops,
};

UCLASS_DRIVER(pci_generic) = {
	.id		= UCLASS_PCI_GENERIC,
	.name		= "pci_generic",
};

static const struct udevice_id pci_generic_ids[] = {
	{ .compatible = "pci-generic" },
	{ }
};

U_BOOT_DRIVER(pci_generic_drv) = {
	.name		= "pci_generic_drv",
	.id		= UCLASS_PCI_GENERIC,
	.of_match	= pci_generic_ids,
};

void pci_init(void)
{
	struct udevice *bus;

	/*
	 * Enumerate all known controller devices. Enumeration has the side-
	 * effect of probing them, so PCIe devices will be enumerated too.
	 */
	for (uclass_first_device(UCLASS_PCI, &bus);
	     bus;
	     uclass_next_device(&bus)) {
		;
	}
}

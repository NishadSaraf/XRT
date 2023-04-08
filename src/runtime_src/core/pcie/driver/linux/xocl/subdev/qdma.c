/*
 * A GEM style device manager for PCIe based OpenCL accelerators.
 *
 * Copyright (C) 2020-2022 Xilinx, Inc. All rights reserved.
 *
 * Authors: Lizhi.Hou@Xilinx.com
 *          Jan Stephan <j.stephan@hzdr.de>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/dma/amd_qdma.h>
#include <linux/dmaengine.h>
#include <linux/eventfd.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/platform_data/amd_qdma.h>
#include <linux/platform_device.h>

#include "../xocl_drv.h"
#include "../xocl_drm.h"

#define QDMA_MAX_USER_INTR			16
#define QDMA_REQ_TIMEOUT_MS			10000
#define QDMA_MAX_MM_CHANNELS			1
#define QDMA_HW_MAX_MM_CHANNELS			16

#define AMD_PCIE_READ_RQ_SIZE			512
#define AMD_PCIE_QDMA_ADDR_CAP_SIZE		64 /* bits */
#define AMD_PCIE_QDMA_BAR_IDENTIFIER		0x1FD3
#define AMD_PCIE_QDMA_BAR_IDENTIFIER_REGOFF	0x0
#define AMD_PCIE_QDMA_BAR_IDENTIFIER_MASK	GENMASK(31, 16)

#define AMD_QDMA_DEVICE_NAME			"amd-qdma"
#define AMD_QDMA_DEV_NUM_RES			2U

/* Module Parameters */
unsigned int qdma_max_channel = QDMA_MAX_MM_CHANNELS;
module_param(qdma_max_channel, uint, 0644);
MODULE_PARM_DESC(qdma_max_channel, "Set number of channels for qdma, default is 8");

struct dentry *qdma_debugfs_root;

struct qdma_irq {
	struct eventfd_ctx	*event_ctx;
	bool			in_use;
	bool			enabled;
	irq_handler_t		handler;
	void			*arg;
	u32			user_irq;
};

struct qdma_channel {
	struct dma_chan		*chan;
	dma_cookie_t		dma_cookie;
	u64			usage;
	void			*dma_hdl;
	struct completion	req_compl;
};

struct xocl_qdma {
	unsigned long 		dma_hndl;

	struct platform_device	*pdev;
	struct platform_device	*dma_pdev;

	/* Number of bidirectional channels */
	u32			channel;
	/*
	 * Semaphore, one for each direction.
	 * 0 index implies C2H, 1 implies H2C
	 */
	struct semaphore	channel_sem[2];
	/*
	 * Channel usage bitmasks, one for each direction
	 * bit 1 indicates channel is free, bit 0 indicates channel is free
	 */
	volatile unsigned long	channel_bitmap[2];
	struct qdma_channel	chans[2][QDMA_HW_MAX_MM_CHANNELS];
	struct mutex		str_dev_lock;

	u16			instance;

	struct qdma_irq		*user_msix_table;
	u32			user_msix_mask;
	spinlock_t		user_msix_table_lock;
};

static u32 get_channel_count(struct platform_device *pdev);
static u64 get_channel_stat(struct platform_device *pdev, u32 channel,
	u32 write);

static void dump_sgtable(struct device *dev, struct sg_table *sgt)
{
	int i;
	struct page *pg;
	struct scatterlist *sg = sgt->sgl;
	unsigned long long pgaddr;
	int nents = sgt->orig_nents;

	for (i = 0; i < nents; i++, sg = sg_next(sg)) {
		if (!sg)
			break;
		pg = sg_page(sg);
		if (!pg)
			continue;
		pgaddr = page_to_phys(pg);
		xocl_err(dev, "%i, 0x%llx, offset %d, len %d\n",
			i, pgaddr, sg->offset, sg->length);
	}
}

/* sysfs */
static ssize_t error_show(struct device *dev, struct device_attribute *da,
	char *buf)
{
#if 0
	struct platform_device *pdev = to_platform_device(dev);
	struct xocl_qdma *qdma;

	qdma = platform_get_drvdata(pdev);

	return qdma_device_error_stat_dump(qdma->dma_hndl, buf, 0);
#else
	return 0;
#endif
}
static DEVICE_ATTR_RO(error);

static ssize_t channel_stat_raw_show(struct device *dev,
		        struct device_attribute *attr, char *buf)
{
	ssize_t nbytes = 0;
	u32 i;
	struct platform_device *pdev = to_platform_device(dev);
	u32 chs = get_channel_count(pdev);

	for (i = 0; i < chs; i++) {
		nbytes += sprintf(buf + nbytes, "%llu %llu\n",
		get_channel_stat(pdev, i, 0),
		get_channel_stat(pdev, i, 1));
	}
	return nbytes;
}
static DEVICE_ATTR_RO(channel_stat_raw);

static struct attribute *qdma_attributes[] = {
	&dev_attr_error.attr,
	&dev_attr_channel_stat_raw.attr,
	NULL,
};

static const struct attribute_group qdma_attrgroup = {
	.attrs = qdma_attributes,
};
/* end of sysfs */

static void qdma_chan_irq(void *param)
{
	struct qdma_channel *chan = param;

	complete(&chan->req_compl);
}

static void qdma_print_sg_buf(struct platform_device *pdev,
			      struct sg_table *sgt, u64 len)
{
	struct scatterlist *sg;
	u32 i;

	for_each_sg(sgt->sgl, sg, sg_nents(sgt->sgl), i) {
		u32 j;
		char *vaddr = sg_virt(sg);

		for (j = 0; j < sg->length && len; j++, len--)
			xocl_info(&pdev->dev, "%c", vaddr[j]);
	}
}

static void qdma_zeroise_sg_buf(struct platform_device *pdev,
				struct sg_table *sgt, u64 len)
{
	struct scatterlist *sg;
	u32 i;

	for_each_sg(sgt->sgl, sg, sg_nents(sgt->sgl), i) {
		u32 j;
		char *vaddr = sg_virt(sg);

		for (j = 0; j < sg->length && len; j++, len--)
			vaddr[j] = 0;
	}
}

static ssize_t qdma_migrate_bo(struct platform_device *pdev,
			       struct sg_table *sgt, u32 dir, u64 paddr,
			       u32 channel, u64 len)
{
	struct dma_async_tx_descriptor *tx;
	enum dma_data_direction dma_dir;
	struct dma_slave_config cfg;
	struct qdma_channel *chan;
	struct pci_dev *pci_dev;
	struct xocl_qdma *qdma;
	ssize_t ret;
	int nents;

	qdma = platform_get_drvdata(pdev);
	chan = &qdma->chans[dir][channel];
	if (dir) {
		cfg.direction = DMA_MEM_TO_DEV;
		cfg.dst_addr = paddr;
		dma_dir = DMA_TO_DEVICE;
	} else {
		cfg.direction = DMA_DEV_TO_MEM;
		cfg.src_addr = paddr;
		dma_dir = DMA_FROM_DEVICE;
		qdma_zeroise_sg_buf(pdev, sgt, len);
	}

	pci_dev = XDEV(xocl_get_xdev(pdev))->pdev;

	nents = pci_map_sg(pci_dev, sgt->sgl, sgt->orig_nents, dma_dir);
	if (!nents) {
		xocl_err(&pdev->dev, "Failed to map sg");
		return -EIO;
	}

	ret = dmaengine_slave_config(chan->chan, &cfg);
	if (ret) {
		xocl_err(&pdev->dev, "Failed to config DMA: %ld", ret);
		return ret;
	}

	tx = dmaengine_prep_slave_sg(chan->chan, sgt->sgl, nents, cfg.direction,
				     0);
	if (!tx) {
		xocl_err(&pdev->dev, "Failed to prep slave sg");
		pci_unmap_sg(pci_dev, sgt->sgl, sgt->orig_nents, dir);
		return -EIO;
	}

	tx->callback = qdma_chan_irq;
	tx->callback_param = chan;

	chan->dma_cookie = dmaengine_submit(tx);

	dma_async_issue_pending(chan->chan);

	ret = wait_for_completion_timeout(&chan->req_compl,
					  msecs_to_jiffies(QDMA_REQ_TIMEOUT_MS));
	if (!ret) {
		xocl_err(&pdev->dev, "DMA transaction timed out for %s%d",
			 dir ? "H2C": "C2H", channel);
		dump_sgtable(&pdev->dev, sgt);
//		qdma_print_sg_buf(pdev, sgt, len);
		ret = -EIO;
	} else {
		ret = len;
		chan->usage += len;
	}

	pci_unmap_sg(pci_dev, sgt->sgl, sgt->orig_nents, dir);

	return ret;
}

static void release_channel(struct platform_device *pdev, u32 dir, u32 channel)
{
	struct xocl_qdma *qdma = platform_get_drvdata(pdev);

	if (channel >= qdma->channel) {
		xocl_err(&pdev->dev, "Invalid channel ID");
		return;
	}

//	xocl_info(&pdev->dev, "Releasing %s%d DMA channel", dir ? "H2C" : "C2H",
//		  channel);

	set_bit(channel, &qdma->channel_bitmap[dir]);
	up(&qdma->channel_sem[dir]);
}

static void free_channel(struct platform_device *pdev, bool dir, u32 channel)
{
	struct xocl_qdma *qdma = platform_get_drvdata(pdev);

	//xocl_info(&pdev->dev, "Free %s%d channel", dir ? "H2C" : "C2H", channel);
	dma_release_channel(qdma->chans[dir][channel].chan);
}

static int acquire_channel(struct platform_device *pdev, u32 dir)
{
	struct xocl_qdma *qdma;
	u32 channel, id = 0;

	qdma = platform_get_drvdata(pdev);

	if (down_killable(&qdma->channel_sem[dir]))
		return -ERESTARTSYS;

	/* Acquire a free DMA channel */
	for (channel = 0; channel < qdma->channel; channel++) {
		id = test_and_clear_bit(channel, &qdma->channel_bitmap[dir]);
		if (id)
			break;
	}

	if (!id) {
		xocl_err(&pdev->dev, "No free DMA channel found");
		up(&qdma->channel_sem[dir]);
		return -EIO;
	}

//	xocl_info(&pdev->dev, "Acquired %s%d channel", dir ? "H2C" : "C2H",
//			channel);

	return channel;
}

static int reserve_channel(struct platform_device *pdev, bool dir, u32 channel)
{
	struct xocl_qdma *qdma;
	char name[16];

	qdma = platform_get_drvdata(pdev);

	if (dir)
		sprintf(name, "h2c%d", channel);
	else
		sprintf(name, "c2h%d", channel);

	qdma->chans[dir][channel].chan = dma_request_chan(&qdma->pdev->dev,
							  name);
	if (IS_ERR(qdma->chans[dir][channel].chan)) {
		xocl_err(&pdev->dev, "Failed to get %s DMA channel", name);
		set_bit(channel, &qdma->channel_bitmap[dir]);
		return -EBUSY;
	}
	qdma->chans[dir][channel].dma_hdl = qdma;
	init_completion(&qdma->chans[dir][channel].req_compl);

	//xocl_info(&pdev->dev, "Reserved %s%d channel", dir ? "H2C" : "C2H",
	//	  channel);

	return 0;
}

static u32 get_channel_count(struct platform_device *pdev)
{
	struct xocl_qdma *qdma  = platform_get_drvdata(pdev);

        BUG_ON(!qdma);

        return qdma->channel;
}

static u64 get_channel_stat(struct platform_device *pdev, u32 channel, u32 dir)
{
	struct xocl_qdma *qdma = platform_get_drvdata(pdev);

	BUG_ON(!qdma);

	return qdma->chans[dir][channel].usage;
}

static u64 get_str_stat(struct platform_device *pdev, u32 q_idx)
{
	struct xocl_qdma *qdma = platform_get_drvdata(pdev);

	BUG_ON(!qdma);

	return 0;
}

static irqreturn_t qdma_isr(int irq, void *arg)
{
	struct qdma_irq *irq_entry = arg;
	int ret = IRQ_HANDLED;

	if (irq_entry->handler)
		ret = irq_entry->handler(irq, irq_entry->arg);

	if (!IS_ERR_OR_NULL(irq_entry->event_ctx))
		eventfd_signal(irq_entry->event_ctx, 1);

	return ret;
}

static int user_intr_register(struct platform_device *pdev, u32 intr,
	irq_handler_t handler, void *arg, int event_fd)
{
	struct eventfd_ctx *trigger = ERR_PTR(-EINVAL);
	struct xocl_qdma *qdma;
	struct qdma_irq *irq;
	unsigned long flags;
	int ret;

	qdma = platform_get_drvdata(pdev);

	irq = &qdma->user_msix_table[intr];

	if (event_fd >= 0) {
		trigger = eventfd_ctx_fdget(event_fd);
		if (IS_ERR(trigger)) {
			xocl_err(&pdev->dev, "get event ctx failed");
			return -EFAULT;
		}
	}

	spin_lock_irqsave(&qdma->user_msix_table_lock, flags);
	if (irq->in_use) {
		xocl_err(&pdev->dev, "IRQ %d is in use", intr);
		ret = -EPERM;
		goto failed;
	}

	irq->event_ctx = trigger;
	irq->handler = handler;
	irq->arg = arg;
	irq->user_irq = qdma_get_user_irq(qdma->dma_pdev, intr);

	ret = request_irq(irq->user_irq, qdma_isr, 0, "xocl-qdma-user",
			  &qdma->user_msix_table[intr]);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register intr %d", intr);
		goto failed;
	}

	irq->in_use = true;

	spin_unlock_irqrestore(&qdma->user_msix_table_lock, flags);

	return 0;

failed:
	irq->handler = NULL;
	irq->arg = NULL;
	irq->event_ctx = NULL;
	spin_unlock_irqrestore(&qdma->user_msix_table_lock, flags);
	if (!IS_ERR(trigger))
		eventfd_ctx_put(trigger);

	return ret;
}

static int user_intr_unreg(struct platform_device *pdev, u32 intr)
{
	struct xocl_qdma *qdma;
	unsigned long flags;
	int ret = 0;

	qdma = platform_get_drvdata(pdev);

	spin_lock_irqsave(&qdma->user_msix_table_lock, flags);
	if (!qdma->user_msix_table[intr].in_use) {
		xocl_err(&pdev->dev, "intr %d is not in use", intr);
		ret = -EINVAL;
		goto failed;
	}

	free_irq(qdma->user_msix_table[intr].user_irq,
		 &qdma->user_msix_table[intr]);

	qdma->user_msix_table[intr].handler = NULL;
	qdma->user_msix_table[intr].arg = NULL;
	qdma->user_msix_table[intr].in_use = false;

failed:
	spin_unlock_irqrestore(&qdma->user_msix_table_lock, flags);
	return ret;
}

static int user_intr_config(struct platform_device *pdev, u32 intr, bool en)
{
	struct xocl_qdma *qdma;
	unsigned long flags;

	qdma = platform_get_drvdata(pdev);

	spin_lock_irqsave(&qdma->user_msix_table_lock, flags);

	qdma->user_msix_table[intr].enabled = en;

	spin_unlock_irqrestore(&qdma->user_msix_table_lock, flags);

	return 0;
}

static struct xocl_dma_funcs qdma_ops = {
	.migrate_bo = qdma_migrate_bo,
	.ac_chan = acquire_channel,
	.rel_chan = release_channel,
	.get_chan_count = get_channel_count,
	.get_chan_stat = get_channel_stat,
	.user_intr_config = user_intr_config,
	.user_intr_register = user_intr_register,
	.user_intr_unreg = user_intr_unreg,
	/* qdma */
	.get_str_stat = get_str_stat,
};

static int qdma_config_pci(struct pci_dev *pdev)
{
	u16 vid, did, val;
	int ret;

	pci_read_config_word(pdev, PCI_VENDOR_ID, &vid);
	pci_read_config_word(pdev, PCI_DEVICE_ID, &did);

	dev_info(&pdev->dev,
		 "AMD PCI test driver probed with VID: %#x, DID: %#x\n", vid,
		 did);

	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to enable PCI device\n");
		return ret;
	}

	/* Clear pending interrupt if needed */
	ret = pci_read_config_word(pdev, PCI_STATUS, &val);
	if (ret)
		return ret;

	if (val & PCI_STATUS_INTERRUPT) {
		dev_info(&pdev->dev, "PCI STATUS Interrupt pending 0x%x", val);
		ret = pci_write_config_word(pdev, PCI_STATUS,
					    PCI_STATUS_INTERRUPT);
		if (ret)
			return ret;
	}

	/* Enable relaxed ordering */
	pcie_capability_set_word(pdev, PCI_EXP_DEVCTL, PCI_EXP_DEVCTL_RELAX_EN);

	/* Enable extended tag */
	ret = pcie_capability_read_word(pdev, PCI_EXP_DEVCTL, &val);
	if (ret)
		return ret;

	if (!(val & PCI_EXP_DEVCTL_EXT_TAG)) {
		ret = pcie_capability_set_word(pdev, PCI_EXP_DEVCTL,
		                               PCI_EXP_DEVCTL_EXT_TAG);
		if (ret)
		        return ret;
	}

	/* PCIE read request size set to 512B */
	if (pcie_get_readrq(pdev) < AMD_PCIE_READ_RQ_SIZE) {
		ret = pcie_set_readrq(pdev, AMD_PCIE_READ_RQ_SIZE);
		if (ret)
			return ret;
	}

	/* Enable bus master capability */
	pci_set_master(pdev);

	/* Set DMA addressing capability */
	ret = dma_set_mask_and_coherent(&pdev->dev,
					DMA_BIT_MASK(AMD_PCIE_QDMA_ADDR_CAP_SIZE));
	if (ret) {
		dev_err(&pdev->dev, "Failed to set DMA addressing to %d-bits\n",
			AMD_PCIE_QDMA_ADDR_CAP_SIZE);
		return ret;
	}

	return 0;
}

static void qdma_clear_config_pci(struct pci_dev *pdev)
{
	/* Disable extended tag*/
	pcie_capability_clear_word(pdev, PCI_EXP_DEVCTL,
				   PCI_EXP_DEVCTL_EXT_TAG);
	/* Disable relaxed ordering */
	pcie_capability_clear_word(pdev, PCI_EXP_DEVCTL,
				   PCI_EXP_DEVCTL_RELAX_EN);
	pci_clear_master(pdev);
	pci_disable_device(pdev);
}

static struct qdma_queue_info h2c_queue_info = {
	.dir = DMA_MEM_TO_DEV,
};

static struct qdma_queue_info c2h_queue_info = {
	.dir = DMA_DEV_TO_MEM,
};

static int qdma_alloc_platform_device(struct xocl_qdma *qdma)
{
	struct resource res[AMD_QDMA_DEV_NUM_RES] = {0};
	struct platform_device *dev;
	struct dma_slave_map *map;
	struct qdma_platdata data;
	struct pci_dev *pdev;
	int ret = 0, nvec;
	u8 i, index = 0;

	pdev = XDEV(xocl_get_xdev(qdma->pdev))->pdev;

	/* Populate resources from BAR */
	for (i = PCI_STD_RESOURCES; i <= PCI_STD_RESOURCE_END; i++) {
		resource_size_t len;
		void __iomem *regs;
		u32 value;

		len = pci_resource_len(pdev, i);
		if (!len)
			continue;

		regs = pci_iomap(pdev, i, len);
		if (IS_ERR(regs)) {
			ret = PTR_ERR(regs);
			xocl_err(&pdev->dev,
				 "Failed to map bar %d with error %d\n",
				 i, ret);
			return ret;
		}

		/* Check if BAR is type is DMA */
		value = ioread32(regs + AMD_PCIE_QDMA_BAR_IDENTIFIER_REGOFF);
		value = FIELD_GET(AMD_PCIE_QDMA_BAR_IDENTIFIER_MASK, value);
		if (value == AMD_PCIE_QDMA_BAR_IDENTIFIER) {
			xocl_info(&pdev->dev,
				 "PCIe QDMA config bar found at index: %d", i);

			res[index].start = pci_resource_start(pdev, i);
			res[index].end = pci_resource_end(pdev, i);
			res[index].flags = IORESOURCE_MEM;
			res[index].parent = &pdev->resource[i];
			index++;
		}

		pci_iounmap(pdev, regs);

		xocl_info(&pdev->dev,
			  "Resource id: %d\tlen: %#llx\tstart: %#llx\tend: %#llx\n",
			  i, len, pci_resource_start(pdev, i),
			  pci_resource_end(pdev, i));
	}

	if (!index) {
		xocl_err(&pdev->dev, "Failed to find DMA device\n");
		return -ENODEV;
	}

	nvec = pci_msix_vec_count(pdev);
	if (nvec < 0) {
		xocl_err(&pdev->dev, "Failed to find MSI-X vectors\n");
		return nvec;
	}

	xocl_info(&pdev->dev, "%d MSI-X interrupt supported", nvec);

	ret = pci_alloc_irq_vectors(pdev, nvec, nvec, PCI_IRQ_MSIX);
	if (ret < 0) {
		xocl_err(&pdev->dev, "Failed to alloc IRQ vectors: %d", ret);
		return ret;
	}

	res[index].start = pci_irq_vector(pdev, 0);
	res[index].end = res[index].start + nvec - 1;
	res[index].flags = IORESOURCE_IRQ;
	index++;

	data.user_irqs = QDMA_MAX_USER_INTR;

	dev = platform_device_alloc(AMD_QDMA_DEVICE_NAME, PLATFORM_DEVID_AUTO);
	if (!dev) {
		xocl_err(&pdev->dev,
			"Failed to register QDMA platform device with error\n");
		pci_free_irq_vectors(pdev);
		return -ENOMEM;
	}

	ret = platform_device_add_resources(dev, res, index);
	if (ret) {
		xocl_err(&pdev->dev, "Failed to add resource\n");
		goto failed;
	}

	data.device_map = devm_kzalloc(&qdma->pdev->dev,
				       sizeof(struct dma_slave_map) *
				       qdma_max_channel * 2,
				       GFP_KERNEL);
	data.device_map_cnt = qdma_max_channel * 2;

	for (i = 0; i < qdma_max_channel; i++) {
		map = &data.device_map[i];
		map->devname = dev_name(&qdma->pdev->dev);
		map->slave = devm_kasprintf(&qdma->pdev->dev, GFP_KERNEL,
					    "h2c%d", i);
		if (!map->slave)
			goto failed;
		map->param = QDMA_FILTER_PARAM(&h2c_queue_info);

		map = &data.device_map[i + qdma_max_channel];
		map->devname = dev_name(&qdma->pdev->dev);
		map->slave = devm_kasprintf(&qdma->pdev->dev, GFP_KERNEL,
					    "c2h%d", i);
		if (!map->slave)
			goto failed;
		map->param = QDMA_FILTER_PARAM(&c2h_queue_info);
	}

	data.max_mm_channels = qdma_max_channel;

	ret = platform_device_add_data(dev, &data,
				       sizeof(struct qdma_platdata));
	if (ret) {
		xocl_err(&pdev->dev, "Failed to add device config data\n");
		goto failed;
	}

	dev->dev.parent = &pdev->dev;

	ret = platform_device_add(dev);
	if (ret) {
		xocl_err(&pdev->dev, "Failed to add platform device\n");
		goto failed;
	}

	qdma->dma_pdev = dev;
	return 0;
failed:
	pci_free_irq_vectors(pdev);
	platform_device_put(dev);
	return ret;
}

static int qdma_probe(struct platform_device *pdev)
{
	struct xocl_qdma *qdma = NULL;
	xdev_handle_t xdev;
	int ret = 0;
	u32 i, dir;

	xdev = xocl_get_xdev(pdev);
	BUG_ON(!xdev);

	qdma = xocl_drvinst_alloc(&pdev->dev, sizeof(*qdma));
	if (!qdma) {
		xocl_err(&pdev->dev, "alloc mm dev failed");
		ret = -ENOMEM;
		goto failed;
	}

	qdma->pdev = pdev;

	platform_set_drvdata(pdev, qdma);

	ret = qdma_config_pci(XDEV(xdev)->pdev);
	if (ret) {
		xocl_err(&pdev->dev, "Failed to configure PCIe: %d", ret);
		goto failed;
	}

	ret = qdma_alloc_platform_device(qdma);
	if (ret) {
		xocl_err(&pdev->dev,
			 "Failed to allocate QDMA platform device: %d", ret);
		goto failed;
	}

	qdma->channel = qdma_max_channel;

	for (i = 0; i < qdma->channel; i++) {
		for (dir = 0; dir < 2; dir++) {
			ret = reserve_channel(pdev, dir, i);
			if (ret < 0) {
				xocl_info(&pdev->dev,
					  "Failed to reserve DMA channel");
				goto failed;
			}

			sema_init(&qdma->channel_sem[dir], qdma->channel);
			qdma->channel_bitmap[dir] = BIT(qdma->channel) - 1;
		}
	}

	xocl_info(&pdev->dev, "Max MM DMA channels: %d", qdma->channel);

	qdma->user_msix_table = devm_kzalloc(&pdev->dev, QDMA_MAX_USER_INTR *
					     sizeof(struct qdma_irq),
					     GFP_KERNEL);
	if (!qdma->user_msix_table) {
		xocl_err(&pdev->dev, "user_msix_table allocation failed");
		ret = -ENOMEM;
		goto failed;
	}
	spin_lock_init(&qdma->user_msix_table_lock);

	ret = sysfs_create_group(&pdev->dev.kobj, &qdma_attrgroup);
	if (ret) {
		xocl_err(&pdev->dev, "Failed to create sysfs group: %d", ret);
		goto failed;
	}

	return 0;
failed:
	if (qdma) {
		platform_device_unregister(qdma->dma_pdev);
		qdma_clear_config_pci(XDEV(xocl_get_xdev(pdev))->pdev);
		xocl_drvinst_release(qdma, NULL);
	}

	platform_set_drvdata(pdev, NULL);

	return ret;
}

static int qdma_remove(struct platform_device *pdev)
{
	struct xocl_qdma *qdma = platform_get_drvdata(pdev);
	xdev_handle_t xdev;
	struct pci_dev *pcidev;
	struct qdma_irq *irq_entry;
	u32 i, dir;
	void *hdl;

	xdev = xocl_get_xdev(pdev);
	BUG_ON(!xdev);

	if (!qdma) {
		xocl_err(&pdev->dev, "Driver data is NULL");
		return -EINVAL;
	}

	pcidev = XDEV(xdev)->pdev;

	if (!dev_is_pci(&pcidev->dev)) {
		xocl_err(&pdev->dev, "Invalid PCIe device");
		return -EINVAL;
	}

	sysfs_remove_group(&pdev->dev.kobj, &qdma_attrgroup);

	for (i = 0; i < QDMA_MAX_USER_INTR; i++) {
		irq_entry = &qdma->user_msix_table[i];
		if (irq_entry->in_use) {
			if (irq_entry->enabled) {
				xocl_warn(&pdev->dev,
					"ERROR: Interrupt %d is still on", i);
				user_intr_config(pdev, i, false);
				user_intr_unreg(pdev, i);
			}

			if(!IS_ERR_OR_NULL(irq_entry->event_ctx))
				eventfd_ctx_put(irq_entry->event_ctx);
		}
	}

	for (i = 0; i < qdma->channel; i++) {
		for (dir = 0; dir < 2; dir++) {
			if (!test_bit(i, &qdma->channel_bitmap[dir]))
				release_channel(pdev, dir, i);
			free_channel(pdev, dir, i);
		}
	}

	platform_device_unregister(qdma->dma_pdev);
	pci_free_irq_vectors(pcidev);
	qdma_clear_config_pci(pcidev);
	xocl_drvinst_release(qdma, &hdl);
	xocl_drvinst_free(hdl);
	platform_set_drvdata(pdev, NULL);
	return 0;
}

struct xocl_drv_private qdma_priv = {
	.ops = &qdma_ops,
	.dev = -1,
};

static struct platform_device_id qdma_id_table[] = {
	{ XOCL_DEVNAME(XOCL_QDMA), (kernel_ulong_t)&qdma_priv },
	{ },
};

static struct platform_driver	qdma_driver = {
	.probe		= qdma_probe,
	.remove		= qdma_remove,
	.driver		= {
		.name = XOCL_DEVNAME(XOCL_QDMA),
	},
	.id_table	= qdma_id_table,
};

int __init xocl_init_qdma(void)
{
	return platform_driver_register(&qdma_driver);
}

void xocl_fini_qdma(void)
{
	return platform_driver_unregister(&qdma_driver);
}

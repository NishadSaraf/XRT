/*
 * A GEM style device manager for PCIe based OpenCL accelerators.
 *
 * Copyright (C) 2016-2018 Xilinx, Inc. All rights reserved.
 *
 * Authors:
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

/* XDMA version Memory Mapped DMA */

#include <linux/version.h>
#include <linux/eventfd.h>
#include <linux/dmaengine.h>
#include <linux/platform_data/amd_xdma.h>
#include <linux/dma/amd_xdma.h>
#include "../xocl_drv.h"
#include "../xocl_drm.h"

#ifndef VM_RESERVED
#define VM_RESERVED (VM_DONTEXPAND | VM_DONTDUMP)
#endif

#define XDMA_MAX_CHANNELS	4

struct xdma_channel {
	struct dma_chan		*chan;
	dma_cookie_t		dma_cookie;
	u64			usage;
	void			*dma_hdl;
	struct completion	req_compl;
};

struct xdma_irq {
	struct eventfd_ctx	*event_ctx;
	bool			in_use;
	bool			enabled;
	irq_handler_t		handler;
	void			*arg;
	u32			user_irq;
};

struct xocl_xdma {
	struct platform_device	*pdev;
	struct platform_device	*dma_dev;
	struct xdma_channel	h2c_chan[XDMA_MAX_CHANNELS];
	struct xdma_channel	c2h_chan[XDMA_MAX_CHANNELS];
	u32			chan_num;
	struct semaphore	channel_sem[2];
	/*
	 * Channel usage bitmasks, one for each direction
	 * bit 1 indicates channel is free, bit 0 indicates channel is free
	 */
	volatile unsigned long	channel_bitmap[2];

	struct mutex		stat_lock;

	u32			start_user_intr;
	struct xdma_irq		*user_msix_table;
	spinlock_t		user_msix_table_lock;
};

static void xdma_chan_irq(void *param)
{
	struct xdma_channel *chan = param;

	complete(&chan->req_compl);
}

static ssize_t xdma_migrate_bo(struct platform_device *pdev,
	struct sg_table *sgt, u32 dir, u64 paddr, u32 channel, u64 len)
{
	struct xocl_xdma *xdma;
	struct dma_slave_config cfg;
	struct xdma_channel *chan;
	struct dma_async_tx_descriptor *tx;
	struct pci_dev *pci_dev;
	enum dma_data_direction dma_dir;
	int nents;
	ssize_t ret;

	xdma = platform_get_drvdata(pdev);
	if (dir) {
		cfg.direction = DMA_MEM_TO_DEV;
		cfg.dst_addr = paddr;
		chan = &xdma->h2c_chan[channel];
		dma_dir = DMA_TO_DEVICE;
	} else {
		cfg.direction = DMA_DEV_TO_MEM;
		cfg.src_addr = paddr;
		chan = &xdma->c2h_chan[channel];
		dma_dir = DMA_FROM_DEVICE;
	}

	pci_dev = XDEV(xocl_get_xdev(pdev))->pdev;
	nents = pci_map_sg(pci_dev, sgt->sgl, sgt->orig_nents, dma_dir);
	if (!nents) {
		dev_err(&pdev->dev, "failed to map sg");
		return -EIO;
	}

	ret = dmaengine_slave_config(chan->chan, &cfg);
	if (ret) {
		dev_err(&pdev->dev, "failed to config dma: %ld", ret);
		return ret;
	}

	tx = dmaengine_prep_slave_sg(chan->chan, sgt->sgl, nents, cfg.direction, 0);
	if (!tx) {
		dev_err(&pdev->dev, "failed to prep slave sg");
		pci_unmap_sg(pci_dev, sgt->sgl, sgt->orig_nents, dir);
		return -EIO;
	}

	tx->callback = xdma_chan_irq;
	tx->callback_param = chan;

	chan->dma_cookie = dmaengine_submit(tx);

	dma_async_issue_pending(chan->chan);

	if (!wait_for_completion_timeout(&chan->req_compl,
					 msecs_to_jiffies(10000))) {
		dev_err(&pdev->dev, "dma timeout");
		ret = -EIO;
	} else {
		ret = len;
		chan->usage += len;
	}
	pci_unmap_sg(pci_dev, sgt->sgl, sgt->orig_nents, dir);

	return ret;
}

static int acquire_channel(struct platform_device *pdev, u32 dir)
{
	struct xocl_xdma *xdma;
	int channel = 0;
	int result = 0;

	xdma = platform_get_drvdata(pdev);
	if (down_killable(&xdma->channel_sem[dir])) {
		channel = -ERESTARTSYS;
		goto out;
	}

	for (channel = 0; channel < xdma->chan_num; channel++) {
		result = test_and_clear_bit(channel,
			&xdma->channel_bitmap[dir]);
		if (result)
			break;
        }
        if (!result) {
		// How is this possible?
		up(&xdma->channel_sem[dir]);
		channel = -EIO;
	}

out:
	return channel;
}

static void release_channel(struct platform_device *pdev, u32 dir, u32 channel)
{
	struct xocl_xdma *xdma;


	xdma = platform_get_drvdata(pdev);
        set_bit(channel, &xdma->channel_bitmap[dir]);
        up(&xdma->channel_sem[dir]);
}

static u32 get_channel_count(struct platform_device *pdev)
{
	struct xocl_xdma *xdma;

        xdma= platform_get_drvdata(pdev);
        BUG_ON(!xdma);

        return xdma->chan_num;
}

static u64 get_channel_stat(struct platform_device *pdev, u32 channel,
	u32 write)
{
	struct xocl_xdma *xdma;
	struct xdma_channel *chan;

        xdma= platform_get_drvdata(pdev);
        BUG_ON(!xdma);

	if (write)
		chan = &xdma->h2c_chan[channel];
	else
		chan = &xdma->c2h_chan[channel];

        return chan->usage;
}

static int user_intr_config(struct platform_device *pdev, u32 intr, bool en)
{
	struct xocl_xdma *xdma;
	unsigned long flags;
	int ret;

	xdma= platform_get_drvdata(pdev);

	spin_lock_irqsave(&xdma->user_msix_table_lock, flags);
	if (xdma->user_msix_table[intr].enabled == en) {
		ret = 0;
		goto end;
	}

	xdma->user_msix_table[intr].enabled = en;
end:
	spin_unlock_irqrestore(&xdma->user_msix_table_lock, flags);

	return ret;
}

static irqreturn_t xdma_isr(int irq, void *arg)
{
	struct xdma_irq *irq_entry = arg;
	int ret = IRQ_HANDLED;

	if (irq_entry->handler)
		ret = irq_entry->handler(irq, irq_entry->arg);

	if (!IS_ERR_OR_NULL(irq_entry->event_ctx)) {
		eventfd_signal(irq_entry->event_ctx, 1);
	}

	return ret;
}

static int user_intr_unreg(struct platform_device *pdev, u32 intr)
{
	struct xocl_xdma *xdma;
	unsigned long flags;
	int ret = 0;

	xdma= platform_get_drvdata(pdev);

	spin_lock_irqsave(&xdma->user_msix_table_lock, flags);
	if (!xdma->user_msix_table[intr].in_use) {
		xocl_err(&pdev->dev, "intr %d is not in use", intr);
		ret = -EINVAL;
		goto failed;
	}
	xdma_disable_user_irq(xdma->dma_dev, xdma->user_msix_table[intr].user_irq);
	free_irq(xdma->user_msix_table[intr].user_irq,
		 &xdma->user_msix_table[intr]);

	xdma->user_msix_table[intr].handler = NULL;
	xdma->user_msix_table[intr].arg = NULL;

	xdma->user_msix_table[intr].in_use = false;

failed:
	spin_unlock_irqrestore(&xdma->user_msix_table_lock, flags);
	return ret;

}

static int user_intr_register(struct platform_device *pdev, u32 intr,
	irq_handler_t handler, void *arg, int event_fd)
{
	struct xocl_xdma *xdma;
	struct eventfd_ctx *trigger = ERR_PTR(-EINVAL);
	unsigned long flags;
	int ret;

	xdma= platform_get_drvdata(pdev);

	if (event_fd >= 0) {
		trigger = eventfd_ctx_fdget(event_fd);
		if (IS_ERR(trigger)) {
			xocl_err(&pdev->dev, "get event ctx failed");
			return -EFAULT;
		}
	}

	spin_lock_irqsave(&xdma->user_msix_table_lock, flags);
	if (xdma->user_msix_table[intr].in_use) {
		xocl_err(&pdev->dev, "IRQ %d is in use", intr);
		ret = -EPERM;
		goto failed;
	}
	xdma->user_msix_table[intr].event_ctx = trigger;
	xdma->user_msix_table[intr].handler = handler;
	xdma->user_msix_table[intr].arg = arg;
	xdma->user_msix_table[intr].user_irq = xdma_get_user_irq(xdma->dma_dev, intr);

	ret = xdma_enable_user_irq(xdma->dma_dev, xdma->user_msix_table[intr].user_irq);
	if (ret) {
		dev_err(&pdev->dev, "failed to enble user intr %d", intr);
		goto failed;
	}

	ret = request_irq(xdma->user_msix_table[intr].user_irq, xdma_isr,
			  0, "xdma-user", &xdma->user_msix_table[intr]);
	if (ret) {
		dev_err(&pdev->dev, "failed to register intr %d", intr);
		goto failed;
	}

	xdma->user_msix_table[intr].in_use = true;
	spin_unlock_irqrestore(&xdma->user_msix_table_lock, flags);

	return 0;

failed:
	xdma->user_msix_table[intr].handler = NULL;
	xdma->user_msix_table[intr].arg = NULL;
	xdma->user_msix_table[intr].event_ctx = NULL;
	spin_unlock_irqrestore(&xdma->user_msix_table_lock, flags);
	if (!IS_ERR(trigger))
		eventfd_ctx_put(trigger);

	return ret;
}

static int xdma_config_pci(struct pci_dev *pdev)
{
	u16 val;
        int ret;

        /* clear pending interrupt if needed */
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

        /* enable relaxed ordering */
        ret = pcie_capability_set_word(pdev, PCI_EXP_DEVCTL,
                                       PCI_EXP_DEVCTL_RELAX_EN);
        if (ret)
                return ret;

        /* PCIE extended tag needs to be enabled */
        ret = pcie_capability_read_word(pdev, PCI_EXP_DEVCTL, &val);
        if (ret)
                return ret;

        if (!(val & PCI_EXP_DEVCTL_EXT_TAG)) {
                dev_info(&pdev->dev, "Enable ExtTag");
                ret = pcie_capability_set_word(pdev, PCI_EXP_DEVCTL,
                                               PCI_EXP_DEVCTL_EXT_TAG);
                if (ret)
                        return ret;
        }

        /* MRRS can not beyond 512 */
        ret = pcie_get_readrq(pdev);
        if (ret < 0)
                return ret;
        if (ret > 512) {
		ret = pcie_set_readrq(pdev, 512);
                if (ret) {
                        dev_err(&pdev->dev, "failed to set mrrs: %d", ret);
                        return ret;
                }
        }

        pci_set_master(pdev);
        ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
        if (ret) {
                dev_err(&pdev->dev, "failed to set dma mask: %d", ret);
                return ret;
        }

        return 0;
}

static struct xocl_dma_funcs xdma_ops = {
	.migrate_bo = xdma_migrate_bo,
	.ac_chan = acquire_channel,
	.rel_chan = release_channel,
	.get_chan_count = get_channel_count,
	.get_chan_stat = get_channel_stat,
	.user_intr_register = user_intr_register,
	.user_intr_config = user_intr_config,
	.user_intr_unreg = user_intr_unreg,
};

static ssize_t channel_stat_raw_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	u32 i;
	ssize_t nbytes = 0;
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

static struct attribute *xdma_attrs[] = {
	&dev_attr_channel_stat_raw.attr,
	NULL,
};

static struct attribute_group xdma_attr_group = {
	.attrs = xdma_attrs,
};

static struct xdma_chan_info h2c_chan_info = {
	.dir = DMA_MEM_TO_DEV,
};

static struct xdma_chan_info c2h_chan_info = {
	.dir = DMA_DEV_TO_MEM,
};

static void xdma_remove_dma_dev(struct xocl_xdma *xdma)
{
	struct pci_dev *pci_dev;

	pci_dev = XDEV(xocl_get_xdev(xdma->pdev))->pdev;
	//device_release_driver(&xdma->dma_dev->dev);
	platform_device_unregister(xdma->dma_dev);
	pci_free_irq_vectors(pci_dev);
}

static int xdma_create_dma_dev(struct xocl_xdma *xdma)
{
	struct xdma_platdata data;
	struct resource res[2] = { 0 };
	struct dma_slave_map *map;
	struct pci_dev *pdev;
	int i, ret, nvec;

	pdev = XDEV(xocl_get_xdev(xdma->pdev))->pdev;

	nvec = pci_msix_vec_count(pdev);
	ret = pci_alloc_irq_vectors(pdev, nvec, nvec, PCI_IRQ_MSIX);
	if (ret < 0) {
		xocl_err(&xdma->pdev->dev, "failed to alloc irq vectors: %d", ret);
		goto failed;
	}

	xdma->user_msix_table = devm_kzalloc(&pdev->dev, nvec * sizeof(struct xdma_irq), GFP_KERNEL);
	if (!xdma->user_msix_table) {
		xocl_err(&pdev->dev, "alloc user_msix_table failed");
		ret = -ENOMEM;
		goto failed;
	}
	spin_lock_init(&xdma->user_msix_table_lock);

	res[0].start = pci_resource_start(pdev, 2);
	res[0].end = pci_resource_end(pdev, 2);
	res[0].flags = IORESOURCE_MEM;
	res[0].parent = &pdev->resource[2];
	res[1].start = pci_irq_vector(pdev, 0);
	res[1].end = res[1].start + nvec - 1;
	res[1].flags = IORESOURCE_IRQ;

	data.device_map = devm_kzalloc(&xdma->pdev->dev,
				      sizeof(struct dma_slave_map) *
				      XDMA_MAX_CHANNELS * 2,
				      GFP_KERNEL);
	data.device_map_cnt = XDMA_MAX_CHANNELS * 2;

	for (i = 0; i < XDMA_MAX_CHANNELS; i++) {
		map = &data.device_map[i];
		map->devname = dev_name(&xdma->pdev->dev);
		map->slave = devm_kasprintf(&xdma->pdev->dev, GFP_KERNEL,
					    "h2c%d", i);
		if (!map->slave)
			goto failed;
		map->param = XDMA_FILTER_PARAM(&h2c_chan_info);

		map = &data.device_map[i + XDMA_MAX_CHANNELS];
		map->devname = dev_name(&xdma->pdev->dev);
		map->slave = devm_kasprintf(&xdma->pdev->dev, GFP_KERNEL,
					    "c2h%d", i);
		if (!map->slave)
			goto failed;
		map->param = XDMA_FILTER_PARAM(&c2h_chan_info);
	}

	data.max_dma_channels = XDMA_MAX_CHANNELS;

	xdma->dma_dev = platform_device_register_resndata(&pdev->dev, "xdma", PLATFORM_DEVID_AUTO, res, 2, &data, sizeof(data));
	if (!xdma->dma_dev) {
		xocl_err(&xdma->pdev->dev, "failed to alloc dma device");
		return -ENOMEM;
	}

	return 0;

failed:
	platform_device_put(xdma->dma_dev);

	return ret;
}

static int xdma_probe(struct platform_device *pdev)
{
	struct xocl_xdma	*xdma = NULL;
	int	i, ret = 0;
	xdev_handle_t		xdev;
	char name[16];

	xdev = xocl_get_xdev(pdev);
	BUG_ON(!xdev);

	xdma = devm_kzalloc(&pdev->dev, sizeof(*xdma), GFP_KERNEL);
	if (!xdma) {
		xocl_err(&pdev->dev, "alloc xdma dev failed");
		ret = -ENOMEM;
		goto failed;
	}
	xdma->pdev = pdev;

	platform_set_drvdata(pdev, xdma);

	ret = xdma_config_pci(XDEV(xdev)->pdev);
	if (ret) {
		xocl_err(&pdev->dev, "failed to config pci %d", ret);
		goto failed;
	}

	ret = xdma_create_dma_dev(xdma);
	if (ret)
		goto failed;

	for (i = 0; i < XDMA_MAX_CHANNELS; i++) {
		sprintf(name, "h2c%d", i);
		xdma->h2c_chan[i].chan = dma_request_chan(&xdma->pdev->dev, name);
		sprintf(name, "c2h%d", i);
		xdma->c2h_chan[i].chan = dma_request_chan(&xdma->pdev->dev, name);
		if (IS_ERR(xdma->h2c_chan[i].chan) || IS_ERR(xdma->c2h_chan[i].chan))
			break;
		xdma->h2c_chan[i].dma_hdl = xdma;
		xdma->c2h_chan[i].dma_hdl = xdma;
		init_completion(&xdma->h2c_chan[i].req_compl);
		init_completion(&xdma->c2h_chan[i].req_compl);
	}
	xdma->chan_num = i;
	if (!xdma->chan_num) {
		xocl_err(&pdev->dev, "failed to get dma channel");
		ret = -EINVAL;
		goto failed;
	}

	ret = sysfs_create_group(&pdev->dev.kobj, &xdma_attr_group);
	if (ret) {
		xocl_err(&pdev->dev, "create attrs failed: %d", ret);
		goto failed;
	}

	xocl_info(&pdev->dev, "created %d channels", xdma->chan_num);
	sema_init(&xdma->channel_sem[0], xdma->chan_num);
	sema_init(&xdma->channel_sem[1], xdma->chan_num);
	xdma->channel_bitmap[0] = BIT(xdma->chan_num);
	xdma->channel_bitmap[0]--;
	xdma->channel_bitmap[1] = xdma->channel_bitmap[0];

	return 0;

failed:
	if (xdma) {
		for (i = 0; i < XDMA_MAX_CHANNELS; i++) {
			if (!IS_ERR(xdma->h2c_chan[i].chan))
				dma_release_channel(xdma->h2c_chan[i].chan);
			if (!IS_ERR(xdma->c2h_chan[i].chan))
				dma_release_channel(xdma->c2h_chan[i].chan);
		}
		xdma_remove_dma_dev(xdma);
		devm_kfree(&pdev->dev, xdma);
	}

	platform_set_drvdata(pdev, NULL);

	return ret;
}

static int xdma_remove(struct platform_device *pdev)
{
	struct xocl_xdma *xdma = platform_get_drvdata(pdev);
	int i;

	if (!xdma) {
		xocl_err(&pdev->dev, "driver data is NULL");
		return -EINVAL;
	}

	sysfs_remove_group(&pdev->dev.kobj, &xdma_attr_group);

	for (i = 0; i < xdma->chan_num; i++) {
		dma_release_channel(xdma->h2c_chan[i].chan);
		dma_release_channel(xdma->c2h_chan[i].chan);
	}

	xdma_remove_dma_dev(xdma);

	platform_set_drvdata(pdev, NULL);

	return 0;
}

struct xocl_drv_private xdma_priv = {
	.ops = &xdma_ops,
};

static struct platform_device_id xdma_id_table[] = {
	{ XOCL_DEVNAME(XOCL_XDMA), (kernel_ulong_t)&xdma_priv },
	{ },
};

static struct platform_driver	xdma_driver = {
	.probe		= xdma_probe,
	.remove		= xdma_remove,
	.driver		= {
		.name = XOCL_DEVNAME(XOCL_XDMA),
	},
	.id_table	= xdma_id_table,
};

int __init xocl_init_xdma(void)
{
	return platform_driver_register(&xdma_driver);
}

void xocl_fini_xdma(void)
{
	return platform_driver_unregister(&xdma_driver);
}

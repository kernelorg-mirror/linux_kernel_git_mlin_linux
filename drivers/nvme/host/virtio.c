#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/scatterlist.h>
#include <linux/string_helpers.h>
#include <linux/idr.h>
#include <linux/blk-mq.h>
#include <linux/numa.h>
#include <linux/nvme.h>
#include <linux/blk-mq.h>
#include <linux/module.h>

#include "nvme.h"

#define VIRTIO_NVME_F_SEG_MAX   1       /* Indicates maximum # of segments */
#define VIRTIO_NVME_F_MQ        2       /* support more than one vq */
#define VIRTIO_ID_NVME         19

#define VQ_NAME_LEN 16

#define NVME_AQ_DEPTH           256

static unsigned int nvme_virtio_queue_depth;
module_param_named(queue_depth, nvme_virtio_queue_depth, uint, 0444);

static LIST_HEAD(dev_list);

struct nvme_virtio_dev;
struct nvme_virtio_queue {
	struct nvme_virtio_dev *dev;
	struct virtqueue *vq;
	spinlock_t lock;
	char name[VQ_NAME_LEN];
} ____cacheline_aligned_in_smp;

struct nvme_virtio_dev {
	struct virtio_device *vdev;
	wait_queue_head_t queue_wait;

	struct list_head node;
	struct nvme_queue **queues;
	struct blk_mq_tag_set tagset;
	struct blk_mq_tag_set admin_tagset;
	struct device *dev;

	int num_vqs;
	struct nvme_virtio_queue *vqs;
	unsigned int sg_elems;

	struct nvme_ctrl ctrl;
};

struct nvme_virtio_config {
	__u64   cap;
	__u32	vs;
	__u32	intms;
	__u32	intmc;
	__u32	cc;
	__u32	csts;
	__u32	nssr;
	__u32	aqa;
	__u64	asq;
	__u64	acq;
	__u32	cmbloc;
	__u32	cmbsz;
	
	/* The maximum number of segments (if VIRTIO_NVME_F_SEG_MAX) */
	__u32   seg_max;
	/* number of vqs, only available when VIRTIO_NVME_F_MQ is set */
	__u32 num_queues;
} __attribute__((packed));

struct nvme_virtio_resp {
	 __u32	result;
	__u16	cid;
	__u16	status;
};

struct nvme_virtio_req
{       
	struct request *req;
	struct nvme_command cmd;
	struct nvme_virtio_resp resp;
	struct scatterlist sg[];
};

static inline struct nvme_virtio_dev *to_nvme_virtio_dev(struct nvme_ctrl *ctrl)
{
	return container_of(ctrl, struct nvme_virtio_dev, ctrl);
}

static const struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_NVME, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static unsigned int features[] = {
	VIRTIO_NVME_F_SEG_MAX, VIRTIO_NVME_F_MQ,
};

static int nvme_virtio_wait_ready(struct nvme_virtio_dev *dev, u64 cap)
{
	struct virtio_device *vdev = dev->vdev;
	unsigned long timeout;
	u32 csts;

	timeout = ((NVME_CAP_TIMEOUT(cap) + 1) * HZ / 2) + jiffies;

	while (1) {
		virtio_cread(vdev, struct nvme_virtio_config, csts, &csts);
		if ((csts & NVME_CSTS_RDY) == NVME_CSTS_RDY)
			break;

		msleep(100);
		if (fatal_signal_pending(current))
			return -EINTR;
		if (time_after(jiffies, timeout)) {
			printk("Device not ready; aborting initialisation\n");
			return -ENODEV;
		}
	}

	return 0;
}

static int nvme_virtio_reg_read32(struct nvme_ctrl *ctrl, u32 off, u32 *val)
{
	struct nvme_virtio_dev *dev = to_nvme_virtio_dev(ctrl);
	struct virtio_device *vdev = dev->vdev;

	*val = virtio_cread32(vdev, off);

	return 0;
}

static int nvme_virtio_reg_read64(struct nvme_ctrl *ctrl, u32 off, u64 *val)
{
	struct nvme_virtio_dev *dev = to_nvme_virtio_dev(ctrl);
	struct virtio_device *vdev = dev->vdev;

	*val = virtio_cread64(vdev, off);

	return 0;
}

static int nvme_virtio_reg_write32(struct nvme_ctrl *ctrl, u32 off, u32 val)
{
	struct nvme_virtio_dev *dev = to_nvme_virtio_dev(ctrl);
	struct virtio_device *vdev = dev->vdev;

	virtio_cwrite32(vdev, off, val);

	return 0;
}

static bool nvme_virtio_io_incapable(struct nvme_ctrl *ctrl)
{
	//TODO
	return false;
}

static int nvme_virtio_reset_ctrl(struct nvme_ctrl *ctrl)
{
	//TODO
	return 0;
}

static void nvme_virtio_free_ctrl(struct nvme_ctrl *ctrl)
{
	struct nvme_virtio_dev *dev = to_nvme_virtio_dev(ctrl);

	put_device(dev->dev);
	if (dev->tagset.tags)
		blk_mq_free_tag_set(&dev->tagset);
	if (dev->ctrl.admin_q)
		blk_put_queue(dev->ctrl.admin_q);
	kfree(dev->queues);
	kfree(dev);
}

static const struct nvme_ctrl_ops nvme_virtio_ctrl_ops = {
	.reg_read32	= nvme_virtio_reg_read32,
	.reg_read64	= nvme_virtio_reg_read64,
	.reg_write32	= nvme_virtio_reg_write32,
	.io_incapable	= nvme_virtio_io_incapable,
	.reset_ctrl	= nvme_virtio_reset_ctrl,
	.free_ctrl	= nvme_virtio_free_ctrl,
};

static void nvme_virtio_admin_done(struct virtqueue *vq)
{
	struct nvme_virtio_dev *dev = vq->vdev->priv;
	struct nvme_virtio_req *vnr;
	int qid = vq->index;
	unsigned long flags;
	unsigned int len;

	spin_lock_irqsave(&dev->vqs[qid].lock, flags);
	do {
		virtqueue_disable_cb(vq);
		while ((vnr = virtqueue_get_buf(dev->vqs[qid].vq, &len)) != NULL)
			blk_mq_complete_request(vnr->req, 0);
		if (unlikely(virtqueue_is_broken(vq)))
			break;
	} while (!virtqueue_enable_cb(vq));

	spin_unlock_irqrestore(&dev->vqs[qid].lock, flags);
}

static void nvme_virtio_io_done(struct virtqueue *vq)
{
	struct nvme_virtio_dev *dev = vq->vdev->priv;
	int qid = vq->index;
	struct nvme_virtio_req *vnr;
	unsigned long flags;
	unsigned int len;
	bool bio_done = false;

	spin_lock_irqsave(&dev->vqs[qid].lock, flags);
	do {
		virtqueue_disable_cb(vq);
		while ((vnr = virtqueue_get_buf(dev->vqs[qid].vq, &len)) != NULL) {
			blk_mq_complete_request(vnr->req, 0);
			bio_done = true;
		}

		if (unlikely(virtqueue_is_broken(vq)))
			break;
	} while (!virtqueue_enable_cb(vq));

	spin_unlock_irqrestore(&dev->vqs[qid].lock, flags);

	if (bio_done)
		wake_up(&dev->queue_wait);
}

static int nvme_virtio_init_vq(struct nvme_virtio_dev *dev)
{
	int err = 0;
	int i;
	vq_callback_t **callbacks;
	const char **names;
	struct virtqueue **vqs;
	unsigned num_vqs;
	struct virtio_device *vdev = dev->vdev;

	err = virtio_cread_feature(vdev, VIRTIO_NVME_F_MQ,
				   struct nvme_virtio_config, num_queues,
				   &num_vqs);
	if (err)
		num_vqs = 1;

	num_vqs++;

	dev->vqs = kmalloc(sizeof(*dev->vqs) * num_vqs, GFP_KERNEL);
	if (!dev->vqs) {
		err = -ENOMEM;
		goto out;
	}

	names = kmalloc(sizeof(*names) * num_vqs, GFP_KERNEL);
	if (!names)
		goto err_names;

	callbacks = kmalloc(sizeof(*callbacks) * num_vqs, GFP_KERNEL);
	if (!callbacks)
		goto err_callbacks;

	vqs = kmalloc(sizeof(*vqs) * num_vqs, GFP_KERNEL);
	if (!vqs)
		goto err_vqs;

	callbacks[0] = nvme_virtio_admin_done;
	names[0] = "admin";
	dev->vqs[0].dev = dev;

	for (i = 1; i < num_vqs; i++) {
		callbacks[i] = nvme_virtio_io_done;
		snprintf(dev->vqs[i].name, VQ_NAME_LEN, "req.%d", i);
		names[i] = dev->vqs[i].name;
		dev->vqs[i].dev = dev;
	}

	/* Discover virtqueues and write information to configuration.  */
	err = vdev->config->find_vqs(vdev, num_vqs, vqs, callbacks, names);
	if (err)
		goto err_find_vqs;

	for (i = 0; i < num_vqs; i++) {
		spin_lock_init(&dev->vqs[i].lock);
		dev->vqs[i].vq = vqs[i];
	}
	dev->num_vqs = num_vqs;

err_find_vqs:
	kfree(vqs);
err_vqs:
	kfree(callbacks);
err_callbacks:
	kfree(names);
err_names:
	if (err)
		kfree(dev->vqs);
out:
	return err;
}

static inline struct nvme_virtio_req *nvme_virtio_alloc_req(struct nvme_virtio_dev *dev,
		gfp_t gfp_mask)
{
	struct nvme_virtio_req *vnr;

	vnr = kmalloc(sizeof(*vnr) + dev->sg_elems*sizeof(struct scatterlist),
			gfp_mask);
	if (!vnr)
		return NULL;

	sg_init_table(vnr->sg, dev->sg_elems);

	return vnr;
}

static inline u64 nvme_virtio_block_nr(struct nvme_ns *ns, sector_t sector)
{
        return (sector >> (ns->lba_shift - 9));
}

static int nvme_virtio_add_req(struct nvme_ns *ns, struct virtqueue *vq,
			     struct nvme_virtio_req *vnr,
			     struct scatterlist *data_sg,
			     bool have_data)
{
	struct scatterlist cmd, resp, *sgs[5];
	unsigned int num_out = 0, num_in = 0;

	sg_init_one(&cmd, vnr->req->cmd, sizeof(struct nvme_command));
	sgs[num_out++] = &cmd;

	if (have_data) {
		if (rq_data_dir(vnr->req))
			sgs[num_out++] = data_sg;
		else
			sgs[num_out + num_in++] = data_sg;
	}

	sg_init_one(&resp, &vnr->resp, sizeof(struct nvme_virtio_resp));
	sgs[num_out + num_in++] = &resp;

	return virtqueue_add_sgs(vq, sgs, num_out, num_in, vnr, GFP_ATOMIC);
}

static int nvme_virtio_setup_io(struct nvme_virtio_req *vnr, struct nvme_ns *ns)
{
	struct nvme_command *cmnd;
	struct request *req = vnr->req;
	u16 control = 0;
	u32 dsmgmt = 0;

#if 0 /* TODO */
	if (req->cmd_flags & REQ_FUA)
		control |= NVME_RW_FUA;
	if (req->cmd_flags & (REQ_FAILFAST_DEV | REQ_RAHEAD))
		control |= NVME_RW_LR;

	if (req->cmd_flags & REQ_RAHEAD)
		dsmgmt |= NVME_RW_DSM_FREQ_PREFETCH;
#endif

	cmnd = &vnr->cmd;
	req->cmd = (unsigned char *)cmnd;
	req->cmd_len = sizeof(struct nvme_command);
	memset(cmnd, 0, sizeof(*cmnd));

	cmnd->rw.opcode = (rq_data_dir(req) ? nvme_cmd_write : nvme_cmd_read);
	cmnd->rw.command_id = req->tag;
	cmnd->rw.nsid = cpu_to_le32(ns->ns_id);
	cmnd->rw.slba = cpu_to_le64(nvme_virtio_block_nr(ns, blk_rq_pos(req)));
	cmnd->rw.length = cpu_to_le16((blk_rq_bytes(req) >> ns->lba_shift) - 1);
	cmnd->rw.control = cpu_to_le16(control);
	cmnd->rw.dsmgmt = cpu_to_le32(dsmgmt);

	return 0;
}

static int nvme_virtio_queue_rq(struct blk_mq_hw_ctx *hctx,
		const struct blk_mq_queue_data *bd)
{
	struct nvme_ns *ns = hctx->queue->queuedata;
	struct nvme_virtio_queue *nvmeq = hctx->driver_data;
	struct request *req = bd->rq;
	struct nvme_virtio_req *vnr = blk_mq_rq_to_pdu(req);
	unsigned long flags;
	unsigned int num;
	int err;
	bool notify = false;

	vnr->req = req;

	if (req->cmd_type == REQ_TYPE_DRV_PRIV)
		; /* TODO: nvme_submit_priv(nvmeq, req, iod) */
	else if (req->cmd_flags & REQ_DISCARD)
		; /* TODO: nvme_submit_discard(nvmeq, ns, req, iod) */
	else if (req->cmd_flags & REQ_FLUSH)
		; /* TODO: nvme_submit_flush(nvmeq, ns, req->tag) */
	else
		nvme_virtio_setup_io(vnr, ns);

	blk_mq_start_request(req);

	num = blk_rq_map_sg(hctx->queue, vnr->req, vnr->sg);

	spin_lock_irqsave(&nvmeq->lock, flags);
	err = nvme_virtio_add_req(ns, nvmeq->vq, vnr, vnr->sg, num);
	if (err) {
		virtqueue_kick(nvmeq->vq);
		blk_mq_stop_hw_queue(hctx);
		spin_unlock_irqrestore(&nvmeq->lock, flags);
		if (err == -ENOMEM || err == -ENOSPC)
			return BLK_MQ_RQ_QUEUE_BUSY;
		return BLK_MQ_RQ_QUEUE_ERROR;
	}

	if (bd->last && virtqueue_kick_prepare(nvmeq->vq))
		notify = true;
	spin_unlock_irqrestore(&nvmeq->lock, flags);

	if (notify)
		virtqueue_notify(nvmeq->vq);
	return BLK_MQ_RQ_QUEUE_OK;
}

static inline void nvme_virtio_request_done(struct request *req)
{
	struct nvme_virtio_req *vnr = blk_mq_rq_to_pdu(req);
	int error = vnr->resp.status;

#if 0 /* TODO */
	if (req->cmd_type == REQ_TYPE_BLOCK_PC) {
		req->resid_len = virtio32_to_cpu(dev->vdev, vbr->in_hdr.residual);
		req->sense_len = virtio32_to_cpu(dev->vdev, vbr->in_hdr.sense_len);
		req->errors = virtio32_to_cpu(dev->vdev, vbr->in_hdr.errors);
	} else if (req->cmd_type == REQ_TYPE_DRV_PRIV) {
		req->errors = (error != 0);
	}
#endif

	blk_mq_end_request(req, error);
}

static int nvme_virtio_init_request(void *data, struct request *rq,
		unsigned int hctx_idx, unsigned int request_idx,
		unsigned int numa_node)
{
	struct nvme_virtio_dev *dev = data;
	struct nvme_virtio_req *vnr = blk_mq_rq_to_pdu(rq);

	sg_init_table(vnr->sg, dev->sg_elems);
	return 0;
}

static int nvme_virtio_admin_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,
				unsigned int hctx_idx)
{
	struct nvme_virtio_dev *dev = data;
	struct nvme_virtio_queue *nvmeq = &dev->vqs[0];

	hctx->driver_data = nvmeq;
	return 0;
}

static int nvme_virtio_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,
				unsigned int hctx_idx)
{
	struct nvme_virtio_dev *dev = data;
	struct nvme_virtio_queue *nvmeq = &dev->vqs[hctx_idx+1];

	hctx->driver_data = nvmeq;
	return 0;
}

static struct blk_mq_ops nvme_virtio_mq_admin_ops = {
	.queue_rq	= nvme_virtio_queue_rq,
	.map_queue	= blk_mq_map_queue,
	.init_hctx	= nvme_virtio_admin_init_hctx,
	.complete	= nvme_virtio_request_done,
	.init_request	= nvme_virtio_init_request,
};

static struct blk_mq_ops nvme_virtio_mq_ops = {
	.queue_rq	= nvme_virtio_queue_rq,
	.map_queue	= blk_mq_map_queue,
	.init_hctx	= nvme_virtio_init_hctx,
	.complete	= nvme_virtio_request_done,
	.init_request	= nvme_virtio_init_request,
};

static unsigned int nvme_virtio_cmd_size(struct nvme_virtio_dev *dev)
{
	unsigned int ret;

	ret = sizeof(struct nvme_virtio_req) +
		sizeof(struct scatterlist) * dev->sg_elems;

        return ret;
}

static void nvme_virtio_dev_remove_admin(struct nvme_virtio_dev *dev)
{
	if (dev->ctrl.admin_q && !blk_queue_dying(dev->ctrl.admin_q)) {
		blk_cleanup_queue(dev->ctrl.admin_q);
		blk_mq_free_tag_set(&dev->admin_tagset);
	}
}

static int nvme_virtio_alloc_admin_tags(struct nvme_virtio_dev *dev)
{
	if (!dev->ctrl.admin_q) {
		dev->admin_tagset.ops = &nvme_virtio_mq_admin_ops;
		dev->admin_tagset.nr_hw_queues = 1;
		dev->admin_tagset.queue_depth = NVME_AQ_DEPTH;
		dev->admin_tagset.reserved_tags = 1;
		dev->admin_tagset.timeout = ADMIN_TIMEOUT;
		dev->admin_tagset.numa_node = NUMA_NO_NODE;
		dev->admin_tagset.cmd_size = nvme_virtio_cmd_size(dev);
		dev->admin_tagset.driver_data = dev;

		if (blk_mq_alloc_tag_set(&dev->admin_tagset))
			return -ENOMEM;

		dev->ctrl.admin_q = blk_mq_init_queue(&dev->admin_tagset);
		if (IS_ERR(dev->ctrl.admin_q)) {
			blk_mq_free_tag_set(&dev->admin_tagset);
			return -ENOMEM;
		}
		if (!blk_get_queue(dev->ctrl.admin_q)) {
			nvme_virtio_dev_remove_admin(dev);
			dev->ctrl.admin_q = NULL;
			return -ENODEV;
		}
	} else
		blk_mq_unfreeze_queue(dev->ctrl.admin_q);

	return 0;
}

static int nvme_virtio_dev_add(struct nvme_virtio_dev *dev)
{
	int err;

	/* Default queue sizing is to fill the ring. */
	if (!nvme_virtio_queue_depth)
		nvme_virtio_queue_depth = dev->vqs[1].vq->num_free;

	if (!dev->ctrl.tagset) {
		dev->tagset.ops = &nvme_virtio_mq_ops;
		dev->tagset.queue_depth = nvme_virtio_queue_depth;
		dev->tagset.numa_node = NUMA_NO_NODE;
		dev->tagset.flags = BLK_MQ_F_SHOULD_MERGE;
		dev->tagset.cmd_size = nvme_virtio_cmd_size(dev);
		dev->tagset.driver_data = dev;
		dev->tagset.nr_hw_queues = dev->num_vqs - 1;

		err = blk_mq_alloc_tag_set(&dev->tagset);
		if (err)
			return err;
		dev->ctrl.tagset = &dev->tagset;
	}

	return 0;
}

static int nvme_virtio_probe(struct virtio_device *vdev)
{
	struct nvme_virtio_dev *dev;
	u64 cap;
	u32 ctrl_config;
	u32 sg_elems;
	int err;

	if (!vdev->config->get) {
		printk("%s failure: config access disabled\n", __func__);
		return -EINVAL;
	}

	vdev->priv = dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	init_waitqueue_head(&dev->queue_wait);
	dev->vdev = vdev;

	/* We need to know how many segments before we allocate. */
	err = virtio_cread_feature(vdev, VIRTIO_NVME_F_SEG_MAX,
				   struct nvme_virtio_config, seg_max,
				   &sg_elems);
	/* We need at least one SG element, whatever they say. */
	if (err || !sg_elems)
		sg_elems = 1;

	/* We need two extra sg elements at head for command and response */
	sg_elems += 2;
	dev->sg_elems = sg_elems;

	virtio_cread(vdev, struct nvme_virtio_config, cap, &cap);

	ctrl_config = NVME_CC_ENABLE | NVME_CC_CSS_NVM;
	ctrl_config |= (PAGE_SHIFT - 12) << NVME_CC_MPS_SHIFT;
	ctrl_config |= NVME_CC_ARB_RR | NVME_CC_SHN_NONE;
	ctrl_config |= NVME_CC_IOSQES | NVME_CC_IOCQES;
	virtio_cwrite(vdev, struct nvme_virtio_config, cc, &ctrl_config);

	err = nvme_virtio_wait_ready(dev, cap);
	if (err)
		goto out_free_dev;

	/* Qemu starts controller and creates VQs */
	err = nvme_virtio_init_vq(dev);
	if (err)
		goto out_free_dev;

	err = nvme_init_ctrl(&dev->ctrl, &vdev->dev, &nvme_virtio_ctrl_ops,
			0, 0);
        if (err)
                goto out_free_dev;

	err = nvme_virtio_alloc_admin_tags(dev);
	if (err)
		goto out_free_dev;

	err = nvme_init_identify(&dev->ctrl);
	if (err)
		goto out_free_dev;

	spin_lock(&dev_list_lock);
	list_add(&dev->node, &dev_list);
	spin_unlock(&dev_list_lock);

	err = nvme_virtio_dev_add(dev);
	if (err)
		goto out_free_vq;

	nvme_scan_namespaces(&dev->ctrl);

	return 0;

out_free_vq:
	vdev->config->del_vqs(vdev);

out_free_dev:
	kfree(dev);
	return err;
}

static void nvme_virtio_remove(struct virtio_device *vdev)
{
	struct nvme_virtio_dev *dev = vdev->priv;

	spin_lock(&dev_list_lock);
	list_del_init(&dev->node);
	spin_unlock(&dev_list_lock);

	/* Stop all the virtqueues. */
	vdev->config->reset(vdev);

	vdev->config->del_vqs(vdev);

	nvme_remove_namespaces(&dev->ctrl);

	nvme_virtio_dev_remove_admin(dev);

	blk_mq_free_tag_set(&dev->tagset);
	kfree(dev->vqs);

	nvme_put_ctrl(&dev->ctrl);
}

static struct virtio_driver nvme_virtio_driver = {
	.feature_table			= features,
	.feature_table_size		= ARRAY_SIZE(features),
	.driver.name			= "virtio_nvme",
	.driver.owner			= THIS_MODULE,
	.id_table			= id_table,
	.probe				= nvme_virtio_probe,
	.remove				= nvme_virtio_remove,
};

static int __init nvme_virtio_init(void)
{
	int error;

	error = register_virtio_driver(&nvme_virtio_driver);
	return error;
}

static void __exit nvme_virtio_exit(void)
{
	unregister_virtio_driver(&nvme_virtio_driver);
}
module_init(nvme_virtio_init);
module_exit(nvme_virtio_exit);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio NVMe driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ming Lin <ming.l@ssi.samsung.com>");

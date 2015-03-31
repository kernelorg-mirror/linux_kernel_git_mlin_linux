#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/configfs.h>
#include <scsi/scsi.h>
#include <scsi/scsi_tcq.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_cmnd.h>

#include <target/target_core_base.h>
#include <target/target_core_fabric.h>
#include <target/target_core_fabric_configfs.h>
#include <target/target_core_configfs.h>

#define TA_WWN_ADDR_LEN 7 /* major:minor -> AAAA:BB */

struct tcm_aoe_dev {
	unsigned char ta_wwn_address[TA_WWN_ADDR_LEN];
	int major;
	int minor;
	struct se_wwn ta_dev_wwn;
};

#define TCM_TRANSPORT_ONLINE 0
#define TCM_TRANSPORT_OFFLINE 1

struct tcm_aoe_tpg {
	u32 index;
	unsigned short ta_transport_status;
	struct se_portal_group ta_se_tpg;
	struct tcm_aoe_dev *ta_wwn;
};

struct tcm_aoe_nacl {
	struct se_node_acl se_node_acl; 
};

/* Local pointer to allocated TCM configfs fabric module */
static struct target_fabric_configfs *tcm_aoe_configfs;

static char *tcm_aoe_get_fabric_name(void)
{
        return "aoe";
}

static u8 tcm_aoe_get_fabric_proto_ident(struct se_portal_group *se_tpg)
{
	//TBD
	return 0xf;
}

static char *tcm_aoe_get_wwn(struct se_portal_group *se_tpg)
{
	struct tcm_aoe_tpg *ta_tpg = se_tpg->se_tpg_fabric_ptr;
	/*
	 * Return the passed major:minor
	 */
	return ta_tpg->ta_wwn->ta_wwn_address; 
}

static struct se_wwn *tcm_aoe_make_dev(
	struct target_fabric_configfs *tf,
	struct config_group *group,
	const char *name)
{
	struct tcm_aoe_dev *ta_dev;

	ta_dev = kzalloc(sizeof(struct tcm_aoe_dev), GFP_KERNEL);
	if (!ta_dev) {
		pr_err("Unable to allocate struct tcm_aoe_dev\n");
		return ERR_PTR(-ENOMEM);
	}

	//TBD

	return &ta_dev->ta_dev_wwn;
}

static void tcm_aoe_drop_dev(
	struct se_wwn *wwn)
{
	//TBD
}

static struct se_portal_group *tcm_aoe_add_tpg(
	struct se_wwn *wwn,
	struct config_group *group,
	const char *name)
{
	struct tcm_aoe_dev *ta_dev;
	struct tcm_aoe_tpg *tpg;
	unsigned long index;
	int ret;

	pr_debug("tcm_aoe: add tpg %s\n", name);

	/*
	 * Name must be "tpgt_" followed by the index.
	 */
	if (strstr(name, "tpgt_") != name)
		return NULL;

	ret = kstrtoul(name + 5, 10, &index);
	if (ret)
		return NULL;
	if (index > UINT_MAX)
		return NULL;

	if ((index != 1)) {
		pr_err("Error, a single TPG=1 is used for HW port mappings\n");
		return ERR_PTR(-ENOSYS);
	}

	ta_dev = container_of(wwn, struct tcm_aoe_dev, ta_dev_wwn);
	tpg = kzalloc(sizeof(*tpg), GFP_KERNEL);
	if (!tpg)
		return NULL;
	tpg->index = index;
	tpg->ta_wwn = ta_dev;

	ret = core_tpg_register(&tcm_aoe_configfs->tf_ops, wwn, &tpg->ta_se_tpg,
				tpg, TRANSPORT_TPG_TYPE_NORMAL);
	if (ret < 0) {
		kfree(tpg);
		return NULL;
	}

	return &tpg->ta_se_tpg;
}

static void tcm_aoe_drop_tpg(
	struct se_portal_group *se_tpg)
{
	//TBD
}

static u16 tcm_aoe_get_tag(struct se_portal_group *se_tpg)
{
	struct tcm_aoe_tpg *ta_tpg = se_tpg->se_tpg_fabric_ptr;

	/*
	 * This tag is used when forming SCSI Name identifier in EVPD=1 0x83
	 * to represent the SCSI Target Port.
	 */
	return ta_tpg->index;
}

static u32 tcm_aoe_get_default_depth(struct se_portal_group *se_tpg)
{
	return 1;
}

/*
 * Returning (1) here allows for target_core_mod struct se_node_acl to be generated
 * based upon the incoming fabric dependent SCSI Initiator Port
 */
static int tcm_aoe_check_demo_mode(struct se_portal_group *se_tpg)
{
	return 1;
}

static int tcm_aoe_check_demo_mode_cache(struct se_portal_group *se_tpg)
{
	return 0;
}

/*
 * Allow I_T Nexus full READ-WRITE access without explict Initiator Node ACLs for
 * local virtual Linux/SCSI LLD passthrough into VM hypervisor guest
 */
static int tcm_aoe_check_demo_mode_write_protect(struct se_portal_group *se_tpg)
{
	return 0;
}

/*
 * Because TCM_Loop does not use explict ACLs and MappedLUNs, this will
 * never be called for TCM_Loop by target_core_fabric_configfs.c code.
 * It has been added here as a nop for target_fabric_tf_ops_check()
 */
static int tcm_aoe_check_prod_mode_write_protect(struct se_portal_group *se_tpg)
{
	return 0;
}

static struct se_node_acl *tcm_aoe_tpg_alloc_fabric_acl(
	struct se_portal_group *se_tpg)
{
	struct tcm_aoe_nacl *ta_nacl;

	ta_nacl = kzalloc(sizeof(struct tcm_aoe_nacl), GFP_KERNEL);
	if (!ta_nacl) {
		pr_err("Unable to allocate struct tcm_aoe_nacl\n");
		return NULL;
	}

	return &ta_nacl->se_node_acl;
}

static void tcm_aoe_tpg_release_fabric_acl(
	struct se_portal_group *se_tpg,
	struct se_node_acl *se_nacl)
{
	struct tcm_aoe_nacl *ta_nacl = container_of(se_nacl,
				struct tcm_aoe_nacl, se_node_acl);

	kfree(ta_nacl);
}

static u32 tcm_aoe_get_inst_index(struct se_portal_group *se_tpg)
{
	return 1;
}

int tcm_aoe_check_stop_free(struct se_cmd *se_cmd)
{
	transport_generic_free_cmd(se_cmd, 0);
	return 1;
}

static void tcm_aoe_release_cmd(struct se_cmd *se_cmd)
{
#if 0 /* TBD */
	struct tcm_loop_cmd *ta_cmd = container_of(se_cmd,
			struct tcm_loop_cmd, ta_se_cmd);

	kmem_cache_free(tcm_loop_cmd_cache, ta_cmd);
#endif
}

static u32 tcm_aoe_sess_get_index(struct se_session *se_sess)
{
	return 1;
}

static void tcm_aoe_set_default_node_attributes(struct se_node_acl *se_acl)
{
	return;
}

static u32 tcm_aoe_get_task_tag(struct se_cmd *se_cmd)
{
#if 0 /* TBD */
	struct tcm_aoe_cmd *ta_cmd = container_of(se_cmd,
			struct tcm_aoe_cmd, ta_se_cmd);

	return ta_cmd->sc_cmd_tag;
#else
	return -1;
#endif
}

static int tcm_aoe_get_cmd_state(struct se_cmd *se_cmd)
{
#if 0 /* TBD */
	struct tcm_aoe_cmd *ta_cmd = container_of(se_cmd,
			struct tcm_aoe_cmd, ta_se_cmd);

	return ta_cmd->sc_cmd_state;
#else
	return -1;
#endif
}

static int tcm_aoe_shutdown_session(struct se_session *se_sess)
{
	return 0;
}

static void tcm_aoe_close_session(struct se_session *se_sess)
{
	return;
}

static int tcm_aoe_write_pending(struct se_cmd *se_cmd)
{
	/*
	 * Since Linux/SCSI has already sent down a struct scsi_cmnd
	 * sc->sc_data_direction of DMA_TO_DEVICE with struct scatterlist array
	 * memory, and memory has already been mapped to struct se_cmd->t_mem_list
	 * format with transport_generic_map_mem_to_cmd().
	 *
	 * We now tell TCM to add this WRITE CDB directly into the TCM storage
	 * object execution queue.
	 */
	target_execute_cmd(se_cmd);
	return 0;
}

static int tcm_aoe_write_pending_status(struct se_cmd *se_cmd)
{
	return 0;
}

static int tcm_aoe_queue_data_in(struct se_cmd *se_cmd)
{
#if 0 /* TBD */
	struct tcm_aoe_cmd *ta_cmd = container_of(se_cmd,
				struct tcm_aoe_cmd, ta_se_cmd);
	struct scsi_cmnd *sc = ta_cmd->sc;

	pr_debug("tcm_aoe_queue_data_in() called for scsi_cmnd: %p"
		     " cdb: 0x%02x\n", sc, sc->cmnd[0]);

	sc->result = SAM_STAT_GOOD;
	set_host_byte(sc, DID_OK);
	if ((se_cmd->se_cmd_flags & SCF_OVERFLOW_BIT) ||
	    (se_cmd->se_cmd_flags & SCF_UNDERFLOW_BIT))
		scsi_set_resid(sc, se_cmd->residual_count);
	sc->scsi_done(sc);
	return 0;
#else
	return -1;
#endif
}

static int tcm_aoe_queue_status(struct se_cmd *se_cmd)
{
	/* TBD */
	return -1;
}

static void tcm_aoe_queue_tm_rsp(struct se_cmd *se_cmd)
{
	/* TBD */
}

static void tcm_aoe_aborted_task(struct se_cmd *se_cmd)
{
	return;
}

/* Start items for tcm_aoe_port_cit */

static int tcm_aoe_port_link(
	struct se_portal_group *se_tpg,
	struct se_lun *lun)
{
	/* TBD */
	return 0;
}

static void tcm_aoe_port_unlink(
	struct se_portal_group *se_tpg,
	struct se_lun *se_lun)
{
	/* TBD */
}

static struct target_core_fabric_ops tcm_aoe_fabric_ops = {
	.get_fabric_name		= tcm_aoe_get_fabric_name,
	.get_fabric_proto_ident		= tcm_aoe_get_fabric_proto_ident,
	.tpg_get_wwn			= tcm_aoe_get_wwn,
	.tpg_get_tag			= tcm_aoe_get_tag,
	.tpg_get_default_depth		= tcm_aoe_get_default_depth,
	.tpg_get_pr_transport_id	= sas_get_pr_transport_id,
	.tpg_get_pr_transport_id_len	= sas_get_pr_transport_id_len,
	.tpg_parse_pr_out_transport_id	= sas_parse_pr_out_transport_id,
	.tpg_check_demo_mode		= tcm_aoe_check_demo_mode,
	.tpg_check_demo_mode_cache	= tcm_aoe_check_demo_mode_cache,
	.tpg_check_demo_mode_write_protect = tcm_aoe_check_demo_mode_write_protect,
	.tpg_check_prod_mode_write_protect = tcm_aoe_check_prod_mode_write_protect,
	.tpg_alloc_fabric_acl		= tcm_aoe_tpg_alloc_fabric_acl,
	.tpg_release_fabric_acl		= tcm_aoe_tpg_release_fabric_acl,
	.tpg_get_inst_index		= tcm_aoe_get_inst_index,
	.check_stop_free		= tcm_aoe_check_stop_free,
	.release_cmd			= tcm_aoe_release_cmd,
	.shutdown_session		= tcm_aoe_shutdown_session,
	.close_session			= tcm_aoe_close_session,
	.sess_get_index			= tcm_aoe_sess_get_index,
	.sess_get_initiator_sid		= NULL,
	.write_pending			= tcm_aoe_write_pending,
	.write_pending_status		= tcm_aoe_write_pending_status,
	.set_default_node_attributes	= tcm_aoe_set_default_node_attributes,
	.get_task_tag			= tcm_aoe_get_task_tag,
	.get_cmd_state			= tcm_aoe_get_cmd_state,
	.queue_data_in			= tcm_aoe_queue_data_in,
	.queue_status			= tcm_aoe_queue_status,
	.queue_tm_rsp			= tcm_aoe_queue_tm_rsp,
	.aborted_task			= tcm_aoe_aborted_task,
	/*
	 * fabric_post_link() and fabric_pre_unlink() are used for
	 * registration and release of TCM Loop Virtual SCSI LUNs.
	 */
	.fabric_post_link		= tcm_aoe_port_link,
	.fabric_pre_unlink		= tcm_aoe_port_unlink,
	.fabric_make_np			= NULL,
	.fabric_drop_np			= NULL,

	.fabric_make_wwn		= tcm_aoe_make_dev,
	.fabric_drop_wwn		= tcm_aoe_drop_dev,
	.fabric_make_tpg		= tcm_aoe_add_tpg,
	.fabric_drop_tpg		= tcm_aoe_drop_tpg,
};

static int tcm_aoe_register_configfs(void)
{
	struct target_fabric_configfs *fabric;
	int ret;

	/*
	 * Register the top level struct config_item_type with TCM core
	 */
	fabric = target_fabric_configfs_init(THIS_MODULE, "aoe");
	if (IS_ERR(fabric)) {
		pr_err("tcm_aoe_register_configfs() failed!\n");
		return PTR_ERR(fabric);
	}

	fabric->tf_ops = tcm_aoe_fabric_ops;

	/*
	 * register the fabric for use within TCM
	 */
	ret = target_fabric_configfs_register(fabric);
	if (ret < 0) {
		pr_debug("target_fabric_configfs_register() for"
			    " AoE Target failed!\n");
		target_fabric_configfs_free(fabric);
		return -1;
	}

	/*
	 * Setup our local pointer to *fabric.
	 */
	tcm_aoe_configfs = fabric;
	return 0;
}

static void tcm_aoe_deregister_configfs(void)
{
	if (!tcm_aoe_configfs)
		return;
	target_fabric_configfs_deregister(tcm_aoe_configfs);
	tcm_aoe_configfs = NULL;
}

static int __init tcm_aoe_fabric_init(void)
{
	if (tcm_aoe_register_configfs())
		return -1;
	return 0;
}

static void __exit tcm_aoe_fabric_exit(void)
{
	tcm_aoe_deregister_configfs();
}

MODULE_DESCRIPTION("TCM ATA over ethernet fabric module");
MODULE_AUTHOR("Ming Lin <mlin@kernel.org>");
MODULE_LICENSE("GPL");
module_init(tcm_aoe_fabric_init);
module_exit(tcm_aoe_fabric_exit)

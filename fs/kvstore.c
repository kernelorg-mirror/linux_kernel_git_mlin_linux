/* A super simple K/V device simulation */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include "kvstore.h"

static DEFINE_SPINLOCK(kvs_list_lock);
static LIST_HEAD(kvs_list);
static struct kvstore simulate_kv_store;

/* TODO: some way to find kvstore by dev_name? */
struct kvstore *kvs_lookup(const char *dev_name)
{
	return &simulate_kv_store;	
}
EXPORT_SYMBOL(kvs_lookup);

static struct kvstore_entry *kv_find_key(struct kvstore *kvs, char *key)
{
	struct kvstore_entry *entry;
	int i;	

	for (i = 0; i < MAX_KV_ENTRIES; i++) {
		entry = &kvs->entries[i];
		if (!entry->valid)
			continue;
		if (!strcmp(key, entry->key))
			return entry;
	}

	return NULL;
}

static struct kvstore_entry *kv_find_empty(struct kvstore *kvs)
{
	struct kvstore_entry *entry;
	int i;	

	for (i = 0; i < MAX_KV_ENTRIES; i++) {
		entry = &kvs->entries[i];
		if (!entry->valid)
			return entry;
	}

	return NULL;
}

static int simulate_get(struct kvstore *kvs, char *key, char *value, int len)
{
	struct kvstore_entry *entry;

	entry = kv_find_key(kvs, key);
	if (!entry)
		return KV_NOT_FOUND;

	//if (len != entry->len)
	//	return KV_WRONG_SIZE;

	printk("KVSTORE: GET() called: len %d\n", entry->len);

	memcpy(value, entry->value, entry->len);
	return KV_SUCCESS;
}

static int simulate_put(struct kvstore *kvs, char *key, int key_len, char *value, int value_len)
{
	struct kvstore_entry *entry;

	//printk("LINMING: %s called: key %s, value_len %d\n", __func__, key, value_len);

	if (key_len > MAX_KEY_LEN || value_len > MAX_VALUE_LEN)
		return KV_WRONG_SIZE;

	BUG_ON(!value && value_len);

	entry = kv_find_key(kvs, key);
	if (entry) { /* Just update it */
		if (value)
			memcpy(entry->value, value, value_len);
		entry->len = value_len;
		return KV_SUCCESS;
	}

	if (kvs->num == MAX_KV_ENTRIES)
		return KV_FULL;

	entry = kv_find_empty(kvs);
	BUG_ON(!entry);
	memcpy(entry->key, key, key_len);
	if (value)
		memcpy(entry->value, value, value_len);
	entry->len = value_len;
	entry->valid = 1;
	
	kvs->num++;

	return KV_SUCCESS;
}

static int simulate_del(struct kvstore *kvs, char *key)
{
	struct kvstore_entry *entry;
	entry = kv_find_key(kvs, key);

	if (!entry)
		return KV_NOT_FOUND;

	entry->valid = 0;
	kvs->num--;
	return KV_SUCCESS;
}

static struct kvstore_entry *simulate_search(struct kvstore *kvs, char *key)
{
	return kv_find_key(kvs, key);
}

static int simulate_kv_open(struct inode *inode, struct file *f)
{
	struct kvstore *kvs;
	int instance = iminor(inode);
	int ret = -ENODEV;

	spin_lock(&kvs_list_lock);
	list_for_each_entry(kvs, &kvs_list, node) {
		if (kvs->instance == instance) {
			f->private_data = kvs;
			ret = 0;
			break;
		}
	}
	spin_unlock(&kvs_list_lock);

	return ret;
}

static long simulate_kv_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	//struct kvstore *kvs = f->private_data;
	int is_kv = 1; /* report to userspace: yes, I'm a valid k/v device */

	switch (cmd) {
	case KV_IOCTL_IS_KV:
		copy_to_user((void __user *)arg, &is_kv, sizeof(int));
		return 0;
	/* TODO: k/v ioctl GET/PUT/DEL */
	default:
		return -ENODEV;
	}
}

static const struct file_operations simulate_kv_fops = {
	.open		= simulate_kv_open,
	.unlocked_ioctl	= simulate_kv_ioctl,
	.compat_ioctl	= simulate_kv_ioctl,
	.owner		= THIS_MODULE,
};

static struct miscdevice simulate_kv_device = {
	.minor		= 0,
	.name		= "kv0",
	.fops		= &simulate_kv_fops,
};

static void add_debug_kv(struct kvstore *kvs)
{
	char key[] = "key1";
	char value[] = "blablabla\n";

	kvs->put(kvs, key, strlen(key)+1, value, strlen(value)+1);
}

static int __init kv_store_init(void)
{
	simulate_kv_store.num = 0;
	simulate_kv_store.instance = 0;
	INIT_LIST_HEAD(&simulate_kv_store.node);
	simulate_kv_store.get = simulate_get;
	simulate_kv_store.put = simulate_put;
	simulate_kv_store.del = simulate_del;
	simulate_kv_store.search = simulate_search;
	list_add(&simulate_kv_store.node, &kvs_list);

	add_debug_kv(&simulate_kv_store);

	return misc_register(&simulate_kv_device);
}

static void __exit kv_store_exit(void)
{
	misc_deregister(&simulate_kv_device);
}

MODULE_AUTHOR("Ming Lin");
MODULE_DESCRIPTION("key/value device simulation");
MODULE_LICENSE("GPL");
module_init(kv_store_init);
module_exit(kv_store_exit);

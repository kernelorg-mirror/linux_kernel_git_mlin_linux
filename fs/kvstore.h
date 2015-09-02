#ifndef __KV_STORE_H_
#define __KV_STORE_H_

#define MAX_KEY_LEN	64
#define MAX_VALUE_LEN	(8*1024)
#define MAX_KV_ENTRIES	10

#define KV_SUCCESS	0
#define KV_FULL		1
#define KV_WRONG_SIZE	2
#define KV_NOT_FOUND	3

#define KV_IOCTL_IS_KV  1

#include <linux/list.h>

struct kvstore_entry {
	char key[MAX_KEY_LEN];
	char value[MAX_VALUE_LEN];
	int len; /* value length */
	int valid;
	void *private;
};

struct kvstore {
	/*TODO: add spinlock */

	struct kvstore_entry entries[MAX_KV_ENTRIES];
	int num;
	int (*get)(struct kvstore *kvs, char *key, char *value, int len);
	int (*put)(struct kvstore *kvs, char *key, int key_len, char *value, int len);
	int (*del)(struct kvstore *kvs, char *key);	
	struct kvstore_entry *(*search)(struct kvstore *kvs, char *key);	

	struct list_head node;
	int instance;
};

extern struct kvstore *kvs_lookup(const char *dev_name);

#endif

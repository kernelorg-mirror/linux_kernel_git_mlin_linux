#include <linux/module.h>     
#include <linux/proc_fs.h> 
#include <linux/acpi.h> 

MODULE_AUTHOR("Lin Ming");
MODULE_LICENSE("GPL");

static struct proc_dir_entry *rtd3_proc;
static unsigned long long start_addr, end_addr;

static int rtd3_proc_read(char * buf, char ** start, off_t off,
                int count, int *eof, void *_data)
{
	char *p, *tmp;
	int n = 0;
	int len = end_addr - start_addr;
	char debug[17];

	*eof = 1;

	p = acpi_os_map_memory(start_addr, len);
	if (!p) {
		printk("map memory fail: 0x%llx - 0x%llx\n", start_addr, end_addr);
		return 0;
	}

	tmp = p;

	while (tmp < p + len) {
		if (n + 16 > count)
			break;

		strncpy(debug, tmp, 16);
		strcpy(buf+n, debug);
		n += strlen(debug) + 1; /* 1 for '\n' */
		buf[n-1] = '\n';

		tmp += 16;
	}

	acpi_os_unmap_memory(p, len);

        return n;
}

static int __init rtd3_init(void)
{
	acpi_status status;

	status = acpi_evaluate_integer(NULL, "\\DPTR", NULL, &start_addr);
	if (ACPI_FAILURE(status)) {
		printk("No \\DPTR node\n");
		return status;
	}
 
	status = acpi_evaluate_integer(NULL, "\\EPTR", NULL, &end_addr);
	if (ACPI_FAILURE(status)) {
		printk("No \\EPTR node\n");
		return status;
	}

	rtd3_proc = create_proc_entry("rtd3", S_IFREG | S_IRUGO, NULL);
	if (!rtd3_proc) {
		printk("create /proc/rtd3 fail\n");
	} else {
		rtd3_proc->read_proc = rtd3_proc_read;
		rtd3_proc->data = NULL;
	}
	
	return 0;
}

static void rtd3_cleanup(void)
{
	if (rtd3_proc)
		remove_proc_entry("rtd3", NULL);
}


module_init(rtd3_init);
module_exit(rtd3_cleanup);

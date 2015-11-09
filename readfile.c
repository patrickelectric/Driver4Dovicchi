#include <linux/module.h>  // Needed by all modules
#include <linux/kernel.h>  // Needed for KERN_INFO
#include <linux/mm.h>      // read information about memory
#include <linux/fs.h>      // Needed by filp
#include <asm/uaccess.h>   // Needed by segment descriptors

#define con(x) ((x) << (PAGE_SHIFT -10))

int init_module(void)
{
    printk(KERN_INFO "My module is loaded =D\n");
    struct sysinfo kk;
    si_meminfo(&kk);
    printk(KERN_INFO "%8lu end =D\n", con(kk.totalram));
    return 0;
}

void cleanup_module(void)
{
    printk(KERN_INFO "My module is unloaded\n");
}

#include <linux/cdev.h>      /* cdev_init, cdev_add, cdev_del */
#include <linux/device.h>    /* class_create, device_create */
#include <linux/fs.h>        /* alloc_chrdev_region, file_operations */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/uaccess.h>   /* copy_to_user */
#include <linux/mutex.h>     /* DEFINE_MUTEX, mutex_lock, mutex_unlock */
#include <linux/cpumask.h>       /* num_online_cpus, num_possible_cpus */
#include <linux/mm.h>            /* si_meminfo, struct sysinfo */
#include <linux/rcupdate.h>      /* rcu_read_lock, rcu_read_unlock */
#include <linux/sched/signal.h>  /* for_each_process */
#include <linux/string.h>        /* strscpy */
#include <linux/timekeeping.h>   /* ktime_get_boottime_seconds */
#include <linux/utsname.h>       /* utsname */

#define KFETCH_DEV_NAME "kfetch"
#define KFETCH_BUF_SIZE 1024

#define KFETCH_NUM_INFO 6

#define KFETCH_RELEASE   (1 << 0)
#define KFETCH_NUM_CPUS  (1 << 1)
#define KFETCH_CPU_MODEL (1 << 2)
#define KFETCH_MEM       (1 << 3)
#define KFETCH_UPTIME    (1 << 4)
#define KFETCH_NUM_PROCS (1 << 5)

#define KFETCH_FULL_INFO ((1 << KFETCH_NUM_INFO) - 1)

#define LOGO_LINES 7    // how many lines logo needs
#define LOGO_WIDTH 20   // for the alignment
#define LOGO_TOP   0    // vertically, from which line to start printing logo 
#define INFO_MAX   (2 + KFETCH_NUM_INFO)    // max number of lines to print
#define LINE_LEN   128  // max length for each line

static dev_t kfetch_devno;           /* store major + minor */
static struct cdev kfetch_cdev;
static struct class *kfetch_class;

static DEFINE_MUTEX(kfetch_mutex);
static int kfetch_mask = KFETCH_FULL_INFO;  // initialization to full info
static char kfetch_buf[KFETCH_BUF_SIZE];    // max size of buffer (same as user buffer size)

static const char *kfetch_logo[LOGO_LINES] = {
    "        .-.",
    "       (.. |",
    "       <>  |",
    "      / --- \\",
    "     ( |   | )",
    "   |\\\\_)__(_//|",
    "  <__)------(__>",
};

static char kfetch_info[INFO_MAX][LINE_LEN];   // total 8 lines, each has 128 bytes space

/* ---------- system information ---------- */

/* get memory information (from fs/proc/meminfo.c) */
static void kfetch_get_mem(unsigned long *free_mb, unsigned long *total_mb)
{
    struct sysinfo si;

    si_meminfo(&si);    // store memory info to si struct
    /* take out the info */
    *free_mb  = (si.freeram  * si.mem_unit) >> 20;  // si.freeram returns in unit of pages, multiply by the page size (si.mem_unit) in bytes then divided to MB
    *total_mb = (si.totalram * si.mem_unit) >> 20;
}

/* count the total number of processes */
static int kfetch_get_procs(void)
{
    struct task_struct *p;      // process descriptor
    int n = 0;

    rcu_read_lock();    // rcu: Read-Copy Update
    for_each_process(p)     // trace the main thread for each processes
        n++;
    rcu_read_unlock();
    return n;
}

/* get the total time that starts from booting */
static long long kfetch_get_uptime_min(void)
{
    return ktime_get_boottime_seconds() / 60;
}

/* ---------- show information & logo ---------- */

/* show information */
static int kfetch_build_info(int mask)
{
    /* 
     *  uts->nodename: hostname, uts->release: kernel version
     *  num_online_cpus(): number of cpus that are currently working
     *  num_possible_cpus(): the maximum number of cpus that this machine possibly have
     */

    struct new_utsname *uts = utsname();
    unsigned long free_mb, total_mb;
    size_t host_len;
    char *line;
    int n = 0;
    int i;

    /* first line: hostname */
    strscpy(kfetch_info[n++], uts->nodename, LINE_LEN);     // automatically add '\0'

    /* second line: separate line that length is the same as hostname */
    host_len = strlen(uts->nodename);
    memset(kfetch_info[n], '-', host_len);
    kfetch_info[n][host_len] = '\0';
    n++;

    /* other lines：從 bit 0 檢查到 bit 5，順序自動正確 */
    for (i = 0; i < KFETCH_NUM_INFO; i++) {
        if (!(mask & (1 << i)))     // if do not need to show the current info, run the next round
            continue;

        line = kfetch_info[n++];    // line = the current line

        switch (1 << i) {
            case KFETCH_RELEASE:
                snprintf(line, LINE_LEN, "Kernel: %s", uts->release);   // line: the buffer that the texts should write into, LINE_LEN: max size to write
                break;

            case KFETCH_NUM_CPUS:
                snprintf(line, LINE_LEN, "CPUs:   %u / %u", num_online_cpus(), num_possible_cpus());
                break;

            case KFETCH_CPU_MODEL:
                snprintf(line, LINE_LEN, "CPU:    %s", uts->machine);
                break;

            case KFETCH_MEM:
                kfetch_get_mem(&free_mb, &total_mb);
                snprintf(line, LINE_LEN, "Mem:    %lu MB / %lu MB", free_mb, total_mb);
                break;

            case KFETCH_UPTIME:
                snprintf(line, LINE_LEN, "Uptime: %lld mins", kfetch_get_uptime_min());
                break;

            case KFETCH_NUM_PROCS:
                snprintf(line, LINE_LEN, "Procs:  %d", kfetch_get_procs());
                break;
        }
    }

    return n;   // return the total lines info occupied
}

/* compose info & logo together */
static size_t kfetch_compose(char *buf, size_t size, int mask)
{
    int n_info = kfetch_build_info(mask);   // how many lines info use
    int total = max(LOGO_TOP + LOGO_LINES, n_info);     // maximum total line
    size_t len = 0;
    int i;

    for (i = 0; i < total; i++) {
        int li = i - LOGO_TOP;
        const char *logo = (li >= 0 && li < LOGO_LINES) ? kfetch_logo[li] : "";
        const char *info = (i < n_info) ? kfetch_info[i] : "";

        if (*info)
            len += scnprintf(buf + len, size - len, "%-*s%s\n",
                             LOGO_WIDTH, logo, info);   
                // -: align to left, place white space at the rest, *: depends on the next parameter(LOGO_WIDTH)
                // ex: * = 20, 5 bytes text -> 5 text first, 15 white space for the rest
        else
            len += scnprintf(buf + len, size - len, "%s\n", logo);
        // scnprintf(): return the bytes that actually be written in, includes '\n' but not '\0'
    }

    return len;     // how many bytes that logo & info actually used
}

/* ---------- file operations ---------- */

/* When user call open("dev/kfetch") */
static int kfetch_open(struct inode *inode, struct file *filp)
{
    pr_info("kfetch: open\n");
    return 0;
}

/* When user call close() */
static int kfetch_release(struct inode *inode, struct file *filp)
{
    pr_info("kfetch: release\n");
    return 0;
}

/* When user call read(fd, buf, 4096) -> (file descriptor, store to where, max buffer size) */
static ssize_t kfetch_read(struct file *filp, char __user *buffer,
                           size_t length, loff_t *offset)   // length is buffer size - 1
{
    ssize_t ret;
    size_t len;

    if (*offset > 0)    // read only one time(does not need multiple)
        return 0;

    mutex_lock(&kfetch_mutex);

    /* 0. clear buffer */
    memset(kfetch_buf, 0, sizeof(kfetch_buf));
    
    /* 1. write logo & info into kfetch_buf, at most write sizeof(kfetch_buf), return the len that actually written in */
    len = kfetch_compose(kfetch_buf, sizeof(kfetch_buf), kfetch_mask);

    if (len > length)   // cannot send data that its size is larger than the user buffer
        len = length;

    /* 2. send info to user (write the result to user buffer) */
    if (copy_to_user(buffer, kfetch_buf, len)) {
        pr_alert("Failed to copy data to user");
        ret = -EFAULT;
    } else {
        *offset += len;
        ret = len;
    }

    mutex_unlock(&kfetch_mutex);
    return ret;
}

/* When user call write() */    // loff_t: long file offset
// for the return type, ssize_t has negative value, can return -EFAULT...
static ssize_t kfetch_write(struct file *filp, const char __user *buffer,
                            size_t length, loff_t *offset)  // length is 4 bytes
{
    int mask_info;

    if (length < sizeof(mask_info))     // mask_info passed to here has exactly 4 bytes
        return -EINVAL;

    /* 1. copy the contents in user buffer to kernel buffer */
    if (copy_from_user(&mask_info, buffer, sizeof(mask_info))){  // ensure copying 4 bytes
        pr_alert("Failed to copy data from user");
        return -EFAULT;
    }

    mutex_lock(&kfetch_mutex);
    /* 2. set the mask that user requested */
    kfetch_mask = mask_info & KFETCH_FULL_INFO;     // set mask_info (with mutex since kfetch_mask is shared)
    mutex_unlock(&kfetch_mutex);

    return length;
}

/* Operation translating table (ex: open() -> kfetch_open()) */
static const struct file_operations kfetch_ops = {
    .owner   = THIS_MODULE,
    .read    = kfetch_read,
    .write   = kfetch_write,
    .open    = kfetch_open,
    .release = kfetch_release,
};

/* ---------- insmod / rmmod ---------- */

static int __init kfetch_init(void)     // with __init marks, while using insmod, kernel will call the functions with this marks
{
    struct device *dev;     // define by "linux/define.h"
    int ret;

    /* 1. Register device number： minor starts from 0, register 1 number, kfetch name 
    *     major: main device number (ex: RAM), minor: second device number (ex: sda0, sda1)
    *     kfetch_devno: number allocated
    *     after registered, kernel will write this number into /proc/devices
    */
    ret = alloc_chrdev_region(&kfetch_devno, 0, 1, KFETCH_DEV_NAME);
    if (ret < 0) {
        pr_err("kfetch: alloc_chrdev_region failed %d\n", ret);
        return ret;
    }

    /* 2. bind fops，and add it to the system */
    cdev_init(&kfetch_cdev, &kfetch_ops);   // set who deals with this file operations. initialize kfetch_cdev (ex: store kfetch_ops into cdev)
    ret = cdev_add(&kfetch_cdev, kfetch_devno, 1);  // register in cdev_map, when calling by user, kernel will use this to find file operations we developed
    if (ret)
        goto err_region;

    /* 3. create device class => generate sys/class/kfetch */
    kfetch_class = class_create(KFETCH_DEV_NAME);   // if error, return a pointer
    if (IS_ERR(kfetch_class)) {     // check error or not
        ret = PTR_ERR(kfetch_class);    // take out the error code
        goto err_cdev;
    }

    /* 4. create dev/kfetch 
    *     kfetch_class: which class this device belongs to
    *     NULL: parent device
    *     NULL: private data    
    */
    dev = device_create(kfetch_class, NULL, kfetch_devno, NULL, KFETCH_DEV_NAME);  
    if (IS_ERR(dev)) {
        ret = PTR_ERR(dev);
        goto err_class;
    }

    pr_info("kfetch: /dev/%s created, major=%d minor=%d\n",
            KFETCH_DEV_NAME, MAJOR(kfetch_devno), MINOR(kfetch_devno));
    return 0;

err_class:
    class_destroy(kfetch_class);
err_cdev:
    cdev_del(&kfetch_cdev);
err_region:
    unregister_chrdev_region(kfetch_devno, 1);
    return ret;
}

static void __exit kfetch_exit(void)
{
    device_destroy(kfetch_class, kfetch_devno);   
    class_destroy(kfetch_class);
    cdev_del(&kfetch_cdev);                       
    unregister_chrdev_region(kfetch_devno, 1);    
    pr_info("kfetch: removed\n");
}

module_init(kfetch_init);   // tell kernel to run while calling insmod
module_exit(kfetch_exit);   // tell kernel to run while calling rmmod
MODULE_LICENSE("GPL");
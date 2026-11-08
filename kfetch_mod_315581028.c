#include <linux/cdev.h>      /* cdev_init, cdev_add, cdev_del */
#include <linux/device.h>    /* class_create, device_create */
#include <linux/fs.h>        /* alloc_chrdev_region, file_operations */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/uaccess.h>   /* copy_to_user */
#include <linux/mutex.h>     /* DEFINE_MUTEX, mutex_lock, mutex_unlock */

#define DEVICE_NAME "kfetch"
#define KFETCH_BUF_SIZE 1024

#define KFETCH_NUM_INFO 6

#define KFETCH_RELEASE   (1 << 0)
#define KFETCH_NUM_CPUS  (1 << 1)
#define KFETCH_CPU_MODEL (1 << 2)
#define KFETCH_MEM       (1 << 3)
#define KFETCH_UPTIME    (1 << 4)
#define KFETCH_NUM_PROCS (1 << 5)

#define KFETCH_FULL_INFO ((1 << KFETCH_NUM_INFO) - 1)

static dev_t kfetch_devno;           /* store major + minor */
static struct cdev kfetch_cdev;
static struct class *kfetch_class;

static DEFINE_MUTEX(kfetch_mutex);
static int kfetch_mask = KFETCH_FULL_INFO;  // initialization to full info
static char kfetch_buf[KFETCH_BUF_SIZE];    // max size of buffer (same as user buffer size)

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
    
    /* 1. write text (kfetch_info) into kfetch_buf, at most write sizeof(kfetch_buf), return the len that actually written in */
    len = scnprintf(kfetch_buf, sizeof(kfetch_buf), "current mask = 0x%02x\n", kfetch_mask);  

    if (len > length)   // cannot send data that its size is larger than the user buffer
        len = length;

    /* 2. send info to user (write the result to user buffer) */
    if (copy_to_user(buffer, kfetch_buf, len)) {
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
    if (copy_from_user(&mask_info, buffer, sizeof(mask_info)))  // ensure copying 4 bytes
        return -EFAULT;

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
    ret = alloc_chrdev_region(&kfetch_devno, 0, 1, DEVICE_NAME);
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
    kfetch_class = class_create(DEVICE_NAME);   // if error, return a pointer
    if (IS_ERR(kfetch_class)) {     // check error or not
        ret = PTR_ERR(kfetch_class);    // take out the error code
        goto err_cdev;
    }

    /* 4. create dev/kfetch 
    *     kfetch_class: which class this device belongs to
    *     NULL: parent device
    *     NULL: private data    
    */
    dev = device_create(kfetch_class, NULL, kfetch_devno, NULL, DEVICE_NAME);  
    if (IS_ERR(dev)) {
        ret = PTR_ERR(dev);
        goto err_class;
    }

    pr_info("kfetch: /dev/%s created, major=%d minor=%d\n",
            DEVICE_NAME, MAJOR(kfetch_devno), MINOR(kfetch_devno));
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
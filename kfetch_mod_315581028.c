#include <linux/cdev.h>      /* cdev_init, cdev_add, cdev_del */
#include <linux/device.h>    /* class_create, device_create */
#include <linux/fs.h>        /* alloc_chrdev_region, file_operations */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/uaccess.h>   /* copy_to_user */

#define DEVICE_NAME "kfetch"

static dev_t kfetch_devno;           /* store major + minor */
static struct cdev kfetch_cdev;
static struct class *kfetch_class;

/* ---------- file operations（暫時版，Step 3 會改） ---------- */

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
                           size_t length, loff_t *offset)
{
    static const char msg[] = "kfetch device works!\n";
    size_t msg_len = sizeof(msg) - 1;
    size_t n;

    if (*offset >= msg_len)
        return 0;

    n = min(length, msg_len - (size_t)*offset);
    if (copy_to_user(buffer, msg + *offset, n))
        return -EFAULT;

    *offset += n;
    return n;
}

/* When user call write() */    // loff_t: long file offset
static ssize_t kfetch_write(struct file *filp, const char __user *buffer,
                            size_t length, loff_t *offset)  
{
    pr_info("kfetch: write %zu bytes\n", length);
    return length;                   /* 先假裝收下，Step 3 再真的處理 */
}

/* Operation translating table (ex: open() -> kfetch_open()) */
static const struct file_operations kfetch_ops = {
    .owner   = THIS_MODULE,
    .read    = kfetch_read,
    .write   = kfetch_write,
    .open    = kfetch_open,
    .release = kfetch_release,
};

/* ---------- 載入 / 卸載 ---------- */

static int __init kfetch_init(void)     // with __init marks, while using insmod, kernel will call the functions with this marks
{
    struct device *dev;     // define by "linux/define.h"
    int ret;

    /* 1. Register device number： minor starts from 0, register 1 number, kfetch name 
    *     major: main device number (ex: RAM), minor: second device number (ex: sda0, sda1)
    *     kfetch_devno: number allocated,
    */
    ret = alloc_chrdev_region(&kfetch_devno, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("kfetch: alloc_chrdev_region failed %d\n", ret);
        return ret;
    }

    /* 2. bind fops，and add it to the system */
    cdev_init(&kfetch_cdev, &kfetch_ops);   // initialize kfetch_cdev (ex: store kfetch_ops into cdev)
    ret = cdev_add(&kfetch_cdev, kfetch_devno, 1);  // register in cdev_map to represent that this number is managed by kfetch_cdev
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
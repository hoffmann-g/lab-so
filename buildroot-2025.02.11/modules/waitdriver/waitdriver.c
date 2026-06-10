#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include <linux/wait.h>

#define DEVICE_NAME "waitdrv"
#define DEVCOUNT 1

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Estudante");
MODULE_DESCRIPTION("Waitqueue device driver");
MODULE_VERSION("0.0.1");

static ssize_t	waitdrv_read   (struct  file *,       char *, size_t, loff_t *);
static ssize_t	waitdrv_write  (struct  file *, const char *, size_t, loff_t *);
static int	    waitdrv_open   (struct inode *, struct file *);
static int	    waitdrv_release(struct inode *, struct file *);

static dev_t devno = 0;
static struct cdev chardev = {};
static struct class *cls = NULL;
static struct device *dev = NULL;
static struct task_struct *wait_thread = NULL;
static wait_queue_head_t wait_queue;

static int wait_function(void *);

static struct file_operations fops =
{
    .read    = waitdrv_read,
    .write   = waitdrv_write,
    .open    = waitdrv_open,
    .release = waitdrv_release
};

static int wait_flag = 0;

static int wait_function(void *arg)
{
	while (1) {
		pr_info("Waiting for event...\n");

		wait_event_interruptible(wait_queue, wait_flag != 0);
		switch (wait_flag) {
		case 1:
			pr_info("event came from open() function\n");
			break;
		case 2:
			pr_info("event came from read() function\n");
			break;
		case 3:
			pr_info("event came from write() function\n");
			break;
        case 4:
			pr_info("event came from close() function\n");
			break;
		case 5:
			pr_info("event came from exit function\n");
			return 0;
		}

		wait_flag = 0;
	}

	return 0;
}

static int waitdrv_open(struct inode *inode, struct file *file)
{
    wait_flag = 1;
    wake_up_interruptible(&wait_queue);

    return 0;
}

static ssize_t waitdrv_read(struct file *filp, char __user *buf, size_t len, loff_t *off)
{
    wait_flag = 2;
    wake_up_interruptible(&wait_queue);

    return 0;
}

static ssize_t waitdrv_write(struct file *filp, const char __user *buf, size_t len, loff_t *off)
{
    wait_flag = 3;
    wake_up_interruptible(&wait_queue);

    return len;
}

static int waitdrv_release(struct inode *inode, struct file *file)
{
	wait_flag = 4;
    wake_up_interruptible(&wait_queue);

	return 0;
}

static int waitdrv_init(void)
{
    pr_info("Inserting the Waitqueue Device\n");

    int err = alloc_chrdev_region(&devno, 0, DEVCOUNT, DEVICE_NAME);
    if (err != 0) {
        pr_err("Waitqueue Device failed to register a major number");
        return err;
    }
    pr_info("Waitqueue Device registered driver\n");
    cdev_init(&chardev, &fops);
    err = cdev_add(&chardev, devno, DEVCOUNT);
    if (err != 0) {
        pr_err("Waitqueue Device failed to register a major number");
        goto err_cdev;
    }
    pr_info("Waiqueue Device successfuly added device\n");

    cls = class_create(DEVICE_NAME);
    if (IS_ERR(cls)) {
        pr_err("Waitqueue Device failed to register device class\n");
        err = PTR_ERR(cls);
        goto err_cls;
    }
    pr_info("Waitqueue Device registered class\n");

    dev = device_create(cls, NULL, devno, NULL, DEVICE_NAME);
    if (IS_ERR(dev)) {
        pr_alert("Waitqueue Device failed to map device\n");
        err = PTR_ERR(dev);
        goto err_dev;
    }
    pr_info("Waitqueue Device mapped device\n");
    init_waitqueue_head(&wait_queue);
    wait_thread = kthread_create(wait_function, NULL, "wait_thread");
    if (wait_thread == NULL) {
        pr_info("thread creation failed\n");
        goto err_thread;
    }

    pr_info("Kernel Thread created successfully\n");
    wake_up_process(wait_thread);
    return 0;
   
    

err_thread:
    device_destroy(cls, devno);
err_dev:
    class_destroy(cls);
err_cls:
    cdev_del(&chardev);
err_cdev:
    unregister_chrdev_region(devno, DEVCOUNT);

    return err;

}

static void waitdrv_exit(void)
{
    wait_flag = 5;
    wake_up_interruptible(&wait_queue);

    device_destroy(cls, devno);
    class_destroy(cls);
    cdev_del(&chardev);
    unregister_chrdev_region(devno, DEVCOUNT);
    pr_info("Removed the Waitqueue Device\n");
}

module_init(waitdrv_init);
module_exit(waitdrv_exit);

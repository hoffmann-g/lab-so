#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/errno.h>

#define DEVICE_NAME "chardrv"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Estudante");
MODULE_DESCRIPTION("Character Device Driver");
MODULE_VERSION("0.0.1");

//int register_chrdev(unsigned int major, const char *name, struct file_operations *fops);
static int major;
static int number_opens = 0;
static ssize_t	chardrv_read   (struct  file *,       char *, size_t, loff_t *);
static ssize_t	chardrv_write  (struct  file *, const char *, size_t, loff_t *);
static int	    chardrv_open   (struct inode *, struct file *);
static int	    chardrv_release(struct inode *, struct file *);

static char message[256] = {}; 
static short size_of_message = 0;

static struct class *cls = NULL;
static struct device *dev = NULL;

static struct file_operations fops =
{
	.read    = chardrv_read,
	.write   = chardrv_write,
    .open    = chardrv_open,
	.release = chardrv_release
};

static int chardrv_init(void)
{
    pr_info("Inserting the Character Device\n");

    major = register_chrdev(0, DEVICE_NAME, &fops);
    if (major < 0) {
        pr_alert("Character Device failed to register a major number");
        return major;
    }
    pr_info("Character Device registered driver\n");

    cls = class_create(DEVICE_NAME);
    if (IS_ERR(cls)) {
        unregister_chrdev(major, DEVICE_NAME);
        pr_alert("Character Device failed to register device class\n");
        return PTR_ERR(cls);
    }
    pr_info("Character Device registered class\n");

    dev = device_create(cls, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);
    if (IS_ERR(dev)) {
        class_destroy(cls);
        unregister_chrdev(major, DEVICE_NAME);
        pr_alert("Character Device failed to create device\n");
        return PTR_ERR(dev);
    }
    pr_info("Character Device registered device\n");

    return 0;
}

static int chardrv_open(struct inode *inodep, struct file *filep)
{
    number_opens++;
	pr_info("Device has been opened %d time(s)\n", number_opens);

	return 0;
}
static ssize_t chardrv_read(struct file *filep, char *buffer, size_t len, loff_t *offset)
{
    int error_count = copy_to_user(buffer, message, size_of_message);
    if (error_count != 0) {
        pr_alert("Failed to send %d characters to the user\n", error_count);
        return -EFAULT;
    }

    pr_info("Sent %zu characters to the user\n", size_of_message);
    int ret = size_of_message;
    size_of_message = 0;
    return ret;
}
static int chardrv_release(struct inode *inodep, struct file *filep)
{
    pr_info("Device successfully closed\n");
	return 0;
}
static void chardrv_exit(void)
{
    device_destroy(cls, MKDEV(major, 0));
    class_destroy(cls);
    unregister_chrdev(major, DEVICE_NAME);
    pr_info("Removed the Character Device\n");
}

static ssize_t chardrv_write(struct file *filep, const char *buffer, size_t len, loff_t *offset)
{
    if (len >= sizeof(message)) {
        sprintf(message, "(0 characters)");
        pr_alert("Too many characters to deal with (%d)\n", len);
        return 0;
    }

    sprintf(message, "(%zu characters) %s", len, buffer);
    size_of_message = strlen(message);
    pr_info("Received %zu characters from the user\n", len);
    return len;
}

module_init(chardrv_init);
module_exit(chardrv_exit);

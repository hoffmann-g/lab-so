#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/hrtimer.h>
#include <linux/errno.h>

#define TIMEOUT_SEC 1
#define TIMEOUT_NSEC 0

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Estudante");
MODULE_DESCRIPTION("Timer Module");
MODULE_VERSION("0.0.1");

static unsigned long timeout_nsec = TIMEOUT_NSEC;
static s64 timeout_sec = TIMEOUT_SEC;
static ssize_t timeout_sec_show(struct kobject *, struct kobj_attribute *, char *);
static ssize_t timeout_sec_store(struct kobject *, struct kobj_attribute *, const char *, size_t);

static struct kobject *timerdrv_kobj;

static struct hrtimer hr_timer;

static struct kobj_attribute timeout_sec_attr = __ATTR(
    timeout_sec, 
    0660, 
    timeout_sec_show, 
    timeout_sec_store
);

static enum hrtimer_restart timer_callback(struct hrtimer *);

static enum hrtimer_restart timer_callback(struct hrtimer *timer)
{
	pr_info("Hello from timer!\n");
	ktime_t ktime = ktime_set(timeout_sec, timeout_nsec);
	hrtimer_forward_now(timer, ktime);

	return HRTIMER_RESTART;
}

static ssize_t timeout_sec_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%lld\n", timeout_sec);
}

static ssize_t timeout_sec_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    int value;
    if (sscanf(buf, "%d", &value) != 1)
        return -EINVAL;

    timeout_sec = value;
    return count;
}

static int timerdrv_init(void)
{
    pr_info("Inserting the Timer module\n");

    hrtimer_init(&hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    hr_timer.function = timer_callback;

    timerdrv_kobj = kobject_create_and_add("timerdrv", NULL);
    if (timerdrv_kobj == NULL) {
        pr_alert("Failed to create kobject\n");
        return -ENOMEM;
    }

    int error = sysfs_create_file(timerdrv_kobj, &timeout_sec_attr.attr);
    if (error) {
        pr_alert("Failed to create sysfs /sys/timerdrv/timeout_sec\n");
        kobject_put(timerdrv_kobj);
        return error;
    }

    ktime_t ktime = ktime_set(TIMEOUT_SEC, TIMEOUT_NSEC);
    hrtimer_start(&hr_timer, ktime, HRTIMER_MODE_REL);

    return 0;
}

static void timerdrv_exit(void)
{
    hrtimer_cancel(&hr_timer);
    sysfs_remove_file(timerdrv_kobj, &timeout_sec_attr.attr);
    kobject_put(timerdrv_kobj);
    pr_info("Removed the Timer module\n");
}


module_param(timeout_nsec, ulong, 0644);
module_init(timerdrv_init);
module_exit(timerdrv_exit);


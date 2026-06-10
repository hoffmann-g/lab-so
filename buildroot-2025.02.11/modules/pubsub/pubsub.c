#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/sched.h>
#include <linux/pid.h>

#define DEVICE_NAME      "pubsub"
#define TOPIC_NAME_MAX   64
#define MAX_MSG_LEN      4096
#define DEFAULT_MAX_SUBS 10

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Guilherme Hoffmann, Endrew Soares, João Sbardelotto, George Rother");
MODULE_DESCRIPTION("Publish/Subscribe kernel module");
MODULE_VERSION("1.0.0");

static int max_topics = 10;
module_param(max_topics, int, 0444);
MODULE_PARM_DESC(max_topics, "Maximum number of topics");

/* data structures */
struct pubsub_message {
	char           *data;
	size_t          len;
	struct list_head list;
};

struct pubsub_subscriber {
	pid_t            pid;
	struct list_head messages;
	struct mutex     msg_lock;
	struct list_head list;
};

struct pubsub_topic {
	char             name[TOPIC_NAME_MAX];
	struct list_head subscribers;
	struct mutex     sub_lock;
	int              sub_count;
	unsigned long    msg_count;
	int              max_subscribers;
	struct kobject   kobj;
	struct list_head list;
};

struct pubsub_ctx {
	char fetch_topic[TOPIC_NAME_MAX];
	bool has_fetch;
};

/* global state */
static LIST_HEAD(topics_list);
static DEFINE_MUTEX(topics_lock);
static int topic_count = 0;

static int major;
static struct class *dev_class = NULL;
static struct device *dev_node  = NULL;

static struct kobject        *pubsub_kobj = NULL;
static struct proc_dir_entry *proc_entry  = NULL;

/* sysfs: /sys/pubsub/<topic>/max_subscribers */

/* read max_subscribers from sysfs */
static ssize_t max_subs_show(struct kobject *kobj,
			     struct kobj_attribute *attr, char *buf)
{
	struct pubsub_topic *t =
		container_of(kobj, struct pubsub_topic, kobj);
	return sysfs_emit(buf, "%d\n", t->max_subscribers);
}

/* write max_subscribers via sysfs */
static ssize_t max_subs_store(struct kobject *kobj,
			      struct kobj_attribute *attr,
			      const char *buf, size_t count)
{
	struct pubsub_topic *t =
		container_of(kobj, struct pubsub_topic, kobj);
	int val;

	if (kstrtoint(buf, 10, &val) < 0 || val < 1)
		return -EINVAL;
	t->max_subscribers = val;
	return count;
}

static struct kobj_attribute max_subs_attr =
	__ATTR(max_subscribers, 0664, max_subs_show, max_subs_store);

static struct attribute *topic_attrs[] = {
	&max_subs_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(topic);

/* free topic struct when kobject refcount hits zero */
static void topic_kobj_release(struct kobject *kobj)
{
	kfree(container_of(kobj, struct pubsub_topic, kobj));
}

static struct kobj_type topic_ktype = {
	.sysfs_ops      = &kobj_sysfs_ops,
	.default_groups = topic_groups,
	.release        = topic_kobj_release,
};

/* /proc/pubsub: lists each topic and its total message count */

/* print topic name and message count */
static int pubsub_proc_show(struct seq_file *m, void *v)
{
	struct pubsub_topic *t;

	mutex_lock(&topics_lock);
	list_for_each_entry(t, &topics_list, list)
		seq_printf(m, "%s: %lu\n", t->name, t->msg_count);
	mutex_unlock(&topics_lock);
	return 0;
}

/* open /proc/pubsub for reading */
static int pubsub_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, pubsub_proc_show, NULL);
}

static const struct proc_ops proc_fops = {
	.proc_open    = pubsub_proc_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* helpers: search, alloc, and free topics/subscribers/messages */

/* find topic by name */
static struct pubsub_topic *find_topic(const char *name)
{
	struct pubsub_topic *t;

	list_for_each_entry(t, &topics_list, list)
		if (strcmp(t->name, name) == 0)
			return t;
	return NULL;
}

/* find subscriber by pid in topic */
static struct pubsub_subscriber *find_subscriber(struct pubsub_topic *t,
						  pid_t pid)
{
	struct pubsub_subscriber *s;

	list_for_each_entry(s, &t->subscribers, list)
		if (s->pid == pid)
			return s;
	return NULL;
}

/* drain and free all queued messages */
static void free_messages(struct pubsub_subscriber *s)
{
	struct pubsub_message *m, *tmp;

	list_for_each_entry_safe(m, tmp, &s->messages, list) {
		list_del(&m->list);
		kfree(m->data);
		kfree(m);
	}
}

/* unlink and free a subscriber */
static void remove_subscriber(struct pubsub_topic *t,
			      struct pubsub_subscriber *s)
{
	mutex_lock(&s->msg_lock);
	free_messages(s);
	mutex_unlock(&s->msg_lock);
	list_del(&s->list);
	t->sub_count--;
	kfree(s);
}

/* unlink topic and drop kobject ref */
static void remove_topic(struct pubsub_topic *t)
{
	list_del(&t->list);
	topic_count--;
	kobject_del(&t->kobj);
	kobject_put(&t->kobj);
}

/* allocate and register a new topic */
static struct pubsub_topic *create_topic(const char *name)
{
	struct pubsub_topic *t;

	if (topic_count >= max_topics)
		return NULL;

	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return NULL;

	strscpy(t->name, name, TOPIC_NAME_MAX);
	INIT_LIST_HEAD(&t->subscribers);
	mutex_init(&t->sub_lock);
	t->sub_count       = 0;
	t->msg_count       = 0;
	t->max_subscribers = DEFAULT_MAX_SUBS;

	kobject_init(&t->kobj, &topic_ktype);
	if (kobject_add(&t->kobj, pubsub_kobj, "%s", name) != 0) {
		kobject_put(&t->kobj);
		return NULL;
	}

	list_add_tail(&t->list, &topics_list);
	topic_count++;
	return t;
}

/* command handlers: called from pubsub_write based on the written prefix */

/* add current pid to topic */
static void handle_subscribe(struct file *filep, const char *topic_name)
{
	pid_t pid = task_pid_nr(current);
	struct pubsub_topic *t;
	struct pubsub_subscriber *s;

	mutex_lock(&topics_lock);

	t = find_topic(topic_name);
	if (!t) {
		t = create_topic(topic_name);
		if (!t)
			goto out_topics;
	}

	mutex_lock(&t->sub_lock);

	if (find_subscriber(t, pid))
		goto out_sub;

	if (t->sub_count >= t->max_subscribers)
		goto out_sub;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		goto out_sub;

	s->pid = pid;
	INIT_LIST_HEAD(&s->messages);
	mutex_init(&s->msg_lock);
	list_add_tail(&s->list, &t->subscribers);
	t->sub_count++;

out_sub:
	mutex_unlock(&t->sub_lock);
out_topics:
	mutex_unlock(&topics_lock);
}

/* remove current pid from topic */
static void handle_unsubscribe(struct file *filep, const char *topic_name)
{
	pid_t pid = task_pid_nr(current);
	struct pubsub_topic *t;
	struct pubsub_subscriber *s;

	mutex_lock(&topics_lock);

	t = find_topic(topic_name);
	if (!t)
		goto out;

	mutex_lock(&t->sub_lock);

	s = find_subscriber(t, pid);
	if (s)
		remove_subscriber(t, s);

	if (t->sub_count == 0) {
		mutex_unlock(&t->sub_lock);
		remove_topic(t);
		goto out;
	}

	mutex_unlock(&t->sub_lock);
out:
	mutex_unlock(&topics_lock);
}

/* fan-out message to all subscribers */
static void handle_publish(struct file *filep, char *args)
{
	char *p = args;
	char *topic_name, *msg_data;
	size_t msg_len;
	struct pubsub_topic *t;
	struct pubsub_subscriber *s;

	topic_name = args;
	while (*p && !isspace((unsigned char)*p))
		p++;
	if (!*p)
		return;
	*p++ = '\0';
	while (*p && isspace((unsigned char)*p))
		p++;

	msg_data = p;
	msg_len  = strlen(msg_data);

	if (msg_len >= 2 && msg_data[0] == '"' &&
	    msg_data[msg_len - 1] == '"') {
		msg_data[msg_len - 1] = '\0';
		msg_data++;
		msg_len -= 2;
	}

	if (msg_len == 0)
		return;
	if (msg_len > MAX_MSG_LEN)
		msg_len = MAX_MSG_LEN;

	mutex_lock(&topics_lock);

	t = find_topic(topic_name);
	if (!t || t->sub_count == 0) {
		mutex_unlock(&topics_lock);
		return;
	}

	t->msg_count++;

	mutex_lock(&t->sub_lock);
	mutex_unlock(&topics_lock);

	list_for_each_entry(s, &t->subscribers, list) {
		struct pubsub_message *msg;

		msg = kmalloc(sizeof(*msg), GFP_KERNEL);
		if (!msg)
			continue;

		msg->data = kmalloc(msg_len + 1, GFP_KERNEL);
		if (!msg->data) {
			kfree(msg);
			continue;
		}

		memcpy(msg->data, msg_data, msg_len);
		msg->data[msg_len] = '\0';
		msg->len = msg_len;

		mutex_lock(&s->msg_lock);
		list_add_tail(&msg->list, &s->messages);
		mutex_unlock(&s->msg_lock);
	}

	mutex_unlock(&t->sub_lock);
}

/* record which topic to read next */
static void handle_fetch(struct file *filep, const char *topic_name)
{
	struct pubsub_ctx *ctx = filep->private_data;

	strscpy(ctx->fetch_topic, topic_name, TOPIC_NAME_MAX);
	ctx->has_fetch = true;
}

/* file operations: open/read/write/release for /dev/pubsub */

/* allocate per-fd context */
static int pubsub_open(struct inode *inodep, struct file *filep)
{
	struct pubsub_ctx *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);

	if (!ctx)
		return -ENOMEM;
	filep->private_data = ctx;
	return 0;
}

/* pop oldest message for the fetched topic */
static ssize_t pubsub_read(struct file *filep, char __user *buf,
			   size_t len, loff_t *offset)
{
	struct pubsub_ctx        *ctx = filep->private_data;
	pid_t                     pid = task_pid_nr(current);
	struct pubsub_topic      *t;
	struct pubsub_subscriber *s;
	struct pubsub_message    *msg = NULL;
	char   *data     = NULL;
	size_t  data_len = 0;
	ssize_t ret      = 0;

	if (!ctx->has_fetch)
		return 0;

	mutex_lock(&topics_lock);
	t = find_topic(ctx->fetch_topic);
	if (!t) {
		mutex_unlock(&topics_lock);
		return 0;
	}
	mutex_lock(&t->sub_lock);
	mutex_unlock(&topics_lock);

	s = find_subscriber(t, pid);
	if (!s) {
		mutex_unlock(&t->sub_lock);
		return 0;
	}

	mutex_lock(&s->msg_lock);
	if (!list_empty(&s->messages)) {
		msg = list_first_entry(&s->messages,
				       struct pubsub_message, list);
		list_del(&msg->list);
		data     = msg->data;
		data_len = msg->len;
		kfree(msg);
	}
	mutex_unlock(&s->msg_lock);
	mutex_unlock(&t->sub_lock);

	if (!data)
		return 0;

	if (data_len > len)
		data_len = len;

	if (copy_to_user(buf, data, data_len))
		ret = -EFAULT;
	else
		ret = (ssize_t)data_len;

	kfree(data);
	return ret;
}

/* parse and dispatch a command string */
static ssize_t pubsub_write(struct file *filep, const char __user *buf,
			    size_t len, loff_t *offset)
{
	size_t klen = min(len, (size_t)(MAX_MSG_LEN + 256));
	char *kbuf;

	kbuf = kmalloc(klen + 1, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	if (copy_from_user(kbuf, buf, klen)) {
		kfree(kbuf);
		return -EFAULT;
	}
	kbuf[klen] = '\0';

	if (klen > 0 && kbuf[klen - 1] == '\n')
		kbuf[--klen] = '\0';

	if (strncmp(kbuf, "/subscribe ", 11) == 0)
		handle_subscribe(filep, strim(kbuf + 11));
	else if (strncmp(kbuf, "/unsubscribe ", 13) == 0)
		handle_unsubscribe(filep, strim(kbuf + 13));
	else if (strncmp(kbuf, "/publish ", 9) == 0)
		handle_publish(filep, kbuf + 9);
	else if (strncmp(kbuf, "/fetch ", 7) == 0)
		handle_fetch(filep, strim(kbuf + 7));

	kfree(kbuf);
	return (ssize_t)len;
}

/* auto-unsubscribe pid on close */
static int pubsub_release(struct inode *inodep, struct file *filep)
{
	struct pubsub_ctx        *ctx = filep->private_data;
	pid_t                     pid = task_pid_nr(current);
	struct pubsub_topic      *t, *tmp_t;
	struct pubsub_subscriber *s;

	mutex_lock(&topics_lock);

	list_for_each_entry_safe(t, tmp_t, &topics_list, list) {
		mutex_lock(&t->sub_lock);

		s = find_subscriber(t, pid);
		if (s) {
			remove_subscriber(t, s);
			if (t->sub_count == 0) {
				mutex_unlock(&t->sub_lock);
				remove_topic(t);
				continue;
			}
		}

		mutex_unlock(&t->sub_lock);
	}

	mutex_unlock(&topics_lock);

	kfree(ctx);
	filep->private_data = NULL;
	return 0;
}

static const struct file_operations fops = {
	.owner   = THIS_MODULE,
	.open    = pubsub_open,
	.read    = pubsub_read,
	.write   = pubsub_write,
	.release = pubsub_release,
};

/* module init and exit */

/* register device, sysfs, and proc entries */
static int __init pubsub_init(void)
{
	int err;

	pr_info("pubsub: loading (max_topics=%d)\n", max_topics);

	major = register_chrdev(0, DEVICE_NAME, &fops);
	if (major < 0) {
		pr_err("pubsub: register_chrdev failed\n");
		return major;
	}

	dev_class = class_create(DEVICE_NAME);
	if (IS_ERR(dev_class)) {
		err = PTR_ERR(dev_class);
		goto err_class;
	}

	dev_node = device_create(dev_class, NULL, MKDEV(major, 0),
				 NULL, DEVICE_NAME);
	if (IS_ERR(dev_node)) {
		err = PTR_ERR(dev_node);
		goto err_device;
	}

	pubsub_kobj = kobject_create_and_add(DEVICE_NAME, NULL);
	if (!pubsub_kobj) {
		err = -ENOMEM;
		goto err_kobj;
	}

	proc_entry = proc_create(DEVICE_NAME, 0444, NULL, &proc_fops);
	if (!proc_entry) {
		err = -ENOMEM;
		goto err_proc;
	}

	pr_info("pubsub: /dev/pubsub, /sys/pubsub/, /proc/pubsub ready\n");
	return 0;

err_proc:
	kobject_put(pubsub_kobj);
err_kobj:
	device_destroy(dev_class, MKDEV(major, 0));
err_device:
	class_destroy(dev_class);
err_class:
	unregister_chrdev(major, DEVICE_NAME);
	return err;
}

/* free all topics and tear down interfaces */
static void __exit pubsub_exit(void)
{
	struct pubsub_topic      *t, *tmp_t;
	struct pubsub_subscriber *s, *tmp_s;

	proc_remove(proc_entry);

	mutex_lock(&topics_lock);
	list_for_each_entry_safe(t, tmp_t, &topics_list, list) {
		mutex_lock(&t->sub_lock);
		list_for_each_entry_safe(s, tmp_s, &t->subscribers, list)
			remove_subscriber(t, s);
		mutex_unlock(&t->sub_lock);

		list_del(&t->list);
		topic_count--;
		kobject_del(&t->kobj);
		kobject_put(&t->kobj);
	}
	mutex_unlock(&topics_lock);

	kobject_put(pubsub_kobj);
	device_destroy(dev_class, MKDEV(major, 0));
	class_destroy(dev_class);
	unregister_chrdev(major, DEVICE_NAME);

	pr_info("pubsub: unloaded\n");
}

module_init(pubsub_init);
module_exit(pubsub_exit);

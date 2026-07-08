#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

static void print_mem(struct task_struct *task){
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    VMA_ITERATOR(vmi, task->mm, 0);
    int count = 0;
    mm = task->mm;

    printk("\nA struct mm_struct tem %d VMAs.\n", mm->map_count);

    for_each_vma(vmi, vma) {
        printk("%d)   Inicio em 0x%lx, Termina em 0x%lx\n", ++count,
            vma->vm_start, vma->vm_end);
    }

    printk("Segmento de Codigo start = 0x%lx, end = 0x%lx\n"
        "Segmento de Dados  start = 0x%lx, end = 0x%lx\n"
        "Segmento de Heap   start = 0x%lx, end = 0x%lx\n"
        "Segmento de Pilha  start = 0x%lx\n",
        mm->start_code, mm->end_code,
        mm->start_data, mm->end_data,
        mm->start_brk, mm->brk,
        mm->start_stack);
}

static ssize_t segments_write(struct file *file, const char __user *buf,
                              size_t count, loff_t *ppos){
    char kbuf[16];
    int pid;
    struct pid *pid_struct;
    struct task_struct *task;

    if (count >= sizeof(kbuf))
        return -EINVAL;

    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;

    kbuf[count] = '\0';

    if (kstrtoint(strstrip(kbuf), 10, &pid))
        return -EINVAL;

    pid_struct = find_get_pid(pid);
    if (!pid_struct)
        return -ESRCH;

    task = get_pid_task(pid_struct, PIDTYPE_PID);
    put_pid(pid_struct);
    if (!task)
        return -ESRCH;

    printk("Processo: %s[%d]\n", task->comm, task->pid);
    print_mem(task);
    put_task_struct(task);

    return count;
}

static const struct file_operations segments_fops = {
    .write = segments_write,
};

static struct miscdevice segments_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "segments",
    .fops  = &segments_fops,
};

static int segments_load(void){
    return misc_register(&segments_dev);
}

static void segments_unload(void){
    misc_deregister(&segments_dev);
}

module_init(segments_load);
module_exit(segments_unload);

MODULE_AUTHOR("Guilherme Hoffmann, Endrew Soares, João Sbardelotto, George Rother");
MODULE_DESCRIPTION("Imprime os segmentos de memória de um processo via /dev/segments.");
MODULE_LICENSE("GPL");

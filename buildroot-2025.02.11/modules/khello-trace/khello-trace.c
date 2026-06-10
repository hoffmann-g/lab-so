/*
 * Lab 3.3 -- Atividade: khello (lab 2.1) com printk() substituído por
 * trace_printk(). A saída vai para o buffer em anel do ftrace, visível em
 * /sys/kernel/tracing/trace, em vez do console/dmesg.
 *
 * Uso:
 *   mount -t tracefs none /sys/kernel/tracing   (se ainda não montado)
 *   insmod khello-trace.ko
 *   cat /sys/kernel/tracing/trace
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Estudante");
MODULE_DESCRIPTION("KMOD Exemplo com trace_printk (ftrace)");
MODULE_VERSION("0.0.1");

static int hello_init(void)
{
	trace_printk("KERN_EMERG: Used for emergency messages, usually those that precede a crash.\n");
	trace_printk("KERN_ALERT: A situation requiring immediate action.\n");
	trace_printk("KERN_CRIT: Critical conditions, often related to serious hardware or software failures.\n");
	trace_printk("KERN_ERR: Used to report error conditions; device drivers often use KERN_ERR to report hardware difficulties.\n");
	trace_printk("KERN_WARNING: Warnings about problematic situations that do not, in themselves, create serious problems with the system.\n");
	trace_printk("KERN_NOTICE: Situations that are normal, but still worthy of note. A number of security-related conditions are reported at this level.\n");
	trace_printk("KERN_INFO: Informational messages. Many drivers print information about the hardware they find at startup time at this level.\n");
	trace_printk("KERN_DEBUG: Used for debugging messages.\n");

	return 0;
}

static void hello_exit(void)
{
	trace_printk("Goodbye, cruel world\n");
}

module_init(hello_init);
module_exit(hello_exit);

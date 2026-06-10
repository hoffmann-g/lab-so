#!/bin/sh
# Trabalho 3 -- SSTF vs FCFS disk scheduler test battery.
#
# Runs the 5 report cases (random, sequential, small/large queue, short/long
# timeout, write-heavy), loading the module with each case's parameters and
# printing, per case, how many dispatches happened due to "queue full" and the
# comparison of sectors traveled (WITHOUT vs WITH reordering).
#
# Usage: sstf-bench [n_ops] [n_procs]   (default: 5000 ops, 128 procs)
#
# Requirements: the test disk is /dev/sdb (--hdb sdb.bin from lab 3.2). A plain
# IDE disk reports nr_requests=1, which would never fill the scheduler queue.
# This script raises nr_requests AFTER attaching the scheduler (only then can
# the staging queue be deeper than the hardware depth), so the queue fills.

DEV=sdb                                   # test disk (--hdb sdb.bin)
SCHED=/sys/block/$DEV/queue/scheduler
NRREQ=128                                 # request-queue depth (>= largest queue_size)
KO=$(find /lib/modules -name sstf.ko | head -1)
export SSTF_DEV=/dev/$DEV                  # sstf-teste uses this device

# base parameters (fixed across cases, except what each case varies)
BS=4096          # block size (bytes)
DISK=262144      # disk size (blocks) = 1 GB
MINREQ=512
MAXREQ=4096
NOPS=${1:-5000}  # total operations
PROCS=${2:-128}  # concurrent processes (fork)

if [ ! -b /dev/$DEV ]; then
	echo "ERROR: /dev/$DEV does not exist. Start QEMU with --hdb sdb.bin."
	exit 1
fi
if [ -z "$KO" ]; then
	echo "ERROR: sstf.ko not found in /lib/modules."
	exit 1
fi

# silence the debug flood on the console (still recorded in dmesg)
OLD_PRINTK=$(cat /proc/sys/kernel/printk)
echo 1 > /proc/sys/kernel/printk

# $1=label  $2=queue_size  $3=max_wait_ms  $4=write_pct  $5=pattern(rand|seq)  $6=procs(optional)
run_case()
{
	label=$1; qs=$2; tw=$3; wr=$4; pat=$5; np=${6:-$PROCS}

	echo none > $SCHED 2>/dev/null      # make sure sstf is detached before removing
	rmmod sstf 2>/dev/null
	insmod $KO queue_size=$qs max_wait_ms=$tw debug=1
	dmesg -c >/dev/null 2>&1
	echo sstf > $SCHED                  # attach the scheduler FIRST...
	echo $NRREQ > /sys/block/$DEV/queue/nr_requests 2>/dev/null  # ...then deepen the queue

	if [ "$pat" = "seq" ]; then
		sstf-teste $BS $DISK $NOPS $wr $MINREQ $MAXREQ $np seq >/dev/null 2>&1
	else
		sstf-teste $BS $DISK $NOPS $wr $MINREQ $MAXREQ $np >/dev/null 2>&1
	fi

	echo none > $SCHED                  # triggers exit_sched -> prints the comparison

	full=$(dmesg | grep -c 'queue full')
	echo "=================================================================="
	echo " CASE $label"
	echo "   queue_size=$qs  max_wait_ms=$tw  write_pct=${wr}%  pattern=$pat  procs=$np"
	echo "   n_ops=$NOPS  disk=${DISK}blocks x ${BS}B  device=/dev/$DEV  nr_requests=$NRREQ"
	echo "   dispatches by 'queue full': $full"
	dmesg | grep -iE 'WITHOUT reordering|WITH reordering|reduction' | tail -3 | sed 's/^sstf:/  /'
	echo ""
}

echo "############################################################"
echo "#   T3 -- SSTF vs FCFS : test battery                      #"
echo "#   n_ops=$NOPS  procs=$PROCS  device=/dev/$DEV             "
echo "############################################################"
echo ""

#         label                                qs   tw  wr  pattern procs
run_case "1  - pure random"                   50   50  30  rand
run_case "2  - sequential"                    50   50  30  seq    1
run_case "3a - small queue (20)"              20   50  30  rand
run_case "3b - large queue (100)"            100   50  30  rand
run_case "4a - short timeout (20ms)"          50   20  30  rand
run_case "4b - long timeout (100ms)"          50  100  30  rand
run_case "5  - write-heavy (80%)"             50   50  80  rand

# cleanup
echo none > $SCHED 2>/dev/null
rmmod sstf 2>/dev/null
echo "$OLD_PRINTK" > /proc/sys/kernel/printk
echo "############################ end ###########################"

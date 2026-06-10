#!/bin/sh
# Trabalho 3 -- bateria de testes do escalonador SSTF vs FCFS.
#
# Executa os 5 casos do relatório (aleatório, sequencial, fila pequena/grande,
# timeout curto/longo, predomínio de escritas), carregando o módulo com os
# parâmetros de cada caso e imprimindo, por caso, quantos despachos ocorreram
# por "fila cheia" e a comparação de setores percorridos (SEM x COM reordenação).
#
# Uso: sstf-bench [n_ops] [n_procs]   (padrão: 5000 ops, 128 procs)
#
# Requisitos: o disco de teste precisa ter fila profunda (nr_requests > 1) para
# a fila do escalonador encher. Use um disco virtio-blk no QEMU:
#   -drive file=sdb.bin,if=virtio,format=raw    -> aparece como /dev/vda
# (o disco IDE de --hdb tem nr_requests=1 e a fila nunca enche).

DEV=vda                                   # disco de teste (virtio-blk)
SCHED=/sys/block/$DEV/queue/scheduler
KO=$(find /lib/modules -name sstf.ko | head -1)
export SSTF_DEV=/dev/$DEV                  # sstf-teste usa este device

# parâmetros base (fixos entre os casos, salvo o que cada caso varia)
BS=4096          # tamanho do bloco (bytes)
DISK=262144      # tamanho do disco (blocos) = 1 GB
MINREQ=512
MAXREQ=4096
NOPS=${1:-5000}  # operações totais
PROCS=${2:-128}  # processos concorrentes (fork)

if [ ! -b /dev/$DEV ]; then
	echo "ERRO: /dev/$DEV nao existe. Inicie o QEMU com o disco de teste em virtio:"
	echo "      -drive file=sdb.bin,if=virtio,format=raw"
	exit 1
fi
if [ -z "$KO" ]; then
	echo "ERRO: sstf.ko nao encontrado em /lib/modules."
	exit 1
fi

# silencia o flood do debug no console (continua sendo gravado no dmesg)
OLD_PRINTK=$(cat /proc/sys/kernel/printk)
echo 1 > /proc/sys/kernel/printk

# $1=rotulo  $2=queue_size  $3=max_wait_ms  $4=write_pct  $5=padrao(rand|seq)  $6=procs(opcional)
run_case()
{
	rotulo=$1; qs=$2; tw=$3; wr=$4; pat=$5; np=${6:-$PROCS}

	echo none > $SCHED 2>/dev/null      # garante o sstf desacoplado antes de remover
	rmmod sstf 2>/dev/null
	insmod $KO queue_size=$qs max_wait_ms=$tw debug=1
	dmesg -c >/dev/null 2>&1
	echo sstf > $SCHED

	if [ "$pat" = "seq" ]; then
		sstf-teste $BS $DISK $NOPS $wr $MINREQ $MAXREQ $np seq >/dev/null 2>&1
	else
		sstf-teste $BS $DISK $NOPS $wr $MINREQ $MAXREQ $np >/dev/null 2>&1
	fi

	echo none > $SCHED                  # dispara exit_sched -> imprime a comparacao

	cheias=$(dmesg | grep -c 'fila cheia')
	echo "=================================================================="
	echo " CASO $rotulo"
	echo "   queue_size=$qs  max_wait_ms=$tw  write_pct=${wr}%  padrao=$pat  procs=$np"
	echo "   n_ops=$NOPS  disco=${DISK}blocos x ${BS}B  device=/dev/$DEV"
	echo "   despachos por 'fila cheia': $cheias"
	dmesg | grep -iE 'SEM reordenacao|COM reordenacao|reducao' | tail -3 | sed 's/^sstf:/  /'
	echo ""
}

echo "############################################################"
echo "#   T3 -- SSTF vs FCFS : bateria de testes                 #"
echo "#   n_ops=$NOPS  procs=$PROCS  device=/dev/$DEV             "
echo "############################################################"
echo ""

#         rotulo                              qs   tw  wr  padrao procs
run_case "1  - aleatorio puro"                50   50  30  rand
run_case "2  - sequencial"                    50   50  30  seq    1
run_case "3a - fila pequena (20)"             20   50  30  rand
run_case "3b - fila grande (100)"            100   50  30  rand
run_case "4a - timeout curto (20ms)"          50   20  30  rand
run_case "4b - timeout longo (100ms)"         50  100  30  rand
run_case "5  - predominio de escritas (80%)"  50   50  80  rand

# limpeza
echo none > $SCHED 2>/dev/null
rmmod sstf 2>/dev/null
echo "$OLD_PRINTK" > /proc/sys/kernel/printk
echo "############################ fim ###########################"

#!/bin/sh

# Detecta o IP do host pela rota padrão (usado para configurar o gateway do guest)
HOST=$(ip route show | head -n 1 | awk '{print $9}')

# Substitui o placeholder [IP-DO-HOST] no template de rede pelo IP real do host,
# gerando o script de init de rede S41network-config
sed "s/\[IP-DO-HOST\]/$HOST/g" \
"$BASE_DIR/../custom-scripts/network-config" \
> "$BASE_DIR/../custom-scripts/S41network-config"

# Instala o script de rede no diretório de init do target (rootfs em construção)
cp "$BASE_DIR/../custom-scripts/S41network-config" \
"$BASE_DIR/target/etc/init.d"

chmod +x "$BASE_DIR/target/etc/init.d/S41network-config"

# Instala a aplicação hello e seu script de init
cp $BASE_DIR/../apps/hello $BASE_DIR/target/usr/bin
cp $BASE_DIR/../custom-scripts/S50hello $BASE_DIR/target/etc/init.d
chmod +x $BASE_DIR/target/etc/init.d/S50hello

# Cria a estrutura de diretórios do servidor web no rootfs
mkdir -p "$BASE_DIR/target/var/www/cgi-bin"

# Instala o index.html (faz redirect automático para /cgi-bin/monitor)
cp "$BASE_DIR/../custom-scripts/index.html" "$BASE_DIR/target/var/www/"

# Instala o script de init do httpd (S60 = sobe após a rede S41)
cp "$BASE_DIR/../custom-scripts/S60httpd" "$BASE_DIR/target/etc/init.d/"
chmod +x "$BASE_DIR/target/etc/init.d/S60httpd"

# Compila o CGI monitor usando o cross-compiler do Buildroot (-static = sem deps em runtime)
CC="$BASE_DIR/host/bin/i686-buildroot-linux-gnu-gcc"
$CC -static -o "$BASE_DIR/../apps/monitor" "$BASE_DIR/../apps/monitor.c"

# Instala o binário CGI que gera a página de monitoramento do sistema
cp "$BASE_DIR/../apps/monitor" "$BASE_DIR/target/var/www/cgi-bin/"
chmod +x "$BASE_DIR/target/var/www/cgi-bin/monitor"

# --- Cuberite (servidor Minecraft) desabilitado: extra, fora dos materiais ---
# Para reativar, descomente o bloco abaixo (libstdc++ é dependência só do Cuberite).
#
# # Copia libstdc++ para o target (necessário para o Cuberite)
# cp "$BASE_DIR/host/i686-buildroot-linux-gnu/lib/libstdc++.so.6.0.32" "$BASE_DIR/target/usr/lib/"
# ln -sf libstdc++.so.6.0.32 "$BASE_DIR/target/usr/lib/libstdc++.so.6"
#
# # Instala o Cuberite (servidor Minecraft) em /opt/cuberite
# mkdir -p "$BASE_DIR/target/opt/cuberite"
# cp -r "$BASE_DIR/../apps/cuberite/." "$BASE_DIR/target/opt/cuberite/"
# chmod +x "$BASE_DIR/target/opt/cuberite/Cuberite"
#
# # Cuberite é iniciado manualmente: cd /opt/cuberite && ./Cuberite

# Lab 3.3 -- monta o tracefs no boot para uso do ftrace (/sys/kernel/tracing)
FSTAB="$BASE_DIR/target/etc/fstab"
grep -q "/sys/kernel/tracing" "$FSTAB" || \
	echo "tracefs	/sys/kernel/tracing	tracefs	defaults	0	0" >> "$FSTAB"

make -C $BASE_DIR/../modules/hello/
make -C $BASE_DIR/../modules/mymodule_param/
make -C $BASE_DIR/../modules/procdriver
make -C $BASE_DIR/../modules/chardriver/
make -C $BASE_DIR/../modules/timerdriver/
make -C $BASE_DIR/../modules/waitdriver/

# Lab 3.3 -- khello com trace_printk (ftrace)
make -C $BASE_DIR/../modules/khello-trace/

# Trabalho 3 -- escalonador de disco SSTF
make -C $BASE_DIR/../modules/sstf/

# Compila o módulo pub/sub (T2) e o instala no rootfs (/lib/modules)
make -C $BASE_DIR/../modules/pubsub/

# Compila e instala a aplicação de teste do pub/sub (T2)
$CC -o "$BASE_DIR/../apps/pubsub-teste" "$BASE_DIR/../apps/pubsub-teste.c"
cp "$BASE_DIR/../apps/pubsub-teste" "$BASE_DIR/target/usr/bin/"
chmod +x "$BASE_DIR/target/usr/bin/pubsub-teste"

# Lab 3.1 -- Escalonamento de tempo real com SCHED_DEADLINE (apps/schedrt)
$CC -o "$BASE_DIR/target/usr/bin/schedrt-query" "$BASE_DIR/../apps/schedrt/query.c"
$CC -o "$BASE_DIR/target/usr/bin/schedrt-dl"    "$BASE_DIR/../apps/schedrt/dl.c"
$CC -pthread -o "$BASE_DIR/target/usr/bin/schedrt-multi" "$BASE_DIR/../apps/schedrt/multi.c"

# Lab 3.2 -- Escalonamento de E/S de disco (apps/disk-test)
$CC -o "$BASE_DIR/target/usr/bin/disk-test-raw"         "$BASE_DIR/../apps/disk-test/raw.c"
$CC -o "$BASE_DIR/target/usr/bin/disk-test-sector-read" "$BASE_DIR/../apps/disk-test/sector-read.c"
$CC -o "$BASE_DIR/target/usr/bin/disk-test-bench"       "$BASE_DIR/../apps/disk-test/bench.c"

# Trabalho 3 -- aplicação de teste do escalonador SSTF
$CC -o "$BASE_DIR/target/usr/bin/sstf-teste" "$BASE_DIR/../apps/sstf-teste.c"

# Trabalho 3 -- script de bateria de testes (SSTF vs FCFS)
cp "$BASE_DIR/../custom-scripts/sstf-bench.sh" "$BASE_DIR/target/usr/bin/sstf-bench"
chmod +x "$BASE_DIR/target/usr/bin/sstf-bench"

# Lab 4.1 -- estresse do sistema de alocação de memória (apps/mem-test)
$CC -o "$BASE_DIR/target/usr/bin/mem-test-malloc" "$BASE_DIR/../apps/mem-test/malloc.c"
$CC -o "$BASE_DIR/target/usr/bin/mem-test-write"  "$BASE_DIR/../apps/mem-test/write.c"
$CC -o "$BASE_DIR/target/usr/bin/mem-test-read"   "$BASE_DIR/../apps/mem-test/read.c"
$CC -o "$BASE_DIR/target/usr/bin/mem-test-ulimit" "$BASE_DIR/../apps/mem-test/ulimit.c"

# Lab 4.2 -- segmentos de processo (apps/segments + modules/segments)
$CC -o "$BASE_DIR/target/usr/bin/segments-address" "$BASE_DIR/../apps/segments/address.c"
$CC -o "$BASE_DIR/target/usr/bin/segments-symbols" "$BASE_DIR/../apps/segments/symbols.c"
make -C $BASE_DIR/../modules/segments

# Lab 4.3 -- app multithread p/ observar o escalonador com trace-cmd/KernelShark
$CC -pthread -o "$BASE_DIR/target/usr/bin/sched-threads" "$BASE_DIR/../apps/sched-threads/sched-threads.c"

# Trabalho 4 -- thread_runner
$CC -pthread -o "$BASE_DIR/target/usr/bin/thread_runner" "$BASE_DIR/../apps/thread_runner/thread_runner.c"

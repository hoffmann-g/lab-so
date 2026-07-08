# Trabalho 4 — Escalonamento de Processos com thread_runner

**Laboratório de Sistemas Operacionais — PUCRS**\
**Prof. Angelo Elias Dal Zotto**\
\
**Integrantes:**\
Guilherme Hoffmann\
Endrew Soares\
João Sbardelotto\
George Rother

---

## 1. Introdução

Este trabalho investiga o escalonador de processos do Linux através de um
programa multithread, o `thread_runner`, que serve como instrumento de
medição: cada thread escreve repetidamente seu caractere identificador em um
buffer global compartilhado, sob um mutex de espera ocupada. O padrão de
caracteres resultante no buffer — e a contagem de vezes que cada thread foi
efetivamente despachada para a CPU — funciona como um "traço" indireto do
comportamento do escalonador, que comparamos com trechos de trace do
`ftrace`/`trace-cmd` capturados durante a mesma execução.

O objetivo não é o programa em si, mas a experimentação: para cada política de
escalonamento (`SCHED_OTHER`, `SCHED_BATCH`, `SCHED_IDLE`, `SCHED_RR`,
`SCHED_FIFO`), formulamos uma hipótese sobre o padrão esperado a partir do
comportamento teórico da política, executamos o experimento e comparamos
hipótese com evidência, discutindo convergências e divergências.

## 2. Implementação (`apps/thread_runner/thread_runner.c`)

- **Buffer global e cursor**: um buffer de tamanho `buffer_kb * 1024` bytes é
  alocado antes da criação das threads; um ponteiro/índice global
  (`current_pos`) marca a próxima posição livre.
- **Mutex com espera ocupada**: implementado com `atomic_flag` — cada thread
  chama `atomic_flag_test_and_set()` em loop para adquirir a região crítica
  (retorna `false` quando conseguiu) e `atomic_flag_clear()` para liberá-la.
  Essas operações usam CMPXCHG atômico em x86, sem chamada de sistema.
- **Delay com espera ocupada**: `busy_wait_delay_ms()` usa
  `clock_gettime(CLOCK_MONOTONIC_RAW, ...)` em loop, comparando o tempo
  decorrido contra o alvo (1ms), sem `sleep()`/`nanosleep()`.
- **Política/prioridade**: configuradas com `sched_setscheduler()` na thread
  **principal**, antes de criar as threads. Como `pthread_create()` é chamado
  com atributo padrão (`PTHREAD_INHERIT_SCHED`), todas as threads criadas em
  seguida herdam automaticamente a mesma política/prioridade da thread
  principal. Para `SCHED_OTHER`/`BATCH`/`IDLE` a prioridade estática
  permanece 0 e o parâmetro `<prioridade>` é aplicado como nice value via
  `nice()`; para `SCHED_RR`/`SCHED_FIFO` o parâmetro é a prioridade de tempo
  real (1–99) passada em `struct sched_param`. Se `sched_setscheduler()` falha
  (por exemplo, política de tempo real sem privilégio), o programa **aborta**
  em vez de continuar silenciosamente sob outra política — importante para
  não coletar dados de um experimento que na prática rodou com configuração
  diferente da pretendida.
- **Reconstrução da sequência e da contagem de escalonamentos**: ao final,
  antes de qualquer poda/reprocessamento, a saída bruta do buffer é impressa.
  Em seguida, o buffer é varrido uma vez: cada **troca** de caractere em
  relação ao anterior indica uma nova rajada (run) contígua, ou seja, uma nova
  vez em que o escalonador despachou aquela thread para a CPU. A "sequência
  de execução" impressa é a lista ordenada dessas rajadas (ex.: `ABCDABCD...`)
  e a contagem por thread é o número de rajadas daquela thread — **não** o
  total de bytes escritos por ela. Essa distinção é essencial: no exemplo do
  próprio enunciado, cada thread escreve dezenas de caracteres consecutivos
  mas é contada como escalonada apenas **uma vez** (`A = 1`), porque só houve
  uma rajada de cada.

  > Esse ponto foi um bug real encontrado numa implementação inicial de
  > referência do grupo: contar `thread_counts[id]++` a cada byte escrito
  > produz o total de caracteres por thread (que soma sempre `buffer_size`),
  > não o número de vezes que o escalonador a despachou; e imprimir a
  > sequência apenas na ordem de criação das threads (`for i: putchar('A'+i)`)
  > não reflete o que de fato ocorreu no buffer. Ambos foram corrigidos
  > reconstruindo sequência/contagem a partir das rajadas contíguas do buffer
  > final, o que também torna a contagem verificável (soma das rajadas por
  > thread deve bater com o número de blocos visíveis no buffer impresso).

## 3. Metodologia experimental

- **Parâmetros fixos**: 4 threads (`A`,`B`,`C`,`D`), buffer de 8 KiB (8192
  bytes), delay de 1ms por escrita.
- **Matriz de experimentos**: as 5 políticas, cada uma com 2 prioridades, nas
  3 configurações de CPU do QEMU (1, 2 e 4 cores via `-smp N`) — 30
  combinações no total:

  | Política | Prioridades testadas |
  |---|---|
  | `SCHED_OTHER` | nice 0, nice 19 |
  | `SCHED_BATCH` | nice 0, nice 19 |
  | `SCHED_IDLE`  | nice 0, nice 19 |
  | `SCHED_RR`    | 1, 99 |
  | `SCHED_FIFO`  | 1, 99 |

- **Evidência coletada por execução**: (1) saída do `thread_runner` (buffer,
  sequência, contagens); (2) trace de `sched:sched_switch` capturado durante
  a mesma execução com `trace-cmd record -e sched:sched_switch -o
  <arquivo>.dat thread_runner ...`, exportado do rootfs para o host e
  analisável em texto (`trace-cmd report`) ou visualmente no KernelShark.
- Todas as 30 combinações rodaram como root dentro do QEMU (necessário para
  `SCHED_RR`/`SCHED_FIFO`).
- Os `.dat` de cada execução foram extraídos do `rootfs.ext2` para o host com
  `debugfs -R "dump /root/traces/<arquivo> <destino>" output/images/rootfs.ext2`
  (equivalente a montar a imagem e copiar, como no tutorial 4.3, mas sem
  precisar de privilégio de root no host) e ficam em `t4-traces/` na raiz do
  repositório. **A inspeção visual no KernelShark (capturas de tela) fica
  para o grupo fazer localmente** — os `.dat` já estão prontos para abrir
  (`kernelshark t4-traces/SCHED_RR_p1_2c.dat`, por exemplo); os trechos
  textuais usados na análise (seção 4.7) foram obtidos com
  `trace-cmd report -i <arquivo>.dat`.

## 4. Resultados e análise por política

Tabela consolidada das 30 execuções (número de rajadas/trocas observadas no
buffer e contagem por thread; buffer de 8192 bytes em todos os casos):

| Política | Prio | 1 core (trocas) | 1 core (A,B,C,D) | 2 cores (trocas) | 2 cores (A,B,C,D) | 4 cores (trocas) | 4 cores (A,B,C,D) |
|---|---|---|---|---|---|---|---|
| OTHER | 0  | 2031 | 509,504,508,510 | 8136 | 2049,2014,2017,2056 | 8192 | 2029,2056,2054,2053 |
| OTHER | 19 | 2042 | 509,510,513,510 | 8156 | 2005,1981,1948,2222 | 8192 | 2046,2050,2048,2048 |
| BATCH | 0  | 2033 | 509,506,508,510 | 8146 | 2058,2076,2006,2006 | 8192 | 2047,2047,2049,2049 |
| BATCH | 19 | 2042 | 511,510,510,511 | 8161 | 2264,1964,1972,1961 | 8192 | 2048,2050,2047,2047 |
| IDLE  | 0  | 2029 | 507,508,507,507 | 8137 | 2178,2164,1778,2017 | 8191 | 2050,2046,2048,2047 |
| IDLE  | 19 | 2036 | 509,510,508,509 | 8137 | 2062,2007,2052,2016 | 8192 | 2047,2048,2048,2049 |
| RR    | 1  | 82   | 21,21,20,20      | 8184 | 4091,1394,1398,1301 | 8192 | 2050,2044,2049,2049 |
| RR    | 99 | 83   | 21,21,21,20      | 8185 | 4091,1396,1398,1300 | 8192 | 2048,2049,2048,2047 |
| FIFO  | 1  | 1    | 1,0,0,0          | 8188 | 4094,4094,0,0        | 8192 | 2049,2048,2047,2048 |
| FIFO  | 99 | 1    | 1,0,0,0          | 8189 | 4095,4094,0,0        | 8192 | 2049,2048,2049,2046 |

### 4.1 SCHED_OTHER

**Hipótese:** `SCHED_OTHER` é a política padrão do CFS (Completely Fair
Scheduler), que distribui o tempo de CPU proporcionalmente ao peso (derivado
do nice value) entre as threads prontas. Com as 4 threads no mesmo nice value
simultaneamente, esperamos alternância frequente e aproximadamente igualitária
entre elas — muitas trocas pequenas, não poucas trocas grandes — já que o CFS
usa uma granularidade de escalonamento (`sched_min_granularity_ns`) da ordem
de poucos milissegundos, bem menor que o quantum fixo do RR. Quanto ao nice
value (0 vs. 19): como **todas** as 4 threads do processo compartilham o
mesmo nice a cada execução, a prioridade relativa *entre elas* não muda — o
nice absoluto só afetaria a disputa por CPU contra processos de sistema
(kworkers, sh, etc.), não a distribuição interna do próprio `thread_runner`.
Hipótese: nice 0 e nice 19 devem produzir buffers estatisticamente
equivalentes entre si.

**Resultado (1 core):** 2031–2042 trocas para um buffer de 8192 bytes (~4
bytes por rajada), com contagens quase idênticas entre as 4 threads (ex.:
nice 0 → A=509, B=504, C=508, D=510). O nice 19 produziu resultado
estatisticamente indistinguível (A=509, B=510, C=513, D=510). A sequência
observada é irregular, sem um padrão cíclico fixo (ex.:
`ABCDABCDABCDABCDABCDABDCABDCABDCABDCABDC...`).

**Análise:** confirma a hipótese — o CFS realmente intercala as 4 threads em
janelas muito menores que o quantum fixo do RR (~4 bytes ≈ poucos
milissegundos de CPU por rajada, compatível com a granularidade mínima do
CFS), e o nice value não altera a distribuição relativa entre threads
irmãs de mesmo peso, como esperado.

**Hipótese (efeito do número de cores):** com 2 ou 4 cores, mais de uma
thread pode estar genuinamente em execução ao mesmo tempo. Como o mutex de
`thread_runner` é uma seção crítica muito curta (uma escrita + incremento),
esperamos que o número de trocas visíveis no buffer **suba** bastante (cada
thread ganha e perde o mutex a cada iteração, então cada núcleo adicional
tende a gerar uma troca a mais por byte), aproximando-se do limite de uma
troca por byte (8192) à medida que mais threads rodam simultaneamente.

**Resultado (2 e 4 cores):** em 2 cores, 8136–8161 trocas (quase 1 por byte);
em 4 cores, 8192 trocas — o **máximo possível** para um buffer de 8192 bytes,
ou seja, toda escrita alterna de thread. Distribuição continua equilibrada
entre as 4 threads em ambos os casos.

**Análise:** confirma a hipótese, mas revela uma nuance importante: a partir
do momento em que há núcleos suficientes para mais de uma thread rodar em
paralelo, a alternância no buffer deixa de refletir *o escalonador
decidindo trocar quem usa a CPU* e passa a refletir *a disputa pelo mutex
entre threads que já estão, cada uma, rodando continuamente em seu próprio
núcleo*. Discutimos isso com mais detalhe na seção 4.6, pois o mesmo efeito
aparece em todas as políticas testadas com 4 cores.

### 4.2 SCHED_BATCH

**Hipótese:** `SCHED_BATCH` também é uma classe do CFS, pensada para cargas
não interativas: a principal diferença em relação a `SCHED_OTHER` é que
threads `BATCH` **não recebem o bônus de preempção por wakeup** (não
"furam a fila" ao acordar de um bloqueio). Como as threads do
`thread_runner` nunca dormem/bloqueiam de fato (a espera é ocupada, não
`sleep()`), esse workload não exercita a diferença entre as duas classes —
esperamos um padrão **estatisticamente equivalente ao `SCHED_OTHER`**, e não
uma diferença perceptível.

**Resultado (1 core):** 2033–2042 trocas, contagens equilibradas
(A=509,B=506,C=508,D=510 em nice 0; A=511,B=510,C=510,D=511 em nice 19) —
praticamente idêntico ao observado em `SCHED_OTHER`.

**Análise:** confirma a hipótese de equivalência. Este é um caso de
**divergência esperada e explicada, não um erro**: a ausência de diferença
entre `BATCH` e `OTHER` não indica falha do experimento, mas sim que o
workload escolhido (100% CPU-bound, sem sleep/wakeup) não exercita a
característica que distingue as duas classes.

**Resultado (2 e 4 cores):** mesmo padrão de `SCHED_OTHER` — 8146–8161
trocas em 2 cores, 8192 (máximo) em 4 cores, sempre com distribuição
equilibrada entre as 4 threads. `BATCH` permanece indistinguível de `OTHER`
em todas as configurações de CPU testadas, reforçando que a diferença entre
as duas classes só se manifestaria com um padrão de sleep/wakeup, ausente
neste workload.

### 4.3 SCHED_IDLE

**Hipótese:** `SCHED_IDLE` é a classe de menor peso possível no CFS —
threads `IDLE` só recebem CPU quando não há nenhuma outra tarefa
`SCHED_OTHER`/`BATCH` pronta para rodar. Como o sistema dentro do QEMU está
praticamente ocioso (sem tráfego de rede, sem I/O de disco durante o teste),
esperamos que as 4 threads `IDLE` do `thread_runner` ainda consigam
praticamente toda a CPU disponível na maior parte do tempo — produzindo um
padrão parecido com `SCHED_OTHER` —, mas com possíveis pausas maiores e
esporádicas quando threads de kernel (kworkers, timers) acordam e preemptam
brevemente.

**Resultado (1 core):** 2029–2036 trocas, contagens equilibradas
(A=507,B=508,C=507,D=507 em nice 0; A=509,B=510,C=508,D=509 em nice 19) —
novamente muito próximo de `SCHED_OTHER`/`BATCH`.

**Análise:** confirma a hipótese — num sistema ocioso, `SCHED_IDLE` não
distingue-se visivelmente de `SCHED_OTHER`, pois a condição que ativaria seu
comportamento diferenciado (concorrência real com tarefas de peso normal)
raramente ocorre. Divergência esperada e explicada da mesma forma que em
4.2.

**Resultado (2 e 4 cores):** 8137 trocas em 2 cores, 8191–8192 em 4 cores,
distribuição equilibrada — novamente equivalente a `SCHED_OTHER`/`BATCH` em
todas as configurações de CPU, pelo mesmo motivo: sem concorrência real por
CPU num sistema ocioso, a classe `IDLE` não é diferenciável das demais
classes CFS neste experimento.

### 4.4 SCHED_RR

**Hipótese:** `SCHED_RR` é uma política de tempo real com quantum fixo
(`/proc/sys/kernel/sched_rr_timeslice_ms`, padrão 100ms no Linux): threads de
mesma prioridade RR se revezam em round-robin, cada uma recebendo o quantum
inteiro antes de ceder a CPU à próxima, mesmo sem bloquear. Com 4 threads na
mesma prioridade, esperamos um padrão **cíclico e regular**
(`ABCDABCDABCD...`), com rajadas bem maiores e mais uniformes que no CFS —
da ordem de `quantum / delay` ≈ 100ms / 1ms ≈ **100 bytes por rajada**. Como
as 4 threads têm sempre a mesma prioridade entre si (o parâmetro
`<prioridade>` é aplicado a todas simultaneamente), o valor absoluto (1 ou
99) não deveria alterar o padrão — apenas a prioridade *relativa* entre
threads concorrentes importa no RR, e aqui não há concorrente de prioridade
diferente.

**Resultado (1 core):** exatamente **82–83 trocas** para 8192 bytes — ou
seja, ~99 bytes por rajada, batendo com precisão notável no valor teórico de
100ms/1ms. Contagens quase idênticas entre prioridade 1 e 99
(A=21,B=21,C=20,D=20 vs. A=21,B=21,C=21,D=20). Sequência perfeitamente
cíclica: `ABCDABCDABCDABCDABCD...` (repete continuamente até completar 21
ciclos).

**Análise:** confirma fortemente a hipótese, com uma coincidência numérica
que reforça a explicação teórica: o quantum default de 100ms do `SCHED_RR`
é visível diretamente na contagem de rajadas. A prioridade absoluta (1 vs.
99), como esperado, não influenciou o resultado — reforçando que no RR só a
prioridade *relativa* entre concorrentes importa.

**Hipótese (efeito do número de cores):** com 2 cores para 4 threads RR de
mesma prioridade, esperamos que 2 threads ocupem os 2 núcleos e o quantum de
100ms passe a governar apenas o **revezamento das 2 threads restantes** pelo
núcleo que sobrar (as 2 primeiras, uma vez em execução contínua e nunca
bloqueando, tendem a permanecer nos seus núcleos). Com 4 cores (= número de
threads), todas as 4 devem caber, uma por núcleo, sem nunca precisar do
quantum para revezar — o comportamento deve ficar indistinguível do
CFS em termos de ausência de starvation.

**Resultado (2 cores):** 8184–8185 trocas, mas com distribuição **desigual**:
a thread `A` participa de praticamente **metade** de todas as trocas
(4091/8184), enquanto `B`, `C` e `D` dividem a outra metade de forma
desigual (1394/1398/1301) — a sequência real mostra `A` alternando
constantemente com uma parceira que muda a cada ~100ms
(`...ABABAB...ACACAC...ADADAD...`, com trechos longos de `AB` intercalados
por trechos de `AC` e `AD`). Em 4 cores, 8192 trocas com distribuição quase
perfeitamente igual (~2048 cada).

**Análise:** parcialmente divergente da hipótese ingênua ("todas rodam
igual em multicore"), mas explicável: em 2 cores, a thread `A` conseguiu
"prender" um dos 2 núcleos para si (ficou sempre em execução, nunca perdeu
o núcleo, porque tecnicamente ao ceder o mutex ela continua *runnable* e o
escalonador não tem motivo para trocá-la enquanto o núcleo está ocupado só
por ela e mais ninguém de prioridade igual disputando *aquele* núcleo
especificamente), enquanto `B`, `C` e `D` se revezam no segundo núcleo a
cada ~100ms (quantum do RR). Isso é coerente com o funcionamento real do
balanceamento de carga do SMP: o RR não redistribui globalmente a cada
quantum, ele expira o quantum da thread corrente **naquele núcleo** e
escolhe a próxima da fila de prontos daquele núcleo — como `A` nunca é
colocada de volta na fila de prontos de outro núcleo (não bloqueia, não é
migrada), ela mantém seu núcleo indefinidamente, enquanto o outro núcleo
faz round-robin normal entre as 3 threads restantes. Em 4 cores, como cada
thread cabe em seu próprio núcleo, o quantum nunca chega a ser necessário e
o resultado converge para o mesmo padrão "1 troca por byte" das políticas
CFS (ver 4.6).

### 4.5 SCHED_FIFO

**Hipótese:** `SCHED_FIFO` é a política de tempo real **sem** quantum: uma
thread `FIFO` mantém a CPU até bloquear, terminar ou ser preemptada por uma
thread de prioridade *maior*. Como a espera ocupada do `thread_runner` nunca
bloqueia (não há `sleep()`/E/S), e as 4 threads têm a mesma prioridade FIFO,
prevemos um cenário extremo de **monopolização**: a primeira thread
despachada para a CPU nunca cede voluntariamente e preenche o buffer
**inteiro sozinha**, fazendo com que as demais 3 threads jamais sejam
escalonadas (contagem 0) — um caso de inanição (starvation) total em 1 core.
Como não há concorrente de prioridade diferente, o valor absoluto de
prioridade (1 ou 99) não deve mudar esse resultado.

**Resultado (1 core):** confirmado de forma extrema — **1 única rajada**:
sequência = `A`, contagens A=1, B=0, C=0, D=0, tanto em prioridade 1 quanto
99. A thread `A` (primeira criada/despachada) escreveu todo o buffer de 8192
bytes sozinha; `B`, `C` e `D` nunca chegaram a rodar.

**Análise:** confirma a hipótese de forma quase didática — é o contraste mais
nítido de todo o experimento entre `SCHED_RR` (round-robin, todas as threads
executam) e `SCHED_FIFO` (sem timeslicing entre iguais, uma única thread
monopoliza) em 1 core. Isso demonstra concretamente por que `SCHED_FIFO`
exige disciplina do próprio código de tempo real (yield/bloqueio explícito)
para não travar as demais threads de igual prioridade.

**Hipótese (efeito do número de cores):** como o `SCHED_FIFO` nunca expira
por tempo, a inanição das threads que "perdem a largada" deve ser
**permanente** enquanto houver menos núcleos que threads — ou seja,
esperamos que em 2 cores apenas 2 das 4 threads (as que conseguirem um
núcleo livre no instante da criação) executem, e as outras 2 fiquem com
contagem 0 durante toda a execução (starvation total, e não apenas parcial
como no RR, já que não há quantum para dar uma chance às demais). Com 4
cores, esperamos que as 4 caibam, uma por núcleo, e a starvation desapareça
completamente.

**Resultado (2 cores):** exatamente como previsto — 8188–8189 trocas, mas
**somente entre `A` e `B`** (4094/4094 e 4095/4094); `C` e `D` ficam com
contagem **0** em ambas as prioridades testadas. A sequência é um
`ABABABAB...` quase perfeito do início ao fim. Em 4 cores, 8192 trocas com
distribuição perfeitamente igual entre as 4 threads (~2048 cada), sequência
cíclica `ABCDABCDABCD...` — nenhuma starvation.

**Análise:** confirma integralmente a hipótese, e é o resultado mais
demonstrativo de todo o trabalho: `SCHED_FIFO` produz starvation **total e
permanente** de `C`/`D` em 2 cores (diferente da starvation apenas parcial
observada no `SCHED_RR` na mesma configuração, seção 4.4), porque não existe
mecanismo de quantum que force `A` e `B` a devolver seus núcleos. Isso só
deixa de acontecer quando o número de núcleos alcança o número de threads
(4 cores) — nesse ponto, a diferença entre `FIFO`, `RR` e as classes CFS
desaparece por completo (ver 4.6), pois nenhuma política real precisa
"decidir" quem executa quando todo mundo tem seu próprio núcleo.

### 4.6 Efeito do número de núcleos: escalonador vs. contenção de mutex

Um achado transversal a todas as políticas merece destaque próprio. Em 1
núcleo, o padrão de trocas no buffer é uma medida direta de **quando o
escalonador troca qual thread está executando** — é por isso que políticas
diferentes produzem números de trocas tão distintos (de 1, no `FIFO`, a mais
de 2000, no `OTHER`). Já quando o número de núcleos é suficiente para que
mais de uma thread rode continuamente em paralelo (2 ou, principalmente, 4
núcleos com 4 threads), cada thread que "ganha" um núcleo dedicado passa a
executar o laço inteiro **sem nunca ser trocada pelo escalonador** — a
alternância que aparece no buffer deixa de ser causada pelo escalonador e
passa a ser causada pela **disputa pelo mutex** (`atomic_flag`) entre threads
que já estão todas rodando ao mesmo tempo, cada uma tentando escrever assim
que termina seu próprio delay de 1ms. É por isso que, em 4 núcleos, **todas
as cinco políticas convergem para praticamente o mesmo resultado** (8191–8192
trocas, distribuição quase perfeitamente igual): a variável determinante
deixou de ser a política de escalonamento e passou a ser a granularidade do
mutex e o tempo de delay, que são iguais para todas as threads
independentemente da política. Esse é um exemplo concreto de por que a
avaliação de uma política de escalonamento deve necessariamente considerar a
relação entre número de threads e número de núcleos: o mesmo programa, com o
mesmo código, mede coisas conceitualmente diferentes dependendo dessa
relação.

### 4.7 Evidência de trace (`sched:sched_switch`)

Trechos abaixo, obtidos com `trace-cmd report -i <arquivo>.dat | grep
thread_runner`, sustentam diretamente as conclusões das seções 4.4–4.6.

**`SCHED_RR`, prioridade 1, 2 núcleos** (`SCHED_RR_p1_2c.dat`) — o trecho mais
revelador de todo o experimento. No CPU 1, três PIDs de `thread_runner`
(172, 173, 174 — três das quatro threads) se revezam em intervalos quase
exatamente iguais a **100ms**, enquanto no CPU 0 uma quarta thread (PID 171)
permanece em execução contínua, interrompida apenas por threads de kernel
(`kworker`, `kcompactd0`, `rcu_preempt`) — nunca por outra thread do próprio
`thread_runner`:

```
thread_runner-172 [001] ... 46.421806: sched_switch: thread_runner:172 [98] R ==> thread_runner:173 [98]
thread_runner-173 [001] ... 46.521705: sched_switch: thread_runner:173 [98] R ==> thread_runner:174 [98]
thread_runner-174 [001] ... 46.621683: sched_switch: thread_runner:174 [98] R ==> thread_runner:172 [98]
thread_runner-172 [001] ... 46.721706: sched_switch: thread_runner:172 [98] R ==> thread_runner:173 [98]
thread_runner-173 [001] ... 46.821678: sched_switch: thread_runner:173 [98] R ==> thread_runner:174 [98]
thread_runner-174 [001] ... 46.921716: sched_switch: thread_runner:174 [98] R ==> thread_runner:172 [98]
...
thread_runner-171 [000] ... 47.275006: sched_switch: thread_runner:171 [98] R ==> kworker/0:0:9 [120]
thread_runner-171 [000] ... 47.301212: sched_switch: thread_runner:171 [98] R ==> kworker/u8:0:12 [120]
```

Os *timestamps* confirmam, no nível do kernel, exatamente o que a análise da
seção 4.4 inferiu apenas a partir da contagem do buffer: o quantum de 100ms
do `SCHED_RR` governa o revezamento no CPU 1, enquanto o CPU 0 fica
"reservado" para uma única thread, que só cede a CPU a threads de kernel,
nunca a outra thread de `thread_runner`.

**`SCHED_FIFO`, prioridade 1, 2 núcleos** (`SCHED_FIFO_p1_2c.dat`) — as
únicas trocas de contexto **entre** threads de `thread_runner` acontecem uma
única vez, no início da execução (PID 188 cede para 190); depois disso, os
PIDs 189 e 190 alternam apenas com processos de kernel (nunca entre si nem
com outras threads de `thread_runner`, que sequer aparecem no trace),
confirmando a starvation total de duas das quatro threads:

```
thread_runner-188 [000] ... 60.684413: sched_switch: thread_runner:188 [98] S ==> thread_runner:190 [98]
thread_runner-190 [000] ... 61.633843: sched_switch: thread_runner:190 [98] R ==> kworker/0:0:9 [120]
thread_runner-189 [001] ... 61.634720: sched_switch: thread_runner:189 [98] R ==> rcu_preempt:17 [120]
thread_runner-190 [000] ... 62.142119: sched_switch: thread_runner:190 [98] R ==> kcompactd0:33 [120]
```

## 5. Conclusão

Os experimentos confirmam, com evidência quantitativa nítida, o
comportamento teórico de cada classe de escalonamento do Linux:

- **`SCHED_OTHER`, `SCHED_BATCH` e `SCHED_IDLE`** (CFS) produziram resultados
  estatisticamente indistinguíveis entre si em todas as configurações de
  núcleos testadas — confirmando a hipótese de que, sem um padrão de
  sleep/wakeup ou concorrência real com tarefas de peso diferente, essas três
  classes não se diferenciam neste workload 100% CPU-bound. Não é uma falha
  do experimento: é uma consequência esperada e explicada da natureza do
  workload.
- **`SCHED_RR`** revelou o quantum fixo de 100ms de forma quase exata em 1
  núcleo (~99 bytes/rajada) e, em 2 núcleos, um comportamento assimétrico
  interessante (uma thread "prende" um núcleo inteiro para si enquanto as
  outras três se revezam no núcleo restante) — uma divergência da hipótese
  ingênua de igualdade total, mas plenamente explicável pelo funcionamento
  do balanceamento de carga do SMP.
- **`SCHED_FIFO`** produziu o resultado mais extremo: monopolização total em
  1 núcleo e starvation permanente de metade das threads em 2 núcleos —
  starvation mais severa que a do `SCHED_RR` na mesma configuração, por não
  haver quantum. Esse contraste FIFO×RR em multiprocessamento é a evidência
  mais clara de todo o trabalho sobre a diferença prática entre as duas
  políticas de tempo real.
- **O número de núcleos** foi, na prática, a variável que mais mudou o
  significado da medição: com núcleos suficientes para acomodar todas as
  threads, a política de escalonamento deixa de ser o fator determinante do
  padrão observado, que passa a refletir a disputa pelo mutex da aplicação
  (seção 4.6) — um lembrete de que a interpretação de qualquer experimento
  de escalonamento deve levar em conta a relação entre grau de paralelismo
  do hardware e número de threads concorrentes.
- Em nenhum experimento a prioridade absoluta (nice 0 vs. 19; RR/FIFO 1 vs.
  99) alterou o resultado de forma perceptível, confirmando a expectativa
  teórica de que só a prioridade **relativa** entre threads concorrentes
  importa — e, neste programa, as 4 threads de cada execução sempre
  compartilham a mesma prioridade.

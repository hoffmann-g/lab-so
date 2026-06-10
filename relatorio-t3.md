# Trabalho 3 — Escalonador de Disco SSTF

**Laboratório de Sistemas Operacionais — PUCRS**
Integrantes: _(preencher)_

---

## 1. Introdução

Este trabalho implementa um escalonador de E/S de disco para o kernel Linux
seguindo a política **SSTF (Shortest Seek Time First)**: a cada despacho, dentre
as requisições pendentes, escolhe-se a que possui o setor **mais próximo da
posição atual do cabeçote**, minimizando a movimentação total.

A implementação é feita como um módulo carregável sobre a infraestrutura
**blk-mq** (Multi-Queue Block IO), registrando um `struct elevator_type` via
`elv_register()`. O escalonador resultante aparece e pode ser selecionado em
`/sys/block/<dev>/queue/scheduler`.

O objetivo da avaliação é comparar o SSTF com a política **FCFS (none)**, que
atende as requisições na ordem de chegada. Como o SSTF reordena as requisições
por proximidade, espera-se uma redução no **total de setores percorridos** pelo
cabeçote.

## 2. Implementação

### 2.1 Módulo (`modules/sstf/sstf.c`)

Callbacks implementadas (`elevator_mq_ops`):

- `sstf_init_sched` — aloca a fila via `elevator_alloc()`, inicializa a estrutura
  de dados do SSTF (lista de pendentes, posição do cabeçote, mutex/spinlock,
  hrtimer), sinaliza `QUEUE_FLAG_SQ_SCHED` e aponta `q->elevator`.
- `sstf_exit_sched` — cancela o timer, libera toda a memória e imprime a
  comparação final (em modo debug).
- `sstf_insert_requests` — retira cada requisição da lista recebida (campo
  `queuelist`) e a insere na fila interna; (re)arma o timer; marca a fila como
  pronta quando atinge `queue_size`.
- `sstf_dispatch_request` — percorre a fila e retorna a requisição de menor
  distância (`blk_rq_pos`) em relação ao cabeçote, removendo-a; `NULL` se vazia.
- `sstf_has_work` — retorna verdadeiro **somente** quando a fila encheu ou o
  timer expirou.
- `sstf_finish_request` — vazia (apenas presente).

**Parâmetros configuráveis em tempo de carga** (`module_param`):

| Parâmetro | Descrição | Faixa recomendada |
|---|---|---|
| `queue_size` | nº de requisições enfileiradas antes do despacho | 20–100 |
| `max_wait_ms` | tempo máximo de espera antes de despachar fila não cheia | 20–100 ms |
| `debug` | mensagens de comportamento no log do kernel | 0/1 |

**Sincronização:** a fila do SSTF é protegida por uma *spinlock*. Como o
`hrtimer` executa em contexto de interrupção, todos os acessos usam
`spin_lock_irqsave`/`spin_unlock_irqrestore`, evitando condições de corrida entre
`insert_requests`, `dispatch_request` e o callback do timer.

### 2.2 Aplicação de teste (`apps/sstf-teste.c`)

Cria, via `fork()`, múltiplos processos que geram requisições de leitura e
escrita em blocos do disco, cobrindo toda a sua extensão. Parâmetros:
tamanho do bloco, tamanho do disco (blocos), nº de operações, percentual de
escritas, tamanho mín./máx. da requisição, nº de processos e (opcional) padrão
de acesso (`rand`/`seq`).

O acesso usa **`O_DIRECT`**, de modo que cada `read()/write()` se torne uma
requisição imediata ao escalonador (sem ser absorvida/adiada pela *page cache*).

### 2.3 Ambiente de medição

Distribuição gerada com Buildroot, executada no QEMU. O disco de teste é exposto
como **virtio-blk** (`/dev/vda`), que possui fila de requisições profunda
(`nr_requests` ≫ 1) — condição necessária para que a fila do escalonador encha.
Um disco IDE (`--hdb`) possui `nr_requests = 1` e nunca permitiria acumular
requisições para reordenação.

### 2.4 Metodologia de comparação

Para cada execução, o módulo contabiliza, com o mesmo fluxo de requisições:

- **SEM reordenação (FCFS):** soma das distâncias entre setores na **ordem de
  chegada** das requisições;
- **COM reordenação (SSTF):** soma das distâncias na **ordem de atendimento**
  escolhida pela política SSTF.

Medir ambas a partir do mesmo fluxo de chegada é mais preciso do que executar o
escalonador `none` separadamente, pois elimina a variação do padrão aleatório e
a interferência do escalonador do SO hospedeiro entre execuções.

Todos os casos foram executados com a bateria automatizada `sstf-bench`
(`apps`/`custom-scripts`), com **1000 operações** e disco de **262144 blocos de
4096 B (1 GB)**.

## 3. Resultados

| Caso | queue_size | max_wait_ms | escritas | padrão | procs | FCFS (setores) | SSTF (setores) | Redução |
|---|---|---|---|---|---|---|---|---|
| 1 — aleatório puro      |  50 |  50 | 30% | rand |  128 | 643.973.480 | 42.445.216 | **93,4%** |
| 2 — sequencial          |  50 |  50 | 30% | seq  |    1 |       7.992 |      7.992 | **0,0%**  |
| 3a — fila pequena       |  20 |  50 | 30% | rand |  128 | 628.482.704 | 93.859.968 | **85,1%** |
| 3b — fila grande        | 100 |  50 | 30% | rand |  128 | 633.628.776 | 30.089.216 | **95,2%** |
| 4a — timeout curto      |  50 |  20 | 30% | rand |  128 | 603.157.280 | 49.983.696 | **91,7%** |
| 4b — timeout longo      |  50 | 100 | 30% | rand |  128 | 610.517.128 | 46.103.560 | **92,5%** |
| 5 — escritas predomin.  |  50 |  50 | 80% | rand |  128 | 661.053.384 | 39.431.512 | **94,0%** |

(Redução = (FCFS − SSTF) / FCFS.)

## 4. Discussão por caso

### Caso 1 — Acesso aleatório puro
Com endereços uniformemente distribuídos por todo o disco e alta concorrência, a
fila do SSTF acumula dezenas de requisições espalhadas. Ordená-las por
proximidade transforma uma sequência de saltos longos (FCFS) em uma varredura
curta e local, reduzindo os setores percorridos em **93,4%**. É o cenário de
ganho máximo: quanto mais aleatória a chegada, maior a diferença entre atender na
ordem de chegada e atender pelo vizinho mais próximo.

### Caso 2 — Acesso sequencial
Com um único processo gerando endereços **crescentes e contíguos**, a ordem de
chegada já é a ordem ótima de atendimento. FCFS e SSTF percorrem exatamente os
mesmos 7.992 setores → **redução de 0%**. O SSTF não tem o que melhorar quando o
padrão já é sequencial; seu custo de reordenação só compensa quando há
aleatoriedade. (Usou-se 1 processo justamente para preservar a natureza
sequencial — com alta concorrência, vários fluxos sequenciais se intercalam e o
agregado volta a parecer aleatório.)

### Caso 3 — Fila pequena (20) vs. fila grande (100)
A redução cresce de **85,1%** (fila 20) para **95,2%** (fila 100). Quanto maior a
fila, mais requisições o escalonador enxerga de uma só vez e melhor é a escolha
do vizinho mais próximo — uma janela maior aproxima o comportamento de um
ordenamento global. Com fila pequena, a reordenação é mais "míope" (otimiza
apenas dentro de lotes de 20), deixando saltos maiores entre lotes consecutivos.

### Caso 4 — Timeout curto (20 ms) vs. longo (100 ms)
As reduções são quase idênticas (**91,7%** vs. **92,5%**). Com alta concorrência
(128 processos), a fila atinge `queue_size` (50) **antes** de o timer expirar, de
modo que o despacho é disparado pelo enchimento da fila, e não pelo tempo — daí o
`max_wait_ms` ter pouca influência neste regime. O timeout é decisivo apenas
quando a carga é baixa e a fila demora a encher: ele garante que requisições não
fiquem represadas indefinidamente, ao custo de despachar lotes menores (menos
reordenação). É o compromisso latência × eficiência.

### Caso 5 — Predominância de escritas (80%)
O resultado (**94,0%**) é praticamente o mesmo do caso 1 (predominância de
leituras). Do ponto de vista do escalonador de disco, leitura e escrita são
apenas requisições com um setor-alvo; a distância de busca não depende da
direção da operação. Portanto, o ganho do SSTF é governado pela **distribuição
espacial** das requisições, não pela proporção leitura/escrita.

## 5. Conclusão

O módulo SSTF cumpre o ciclo de vida completo (registro, seleção via sysfs,
liberação em `exit_sched`), despacha por enchimento da fila e por timeout (ambos
parametrizáveis), e implementa corretamente a escolha pelo menor deslocamento do
cabeçote. Os experimentos confirmam o comportamento esperado: ganho elevado em
cargas aleatórias (até ~95%), nulo em cargas sequenciais, crescente com o tamanho
da fila, e independente da proporção de escritas. O timeout mostrou-se relevante
apenas sob baixa concorrência, atuando como salvaguarda contra represamento.

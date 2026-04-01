#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>

#define BUFSIZE  16384
#define SMALLBUF 512

/* ── Helpers ── */

static int read_file(const char *path, char *buf, int size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { buf[0] = '\0'; return -1; }
    int n = read(fd, buf, size - 1);
    if (n < 0) n = 0;
    buf[n] = '\0';
    close(fd);
    return n;
}

static void format_seconds(long total, char *out) {
    long days  = total / 86400; total %= 86400;
    long hours = total / 3600;  total %= 3600;
    long mins  = total / 60;    total %= 60;
    sprintf(out, "%ldd %ldh %ldm %lds", days, hours, mins, total);
}

/* Extrai valor de linhas no formato "Chave[:\t ]+Valor\n" */
static void extract_field(const char *buf, const char *key, char *out, int outsize) {
    const char *p = strstr(buf, key);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(key);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    int i = 0;
    while (*p && *p != '\n' && i < outsize - 1)
        out[i++] = *p++;
    out[i] = '\0';
}

/* ── HTML helpers ── */

static void row(const char *label, const char *value) {
    printf("<tr><td>%s</td><td>%s</td></tr>", label, value);
}

static void rowf(const char *label, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    row(label, tmp);
}

/* ── Sections ── */

static void section_kernel(void) {
    char buf[BUFSIZE];
    read_file("/proc/version", buf, BUFSIZE);
    char *nl = strchr(buf, '\n'); if (nl) *nl = '\0';
    printf("<h2>Kernel</h2><table>");
    row("Vers&atilde;o", buf);
    printf("</table>");
}

static void section_uptime(void) {
    char buf[BUFSIZE];
    read_file("/proc/uptime", buf, BUFSIZE);
    double up_f = 0, idle_f = 0;
    sscanf(buf, "%lf %lf", &up_f, &idle_f);
    char up_s[64], idle_s[64];
    format_seconds((long)up_f, up_s);
    format_seconds((long)idle_f, idle_s);
    printf("<h2>Uptime</h2><table>");
    row("Uptime",       up_s);
    row("Tempo ocioso", idle_s);
    printf("</table>");
}

static void section_datetime(void) {
    char buf[BUFSIZE];
    printf("<h2>Data e Hora</h2>");
    if (read_file("/proc/driver/rtc", buf, BUFSIZE) < 0)
        printf("<pre>RTC n&atilde;o dispon&iacute;vel (habilite o driver no kernel)</pre>");
    else
        printf("<pre>%s</pre>", buf);
}

static void section_cpu(void) {
    char buf[BUFSIZE];
    read_file("/proc/cpuinfo", buf, BUFSIZE);

    char model[128] = "N/A", mhz[32] = "N/A";
    int cores = 0;
    /* conta ocorrências de "processor\t:" para determinar núcleos */
    const char *p = buf;
    while ((p = strstr(p, "processor")) != NULL) { cores++; p++; }

    extract_field(buf, "model name", model, sizeof(model));
    extract_field(buf, "cpu MHz",    mhz,   sizeof(mhz));

    printf("<h2>Processador</h2><table>");
    row("Modelo",     model);
    rowf("Velocidade", "%s MHz", mhz);
    rowf("N&uacute;cleos", "%d", cores);
    printf("</table>");
}

static void section_load(void) {
    char buf[BUFSIZE];
    read_file("/proc/loadavg", buf, BUFSIZE);
    float l1 = 0, l5 = 0, l15 = 0;
    sscanf(buf, "%f %f %f", &l1, &l5, &l15);
    printf("<h2>Carga do Sistema</h2><table>");
    rowf("1 min",  "%.2f", l1);
    rowf("5 min",  "%.2f", l5);
    rowf("15 min", "%.2f", l15);
    printf("</table>");
}

static void section_cpu_usage(void) {
    char buf[BUFSIZE];
    unsigned long u1,n1,s1,i1,w1,q1,sq1;
    unsigned long u2,n2,s2,i2,w2,q2,sq2;

    read_file("/proc/stat", buf, BUFSIZE);
    sscanf(buf, "cpu %lu %lu %lu %lu %lu %lu %lu",
           &u1, &n1, &s1, &i1, &w1, &q1, &sq1);
    usleep(200000);
    read_file("/proc/stat", buf, BUFSIZE);
    sscanf(buf, "cpu %lu %lu %lu %lu %lu %lu %lu",
           &u2, &n2, &s2, &i2, &w2, &q2, &sq2);

    unsigned long idle  = i2 - i1;
    unsigned long total = (u2+n2+s2+i2+w2+q2+sq2) - (u1+n1+s1+i1+w1+q1+sq1);
    float pct = (total > 0) ? 100.0f * (total - idle) / total : 0.0f;

    printf("<h2>Uso do Processador</h2><table>");
    rowf("CPU", "%.1f%%", pct);
    printf("</table>");
}

static void section_memory(void) {
    char buf[BUFSIZE];
    read_file("/proc/meminfo", buf, BUFSIZE);

    long total_kb = 0, avail_kb = 0;
    char *line = buf;
    while (*line) {
        if      (strncmp(line, "MemTotal:",     9)  == 0) sscanf(line, "MemTotal: %ld",     &total_kb);
        else if (strncmp(line, "MemAvailable:", 13) == 0) sscanf(line, "MemAvailable: %ld", &avail_kb);
        while (*line && *line != '\n') line++;
        if (*line) line++;
    }

    printf("<h2>Mem&oacute;ria RAM</h2><table>");
    rowf("Total", "%ld MB", total_kb / 1024);
    rowf("Usada", "%ld MB", (total_kb - avail_kb) / 1024);
    printf("</table>");
}

static void section_io(void) {
    char buf[BUFSIZE];
    read_file("/proc/diskstats", buf, BUFSIZE);

    printf("<h2>Opera&ccedil;&otilde;es de E/S</h2><table>");
    printf("<tr><td><b>Dispositivo</b></td><td><b>Leituras</b></td><td><b>Escritas</b></td></tr>");

    char *line = buf;
    while (*line) {
        unsigned int maj, min_dev;
        char devname[32];
        unsigned long f1,f2,f3,f4,f5,f6,f7,f8,f9,f10,f11;
        if (sscanf(line, " %u %u %31s %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
                   &maj, &min_dev, devname,
                   &f1,&f2,&f3,&f4,&f5,&f6,&f7,&f8,&f9,&f10,&f11) >= 8) {
            /* ignora loop (l) e ram (r); mostra disco principal (minor == 0) */
            if (devname[0] != 'l' && devname[0] != 'r' && min_dev == 0) {
                printf("<tr><td>%s</td><td>%lu</td><td>%lu</td></tr>",
                       devname, f1, f5);
            }
        }
        while (*line && *line != '\n') line++;
        if (*line) line++;
    }
    printf("</table>");
}

static void section_filesystems(void) {
    char buf[BUFSIZE];
    read_file("/proc/filesystems", buf, BUFSIZE);
    printf("<h2>Sistemas de Arquivos Suportados</h2><pre>%s</pre>", buf);
}

static void section_devices(void) {
    char buf[BUFSIZE];
    read_file("/proc/devices", buf, BUFSIZE);
    printf("<h2>Dispositivos (Caracter, Bloco e Grupos)</h2><pre>%s</pre>", buf);
}

static void section_network(void) {
    char buf[BUFSIZE];
    read_file("/proc/net/dev", buf, BUFSIZE);
    printf("<h2>Dispositivos de Rede</h2><pre>%s</pre>", buf);
}

static void section_processes(void) {
    printf("<h2>Processos em Execu&ccedil;&atilde;o</h2>");
    printf("<table><tr><td><b>PID</b></td><td><b>Nome</b></td></tr>");

    DIR *proc_dir = opendir("/proc");
    if (!proc_dir) { printf("</table>"); return; }

    struct dirent *entry;
    while ((entry = readdir(proc_dir)) != NULL) {
        int is_pid = 1;
        for (int i = 0; entry->d_name[i]; i++)
            if (!isdigit((unsigned char)entry->d_name[i])) { is_pid = 0; break; }
        if (!is_pid) continue;

        char path[64], sbuf[SMALLBUF], name[64] = "";
        snprintf(path, sizeof(path), "/proc/%s/status", entry->d_name);
        if (read_file(path, sbuf, sizeof(sbuf)) < 0) continue;
        extract_field(sbuf, "Name:", name, sizeof(name));
        printf("<tr><td>%s</td><td>%s</td></tr>", entry->d_name, name);
    }
    closedir(proc_dir);
    printf("</table>");
}

/* ── Main ── */

int main(void) {
    printf("Content-Type: text/html\r\n\r\n");
    printf("<!DOCTYPE html><html><head><meta charset='utf-8'><title>Monitor</title>");
    printf("<style>"
           "body{font-family:monospace;background:#1a1a2e;color:#e0e0e0;padding:2em;margin:0}"
           "h1{color:#00d4aa}"
           ".sub{color:#888;margin-bottom:2em}"
           "h2{color:#7eb8f7;border-left:3px solid #00d4aa;padding-left:.5em;margin-top:1.5em}"
           "table{border-collapse:collapse;width:100%%;max-width:900px}"
           "td{padding:5px 14px;border-bottom:1px solid #2a2a3e}"
           "td:first-child{color:#aaa;width:220px}"
           "td:last-child{color:#ffd700}"
           "pre{background:#0d1117;padding:1em;border-radius:4px;color:#c9d1d9;overflow-x:auto}"
           "</style></head><body>");
    printf("<h1>System Monitor</h1><p class='sub'>PUCRS &mdash; Lab SO</p>");

    section_kernel();
    section_uptime();
    section_datetime();
    section_cpu();
    section_load();
    section_cpu_usage();
    section_memory();
    section_io();
    section_filesystems();
    section_devices();
    section_network();
    section_processes();

    printf("</body></html>");
    return 0;
}

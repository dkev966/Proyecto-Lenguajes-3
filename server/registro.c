/*
 * registro.c — Implementación del registro seguro para hilos y procesos.
 */
#include "registro.h"
#include "estado.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static const char *g_modo = "-";
static int g_fd_info = STDOUT_FILENO;
static int g_fd_error = STDERR_FILENO;
static int g_fd_archivo = -1;
static int g_verboso = 0;

int registro_iniciar(const char *modo, const char *ruta, int verboso)
{
    g_modo = modo;
    g_verboso = verboso;
    if (ruta != NULL) {
        g_fd_archivo = open(ruta, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (g_fd_archivo < 0)
            return -1;
        g_fd_info = g_fd_error = g_fd_archivo;
    }
    return 0;
}

void registro_cerrar(void)
{
    if (g_fd_archivo >= 0) {
        close(g_fd_archivo);
        g_fd_archivo = -1;
        g_fd_info = STDOUT_FILENO;
        g_fd_error = STDERR_FILENO;
    }
}

static void escribir_todo(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t k = write(fd, p, n);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        p += k;
        n -= (size_t)k;
    }
}

FORMATO_PRINTF(4, 0)
static void emitir(int fd, const char *nivel, const char *ip, const char *fmt, va_list ap)
{
    char linea[1024];
    struct timespec ts;
    struct tm tm;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);

    size_t n = strftime(linea, sizeof linea, "[%Y-%m-%d %H:%M:%S", &tm);
    int k = snprintf(linea + n, sizeof linea - n, ".%03ld] [%s] [pid %d tid %ld] %s%s%s%s",
                     ts.tv_nsec / 1000000, g_modo, (int)getpid(), (long)syscall(SYS_gettid),
                     nivel, ip ? "[" : "", ip ? ip : "", ip ? "] " : "");
    if (k > 0)
        n += (size_t)k;
    if (n < sizeof linea) {
        k = vsnprintf(linea + n, sizeof linea - n, fmt, ap);
        if (k > 0)
            n += (size_t)k;
    }
    if (n >= sizeof linea - 1)
        n = sizeof linea - 2;   /* línea truncada si era más larga */
    linea[n++] = '\n';

    estado_bloquear_registro();
    escribir_todo(fd, linea, n);
    estado_desbloquear_registro();
}

void reg_info(const char *ip, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emitir(g_fd_info, "", ip, fmt, ap);
    va_end(ap);
}

void reg_error(const char *ip, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emitir(g_fd_error, "ERROR ", ip, fmt, ap);
    va_end(ap);
}

void reg_debug(const char *ip, const char *fmt, ...)
{
    if (!g_verboso)
        return;
    va_list ap;
    va_start(ap, fmt);
    emitir(g_fd_info, "debug ", ip, fmt, ap);
    va_end(ap);
}

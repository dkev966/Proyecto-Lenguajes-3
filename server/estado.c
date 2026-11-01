/*
 * estado.c — Región de memoria compartida con contadores y mutex robustos.
 *
 * Orden de adquisición de mutex (RFx-57.3): NINGUNO se toma mientras se
 * sostiene otro. mtx_contadores y mtx_registro son hojas; las funciones de
 * threaded.c sueltan su mutex local antes de llamar a estado_* o reg_*.
 * Sin anidamiento no puede haber interbloqueo.
 */
#include "estado.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>

typedef struct {
    pthread_mutex_t mtx_contadores;
    pthread_mutex_t mtx_registro;
    Contadores      c;
} EstadoCompartido;

static EstadoCompartido *g = NULL;

static int crear_mutex(pthread_mutex_t *m)
{
    pthread_mutexattr_t a;
    if (pthread_mutexattr_init(&a) != 0)
        return -1;
    int r = pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    if (r == 0)
        r = pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST);
    if (r == 0)
        r = pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    return r == 0 ? 0 : -1;
}

static void bloquear(pthread_mutex_t *m)
{
    if (pthread_mutex_lock(m) == EOWNERDEAD)
        pthread_mutex_consistent(m);   /* el dueño anterior murió: recuperar */
}

int estado_iniciar(void)
{
    void *p = mmap(NULL, sizeof(EstadoCompartido), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    g = p;
    memset(&g->c, 0, sizeof g->c);
    if (crear_mutex(&g->mtx_contadores) != 0 || crear_mutex(&g->mtx_registro) != 0) {
        munmap(p, sizeof(EstadoCompartido));
        g = NULL;
        return -1;
    }
    return 0;
}

void estado_destruir(void)
{
    if (g == NULL)
        return;
    pthread_mutex_destroy(&g->mtx_contadores);
    pthread_mutex_destroy(&g->mtx_registro);
    munmap(g, sizeof(EstadoCompartido));
    g = NULL;
}

void estado_sumar(contador_t c, int64_t delta)
{
    if (g == NULL)
        return;
    bloquear(&g->mtx_contadores);
    switch (c) {
    case CNT_ADMITIDAS:      g->c.admitidas      += (uint64_t)delta; break;
    case CNT_RECHAZADAS:     g->c.rechazadas     += (uint64_t)delta; break;
    case CNT_TRANSFERENCIAS: g->c.transferencias += (uint64_t)delta; break;
    case CNT_ERRORES:        g->c.errores        += (uint64_t)delta; break;
    case CNT_ACTIVOS:        g->c.activos        += delta;           break;
    }
    pthread_mutex_unlock(&g->mtx_contadores);
}

Contadores estado_leer(void)
{
    Contadores copia;
    memset(&copia, 0, sizeof copia);
    if (g == NULL)
        return copia;
    bloquear(&g->mtx_contadores);
    copia = g->c;
    pthread_mutex_unlock(&g->mtx_contadores);
    return copia;
}

void estado_bloquear_registro(void)
{
    if (g != NULL)
        bloquear(&g->mtx_registro);
}

void estado_desbloquear_registro(void)
{
    if (g != NULL)
        pthread_mutex_unlock(&g->mtx_registro);
}

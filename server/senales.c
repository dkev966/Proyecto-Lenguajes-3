/*
 * senales.c — Instalación de manejadores de señales.
 */
#include "senales.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>

/* Solo es seguro en un manejador de señal si es atómico sin candados. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "atomic_int debe ser lock-free");

atomic_int servidor_corriendo = 1;

static void al_recibir_terminacion(int sig)
{
    (void)sig;
    int errno_previo = errno;   /* un manejador nunca debe alterar errno */
    atomic_store(&servidor_corriendo, 0);
    errno = errno_previo;
}

int senales_instalar(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = al_recibir_terminacion;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   /* sin SA_RESTART: accept()/poll() vuelven con EINTR */

    if (sigaction(SIGINT, &sa, NULL) < 0)
        return -1;
    if (sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;

    /* Escribir a un cliente que ya se fue generaría SIGPIPE y mataría el
       proceso entero (RNF-08). Se ignora y el error llega como EPIPE. */
    struct sigaction ign;
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    if (sigaction(SIGPIPE, &ign, NULL) < 0)
        return -1;

    return 0;
}

int senales_bloquear_terminacion(sigset_t *previo)
{
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGINT);
    sigaddset(&s, SIGTERM);
    return pthread_sigmask(SIG_BLOCK, &s, previo);
}

int senales_restaurar(const sigset_t *previo)
{
    return pthread_sigmask(SIG_SETMASK, previo, NULL);
}

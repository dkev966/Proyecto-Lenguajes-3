/*
 * threaded.c — Modo THREADED (RF-19, RF-56 a RF-59).
 *
 * - El hilo principal solo admite conexiones; cada una la atiende un hilo
 *   desacoplado (detached), que libera sus recursos al terminar (RFx-56.2/56.3).
 * - Estado compartido entre hilos, protegido por mtx_hilos:
 *     activos       -> cuántos hilos de atención viven (límite RF-20, RFx-19.4)
 *     sockets[]     -> fd de cada sesión, para poder despertarlas al detenerse
 *   Contadores y registro usan los mutex de estado.c.
 * - Orden de adquisición (RFx-57.3): mtx_hilos nunca se sostiene mientras se
 *   llama a reg_* o estado_*; ningún mutex se anida, no hay interbloqueo posible.
 * - Las secciones críticas solo tocan enteros: nunca se transfiere contenido con
 *   un mutex tomado (RFx-57.1).
 * - Señales (RF-59): los hilos de atención se crean con SIGINT/SIGTERM
 *   bloqueadas, así que siempre las recibe el hilo principal; los hilos ven la
 *   bandera servidor_corriendo en puntos seguros (entre bloques / cada 200 ms).
 * - Un error en un hilo (cliente caído, archivo ilegible) solo termina ese hilo
 *   (RFx-19.3): nada en atender_cliente() llama a exit().
 */
#include "servidor.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "atencion.h"
#include "estado.h"
#include "protocolo.h"
#include "registro.h"
#include "senales.h"

typedef struct {
    int                   fd;
    int                   ranura;
    char                  ip[64];
    const Servicio       *svc;
    const ConfigServidor *cfg;
} ArgHilo;

static pthread_mutex_t mtx_hilos = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond_hilos;          /* señala "un hilo terminó" */
static int             activos = 0;
static int            *sockets = NULL;      /* -1 = ranura libre */
static int             capacidad = 0;

/* Reserva una ranura. Devuelve su índice o -1 si se alcanzó el límite. */
static int reservar_ranura(int fd)
{
    int ranura = -1;
    pthread_mutex_lock(&mtx_hilos);
    if (activos < capacidad) {
        for (int i = 0; i < capacidad; i++) {
            if (sockets[i] < 0) {
                sockets[i] = fd;
                ranura = i;
                activos++;
                break;
            }
        }
    }
    pthread_mutex_unlock(&mtx_hilos);
    return ranura;
}

/* Libera la ranura. Si fd >= 0 lo cierra DENTRO de la sección crítica: así
   forzar_sockets() nunca puede hacer shutdown() sobre un número de fd que el
   sistema ya reasignó a otra cosa. */
static void liberar_ranura(int ranura, int fd)
{
    pthread_mutex_lock(&mtx_hilos);
    sockets[ranura] = -1;
    if (fd >= 0)
        close(fd);
    activos--;
    pthread_cond_broadcast(&cond_hilos);
    pthread_mutex_unlock(&mtx_hilos);
}

static void *hilo_atencion(void *p)
{
    ArgHilo *a = p;

    reg_info(a->ip, "Conexión admitida; atendida por un hilo dedicado");
    atender_cliente(a->fd, a->ip, a->svc, a->cfg);

    /* Todo lo que usa memoria compartida (registro, contadores) va ANTES de
       liberar la ranura: en cuanto activos llega a 0 el hilo principal puede
       terminar y liberar esa memoria. Después de liberar_ranura() el hilo ya
       no toca nada global. */
    estado_sumar(CNT_ACTIVOS, -1);
    reg_info(a->ip, "Sesión finalizada; el hilo libera sus recursos");

    int fd = a->fd, ranura = a->ranura;
    free(a);
    liberar_ranura(ranura, fd);
    return NULL;
}

static int iniciar_sincronizacion(void)
{
    pthread_condattr_t ca;
    if (pthread_condattr_init(&ca) != 0)
        return -1;
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);   /* inmune a cambios de hora */
    int r = pthread_cond_init(&cond_hilos, &ca);
    pthread_condattr_destroy(&ca);
    return r == 0 ? 0 : -1;
}

/* Espera hasta plazo_ms a que activos llegue a 0. Devuelve cuántos quedan. */
static int esperar_hilos(int plazo_ms)
{
    struct timespec limite;
    clock_gettime(CLOCK_MONOTONIC, &limite);
    limite.tv_sec += plazo_ms / 1000;
    limite.tv_nsec += (long)(plazo_ms % 1000) * 1000000L;
    if (limite.tv_nsec >= 1000000000L) {
        limite.tv_sec++;
        limite.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&mtx_hilos);
    while (activos > 0) {
        if (pthread_cond_timedwait(&cond_hilos, &mtx_hilos, &limite) == ETIMEDOUT)
            break;
    }
    int quedan = activos;
    pthread_mutex_unlock(&mtx_hilos);
    return quedan;
}

/* Despierta a los hilos bloqueados en send() hacia clientes lentos. */
static void forzar_sockets(void)
{
    pthread_mutex_lock(&mtx_hilos);
    for (int i = 0; i < capacidad; i++)
        if (sockets[i] >= 0)
            shutdown(sockets[i], SHUT_RDWR);
    pthread_mutex_unlock(&mtx_hilos);
}

static int detener_hilos(void)
{
    pthread_mutex_lock(&mtx_hilos);
    int n = activos;
    pthread_mutex_unlock(&mtx_hilos);
    if (n == 0)
        return 0;

    reg_info(NULL, "Esperando a que %d hilo(s) cierren su sesión", n);
    int quedan = esperar_hilos(PLAZO_DETENCION_MS);
    if (quedan == 0) {
        reg_info(NULL, "Todos los hilos terminaron ordenadamente");
        return 0;
    }

    reg_error(NULL, "%d hilo(s) no terminaron en %d ms; se cortan sus conexiones",
              quedan, PLAZO_DETENCION_MS);
    forzar_sockets();
    quedan = esperar_hilos(1000);
    if (quedan == 0) {
        reg_info(NULL, "Hilos restantes terminados tras cortar sus conexiones");
        return 0;
    }
    reg_error(NULL, "%d hilo(s) siguen sin terminar; se abandonan al salir del proceso", quedan);
    return 1;
}

int iniciar_modo_threaded(int server_fd, const ConfigServidor *cfg, const Servicio *svc)
{
    capacidad = cfg->max_clientes;
    sockets = malloc((size_t)capacidad * sizeof *sockets);
    if (sockets == NULL || iniciar_sincronizacion() < 0) {
        reg_error(NULL, "No se pudieron preparar las estructuras de hilos");
        free(sockets);
        return -1;
    }
    for (int i = 0; i < capacidad; i++)
        sockets[i] = -1;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    reg_info(NULL, "Un hilo por conexión (máximo %d simultáneos)", capacidad);
    reg_info(NULL, "Esperando conexiones...");

    while (servidor_corriendo) {
        char ip[64];
        int fd = servidor_aceptar(server_fd, ip, sizeof ip);
        if (fd < 0)
            continue;

        int ranura = reservar_ranura(fd);
        if (ranura < 0) {   /* RF-20, RFx-19.4 */
            servidor_rechazar(fd, ip, ESTADO_CAPACIDAD, "Servidor lleno: límite de clientes alcanzado");
            continue;
        }

        ArgHilo *a = malloc(sizeof *a);
        if (a == NULL) {
            liberar_ranura(ranura, -1);
            servidor_rechazar(fd, ip, ESTADO_INTERNO, "Error interno: no se pudo atender la conexión");
            continue;
        }
        a->fd = fd;
        a->ranura = ranura;
        a->svc = svc;
        a->cfg = cfg;
        snprintf(a->ip, sizeof a->ip, "%s", ip);

        estado_sumar(CNT_ADMITIDAS, 1);
        estado_sumar(CNT_ACTIVOS, 1);

        /* El hilo nuevo hereda la máscara: nace con SIGINT/SIGTERM bloqueadas. */
        sigset_t previa;
        senales_bloquear_terminacion(&previa);
        pthread_t tid;
        int r = pthread_create(&tid, &attr, hilo_atencion, a);
        senales_restaurar(&previa);

        if (r != 0) {
            estado_sumar(CNT_ADMITIDAS, -1);
            estado_sumar(CNT_ACTIVOS, -1);
            liberar_ranura(ranura, -1);
            free(a);
            reg_error(ip, "pthread_create falló: %s", strerror(r));
            servidor_rechazar(fd, ip, ESTADO_INTERNO, "Error interno: no se pudo atender la conexión");
        }
    }

    pthread_attr_destroy(&attr);
    int forzados = detener_hilos();
    if (forzados == 0) {
        free(sockets);
        sockets = NULL;
        pthread_cond_destroy(&cond_hilos);
    }
    return forzados;
}

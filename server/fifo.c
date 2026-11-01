/*
 * fifo.c — Modo FIFO (RF-17, RF-50, RF-51).
 *
 * El propio proceso principal atiende una conexión a la vez. Mientras tanto, las
 * demás quedan en la cola de admisión del sistema operativo (backlog de listen,
 * RFx-50.1); si la cola se llena, el SO rechaza las excedentes y el servidor no
 * lo considera un fallo (RFx-50.2).
 *
 * RFx-17.2 (mostrar las conexiones en espera): mientras el proceso está ocupado
 * en una sesión no puede mirar la cola, así que un hilo MONITOR consulta cada
 * segundo su largo (TCP_INFO del socket de escucha) y lo imprime cuando cambia.
 * Ese hilo nunca acepta ni atiende conexiones: la atención sigue siendo
 * estrictamente secuencial y hecha por un único sujeto.
 *
 * Consecuencia aceptada y documentada (RFx-17.3, RF-51): una sesión del canal en
 * vivo no termina por sí sola, así que mantiene ocupado al servidor hasta que ese
 * cliente envía SALIR/CERRAR, se desconecta o el servidor se detiene.
 */
#include "servidor.h"

#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "atencion.h"
#include "estado.h"
#include "registro.h"
#include "senales.h"

typedef struct {
    int             server_fd;
    pthread_mutex_t mtx;
    pthread_cond_t  cond;
    int             fin;
} Monitor;

static void *monitor_cola(void *p)
{
    Monitor *m = p;
    int ultimo = 0;

    pthread_mutex_lock(&m->mtx);
    while (!m->fin) {
        struct timespec limite;
        clock_gettime(CLOCK_REALTIME, &limite);
        limite.tv_sec += 1;
        pthread_cond_timedwait(&m->cond, &m->mtx, &limite);
        if (m->fin)
            break;
        pthread_mutex_unlock(&m->mtx);   /* nada de registro con el mutex tomado */

        int pendientes = servidor_conexiones_pendientes(m->server_fd);
        if (pendientes >= 0 && pendientes != ultimo) {
            if (pendientes > 0)
                reg_info(NULL, "%d conexión(es) en espera en la cola de admisión", pendientes);
            else
                reg_info(NULL, "La cola de admisión quedó vacía");
            ultimo = pendientes;
        }
        pthread_mutex_lock(&m->mtx);
    }
    pthread_mutex_unlock(&m->mtx);
    return NULL;
}

int iniciar_modo_fifo(int server_fd, const ConfigServidor *cfg, const Servicio *svc)
{
    reg_info(NULL, "Atención secuencial: una sesión a la vez; el resto espera en la cola del SO");

    Monitor mon = { .server_fd = server_fd, .fin = 0 };
    pthread_mutex_init(&mon.mtx, NULL);
    pthread_cond_init(&mon.cond, NULL);

    /* El monitor nace con SIGINT/SIGTERM bloqueadas: siempre las recibe el principal. */
    pthread_t tid;
    sigset_t previa;
    senales_bloquear_terminacion(&previa);
    int hay_monitor = pthread_create(&tid, NULL, monitor_cola, &mon) == 0;
    senales_restaurar(&previa);
    if (!hay_monitor)
        reg_error(NULL, "No se pudo iniciar el monitor de la cola; se informará solo entre sesiones");

    reg_info(NULL, "Esperando conexiones...");

    while (servidor_corriendo) {
        char ip[64];
        int fd = servidor_aceptar(server_fd, ip, sizeof ip);
        if (fd < 0)
            continue;   /* sin conexión en 500 ms, señal o error transitorio */

        estado_sumar(CNT_ADMITIDAS, 1);
        estado_sumar(CNT_ACTIVOS, 1);
        reg_info(ip, "Conexión admitida; inicia sesión exclusiva");   /* RFx-50.3 */

        atender_cliente(fd, ip, svc, cfg);   /* hasta su conclusión natural (RFx-17.1) */

        close(fd);
        estado_sumar(CNT_ACTIVOS, -1);
        int pendientes = servidor_conexiones_pendientes(server_fd);
        reg_info(ip, "Sesión finalizada; recursos liberados. Siguiente en la cola: %d en espera",
                 pendientes > 0 ? pendientes : 0);
    }

    if (hay_monitor) {
        pthread_mutex_lock(&mon.mtx);
        mon.fin = 1;
        pthread_cond_signal(&mon.cond);
        pthread_mutex_unlock(&mon.mtx);
        pthread_join(tid, NULL);
    }
    pthread_cond_destroy(&mon.cond);
    pthread_mutex_destroy(&mon.mtx);

    /* Las conexiones que siguen en la cola sin haber sido aceptadas las cierra
       el kernel al cerrar el socket de escucha en servidor_ejecutar(). */
    reg_info(NULL, "Detención solicitada; cola de admisión descartada");
    return 0;
}

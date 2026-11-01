/*
 * forked.c — Modo FORKED (RF-18, RF-52 a RF-55).
 *
 * - El proceso principal solo admite conexiones; cada una la atiende un hijo.
 * - El hijo cierra el socket de escucha (lo único ajeno que hereda, porque el
 *   padre cierra el socket de cada cliente justo después del fork) y termina
 *   con _exit() al concluir la sesión (RFx-52.1, RFx-52.2).
 * - Recolección continua de zombis sin bloquear accept(): SIGCHLD interrumpe el
 *   poll() del ciclo y además se recolecta en cada vuelta con waitpid(WNOHANG)
 *   (RFx-52.3).
 * - Al detenerse: SIGTERM a cada hijo, espera acotada y SIGKILL a los que no
 *   respondan (RFx-52.4).
 */
#include "servidor.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "atencion.h"
#include "estado.h"
#include "protocolo.h"
#include "registro.h"
#include "senales.h"

static pid_t *hijos = NULL;   /* tabla de PIDs vivos, propia del proceso principal */
static int    capacidad = 0;
static int    activos = 0;

static void al_terminar_hijo(int sig)
{
    (void)sig;   /* solo existe para interrumpir poll(); se recolecta en el ciclo */
}

static void registrar_hijo(pid_t pid)
{
    for (int i = 0; i < capacidad; i++) {
        if (hijos[i] == 0) {
            hijos[i] = pid;
            activos++;
            return;
        }
    }
}

static void recolectar_hijos(void)
{
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
        for (int i = 0; i < capacidad; i++) {
            if (hijos[i] == pid) {
                hijos[i] = 0;
                activos--;
                break;
            }
        }
        estado_sumar(CNT_ACTIVOS, -1);
        if (WIFSIGNALED(st))
            reg_error(NULL, "Proceso hijo %d terminó por la señal %d (aislado: el servidor sigue)",
                      (int)pid, WTERMSIG(st));
        else
            reg_debug(NULL, "Proceso hijo %d recolectado (código %d)", (int)pid, WEXITSTATUS(st));
    }
}

static void dormir_ms(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);   /* SIGCHLD la interrumpe: se vuelve a recolectar antes */
}

static int detener_hijos(void)
{
    if (activos == 0)
        return 0;

    reg_info(NULL, "Propagando la detención a %d proceso(s) hijo", activos);
    for (int i = 0; i < capacidad; i++)
        if (hijos[i] > 0)
            kill(hijos[i], SIGTERM);

    for (int t = 0; t < PLAZO_DETENCION_MS && activos > 0; t += 100) {
        dormir_ms(100);
        recolectar_hijos();
    }
    if (activos == 0) {
        reg_info(NULL, "Todos los procesos hijo terminaron ordenadamente");
        return 0;
    }

    reg_error(NULL, "%d hijo(s) no terminaron en %d ms; se fuerzan con SIGKILL",
              activos, PLAZO_DETENCION_MS);
    for (int i = 0; i < capacidad; i++) {
        if (hijos[i] > 0) {
            kill(hijos[i], SIGKILL);
            waitpid(hijos[i], NULL, 0);
            hijos[i] = 0;
            estado_sumar(CNT_ACTIVOS, -1);
        }
    }
    activos = 0;
    return 1;
}

int iniciar_modo_forked(int server_fd, const ConfigServidor *cfg, const Servicio *svc)
{
    capacidad = cfg->max_clientes;
    hijos = calloc((size_t)capacidad, sizeof *hijos);
    if (hijos == NULL) {
        reg_error(NULL, "Sin memoria para la tabla de procesos");
        return -1;
    }

    struct sigaction sa, sa_previa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = al_terminar_hijo;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP;   /* sin SA_RESTART a propósito */
    sigaction(SIGCHLD, &sa, &sa_previa);

    reg_info(NULL, "Un proceso hijo por conexión (máximo %d simultáneos)", capacidad);
    reg_info(NULL, "Esperando conexiones...");

    while (servidor_corriendo) {
        recolectar_hijos();

        char ip[64];
        int fd = servidor_aceptar(server_fd, ip, sizeof ip);
        if (fd < 0)
            continue;

        recolectar_hijos();
        if (activos >= capacidad) {   /* RF-20, RF-54 */
            servidor_rechazar(fd, ip, ESTADO_CAPACIDAD, "Servidor lleno: límite de clientes alcanzado");
            continue;
        }

        /* Vaciar los buffers de stdio antes de fork: si no, el hijo los heredaría
           y los imprimiría por duplicado. */
        fflush(stdout);
        fflush(stderr);

        pid_t pid = fork();
        if (pid < 0) {
            char motivo[96];
            snprintf(motivo, sizeof motivo, "No se pudo crear el proceso de atención (%s)", strerror(errno));
            servidor_rechazar(fd, ip, ESTADO_INTERNO, "Error interno: no se pudo atender la conexión");
            reg_error(ip, "%s", motivo);
            continue;
        }

        if (pid == 0) {
            /* ---------------- Proceso hijo ---------------- */
            close(server_fd);                       /* RFx-52.1 */
            sigaction(SIGCHLD, &sa_previa, NULL);   /* el hijo no tiene hijos que recolectar */
            free(hijos);                            /* la tabla es del padre */
            hijos = NULL;

            reg_info(ip, "Conexión admitida; atendida por el proceso hijo");
            atender_cliente(fd, ip, svc, cfg);
            close(fd);
            reg_info(ip, "Sesión finalizada; el proceso hijo termina");
            _exit(0);   /* _exit: no ejecutar atexit() ni vaciar buffers heredados */
        }

        /* ---------------- Proceso principal ---------------- */
        close(fd);   /* el socket del cliente es del hijo */
        registrar_hijo(pid);
        estado_sumar(CNT_ADMITIDAS, 1);
        estado_sumar(CNT_ACTIVOS, 1);
        reg_debug(ip, "Delegada al proceso hijo %d (%d activos)", (int)pid, activos);
    }

    int forzados = detener_hijos();
    sigaction(SIGCHLD, &sa_previa, NULL);
    free(hijos);
    hijos = NULL;
    return forzados;
}

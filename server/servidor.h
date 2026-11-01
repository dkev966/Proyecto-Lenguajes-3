/*
 * servidor.h — Punto de entrada de la capa de concurrencia.
 *
 * main.c (interfaz de consola) solo tiene que:
 *   1. llenar un ConfigServidor (config_servidor_por_omision() da los valores base),
 *   2. cargar los dos catálogos y las duraciones del canal en un Servicio,
 *   3. llamar a servidor_ejecutar() y devolver su resultado como código de salida.
 */
#ifndef SERVIDOR_H
#define SERVIDOR_H

#include <stdint.h>
#include "catalogo.h"

/*
 * Contexto de servicio: todo lo que la capa de concurrencia necesita de la
 * capa de servicio (catalogo.c / sesiones.c). Se arma UNA vez en main(), antes
 * de servidor_ejecutar(), y después es de solo lectura: los procesos hijo
 * (FORKED) lo heredan y los hilos (THREADED) lo comparten sin mutex.
 */
typedef struct {
    const Catalogo *demanda;      /* catálogo bajo demanda (LISTAR / ABRIR)            */
    const Catalogo *canal;        /* elementos del canal en vivo, en orden de emisión  */
    const int64_t  *duraciones;   /* ms de cada elemento del canal (catalogo_duraciones) */
    size_t          n_canal;      /* cantidad de elementos del canal                   */
    int64_t         ciclo_ms;     /* T: duración total de un ciclo                     */
    int64_t         t0_ms;        /* instante de arranque del canal (reloj monótono)   */
} Servicio;

typedef enum { MODO_FIFO, MODO_FORKED, MODO_THREADED } modo_t;

typedef struct {
    modo_t      modo;
    const char *direccion;            /* "0.0.0.0", "127.0.0.1", "::" ...        */
    int         puerto;
    int         profundidad_admision; /* backlog de listen() (RFx-50.1)           */
    int         max_clientes;         /* límite de clientes activos (RF-20)       */
    int         inactividad_s;        /* 0 = sin límite                           */
    const char *ruta_registro;        /* NULL = salida estándar / de error        */
    int         verboso;
} ConfigServidor;

void        config_servidor_por_omision(ConfigServidor *cfg);
const char *modo_nombre(modo_t m);
int         modo_desde_texto(const char *s, modo_t *out);   /* 0 ok, -1 inválido */

/* Corre el servidor hasta recibir SIGINT/SIGTERM. Devuelve un SALIDA_* */
int servidor_ejecutar(const ConfigServidor *cfg, const Servicio *svc);

/* --- Uso interno de la capa de concurrencia --- */

/* Cada modo recibe el socket ya en escucha. Devuelve 0 si todas las sesiones
   cerraron a tiempo, 1 si hubo que forzar alguna, -1 ante error fatal. */
int iniciar_modo_fifo(int server_fd, const ConfigServidor *cfg, const Servicio *svc);
int iniciar_modo_forked(int server_fd, const ConfigServidor *cfg, const Servicio *svc);
int iniciar_modo_threaded(int server_fd, const ConfigServidor *cfg, const Servicio *svc);

/* Espera hasta 500 ms una conexión y la acepta. Devuelve el fd del cliente,
   -1 si no hubo conexión (timeout, señal o error transitorio). Escribe la IP. */
int servidor_aceptar(int server_fd, char *ip, size_t ip_cap);

/* Conexiones completadas que esperan en la cola de admisión (RFx-17.2). */
int servidor_conexiones_pendientes(int server_fd);

/* Rechaza una conexión con un ERROR y la cierra (capacidad, fallo de fork...). */
void servidor_rechazar(int fd, const char *ip, int codigo, const char *motivo);

/* Plazo máximo para el cierre ordenado de sesiones al detenerse (RF-58, RFx-52.4). */
#define PLAZO_DETENCION_MS 5000

#endif /* SERVIDOR_H */

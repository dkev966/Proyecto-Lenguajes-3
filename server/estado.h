/*
 * estado.h — Estado compartido del servidor (sección 6.3, RF-53, RF-57).
 *
 * Vive en una región mmap(MAP_SHARED | MAP_ANONYMOUS) creada ANTES de cualquier
 * fork(), así que los procesos hijo del modo FORKED y los hilos del modo
 * THREADED ven la misma memoria. Los mutex son PTHREAD_PROCESS_SHARED y ROBUST:
 * si un hijo muere (kill -9) sosteniendo un mutex, el siguiente que lo tome
 * recibe EOWNERDEAD y lo recupera en vez de quedar bloqueado para siempre.
 *
 * Qué es de quién (exigido por RF-53):
 *   - Compartido real (esta región): contadores de operación y el mutex del registro.
 *   - Copia heredada de solo lectura: configuración, catálogo, instante t0 del canal.
 *     No cambian tras el arranque, así que la copia de cada hijo es coherente.
 *     El reloj monótono es del sistema, idéntico en todos los procesos.
 *   - Propio del proceso principal: socket de escucha, tabla de PIDs hijos.
 *   - Propio de cada hijo/hilo: socket de su cliente, su Sesion, sus buffers.
 */
#ifndef ESTADO_H
#define ESTADO_H

#include <stdint.h>

typedef enum {
    CNT_ADMITIDAS,       /* conexiones aceptadas y atendidas          */
    CNT_RECHAZADAS,      /* rechazadas por capacidad o fallo interno  */
    CNT_TRANSFERENCIAS,  /* transferencias bajo demanda completas     */
    CNT_ERRORES,         /* errores reportados a clientes o de red    */
    CNT_ACTIVOS          /* clientes activos en este momento          */
} contador_t;

typedef struct {
    uint64_t admitidas;
    uint64_t rechazadas;
    uint64_t transferencias;
    uint64_t errores;
    int64_t  activos;
} Contadores;

int        estado_iniciar(void);
void       estado_destruir(void);
void       estado_sumar(contador_t c, int64_t delta);
Contadores estado_leer(void);

/* Exclusión mutua para la salida por consola (RFx-57.2). Solo la usa registro.c */
void estado_bloquear_registro(void);
void estado_desbloquear_registro(void);

#endif /* ESTADO_H */

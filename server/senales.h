/*
 * senales.h — Gestión de señales para la detención ordenada (RNF-12, RF-58, RF-59).
 *
 * El manejador SOLO baja la bandera servidor_corriendo (es lo único seguro dentro
 * de un manejador). Cada modo la revisa en puntos seguros:
 *   - el ciclo de accept (accept/poll devuelve EINTR porque no usamos SA_RESTART),
 *   - las lecturas del cliente (red_leer_exacto la revisa cada 200 ms),
 *   - cada bloque de una transferencia.
 */
#ifndef SENALES_H
#define SENALES_H

#include <signal.h>
#include <stdatomic.h>

/* Definida en senales.c. NO volver a definirla en main.c ni en otro archivo.
 * atomic_int y no volatile sig_atomic_t: la escriben el manejador de señal y
 * la leen varios hilos; atomic_int sin candados es seguro en ambos casos
 * (C11 7.14.1.1) y no produce carreras de datos (verificado con ThreadSanitizer). */
extern atomic_int servidor_corriendo;

/* SIGINT y SIGTERM -> bajan la bandera (sin SA_RESTART). SIGPIPE -> ignorada. */
int senales_instalar(void);

/* Bloquea SIGINT/SIGTERM en el hilo actual (para crear hilos que no las reciban). */
int senales_bloquear_terminacion(sigset_t *previo);
int senales_restaurar(const sigset_t *previo);

#endif /* SENALES_H */

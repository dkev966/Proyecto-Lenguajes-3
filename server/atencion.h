/*
 * atencion.h — Atención de UNA sesión de cliente, común a los tres modos.
 *
 * Esta es la pieza que hace cumplir RF-21 y la sección 6.2: FIFO, FORKED y
 * THREADED solo deciden QUIÉN llama a atender_cliente() (el proceso principal,
 * un hijo o un hilo). El protocolo y la semántica son idénticos en los tres.
 */
#ifndef ATENCION_H
#define ATENCION_H

#include "catalogo.h"
#include "servidor.h"

/* Atiende la sesión completa (SALUDO ... CERRAR). NO cierra fd: lo cierra quien llama. */
void atender_cliente(int fd, const char *ip, const Servicio *svc, const ConfigServidor *cfg);

#endif /* ATENCION_H */

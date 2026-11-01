/*
 * red.h — Lectura/escritura robusta sobre sockets TCP y (de)serialización del
 * protocolo. Común a servidor y cliente.
 *
 * TCP es un flujo de bytes: un read() puede devolver medio header o dos mensajes
 * juntos. Estas funciones repiten la operación hasta completar exactamente los
 * bytes pedidos, con tiempo límite, y nunca generan SIGPIPE.
 */
#ifndef RED_H
#define RED_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include "protocolo.h"

/* Resultados de las operaciones de red */
#define RED_OK            1
#define RED_CERRADO       0   /* el otro extremo cerró o reinició la conexión   */
#define RED_ERROR        -1   /* error de E/S                                   */
#define RED_TIEMPO       -2   /* venció el tiempo límite (inactividad / truncado) */
#define RED_INTERRUMPIDO -3   /* la bandera *seguir pasó a 0 (detención ordenada) */

/*
 * Lee exactamente n bytes. timeout_ms < 0 = sin límite.
 * Si seguir != NULL, revisa *seguir cada 200 ms y aborta con RED_INTERRUMPIDO
 * cuando vale 0 (así un hilo o proceso nota la señal de detención).
 * Es atomic_int (no volatile sig_atomic_t) porque la leen varios hilos a la vez:
 * con volatile eso sería una carrera de datos según C11.
 */
int red_leer_exacto(int fd, void *buf, size_t n, int timeout_ms,
                    const atomic_int *seguir);

/*
 * Configura todas las escrituras del proceso: si seguir != NULL, una escritura
 * bloqueada (cliente que no consume) se aborta con RED_INTERRUMPIDO cuando
 * *seguir vale 0; si plazo_ms >= 0, se aborta con RED_TIEMPO tras plazo_ms sin
 * progreso. Por omisión (sin llamar a esta función): sin bandera ni plazo.
 */
void red_configurar_escritura(const atomic_int *seguir, int plazo_ms);

/* Escribe exactamente n bytes (con MSG_NOSIGNAL: un cliente caído no mata al proceso). */
int red_escribir_exacto(int fd, const void *buf, size_t n);

/* Lee y descarta n bytes (para resincronizar tras rechazar un mensaje). */
int red_descartar(int fd, uint32_t n, int timeout_ms, const atomic_int *seguir);

/* Opciones de socket para cada conexión (TCP_NODELAY). Llamar tras accept()
   en el servidor y tras connect() en el cliente. */
void red_ajustar_socket(int fd);

/* Espera hasta timeout_ms a que haya datos (o cierre) en fd. 1 = sí, 0 = no, -1 = error. */
int red_esperar_datos(int fd, int timeout_ms);

/* Header <-> 14 bytes en el cable */
void red_serializar_header(const HeaderProtocolo *h, uint8_t out[TAM_HEADER]);
void red_deserializar_header(const uint8_t in[TAM_HEADER], HeaderProtocolo *h);

int red_recibir_header(int fd, HeaderProtocolo *h, int timeout_ms,
                       const atomic_int *seguir);

/* Envía header + carga como una sola unidad de protocolo. */
int red_enviar(int fd, uint16_t tipo, uint16_t estado, uint32_t id,
               const void *carga, uint32_t len);

/* Igual, pero ignora la bandera de detención y espera como máximo
   PLAZO_URGENTE_MS: para el último mensaje (DESPEDIDA al detenerse, ERROR de
   rechazo por capacidad), que debe intentarse aunque el servidor se apague. */
#define PLAZO_URGENTE_MS 1000
int red_enviar_urgente(int fd, uint16_t tipo, uint16_t estado, uint32_t id,
                       const void *carga, uint32_t len);

/* Enteros de 64 bits en big-endian sin depender de htobe64 (no es C11 estándar). */
void     red_u64_a_be(uint64_t v, uint8_t out[8]);
uint64_t red_be_a_u64(const uint8_t in[8]);

/* Nombres legibles para los registros */
const char *protocolo_nombre_msg(uint16_t tipo);
const char *protocolo_nombre_estado(uint16_t estado);
const char *red_describir(int resultado);

#endif /* RED_H */

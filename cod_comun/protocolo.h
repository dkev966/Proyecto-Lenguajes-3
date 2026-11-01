/*
 * protocolo.h — Definición del protocolo de aplicación (sección 7 de la SRS).
 *
 * PROPUESTA: si un compañero ya escribió su propio protocolo.h, hay que unificar
 * los valores de MSG_* y ESTADO_* con los de aquí. Lo importante para la capa de
 * concurrencia es que el header viaja SIEMPRE con este formato de 14 bytes:
 *
 *   desplaz.  tamaño  campo
 *   0         2       versión            (big-endian)
 *   2         2       tipo de mensaje    (big-endian)
 *   4         4       longitud de carga  (big-endian)
 *   8         2       código de estado   (big-endian)
 *   10        4       identificador      (big-endian)
 *   14        n       carga útil (n = longitud)
 *
 * No se envía el struct directamente con write(): el compilador mete relleno
 * (padding) entre campos y sizeof(HeaderProtocolo) no es 14. La serialización
 * byte a byte vive en red.c y la usan tanto el servidor como el cliente.
 */
#ifndef PROTOCOLO_H
#define PROTOCOLO_H

#include <stdint.h>

#define PROTOCOL_VERSION   1
#define TAM_HEADER         14            /* bytes del header en el cable          */
#define MAX_NOMBRE         255           /* largo máximo de un nombre de contenido */
#define MAX_CARGA_CLIENTE  1024          /* ningún mensaje cliente->servidor legítimo pesa más */
#define TAM_BLOQUE_DATOS       (64 * 1024)   /* tamaño de cada mensaje DATOS (RF-33, RNF-23) */

/* Tipos de mensaje (7.3) */
enum {
    MSG_SALUDO = 1,
    MSG_BIENVENIDA,
    MSG_LISTAR,
    MSG_CATALOGO,
    MSG_ABRIR,
    MSG_ACEPTADO,
    MSG_DATOS,
    MSG_FINAL,       /* "FIN" de la SRS; MSG_FIN ya existe en <sys/socket.h> */
    MSG_ERROR,
    MSG_CERRAR,
    MSG_DESPEDIDA,
    MSG_UNIR,
    MSG_EMISION,
    MSG_SALIR
};

/* Códigos de estado (7.4) */
enum {
    ESTADO_EXITO = 0,
    ESTADO_VERSION,
    ESTADO_SOLICITUD,
    ESTADO_NO_HALLADO,
    ESTADO_NO_SERVIBLE,
    ESTADO_NO_PERMITIDO,
    ESTADO_CAPACIDAD,
    ESTADO_INTERNO
};

/* Header ya deserializado, en el orden de bytes del host. */
typedef struct {
    uint16_t version;
    uint16_t tipo;
    uint32_t longitud;
    uint16_t estado;
    uint32_t id;
} HeaderProtocolo;

/*
 * Cargas útiles (todas las cadenas en UTF-8, sin terminador NUL en el cable):
 *   BIENVENIDA : texto con los servicios, p. ej. "LISTAR,ABRIR,UNIR,SALIR,CERRAR"
 *   CATALOGO   : una línea por contenido: "<nombre>\t<bytes>\t<disponible 0|1>\n"
 *   ABRIR      : nombre del contenido
 *   ACEPTADO   : 8 bytes, tamaño total en bytes (big-endian)
 *   EMISION    : 8 bytes posición de incorporación en ms + 8 bytes duración del ciclo en ms
 *   DATOS      : bytes opacos del contenido
 *   ERROR      : texto legible con la causa
 *   DESPEDIDA  : vacía si respondió a CERRAR; "Servidor detenido" si el servidor se apaga
 */

#endif /* PROTOCOLO_H */

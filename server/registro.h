/*
 * registro.h — Salida informativa y de errores del servidor (RF-27, RFx-12.1,
 * RFx-57.2, RF-69, RNF-22).
 *
 * Cada línea lleva: instante, modo, pid/tid, IP del cliente (si aplica) y mensaje.
 * Información -> stdout; errores -> stderr (o ambos al archivo de registro).
 * Una línea se arma completa en memoria y se escribe con un solo write() bajo
 * mutex compartido: nunca se entremezclan líneas de hilos o procesos distintos.
 */
#ifndef REGISTRO_H
#define REGISTRO_H

#if defined(__GNUC__)
#define FORMATO_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define FORMATO_PRINTF(a, b)
#endif

/* ruta == NULL -> stdout/stderr. Devuelve -1 si no pudo abrir el archivo. */
int  registro_iniciar(const char *modo, const char *ruta, int verboso);
void registro_cerrar(void);

/* ip puede ser NULL para mensajes que no son de un cliente. */
void reg_info(const char *ip, const char *fmt, ...) FORMATO_PRINTF(2, 3);
void reg_error(const char *ip, const char *fmt, ...) FORMATO_PRINTF(2, 3);
void reg_debug(const char *ip, const char *fmt, ...) FORMATO_PRINTF(2, 3);

#endif /* REGISTRO_H */

#ifndef SESIONES_H
#define SESIONES_H 
#include "catalogo.h"

/* Estructura OPACA: guarda descriptor de archivo, posicion, archivo
 * actual del ciclo (si es canal), etc. */
typedef struct Sesion Sesion;
 
typedef struct {
    size_t  indice;       /* archivo del ciclo que esta "al aire"      */
    int64_t offset_ms;    /* milisegundos dentro de ese archivo        */
} PuntoCanal;

estado_t canal_calcular_punto(const int64_t *duraciones_ms, size_t n,
                              int64_t transcurrido_ms, PuntoCanal *out);

/* Bajo demanda: abre 'nombre' desde el byte 0.
 * Valida el nombre (nunca interpreta rutas).
 * Salida: *out (sesion) y *tamano_total (bytes que se enviaran). */
estado_t sesion_abrir_demanda(const Catalogo *cat, const char *nombre,
                              Sesion **out, uint64_t *tamano_total);
 
/* Canal en vivo: abre una sesion posicionada en el punto que corresponde
 * a 'transcurrido_ms' (tiempo monotonico desde que arranco el canal). */
estado_t sesion_abrir_canal(const Catalogo *cat, int64_t transcurrido_ms,
                            Sesion **out);
 
/* Lee el siguiente bloque de la sesion en 'buf' (capacidad 'max').
 * Salida: *leidos = bytes escritos en buf.
 *  - Bajo demanda: *leidos == 0 significa fin de archivo.
 *  - Canal en vivo: nunca hay fin; al terminar un archivo pasa al
 *    siguiente y al terminar el ultimo vuelve al primero.
 * El contenido es binario: usar siempre buf + *leidos, nunca '\0'. */
estado_t sesion_leer_bloque(Sesion *s, void *buf, size_t max,
                            size_t *leidos);
 
/* Cierra el descriptor y libera la sesion. Acepta NULL.
 * Llamar tambien si el cliente se desconecta a la mitad. */
void sesion_cerrar(Sesion *s);
 
/* Ms del reloj monotonico (CLOCK_MONOTONIC). Usado por P3 para marcar
 * el inicio del canal y obtener 'transcurrido_ms' en cada conexion. */
int64_t reloj_monotonico_ms(void);

#endif // !SESIONES_H

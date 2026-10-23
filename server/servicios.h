#ifndef SERVICIOS_H
#define SERVICIOS_H 
#include <stddef.h>
#include <stdint.h>
#include <dirent.h>

#define LARGO_NOMBRE 50
#define TAM_BLOQUE 4096

typedef enum {
    SRV_OK = 0,
    SRV_ERR_ARGUMENTO,        
    SRV_ERR_NOMBRE_INVALIDO, 
    SRV_ERR_NO_ENCONTRADO,  
    SRV_ERR_NO_SERVIBLE,   
    SRV_ERR_CATALOGO_VACIO,
    SRV_ERR_IO,           
    SRV_ERR_MEMORIA      
} estado_t;

typedef struct Catalogo Catalogo;

typedef struct EntradaInterna EntradaInterna;

typedef struct {
  char nombre[LARGO_NOMBRE];
  uint64_t tamano;
} EntradaCatalogo;

/* Escanea 'directorio' y crea el catalogo. Se llama dos veces al inicio 
 * una para el live y otra para bajo demanda;
 * despues es de solo lectura.
 * Salida: *out (reservado por el modulo; liberar con catalogo_liberar). */
estado_t catalogo_escanear(const char *directorio, Catalogo **out);
 
/* Cantidad de entradas servibles. */
size_t catalogo_cantidad(const Catalogo *cat);
 
/* Copia la entrada 'indice' (0 .. cantidad-1) en *out.
 * Devuelve SRV_ERR_ARGUMENTO si el indice esta fuera de rango. */
estado_t catalogo_obtener(const Catalogo *cat, size_t indice,
                          EntradaCatalogo *out);
 
/* Libera el catalogo. Acepta NULL. No llamar si hay sesiones abiertas. */
void catalogo_liberar(Catalogo *cat);

/* Carga las duraciones (ms) del canal desde 'archivo', una por línea y en
 * el orden del catálogo. Llamar una vez, tras catalogo_escanear y antes de
 * crear hilos/procesos. Con error, el catálogo no se modifica. */
estado_t catalogo_cargar_duraciones(Catalogo *cat, const char *archivo);

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

#endif // !SERVICIOS_H

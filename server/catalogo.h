#ifndef CATALOGO_H
#define CATALOGO_H
#define LARGO_NOMBRE 50
#define TAM_BLOQUE 4096
#include <stddef.h>
#include <stdint.h>
#include <dirent.h>

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


typedef struct {
  char nombre[LARGO_NOMBRE];
  uint64_t tamano;
  char ruta[LARGO_NOMBRE];
} EntradaInterna;

/* Escanea 'directorio' y crea el catalogo. Se llama dos veces al inicio 
 * una para el live y otra para bajo demanda;
 * despues es de solo lectura.
 * Salida: *out (reservado por el modulo; liberar con catalogo_liberar). */
estado_t catalogo_escanear(const char *directorio, Catalogo **out);
 
/* Cantidad de entradas servibles. */
size_t catalogo_cantidad(const Catalogo *cat);

/* Lista de duraciones en de cada entrada. */
int64_t* catalogo_duraciones(const Catalogo *cat);
 
/* Copia la entrada 'indice' (0 .. cantidad-1) en *out.
 * Devuelve SRV_ERR_ARGUMENTO si el indice esta fuera de rango. */
const EntradaInterna *catalogo_obtener(const Catalogo *cat, size_t indice);
 
/* Libera el catalogo. Acepta NULL. No llamar si hay sesiones abiertas. */
void catalogo_liberar(Catalogo *cat);

/* Carga las duraciones (ms) del canal desde 'archivo', una por línea y en
 * el orden del catálogo. Llamar una vez, tras catalogo_escanear y antes de
 * crear hilos/procesos. Con error, el catálogo no se modifica. */
estado_t catalogo_cargar_duraciones(Catalogo *cat);

estado_t entrada_nombre(EntradaInterna *en, char *nombre);

#endif // !CATALOGO_H

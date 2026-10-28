#include "catalogo.h"
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "sesiones.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "uso: %s <directorio> <archivo_duraciones>\n", argv[0]);
        return 1;
    }

    printf("Ruta: %s\n", argv[1]);
    /* 1. Catálogo */
    Catalogo *cat = NULL;
    estado_t e = catalogo_escanear(argv[1], &cat);

    printf("escanear -> %d\n", e);
    if (e != SRV_OK) return 1;

    size_t n = catalogo_cantidad(cat);
    printf("cantidad = %zu\n", n);
    for (size_t i = 0; i < n; i++) {
        const EntradaInterna *ent = catalogo_obtener(cat, i);
        printf("  [%zu] estado=%d nombre=%s tamano=%llu\n",
               i, e, ent->nombre, (unsigned long long)ent->tamano);
    }

    /* 2. Catálogo live*/ 
    printf("Ruta_live: %s\n", argv[2]);
    Catalogo *cat_live = NULL;
    estado_t e_live = catalogo_escanear(argv[2], &cat_live);

    printf("escanear_live -> %d\n", e_live);
    if (e_live != SRV_OK) return 1;

    size_t n_live = catalogo_cantidad(cat);
    printf("cantidad = %zu\n", n_live);
    for (size_t i = 0; i < n_live; i++) {
        const EntradaInterna *ent_live = catalogo_obtener(cat_live, i);
        printf("  [%zu] estado=%d nombre=%s tamano=%llu\n",
               i, e_live, ent_live->nombre, (unsigned long long)ent_live->tamano);
    }

    /* 2. Duraciones */
    e = catalogo_cargar_duraciones(cat);
    printf("cargar_duraciones -> %d\n", e);

    /* 3. Modelo temporal con valores inventados */
    int64_t dur[] = {10000, 10000, 10000};
    int64_t tiempos[] = {0, 9999, 10000, 25000, 30000, 35000};
    for (size_t i = 0; i < sizeof(tiempos) / sizeof(tiempos[0]); i++) {
        PuntoCanal p;
        e = canal_calcular_punto(dur, 3, tiempos[i], &p);
        printf("punto(t=%lld) -> estado=%d indice=%zu offset_ms=%lld\n",
               (long long)tiempos[i], e, p.indice, (long long)p.offset_ms);
    }

    /* 4. Bajo demanda: primer archivo completo */
    const EntradaInterna *primera = catalogo_obtener(cat, 0);

    Sesion *s = NULL;
    uint64_t total = 0;
    e = sesion_abrir_demanda(cat, primera->nombre, &s, &total);
    printf("abrir_demanda(%s) -> estado=%d total=%llu\n",
           primera->nombre, e, (unsigned long long)total);

    if (e == SRV_OK) {
        unsigned char buf[TAM_BLOQUE];
        size_t leidos, suma = 0;
        int bloques = 0;
        while ((e = sesion_leer_bloque(s, buf, sizeof(buf), &leidos)) == SRV_OK
               && leidos > 0) {
            suma += leidos;
            bloques++;
        }
        printf("  leidos=%zu bytes en %d bloques, estado final=%d\n",
               suma, bloques, e);
        sesion_cerrar(s);
    }

    /* 5. Casos inválidos */
    s = NULL;
    printf("nombre inexistente -> %d\n", sesion_abrir_demanda(cat, "nope", &s, &total));
    printf("nombre ../x        -> %d\n", sesion_abrir_demanda(cat, "../x", &s, &total));
    printf("nombre vacio       -> %d\n", sesion_abrir_demanda(cat, "", &s, &total));
    printf("cat NULL           -> %d\n", sesion_abrir_demanda(NULL, "a", &s, &total));

    /* 6. Canal en vivo: arranca en 25 s y lee unos bloques */
    e = sesion_abrir_canal(cat, 25000, &s);
    printf("abrir_canal(25000) -> %d\n", e);
    if (e == SRV_OK) {
        unsigned char buf[30];      /* bloque pequeño para ver los cortes */
        size_t leidos;
        for (int i = 0; i < 12; i++) {
            e = sesion_leer_bloque(s, buf, sizeof(buf), &leidos);
            printf("  bloque %2d: estado=%d leidos=%zu primer_byte=%u\n",
                   i, e, leidos, leidos ? buf[0] : 0);
            if (e != SRV_OK) break;
        }
        sesion_cerrar(s);
    }

    /* 7. Reloj */
    int64_t t1 = reloj_monotonico_ms();
    sleep(1);
    int64_t t2 = reloj_monotonico_ms();
    printf("reloj: diferencia tras sleep(1) = %lld ms\n", (long long)(t2 - t1));

    catalogo_liberar(cat);
    catalogo_liberar(cat_live);
    return 0;
}

#include <dirent.h>
#include <limits.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "catalogo.h"

struct Catalogo{
  EntradaInterna *entradas;
  size_t cantidad;
  size_t capacidad;
  int64_t *duraciones;
};

static int comparar_entradas(const void *a, const void *b)
{
    const EntradaInterna *ea = a;
    const EntradaInterna *eb = b;
    return strcmp(ea->nombre, eb->nombre);
}

static estado_t catalogo_agregar(struct Catalogo *cat, const EntradaInterna *nueva)
{
    if (cat->cantidad == cat->capacidad) {
        size_t nueva_cap = (cat->capacidad == 0) ? 8 : cat->capacidad * 2;
        EntradaInterna *tmp = realloc(cat->entradas,
                                      nueva_cap * sizeof(EntradaInterna));
        if (tmp == NULL)
            return SRV_ERR_MEMORIA;   /* cat->entradas sigue siendo válido */
        cat->entradas = tmp;
        cat->capacidad = nueva_cap;
    }
    cat->entradas[cat->cantidad] = *nueva;   /* copia el struct completo */
    cat->cantidad++;
    return SRV_OK;
}

estado_t catalogo_escanear(const char *ruta, Catalogo **out){
  if (ruta == NULL || out == NULL || ruta[0] == '\0') return SRV_ERR_ARGUMENTO;

  DIR *dir = opendir(ruta);
  if (dir == NULL) return SRV_ERR_IO;

  struct Catalogo *cat = calloc(1, sizeof(*cat));
  if (cat == NULL) {
      closedir(dir);
      return SRV_ERR_MEMORIA;
  }

  estado_t estado = SRV_OK;
  struct dirent *ent;

  while ((ent = readdir(dir)) != NULL) {
      if (ent->d_name[0] == '.') continue;

      if (strlen(ent->d_name) >= LARGO_NOMBRE) continue; /*Nombre no cabe, no es servible*/

      EntradaInterna nueva;
      memset(&nueva, 0, sizeof(nueva));

      int n = snprintf(nueva.ruta, sizeof(nueva.ruta), "%s/%s", ruta, ent->d_name);

      if (n < 0 || (size_t)n >= sizeof(nueva.ruta)) continue;

      struct stat st;
      if (lstat(nueva.ruta, &st) == -1)
          continue;
      if (!S_ISREG(st.st_mode) || st.st_size == 0)
          continue;
      if (access(nueva.ruta, R_OK) == -1)
          continue;

      size_t len = strlen(ent->d_name);
      memcpy(nueva.nombre, ent->d_name, len+1);
      nueva.tamano = (uint64_t)st.st_size;

      estado = catalogo_agregar(cat, &nueva);
      if (estado != SRV_OK)
          break;
  }

  closedir(dir);

  if (estado == SRV_OK && cat->cantidad == 0) estado = SRV_ERR_CATALOGO_VACIO;

  if (estado != SRV_OK) {
      free(cat->entradas);
      free(cat);
      return estado;
  }

  qsort(cat->entradas, cat->cantidad, sizeof(EntradaInterna), comparar_entradas);

  *out = cat;
  return SRV_OK;
}

size_t catalogo_cantidad(const Catalogo *cat){
  return (cat == NULL) ? 0:cat->cantidad;
}

int64_t* catalogo_duraciones(const Catalogo *cat){
  return cat->duraciones;
}

const EntradaInterna *catalogo_obtener(const Catalogo *cat, size_t indice){
  if(cat == NULL || indice >= cat->cantidad) return NULL;
  return &cat->entradas[indice];
}

void catalogo_liberar(Catalogo *cat){
  if(cat ==NULL) return;
  free(cat->duraciones);
  free(cat->entradas);
  free(cat);
}

estado_t catalogo_cargar_duraciones(Catalogo *cat){
  if (cat == NULL ) return SRV_ERR_ARGUMENTO;
  if (cat->cantidad == 0) return SRV_ERR_CATALOGO_VACIO;

  FILE *f = fopen("../videos/.duraciones.txt", "r");

  int64_t *dur = calloc(cat->cantidad, sizeof(*dur));
  if (dur == NULL) {
      fclose(f);
      return SRV_ERR_MEMORIA;
  }

  estado_t estado = SRV_OK;
  size_t n = 0;
  char linea[64];

  while (fgets(linea, sizeof(linea), f) != NULL) {
      size_t len = strlen(linea);

      if (strspn(linea, " \t\r\n") == len)
          continue;

      char *fin;
      errno = 0;
      long long v = strtoll(linea, &fin, 10);

      if (fin == linea || errno == ERANGE) {   /* sin dígitos o desbordó */
          estado = SRV_ERR_IO;
          break;
      }
      while (*fin == ' ' || *fin == '\t' || *fin == '\r')
          fin++;
      if (*fin != '\n' && *fin != '\0') {      /* basura tras el número */
          estado = SRV_ERR_IO;
          break;
      }
      dur[n++] = (int64_t)v;
  }

  if (estado == SRV_OK && ferror(f))
      estado = SRV_ERR_IO;
  if (estado == SRV_OK && n != cat->cantidad)  /* faltan duraciones */
      estado = SRV_ERR_IO;

  fclose(f);

  if (estado != SRV_OK) {
      free(dur);
      return estado;           /* el catálogo queda como estaba */
  }

  free(cat->duraciones);    /* por si se llama dos veces */
  cat->duraciones = dur;
  return SRV_OK;
}

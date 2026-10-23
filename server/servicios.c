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
#include <fcntl.h>
#include "servicios.h"

struct Catalogo{
  EntradaInterna *entradas;
  size_t cantidad;
  size_t capacidad;
  int64_t *duraciones;
};

struct EntradaInterna{
  EntradaCatalogo pub;
  char *ruta;
};

static int comparar_entradas(const void *a, const void *b)
{
    const EntradaInterna *ea = a;
    const EntradaInterna *eb = b;
    return strcmp(ea->pub.nombre, eb->pub.nombre);
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

      snprintf(nueva.pub.nombre, LARGO_NOMBRE, "%s", ent->d_name);
      nueva.pub.tamano = (uint64_t)st.st_size;

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

estado_t catalogo_obtener(const Catalogo *cat, size_t indice, EntradaCatalogo *out){
  if(out==NULL || cat == NULL || indice >= (cat->cantidad)) return SRV_ERR_ARGUMENTO;

  memset(out, 0, sizeof(*out));
  *out = cat->entradas[indice].pub;
  return SRV_OK;
}

void catalogo_liberar(Catalogo *cat){
  if(cat ==NULL) return;
  free(cat->entradas);
  free(cat);
}

estado_t catalogo_cargar_duraciones(Catalogo *cat){
  if (cat == NULL ) return SRV_ERR_ARGUMENTO;
  if (cat->cantidad == 0) return SRV_ERR_CATALOGO_VACIO;

  FILE *f = fopen(".duraciones.txt", "r");

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

typedef enum { SESION_DEMANDA, SESION_CANAL } modo_sesion_t;

struct Sesion{
  modo_sesion_t modo;
  int           fd;
  uint64_t      tamano_archivo;   /* tamaño del archivo abierto  */
  uint64_t      posicion;         /* bytes leídos de ese archivo */

  /* Solo para el canal */
  const Catalogo *catalogo;
  size_t          indice;         /* archivo actual del ciclo    */
};

estado_t canal_calcular_punto(const int64_t *duraciones_ms, size_t n, int64_t transcurrido_ms, PuntoCanal *out){
  if (out == NULL) return SRV_ERR_ARGUMENTO;
  memset(out, 0, sizeof(*out));

  if (duraciones_ms == NULL || n == 0 || transcurrido_ms < 0) return SRV_ERR_ARGUMENTO;

  /* Duración total del ciclo, vigilando el desbordamiento */
  int64_t total = 0;
  for (size_t i = 0; i < n; i++) {
      if (duraciones_ms[i] <= 0)
          return SRV_ERR_ARGUMENTO;
      if (total > INT64_MAX - duraciones_ms[i])
          return SRV_ERR_ARGUMENTO;
      total += duraciones_ms[i];
  }

  /* Posición dentro del ciclo */
  int64_t t = transcurrido_ms % total;

  /* Ubicar archivo y offset */
  for (size_t i = 0; i < n; i++) {
      if (t < duraciones_ms[i]) {
          out->indice = i;
          out->offset_ms = t;
          return SRV_OK;
      }
      t -= duraciones_ms[i];
  }

  return SRV_ERR_ARGUMENTO;   /* inalcanzable si el código es correcto */
}

estado_t sesion_abrir_demanda(const Catalogo *cat, const char *nombre, Sesion **out, uint64_t *tamano_total){
  if(out==NULL || cat==NULL || nombre==NULL || tamano_total==NULL) return SRV_ERR_ARGUMENTO;
  *out = NULL;
  *tamano_total = 0;

  if (nombre[0] == '\0' || strlen(nombre) >= LARGO_NOMBRE ||
      strchr(nombre, '/') != NULL || strstr(nombre, "..") != NULL) return SRV_ERR_NOMBRE_INVALIDO;

  const EntradaInterna *entrada = NULL;
  for (size_t i = 0; i < cat->cantidad; i++) {
    if (strcmp(cat->entradas[i].pub.nombre, nombre) == 0) {
      entrada = &cat->entradas[i];
      break;
    }
  }
  if (entrada == NULL) return SRV_ERR_NO_ENCONTRADO;

  int fd = open(entrada->ruta, O_RDONLY | O_CLOEXEC);
  if (fd == -1) {
      if (errno == ENOENT) return SRV_ERR_NO_ENCONTRADO;
      if (errno == EACCES) return SRV_ERR_NO_SERVIBLE;
      return SRV_ERR_IO;
  }

  struct stat st;
  if (fstat(fd, &st) == -1) {
      close(fd);
      return SRV_ERR_IO;
  }

  if (!S_ISREG(st.st_mode)) {
      close(fd);
      return SRV_ERR_NO_SERVIBLE;
  }

  struct Sesion *ses = calloc(1, sizeof(*ses));
  if (ses == NULL) {
      close(fd);
      return SRV_ERR_MEMORIA;
  }

  ses->modo = SESION_DEMANDA;
  ses->fd = fd;
  ses->tamano_archivo = (uint64_t)st.st_size;
  ses->posicion = 0;

  *out = ses;
  *tamano_total = ses->tamano_archivo;
  return SRV_OK;
}

estado_t sesion_abrir_canal(const Catalogo *cat, int64_t transcurrido_ms, Sesion **out){
  if(out==NULL || cat==NULL || transcurrido_ms<0) return SRV_ERR_ARGUMENTO;
  if(!cat->cantidad) return SRV_ERR_CATALOGO_VACIO;
  *out = NULL;
  
}

estado_t sesion_leer_bloque(Sesion *s, void *buf, size_t max, size_t *leidos){

}

void sesion_cerrar(Sesion *s){

}

int64_t reloj_monotonico_ms(void){

}

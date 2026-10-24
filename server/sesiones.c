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
#include "catalogo.h"
#include "sesiones.h"

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

  for (size_t i = 0; i < catalogo_cantidad(cat); i++) {
    EntradaInterna en;
    catalogo_obtener(cat, i, &en);
    if (strcmp(en.nombre, nombre) == 0) {
      entrada = &en;
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
  if(!catalogo_cantidad(cat)) return SRV_ERR_CATALOGO_VACIO;
  *out = NULL;

  PuntoCanal *punto = calloc(1, sizeof(*punto));
  canal_calcular_punto(catalogo_duraciones(cat), catalogo_cantidad(cat), transcurrido_ms, punto);
  
  EntradaInterna *en;
  catalogo_obtener(cat, punto->indice, en);

  if (en == NULL) return SRV_ERR_NO_ENCONTRADO;

  int fd = open(en->ruta, O_RDONLY | O_CLOEXEC);
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

  ses->modo = SESION_CANAL;
  ses->fd = fd;
  ses->tamano_archivo = (uint64_t)st.st_size;
  ses->posicion = punto->offset_ms;

  *out = ses;
  return SRV_OK;
}

estado_t sesion_leer_bloque(Sesion *s, void *buf, size_t max, size_t *leidos){

}

void sesion_cerrar(Sesion *s){
  
}

int64_t reloj_monotonico_ms(void){

}

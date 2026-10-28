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
#include <time.h>
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

static estado_t abrir_entrada(const EntradaInterna *en,
                              int *fd_out, uint64_t *tam_out){
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
    if (!S_ISREG(st.st_mode) || st.st_size == 0) {
        close(fd);
        return SRV_ERR_NO_SERVIBLE;
    }

    *fd_out = fd;
    *tam_out = (uint64_t)st.st_size;
    return SRV_OK;
}

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
    const EntradaInterna *en = catalogo_obtener(cat, i);
    if (strcmp(en->nombre, nombre) == 0) {
      entrada = en;
      break;
    }
  }

  if (entrada == NULL) return SRV_ERR_NO_ENCONTRADO;

  int fd;
  uint64_t tam;
  estado_t e = abrir_entrada(entrada, &fd , &tam);

  if(e != SRV_OK) return e;

  struct Sesion *ses = calloc(1, sizeof(*ses));
  if (ses == NULL) {
      close(fd);
      return SRV_ERR_MEMORIA;
  }

  ses->modo = SESION_DEMANDA;
  ses->fd = fd;
  ses->tamano_archivo = tam;
  ses->posicion = 0;

  *out = ses;
  *tamano_total = tam;
  return SRV_OK;
}

estado_t sesion_abrir_canal(const Catalogo *cat, int64_t transcurrido_ms,
                            Sesion **out){
  if (out == NULL)
      return SRV_ERR_ARGUMENTO;
  *out = NULL;
  if (cat == NULL || transcurrido_ms < 0)
      return SRV_ERR_ARGUMENTO;
  if (!catalogo_cantidad(cat))
      return SRV_ERR_CATALOGO_VACIO;

  const int64_t *duraciones = catalogo_duraciones(cat);
  if (duraciones == NULL)               /* faltó cargar duraciones */
      return SRV_ERR_ARGUMENTO;

  PuntoCanal punto;
  estado_t e = canal_calcular_punto(duraciones, catalogo_cantidad(cat),
                                    transcurrido_ms, &punto);
  if (e != SRV_OK)
      return e;

  const EntradaInterna *en = catalogo_obtener(cat, punto.indice);

  int fd;
  uint64_t tam;
  e = abrir_entrada(en, &fd, &tam);
  if (e != SRV_OK)
      return e;

  /* ms -> bytes: tamaño * offset_ms / duración (128 bits evita desbordar) */
  uint64_t offset_bytes = (uint64_t)(((unsigned __int128)tam *
                          (uint64_t)punto.offset_ms) /
                          (uint64_t)duraciones[punto.indice]);
  if (offset_bytes >= tam) {            /* defensivo: no debería pasar */
      close(fd);
      return SRV_ERR_IO;
  }

  if (lseek(fd, (off_t)offset_bytes, SEEK_SET) == (off_t)-1) {
      close(fd);
      return SRV_ERR_IO;
  }

  struct Sesion *ses = calloc(1, sizeof(*ses));
  if (ses == NULL) {
      close(fd);
      return SRV_ERR_MEMORIA;
  }

  ses->modo = SESION_CANAL;
  ses->fd = fd;
  ses->tamano_archivo = tam;
  ses->posicion = offset_bytes;
  ses->catalogo = cat;
  ses->indice = punto.indice;

  *out = ses;
  return SRV_OK;
}

estado_t sesion_leer_bloque(Sesion *s, void *buf, size_t max, size_t *leidos){
  if (leidos == NULL)
    return SRV_ERR_ARGUMENTO;
  *leidos = 0;
  if (s == NULL || buf == NULL || max == 0)
    return SRV_ERR_ARGUMENTO;

  /* ¿Se acabó el archivo actual? */
  if (s->posicion >= s->tamano_archivo) {
      if (s->modo == SESION_DEMANDA)
        return SRV_OK;

      /* Canal: pasar al siguiente archivo del ciclo (con vuelta al 0) */
      size_t sig = (s->indice + 1) % catalogo_cantidad(s->catalogo);
      int nuevo_fd;
      uint64_t nuevo_tam;

      estado_t e = abrir_entrada(catalogo_obtener(s->catalogo, sig),
                                 &nuevo_fd, &nuevo_tam);
      if (e != SRV_OK)
          return e;           /* la sesión sigue válida; se puede reintentar */

      close(s->fd);
      s->fd = nuevo_fd;
      s->tamano_archivo = nuevo_tam;
      s->posicion = 0;
      s->indice = sig;
  }

  /* Nunca leer más allá del final del archivo actual */
  uint64_t restante = s->tamano_archivo - s->posicion;
  size_t pedir = (restante < max) ? (size_t)restante : max;

  ssize_t n;
  do {
      n = read(s->fd, buf, pedir);
  } while (n == -1 && errno == EINTR);      /* reintentar si una señal interrumpe */

  if (n < 0)
      return SRV_ERR_IO;
  if (n == 0)                               /* el archivo se truncó por fuera */
      return SRV_ERR_IO;

  s->posicion += (uint64_t)n;
  *leidos = (size_t)n;
  return SRV_OK;
}

void sesion_cerrar(Sesion *s){
  if (s == NULL)
      return;
  if (s->fd >= 0)
      close(s->fd);
  free(s); 
}

int64_t reloj_monotonico_ms(void){
  struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
        return -1;

    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

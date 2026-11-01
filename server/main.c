/*
 * main.c — Interfaz de consola del servidor (sección 11.1, RF-23, RF-25, RF-60,
 * RF-64 a RF-69) y arranque de la capa de servicio.
 *
 * Orden de arranque:
 *   1. Validar TODOS los argumentos (RF-64): nada de red ni de disco antes.
 *   2. Cargar los catálogos bajo demanda y del canal, y las duraciones del canal
 *      (catalogo.c / sesiones.c, sin modificar).
 *   3. Fijar t0 del canal y entregar el control a la capa de concurrencia.
 *
 * Dos particularidades de catalogo.c que este archivo resuelve sin tocarlo:
 *   - catalogo_cargar_duraciones() lee SIEMPRE "../videos/.duraciones.txt",
 *     relativo al directorio de trabajo, y no comprueba si el archivo existe.
 *     Aquí se verifica antes de llamarla y se explica cómo ejecutar el servidor.
 *   - catalogo_escanear() descarta en silencio los archivos cuya ruta
 *     "directorio/nombre" no cabe en LARGO_NOMBRE (50) bytes. Aquí se detectan y
 *     se informan por consola (RF-49).
 */
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "catalogo.h"
#include "codigos_salida.h"
#include "servidor.h"
#include "sesiones.h"

#define RUTA_DURACIONES "../videos/.duraciones.txt"   /* fija en catalogo.c */

static void ayuda(const char *prog, FILE *f)
{
    ConfigServidor d;
    config_servidor_por_omision(&d);
    fprintf(f,
        "Uso: %s --modo MODO --puerto N --demanda DIR --canal DIR [opciones]\n"
        "\n"
        "Servidor de video: canal en vivo sincronizado y servicio bajo demanda.\n"
        "\n"
        "Obligatorios:\n"
        "  -m, --modo MODO          Modo de concurrencia: FIFO | FORKED | THREADED\n"
        "  -p, --puerto N           Puerto de escucha (1-65535)\n"
        "  -d, --demanda DIR        Almacén del contenido bajo demanda (cada archivo\n"
        "                           regular no oculto es una entrada del catálogo)\n"
        "  -c, --canal DIR          Contenido del canal en vivo: sus archivos se emiten en\n"
        "                           orden alfabético y el ciclo se repite indefinidamente\n"
        "\n"
        "Opcionales:\n"
        "  -a, --direccion IP       Dirección de escucha IPv4 o IPv6 (por omisión: %s,\n"
        "                           todas las interfaces IPv4)\n"
        "  -n, --max-clientes N     Clientes activos simultáneos, N >= 1 (por omisión: %d;\n"
        "                           en FIFO siempre se atiende de a uno)\n"
        "  -q, --cola N             Profundidad de la cola de admisión, N >= 1 (por omisión: %d)\n"
        "  -t, --inactividad S      Segundos sin actividad antes de cerrar una sesión;\n"
        "                           0 = sin límite (por omisión: %d)\n"
        "  -r, --registro ARCHIVO   Escribe el registro en ARCHIVO en vez de la consola\n"
        "  -v, --verboso            Más detalle en el registro\n"
        "  -h, --ayuda              Muestra esta ayuda y termina\n"
        "\n"
        "Duración del canal:\n"
        "  Las duraciones (ms) de los archivos del canal se leen de " RUTA_DURACIONES ",\n"
        "  una por línea y en el mismo orden alfabético que los archivos de --canal.\n"
        "  Esa ruta es relativa al directorio de trabajo: ejecute el servidor desde un\n"
        "  directorio hermano de videos/ (por ejemplo bin/ o server/).\n"
        "  Cada ruta \"DIR/archivo\" debe tener menos de %d caracteres: use rutas relativas.\n"
        "\n"
        "Ejemplos (desde bin/, con los videos en ../videos/demanda y ../videos/live):\n"
        "  %s --modo FIFO     --puerto 8080 --demanda ../videos/demanda --canal ../videos/live\n"
        "  %s --modo FORKED   --puerto 8080 --demanda ../videos/demanda --canal ../videos/live -n 50\n"
        "  %s --modo THREADED --puerto 8080 --demanda ../videos/demanda --canal ../videos/live -t 60 -v\n"
        "\n"
        "Códigos de salida:\n"
        "  %d éxito o detención ordenada (SIGINT/SIGTERM)   %d argumentos o configuración inválidos\n"
        "  %d error de red (puerto ocupado, dirección)       %d error interno\n",
        prog, d.direccion, d.max_clientes, d.profundidad_admision, d.inactividad_s,
        LARGO_NOMBRE, prog, prog, prog,
        SALIDA_EXITO, SALIDA_ARGUMENTOS, SALIDA_RED, SALIDA_INTERNO);
}

/* RF-65: dice qué parámetro está mal y por qué; va a la salida de error (RF-69). */
__attribute__((format(printf, 2, 3)))
static int error_args(const char *prog, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", prog);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\nUse '%s --ayuda' para ver las opciones.\n", prog);
    return SALIDA_ARGUMENTOS;
}

/* Entero estricto en [min, max]: sin espacios, signos raros ni basura al final. */
static int leer_entero(const char *s, long min, long max, int *out)
{
    if (s == NULL || *s == '\0')
        return -1;
    char *fin;
    errno = 0;
    long v = strtol(s, &fin, 10);
    if (errno != 0 || *fin != '\0' || v < min || v > max)
        return -1;
    *out = (int)v;
    return 0;
}

static int es_directorio_legible(const char *ruta)
{
    struct stat st;
    return stat(ruta, &st) == 0 && S_ISDIR(st.st_mode) && access(ruta, R_OK | X_OK) == 0;
}

static const char *texto_estado(estado_t e)
{
    switch (e) {
    case SRV_OK:                  return "correcto";
    case SRV_ERR_ARGUMENTO:       return "argumento inválido";
    case SRV_ERR_NOMBRE_INVALIDO: return "nombre inválido";
    case SRV_ERR_NO_ENCONTRADO:   return "no encontrado";
    case SRV_ERR_NO_SERVIBLE:     return "no servible";
    case SRV_ERR_CATALOGO_VACIO:  return "no contiene ningún archivo servible";
    case SRV_ERR_IO:              return "error de lectura";
    case SRV_ERR_MEMORIA:         return "memoria insuficiente";
    }
    return "error desconocido";
}

/* RF-49: informa por consola los archivos del directorio que el catálogo excluyó. */
static void informar_excluidos(const char *dir, const Catalogo *cat, const char *que)
{
    DIR *d = opendir(dir);
    if (d == NULL)
        return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        int esta = 0;
        for (size_t i = 0; i < catalogo_cantidad(cat) && !esta; i++)
            esta = strcmp(catalogo_obtener(cat, i)->nombre, de->d_name) == 0;
        if (esta)
            continue;
        char ruta[PATH_MAX];
        struct stat st;
        snprintf(ruta, sizeof ruta, "%s/%s", dir, de->d_name);
        if (stat(ruta, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        const char *causa = "vacío o ilegible";
        if (strlen(dir) + 1 + strlen(de->d_name) >= LARGO_NOMBRE)
            causa = "la ruta supera el largo admitido; use una ruta más corta para el directorio";
        fprintf(stderr, "Aviso: \"%s\" quedó fuera del %s (%s)\n", de->d_name, que, causa);
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    const char *prog = argv[0];
    ConfigServidor cfg;
    config_servidor_por_omision(&cfg);

    const char *modo_txt = NULL, *dir_demanda = NULL, *dir_canal = NULL;
    int puerto_dado = 0;

    static const struct option opciones[] = {
        { "modo",         required_argument, NULL, 'm' },
        { "puerto",       required_argument, NULL, 'p' },
        { "demanda",      required_argument, NULL, 'd' },
        { "canal",        required_argument, NULL, 'c' },
        { "direccion",    required_argument, NULL, 'a' },
        { "max-clientes", required_argument, NULL, 'n' },
        { "cola",         required_argument, NULL, 'q' },
        { "inactividad",  required_argument, NULL, 't' },
        { "registro",     required_argument, NULL, 'r' },
        { "verboso",      no_argument,       NULL, 'v' },
        { "ayuda",        no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    opterr = 0;   /* los errores de getopt se informan aquí, con más contexto */
    int op;
    while ((op = getopt_long(argc, argv, ":m:p:d:c:a:n:q:t:r:vh", opciones, NULL)) != -1) {
        switch (op) {
        case 'm':
            /* RF-60: dos modos distintos en la misma invocación es contradictorio */
            if (modo_txt != NULL && strcmp(modo_txt, optarg) != 0)
                return error_args(prog, "se indicaron dos modos distintos (%s y %s); indique uno solo",
                                  modo_txt, optarg);
            modo_txt = optarg;
            break;
        case 'p':
            if (leer_entero(optarg, 1, 65535, &cfg.puerto) != 0)
                return error_args(prog, "puerto inválido \"%s\"%s: debe ser un entero entre 1 y 65535",
                                  optarg, "");
            puerto_dado = 1;
            break;
        case 'd': dir_demanda = optarg; break;
        case 'c': dir_canal = optarg; break;
        case 'a': cfg.direccion = optarg; break;
        case 'n':
            if (leer_entero(optarg, 1, 100000, &cfg.max_clientes) != 0)
                return error_args(prog, "--max-clientes \"%s\" inválido%s: debe ser un entero >= 1",
                                  optarg, "");
            break;
        case 'q':
            if (leer_entero(optarg, 1, 65535, &cfg.profundidad_admision) != 0)
                return error_args(prog, "--cola \"%s\" inválida%s: debe ser un entero entre 1 y 65535",
                                  optarg, "");
            break;
        case 't':
            if (leer_entero(optarg, 0, 86400, &cfg.inactividad_s) != 0)
                return error_args(prog, "--inactividad \"%s\" inválida%s: debe ser un entero entre 0 y 86400",
                                  optarg, "");
            break;
        case 'r': cfg.ruta_registro = optarg; break;
        case 'v': cfg.verboso = 1; break;
        case 'h':
            ayuda(prog, stdout);
            return SALIDA_EXITO;
        case ':':
            return error_args(prog, "a la opción %s le falta su valor%s",
                              argv[optind - 1], "");
        default:
            return error_args(prog, "opción desconocida \"%s\"%s", argv[optind - 1], "");
        }
    }
    if (optind < argc)
        return error_args(prog, "argumento inesperado \"%s\"%s", argv[optind], "");

    /* ---- RF-25 / RF-67: obligatorios y combinaciones ---- */
    if (modo_txt == NULL)
        return error_args(prog, "falta --modo (%s%s)", "FIFO | FORKED | THREADED", "");
    if (modo_desde_texto(modo_txt, &cfg.modo) != 0)
        return error_args(prog, "modo desconocido \"%s\"%s: use FIFO, FORKED o THREADED", modo_txt, "");
    if (!puerto_dado)
        return error_args(prog, "falta --puerto%s%s", "", "");
    if (dir_demanda == NULL)
        return error_args(prog, "falta --demanda (directorio del contenido bajo demanda)%s%s", "", "");
    if (dir_canal == NULL)
        return error_args(prog, "falta --canal (directorio del contenido del canal en vivo)%s%s", "", "");
    if (!es_directorio_legible(dir_demanda))
        return error_args(prog, "--demanda \"%s\"%s no existe, no es un directorio o no se puede leer",
                          dir_demanda, "");
    if (!es_directorio_legible(dir_canal))
        return error_args(prog, "--canal \"%s\"%s no existe, no es un directorio o no se puede leer",
                          dir_canal, "");

    /* ---- Capa de servicio ---- */
    Catalogo *demanda = NULL, *canal = NULL;
    estado_t e = catalogo_escanear(dir_demanda, &demanda);
    if (e != SRV_OK) {
        fprintf(stderr, "%s: el catálogo bajo demanda \"%s\" %s\n", prog, dir_demanda, texto_estado(e));
        return SALIDA_ARGUMENTOS;
    }
    informar_excluidos(dir_demanda, demanda, "catálogo bajo demanda");

    e = catalogo_escanear(dir_canal, &canal);
    if (e != SRV_OK) {
        fprintf(stderr, "%s: el contenido del canal \"%s\" %s\n", prog, dir_canal, texto_estado(e));
        catalogo_liberar(demanda);
        return SALIDA_ARGUMENTOS;
    }
    informar_excluidos(dir_canal, canal, "canal en vivo");

    if (access(RUTA_DURACIONES, R_OK) != 0) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof cwd) == NULL)
            snprintf(cwd, sizeof cwd, "?");
        fprintf(stderr,
                "%s: no se encuentra el archivo de duraciones del canal \"%s\"\n"
                "  (la ruta es relativa al directorio de trabajo actual: %s).\n"
                "  Ejecute el servidor desde un directorio hermano de videos/, por ejemplo bin/.\n",
                prog, RUTA_DURACIONES, cwd);
        catalogo_liberar(demanda);
        catalogo_liberar(canal);
        return SALIDA_ARGUMENTOS;
    }
    e = catalogo_cargar_duraciones(canal);
    if (e != SRV_OK) {
        fprintf(stderr,
                "%s: no se pudieron cargar las duraciones de \"%s\" (%s).\n"
                "  Debe haber exactamente %zu líneas, una duración en ms por archivo de \"%s\",\n"
                "  en orden alfabético.\n",
                prog, RUTA_DURACIONES, texto_estado(e), catalogo_cantidad(canal), dir_canal);
        catalogo_liberar(demanda);
        catalogo_liberar(canal);
        return SALIDA_ARGUMENTOS;
    }

    Servicio svc;
    svc.demanda = demanda;
    svc.canal = canal;
    svc.duraciones = catalogo_duraciones(canal);
    svc.n_canal = catalogo_cantidad(canal);
    svc.ciclo_ms = 0;
    for (size_t i = 0; i < svc.n_canal; i++) {
        if (svc.duraciones[i] <= 0) {
            fprintf(stderr, "%s: la duración %zu de \"%s\" debe ser mayor que 0 ms\n",
                    prog, i + 1, RUTA_DURACIONES);
            catalogo_liberar(demanda);
            catalogo_liberar(canal);
            return SALIDA_ARGUMENTOS;
        }
        svc.ciclo_ms += svc.duraciones[i];
    }

    printf("Catálogo bajo demanda: %zu archivo(s) en \"%s\"\n", catalogo_cantidad(demanda), dir_demanda);
    printf("Canal en vivo: %zu archivo(s) en \"%s\", ciclo de %" PRId64 " ms\n",
           svc.n_canal, dir_canal, svc.ciclo_ms);
    fflush(stdout);

    /* t0: el canal arranca con el servidor. Se fija antes de crear procesos o
       hilos, así todos calculan la misma posición (sección 8.2). */
    svc.t0_ms = reloj_monotonico_ms();

    int r = servidor_ejecutar(&cfg, &svc);

    catalogo_liberar(demanda);
    catalogo_liberar(canal);
    return r;
}

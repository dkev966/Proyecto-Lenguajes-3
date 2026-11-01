/*
 * atencion.c — Máquina de estados de la sesión (sección 7.5) y servicio de los
 * mensajes. Es lo que antes estaba incrustado dentro del while del modo FIFO.
 *
 * Estados:   Inicio --SALUDO--> Lista --ABRIR/UNIR--> Transfiriendo --FIN/SALIR--> Lista
 *            Lista --CERRAR--> Cerrando
 *
 * Serialización por sesión (RF-31, RF-62), documentada así:
 *   - Durante una transferencia bajo demanda el servidor no lee del socket:
 *     cualquier solicitud que el cliente envíe queda ENCOLADA en el buffer TCP
 *     y se atiende al terminar la transferencia, en orden.
 *   - Durante el canal en vivo se revisa el socket entre bloques: SALIR y CERRAR
 *     se atienden; cualquier otra solicitud se RECHAZA con "No permitido".
 *
 * Integración con la capa de servicio (catalogo.c / sesiones.c, sin modificar):
 *   - LISTAR y ABRIR usan el catálogo bajo demanda; UNIR usa el catálogo del canal.
 *   - sesion_abrir_canal() recibe el tiempo transcurrido desde t0 (lo lleva esta capa).
 *   - sesion_leer_bloque() del canal entrega bytes sin límite de velocidad; el RITMO
 *     de tiempo real lo impone esta capa (ver "Ritmo del canal" más abajo).
 */
#include "atencion.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "estado.h"
#include "protocolo.h"
#include "red.h"
#include "registro.h"
#include "senales.h"
#include "sesiones.h"

#define CONTINUAR 1
#define TERMINAR  0

#define ESPERA_CANAL_MS 20   /* pausa cuando el canal no tiene bloque listo */

typedef struct {
    int             fd;
    const char     *ip;
    const Servicio *svc;
    int         timeout_ms;   /* inactividad; -1 = sin límite */
    uint32_t    id;           /* id de la solicitud que se está respondiendo */
} Ctx;

/* ---------- Respuestas ---------- */

static int responder(Ctx *c, uint16_t tipo, uint16_t estado, const void *carga, uint32_t len)
{
    int r = red_enviar(c->fd, tipo, estado, c->id, carga, len);
    if (r == RED_INTERRUMPIDO) {
        reg_info(c->ip, "Envío de %s interrumpido por la detención del servidor",
                 protocolo_nombre_msg(tipo));
        return TERMINAR;
    }
    if (r != RED_OK) {
        reg_error(c->ip, "No se pudo enviar %s: %s", protocolo_nombre_msg(tipo), red_describir(r));
        estado_sumar(CNT_ERRORES, 1);
        return TERMINAR;
    }
    return CONTINUAR;
}

/* RF-72: todo rechazo es visible al cliente Y en el registro.
   RF-73: al cliente va "para_cliente"; el detalle interno solo al registro. */
static int responder_error(Ctx *c, uint16_t codigo, const char *para_cliente, const char *detalle)
{
    reg_error(c->ip, "Rechazo [%s]: %s", protocolo_nombre_estado(codigo),
              detalle ? detalle : para_cliente);
    estado_sumar(CNT_ERRORES, 1);
    if (responder(c, MSG_ERROR, codigo, para_cliente, (uint32_t)strlen(para_cliente)) != CONTINUAR)
        return TERMINAR;
    return CONTINUAR;   /* la sesión sigue disponible (RF-12) */
}

/* Distingue para el cliente "el servidor se apagó" de "se cayó la red" (8.4). */
static void avisar_detencion(Ctx *c)
{
    static const char msg[] = "Servidor detenido";
    /* urgente: ignora la bandera (ya está abajo) y espera como máximo 1 s */
    red_enviar_urgente(c->fd, MSG_DESPEDIDA, ESTADO_EXITO, c->id, msg, sizeof msg - 1);
    reg_info(c->ip, "Sesión cerrada por detención del servidor");
}

static void informar_fin_lectura(Ctx *c, int r)
{
    switch (r) {
    case RED_CERRADO:
        reg_info(c->ip, "El cliente se desconectó sin enviar CERRAR");
        break;
    case RED_TIEMPO:
        reg_error(c->ip, "Sesión cerrada: inactividad o mensaje incompleto durante %d ms",
                  c->timeout_ms);
        estado_sumar(CNT_ERRORES, 1);
        break;
    case RED_INTERRUMPIDO:
        avisar_detencion(c);
        break;
    default:
        reg_error(c->ip, "Error de red leyendo del cliente");
        estado_sumar(CNT_ERRORES, 1);
        break;
    }
}

/* Traducción de estado_t (catalogo.h) a los códigos del protocolo (7.4). */
static uint16_t codigo_de(estado_t e)
{
    switch (e) {
    case SRV_ERR_NOMBRE_INVALIDO: return ESTADO_SOLICITUD;
    case SRV_ERR_NO_ENCONTRADO:   return ESTADO_NO_HALLADO;
    case SRV_ERR_NO_SERVIBLE:     return ESTADO_NO_SERVIBLE;
    case SRV_ERR_CATALOGO_VACIO:  return ESTADO_NO_SERVIBLE;
    case SRV_ERR_IO:              return ESTADO_NO_SERVIBLE;
    case SRV_ERR_ARGUMENTO:       return ESTADO_INTERNO;
    case SRV_ERR_MEMORIA:         return ESTADO_INTERNO;
    default:                      return ESTADO_INTERNO;
    }
}

static const char *nombre_estado_servicio(estado_t e)
{
    switch (e) {
    case SRV_OK:                  return "SRV_OK";
    case SRV_ERR_ARGUMENTO:       return "SRV_ERR_ARGUMENTO";
    case SRV_ERR_NOMBRE_INVALIDO: return "SRV_ERR_NOMBRE_INVALIDO";
    case SRV_ERR_NO_ENCONTRADO:   return "SRV_ERR_NO_ENCONTRADO";
    case SRV_ERR_NO_SERVIBLE:     return "SRV_ERR_NO_SERVIBLE";
    case SRV_ERR_CATALOGO_VACIO:  return "SRV_ERR_CATALOGO_VACIO";
    case SRV_ERR_IO:              return "SRV_ERR_IO";
    case SRV_ERR_MEMORIA:         return "SRV_ERR_MEMORIA";
    }
    return "?";
}

static const char *texto_de(uint16_t codigo)
{
    switch (codigo) {
    case ESTADO_NO_HALLADO:   return "El contenido no figura en el catálogo";
    case ESTADO_NO_SERVIBLE:  return "El contenido no puede servirse en este momento";
    case ESTADO_SOLICITUD:    return "Solicitud malformada";
    case ESTADO_NO_PERMITIDO: return "Operación no permitida";
    default:                  return "Error interno del servidor";
    }
}

/* ---------- Recepción de solicitudes ---------- */

/* Lee header + carga validando versión y longitud. 1 = hay solicitud; 0 = terminar. */
static int recibir_solicitud(Ctx *c, HeaderProtocolo *h, uint8_t *carga)
{
    int r = red_recibir_header(c->fd, h, c->timeout_ms, &servidor_corriendo);
    if (r != RED_OK) {
        informar_fin_lectura(c, r);
        return 0;
    }
    c->id = h->id;

    if (h->version != PROTOCOL_VERSION) {
        char m[96];
        snprintf(m, sizeof m, "Versión de protocolo no admitida; el servidor usa la versión %d",
                 PROTOCOL_VERSION);
        responder_error(c, ESTADO_VERSION, m, NULL);
        return 0;
    }
    if (h->longitud > MAX_CARGA_CLIENTE) {
        /* Longitud imposible (RNF-15, RF-77): no se reserva nada y se corta la sesión,
           porque no hay forma segura de resincronizar el flujo. */
        char det[96];
        snprintf(det, sizeof det, "%s con longitud imposible (%" PRIu32 " bytes)",
                 protocolo_nombre_msg(h->tipo), h->longitud);
        responder_error(c, ESTADO_SOLICITUD, "Longitud declarada imposible", det);
        return 0;
    }
    if (h->longitud > 0) {
        r = red_leer_exacto(c->fd, carga, h->longitud, c->timeout_ms, &servidor_corriendo);
        if (r != RED_OK) {
            informar_fin_lectura(c, r);
            return 0;
        }
    }
    carga[h->longitud] = '\0';
    reg_debug(c->ip, "Recibido %s (id %" PRIu32 ", %" PRIu32 " bytes)",
              protocolo_nombre_msg(h->tipo), h->id, h->longitud);
    return 1;
}

/* RF-76: se rechaza sin tocar el sistema de archivos. */
static int nombre_valido(const char *s, size_t n)
{
    if (n == 0 || n > MAX_NOMBRE)
        return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch == 0x7F || ch == '/' || ch == '\\')
            return 0;   /* incluye NUL incrustado, control, separadores de ruta */
    }
    if (s[0] == '.' || strstr(s, "..") != NULL)
        return 0;       /* ".", "..", ocultos y referencias a directorios superiores */
    return 1;
}

/* ---------- Servicios ---------- */

static int manejar_listar(Ctx *c)
{
    const Catalogo *cat = c->svc->demanda;
    size_t n = catalogo_cantidad(cat);
    size_t cap = 64 + n * (LARGO_NOMBRE + 32);
    char *buf = malloc(cap);
    if (buf == NULL)
        return responder_error(c, ESTADO_INTERNO, texto_de(ESTADO_INTERNO), "malloc en LISTAR");

    size_t len = 0;
    for (size_t i = 0; i < n; i++) {
        /* Solo nombre y tamaño: la ruta interna nunca sale del servidor (RF-47).
           catalogo_escanear() ya excluyó lo no servible, así que todo está disponible. */
        const EntradaInterna *en = catalogo_obtener(cat, i);
        if (en == NULL)
            continue;
        int k = snprintf(buf + len, cap - len, "%s\t%" PRIu64 "\t1\n", en->nombre, en->tamano);
        if (k < 0 || (size_t)k >= cap - len)
            break;
        len += (size_t)k;
    }

    int r = responder(c, MSG_CATALOGO, ESTADO_EXITO, buf, (uint32_t)len);
    free(buf);
    if (r == CONTINUAR)
        reg_info(c->ip, "LISTAR atendido (%zu entradas)", n);
    return r;
}

static int manejar_abrir(Ctx *c, const uint8_t *carga, uint32_t len)
{
    char nombre[MAX_NOMBRE + 1];

    if (len == 0 || len > MAX_NOMBRE)
        return responder_error(c, ESTADO_SOLICITUD, "Nombre de contenido vacío o demasiado largo", NULL);
    memcpy(nombre, carga, len);
    nombre[len] = '\0';
    if (!nombre_valido(nombre, len))
        return responder_error(c, ESTADO_SOLICITUD, "Nombre de contenido inválido",
                               "ABRIR con caracteres o componentes de ruta no permitidos");

    reg_info(c->ip, "ABRIR \"%s\"", nombre);

    Sesion *ses = NULL;
    uint64_t total = 0;
    estado_t e = sesion_abrir_demanda(c->svc->demanda, nombre, &ses, &total);
    if (e != SRV_OK) {
        uint16_t cod = codigo_de(e);
        char det[MAX_NOMBRE + 128];
        snprintf(det, sizeof det, "ABRIR \"%s\": %s (%s)", nombre, texto_de(cod),
                 nombre_estado_servicio(e));
        return responder_error(c, cod, texto_de(cod), det);
    }

    uint8_t tam_be[8];
    red_u64_a_be(total, tam_be);
    if (responder(c, MSG_ACEPTADO, ESTADO_EXITO, tam_be, sizeof tam_be) != CONTINUAR) {
        sesion_cerrar(ses);
        return TERMINAR;
    }

    uint8_t *buf = malloc(TAM_BLOQUE_DATOS);   /* memoria acotada (RNF-09) */
    if (buf == NULL) {
        sesion_cerrar(ses);
        return responder_error(c, ESTADO_INTERNO, texto_de(ESTADO_INTERNO), "malloc en ABRIR");
    }

    uint64_t enviados = 0;
    int fallo_lectura = 0;
    int resultado = CONTINUAR;

    for (;;) {
        if (!servidor_corriendo) {
            avisar_detencion(c);
            resultado = TERMINAR;
            goto fin;
        }
        size_t leidos = 0;
        e = sesion_leer_bloque(ses, buf, TAM_BLOQUE_DATOS, &leidos);
        if (e != SRV_OK) {
            fallo_lectura = 1;
            break;
        }
        if (leidos == 0)
            break;
        if (enviados + leidos > total) {   /* el archivo creció: no cumplir lo anunciado */
            fallo_lectura = 1;
            break;
        }
        int r = red_enviar(c->fd, MSG_DATOS, ESTADO_EXITO, c->id, buf, (uint32_t)leidos);
        if (r == RED_INTERRUMPIDO) {
            /* El cliente no consumía y el servidor se detiene. El mensaje pudo
               quedar a medias, así que no se envía DESPEDIDA: solo se cierra. */
            reg_info(c->ip, "Transferencia de \"%s\" interrumpida por la detención del servidor "
                     "(%" PRIu64 "/%" PRIu64 " bytes)", nombre, enviados, total);
            resultado = TERMINAR;
            goto fin;
        }
        if (r != RED_OK) {
            /* RF-35: el cliente desapareció a mitad de transferencia */
            reg_error(c->ip, "Transferencia de \"%s\" abandonada en %" PRIu64 "/%" PRIu64
                      " bytes: %s", nombre, enviados, total, red_describir(r));
            estado_sumar(CNT_ERRORES, 1);
            resultado = TERMINAR;
            goto fin;
        }
        enviados += leidos;
    }

    if (fallo_lectura || enviados != total) {
        /* RF-32: no se cumplió el tamaño anunciado -> ERROR explícito, no FIN */
        char det[MAX_NOMBRE + 128];
        snprintf(det, sizeof det, "Lectura incompleta de \"%s\": %" PRIu64 " de %" PRIu64 " bytes",
                 nombre, enviados, total);
        resultado = responder_error(c, ESTADO_NO_SERVIBLE,
                                    "Error al leer el contenido; transferencia abortada", det);
    } else {
        resultado = responder(c, MSG_FINAL, ESTADO_EXITO, NULL, 0);
        if (resultado == CONTINUAR) {
            estado_sumar(CNT_TRANSFERENCIAS, 1);
            reg_info(c->ip, "Transferencia de \"%s\" completada (%" PRIu64 " bytes)", nombre, enviados);
        }
    }

fin:
    free(buf);
    sesion_cerrar(ses);
    return resultado;
}

/* Resultado de revisar el socket mientras se emite el canal */
#define CANAL_SEGUIR    1
#define CANAL_SALIR     2
#define CANAL_TERMINAR  0

static int revisar_cliente_en_canal(Ctx *c, uint32_t id_canal)
{
    HeaderProtocolo h;
    uint8_t carga[MAX_CARGA_CLIENTE + 1];

    if (!recibir_solicitud(c, &h, carga))
        return CANAL_TERMINAR;

    int r;
    switch (h.tipo) {
    case MSG_SALIR:
        reg_info(c->ip, "SALIR: abandona el canal en vivo");
        r = responder(c, MSG_FINAL, ESTADO_EXITO, NULL, 0) ? CANAL_SALIR : CANAL_TERMINAR;
        break;
    case MSG_CERRAR:
        responder(c, MSG_DESPEDIDA, ESTADO_EXITO, NULL, 0);
        reg_info(c->ip, "CERRAR durante el canal: sesión concluida por el cliente");
        r = CANAL_TERMINAR;
        break;
    default:
        r = responder_error(c, ESTADO_NO_PERMITIDO,
                            "Operación no permitida mientras se recibe el canal",
                            "solicitud durante la emisión del canal") ? CANAL_SEGUIR : CANAL_TERMINAR;
        break;
    }
    c->id = id_canal;   /* los DATOS siguientes pertenecen al UNIR original */
    return r;
}

/* ---------- Ritmo del canal ----------
 *
 * sesion_leer_bloque() del canal entrega bytes tan rápido como se le pidan. Para
 * que el cliente reciba el contenido al ritmo de reproducción (y no adelante de
 * la línea de tiempo compartida), esta capa lleva un "espejo" de la posición de
 * la sesión y solo pide los bytes que ya deberían haberse emitido:
 *
 *     bytes permitidos del elemento i = tamaño_i * (t_actual - inicio_i) / duración_i
 *
 * Todo se calcula desde t0 con el reloj monótono, nunca por incrementos, así que
 * el error no se acumula (riesgo 1 de la sección 18.2). El espejo reproduce la
 * misma aritmética que sesion_abrir_canal() (offset = tamaño*offset_ms/duración)
 * y el mismo cambio de elemento que sesion_leer_bloque() (al agotar el archivo).
 */
typedef struct {
    size_t   indice;      /* elemento del canal que se está emitiendo           */
    uint64_t tamano;      /* bytes de ese elemento                              */
    uint64_t emitidos;    /* bytes ya entregados de ese elemento                */
    int64_t  inicio_ms;   /* transcurrido (sin módulo) en que empezó ese elemento */
} RitmoCanal;

static uint64_t tamano_canal(const Servicio *svc, size_t i)
{
    const EntradaInterna *en = catalogo_obtener(svc->canal, i);
    return en ? en->tamano : 0;
}

static void ritmo_iniciar(RitmoCanal *r, const Servicio *svc, int64_t transcurrido, const PuntoCanal *p)
{
    r->indice = p->indice;
    r->tamano = tamano_canal(svc, p->indice);
    r->emitidos = (uint64_t)(((unsigned __int128)r->tamano * (uint64_t)p->offset_ms) /
                             (uint64_t)svc->duraciones[p->indice]);
    r->inicio_ms = transcurrido - p->offset_ms;
}

/* Bytes que ya se pueden pedir a sesion_leer_bloque(), sin pasar del elemento actual. */
static uint64_t ritmo_permitidos(RitmoCanal *r, const Servicio *svc, int64_t transcurrido)
{
    while (r->emitidos >= r->tamano) {          /* elemento agotado: pasa al siguiente */
        r->inicio_ms += svc->duraciones[r->indice];
        r->indice = (r->indice + 1) % svc->n_canal;
        r->tamano = tamano_canal(svc, r->indice);
        r->emitidos = 0;
        if (r->tamano == 0)
            return 0;
    }
    int64_t dur = svc->duraciones[r->indice];
    int64_t dentro = transcurrido - r->inicio_ms;
    if (dentro <= 0)
        return 0;
    uint64_t limite = (dentro >= dur)
        ? r->tamano
        : (uint64_t)(((unsigned __int128)r->tamano * (uint64_t)dentro) / (uint64_t)dur);
    return (limite > r->emitidos) ? limite - r->emitidos : 0;
}

static int manejar_unir(Ctx *c, uint32_t len)
{
    if (len != 0)   /* RF-05: el cliente no puede elegir posición */
        return responder_error(c, ESTADO_NO_PERMITIDO, "El canal no admite elegir posición",
                               "UNIR con posición solicitada");

    const Servicio *svc = c->svc;
    int64_t transcurrido = reloj_monotonico_ms() - svc->t0_ms;   /* t - t0 */
    PuntoCanal punto;
    Sesion *ses = NULL;
    estado_t e = canal_calcular_punto(svc->duraciones, svc->n_canal, transcurrido, &punto);
    if (e == SRV_OK)
        e = sesion_abrir_canal(svc->canal, transcurrido, &ses);
    if (e != SRV_OK) {
        uint16_t cod = codigo_de(e);
        char det[96];
        snprintf(det, sizeof det, "UNIR: canal no disponible (%s)", nombre_estado_servicio(e));
        return responder_error(c, cod, "El canal en vivo no está disponible", det);
    }

    /* EMISION: posición dentro del ciclo p(t) = (t - t0) mod T, y T (sección 8.2). */
    int64_t pos = transcurrido % svc->ciclo_ms;
    uint8_t meta[16];
    red_u64_a_be((uint64_t)pos, meta);
    red_u64_a_be((uint64_t)svc->ciclo_ms, meta + 8);
    if (responder(c, MSG_EMISION, ESTADO_EXITO, meta, sizeof meta) != CONTINUAR) {
        sesion_cerrar(ses);
        return TERMINAR;
    }
    /* RF-63: evidencia auditable del desfase en cualquiera de los tres modos */
    const EntradaInterna *en = catalogo_obtener(svc->canal, punto.indice);
    reg_info(c->ip, "UNIR: incorporado al canal en la posición %" PRId64 " ms de un ciclo de %" PRId64
             " ms (t-t0 = %" PRId64 " ms; elemento %zu \"%s\", desde el ms %" PRId64 ")",
             pos, svc->ciclo_ms, transcurrido, punto.indice, en ? en->nombre : "?", punto.offset_ms);

    RitmoCanal ritmo;
    ritmo_iniciar(&ritmo, svc, transcurrido, &punto);

    uint8_t *buf = malloc(TAM_BLOQUE_DATOS);
    if (buf == NULL) {
        sesion_cerrar(ses);
        return responder_error(c, ESTADO_INTERNO, texto_de(ESTADO_INTERNO), "malloc en UNIR");
    }

    uint32_t id_canal = c->id;
    int resultado = CONTINUAR;

    for (;;) {
        if (!servidor_corriendo) {
            avisar_detencion(c);
            resultado = TERMINAR;
            break;
        }

        /* ¿El cliente mandó algo (SALIR, CERRAR) o se desconectó? */
        if (red_esperar_datos(c->fd, 0) != 0) {
            int acc = revisar_cliente_en_canal(c, id_canal);
            if (acc == CANAL_SALIR)
                break;
            if (acc == CANAL_TERMINAR) {
                resultado = TERMINAR;
                break;
            }
        }

        uint64_t permitidos = ritmo_permitidos(&ritmo, svc, reloj_monotonico_ms() - svc->t0_ms);
        if (permitidos == 0) {
            /* Aún no toca enviar más: se espera sin quemar CPU, atento al cliente. */
            red_esperar_datos(c->fd, ESPERA_CANAL_MS);
            continue;
        }
        size_t pedir = permitidos < TAM_BLOQUE_DATOS ? (size_t)permitidos : TAM_BLOQUE_DATOS;
        size_t leidos = 0;
        e = sesion_leer_bloque(ses, buf, pedir, &leidos);
        if (e != SRV_OK || leidos == 0) {
            char det[96];
            snprintf(det, sizeof det, "error leyendo el contenido del canal (%s)", nombre_estado_servicio(e));
            resultado = responder_error(c, ESTADO_INTERNO, "El canal dejó de estar disponible", det);
            break;
        }
        ritmo.emitidos += leidos;
        int r = red_enviar(c->fd, MSG_DATOS, ESTADO_EXITO, id_canal, buf, (uint32_t)leidos);
        if (r != RED_OK) {
            reg_info(c->ip, "Cliente abandonó el canal: %s", red_describir(r));
            resultado = TERMINAR;
            break;
        }
    }

    free(buf);
    sesion_cerrar(ses);
    return resultado;
}

/* ---------- Bucle de la sesión ---------- */

void atender_cliente(int fd, const char *ip, const Servicio *svc, const ConfigServidor *cfg)
{
    Ctx c = { fd, ip, svc, cfg->inactividad_s > 0 ? cfg->inactividad_s * 1000 : -1, 0 };
    HeaderProtocolo h;
    uint8_t carga[MAX_CARGA_CLIENTE + 1];

    /* RF-34 (cliente que deja de consumir): lo cubre el plazo de escritura que
       servidor_ejecutar() fija con red_configurar_escritura(). */

    /* Estado Inicio: solo se admite SALUDO */
    if (!recibir_solicitud(&c, &h, carga))
        return;
    if (h.tipo != MSG_SALUDO) {
        responder_error(&c, ESTADO_NO_PERMITIDO, "La sesión debe comenzar con SALUDO",
                        "primer mensaje distinto de SALUDO");
        return;
    }
    static const char servicios[] = "LISTAR,ABRIR,UNIR,SALIR,CERRAR";
    if (responder(&c, MSG_BIENVENIDA, ESTADO_EXITO, servicios, sizeof servicios - 1) != CONTINUAR)
        return;
    reg_info(ip, "SALUDO aceptado (protocolo v%d)", PROTOCOL_VERSION);

    /* Estado Lista */
    for (;;) {
        if (!recibir_solicitud(&c, &h, carga))
            return;

        int seguir;
        switch (h.tipo) {
        case MSG_LISTAR:
            seguir = manejar_listar(&c);
            break;
        case MSG_ABRIR:
            seguir = manejar_abrir(&c, carga, h.longitud);
            break;
        case MSG_UNIR:
            seguir = manejar_unir(&c, h.longitud);
            break;
        case MSG_CERRAR:
            responder(&c, MSG_DESPEDIDA, ESTADO_EXITO, NULL, 0);
            reg_info(ip, "CERRAR: sesión concluida por el cliente");
            return;
        case MSG_SALUDO:
            seguir = responder_error(&c, ESTADO_NO_PERMITIDO, "La sesión ya fue iniciada", "SALUDO repetido");
            break;
        case MSG_SALIR:
            seguir = responder_error(&c, ESTADO_NO_PERMITIDO, "No hay emisión de canal activa",
                                     "SALIR fuera del canal");
            break;
        default:
            seguir = responder_error(&c, ESTADO_SOLICITUD, "Tipo de mensaje desconocido", NULL);
            break;
        }
        if (seguir != CONTINUAR)
            return;
    }
}

/*
 * red.c — Implementación de E/S robusta y serialización del protocolo.
 */
#include "red.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RODAJA_MS 200   /* cada cuánto se revisa la bandera de detención */

static int64_t ahora_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int red_leer_exacto(int fd, void *buf, size_t n, int timeout_ms,
                    const atomic_int *seguir)
{
    uint8_t *p = buf;
    size_t hecho = 0;
    int64_t limite = (timeout_ms >= 0) ? ahora_ms() + timeout_ms : -1;

    while (hecho < n) {
        if (seguir != NULL && !*seguir)
            return RED_INTERRUMPIDO;

        int espera = RODAJA_MS;
        if (limite >= 0) {
            int64_t resta = limite - ahora_ms();
            if (resta <= 0)
                return RED_TIEMPO;
            if (resta < espera)
                espera = (int)resta;
        }

        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int r = poll(&pfd, 1, espera);
        if (r < 0) {
            if (errno == EINTR)
                continue;           /* una señal: se vuelve a revisar *seguir */
            return RED_ERROR;
        }
        if (r == 0)
            continue;               /* rodaja vencida sin datos */

        ssize_t k = recv(fd, p + hecho, n - hecho, 0);
        if (k == 0)
            return RED_CERRADO;
        if (k < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            if (errno == ECONNRESET)
                return RED_CERRADO;
            return RED_ERROR;
        }
        hecho += (size_t)k;
    }
    return RED_OK;
}

/* Configuración de escritura del proceso (la fija el servidor al arrancar). */
static const atomic_int *g_seguir = NULL;
static int g_plazo_escritura_ms = -1;

void red_configurar_escritura(const atomic_int *seguir, int plazo_ms)
{
    g_seguir = seguir;
    g_plazo_escritura_ms = plazo_ms;
}

/*
 * Escritura con send(MSG_DONTWAIT) + poll(): nunca queda bloqueada más de
 * RODAJA_MS sin revisar la bandera de detención, y si el cliente no consume
 * nada durante plazo_ms (sin progreso) devuelve RED_TIEMPO (RF-34).
 */
static int escribir(int fd, const void *buf, size_t n,
                    const atomic_int *seguir, int plazo_ms, int flags_extra)
{
    const uint8_t *p = buf;
    int64_t limite = (plazo_ms >= 0) ? ahora_ms() + plazo_ms : -1;

    while (n > 0) {
        ssize_t k = send(fd, p, n, MSG_NOSIGNAL | MSG_DONTWAIT | flags_extra);
        if (k > 0) {
            p += k;
            n -= (size_t)k;
            if (plazo_ms >= 0)
                limite = ahora_ms() + plazo_ms;   /* hubo progreso: se reinicia el plazo */
            continue;
        }
        if (k < 0 && errno == EINTR)
            continue;
        if (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            if (errno == EPIPE || errno == ECONNRESET)
                return RED_CERRADO;
            return RED_ERROR;
        }

        /* Buffer de envío lleno: el cliente no está consumiendo. */
        if (seguir != NULL && !*seguir)
            return RED_INTERRUMPIDO;
        int espera = RODAJA_MS;
        if (limite >= 0) {
            int64_t resta = limite - ahora_ms();
            if (resta <= 0)
                return RED_TIEMPO;
            if (resta < espera)
                espera = (int)resta;
        }
        struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
        if (poll(&pfd, 1, espera) < 0 && errno != EINTR)
            return RED_ERROR;
    }
    return RED_OK;
}

int red_escribir_exacto(int fd, const void *buf, size_t n)
{
    return escribir(fd, buf, n, g_seguir, g_plazo_escritura_ms, 0);
}

int red_descartar(int fd, uint32_t n, int timeout_ms, const atomic_int *seguir)
{
    uint8_t basura[512];
    while (n > 0) {
        size_t trozo = n < sizeof basura ? n : sizeof basura;
        int r = red_leer_exacto(fd, basura, trozo, timeout_ms, seguir);
        if (r != RED_OK)
            return r;
        n -= (uint32_t)trozo;
    }
    return RED_OK;
}

void red_ajustar_socket(int fd)
{
    /* Sin esto, un mensaje pequeño (FIN, CATALOGO, ERROR) que sigue a uno
       grande queda retenido por el algoritmo de Nagle hasta que llega el ACK,
       y el receptor lo demora ~40 ms (ACK diferido): cada solicitud pagaba
       40 ms extra. El protocolo ya agrupa sus escrituras, así que se desactiva. */
    int uno = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &uno, sizeof uno);
}

int red_esperar_datos(int fd, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
    int r = poll(&pfd, 1, timeout_ms);
    if (r < 0)
        return (errno == EINTR) ? 0 : -1;
    if (r == 0)
        return 0;
    if (pfd.revents & POLLNVAL)
        return -1;
    return 1;   /* datos, o POLLHUP/POLLERR: la siguiente lectura lo reportará */
}

static void poner16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)(v & 0xFF);
}

static void poner32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)((v >> 16) & 0xFF);
    b[2] = (uint8_t)((v >> 8) & 0xFF);
    b[3] = (uint8_t)(v & 0xFF);
}

static uint16_t tomar16(const uint8_t *b)
{
    return (uint16_t)((b[0] << 8) | b[1]);
}

static uint32_t tomar32(const uint8_t *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

void red_serializar_header(const HeaderProtocolo *h, uint8_t out[TAM_HEADER])
{
    poner16(out + 0, h->version);
    poner16(out + 2, h->tipo);
    poner32(out + 4, h->longitud);
    poner16(out + 8, h->estado);
    poner32(out + 10, h->id);
}

void red_deserializar_header(const uint8_t in[TAM_HEADER], HeaderProtocolo *h)
{
    h->version  = tomar16(in + 0);
    h->tipo     = tomar16(in + 2);
    h->longitud = tomar32(in + 4);
    h->estado   = tomar16(in + 8);
    h->id       = tomar32(in + 10);
}

int red_recibir_header(int fd, HeaderProtocolo *h, int timeout_ms,
                       const atomic_int *seguir)
{
    uint8_t bruto[TAM_HEADER];
    int r = red_leer_exacto(fd, bruto, sizeof bruto, timeout_ms, seguir);
    if (r == RED_OK)
        red_deserializar_header(bruto, h);
    return r;
}

static int enviar(int fd, uint16_t tipo, uint16_t estado, uint32_t id,
                  const void *carga, uint32_t len, const atomic_int *seguir, int plazo_ms)
{
    HeaderProtocolo h = { PROTOCOL_VERSION, tipo, len, estado, id };

    /* Mensajes pequeños: header y carga en un solo send() para no pagar el
       retardo de Nagle + ACK diferido entre dos escrituras pequeñas. */
    uint8_t junto[TAM_HEADER + 512];
    if (len <= 512) {
        red_serializar_header(&h, junto);
        if (len > 0)
            memcpy(junto + TAM_HEADER, carga, len);
        return escribir(fd, junto, TAM_HEADER + (size_t)len, seguir, plazo_ms, 0);
    }

    /* MSG_MORE: el kernel retiene el header y lo manda en el mismo segmento
       que el inicio de la carga, en vez de un paquete de 14 bytes suelto. */
    uint8_t cab[TAM_HEADER];
    red_serializar_header(&h, cab);
    int r = escribir(fd, cab, sizeof cab, seguir, plazo_ms, MSG_MORE);
    if (r != RED_OK)
        return r;
    return escribir(fd, carga, len, seguir, plazo_ms, 0);
}

int red_enviar(int fd, uint16_t tipo, uint16_t estado, uint32_t id,
               const void *carga, uint32_t len)
{
    return enviar(fd, tipo, estado, id, carga, len, g_seguir, g_plazo_escritura_ms);
}

int red_enviar_urgente(int fd, uint16_t tipo, uint16_t estado, uint32_t id,
                       const void *carga, uint32_t len)
{
    return enviar(fd, tipo, estado, id, carga, len, NULL, PLAZO_URGENTE_MS);
}

void red_u64_a_be(uint64_t v, uint8_t out[8])
{
    for (int i = 7; i >= 0; i--) {
        out[i] = (uint8_t)(v & 0xFF);
        v >>= 8;
    }
}

uint64_t red_be_a_u64(const uint8_t in[8])
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | in[i];
    return v;
}

const char *protocolo_nombre_msg(uint16_t tipo)
{
    switch (tipo) {
    case MSG_SALUDO:     return "SALUDO";
    case MSG_BIENVENIDA: return "BIENVENIDA";
    case MSG_LISTAR:     return "LISTAR";
    case MSG_CATALOGO:   return "CATALOGO";
    case MSG_ABRIR:      return "ABRIR";
    case MSG_ACEPTADO:   return "ACEPTADO";
    case MSG_DATOS:      return "DATOS";
    case MSG_FINAL:        return "FIN";
    case MSG_ERROR:      return "ERROR";
    case MSG_CERRAR:     return "CERRAR";
    case MSG_DESPEDIDA:  return "DESPEDIDA";
    case MSG_UNIR:       return "UNIR";
    case MSG_EMISION:    return "EMISION";
    case MSG_SALIR:      return "SALIR";
    default:             return "DESCONOCIDO";
    }
}

const char *protocolo_nombre_estado(uint16_t estado)
{
    switch (estado) {
    case ESTADO_EXITO:        return "Éxito";
    case ESTADO_VERSION:      return "Versión";
    case ESTADO_SOLICITUD:    return "Solicitud";
    case ESTADO_NO_HALLADO:   return "No hallado";
    case ESTADO_NO_SERVIBLE:  return "No servible";
    case ESTADO_NO_PERMITIDO: return "No permitido";
    case ESTADO_CAPACIDAD:    return "Capacidad";
    case ESTADO_INTERNO:      return "Interno";
    default:                  return "Desconocido";
    }
}

const char *red_describir(int resultado)
{
    switch (resultado) {
    case RED_OK:           return "correcto";
    case RED_CERRADO:      return "conexión cerrada por el otro extremo";
    case RED_TIEMPO:       return "tiempo límite vencido";
    case RED_INTERRUMPIDO: return "detención del servidor";
    default:               return "error de red";
    }
}

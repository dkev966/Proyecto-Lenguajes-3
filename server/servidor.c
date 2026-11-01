/*
 * servidor.c — Arranque común: socket de escucha, estado compartido, señales,
 * despacho al modo de concurrencia y cierre ordenado.
 */
#include "servidor.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "codigos_salida.h"
#include "estado.h"
#include "protocolo.h"
#include "red.h"
#include "registro.h"
#include "senales.h"

void config_servidor_por_omision(ConfigServidor *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->modo = MODO_FIFO;
    cfg->direccion = "0.0.0.0";
    cfg->puerto = 0;
    cfg->profundidad_admision = 16;
    cfg->max_clientes = 32;
    cfg->inactividad_s = 30;
    cfg->ruta_registro = NULL;
    cfg->verboso = 0;
}

const char *modo_nombre(modo_t m)
{
    switch (m) {
    case MODO_FIFO:     return "FIFO";
    case MODO_FORKED:   return "FORKED";
    case MODO_THREADED: return "THREADED";
    }
    return "?";
}

int modo_desde_texto(const char *s, modo_t *out)
{
    if (s == NULL)
        return -1;
    if (strcmp(s, "FIFO") == 0)     { *out = MODO_FIFO;     return 0; }
    if (strcmp(s, "FORKED") == 0)   { *out = MODO_FORKED;   return 0; }
    if (strcmp(s, "THREADED") == 0) { *out = MODO_THREADED; return 0; }
    return -1;
}

/* Crea, enlaza y pone a escuchar el socket. IPv4 o IPv6 según la dirección. */
static int crear_socket_escucha(const ConfigServidor *cfg)
{
    char puerto_txt[8];
    snprintf(puerto_txt, sizeof puerto_txt, "%d", cfg->puerto);

    struct addrinfo pista, *res = NULL;
    memset(&pista, 0, sizeof pista);
    pista.ai_family = AF_UNSPEC;
    pista.ai_socktype = SOCK_STREAM;
    pista.ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV;

    int r = getaddrinfo(cfg->direccion, puerto_txt, &pista, &res);
    if (r != 0) {
        reg_error(NULL, "Dirección de escucha inválida \"%s\": %s", cfg->direccion, gai_strerror(r));
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        reg_error(NULL, "No se pudo crear el socket: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    int uno = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &uno, sizeof uno);

    if (bind(fd, res->ai_addr, res->ai_addrlen) < 0) {
        int err = errno;
        if (err == EADDRINUSE)
            reg_error(NULL, "El puerto %d ya está ocupado por otro proceso", cfg->puerto);
        else
            reg_error(NULL, "No se pudo enlazar %s:%d: %s", cfg->direccion, cfg->puerto, strerror(err));
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    if (listen(fd, cfg->profundidad_admision) < 0) {
        reg_error(NULL, "listen() falló: %s", strerror(errno));
        close(fd);
        return -1;
    }

    /* No bloqueante: tras poll() un accept() nunca se queda colgado aunque el
       cliente haya abortado entre medio. Los sockets aceptados NO heredan este
       flag en Linux, así que las sesiones siguen siendo bloqueantes. */
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    return fd;
}

int servidor_aceptar(int server_fd, char *ip, size_t ip_cap)
{
    /* poll con plazo: evita la carrera "llega SIGINT justo antes de accept()"
       que dejaría al servidor bloqueado para siempre. */
    struct pollfd pfd = { .fd = server_fd, .events = POLLIN, .revents = 0 };
    if (poll(&pfd, 1, 500) <= 0)
        return -1;

    struct sockaddr_storage dir;
    socklen_t len = sizeof dir;   /* se reinicia en cada llamada */
    int fd = accept(server_fd, (struct sockaddr *)&dir, &len);
    if (fd < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNABORTED)
            reg_error(NULL, "accept() falló: %s", strerror(errno));
        return -1;
    }

    red_ajustar_socket(fd);

    char host[INET6_ADDRSTRLEN] = "?";
    char serv[8] = "?";
    getnameinfo((struct sockaddr *)&dir, len, host, sizeof host, serv, sizeof serv,
                NI_NUMERICHOST | NI_NUMERICSERV);
    snprintf(ip, ip_cap, "%s:%s", host, serv);
    return fd;
}

int servidor_conexiones_pendientes(int server_fd)
{
    /* En Linux, para un socket en LISTEN, tcpi_unacked = largo actual de la
       cola de conexiones completadas y tcpi_sacked = su máximo. */
    struct tcp_info ti;
    socklen_t len = sizeof ti;
    memset(&ti, 0, sizeof ti);
    if (getsockopt(server_fd, IPPROTO_TCP, TCP_INFO, &ti, &len) < 0)
        return -1;
    return (int)ti.tcpi_unacked;
}

void servidor_rechazar(int fd, const char *ip, int codigo, const char *motivo)
{
    reg_error(ip, "Conexión rechazada [%s]: %s", protocolo_nombre_estado((uint16_t)codigo), motivo);
    estado_sumar(CNT_RECHAZADAS, 1);
    red_enviar_urgente(fd, MSG_ERROR, (uint16_t)codigo, 0, motivo, (uint32_t)strlen(motivo));
    close(fd);
}

int servidor_ejecutar(const ConfigServidor *cfg, const Servicio *svc)
{
    if (senales_instalar() < 0) {
        fprintf(stderr, "No se pudieron instalar los manejadores de señales\n");
        return SALIDA_INTERNO;
    }
    if (estado_iniciar() < 0) {
        fprintf(stderr, "No se pudo crear la memoria compartida del servidor\n");
        return SALIDA_INTERNO;
    }
    /* Toda escritura a un cliente: se aborta si el servidor se detiene y, si el
       cliente no consume nada durante el tiempo de inactividad, se abandona. */
    red_configurar_escritura(&servidor_corriendo,
                             cfg->inactividad_s > 0 ? cfg->inactividad_s * 1000 : -1);

    if (registro_iniciar(modo_nombre(cfg->modo), cfg->ruta_registro, cfg->verboso) < 0) {
        fprintf(stderr, "No se pudo abrir el archivo de registro \"%s\"\n", cfg->ruta_registro);
        estado_destruir();
        return SALIDA_ARGUMENTOS;
    }

    int server_fd = crear_socket_escucha(cfg);
    if (server_fd < 0) {
        registro_cerrar();
        estado_destruir();
        return SALIDA_RED;
    }

    /* RF-22 */
    reg_info(NULL, "Servidor iniciado en modo %s, escuchando en %s:%d "
             "(cola de admisión %d, máx. clientes %d, inactividad %d s)",
             modo_nombre(cfg->modo), cfg->direccion, cfg->puerto,
             cfg->profundidad_admision, cfg->max_clientes, cfg->inactividad_s);

    int r = -1;
    switch (cfg->modo) {
    case MODO_FIFO:     r = iniciar_modo_fifo(server_fd, cfg, svc);     break;
    case MODO_FORKED:   r = iniciar_modo_forked(server_fd, cfg, svc);   break;
    case MODO_THREADED: r = iniciar_modo_threaded(server_fd, cfg, svc); break;
    }

    close(server_fd);

    Contadores c = estado_leer();
    reg_info(NULL, "Resumen: %llu admitidas, %llu rechazadas, %llu transferencias completas, "
             "%llu errores, %lld activas al cerrar",
             (unsigned long long)c.admitidas, (unsigned long long)c.rechazadas,
             (unsigned long long)c.transferencias, (unsigned long long)c.errores,
             (long long)c.activos);
    reg_info(NULL, "Servidor detenido ordenadamente");

    /* Si quedaron hilos que no cerraron a tiempo (r == 1 en THREADED) todavía
       podrían usar el registro: no se libera la memoria compartida, el
       sistema la recupera al terminar el proceso. */
    if (r == 0) {
        registro_cerrar();
        estado_destruir();
    }
    return r < 0 ? SALIDA_INTERNO : SALIDA_EXITO;
}

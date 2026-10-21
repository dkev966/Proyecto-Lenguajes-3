#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 8080
#define PROTOCOL_VERSION 1

#define MSG_SALUDO      1
#define MSG_BIENVENIDA  2
#define MSG_LISTAR      3
#define MSG_CATALOGO    4
#define MSG_ABRIR       5
#define MSG_ACEPTADO    6
#define MSG_DATOS       7
#define MSG_FIN         8
#define MSG_ERROR       9
#define MSG_UNIR        12
#define MSG_EMISION     13

typedef struct {
    uint16_t version;
    uint16_t tipo;
    uint32_t longitud;
} __attribute__((packed)) HeaderProtocolo;

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Fallo al crear socket mock");
        exit(EXIT_FAILURE);
    }

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Fallo el bind del mock");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 3) < 0) {
        perror("Fallo el listen del mock");
        exit(EXIT_FAILURE);
    }

    printf("[SERVIDOR MOCK] Listo y esperando conexiones en el puerto %d...\n", PORT);

    while (1) {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("Fallo accept");
            continue;
        }
        printf("\n[SERVIDOR MOCK] ¡Cliente conectado!\n");

        // 1. Leer SALUDO
        HeaderProtocolo h_in;
        if (read(new_socket, &h_in, sizeof(h_in)) <= 0) { close(new_socket); continue; }

        if (ntohs(h_in.tipo) == MSG_SALUDO) {
            HeaderProtocolo h_bienvenida = {htons(PROTOCOL_VERSION), htons(MSG_BIENVENIDA), htonl(0)};
            write(new_socket, &h_bienvenida, sizeof(h_bienvenida));
            printf("[SERVIDOR MOCK] Respondió BIENVENIDA.\n");
        }

        // 2. Leer Solicitud
        if (read(new_socket, &h_in, sizeof(h_in)) > 0) {
            uint16_t tipo_req = ntohs(h_in.tipo);
            uint32_t len_req = ntohl(h_in.longitud);

            if (tipo_req == MSG_LISTAR) {
                printf("[SERVIDOR MOCK] Solicitud recibida: LISTAR\n");
                const char *cat_demo = "1. video1.mp4 (10 MB)\n2. pelicula.mp4 (500 MB)\n3. documental.mp4 (150 MB)";
                uint32_t len_cat = strlen(cat_demo);

                HeaderProtocolo h_cat = {htons(PROTOCOL_VERSION), htons(MSG_CATALOGO), htonl(len_cat)};
                write(new_socket, &h_cat, sizeof(h_cat));
                write(new_socket, cat_demo, len_cat);
                printf("[SERVIDOR MOCK] Catálogo enviado exitosamente.\n");

            } else if (tipo_req == MSG_ABRIR) {
                char nom_archivo[256] = {0};
                read(new_socket, nom_archivo, len_req < 256 ? len_req : 255);
                printf("[SERVIDOR MOCK] Solicitud recibida: ABRIR archivo '%s'\n", nom_archivo);

                // Enviar ACEPTADO
                HeaderProtocolo h_ok = {htons(PROTOCOL_VERSION), htons(MSG_ACEPTADO), htonl(0)};
                write(new_socket, &h_ok, sizeof(h_ok));

                // Enviar bloque de DATOS
                const char *contenido_falso = "Este es el contenido binario de prueba del archivo simulado.\n";
                uint32_t len_falso = strlen(contenido_falso);
                HeaderProtocolo h_datos = {htons(PROTOCOL_VERSION), htons(MSG_DATOS), htonl(len_falso)};
                write(new_socket, &h_datos, sizeof(h_datos));
                write(new_socket, contenido_falso, len_falso);

                // Enviar FIN
                HeaderProtocolo h_fin = {htons(PROTOCOL_VERSION), htons(MSG_FIN), htonl(0)};
                write(new_socket, &h_fin, sizeof(h_fin));
                printf("[SERVIDOR MOCK] Archivo y mensaje FIN enviados con éxito.\n");

            } else if (tipo_req == MSG_UNIR) {
                printf("[SERVIDOR MOCK] Solicitud recibida: UNIR (Canal en vivo)\n");
                HeaderProtocolo h_emision = {htons(PROTOCOL_VERSION), htons(MSG_EMISION), htonl(0)};
                write(new_socket, &h_emision, sizeof(h_emision));
                printf("[SERVIDOR MOCK] Confirmación EMISION enviada al cliente.\n");
            }
        }

        close(new_socket);
        printf("[SERVIDOR MOCK] Sesión concluida con el cliente.\n");
    }

    close(server_fd);
    return 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
//RF que se cumplen: 08,11,24,25,26,29,64,66,67,68,69
// Códigos de salida estándar para cumplir con la especificación (RF-29 y RF-68)
#define EXIT_SUCCESS_CUSTOM 0
#define EXIT_ARGS_ERROR     1
#define EXIT_NET_ERROR      2
#define EXIT_CONTENT_ERROR  3

// Definición de tipos de mensajes del protocolo (Sección 6.3 del documento)
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
#define MSG_CERRAR      10
#define MSG_DESPEDIDA   11
#define MSG_UNIR        12
#define MSG_EMISION     13

// Estructura de la cabecera del protocolo (8 bytes fijos)
typedef struct {
    uint16_t version;   
    uint16_t tipo;      
    uint32_t longitud;  
} __attribute__((packed)) HeaderProtocolo;

// Función de ayuda (RF-26)RF-66
void mostrar_ayuda(const char *nombre_programa) {
    printf("Uso: %s -h <direccion> -p <puerto> -a <accion> [-f <archivo>] [-d <destino>]\n", nombre_programa);
    printf("\nAcciones disponibles:\n");
    printf("  canal     : Conectarse al canal en vivo.\n");
    printf("  listar    : Consultar el catálogo de contenidos.\n");
    printf("  descargar : Solicitar un contenido bajo demanda.\n");
    printf("\nEjemplos:\n");
    printf("  %s -h 127.0.0.1 -p 8080 -a listar\n", nombre_programa);
    printf("  %s -h 127.0.0.1 -p 8080 -a descargar -f video1.mp4 -d ./video_recibido.mp4\n", nombre_programa);
}

int main(int argc, char *argv[]) {
    char *direccion = NULL;
    int puerto = -1;
    char *accion = NULL;
    char *archivo = NULL;
    char *destino = NULL;

    // 1. Análisis de argumentos (RF-24) RF-64 y RF-67
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            direccion = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            puerto = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            accion = argv[++i];
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            archivo = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            destino = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-help") == 0) {
            mostrar_ayuda(argv[0]);
            exit(EXIT_SUCCESS_CUSTOM);
        }
    }

    // Validación previa obligatoria (RF-25 y 64)
    if (direccion == NULL || puerto <= 0 || puerto > 65535 || accion == NULL) {
        fprintf(stderr, "Error: Faltan argumentos obligatorios o son inválidos (-h, -p, -a).\n");
        mostrar_ayuda(argv[0]);
        exit(EXIT_ARGS_ERROR);
    }

    if (strcmp(accion, "descargar") == 0) {//RF67
        if (archivo == NULL || destino == NULL) {
            fprintf(stderr, "Error: La acción 'descargar' requiere -f <archivo> y -d <destino>.\n");
            exit(EXIT_ARGS_ERROR);
        }
    } else if (strcmp(accion, "canal") != 0 && strcmp(accion, "listar") != 0) {
        fprintf(stderr, "Error: Accion desconocida '%s'. Use 'canal', 'listar' o 'descargar'.\n", accion);
        exit(EXIT_ARGS_ERROR);
    }

    // 2. Conexión de Red (Sockets TCP POSIX)
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "Error de red: No se pudo crear el socket.\n");
        exit(EXIT_NET_ERROR);
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(puerto);

    if (inet_pton(AF_INET, direccion, &serv_addr.sin_addr) <= 0) {
        fprintf(stderr, "Error: Dirección IP inválida (%s).\n", direccion);
        close(sockfd);//rnf11
        exit(EXIT_NET_ERROR);
    }

    printf("[RED] Conectando a %s:%d...\n", direccion, puerto);
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        fprintf(stderr, "Error de red: No se pudo conectar al servidor.\n");
        close(sockfd);
        exit(EXIT_NET_ERROR);
    }
    printf("[RED OK] Conexion establecida.\n");

    // 3. Intercambio de Saludo Inicial (SALUDO / BIENVENIDA)
    HeaderProtocolo h_saludo = {htons(PROTOCOL_VERSION), htons(MSG_SALUDO), htonl(0)};
    write(sockfd, &h_saludo, sizeof(h_saludo));

    HeaderProtocolo h_resp;
    if (read(sockfd, &h_resp, sizeof(h_resp)) <= 0 || ntohs(h_resp.tipo) != MSG_BIENVENIDA) {
        fprintf(stderr, "Error: El servidor no respondio con BIENVENIDA.\n");
        close(sockfd);
        exit(EXIT_NET_ERROR);
    }
    printf("[PROTOCOLO OK] Sesion iniciada con exito.\n");

    // ========================================================
    // 4 y 5. EJECUCIÓN DE ACCIONES Y MANEJO DE ERRORES/CIERRE

    if (strcmp(accion, "listar") == 0) {
        HeaderProtocolo h_listar = {htons(PROTOCOL_VERSION), htons(MSG_LISTAR), htonl(0)};
        write(sockfd, &h_listar, sizeof(h_listar));
        printf("[CLIENTE] Solicitud de catalogo enviada. Esperando respuesta...\n");

        HeaderProtocolo h_cat_resp;
        if (read(sockfd, &h_cat_resp, sizeof(h_cat_resp)) > 0) {
            uint16_t tipo = ntohs(h_cat_resp.tipo);
            uint32_t len = ntohl(h_cat_resp.longitud);

            if (tipo == MSG_CATALOGO && len > 0) {
                char *buffer = malloc(len + 1);
                read(sockfd, buffer, len);
                buffer[len] = '\0';
                printf("\n--- CATÁLOGO DE CONTENIDOS ---\n%s\n------------------------------\n", buffer);
                free(buffer);
            } else if (tipo == MSG_ERROR) {
                char err_buf[256] = {0};
                read(sockfd, err_buf, len < sizeof(err_buf) ? len : sizeof(err_buf) - 1);
                fprintf(stderr, "Servidor rechazo la solicitud: %s\n", err_buf);
                close(sockfd);
                exit(EXIT_CONTENT_ERROR);
            } else {
                printf("[CLIENTE] Catalogo recibido vacio o sin elementos.\n");
            }
        }

    } else if (strcmp(accion, "descargar") == 0) {
        uint32_t len_archivo = strlen(archivo);
        HeaderProtocolo h_abrir = {htons(PROTOCOL_VERSION), htons(MSG_ABRIR), htonl(len_archivo)};
        
        write(sockfd, &h_abrir, sizeof(h_abrir));
        write(sockfd, archivo, len_archivo);
        printf("[CLIENTE] Solicitando archivo '%s' al servidor...\n", archivo);

        HeaderProtocolo h_abrir_resp;
        if (read(sockfd, &h_abrir_resp, sizeof(h_abrir_resp)) > 0) {
            uint16_t tipo = ntohs(h_abrir_resp.tipo);
            uint32_t len = ntohl(h_abrir_resp.longitud);

            if (tipo == MSG_ERROR) {
                char err_buf[256] = {0};
                read(sockfd, err_buf, len < sizeof(err_buf) ? len : sizeof(err_buf) - 1);
                fprintf(stderr, "Error del servidor (Contenido no hallado/servible): %s\n", err_buf);
                close(sockfd);
                exit(EXIT_CONTENT_ERROR);
            } else if (tipo == MSG_ACEPTADO) {
                FILE *f_dest = fopen(destino, "wb");
                if (!f_dest) {
                    fprintf(stderr, "Error local: No se pudo crear el archivo de destino en '%s'\n", destino);
                    close(sockfd);
                    exit(EXIT_ARGS_ERROR);
                }

                printf("[CLIENTE] Transferencia aceptada. Guardando en: %s\n", destino);

                int descargando = 1;
                int descarga_exitosa = 0;
                while (descargando) {
                    HeaderProtocolo h_bloque;
                    if (read(sockfd, &h_bloque, sizeof(h_bloque)) <= 0) {
                        fprintf(stderr, "Error de red: Conexion interrumpida a mitad de la transferencia.\n");
                        break;
                    }
                    
                    uint16_t t_bloque = ntohs(h_bloque.tipo);
                    uint32_t l_bloque = ntohl(h_bloque.longitud);

                    if (t_bloque == MSG_DATOS) {
                        char buffer_bloque[1024];
                        uint32_t bytes_restantes = l_bloque;
                        while (bytes_restantes > 0) {
                            int a_leer = bytes_restantes < sizeof(buffer_bloque) ? bytes_restantes : sizeof(buffer_bloque);
                            int leidos = read(sockfd, buffer_bloque, a_leer);
                            if (leidos <= 0) break;
                            fwrite(buffer_bloque, 1, leidos, f_dest);
                            bytes_restantes -= leidos;
                        }
                    } else if (t_bloque == MSG_FIN) {
                        printf("[CLIENTE SUCCESS] Descarga completada con exito.\n");
                        descarga_exitosa = 1;
                        descargando = 0;
                    } else if (t_bloque == MSG_ERROR) {
                        fprintf(stderr, "Error: El servidor aborto la transferencia.\n");
                        descargando = 0;
                    }
                }
                fclose(f_dest);

                // Requisito RF-36 / Paso 5: Si la descarga falló o fue interrumpida, borrar archivo parcial
                if (!descarga_exitosa) {
                    remove(destino);
                    fprintf(stderr, "[LIMPIEZA] Se elimino el archivo parcial incompleto '%s' para evitar confusiones.\n", destino);
                    close(sockfd);
                    exit(EXIT_NET_ERROR);
                }
            }
        }

    } else if (strcmp(accion, "canal") == 0) {
        HeaderProtocolo h_unir = {htons(PROTOCOL_VERSION), htons(MSG_UNIR), htonl(0)};
        write(sockfd, &h_unir, sizeof(h_unir));
        printf("[CLIENTE] Solicitando incorporacion al canal en vivo...\n");

        HeaderProtocolo h_emision;
        if (read(sockfd, &h_emision, sizeof(h_emision)) > 0) {
            if (ntohs(h_emision.tipo) == MSG_EMISION) {
                printf("[CLIENTE CANAL] ¡Incorporado a la linea de tiempo compartida del canal en vivo!\n");
                printf("[CLIENTE CANAL] Reproduciendo transmision en tiempo real (Presione Ctrl+C para salir)...\n");
                
                while (1) {
                    HeaderProtocolo h_stream;
                    if (read(sockfd, &h_stream, sizeof(h_stream)) <= 0) break;
                    if (ntohs(h_stream.tipo) == MSG_DATOS) {
                        uint32_t tam = ntohl(h_stream.longitud);
                        char temp_buf[512];
                        while (tam > 0) {
                            int chunk = tam < sizeof(temp_buf) ? tam : sizeof(temp_buf);
                            int r = read(sockfd, temp_buf, chunk);
                            if (r <= 0) break;
                            tam -= r;
                        }
                    }
                }
            } else if (ntohs(h_emision.tipo) == MSG_ERROR) {
                fprintf(stderr, "El servidor rechazo la incorporacion al canal.\n");
            }
        }
    }

    // Cierre ordenado de la sesión
    HeaderProtocolo h_cerrar = {htons(PROTOCOL_VERSION), htons(MSG_CERRAR), htonl(0)};
    write(sockfd, &h_cerrar, sizeof(h_cerrar));
    
    close(sockfd);
    printf("[RED] Sesion cerrada ordenadamente.\n");
    return EXIT_SUCCESS_CUSTOM;
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <signal.h>
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
#define MSG_FINAL       8   /* "FIN"; MSG_FIN ya existe en <sys/socket.h> */
#define MSG_ERROR       9
#define MSG_CERRAR      10
#define MSG_DESPEDIDA   11
#define MSG_UNIR        12
#define MSG_EMISION     13

// Estructura de la cabecera del protocolo (14 bytes fijos, big-endian, sección 7.2)
typedef struct {
    uint16_t version;   
    uint16_t tipo;      
    uint32_t longitud;  
    uint16_t estado;    // código de estado (7.4), en las respuestas
    uint32_t id;        // identificador de la solicitud
} __attribute__((packed)) HeaderProtocolo;

// TCP es un flujo de bytes: read()/write() pueden transferir menos de lo pedido.
// Devuelven n si se completó, 0 si el servidor cerró la conexión, -1 ante error.
static ssize_t leer_exacto(int fd, void *buf, size_t n) {
    size_t hecho = 0;
    while (hecho < n) {
        ssize_t k = read(fd, (char *)buf + hecho, n - hecho);
        if (k < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (k == 0) return 0;
        hecho += (size_t)k;
    }
    return (ssize_t)n;
}

static ssize_t escribir_exacto(int fd, const void *buf, size_t n) {
    size_t hecho = 0;
    while (hecho < n) {
        ssize_t k = write(fd, (const char *)buf + hecho, n - hecho);
        if (k < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        hecho += (size_t)k;
    }
    return (ssize_t)n;
}

// Lee y descarta la carga útil de un mensaje que no se usa (mantiene el flujo alineado).
static int descartar(int fd, uint32_t n) {
    char tmp[256];
    while (n > 0) {
        size_t t = n < sizeof(tmp) ? n : sizeof(tmp);
        if (leer_exacto(fd, tmp, t) <= 0) return -1;
        n -= (uint32_t)t;
    }
    return 0;
}

// Entero de 64 bits big-endian (tamaño en ACEPTADO, posición en EMISION).
static uint64_t be64(const unsigned char *b) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | b[i];
    return v;
}

// RF-24-bis: el cliente levanta VLC para reproducir lo que envía el servidor.
// ¿Hay un ejecutable "vlc" en el PATH?
static int vlc_disponible(void) {
    const char *path = getenv("PATH");
    if (path == NULL) return 0;
    char ruta[4096];
    while (*path) {
        size_t n = strcspn(path, ":");
        if (n > 0 && n < sizeof(ruta) - 5) {
            memcpy(ruta, path, n);
            memcpy(ruta + n, "/vlc", 5);
            if (access(ruta, X_OK) == 0) return 1;
        }
        path += n;
        if (*path == ':') path++;
    }
    return 0;
}

// Lanza VLC en un proceso aparte.
//  - archivo == NULL: VLC lee el video de una tubería (canal en vivo); en
//    *fd_entrada queda el extremo donde el cliente escribe los bytes.
//  - archivo != NULL: VLC abre ese archivo (bajo demanda) y sigue abierto
//    aunque el cliente termine.
// La consola de VLC se descarta para no mezclarla con los mensajes del cliente.
static pid_t lanzar_vlc(const char *archivo, int *fd_entrada, int sockfd) {
    int tubo[2] = {-1, -1};
    if (archivo == NULL && pipe(tubo) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        if (archivo == NULL) { close(tubo[0]); close(tubo[1]); }
        return -1;
    }
    if (pid == 0) {
        close(sockfd);   // la conexión es del cliente, no del reproductor
        if (archivo == NULL) {
            dup2(tubo[0], STDIN_FILENO);
            close(tubo[0]);
            close(tubo[1]);
        } else {
            setsid();    // independiente de la terminal: no muere con el cliente
        }
        int nulo = open("/dev/null", O_WRONLY);
        if (nulo >= 0) { dup2(nulo, STDOUT_FILENO); dup2(nulo, STDERR_FILENO); close(nulo); }
        execlp("vlc", "vlc", "--play-and-exit", archivo ? archivo : "-", (char *)NULL);
        _exit(127);
    }
    if (archivo == NULL) {
        close(tubo[0]);
        *fd_entrada = tubo[1];
    }
    return pid;
}

// Función de ayuda (RF-26)RF-66
void mostrar_ayuda(const char *nombre_programa) {
    printf("Uso: %s -h <direccion> -p <puerto> -a <accion> [-f <archivo>] [-d <destino>] [-n]\n", nombre_programa);
    printf("\nAcciones disponibles:\n");
    printf("  canal     : Conectarse al canal en vivo.\n");
    printf("  listar    : Consultar el catálogo de contenidos.\n");
    printf("  descargar : Solicitar un contenido bajo demanda.\n");
    printf("\nReproduccion (RF-24-bis): con 'canal' el video se envia a VLC mientras llega;\n");
    printf("con 'descargar' se abre en VLC al terminar la descarga.\n");
    printf("  -n        : No abrir VLC (solo recibir / guardar).\n");
    printf("\nEjemplos:\n");
    printf("  %s -h 127.0.0.1 -p 8080 -a listar\n", nombre_programa);
    printf("  %s -h 127.0.0.1 -p 8080 -a descargar -f video1.mp4 -d ./video_recibido.mp4\n", nombre_programa);
    printf("  %s -h 127.0.0.1 -p 8080 -a canal\n", nombre_programa);
}

int main(int argc, char *argv[]) {
    char *direccion = NULL;
    int puerto = -1;
    char *accion = NULL;
    char *archivo = NULL;
    char *destino = NULL;
    int reproducir = 1;   // -n lo desactiva

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
        } else if (strcmp(argv[i], "-n") == 0) {
            reproducir = 0;
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
    HeaderProtocolo h_saludo = {htons(PROTOCOL_VERSION), htons(MSG_SALUDO), htonl(0), 0, 0};
    escribir_exacto(sockfd, &h_saludo, sizeof(h_saludo));

    HeaderProtocolo h_resp;
    if (leer_exacto(sockfd, &h_resp, sizeof(h_resp)) <= 0 || ntohs(h_resp.tipo) != MSG_BIENVENIDA) {
        if (ntohs(h_resp.tipo) == MSG_ERROR) {   // p. ej. servidor lleno o versión distinta
            uint32_t len = ntohl(h_resp.longitud);
            char err_buf[256] = {0};
            leer_exacto(sockfd, err_buf, len < sizeof(err_buf) ? len : sizeof(err_buf) - 1);
            fprintf(stderr, "Error: El servidor rechazo la conexion: %s\n", err_buf);
        } else {
            fprintf(stderr, "Error: El servidor no respondio con BIENVENIDA.\n");
        }
        close(sockfd);
        exit(EXIT_NET_ERROR);
    }
    descartar(sockfd, ntohl(h_resp.longitud));   // servicios anunciados
    printf("[PROTOCOLO OK] Sesion iniciada con exito.\n");

    // ========================================================
    // 4 y 5. EJECUCIÓN DE ACCIONES Y MANEJO DE ERRORES/CIERRE

    if (strcmp(accion, "listar") == 0) {
        HeaderProtocolo h_listar = {htons(PROTOCOL_VERSION), htons(MSG_LISTAR), htonl(0), 0, 0};
        escribir_exacto(sockfd, &h_listar, sizeof(h_listar));
        printf("[CLIENTE] Solicitud de catalogo enviada. Esperando respuesta...\n");

        HeaderProtocolo h_cat_resp;
        if (leer_exacto(sockfd, &h_cat_resp, sizeof(h_cat_resp)) > 0) {
            uint16_t tipo = ntohs(h_cat_resp.tipo);
            uint32_t len = ntohl(h_cat_resp.longitud);

            if (tipo == MSG_CATALOGO && len > 0) {
                char *buffer = malloc(len + 1);
                leer_exacto(sockfd, buffer, len);
                buffer[len] = '\0';
                printf("\n--- CATÁLOGO DE CONTENIDOS ---\n%s\n------------------------------\n", buffer);
                free(buffer);
            } else if (tipo == MSG_ERROR) {
                char err_buf[256] = {0};
                leer_exacto(sockfd, err_buf, len < sizeof(err_buf) ? len : sizeof(err_buf) - 1);
                fprintf(stderr, "Servidor rechazo la solicitud: %s\n", err_buf);
                close(sockfd);
                exit(EXIT_CONTENT_ERROR);
            } else {
                printf("[CLIENTE] Catalogo recibido vacio o sin elementos.\n");
            }
        }

    } else if (strcmp(accion, "descargar") == 0) {
        uint32_t len_archivo = strlen(archivo);
        HeaderProtocolo h_abrir = {htons(PROTOCOL_VERSION), htons(MSG_ABRIR), htonl(len_archivo), 0, 0};
        
        escribir_exacto(sockfd, &h_abrir, sizeof(h_abrir));
        escribir_exacto(sockfd, archivo, len_archivo);
        printf("[CLIENTE] Solicitando archivo '%s' al servidor...\n", archivo);

        HeaderProtocolo h_abrir_resp;
        if (leer_exacto(sockfd, &h_abrir_resp, sizeof(h_abrir_resp)) > 0) {
            uint16_t tipo = ntohs(h_abrir_resp.tipo);
            uint32_t len = ntohl(h_abrir_resp.longitud);

            if (tipo == MSG_ERROR) {
                char err_buf[256] = {0};
                leer_exacto(sockfd, err_buf, len < sizeof(err_buf) ? len : sizeof(err_buf) - 1);
                fprintf(stderr, "Error del servidor (Contenido no hallado/servible): %s\n", err_buf);
                close(sockfd);
                exit(EXIT_CONTENT_ERROR);
            } else if (tipo == MSG_ACEPTADO) {
                unsigned char tam_be[8] = {0};
                if (len != sizeof(tam_be) || leer_exacto(sockfd, tam_be, sizeof(tam_be)) <= 0) {
                    fprintf(stderr, "Error: Respuesta ACEPTADO malformada.\n");
                    close(sockfd);
                    exit(EXIT_NET_ERROR);
                }
                uint64_t tam_anunciado = be64(tam_be);
                uint64_t tam_recibido = 0;
                FILE *f_dest = fopen(destino, "wb");
                if (!f_dest) {
                    fprintf(stderr, "Error local: No se pudo crear el archivo de destino en '%s'\n", destino);
                    close(sockfd);
                    exit(EXIT_ARGS_ERROR);
                }

                printf("[CLIENTE] Transferencia aceptada (%llu bytes). Guardando en: %s\n",
                       (unsigned long long)tam_anunciado, destino);

                int descargando = 1;
                int descarga_exitosa = 0;
                while (descargando) {
                    HeaderProtocolo h_bloque;
                    if (leer_exacto(sockfd, &h_bloque, sizeof(h_bloque)) <= 0) {
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
                            tam_recibido += (uint64_t)leidos;
                        }
                    } else if (t_bloque == MSG_FINAL) {
                        if (tam_recibido == tam_anunciado) {   // RF-37: integridad
                            printf("[CLIENTE SUCCESS] Descarga completada con exito (%llu bytes).\n",
                                   (unsigned long long)tam_recibido);
                            descarga_exitosa = 1;
                        } else {
                            fprintf(stderr, "Error: Se recibieron %llu bytes pero el servidor anuncio %llu.\n",
                                    (unsigned long long)tam_recibido, (unsigned long long)tam_anunciado);
                        }
                        descargando = 0;
                    } else if (t_bloque == MSG_ERROR) {
                        fprintf(stderr, "Error: El servidor aborto la transferencia.\n");
                        descargando = 0;
                    } else {   // p. ej. DESPEDIDA "Servidor detenido"
                        descartar(sockfd, l_bloque);
                        if (t_bloque == MSG_DESPEDIDA)
                            fprintf(stderr, "Error: El servidor se detuvo durante la transferencia.\n");
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

                // RF-24-bis: reproducir el contenido descargado
                if (reproducir && vlc_disponible()) {
                    if (lanzar_vlc(destino, NULL, sockfd) > 0)
                        printf("[CLIENTE] Abriendo '%s' en VLC...\n", destino);
                    else
                        fprintf(stderr, "[CLIENTE] No se pudo abrir VLC; el video quedo en '%s'.\n", destino);
                } else if (reproducir) {
                    fprintf(stderr, "[CLIENTE] VLC no esta instalado; el video quedo en '%s'.\n", destino);
                }
            }
        }

    } else if (strcmp(accion, "canal") == 0) {
        HeaderProtocolo h_unir = {htons(PROTOCOL_VERSION), htons(MSG_UNIR), htonl(0), 0, 0};
        escribir_exacto(sockfd, &h_unir, sizeof(h_unir));
        printf("[CLIENTE] Solicitando incorporacion al canal en vivo...\n");

        HeaderProtocolo h_emision;
        if (leer_exacto(sockfd, &h_emision, sizeof(h_emision)) > 0) {
            if (ntohs(h_emision.tipo) == MSG_EMISION) {
                unsigned char meta[16] = {0};
                if (ntohl(h_emision.longitud) != sizeof(meta) || leer_exacto(sockfd, meta, sizeof(meta)) <= 0) {
                    fprintf(stderr, "Error: Respuesta EMISION malformada.\n");
                    close(sockfd);
                    exit(EXIT_NET_ERROR);
                }
                printf("[CLIENTE CANAL] ¡Incorporado a la linea de tiempo compartida del canal en vivo!\n");
                printf("[CLIENTE CANAL] Posicion de incorporacion: %llu ms de un ciclo de %llu ms\n",
                       (unsigned long long)be64(meta), (unsigned long long)be64(meta + 8));
                // RF-24-bis: el canal se envía a VLC por una tubería mientras llega
                int fd_vlc = -1;
                if (reproducir && vlc_disponible()) {
                    signal(SIGPIPE, SIG_IGN);   // si se cierra VLC, write() falla en vez de matar al cliente
                    if (lanzar_vlc(NULL, &fd_vlc, sockfd) > 0)
                        printf("[CLIENTE CANAL] Reproduciendo en VLC (cierre VLC o presione Ctrl+C para salir)...\n");
                    else
                        fprintf(stderr, "[CLIENTE CANAL] No se pudo abrir VLC; se recibe el canal sin reproducirlo.\n");
                } else {
                    if (reproducir)
                        fprintf(stderr, "[CLIENTE CANAL] VLC no esta instalado; se recibe el canal sin reproducirlo.\n");
                    printf("[CLIENTE CANAL] Recibiendo transmision en tiempo real (Presione Ctrl+C para salir)...\n");
                }

                int en_canal = 1;
                while (en_canal) {
                    HeaderProtocolo h_stream;
                    if (leer_exacto(sockfd, &h_stream, sizeof(h_stream)) <= 0) break;
                    if (ntohs(h_stream.tipo) == MSG_DATOS) {
                        uint32_t tam = ntohl(h_stream.longitud);
                        char temp_buf[512];
                        while (tam > 0) {
                            int chunk = tam < sizeof(temp_buf) ? tam : sizeof(temp_buf);
                            int r = read(sockfd, temp_buf, chunk);
                            if (r <= 0) break;
                            tam -= r;
                            if (fd_vlc >= 0 && escribir_exacto(fd_vlc, temp_buf, (size_t)r) < 0) {
                                printf("[CLIENTE CANAL] Se cerro VLC; saliendo del canal.\n");
                                en_canal = 0;
                                break;
                            }
                        }
                    } else {   // DESPEDIDA (servidor detenido) o ERROR: fin del canal
                        descartar(sockfd, ntohl(h_stream.longitud));
                        if (ntohs(h_stream.tipo) == MSG_DESPEDIDA)
                            printf("[CLIENTE CANAL] El servidor se detuvo.\n");
                        else
                            fprintf(stderr, "[CLIENTE CANAL] El servidor interrumpio el canal.\n");
                        break;
                    }
                }
                if (fd_vlc >= 0)
                    close(fd_vlc);   // fin del flujo: VLC termina lo que tiene y se cierra
            } else if (ntohs(h_emision.tipo) == MSG_ERROR) {
                fprintf(stderr, "El servidor rechazo la incorporacion al canal.\n");
            }
        }
    }

    // Cierre ordenado de la sesión
    HeaderProtocolo h_cerrar = {htons(PROTOCOL_VERSION), htons(MSG_CERRAR), htonl(0), 0, 0};
    escribir_exacto(sockfd, &h_cerrar, sizeof(h_cerrar));
    
    close(sockfd);
    printf("[RED] Sesion cerrada ordenadamente.\n");
    return EXIT_SUCCESS_CUSTOM;
}
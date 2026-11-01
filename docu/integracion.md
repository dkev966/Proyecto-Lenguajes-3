# Integración: núcleo del servidor, modos de concurrencia y pruebas

Este documento explica cómo se unió la parte de la Persona 3 (núcleo del servidor, modos de concurrencia y pruebas) con el código que ya estaba en el repositorio. La regla fue **no modificar ningún archivo existente salvo que fuera absolutamente necesario**.

## 1. Qué se modificó del código existente

**Solo `client/cliente.c`**, con autorización previa. El diff completo está en `docu/cambios_cliente.diff`.

Ningún otro archivo cambió: `server/catalogo.c`, `server/catalogo.h`, `server/sesiones.c`, `server/sesiones.h`, `server/test_servicios.c`, `server/makefile`, `tests/servidor_mock.c`, `README.md`, `.gitignore` y `videos/.duraciones.txt` están igual que antes. `server/makefile` sigue compilando `test_servicios` como antes.

### Por qué hubo que tocar el cliente

| Problema | Consecuencia | Cambio |
|---|---|---|
| El header era de 8 bytes: versión, tipo y longitud. | La sección 7.2 exige también el código de estado y el identificador. El servidor no podía cumplir la especificación y, al mismo tiempo, entenderse con el cliente. | Se agregaron `estado` e `id` al `struct`, que pasa a medir 14 bytes. |
| No leía la carga útil de BIENVENIDA, ACEPTADO ni EMISION. | Esos bytes se leían como si fueran el siguiente header y la sesión se desalineaba, por ejemplo en toda descarga. | Ahora se leen: el tamaño anunciado en ACEPTADO y la posición de incorporación en EMISION, que el cliente muestra. |
| Usaba `read()` y `write()` sin repetir la operación. | TCP puede entregar medio header en una lectura (RNF-15). | Se agregaron `leer_exacto()` y `escribir_exacto()`. |
| No comparaba los bytes recibidos con los anunciados. | Incumplía RF-37. | Al recibir FIN, el cliente compara y avisa si hay discrepancia. |
| No contemplaba DESPEDIDA ni ERROR dentro del canal o de una descarga. | Si el servidor se detenía, el flujo se desalineaba. | Ahora informa "El servidor se detuvo" y termina. |
| No mostraba el motivo de un rechazo al saludar, por ejemplo cuando el servidor está lleno. | El rechazo era invisible para el usuario (RF-72). | Muestra el mensaje del ERROR. |
| `#define MSG_FIN` chocaba con `<sys/socket.h>`, y además había 10 avisos de valores de retorno ignorados. | Incumplía CA-01 (cero avisos). | Se renombró a `MSG_FINAL`. Los avisos desaparecen con las funciones exactas. |
| No abría ningún reproductor: el canal se descartaba y la descarga solo se guardaba. | Incumplía **RF-24-bis** ("el cliente deberá levantar algún programa de video que reproduzca lo que el servidor está enviando"). | Con `canal`, el cliente lanza VLC (`fork` + `exec`) y le escribe el video por una tubería mientras llega; al cerrar VLC, el cliente sale del canal. Con `descargar`, abre el archivo en VLC al terminar. Si VLC no está instalado, avisa y sigue como antes. La opción nueva `-n` desactiva el reproductor (la usan las pruebas automáticas). |

Se conservaron los argumentos, los mensajes, los códigos de salida y el flujo del cliente; solo se agregó la opción `-n`.

## 2. Qué se agregó (todo son archivos nuevos)

| Ubicación | Archivos |
|---|---|
| `Makefile` (raíz) | Construcción de `bin/servidor` y `bin/cliente` con una sola orden; variantes debug, asan, tsan y verificar; objetivos de prueba |
| `server/` | `main.c` (consola del servidor), `servidor.c/.h`, `fifo.c`, `forked.c`, `threaded.c`, `atencion.c/.h`, `senales.c/.h`, `estado.c/.h`, `registro.c/.h` |
| `cod_comun/` | `protocolo.h`, `red.c/.h`, `codigos_salida.h` |
| `tests/` | `bateria.py`, `argumentos.py`, `canal_multiple.py`, `rendimiento.py`, `resultados/`, `.gitignore` |
| `docu/` | `reporte_pruebas.md`, `decisiones_concurrencia.md`, `integracion.md`, `uso.md`, `cambios_cliente.diff` |

`bin/` y `build/` se generan al compilar. El `Makefile` deja un `.gitignore` dentro de `bin/` porque el `.gitignore` de la raíz no ignora ejecutables sin extensión, y no se quiso modificar.

## 3. Cómo se usa la capa de servicio sin modificarla

| Función de `catalogo.h` / `sesiones.h` | Quién la usa y para qué |
|---|---|
| `catalogo_escanear()` (dos veces) | `main.c`: catálogo bajo demanda (`--demanda`) y catálogo del canal (`--canal`) |
| `catalogo_cargar_duraciones()` y `catalogo_duraciones()` | `main.c`: duraciones del canal; con ellas se calcula T |
| `catalogo_cantidad()` y `catalogo_obtener()` | `atencion.c`: LISTAR envía solo nombre y tamaño, nunca la ruta |
| `sesion_abrir_demanda()`, `sesion_leer_bloque()` y `sesion_cerrar()` | `atencion.c`: ABRIR |
| `canal_calcular_punto()` y `sesion_abrir_canal()` | `atencion.c`: UNIR, con `transcurrido = reloj_monotonico_ms() − t0` |
| `reloj_monotonico_ms()` | `main.c` fija t0 al arrancar; `atencion.c` calcula el tiempo transcurrido |

## 4. Observaciones sobre el código existente

No se modificó nada de esto. Lo que afectaba el funcionamiento se resolvió desde los archivos nuevos; lo demás queda como sugerencia para el equipo.

| Archivo | Observación | Estado |
|---|---|---|
| `sesiones.c` | En el canal, `sesion_leer_bloque()` no controla el ritmo y entrega el contenido tan rápido como se le pida. | **Resuelto** en `atencion.c`, que impone el ritmo de reproducción (ver `decisiones_concurrencia.md`). |
| `catalogo.c` | `catalogo_cargar_duraciones()` abre `../videos/.duraciones.txt`, una ruta relativa al directorio de trabajo, y no comprueba si `fopen` devolvió NULL. El comentario en `catalogo.h` dice que recibe el archivo como parámetro, pero la firma no lo tiene. | **Mitigado**: `main.c` verifica el archivo antes de llamarla y explica el problema. El servidor debe ejecutarse desde un directorio hermano de `videos/`, como `bin/`. *Sugerencia: recibir la ruta como parámetro y comprobar `fopen`.* |
| `catalogo.h` | `LARGO_NOMBRE` (50) limita también la **ruta** "directorio/nombre". Los archivos que no caben quedan fuera del catálogo sin aviso. | **Mitigado**: `main.c` informa cada exclusión (RF-49). Hay que usar rutas relativas cortas, como `../videos/demanda`. |
| `catalogo.h` | Define `TAM_BLOQUE` (4096). | Sin conflicto: el protocolo usa `TAM_BLOQUE_DATOS` (64 KB). |
| `server/'` | Archivo con un nombre accidental (una comilla), con una versión vieja e incompleta de `sesiones.c`. | *Sugerencia: borrarlo.* No se compila. |
| `server/.duraciones.txt` y `video_descargado.txt` | Archivos sobrantes de pruebas. | *Sugerencia: borrarlos.* |
| `tests/servidor_mock.c` | Usa el header viejo de 8 bytes. | Ya no corresponde al protocolo. *Sugerencia: borrarlo*; las pruebas reales están en `tests/*.py`. |
| `README.md` | Está en UTF-16, así que GitHub y muchos editores lo muestran con caracteres raros. | *Sugerencia: guardarlo en UTF-8* y enlazar `docu/uso.md`. |
| `client/cliente.c` | Si se cancela con Ctrl+C a mitad de una descarga, queda el archivo parcial con su nombre final (RF-36). No tiene códigos de salida propios para "capacidad" ni para "interno" (RF-68). No implementa el parámetro opcional "tiempo de canal". | *Pendiente para quien hizo el cliente.* No se tocó porque no era necesario para integrar. |
| `videos/live/` | Para que VLC pueda reproducir el canal cuando el cliente se une a la mitad de un archivo, los videos del canal deben estar en MPEG-TS (`.ts`) con los parámetros de video repetidos en cada fotograma clave. Un MP4 no se puede decodificar empezando a la mitad. | Ver la conversión en `docu/uso.md`. |
| `videos/` | Solo contiene `.duraciones.txt` (49 000 ms); faltan las carpetas `demanda/` y `live/` con los videos. | Para la demostración hay que agregar los videos (ver `docu/uso.md`). |

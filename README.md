# Uso: construcción y ejecución

## Requisitos

Linux, `gcc` (o `clang`), `make` y, para las pruebas, `python3` sin bibliotecas extra.

## Construcción

```bash
make            # bin/servidor y bin/cliente (producción)
make verificar  # lo mismo, desde cero, con los avisos tratados como errores
make debug      # bin/debug/   — con símbolos de depuración
make asan       # bin/asan/    — AddressSanitizer + UBSan
make tsan       # bin/tsan/    — ThreadSanitizer
make clean
```

## Preparar los videos

```
videos/
├── .duraciones.txt     una duración en ms por archivo de live/, en orden alfabético
├── demanda/            contenido bajo demanda (cada archivo es una entrada del catálogo)
└── live/               contenido del canal en vivo (se emite en orden alfabético, en ciclo)
```

Para obtener la duración de un video en milisegundos, con ffmpeg instalado:

```bash
ffprobe -v error -show_entries format=duration -of csv=p=0 videos/live/archivo.mp4 \
  | awk '{printf "%d\n", $1*1000}'
```

## Servidor

`catalogo.c` lee las duraciones de `../videos/.duraciones.txt`, una ruta **relativa al directorio de trabajo**. Por eso el servidor se ejecuta desde `bin/` (o desde cualquier otro directorio hermano de `videos/`), y con rutas relativas cortas.

```bash
cd bin
./servidor --modo FIFO     --puerto 8080 --demanda ../videos/demanda --canal ../videos/live
./servidor --modo FORKED   --puerto 8080 --demanda ../videos/demanda --canal ../videos/live --max-clientes 50
./servidor --modo THREADED --puerto 8080 --demanda ../videos/demanda --canal ../videos/live --inactividad 60 --verboso
./servidor --ayuda
```

| Opción | Valor | Por omisión |
|---|---|---|
| `-m`, `--modo` | `FIFO` \| `FORKED` \| `THREADED` | obligatoria |
| `-p`, `--puerto` | 1–65535 | obligatoria |
| `-d`, `--demanda` | directorio del contenido bajo demanda | obligatoria |
| `-c`, `--canal` | directorio del contenido del canal | obligatoria |
| `-a`, `--direccion` | IPv4 o IPv6 de escucha | `0.0.0.0` |
| `-n`, `--max-clientes` | ≥ 1 | 32 |
| `-q`, `--cola` | profundidad de la cola de admisión | 16 |
| `-t`, `--inactividad` | segundos; 0 = sin límite | 30 |
| `-r`, `--registro` | archivo de registro | consola |
| `-v`, `--verboso` | más detalle en el registro | — |
| `-h`, `--ayuda` | muestra la ayuda | — |

Se detiene con **Ctrl+C** o con `kill -TERM`: cierra las sesiones de forma ordenada y avisa a los clientes con DESPEDIDA "Servidor detenido".

**Códigos de salida:** 0 = éxito o detención ordenada · 2 = argumentos o configuración inválidos · 3 = error de red (puerto ocupado) · 6 = error interno.

## Cliente

```bash
./cliente -h 127.0.0.1 -p 8080 -a listar
./cliente -h 127.0.0.1 -p 8080 -a descargar -f pelicula.mp4 -d ./pelicula.mp4   # al terminar, abre VLC
./cliente -h 127.0.0.1 -p 8080 -a canal          # abre VLC; se sale cerrando VLC o con Ctrl+C
./cliente -h 127.0.0.1 -p 8080 -a canal -n       # sin abrir VLC
./cliente --help
```

**Reproducción (RF-24-bis).** El cliente abre VLC por su cuenta: con `canal`, le pasa el video mientras llega; con `descargar`, abre el archivo al terminar. Requiere VLC instalado (en Arch, `sudo pacman -S vlc`); si no está, el cliente avisa y sigue sin reproducir.

**Videos del canal.** El cliente se une a la mitad de un archivo, y un MP4 no se puede reproducir desde la mitad. Los videos de `videos/live/` deben estar en MPEG-TS, con un fotograma clave por segundo y los parámetros del video repetidos:

```bash
ffmpeg -i mi_video.mp4 -c:v libx264 -preset veryfast -g 25 -keyint_min 25 \
  -x264-params repeat-headers=1 -c:a aac -f mpegts videos/live/01_mi_video.ts
```

(`-g 25` para 25 fps; para 30 fps, `-g 30 -keyint_min 30`.) Después hay que regenerar `videos/.duraciones.txt`.

**Códigos de salida:** 0 = éxito · 1 = argumentos · 2 = red · 3 = contenido rechazado.

## Demostración: los tres modos con canal y bajo demanda a la vez (entregable 7)

```bash
# terminal 1
cd bin && ./servidor --modo THREADED --puerto 8080 --demanda ../videos/demanda --canal ../videos/live
# terminal 2: un cliente en el canal
./bin/cliente -h 127.0.0.1 -p 8080 -a canal
# terminal 3: mientras tanto, un cliente bajo demanda
./bin/cliente -h 127.0.0.1 -p 8080 -a listar
./bin/cliente -h 127.0.0.1 -p 8080 -a descargar -f <nombre> -d /tmp/copia
```

En FORKED y THREADED, la terminal 3 se atiende de inmediato. En FIFO queda esperando hasta que el cliente del canal termina; ese es el comportamiento esperado (RF-51), y el servidor muestra por consola las conexiones en espera.

## Pruebas

```bash
make pruebas                      # batería en los tres modos
python3 tests/argumentos.py       # argumentos del servidor y del cliente
python3 tests/canal_multiple.py   # canal de varios archivos, byte a byte
make rendimiento                  # comparación de rendimiento
make pruebas-tsan                 # carreras de datos (ThreadSanitizer)
make pruebas-asan                 # memoria y fugas (AddressSanitizer/LeakSanitizer)
```

Los resultados quedan en `tests/resultados/`. El análisis está en `docu/reporte_pruebas.md`.

# Reporte de pruebas: sistema integrado y comparación entre modos

Servidor de video en C para Linux. Este es el entregable 6 de la sección 17.1, y cubre RNF-29 (reproducibilidad), RNF-30 (comparación entre modos) y CA-15 (evidencia de pruebas).

**Qué se probó.** El sistema completo e integrado:

- la capa de servicio (`server/catalogo.c`, `server/sesiones.c`), sin modificar;
- la capa de concurrencia y red (`server/fifo.c`, `forked.c`, `threaded.c`, `atencion.c`, `servidor.c`, `senales.c`, `estado.c`, `registro.c`, `cod_comun/red.c`);
- la interfaz de consola del servidor (`server/main.c`);
- el cliente en C (`client/cliente.c`).

## 1. Resumen

| Conjunto de pruebas | FIFO | FORKED | THREADED |
|---|---|---|---|
| Batería funcional `tests/bateria.py` (31 pruebas) | 25 pasan · 6 no aplican | 29 pasan · 2 no aplican | 28 pasan · 3 no aplican |
| Canal de 3 archivos, contenido verificado byte a byte (`tests/canal_multiple.py`) | 3/3 uniones | 3/3 | 3/3 |
| ThreadSanitizer (batería completa más estrés con 64 clientes) | 0 avisos | 0 avisos | 0 avisos |
| AddressSanitizer, LeakSanitizer y UBSan (batería completa; servidor y cliente instrumentados) | 0 errores | 0 errores | 0 errores |

**Ninguna prueba falló.** "No aplica" indica que la prueba verifica una propiedad exclusiva de otro modo. Por ejemplo, la sincronía entre dos clientes *simultáneos* no existe en FIFO, y el `kill -9` a un proceso hijo solo tiene sentido en FORKED.

Además:

- **Pruebas de argumentos** (`tests/argumentos.py`): 19 casos del servidor y 7 del cliente, todos correctos.
- **Compilación** (`make verificar`, CA-01): servidor y cliente compilan en producción con **0 avisos**, tanto con `gcc -Wall -Wextra -Werror` como con `clang -Werror`.

## 2. Entorno

| Elemento | Valor |
|---|---|
| Sistema | Linux 6.18 (x86-64), 2 CPU Intel Xeon a 2,1 GHz |
| Compilador | gcc 13.3.0 (también verificado con clang) |
| Norma y banderas | `-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes -Wformat=2` |
| Generador de carga | Python 3.13, sin bibliotecas externas |
| Red | Interfaz de bucle local (127.0.0.1) |

Las conclusiones se basan en las **diferencias entre modos** medidas en la misma máquina. Para las computadoras del LAIIMI basta repetir las órdenes de la sección 3.

## 3. Procedimiento para reproducirlo (RNF-29)

```bash
make                              # bin/servidor y bin/cliente
make verificar                    # CA-01: compilación de producción sin avisos
make pruebas                      # batería en los tres modos      (unos 3 min)
python3 tests/argumentos.py       # pruebas de argumentos           (unos 5 s)
python3 tests/canal_multiple.py   # canal de 3 archivos              (unos 1 min)
make rendimiento                  # comparación de rendimiento     (unos 2 min)
make pruebas-tsan                 # batería contra ThreadSanitizer
make pruebas-asan                 # batería contra AddressSanitizer/LeakSanitizer
```

Los scripts generan sus datos de forma determinista en `tests/datos/`, con la misma estructura que `videos/`:

| Archivo | Tamaño | Uso |
|---|---|---|
| `videos/demanda/peli_chica.bin` | 100 KB | transferencias rápidas |
| `videos/demanda/peli_mediana.bin` | 5 MB | concurrencia y rendimiento |
| `videos/demanda/peli_grande.bin` | 30 MB | integridad byte a byte y cortes a mitad de transferencia |
| `videos/live/canal.bin` y `videos/.duraciones.txt` (4000 ms) | 400 KB | canal en vivo a 100 000 B/s |
| `videos/secreto.bin` | 20 B | existe en disco pero **fuera** del catálogo (CA-07) |

El servidor se ejecuta desde `tests/datos/srv/`, porque `catalogo.c` siempre lee `../videos/.duraciones.txt`. Durante la batería se configura con un máximo de 40 clientes (3 en la prueba de capacidad), inactividad de 3 s y cola de 8.

Los resultados quedan en `tests/resultados/`:

- `reporte-*.md`: la matriz con la evidencia de cada prueba;
- `resultados-*.json`: los mismos datos en formato estructurado;
- `servidor_<MODO>.log`: el registro del servidor, que sirve de evidencia para RNF-22;
- `argumentos-*.md`, `canal_multiple-*.txt`, `rendimiento-*.json` y `sanitizers/`.

## 4. Resultados de la batería funcional

### 4.1 Matriz comparativa

| Prueba | Requisitos | FIFO | FORKED | THREADED |
|---|---|---|---|---|
| T01 Saludo y bienvenida | 7.1, 7.5 | PASA | PASA | PASA |
| T02 Catálogo sin rutas internas | RF-11, RF-47 | PASA | PASA | PASA |
| T03 Bajo demanda íntegro, byte a byte (30 MB) | RF-08, RF-09, RF-32, CA-06 | PASA | PASA | PASA |
| T04 Nombre fuera del catálogo que sí existe en disco | RF-10, RF-46, CA-07 | PASA | PASA | PASA |
| T05 Siete variantes de ruta maliciosa | RF-76, RF-77 | PASA | PASA | PASA |
| T06 Versión de protocolo incompatible | 7.1, 7.4 | PASA | PASA | PASA |
| T07 Tipo desconocido; la sesión sigue utilizable | RF-12, RF-13 | PASA | PASA | PASA |
| T08 Longitud declarada abusiva (2³¹ − 1) | RNF-15, RF-77 | PASA | PASA | PASA |
| T09 Mensaje truncado: cierre por inactividad | 12.1, RNF-15 | PASA | PASA | PASA |
| T10 Canal: SALIR y luego CERRAR | RF-05, 7.3.2 | PASA | PASA | PASA |
| T11 Canal: el cliente no puede elegir posición | RF-05 | PASA | PASA | PASA |
| T12 Incorporación tardía: desfase coherente | RF-03, RNF-03, CA-03 | PASA | PASA | PASA |
| T13 Cruce de dos o más fronteras de ciclo, al ritmo de reproducción (±5 %) | RF-06, CA-05 | PASA | PASA | PASA |
| T14 Sincronía entre clientes simultáneos | RNF-04, CA-04 | N/A | PASA | PASA |
| T15 El canal activo no bloquea a otros clientes | CA-10, CA-11 | N/A | PASA | PASA |
| T16 La conexión siguiente espera y se atiende después | RF-17, RF-50, CA-09 | PASA | N/A | N/A |
| T17 Desconexión abrupta a mitad de transferencia | RF-35, RNF-08, CA-12 | PASA | PASA | PASA |
| T18 Aislamiento: 4 descargas paralelas mientras un cliente muere | RNF-07, CA-12 | N/A | PASA | PASA |
| T19 `kill -9` a un proceso hijo | RF-55, RNF-07 | N/A | PASA | N/A |
| T20 Estrés: 10 clientes mixtos simultáneos | RNF-24 | N/A | PASA | PASA |
| T21 Recursos estables tras 300 ciclos de conexión | RNF-10, RNF-11, RF-74 | PASA | PASA | PASA |
| T22 Detención con SIGINT y sesiones activas | RNF-12, RF-58, CA-13 | PASA | PASA | PASA |
| T23 Límite de clientes: rechazo explícito | RF-20, RNF-28 | N/A | PASA | PASA |
| T24 Cliente que deja de leer: se abandona y el servidor sigue | RF-34, RNF-08 | PASA | PASA | PASA |
| T25 Ctrl+C con un cliente que no lee y sin límite de inactividad | RNF-12, RF-58, RFx-52.4 | PASA | PASA | PASA |
| T26 Puerto ocupado: mensaje claro y código de salida 3 | RF-26, RF-29 | PASA | PASA | PASA |
| T27 Cola de admisión llena; atención en orden de llegada | RFx-50.1, RFx-50.2, CA-09 | PASA | N/A | N/A |
| T28 **Cliente en C**: listar | RF-11, CA-08 | PASA | PASA | PASA |
| T29 **Cliente en C**: descarga íntegra de 30 MB | RF-09, RF-37, CA-06 | PASA | PASA | PASA |
| T30 **Cliente en C**: contenido fuera del catálogo | RF-10, RF-36, CA-07 | PASA | PASA | PASA |
| T31 **Cliente en C**: canal en vivo y salida con Ctrl+C | RF-03, RF-35, CA-08 | PASA | PASA | PASA |

### 4.2 Evidencia por familia de pruebas (sección 15)

| Familia | Qué se hizo | Resultado: FIFO / FORKED / THREADED |
|---|---|---|
| Argumentos | 19 invocaciones inválidas del servidor (modo ausente o repetido, puerto fuera de rango o no numérico, directorios inexistentes, límites en 0 o negativos, opción desconocida, sin `.duraciones.txt`, duraciones que no coinciden con el canal, canal vacío) y 7 del cliente. | Las 26 terminan con el código esperado y un mensaje que nombra el parámetro (`argumentos-*.md`). |
| Modelo temporal | Se unió al canal en 3 puntos distintos de un ciclo de 3 archivos con ritmos de 50, 100 y 300 KB/s. | La posición anunciada coincide con el reloj (±1 ms). El contenido es idéntico byte a byte y el ritmo se desvía +0,01 % a +0,20 % (`canal_multiple-*.txt`). |
| Incorporación tardía | Un cliente se une en p1 y, 2,7 s después, otro en p2. Se compara p2 con (p1 + Δt) mod T. | Desvío de 0 / 0 / 0 ms (tolerancia: 1000 ms). |
| Ciclo | Una sesión de canal se sostiene 9 s, es decir, más de dos ciclos de 4 s. | 100 078 / 100 211 / 100 178 B/s, frente a 100 000 esperados. Sin cortes ni errores. |
| Sincronía | Dos clientes conectados a la vez, con 1,5 s de separación. En el estrés: 5 de canal más 5 bajo demanda. | Dos clientes: — / 0 ms / 0 ms. Estrés: peor desvío — / 1 ms / 1 ms. |
| Bajo demanda | Descarga de 30 MB, desde la batería y desde el cliente en C. Se compara el SHA-256 y el tamaño anunciado. | Idénticos en los tres modos. |
| Catálogo | Se pide `secreto.bin`, que está en disco pero no en el catálogo. Se revisa que LISTAR no muestre rutas. | "No hallado" y la sesión sigue. El cliente termina con código 3 y no deja archivo parcial. |
| Concurrencia por modo | La misma batería en los tres modos. | Resultados equivalentes. Las únicas diferencias son las que impone cada modo (sección 6). |
| Aislamiento | Un cliente corta con RST a mitad de 30 MB mientras otros 4 descargan. En FORKED, además, `kill -9` a un hijo con canal activo. | Las 4 descargas quedan íntegras. El proceso principal sigue vivo y sin zombis. |
| Recursos | 30 conexiones de calentamiento y 300 ciclos mezclados: cierre ordenado, descarga y corte abrupto. | Descriptores 4 → 4. Memoria 2180 → 2180 / 2156 → 2156 / 2824 → 2824 KB. Cero zombis. |
| Límites | Máximo de 3 clientes con un cuarto que intenta entrar. En FIFO, 16 conexiones contra una cola de 8. | Concurrentes: ERROR "Capacidad" y readmisión al liberar un lugar. FIFO: 9 encoladas, 7 descartadas por el sistema operativo, atención en orden de llegada. |
| Robustez | Header truncado, longitud de 2³¹ − 1, tipo desconocido, versión 99 y 7 rutas maliciosas. | Cada caso recibe su código de error o un cierre por inactividad (3,0 s). |
| Terminación | SIGINT con un canal activo; Ctrl+C a todo el grupo con un cliente que no lee y sin límite de inactividad; puerto ocupado. | Salida 0 en ≤ 0,1 s, el cliente recibe DESPEDIDA "Servidor detenido" y no quedan procesos residuales. Puerto ocupado: código 3. |
| Estrés | 10 clientes mixtos (RNF-24). Aparte, 64 simultáneos bajo ThreadSanitizer. | 10/10 correctos; 0 informes de TSan. |

## 5. Verificación de memoria, concurrencia y compilación

| Herramienta | Alcance | Resultado |
|---|---|---|
| `gcc -Werror` y `clang -Werror` | Todo el código: servicio, concurrencia, consola y cliente | 0 avisos |
| ThreadSanitizer | La batería completa en los 3 modos, más el estrés de 64 clientes | 0 carreras de datos, 0 interbloqueos |
| AddressSanitizer + LeakSanitizer + UBSan | La batería completa en los 3 modos, con servidor y cliente instrumentados | 0 errores de memoria, 0 fugas, 0 comportamientos indefinidos |

En los ejecutables instrumentados, T21 registra la memoria pero no la evalúa: los propios sanitizers reservan memoria interna por cada proceso o hilo. Las fugas, en cambio, sí las evalúa LeakSanitizer.

## 6. Rendimiento comparativo

Medido con `tests/rendimiento.py`. Cada escenario se repitió 3 veces y se reporta la mediana. El servidor arrancó con un máximo de 128 clientes y una cola de 128.

| Escenario | FIFO | FORKED | THREADED |
|---|---|---|---|
| **E1** 32 descargas simultáneas de 5 MB, a máxima velocidad: tiempo total | 100 ms | 127 ms | 116 ms |
| E1: espera máxima para ser atendido | **79 ms** | 30 ms | 27 ms |
| **E5** 8 clientes consumiendo a 10 MB/s, como reproductores: tiempo total | **4,01 s** | 0,51 s | 0,51 s |
| E5: peor tiempo hasta el primer byte | **3,51 s** | 4,7 ms | 7,4 ms |
| **E3** un cliente ocupa el canal; espera de otro que solo quiere LISTAR | **2,81 s** | 1,1 ms | 0,8 ms |
| **E2** costo por conexión (300 conexiones cortas seguidas) | 0,16 ms | **0,39 ms** | 0,22 ms |
| **E4** memoria adicional por sesión de canal (PSS) | 8 KB (1 sesión como máximo) | **114 KB** (21 procesos) | 22 KB (1 proceso) |

## 7. Análisis comparativo: diferencias observadas y su causa (RNF-30)

| Diferencia observada | Causa |
|---|---|
| En FIFO, un cliente nuevo espera mientras otro ocupa el canal: 2,8 s en E3, y también se ve en T16 y T27. En los concurrentes la espera es de unos 1 ms (T15). | El único sujeto que atiende en FIFO está ocupado con una sesión que no termina sola (RF-51). |
| Con 8 clientes de ritmo limitado, FIFO tarda 8 veces más (E5), y el último espera 3,5 s su primer byte. | Atención secuencial: los tiempos de servicio se suman. |
| A máxima velocidad, el tiempo total es parecido en los tres modos (E1). | Cada descarga dura unos 3 ms con 2 CPU y el contenido en caché, y el límite lo pone el generador de carga. Aun así, la *espera para empezar* crece de forma lineal en FIFO (79 ms con 32 clientes) y se mantiene en unos 30 ms en los concurrentes. Con una red real, la ventaja de los concurrentes debería crecer. |
| Con la cola llena, FIFO se mantiene estable y el sistema operativo descarta las conexiones excedentes (T27). | Por omisión, Linux descarta el SYN o el ACK final del saludo TCP en lugar de responder con RST (RFx-50.2). |
| FORKED es el modo con mayor costo por conexión (0,39 ms) y el que más memoria usa por sesión (114 KB). THREADED usa 22 KB por sesión. | `fork()` duplica la tabla de páginas, y cada hijo es un proceso con su propio montículo. Un hilo solo agrega su pila y su búfer. |
| Solo FORKED sobrevive a un `kill -9` sobre la unidad que atiende a un cliente (T19). | Cada proceso tiene su propio espacio de direcciones. En THREADED, un fallo grave de un hilo termina todo el proceso (tabla 10.1). |
| Todo lo demás —protocolo, errores, integridad, desfase y ritmo del canal, detención, cliente en C— da los mismos resultados en los tres modos. | Los tres modos llaman a la misma función, `atender_cliente()`. El cálculo del desfase está en la capa de servicio (RF-21, sección 6.2). |

## 8. Defectos encontrados durante las pruebas y la integración

| # | Dónde | Defecto | Resolución |
|---|---|---|---|
| 1 | Capa de concurrencia | Unos 40 ms extra por solicitud, por el algoritmo de Nagle combinado con el ACK diferido. | `TCP_NODELAY` y `MSG_MORE`. Ahora: 0,09 ms. |
| 2 | Capa de concurrencia | Carrera de datos en la bandera de detención, detectada por ThreadSanitizer. | Se cambió a `atomic_int` *lock-free*. |
| 3 | Capa de concurrencia | Uso de memoria ya liberada al apagar THREADED. | Se reordenó el cierre de cada hilo. |
| 4 | Capa de concurrencia | Ctrl+C colgaba el servidor si un cliente no leía. | Escrituras interrumpibles y con plazo. |
| 5 | `sesiones.c` | El canal no tiene control de ritmo: `sesion_leer_bloque()` entrega todo el contenido tan rápido como se le pida. | Se resolvió **sin modificar el archivo**: `atencion.c` solo pide los bytes que ya corresponden según el reloj (T13 y `canal_multiple.py`). |
| 6 | `catalogo.c` | `catalogo_cargar_duraciones()` usa una ruta fija (`../videos/.duraciones.txt`) y no comprueba el resultado de `fopen`. Si el archivo no existe, el programa se cae. | **Sin modificar el archivo**: `main.c` verifica que exista antes de llamarla y explica cómo ejecutar el servidor (prueba de argumentos). |
| 7 | `catalogo.c` | Si la ruta "directorio/nombre" no cabe en 50 bytes, el archivo se excluye del catálogo sin avisar. | **Sin modificar el archivo**: `main.c` informa cada exclusión por consola (RF-49). |
| 8 | `client/cliente.c` (modificado con autorización) | Usaba un header de 8 bytes, ignoraba las cargas de BIENVENIDA, ACEPTADO y EMISION (lo que desalineaba el flujo), hacía lecturas parciales, no verificaba el tamaño recibido y compilaba con 10 avisos. | Se corrigió lo mínimo necesario; el detalle está en `docu/integracion.md`. |
| 9 | `client/cliente.c` (modificado con autorización) | No abría ningún reproductor (RF-24-bis). | Ahora lanza VLC: el canal por una tubería y las descargas al terminar. Se verificó con VLC 3.0.20: reconoce el flujo MPEG-TS, decodifica H.264 y AAC y se cierra solo al terminar el canal. Las pruebas automáticas usan `-n` para no abrir ventanas. |

## 9. Limitaciones y pendientes

1. **CA-03 con 5 minutos.** T12 y `canal_multiple.py` verifican el mismo criterio con separaciones de segundos. Para la demostración formal conviene levantar el servidor con los videos reales y unirse 5 minutos después del arranque.
2. **Red real.** Todas las mediciones se hicieron por loopback; conviene repetir `make rendimiento` en el LAIIMI.
3. **Pendientes del cliente** que no se tocaron porque no eran necesarios para integrar (ver `docu/integracion.md`):
   - si se interrumpe con Ctrl+C a mitad de una descarga, queda un archivo parcial con el nombre final (RF-36);
   - no tiene códigos de salida propios para "capacidad" ni para "interno" (RF-68);
   - no implementa el parámetro opcional "tiempo de canal".

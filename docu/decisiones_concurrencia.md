# Decisiones de diseño: capa de concurrencia

Esta es la parte del entregable 5 (sección 17.1) que explica cómo se implementó cada modo de concurrencia, cómo se comparte el estado y qué alternativas se descartaron. También cubre la documentación que exigen RF-53 y RFx-57.3.

## 1. Separación entre concurrencia y servicio (RF-21, sección 6.2)

```
                 main.c                    consola (11.1), carga de catálogos y duraciones, t0
                    │
            servidor_ejecutar()            servidor.c   socket, señales, estado compartido, cierre
                    │
      ┌─────────────┼──────────────┐
  fifo.c        forked.c       threaded.c              QUIÉN atiende cada conexión
      └─────────────┼──────────────┘
            atender_cliente()              atencion.c   máquina de estados de la sesión (7.5)
                    │                                   y ritmo de tiempo real del canal
       sesiones.c / catalogo.c             capa de servicio: desfase, lectura, catálogo
                    │
        cod_comun/red.c                    E/S exacta, serialización, plazos
```

Todos los archivos están en `server/`, salvo `red.c`, `protocolo.h` y `codigos_salida.h`, que están en `cod_comun/`.

Los tres modos hacen lo mismo: `accept` → *alguien* llama a `atender_cliente(fd, ip, svc, cfg)` → `close`. Aquí `svc` es el contexto `Servicio` (definido en `servidor.h`) que arma `main.c`: los dos catálogos, las duraciones del canal, el largo del ciclo T y el instante t0. Lo único que cambia es quién es ese alguien: el propio proceso principal, un proceso hijo o un hilo. Ninguna regla del protocolo ni del cálculo del desfase vive en `fifo.c`, `forked.c` ni `threaded.c`. Así se mitiga el riesgo de comportamiento divergente entre modos (sección 18.2), y la batería lo confirma: los tres modos dan resultados idénticos en todas las pruebas que no son específicas de un modo.

## 2. Admisión común

- **Socket de escucha no bloqueante con `poll()` de 500 ms antes de cada `accept()`.** Si se bloqueara directamente en `accept()`, una señal que llegue justo antes de la llamada se perdería y el servidor no se detendría hasta la siguiente conexión. Con el plazo, el ciclo revisa la bandera de detención al menos dos veces por segundo.
- **`getaddrinfo` con `AI_PASSIVE`.** Acepta tanto IPv4 como IPv6, según la dirección configurada.
- **`TCP_NODELAY` en cada conexión aceptada.** Sin esta opción, cada respuesta pequeña se retrasaba unos 40 ms por el algoritmo de Nagle y el ACK diferido (defecto 1 del reporte de pruebas).

## 3. Modo FIFO (`fifo.c`)

- El proceso principal hace `accept` → `atender_cliente` → `close`, y solo después vuelve a llamar a `accept`. Las demás conexiones esperan en la cola del sistema operativo, cuya profundidad se fija con el *backlog* de `listen()` (RFx-50.1). Si la cola se llena, el kernel descarta las conexiones excedentes y el servidor no lo trata como un error (RFx-50.2).
- **Monitor de la cola (RFx-17.2).** Mientras atiende una sesión, el proceso no puede mirar la cola. Por eso un hilo auxiliar consulta cada segundo `TCP_INFO.tcpi_unacked` del socket de escucha —en Linux, ese campo es el largo actual de la cola— e imprime el valor cuando cambia.
  - Ese hilo **nunca acepta ni atiende** conexiones: la atención sigue siendo estrictamente secuencial y la realiza un único sujeto.
  - Nace con SIGINT y SIGTERM bloqueadas, de modo que las señales las recibe siempre el hilo principal.
- Las transiciones entre sesiones se registran por consola (RFx-50.3).

## 4. Modo FORKED (`forked.c`)

| Aspecto | Decisión |
|---|---|
| Herencia (RFx-52.1) | El padre cierra el socket del cliente justo después del `fork()`. Por eso el único recurso ajeno que hereda un hijo es el socket de escucha, y lo primero que hace es cerrarlo. El hijo también restaura el manejador original de SIGCHLD y libera la tabla de PIDs, que pertenece al padre. |
| Fin del hijo (RFx-52.2) | El hijo termina con `_exit(0)` al acabar la sesión. Se usa `_exit` y no `exit` para que no se ejecuten los `atexit` heredados ni se vacíen dos veces los búferes de stdio. Además, el padre hace `fflush` antes de cada `fork()`. |
| Recolección (RFx-52.3) | Hay un manejador de SIGCHLD **sin `SA_RESTART`** cuya única función es interrumpir el `poll()` de admisión. La recolección real ocurre en el ciclo principal, con `waitpid(-1, …, WNOHANG)` antes y después de cada `accept`. Así nunca se bloquea la admisión, y la tabla de PIDs no se toca desde un manejador de señal. |
| Límite (RF-20, RF-54) | Se lleva una tabla de PIDs vivos. Si ya hay `max_clientes` activos, la conexión se rechaza con ERROR "Capacidad" **antes** de llamar a `fork()`, así que la creación de procesos nunca supera el límite. |
| Fallo de `fork()` | El cliente recibe ERROR "Interno" y el servidor sigue operando. |
| Detención (RFx-52.4) | Se envía SIGTERM a cada hijo vivo y se espera hasta 5 s mientras se recolecta. Los que sigan vivos reciben SIGKILL y se recolectan. Los hijos que reciben SIGTERM bajan su propia bandera, envían DESPEDIDA "Servidor detenido" y terminan. |
| Aislamiento (RF-55) | Verificado con T19: tras un `kill -9` a un hijo con canal activo, otro cliente termina su descarga íntegra y no quedan zombis. Si el hijo muere sosteniendo un mutex compartido, el siguiente proceso que lo pida lo recupera, porque los mutex son ROBUST (sección 6). |

## 5. Modo THREADED (`threaded.c`)

| Aspecto | Decisión |
|---|---|
| Unidad de atención (RF-56) | Un hilo **desacoplado** (*detached*) por conexión. Libera su pila y sus recursos al terminar, sin que haga falta `pthread_join` (RFx-56.2, RFx-56.3). |
| Argumentos | Cada hilo recibe una estructura `ArgHilo` propia, reservada con `malloc`, que el hilo libera. Así ningún hilo lee variables locales del ciclo de admisión. |
| Límite (RF-20) | Una tabla de ranuras de tamaño `max_clientes`. Reservar una ranura es atómico (se hace bajo `mtx_hilos`); si no queda ninguna libre, la conexión recibe ERROR "Capacidad". |
| Señales (RF-59) | Los hilos se crean con SIGINT y SIGTERM **bloqueadas**, así que esas señales las recibe siempre el hilo principal. Cada hilo revisa la bandera de detención solo en puntos seguros —entre bloques de datos, y cada 200 ms mientras espera leer o escribir—, de modo que ninguna sesión queda interrumpida a mitad de un estado. |
| Detención (RF-58) | El hilo principal espera con `pthread_cond_timedwait` sobre `CLOCK_MONOTONIC`, con un plazo de 5 s, a que el número de hilos activos llegue a 0. Si no llega, aplica `shutdown()` a los sockets que sigan abiertos para despertar a los hilos bloqueados, y espera 1 s más. |
| Orden de cierre de un hilo | El hilo hace primero todo lo que toca estado compartido (contadores y registro) y después libera su ranura. Además, cierra su socket **dentro** de la sección crítica, para que `shutdown()` nunca actúe sobre un número de descriptor que el sistema ya reasignó (defecto 3 del reporte). |
| No se usa `pthread_cancel` | Cancelar un hilo podría dejar un mutex tomado o una sesión a medio escribir, lo que contradice RF-59. |

## 6. Estado del servidor (RF-53, sección 6.3)

| Estado | Dónde vive | Quién lo modifica | Protección |
|---|---|---|---|
| Contadores (admitidas, rechazadas, transferencias, errores, activos) | Región `mmap(MAP_SHARED \| MAP_ANONYMOUS)` creada antes de cualquier `fork()`, así que la ven todos los procesos y todos los hilos | Todos | `mtx_contadores` |
| Exclusión de la salida por consola | La misma región compartida | Todos | `mtx_registro` |
| Bandera de detención `servidor_corriendo` | Variable global, una copia por proceso | El manejador de señal de cada proceso | `atomic_int`, verificado *lock-free* en compilación |
| Configuración (`ConfigServidor`) | Copia heredada, de solo lectura | Nadie después del arranque | No requiere: es inmutable |
| `Servicio`: catálogo bajo demanda, catálogo del canal, duraciones, T y t0 | Copia heredada, de solo lectura. `main.c` la arma **antes** de `servidor_ejecutar()` | Nadie después del arranque | No requiere: es inmutable (`catalogo.h` lo declara de solo lectura después del escaneo) |
| Reloj monótono | Del sistema, el mismo para todos los procesos | — | — |
| Socket de escucha y tabla de PIDs | Exclusivos del proceso principal | El proceso principal | — |
| Tabla de ranuras y número de hilos activos (THREADED) | Exclusivos del proceso | Hilo principal y hilos de atención | `mtx_hilos` |
| Socket del cliente, `Sesion`, `RitmoCanal`, búfer de 64 KB | Exclusivos de cada hijo o hilo | Solo su dueño | — |

¿Por qué el canal no necesita memoria compartida? Su posición es una **función pura del tiempo**: p(t) = (t − t0) mod T. Todos los procesos parten del mismo t0, que heredan del padre, y leen el mismo reloj monótono del sistema. Por eso calculan la misma posición sin comunicarse entre sí. T14 y T20 lo confirman: dos clientes atendidos por procesos distintos se desvían como mucho 1 ms.

### Ritmo de tiempo real del canal

`sesion_leer_bloque()` (de `sesiones.c`) posiciona bien al cliente en el punto que le corresponde, pero después entrega bytes tan rápido como se le pidan. Sin control, el cliente recibiría en segundos el contenido de minutos y se adelantaría a la línea de tiempo compartida.

Como no se podía modificar `sesiones.c`, el ritmo lo impone `atencion.c` mediante un "espejo" de la sesión (`RitmoCanal`):

- Lleva el elemento actual, su tamaño, los bytes ya emitidos y el instante (sin módulo) en que ese elemento empezó.
- Antes de cada lectura calcula cuántos bytes ya deberían haberse emitido: `tamaño_i × (t − inicio_i) / duración_i`. Solo pide esa cantidad, con un tope de 64 KB por mensaje. Si todavía no toca enviar nada, espera 20 ms sin dejar de vigilar el socket del cliente.
- Usa la misma aritmética que `sesion_abrir_canal()` (offset = tamaño × offset_ms / duración, con 128 bits) y cambia de elemento en el mismo momento que `sesion_leer_bloque()`, es decir, al agotar el archivo. Por eso el espejo y la sesión real nunca se separan.
- Calcula siempre desde t0 y nunca por incrementos, así que el error no se acumula (riesgo 1 de la sección 18.2).

`tests/canal_multiple.py` lo verifica con 3 archivos de ritmos distintos: el contenido llega idéntico byte a byte y el ritmo se desvía menos del 0,2 %.

### Traducción de errores de la capa de servicio (7.4)

| `estado_t` (`catalogo.h`) | Código del protocolo |
|---|---|
| `SRV_ERR_NOMBRE_INVALIDO` | Solicitud |
| `SRV_ERR_NO_ENCONTRADO` | No hallado |
| `SRV_ERR_NO_SERVIBLE`, `SRV_ERR_IO`, `SRV_ERR_CATALOGO_VACIO` | No servible |
| `SRV_ERR_ARGUMENTO`, `SRV_ERR_MEMORIA` | Interno (el detalle va solo al registro, RF-73) |

Los mutex de la región compartida son `PTHREAD_PROCESS_SHARED` y `PTHREAD_MUTEX_ROBUST`. Si un hijo muere con `kill -9` mientras sostiene uno, el siguiente que lo pida recibe `EOWNERDEAD`, llama a `pthread_mutex_consistent` y continúa normalmente, en vez de quedar bloqueado para siempre.

## 7. Orden de adquisición de los mutex (RFx-57.3)

**Regla: nunca se sostiene más de un mutex a la vez.**

- `mtx_contadores` y `mtx_registro` son hojas: dentro de sus secciones críticas no se llama a nada que tome otro mutex.
- `mtx_hilos` (THREADED) y el mutex del monitor (FIFO) se liberan **antes** de llamar a `reg_*()` o a `estado_*()`.

Como nadie espera un segundo mutex mientras sostiene el primero, no puede formarse un ciclo de espera y, por lo tanto, no puede haber interbloqueo.

Las secciones críticas solo leen o escriben enteros, o una línea de texto ya armada. **Nunca se transfiere contenido con un mutex tomado** (RFx-57.1).

## 8. Registro (RFx-57.2, RNF-22)

Cada línea se arma completa en memoria con el instante (en milisegundos), el modo, el pid, el tid, la IP:puerto del cliente y el mensaje. Luego se emite con **un solo `write()`** bajo `mtx_registro`. Como ese mutex está en la región compartida, ni los hilos ni los procesos hijos pueden entremezclar líneas. La información va a stdout y los errores a stderr (RF-69), salvo que se configure un archivo de registro.

## 9. Señales y detención ordenada (RNF-12)

| Señal | Tratamiento |
|---|---|
| SIGINT, SIGTERM | `sigaction` sin `SA_RESTART`. El manejador solo hace `atomic_store(&servidor_corriendo, 0)` y preserva `errno`. |
| SIGPIPE | Ignorada. Además, todos los envíos usan `MSG_NOSIGNAL`. Un cliente que se va a mitad de una escritura produce `EPIPE`, nunca la muerte del proceso (RNF-08). |
| SIGCHLD (FORKED) | Solo interrumpe el `poll()`; la recolección se hace en el ciclo principal (sección 4). |

**Las escrituras también se pueden interrumpir.** Usan `send(MSG_DONTWAIT)` más `poll(POLLOUT)` en tramos de 200 ms y revisan la bandera de detención en cada tramo. Si el cliente no consume nada durante el tiempo de inactividad, la transferencia se abandona (RF-34). Sin este mecanismo, un cliente que deja de leer bloqueaba la detención indefinidamente (defecto 4 del reporte). El aviso final —DESPEDIDA "Servidor detenido"— se envía con un plazo propio de 1 s, para que el cliente pueda distinguir un apagado del servidor de una caída de red (sección 8.4).

**Código de salida.** Si la detención es ordenada, el servidor termina con 0. Si el puerto está ocupado o falla `bind`, termina con 3. Si el error es interno, con 6 (`codigos_salida.h`).

## 10. Alternativas descartadas

| Alternativa | Por qué se descartó |
|---|---|
| `signal(SIGCHLD, SIG_IGN)` para que el kernel recolecte solo a los hijos | Se pierde el estado de salida de cada hijo, así que no se puede registrar que un hijo murió por una señal ni descontarlo con precisión del límite de clientes. |
| Recolectar hijos dentro del manejador de SIGCHLD | Habría que tocar la tabla de PIDs y el registro desde un manejador de señal, y las funciones de registro no son *async-signal-safe*. |
| `volatile sig_atomic_t` para la bandera de detención | ThreadSanitizer lo reporta como carrera de datos cuando varios hilos leen la bandera. `atomic_int` *lock-free* es seguro a la vez en manejadores de señal y entre hilos. |
| `self-pipe` o `signalfd` para atender señales | Funciona, pero agrega un descriptor y complejidad. El `poll()` con plazo, sin `SA_RESTART`, cumple el mismo objetivo con menos código. |
| Grupo fijo de hilos (*thread pool*) | La especificación pide un hilo por conexión (RF-56). Además, el canal en vivo ocupa un hilo indefinidamente, lo que agotaría un grupo de tamaño fijo. |
| `pthread_cancel` para detener hilos | Puede cancelar un hilo con un mutex tomado o en medio de un mensaje (RF-59). |
| Memoria compartida con nombre (`shm_open`) | Deja un objeto en `/dev/shm` si el servidor muere sin limpiar. Con `mmap` anónimo, la memoria desaparece junto con los procesos. |
| Atender todo con un único ciclo de eventos (`epoll`) | No es ninguno de los tres modos pedidos. |

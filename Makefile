# Makefile — Servidor de video en C para Linux (RNF-02, CA-01, entregable 2)
#
# Una sola orden produce ambos ejecutables:
#   make                 producción   -> bin/servidor, bin/cliente
#   make debug           depuración   -> bin/debug/servidor, bin/debug/cliente
#   make asan            depuración + AddressSanitizer/UBSan -> bin/asan/
#   make tsan            depuración + ThreadSanitizer        -> bin/tsan/
#   make verificar       producción desde cero con avisos como errores (CA-01)
#   make clean           borra build/ y bin/
#
# Pruebas (necesitan python3, sin bibliotecas extra):
#   make pruebas         batería comparativa en los tres modos (tests/bateria.py)
#   make rendimiento     mediciones comparativas de rendimiento (tests/rendimiento.py)
#   make pruebas-tsan    batería contra bin/tsan/servidor (carreras de datos, CA-11)
#   make pruebas-asan    batería contra bin/asan/servidor (memoria y fugas)
#
# server/makefile (de la capa de servicio) sigue funcionando por separado:
#   cd server && make test

CC       ?= gcc
CSTD     := -std=c11
CPPFLAGS := -D_DEFAULT_SOURCE -Iserver -Icod_comun -MMD -MP
WARN     := -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes -Wformat=2
LDLIBS   := -pthread

TIPO ?= release

ifeq ($(TIPO),release)
  OPT := -O2 -DNDEBUG
  BIN := bin
else ifeq ($(TIPO),debug)
  OPT := -O0 -g3
  BIN := bin/debug
else ifeq ($(TIPO),asan)
  OPT := -O1 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined
  LDFLAGS += -fsanitize=address,undefined
  BIN := bin/asan
else ifeq ($(TIPO),tsan)
  OPT := -O1 -g3 -fsanitize=thread
  LDFLAGS += -fsanitize=thread
  BIN := bin/tsan
else
  $(error TIPO desconocido "$(TIPO)": use release, debug, asan o tsan)
endif

CFLAGS += $(CSTD) $(WARN) $(OPT) -pthread $(EXTRA_CFLAGS)
OBJ    := build/$(TIPO)

# test_servicios.c es la prueba de la capa de servicio y tiene su propio main().
SRC_COMUN    := $(wildcard cod_comun/*.c)
SRC_SERVIDOR := $(filter-out server/test_servicios.c,$(wildcard server/*.c))
SRC_CLIENTE  := client/cliente.c

OBJ_COMUN    := $(SRC_COMUN:%.c=$(OBJ)/%.o)
OBJ_SERVIDOR := $(SRC_SERVIDOR:%.c=$(OBJ)/%.o)
OBJ_CLIENTE  := $(SRC_CLIENTE:%.c=$(OBJ)/%.o)

.PHONY: all debug release asan tsan verificar pruebas rendimiento pruebas-tsan pruebas-asan clean

all: $(BIN)/servidor $(BIN)/cliente

release:
	@$(MAKE) --no-print-directory TIPO=release
debug:
	@$(MAKE) --no-print-directory TIPO=debug
asan:
	@$(MAKE) --no-print-directory TIPO=asan
tsan:
	@$(MAKE) --no-print-directory TIPO=tsan

verificar:
	@rm -rf build/release bin/servidor bin/cliente
	@$(MAKE) --no-print-directory TIPO=release EXTRA_CFLAGS=-Werror
	@echo "Compilación de producción sin avisos."

# bin/ no figura en el .gitignore del repositorio: se ignora con su propio .gitignore.
$(BIN)/servidor: $(OBJ_COMUN) $(OBJ_SERVIDOR)
	@mkdir -p $(@D) && printf '*\n' > bin/.gitignore
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BIN)/cliente: $(OBJ_CLIENTE)
	@mkdir -p $(@D) && printf '*\n' > bin/.gitignore
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

pruebas: all
	python3 tests/bateria.py --todos-los-modos

rendimiento: all
	python3 tests/rendimiento.py

pruebas-tsan:
	@$(MAKE) --no-print-directory TIPO=tsan
	TSAN_OPTIONS="halt_on_error=0" python3 tests/bateria.py --todos-los-modos --servidor bin/tsan/servidor

pruebas-asan:
	@$(MAKE) --no-print-directory TIPO=asan
	ASAN_OPTIONS="detect_leaks=1" python3 tests/bateria.py --todos-los-modos --servidor bin/asan/servidor

clean:
	rm -rf build bin

-include $(OBJ_COMUN:.o=.d) $(OBJ_SERVIDOR:.o=.d) $(OBJ_CLIENTE:.o=.d)

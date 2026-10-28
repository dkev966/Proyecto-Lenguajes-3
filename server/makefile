CC      = gcc
CFLAGS  = -std=gnu11 -Wall -Wextra -g
LDFLAGS =

SRC  = catalogo.c sesiones.c test_servicios.c
OBJ  = $(SRC:.c=.o)
DEPS = catalogo.h sesiones.h
BIN  = test_servicios

.PHONY: all test asan valgrind clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $@ $(LDFLAGS)

# Cada .o se recompila si cambia su .c o cualquiera de los .h
%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c $< -o $@

ARGS = ../videos/demanda/ ../videos/live/ ../videos/.duraciones.txt
# make test: compila y ejecuta
test: $(BIN)
	./$(BIN) $(ARGS)

# make asan: recompila con sanitizadores y ejecuta
asan: clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address,undefined" \
	        LDFLAGS="-fsanitize=address,undefined" $(BIN)
	./$(BIN) $(ARGS)

# make valgrind: compilación normal, ejecutada bajo valgrind
valgrind: clean $(BIN)
	valgrind --leak-check=full --track-fds=yes ./$(BIN) $(ARGS)

clean:
	rm -f $(OBJ) $(BIN)

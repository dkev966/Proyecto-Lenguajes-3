#include <stdio.h>
#include "sesiones.h"

static int fallos = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #cond); fallos++; } } while (0)

int main(void) {
    
    printf(fallos ? "%d fallos\n" : "OK\n", fallos);
    return fallos != 0;
}

/*
 * codigos_salida.h — Códigos de terminación de ambos programas (RF-29, RF-68).
 * Usarlos en exit()/return de main() del servidor y del cliente.
 */
#ifndef CODIGOS_SALIDA_H
#define CODIGOS_SALIDA_H

#define SALIDA_EXITO        0   /* ejecución correcta / detención ordenada       */
#define SALIDA_ARGUMENTOS   2   /* argumentos ausentes, inválidos o contradictorios */
#define SALIDA_RED          3   /* no se pudo conectar / bind / puerto ocupado    */
#define SALIDA_CONTENIDO    4   /* el servidor rechazó el contenido solicitado    */
#define SALIDA_CAPACIDAD    5   /* el servidor rechazó por límite de clientes     */
#define SALIDA_INTERNO      6   /* error interno (memoria, fork, hilos, señales)  */

#endif /* CODIGOS_SALIDA_H */

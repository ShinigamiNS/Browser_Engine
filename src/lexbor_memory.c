/* lexbor_memory.c
   Implements Lexbor's overrideable memory functions using standard malloc.
   Compile this with gcc alongside the other Lexbor files. */

#include <stdlib.h>

void* lexbor_malloc(size_t size)             { return malloc(size);        }
void* lexbor_calloc(size_t n, size_t size)   { return calloc(n, size);     }
void* lexbor_realloc(void* ptr, size_t size) { return realloc(ptr, size);  }
void  lexbor_free(void* ptr)                 { free(ptr);                  }

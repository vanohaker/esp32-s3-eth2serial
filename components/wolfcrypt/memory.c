#include <stdlib.h>
void* knife_malloc(size_t size, void* heap, int type) {
    (void)heap;
    (void)type;
    return malloc(size);
}
void* knife_realloc(void* pointer, size_t size, void* heap, int type) {
    (void)heap;
    (void)type;
    return realloc(pointer, size);
}
void knife_free(void* pointer, void* heap, int type) {
    (void)heap;
    (void)type;
    free(pointer);
}

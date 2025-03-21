#include "vector.h"
#include "packet.h"

void vector_init(Vector *vec, int cap) {
    vec->v_size = 0;
    vec->v_capacity = cap > 0 ? cap : 1;
    vec->data = (int *)malloc(vec->v_capacity * sizeof(int));
}

void vector_free(Vector *vec) {
    free(vec->data);
    vec->v_size = 0;
    vec->v_capacity = 0;
}

int vector_size(const Vector *vec) {
    return vec->v_size;
}

int vector_capacity(const Vector *vec) {
    return vec->v_capacity;
}

bool vector_empty(const Vector *vec) {
    return vec->v_size == 0;
}

int vector_front(const Vector *vec) {
    return vec->data[0];
}

int vector_back(const Vector *vec) {
    return vec->data[vec->v_size - 1];
}

void vector_push_back(Vector *vec, tcp_packet* packet) {
    if (vec->v_size == vec->v_capacity) {
        vec->v_capacity *= 2;
        vec->data = (int *)realloc(vec->data, vec->v_capacity * sizeof(int));
    }
    vec->data[vec->v_size++] = packet;
}

void vector_insert(Vector *vec, int index, tcp_packet* packet) {
    if (index < 0 || index > vec->v_size) return;

    if (vec->v_size == vec->v_capacity) {
        vec->v_capacity *= 2;
        vec->data = (int *)realloc(vec->data, vec->v_capacity * sizeof(int));
    }

    for (int i = vec->v_size; i > index; --i)
        vec->data[i] = vec->data[i - 1];

    vec->data[index] = packet;
    vec->v_size++;
}

void vector_erase(Vector *vec, int index) {
    if (index < 0 || index >= vec->v_size) return;

    for (int i = index; i < vec->v_size - 1; ++i)
        vec->data[i] = vec->data[i + 1];

    vec->v_size--;
}

int vector_at(Vector *vec, int index) {
    return vec->data[index];
}

void vector_shrink_to_fit(Vector *vec) {
    vec->v_capacity = vec->v_size;
    vec->data = (int *)realloc(vec->data, vec->v_capacity * sizeof(int));
}

void vector_display(const Vector *vec) {
    printf("[");
    for (int i = 0; i < vec->v_size; i++)
        printf("%d%s", vec->data[i], i == vec->v_size - 1 ? "" : ", ");
    printf("]\n");
}
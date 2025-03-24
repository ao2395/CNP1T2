#include "vector.h"

void vector_init(Vector *vec, int cap) {
    vec->v_size = 0;
    vec->v_capacity = cap > 0 ? cap : 1;
    vec->data = (tcp_packet**)malloc(vec->v_capacity * sizeof(tcp_packet*));
    for (int i = 0; i < vec->v_capacity; i++) {
        vec->data[i] = NULL;
    }
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

tcp_packet* vector_front(const Vector *vec) {
    if (vec->v_size == 0) return NULL;
    return vec->data[0];
}

tcp_packet* vector_back(const Vector *vec) {
    if (vec->v_size == 0) return NULL;
    return vec->data[vec->v_size - 1];
}

void vector_push_back(Vector *vec, tcp_packet* packet) {
    if (vec->v_size == vec->v_capacity) {
        vec->v_capacity *= 2;
        vec->data = (tcp_packet**)realloc(vec->data, vec->v_capacity * sizeof(tcp_packet*));
    }
    vec->data[vec->v_size++] = packet;
}

void vector_insert(Vector *vec, int index, tcp_packet* packet) {
    if (index < 0 || index > vec->v_size) return;

    if (vec->v_size == vec->v_capacity) {
        vec->v_capacity *= 2;
        vec->data = (tcp_packet**)realloc(vec->data, vec->v_capacity * sizeof(tcp_packet*));
    }

    for (int i = vec->v_size; i > index; --i)
        vec->data[i] = vec->data[i - 1];

    vec->data[index] = packet;
    vec->v_size++;
}

void vector_erase(Vector *vec, int index) {
    if (index < 0 || index >= vec->v_size) return;

    // Free memory of the packet at index if it exists
    if (vec->data[index] != NULL) {
        free(vec->data[index]);
        vec->data[index] = NULL;
    }

    // Shift elements to fill the gap
    for (int i = index; i < vec->v_size - 1; ++i)
        vec->data[i] = vec->data[i + 1];

    // Set the last element to NULL and decrease size
    vec->data[vec->v_size - 1] = NULL;
    vec->v_size--;
}

tcp_packet* vector_at(Vector *vec, int index) {
    if (index < 0 || index >= vec->v_capacity) {
        return NULL;
    }
    return vec->data[index];
}

void vector_shrink_to_fit(Vector *vec) {
    if (vec->v_size < vec->v_capacity) {
        vec->v_capacity = vec->v_size > 0 ? vec->v_size : 1;
        vec->data = (tcp_packet**)realloc(vec->data, vec->v_capacity * sizeof(tcp_packet*));
    }
}

void vector_display(const Vector *vec) {
    printf("[");
    for (int i = 0; i < vec->v_size; i++) {
        tcp_packet* pkt = vec->data[i];
        if (pkt != NULL) {
            printf("%d", pkt->hdr.seqno);
        } else {
            printf("NULL");
        }
        
        if (i != vec->v_size - 1) {
            printf(", ");
        }
    }
    printf("]\n");
}
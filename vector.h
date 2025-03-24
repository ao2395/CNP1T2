#ifndef VECTOR_H
#define VECTOR_H

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

// Forward declare the struct without typedef to avoid collision
struct tcp_packet;

typedef struct {
    struct tcp_packet** data;
    int v_size;
    int v_capacity;
} Vector;

// Function prototypes
void vector_init(Vector *vec, int cap);
void vector_free(Vector *vec);
int vector_size(const Vector *vec);
int vector_capacity(const Vector *vec);
bool vector_empty(const Vector *vec);
struct tcp_packet* vector_front(const Vector *vec);
struct tcp_packet* vector_back(const Vector *vec);
void vector_push_back(Vector *vec, struct tcp_packet* packet);
void vector_insert(Vector *vec, int index, struct tcp_packet* packet);
void vector_erase(Vector *vec, int index);
struct tcp_packet* vector_at(Vector *vec, int index);
void vector_shrink_to_fit(Vector *vec);
void vector_display(const Vector *vec);

#endif /* VECTOR_H */
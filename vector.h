#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

typedef struct {
    int *data;
    int v_size;
    int v_capacity;
} Vector;

// Function prototypes
void vector_init(Vector *vec, int cap);
void vector_free(Vector *vec);
int vector_size(const Vector *vec);
int vector_capacity(const Vector *vec);
bool vector_empty(const Vector *vec);
int vector_front(const Vector *vec);
int vector_back(const Vector *vec);
void vector_push_back(Vector *vec, int element);
void vector_insert(Vector *vec, int index, int element);
void vector_erase(Vector *vec, int index);
int vector_at(Vector *vec, int index);
void vector_shrink_to_fit(Vector *vec);
void vector_display(const Vector *vec);

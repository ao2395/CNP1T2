#include "vector.h"

void vector_init(Vector *vec, int cap) { // vector initializer, with specifed cap
    vec->v_size = 0; //start with empty vector
    vec->v_capacity = cap > 0 ? cap : 1; //make sure cap is atleast 1
    vec->data = (tcp_packet**)malloc(vec->v_capacity * sizeof(tcp_packet*)); //make array and allocate memory to store the pkt pointers
    for (int i = 0; i < vec->v_capacity; i++) {//loop through all pointers and intially set to null
        vec->data[i] = NULL;
    }
}

void vector_free(Vector *vec) { //deallocating the memory that is used by the vector array
    free(vec->data);
    vec->v_size = 0; //reset size and cap to 0
    vec->v_capacity = 0;
}

//accessor functions to return the size and cap
int vector_size(const Vector *vec) {
    return vec->v_size;
}
int vector_capacity(const Vector *vec) {
    return vec->v_capacity;
}



bool vector_empty(const Vector *vec) { // bool to return true or false checking if vector is empty
    return vec->v_size == 0;
}

tcp_packet* vector_front(const Vector *vec) { //pointing to the first element in vector array 
    if (vec->v_size == 0) return NULL; //if empty return null
    return vec->data[0];
}

tcp_packet* vector_back(const Vector *vec) { //pointing to the last element in vector arrray
    if (vec->v_size == 0) return NULL;//if empty return null
    return vec->data[vec->v_size - 1];
}

void vector_push_back(Vector *vec, tcp_packet* packet) { //add a new element to the end of the vector
    if (vec->v_size == vec->v_capacity) { //in the case thst the vector is full 
        vec->v_capacity *= 2; //double its cap
        vec->data = (tcp_packet**)realloc(vec->data, vec->v_capacity * sizeof(tcp_packet*)); //reallocate memory to resize the array 
    }
    vec->data[vec->v_size++] = packet; //after storing packet increment size
}

void vector_insert(Vector *vec, int index, tcp_packet* packet) { //to insert an element at a specfic position 
    if (index < 0 || index > vec->v_size) return; //check if the specifed index is within bounds

    if (vec->v_size == vec->v_capacity) { //expanding cap if necesarry 
        vec->v_capacity *= 2; //double cap
        vec->data = (tcp_packet**)realloc(vec->data, vec->v_capacity * sizeof(tcp_packet*));
    }

    for (int i = vec->v_size; i > index; --i) //shifting elements forward to make space for new packets
        vec->data[i] = vec->data[i - 1];

    vec->data[index] = packet;//pkt is palced in designated index
    vec->v_size++; //size is incremented
}
//STOPPEDHERE
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
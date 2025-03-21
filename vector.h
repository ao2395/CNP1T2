#include "stdio.h"
struct MyVector
{
	private:
		int *data;						//pointer to int(array) to store elements
		int v_size;						//current size of vector (number of elements in vector)
		int v_capacity;					//capacity of vector
	public:
		MyVector(int cap=0);			//Constructor
		~MyVector();					//Destructor
		int size() const;				//Return current size of vector
		int capacity() const;			//Return capacity of vector
		bool empty() const; 			//Rturn true if the vector is empty, False otherwise
		const int& front();				//Returns reference of the first element in the vector
		const int& back();				//Returns reference of the Last element in the vector
		void push_back(int element);		//Add an element at the end of vector
		void insert(int index, int element); //Add an element at the index 
		void erase(int index);			//Removes an element from the index
		int& operator[](int index);			//Returns the reference of an element at given index
		int& at(int index); 				//return reference of the element at given index
		void shrink_to_fit();			//Reduce vector capacity to fit its size
		void display();
};
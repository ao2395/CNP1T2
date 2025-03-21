# Reference
# http://makepp.sourceforge.net/1.19/makepp_tutorial.html

CC = gcc -c
SHELL = /bin/bash

# Compiling flags here
CFLAGS = -Wall -I.

LINKER = gcc -o
# Linking flags here
LFLAGS = -Wall

OBJDIR = ../obj

# Object files for client and server
CLIENT_OBJECTS := $(OBJDIR)/rdt_sender.o $(OBJDIR)/common.o $(OBJDIR)/packet.o $(OBJDIR)/vector.o
SERVER_OBJECTS := $(OBJDIR)/rdt_receiver.o $(OBJDIR)/common.o $(OBJDIR)/packet.o $(OBJDIR)/vector.o

# Program names
CLIENT := $(OBJDIR)/rdt_sender
SERVER := $(OBJDIR)/rdt_receiver

# Target
TARGET: $(OBJDIR) $(CLIENT) $(SERVER)

$(CLIENT): $(CLIENT_OBJECTS)
	$(LINKER) $@ $(CLIENT_OBJECTS)
	@echo "Client link complete!"

$(SERVER): $(SERVER_OBJECTS)
	$(LINKER) $@ $(SERVER_OBJECTS)
	@echo "Server link complete!"

$(OBJDIR)/%.o: %.c common.h packet.h vector.h
	$(CC) $(CFLAGS) $< -o $@
	@echo "Compiled: $<"

$(OBJDIR)/vector.o: vector.c vector.h
	$(CC) $(CFLAGS) vector.c -o $(OBJDIR)/vector.o
	@echo "Compiled: vector.c"

clean:
	@if [ -d $(OBJDIR) ]; then rm -r $(OBJDIR); fi;
	@echo "Cleanup complete!"

$(OBJDIR):
	@[ -d $(OBJDIR) ] || mkdir $(OBJDIR)
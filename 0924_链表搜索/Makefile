CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
TARGET   = linked_list_pmc

all: $(TARGET)

$(TARGET): linked_list_pmc.c
	$(CC) $(CFLAGS) -o $@ $<

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all run clean

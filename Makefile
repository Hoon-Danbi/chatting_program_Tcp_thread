CC=gcc
CFLAGS=-O2 -Wall -pthread

all: server client

server: chat_server_thread_answer.c
	$(CC) $(CFLAGS) -o chat_server_thread_answer chat_server_thread_answer.c

client: chat_client_thread_answer.c
	$(CC) $(CFLAGS) -o chat_client_thread_answer chat_client_thread_answer.c

clean:
	rm -f chat_server_thread_answer chat_client_thread_answer


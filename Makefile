all:
	gcc -g -c src/message_encoder.c
	gcc -g -c src/client.c
	gcc -g -c src/server.c
	gcc -g -c src/main.c
	gcc -g -o kermit message_encoder.o main.o client.o server.o -lrt
clean:
	rm -rf *.o kermit

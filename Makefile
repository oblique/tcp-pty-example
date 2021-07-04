all: tcp-pty-client tcp-pty-server

tcp-pty-client: tcp-pty-client.c
	gcc -Wall -Wextra $^ -o $@

tcp-pty-server: tcp-pty-server.c
	gcc -Wall -Wextra $^ -o $@

clean:
	rm -f tcp-pty-client tcp-pty-server

#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <arpa/inet.h>

typedef struct s_message {
	char *data;
	int length;
} t_message;

typedef struct s_client {
	int id;
	int fd;
	char *recv_buffer;
	int recv_buffer_length;
	char *send_buffer;
	int send_buffer_length;
} t_client;

typedef struct s_server {
	int listen_fd;
	int	max_fd;
	int new_client_id;
	fd_set read_fds;
	fd_set write_fds;
	t_client **clients;
	int clients_size;
} t_server;

enum e_buffers {
	RECV_BUFFER_SIZE = 4096,
	SPRINTF_BUFFER_SIZE = 128
};


void	error_exit(const char *msg) {
	write(2, msg, strlen(msg));
	write(2, "\n", 1);
	exit(EXIT_FAILURE);
}

void fatal_error() {
	error_exit("Fatal error");
}

void	check_error(int ret) {
	if (ret < 0) {
		fatal_error();
	}
}

void	check_null(void *ptr) {
	if (ptr == NULL) {
		fatal_error();
	}
}

void set_max_fd(t_server *server, int fd) {
	if (fd == server->max_fd) {
		while (!FD_ISSET(server->max_fd, &server->read_fds) &&
				!FD_ISSET(server->max_fd, &server->write_fds)) {
			--server->max_fd;
		}
	}
}

void resize_clients(t_server *server, int new_size) {
	void *ptr = realloc(server->clients,
								sizeof(*server->clients) * new_size);
	check_null(ptr);
	server->clients = ptr;
	memset(&server->clients[server->clients_size], 0,
			sizeof(*server->clients) * (new_size - server->clients_size));
	server->clients_size = new_size;
}

void forward_strcpy(char *dst, const char *src) {
	int i = 0;
	while (src[i]) {
		dst[i] = src[i];
		++i;
	}
	dst[i] = '\0';
}

void buffer_join(char **buffer, int *buffer_length,
									const char *message, int message_length) {
	int new_length = *buffer_length + message_length;
	void *ptr = realloc(*buffer, new_length + 1);
	check_null(ptr);
	*buffer = ptr;
	strcpy(&(*buffer)[*buffer_length], message);
	*buffer_length = new_length;
}

void add_message_to_client(t_client *client, const char *message,
														int message_length) {
	buffer_join(&client->send_buffer, &client->send_buffer_length,
			message, message_length);
}

void add_message_to_other_clients(t_server *server, t_client *client,
								const char *message, int message_length) {
	for (int fd = 0; fd <= server->max_fd; ++fd) {
		if (fd != server->listen_fd &&
				fd != client->fd &&
				server->clients[fd] != NULL) {
			add_message_to_client(server->clients[fd], message, message_length);
			FD_SET(fd, &server->write_fds);
		}
	}
}

void accept_new_client(t_server *server) {
	int fd = accept(server->listen_fd, NULL, NULL);
	check_error(fd);

	if (fd >= server->clients_size) {
		int new_size = server->clients_size * 2;
		if (new_size < fd + 1) {
			new_size = fd + 1;
		}
		resize_clients(server, new_size);
	}

	t_client *client = calloc(1, sizeof(*client));
	check_null(client);

	client->fd = fd;
	client->id = server->new_client_id;
	++server->new_client_id;
	server->clients[fd] = client;

	char message[SPRINTF_BUFFER_SIZE] = {0};
	int length = sprintf(message, "server: client %d just arrived\n", client->id);
	check_error(length);
	add_message_to_other_clients(server, client, message, length);
	FD_SET(fd, &server->read_fds);
	if (fd > server->max_fd) {
		server->max_fd = fd;
	}
}

void remove_client(t_server *server, int fd) {
	t_client *client = server->clients[fd];
	char message[SPRINTF_BUFFER_SIZE] = {0};
	int length = sprintf(message, "server: client %d just left\n", client->id);
	check_error(length);
	add_message_to_other_clients(server, client, message, length);

	free(client->recv_buffer);
	free(client->send_buffer);
	free(client);
	server->clients[fd] = NULL;
	FD_CLR(fd, &server->read_fds);
	FD_CLR(fd, &server->write_fds);
	set_max_fd(server, fd);
	int ret = close(fd);
	check_error(ret);
}

t_message parse_recv_buffer(t_client *client) {
	char prefix[SPRINTF_BUFFER_SIZE] = {0};
	int prefix_length = sprintf(prefix, "client %d: ", client->id);
	check_error(prefix_length);

	t_message message = {0};
	char *newline_pos = strstr(client->recv_buffer, "\n");
	while (newline_pos != NULL) {
		int length = newline_pos - client->recv_buffer + 1;
		int prefixed_message_length = prefix_length + length;
		char *prefixed_message = calloc(prefixed_message_length + 1, 1);
		strcpy(prefixed_message, prefix);
		sprintf(&prefixed_message[prefix_length], "%.*s", length, client->recv_buffer);

		forward_strcpy(client->recv_buffer, &client->recv_buffer[length]);
		client->recv_buffer_length -= length;
		buffer_join(&message.data, &message.length,
									prefixed_message, prefixed_message_length);
		free(prefixed_message);
		newline_pos = strstr(client->recv_buffer, "\n");
	}
	return message;
}

void read_client_data(t_server *server, int fd) {
	char recv_buffer[RECV_BUFFER_SIZE + 1] = {0};

	int nbytes = recv(fd, recv_buffer, RECV_BUFFER_SIZE, 0);
	check_error(nbytes);
	recv_buffer[nbytes] = '\0';
	if (nbytes == 0) {
		remove_client(server, fd);
	} else {
		t_client *client = server->clients[fd];
		buffer_join(&client->recv_buffer,
					&client->recv_buffer_length,
					recv_buffer,
					nbytes);
		t_message message = parse_recv_buffer(client);
		if (message.data) {
			add_message_to_other_clients(server, client,
										message.data, message.length);
		}
		free(message.data);
	}
}

void send_client_data(t_server *server, int fd) {
	t_client *client = server->clients[fd];
	int nbytes = send(fd, client->send_buffer,
									client->send_buffer_length, 0);
	if (nbytes <= 0) {
		fatal_error();
	}
	forward_strcpy(client->send_buffer, &client->send_buffer[nbytes]);
	client->send_buffer_length -= nbytes;
	if (client->send_buffer_length == 0) {
		FD_CLR(fd, &server->write_fds);
	}
}

void run(t_server *server) {
	while (42) {
		fd_set read_fds = server->read_fds;
		fd_set write_fds = server->write_fds;

		int ready_clients = select(server->max_fd + 1, &read_fds, &write_fds,
									NULL, NULL);
		check_error(ready_clients);
		for (int fd = 0; ready_clients > 0 && fd <= server->max_fd; ++fd) {
			if (FD_ISSET(fd, &read_fds)) {
				if (fd == server->listen_fd) {
					accept_new_client(server);
				} else {
					read_client_data(server, fd);
				}
				--ready_clients;
			} else if (FD_ISSET(fd, &write_fds)) {
				send_client_data(server, fd);
				--ready_clients;
			}
		}
	}
}

int	main(int argc, char **argv) {
	if (argc != 2) {
		error_exit("Wrong number of arguments");
	}
	int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	check_error(listen_fd);

	struct sockaddr_in servaddr = {0};
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	servaddr.sin_port = htons(atoi(argv[1]));

	int ret = bind(listen_fd, (const struct sockaddr *)&servaddr, sizeof(servaddr));
	check_error(ret);

	ret = listen(listen_fd, SOMAXCONN);
	check_error(ret);

	t_server server = {0};
	server.listen_fd = listen_fd;
	server.max_fd = listen_fd;
	FD_SET(listen_fd, &server.read_fds);

	run(&server);
}

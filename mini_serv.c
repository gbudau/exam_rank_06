#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/select.h>

typedef struct s_message {
	char *data;
	int len;
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
	int max_fd;
	int new_client_id;

	fd_set read_fds;
	fd_set write_fds;

	t_client **clients;
	int clients_size;
} t_server;

enum e_constants {
	RECV_BUFFER_LENGTH = 4096,
	SPRINTF_BUFFER_LENGTH = 128
};

void error_exit(const char *msg) {
	write(2, msg, strlen(msg));
	exit(EXIT_FAILURE);
}

void fatal_error(void) {
	error_exit("Fatal error\n");
}

void check_error(int ret) {
	if (ret < 0) {
		fatal_error();
	}
}

void check_null(void *ptr) {
	if (ptr == NULL) {
		fatal_error();
	}
}

void resize_clients_array(t_server *server, int new_size) {
	t_client **new_clients = realloc(server->clients, sizeof(*server->clients) * new_size);
	check_null(new_clients);
	memset(&new_clients[server->clients_size], 0,
			sizeof(t_client *) * (new_size - server->clients_size));
	server->clients = new_clients;
	server->clients_size = new_size;
}

void buffer_join(char **dst, int *dst_len, char *src, int src_len) {
	int new_len = *dst_len + src_len;
	char *new_dst = realloc(*dst, new_len + 1);
	check_null(new_dst);
	strcpy(&(new_dst)[*dst_len], src);
	*dst = new_dst;
	*dst_len = new_len;
}

void add_message_to_client(t_client *client, char *message, int len) {
	buffer_join(&client->send_buffer, &client->send_buffer_length,
				message, len);
}

void add_message_to_other_clients(t_server *server, t_client *client,
									char *message, int len) {
	for (int fd = 0; fd <= server->max_fd; ++fd) {
		if (server->clients[fd] != NULL &&
				fd != server->listen_fd &&
				fd != client->fd) {
			add_message_to_client(server->clients[fd], message, len);
			FD_SET(fd, &server->write_fds);
		}
	}
}

void accept_new_client(t_server *server) {
	int fd = accept(server->listen_fd, NULL, NULL);
	check_error(fd);

	if (server->clients_size <= fd) {
		int new_size = server->clients_size * 2;
		if (new_size <= fd) {
			new_size = fd + 1;
		}
		resize_clients_array(server, new_size);
	}

	t_client *client = calloc(1, sizeof(*client));
	client->id = server->new_client_id;
	++server->new_client_id;
	client->fd = fd;

	char message[SPRINTF_BUFFER_LENGTH] = {0};
	int len = sprintf(message, "server: client %d just arrived\n", client->id);
	check_error(len);
	add_message_to_other_clients(server, client, message, len);

	server->clients[fd] = client;
	FD_SET(fd, &server->read_fds);
	if (fd > server->max_fd) {
		server->max_fd = fd;
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

void remove_client(t_server *server, int fd) {
	t_client *client = server->clients[fd];

	char message[SPRINTF_BUFFER_LENGTH] = {0};
	int len = sprintf(message, "server: client %d just left\n", client->id);
	check_error(len);
	add_message_to_other_clients(server, client, message, len);

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

int strchr_index(const char *str, char c) {
	for (int i = 0; str[i]; ++i) {
		if (str[i] == c) {
			return i;
		}
	}
	return -1;
}

void ft_strcpy(char *dst, const char *src) {
	int i = 0;
	while (src[i]) {
		dst[i] = src[i];
		++i;
	}
	dst[i] = '\0';
}

// Check if there are any message and parse them
t_message parse_recv_buffer(t_client *client) {
	char prefix[SPRINTF_BUFFER_LENGTH] = {0};
	int prefix_len = sprintf(prefix, "client %d: ", client->id);
	check_error(prefix_len);

	t_message message = {0};

	int newline_index = strchr_index(client->recv_buffer, '\n');
	while (newline_index != -1) {
		int message_len = newline_index + 1;
		int prefixed_message_len = prefix_len + message_len;
		char *prefixed_message = malloc(prefixed_message_len + 1);
		check_null(prefixed_message);
		strcpy(prefixed_message, prefix);
		int ret = sprintf(&prefixed_message[prefix_len],
							"%.*s", message_len, client->recv_buffer);
		check_error(ret);
		ft_strcpy(client->recv_buffer, &client->recv_buffer[message_len]);
		client->recv_buffer_length -= message_len;

		buffer_join(&message.data, &message.len,
					prefixed_message, prefixed_message_len);

		free(prefixed_message);
		newline_index = strchr_index(client->recv_buffer, '\n');
	}
	return message;
}

void read_client_data(t_server *server, t_client *client) {
	char recv_buffer[RECV_BUFFER_LENGTH + 1] = {0};

	int nbytes = recv(client->fd, recv_buffer, RECV_BUFFER_LENGTH, 0);
	check_error(nbytes);
	if (nbytes == 0) {
		remove_client(server, client->fd);
	} else {
		buffer_join(&client->recv_buffer, &client->recv_buffer_length,
					recv_buffer, nbytes);
		t_message message = parse_recv_buffer(client);
		if (message.data) {
			add_message_to_other_clients(server, client, message.data, message.len);
		}
		free(message.data);
	}
}

void send_client_data(t_server *server, t_client *client) {
	int nbytes = send(client->fd, client->send_buffer, client->send_buffer_length, 0);
	if (nbytes <= 0) {
		fatal_error();
	}

	ft_strcpy(client->send_buffer, &client->send_buffer[nbytes]);
	client->send_buffer_length -= nbytes;
	if (client->send_buffer_length == 0) {
		FD_CLR(client->fd, &server->write_fds);
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
					read_client_data(server, server->clients[fd]);
				}
				--ready_clients;
			} else if (FD_ISSET(fd, &write_fds)) {
					send_client_data(server, server->clients[fd]);
				--ready_clients;
			}
		}
	}
}

int main(int argc, char **argv) {
	if (argc != 2) {
		error_exit("Wrong number of arguments\n");
	}

	int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	check_error(listen_fd);

	struct sockaddr_in servaddr =  {0};

	// assign IP, PORT 
	servaddr.sin_family = AF_INET; 
	servaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); //127.0.0.1
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

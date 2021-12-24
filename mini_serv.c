#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>

enum e_constants {
	RECV_BUFFER_LENGTH = 4096,
	SPRINTF_BUFFER_LENGTH = 128
};

typedef struct s_message {
	char *data;
	int len;
} t_message;

typedef struct s_client {
	int id;
	int fd;

	char *recv_buffer;
	int recv_buffer_len;

	char *send_buffer;
	int send_buffer_len;
} t_client;

typedef struct s_server {
	int listen_fd;
	int max_fd;
	int new_client_id;

	t_client **clients;
	int clients_size;

	fd_set all_fds;
	fd_set read_fds;
	fd_set write_fds;
} t_server;

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
	t_client **new_clients = realloc(server->clients, new_size * sizeof(*new_clients));
	check_null(new_clients);
	memset(&new_clients[server->clients_size], 0,
			sizeof(*new_clients) * (new_size - server->clients_size));
	server->clients = new_clients;
	server->clients_size = new_size;
}

void buffer_join(char **dst, int *dst_len, const char *src, int src_len) {
	int new_len = *dst_len + src_len;
	char *new_dst = realloc(*dst, new_len + 1);
	check_null(new_dst);
	strcpy(&new_dst[*dst_len], src);
	*dst = new_dst;
	*dst_len = new_len;
}

void add_message_to_client(t_client *client, const char *message, int len) {
	buffer_join(&client->send_buffer, &client->send_buffer_len, message, len);
}

void add_message_to_other_clients(t_server *server, t_client *client,
									const char *message, int len) {
	for (int fd = 0; fd <= server->max_fd; ++fd) {
		if (server->clients[fd] != NULL &&
				fd != client->fd) {
			add_message_to_client(server->clients[fd], message, len);
		}
	}
}

void accept_new_client(t_server *server) {
	int fd = accept(server->listen_fd, NULL, NULL);
	if (fd < 0) {
		return;
	}

	if (fd >= server->clients_size) {
		int new_size = server->clients_size * 2;
		if (fd >= new_size) {
			new_size = fd + 1;
		}
		resize_clients_array(server, new_size);
	}

	t_client *client = calloc(1, sizeof(*client));
	client->id = server->new_client_id;
	++server->new_client_id;
	client->fd = fd;

	char message_buffer[SPRINTF_BUFFER_LENGTH];
	int len = sprintf(message_buffer, "server: client %d just arrived\n", client->id);
	check_error(len);
	add_message_to_other_clients(server, client, message_buffer, len);

	server->clients[fd] = client;
	FD_SET(fd, &server->all_fds);
	if (fd > server->max_fd) {
		server->max_fd = fd;
	}
}

void set_max_fd(t_server *server, int fd) {
	if (fd == server->max_fd) {
		while (!FD_ISSET(server->max_fd, &server->all_fds)) {
			--server->max_fd;
		}
	}
}

void remove_client(t_server *server, int fd) {
	t_client *client = server->clients[fd];

	char message_buffer[SPRINTF_BUFFER_LENGTH] = {0};
	int len = sprintf(message_buffer, "server: client %d just left\n", client->id);
	check_error(len);
	add_message_to_other_clients(server, client, message_buffer, len);

	free(client->recv_buffer);
	free(client->send_buffer);
	free(client);
	server->clients[fd] = NULL;

	FD_CLR(fd, &server->all_fds);
	FD_CLR(fd, &server->read_fds);
	FD_CLR(fd, &server->write_fds);
	set_max_fd(server, fd);

	close(fd);
}

void ft_forward_strcpy(char *dst, const char *src) {
	int i = 0;
	while (src[i]) {
		dst[i] = src[i];
		++i;
	}
	dst[i] = '\0';
}

t_message parse_recv_buffer(t_client *client) {
	char prefix_buffer[SPRINTF_BUFFER_LENGTH] = {0};
	int prefix_len = sprintf(prefix_buffer, "client %d: ", client->id);
	check_error(prefix_len);

	t_message messages = {0};
	char *newline_pos = strstr(client->recv_buffer, "\n");
	while (newline_pos != NULL) {
		int message_len = (newline_pos - client->recv_buffer) + 1;
		int prefixed_message_len = prefix_len + message_len;
		
		char *prefixed_message = malloc(prefixed_message_len + 1);
		strcpy(prefixed_message, prefix_buffer);
		int ret = sprintf(&prefixed_message[prefix_len], "%.*s", message_len,
												client->recv_buffer);
		check_error(ret);
		ft_forward_strcpy(client->recv_buffer, &client->recv_buffer[message_len]);
		client->recv_buffer_len -= message_len;

		buffer_join(&messages.data, &messages.len,
										prefixed_message, prefixed_message_len);

		free(prefixed_message);
		newline_pos = strstr(client->recv_buffer, "\n");
	}

	return messages;
}

void read_client_data(t_server *server, t_client *client) {
	char recv_buffer[RECV_BUFFER_LENGTH + 1] = {0};

	int nbytes = recv(client->fd, recv_buffer, RECV_BUFFER_LENGTH, 0);

	if (nbytes < 0) {
		return;
	} else if (nbytes == 0) {
		remove_client(server, client->fd);
	} else {
		buffer_join(&client->recv_buffer, &client->recv_buffer_len,
				recv_buffer, nbytes);

		t_message messages = parse_recv_buffer(client);
		if (messages.data != NULL) {
			add_message_to_other_clients(server, client, messages.data, messages.len);
		}
		free(messages.data);
	}
}

void send_client_data(t_client *client) {
	int nbytes = send(client->fd, client->send_buffer, client->send_buffer_len, 0);
	if (nbytes <= 0) {
		return;
	}

	ft_forward_strcpy(client->send_buffer, &client->send_buffer[nbytes]);
	client->send_buffer_len -= nbytes;
}

void send_all(t_server *server) {
	for (int fd = 0; fd <= server->max_fd; ++fd) {
		t_client *client = server->clients[fd];
		if (client != NULL &&
				client->send_buffer_len != 0 &&
				FD_ISSET(fd, &server->write_fds)) {
			send_client_data(client);
		}
	}
}

void run(t_server *server) {
	while (42) {
		server->read_fds = server->write_fds = server->all_fds;

		int ready_clients = select(server->max_fd + 1,
									&server->read_fds, &server->write_fds,
									NULL, NULL);
		if (ready_clients <= 0) {
			continue;
		}
		for (int fd = 0; fd <= server->max_fd; ++fd) {
			if (FD_ISSET(fd, &server->read_fds)) {
				if (fd == server->listen_fd) {
					accept_new_client(server);
				} else {
					read_client_data(server, server->clients[fd]);
				}
				send_all(server);
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

	struct sockaddr_in servaddr = {0};
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
	FD_SET(listen_fd, &server.all_fds);

	run(&server);
}

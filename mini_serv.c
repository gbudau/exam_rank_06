#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct client {
	int sd;
	size_t id;

	char *recvbuffer;
	size_t recvbuffer_length;

	char *sendbuffer;
	size_t sendbuffer_length;

} t_client;

typedef struct server {
	int listen_sd;
	int max_sd;

	fd_set read_set;
	fd_set write_set;

	t_client **clients;
	size_t clients_capacity;

	size_t new_client_id;

} t_server;

enum e_constants {
	RECV_BUFFER_SIZE = 4096,
	SPRINTF_BUFFER_SIZE = 128
};

static const char *fatal_error = "Fatal error\n";

static void error_exit(const char *msg) {
	write(STDERR_FILENO, msg, strlen(msg));
	exit(EXIT_FAILURE);
}

static void check_null(const void *ptr) {
	if (ptr == NULL) {
		error_exit(fatal_error);
	}
}

static void check_error(const int return_status) {
	if (return_status < 0) {
		error_exit(fatal_error);
	}
}

static int strchr_index(const char *s, char c) {
	for (size_t i = 0; s[i]; ++i) {
		if (s[i] == c) {
			return i;
		}
	}
	return -1;
}

static int make_listen_socket(const int port) {
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	check_error(sockfd);

	struct sockaddr_in servaddr = {0};

	// assign IP, PORT
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	servaddr.sin_port = htons(port);

	// Binding newly created socket to given address
	check_error(bind(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)));

	// Set the socket to listen mode
	check_error(listen(sockfd, SOMAXCONN));
	return sockfd;
}

static void str_join(char **dst, size_t *dst_size,
					const char *src, const size_t src_size) {
	size_t new_size = *dst_size + src_size;
	char *new_str = realloc(*dst, new_size + 1);
	check_null(new_str);
	strcpy(&new_str[*dst_size], src);
	*dst = new_str;
	*dst_size = new_size;
}

static void add_message_to_client(t_client *client,
							const char *message, const size_t message_length) {
	str_join(&client->sendbuffer, &client->sendbuffer_length,
			message, message_length);
}

static void add_message_to_other_clients(t_server *server, t_client *client,
										char *message, size_t message_length) {
	for (int sd = 0; sd <= server->max_sd; ++sd) {
		if (server->clients[sd] &&
				sd != server->listen_sd &&
				sd != client->sd) {
			add_message_to_client(server->clients[sd], message, message_length);
			FD_SET(sd, &server->write_set);
		}
	}
}

static void resize_clients_array(t_server *server, const size_t index) {
		size_t new_capacity = 2 * server->clients_capacity;
		if (index >= new_capacity) {
			new_capacity = index + 1;
		}
		t_client **new_clients_array =
					realloc(server->clients, new_capacity * sizeof(t_client *));
		check_null(new_clients_array);
		size_t new_elements = new_capacity - server->clients_capacity;
		memset(&new_clients_array[server->clients_capacity], 0,
											new_elements * sizeof(t_client *));
		server->clients = new_clients_array;
		server->clients_capacity = new_capacity;
}

static t_client *create_new_client(const t_server *server) {
	int client_sd = accept(server->listen_sd, NULL, NULL);
	check_error(client_sd);
	t_client *client = calloc(1, sizeof(*client));
	check_null(client);
	client->sd = client_sd;
	client->id = server->new_client_id;
	return client;
}

static void add_new_client(t_server *server) {
	t_client *client = create_new_client(server);
	++server->new_client_id;

	if ((size_t)client->sd >= server->clients_capacity) {
		resize_clients_array(server, client->sd);
	}

	server->clients[client->sd] = client;
	FD_SET(client->sd, &server->read_set);
	if (server->max_sd < client->sd) {
		server->max_sd = client->sd;
	}

	char buffer[SPRINTF_BUFFER_SIZE];
	int length =
			sprintf(buffer, "server: client %zu just arrived\n", client->id);
	check_error(length);
	add_message_to_other_clients(server, client, buffer, length);
}

static void set_max_sd(t_server *server, const int sd) {
	if (server->max_sd == sd) {
		while (!FD_ISSET(server->max_sd, &server->read_set) &&
				!FD_ISSET(server->max_sd, &server->write_set)) {
			--server->max_sd;
		}
	}
}

static void remove_client(t_server *server, t_client *client) {
	char buffer[SPRINTF_BUFFER_SIZE];
	int length = sprintf(buffer, "server: client %zu just left\n", client->id);
	check_error(length);
	add_message_to_other_clients(server, client, buffer, length);

	int client_sd = client->sd;
	free(client->recvbuffer);
	free(client->sendbuffer);
	free(client);
	server->clients[client_sd] = NULL;
	FD_CLR(client_sd, &server->read_set);
	FD_CLR(client_sd, &server->write_set);
	set_max_sd(server, client_sd);
	check_error(close(client_sd));
}

static void ft_strcpy(char *dest, const char *src) {
	size_t i = 0;
	for (; src[i]; ++i) {
		dest[i] = src[i];
	}
	dest[i] = '\0';
}

static void extract_message(char **buffer, size_t *buffer_length,
							char **message, size_t *message_length) {
	*message = NULL;
	*message_length = 0;
	int newline_index = strchr_index(*buffer, '\n');
	if (newline_index == -1) {
		return;
	}
	*message_length = newline_index + 1;
	*message = malloc(*message_length + 1);
	const int ret = sprintf(*message, "%.*s", (int)*message_length, *buffer);
	check_error(ret);
	ft_strcpy(*buffer, &(*buffer)[*message_length]);
	*buffer_length = *buffer_length - *message_length;
}

static void extract_all_messages(t_server *server, t_client *client) {
	while (42) {
		char *message = NULL;
		size_t message_length = 0;
		extract_message(&client->recvbuffer, &client->recvbuffer_length,
				&message, &message_length);
		if (message == NULL) {
			return;
		}

		char prefix_buffer[SPRINTF_BUFFER_SIZE];
		int prefix_len = sprintf(prefix_buffer, "client %zu: ", client->id);
		check_error(prefix_len);

		size_t prefixed_message_length = prefix_len + message_length;
		char *prefixed_message = malloc(prefixed_message_length + 1);
		strcpy(prefixed_message, prefix_buffer);
		strcpy(&prefixed_message[prefix_len], message);

		add_message_to_other_clients(server, client,
								prefixed_message, prefixed_message_length);
		free(message);
		free(prefixed_message);
	}
}

static void recv_data(t_server *server, t_client *client) {
	char buffer[RECV_BUFFER_SIZE + 1] = {0};

	ssize_t nbytes = recv(client->sd, buffer, RECV_BUFFER_SIZE, 0);
	check_error(nbytes);
	if (nbytes == 0) {
		remove_client(server, client);
	} else {
		str_join(&client->recvbuffer, &client->recvbuffer_length,
				buffer, nbytes);
		extract_all_messages(server, client);
	}
}

static void send_data(t_server *server, t_client *client) {
	ssize_t nbytes =
			send(client->sd, client->sendbuffer, client->sendbuffer_length, 0);
	check_error(nbytes);
	if (nbytes == 0) {
		remove_client(server, client);
	} else {
		strcpy(client->sendbuffer, &client->sendbuffer[nbytes]);
		client->sendbuffer_length -= nbytes;
		char *new_buff =
					realloc(client->sendbuffer, client->sendbuffer_length + 1);
		if (new_buff == NULL) {
			error_exit(fatal_error);
		}
		client->sendbuffer = new_buff;
		if (client->sendbuffer_length == 0) {
			FD_CLR(client->sd, &server->write_set);
		}
	}
}

static void run_server(t_server *server) {
	while (42) {
		fd_set read_set = server->read_set;
		fd_set write_set = server->write_set;
		int ready_sockets = select(server->max_sd + 1,
									&read_set, &write_set,
									NULL, NULL);
		check_error(ready_sockets);
		for (int sd = 0; sd <= server->max_sd && ready_sockets; ++sd) {
			if (FD_ISSET(sd, &read_set)) {
				if (sd == server->listen_sd) {
					add_new_client(server);
				} else {
					recv_data(server, server->clients[sd]);
				}
				--ready_sockets;
			} else if (FD_ISSET(sd, &write_set)) {
				send_data(server, server->clients[sd]);
				--ready_sockets;
			}
		}
	}
}

static void init_server(t_server *server, int port) {
	server->listen_sd = make_listen_socket(port);
	FD_SET(server->listen_sd, &server->read_set);
	server->max_sd = server->listen_sd;
}

int main(int argc, char **argv) {
	if (argc != 2) {
		error_exit("Wrong number of arguments\n");
	}
	t_server server = {0};
	int port = atoi(argv[1]);
	init_server(&server, port);
	run_server(&server);
}

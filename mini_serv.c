#include <errno.h>
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

	t_client *clients[FD_SETSIZE];
	size_t new_client_id;

} t_server;

enum e_constants {
	RECV_BUFFER_SIZE = 4096,
	SPRINTF_BUFFER_SIZE = 128
};

static const char *wrong_argument_number = "Wrong number of arguments\n";
static const char *fatal_error = "Fatal error\n";
static const char *client_arrive_fmt = "server: client %zu just arrived\n";
static const char *message_prefix_fmt = "client %zu: ";
static const char *client_left_fmt = "server: client %zu just left\n";

static void error_exit(const char *msg) {
	write(STDERR_FILENO, msg, strlen(msg));
	exit(EXIT_FAILURE);
}

static int make_listen_socket(int port) {
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		error_exit(fatal_error);
	}

	struct sockaddr_in servaddr = {0};

	// assign IP, PORT
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	servaddr.sin_port = htons(port);

	// Binding newly created socket to given address
	if (bind(sockfd, (const struct sockaddr*)&servaddr, sizeof(servaddr)) != 0) {
		error_exit(fatal_error);
	}

	if (listen(sockfd, SOMAXCONN) != 0) {
		error_exit(fatal_error);
	}
	return sockfd;
}

static void add_message_to_client(t_client *client,
										char *message, size_t message_length) {
	char *new_buff = realloc(client->sendbuffer,
								client->sendbuffer_length + message_length + 1);
	if (new_buff == NULL) {
		error_exit(fatal_error);
	}
	client->sendbuffer = new_buff;
	strcpy(&client->sendbuffer[client->sendbuffer_length], message);
	client->sendbuffer_length += message_length;
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

static void add_new_client(t_server *server) {
	int client_sd = accept(server->listen_sd, NULL, NULL);
	if (client_sd < 0) {
		error_exit(fatal_error);
	}
	t_client *client = calloc(1, sizeof(*client));
	if (client == NULL) {
		error_exit(fatal_error);
	}
	client->sd = client_sd;
	client->id = server->new_client_id;
	++server->new_client_id;

	server->clients[client_sd] = client;
	FD_SET(client_sd, &server->read_set);
	if (server->max_sd < client_sd) {
		server->max_sd = client_sd;
	}

	char buffer[SPRINTF_BUFFER_SIZE];
	int length = sprintf(buffer, client_arrive_fmt, client->id);
	add_message_to_other_clients(server, client, buffer, length);
}

static void set_max_sd(t_server *server, int sd) {
	if (server->max_sd == sd) {
		while (!FD_ISSET(server->max_sd, &server->read_set) &&
				!FD_ISSET(server->max_sd, &server->write_set)) {
			--server->max_sd;
		}
	}
}

static void remove_client(t_server *server, t_client *client) {
	char buffer[SPRINTF_BUFFER_SIZE];
	int length = sprintf(buffer, client_left_fmt, client->id);
	add_message_to_other_clients(server, client, buffer, length);

	int client_sd = client->sd;
	free(client->recvbuffer);
	free(client->sendbuffer);
	free(client);
	server->clients[client_sd] = NULL;
	FD_CLR(client_sd, &server->read_set);
	FD_CLR(client_sd, &server->write_set);
	set_max_sd(server, client_sd);
	if (close(client_sd) < 0) {
		error_exit(fatal_error);
	}
}

static void recv_data(t_server *server, t_client *client) {
	char buffer[RECV_BUFFER_SIZE + 1] = {0};

	ssize_t nbytes = recv(client->sd, buffer, RECV_BUFFER_SIZE, 0);
	if (nbytes < 0) {
		error_exit(fatal_error);
	}
	if (nbytes == 0) {
		remove_client(server, client);
	} else {
		char prefix_buffer[SPRINTF_BUFFER_SIZE];
		int prefix_len = sprintf(prefix_buffer, message_prefix_fmt, client->id);

		size_t message_length = prefix_len + nbytes;
		char *message = malloc(message_length + 1);
		if (message == NULL) {
			error_exit(fatal_error);
		}
		strcpy(message, prefix_buffer);
		strcpy(&message[prefix_len], buffer);
		add_message_to_other_clients(server, client, message, message_length);
		free(message);
	}
}

static void send_data(t_server *server, t_client *client) {
	ssize_t nbytes =
			send(client->sd, client->sendbuffer, client->sendbuffer_length, 0);
	if (nbytes < 0) {
		error_exit(fatal_error);
	}
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
	t_client **clients = server->clients;

	while (42) {
		fd_set read_set = server->read_set;
		fd_set write_set = server->write_set;
		int ready_sockets = select(server->max_sd + 1,
									&read_set, &write_set,
									NULL, NULL);
		if (ready_sockets < 0) {
			error_exit(fatal_error);
		}
		for (int sd = 0; sd <= server->max_sd && ready_sockets; ++sd) {
			if (FD_ISSET(sd, &read_set)) {
				if (sd == server->listen_sd) {
					add_new_client(server);
				} else {
					recv_data(server, clients[sd]);
				}
				--ready_sockets;
			} else if (FD_ISSET(sd, &write_set)) {
				send_data(server, clients[sd]);
				--ready_sockets;
			}
		}
	}
}

int main(int argc, char **argv) {
	if (argc != 2) {
		error_exit(wrong_argument_number);
	}
	t_server server = {0};
	int port = atoi(argv[1]);
	server.listen_sd = make_listen_socket(port);
	FD_SET(server.listen_sd, &server.read_set);
	server.max_sd = server.listen_sd;
	run_server(&server);
}

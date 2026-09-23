#include "chat.h"
#include "chat_server.h"

#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <unordered_set>
#include <arpa/inet.h> 
#include <pthread.h>
#include <sys/epoll.h>
#include <memory>
#include <queue>
#include <fcntl.h>

#define CLIENT_BUFFER_SIZE 1024
#define MAX_EVENTS 100

struct chat_peer {
	/** Client's socket. To read/write messages. */
	int socket;
	/** Output buffer. */
	std::string out_buffer;
	/* PUT HERE OTHER MEMBERS */
	std::string partial_input{};
	std::string name;
};

struct chat_server {
	/** Listening socket. To accept new clients. */
	int socket = -1;
	/** Array of peers. */
	std::unordered_set<chat_peer *> peers{};
	/* ... */
	/* PUT HERE OTHER MEMBERS */
	int epoll_desk{-1};
	std::queue<std::unique_ptr<chat_message>> output_buffer;
	std::queue<std::string> input_buffer;
	bool is_start_of_line = true; 
};

static void
close_server(chat_server *server)
{
	if(!server) return;
	for(auto& item : server->peers){
		if(item->socket == -1) continue;
		epoll_ctl(server->epoll_desk, EPOLL_CTL_DEL, item->socket, NULL);
		close(item->socket);
		delete item;
	}
	if(server->socket != -1) close(server->socket);
	if(server->epoll_desk != -1) close(server->epoll_desk);
}

#if NEED_AUTHOR
static void
sendDropClientMsg(chat_server *server, int id){
	char id_buf[8];
	snprintf(id_buf, sizeof(id_buf), "%04d", id);
	std::string data(1, static_cast<char>(chat_msg_type::DROP_CLIENT));
	data += std::string(id_buf) + '\n';

	for (chat_peer *item : server->peers) {
		item->out_buffer += data;
		struct epoll_event ev;
		memset(&ev, 0, sizeof(ev));
		ev.events = EPOLLIN | EPOLLOUT;
		ev.data.ptr = item;
		epoll_ctl(server->epoll_desk, EPOLL_CTL_MOD, item->socket, &ev);
	}
}
#endif

static void
close_peer(chat_server *server, chat_peer *peer)
{
	if(!peer) return;
	epoll_ctl(server->epoll_desk, EPOLL_CTL_DEL, peer->socket, NULL);
	server->peers.erase(peer);
#if NEED_AUTHOR
	sendDropClientMsg(server, peer->socket);
#endif
	delete peer;
}

struct chat_server *
chat_server_new(void)
{
	struct chat_server *server = new chat_server();
	return server;
}

void
chat_server_delete(struct chat_server *server)
{
	close_server(server);
	delete server;
}

/*
 * 1) Create a server socket (function socket()).
 * 2) Bind the server socket to addr (function bind()).
 * 3) Listen the server socket (function listen()).
 * 4) Create epoll/kqueue if needed.
 */
int
chat_server_listen(struct chat_server *server, uint16_t port)
{
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	server->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server->socket == -1) {
		return -1;
	}

	if (bind(server->socket, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
		return -1;
	}

	if (listen(server->socket, 128) == -1) {
		printf("listen error = %s\n", strerror(errno));
		return -1;
	}

	int flags = fcntl(server->socket, F_GETFL, 0);
	fcntl(server->socket, F_SETFL, flags | O_NONBLOCK);

	server->epoll_desk = epoll_create1(0);
	if (server->epoll_desk == -1) {
		close_server(server);
		return -1;
	}

	struct epoll_event new_ev;
	new_ev.data.ptr = NULL;
	new_ev.events = EPOLLIN;
	if (epoll_ctl(server->epoll_desk, EPOLL_CTL_ADD, server->socket, &new_ev) == -1) {
		close_server(server);
		return -1;
	}

	return 0;
}

struct chat_message *
chat_server_pop_next(struct chat_server *server)
{
	if(!server || server->output_buffer.empty()) return nullptr;
	auto* res = server->output_buffer.front().release();
	server->output_buffer.pop();
#if NEED_AUTHOR
	std::string tmp = res->data;
	if (!tmp.empty() && static_cast<chat_msg_type>(tmp[0]) == chat_msg_type::MESSAGE) {
		try {
			int id = std::stoi(tmp.substr(1, 4));
			for (chat_peer *p : server->peers) {
				if (p->socket == id) {
					res->author = p->name;
					break;
				}
			}
			res->data = tmp.substr(5);
		} catch (...) {}
	}
#endif
	return res;
}

static void
sendDataToClient(chat_server *server, chat_peer *client, const std::string& raw_msg)
{
	client->out_buffer += raw_msg;
	client->out_buffer.push_back('\n');

	struct epoll_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN | EPOLLOUT;
	ev.data.ptr = client;
	epoll_ctl(server->epoll_desk, EPOLL_CTL_MOD, client->socket, &ev);
}

static int
communicate(chat_server *server, chat_peer *p)
{
	size_t buf_size = 1024;
	char buf[buf_size];
	ssize_t sz = recv(p->socket, buf, buf_size, 0);
	if(sz < 0) return -1;
	if(sz == 0){
		errno = 0;
		return -1;
	}

	p->partial_input.append(buf, sz);
	size_t pos;
	while ((pos = p->partial_input.find('\n')) != std::string::npos) {
		std::string raw_msg = p->partial_input.substr(0, pos);
		
		auto msg = std::make_unique<chat_message>();
		msg->data = raw_msg;
		server->output_buffer.push(std::move(msg));

		for (chat_peer *other : server->peers) {
			if (other == p) continue; 
			sendDataToClient(server, other, raw_msg);
		}
		p->partial_input.erase(0, pos + 1);
	}
	return 0;
}

#if NEED_AUTHOR
static int
sendClientIdMsg(chat_peer *client){
	char id_buf[8];
	snprintf(id_buf, sizeof(id_buf), "%04d", client->socket);
	std::string data(1, static_cast<char>(chat_msg_type::CLIENT_ID));
	data += std::string(id_buf) + '\n';

	int sent = send(client->socket, data.c_str(), data.size(), 0);			
	if (sent < 0 && !(errno == EAGAIN || errno == EWOULDBLOCK)) {
		return -1;
	}
	return 0;
}

static void
sendNewClientMsg(chat_server *server, chat_peer *client){
	char id_buf[8];
	snprintf(id_buf, sizeof(id_buf), "%04d", client->socket);

	std::string data(1, static_cast<char>(chat_msg_type::NEW_CLIENT));
	data += std::string(id_buf) + client->name + '\n';

	for (chat_peer *item : server->peers) {
		if (item == client) continue;
		item->out_buffer += data;
		struct epoll_event ev;
		memset(&ev, 0, sizeof(ev));
		ev.events = EPOLLIN | EPOLLOUT;
		ev.data.ptr = item;
		epoll_ctl(server->epoll_desk, EPOLL_CTL_MOD, item->socket, &ev);
	}
}

static int
recieveClientName(chat_peer *client){
	size_t buf_size = 4096;
	char buf[buf_size];
	ssize_t sz = recv(client->socket, buf, buf_size, 0); 
	if (sz <= 1) {
		return -1;
	}
	// todo: что если имя больше размера буфера?
	client->name = std::string(buf + 1, sz - 1);
	return 0;
}
#endif

static int
createNewClient(int peer_sock, chat_server *server)
{
	chat_peer *p = new chat_peer;
	p->socket = peer_sock;

	epoll_event ev;
	ev.data.ptr = p;
	ev.events = EPOLLIN;
	if (epoll_ctl(server->epoll_desk, EPOLL_CTL_ADD, peer_sock, &ev) == -1) {
		printf("error = %s\n", strerror(errno));
		close(peer_sock);
		delete p;
		return -1;
	}

#if NEED_AUTHOR
	auto statId = sendClientIdMsg( p);
	if(statId < 0){
		close_peer(server, p);
		return -1;
	}
	auto statName = recieveClientName(p);
	if(statName < 0){
		close_peer(server, p);
		return -1;
	}
	
	for (chat_peer *existing : server->peers) {
		char old_id_buf[5];
		snprintf(old_id_buf, sizeof(old_id_buf), "%04d", existing->socket);
		std::string old_cli_packet(1, static_cast<char>(chat_msg_type::NEW_CLIENT));
		old_cli_packet += std::string(old_id_buf) + existing->name + '\n';
		p->out_buffer += old_cli_packet;
	}
	if (!p->out_buffer.empty()) {
		ev.events = EPOLLIN | EPOLLOUT;
		epoll_ctl(server->epoll_desk, EPOLL_CTL_MOD, p->socket, &ev);
	}

	sendNewClientMsg(server, p);
#endif

	server->peers.insert(p);
	int flags = fcntl(peer_sock, F_GETFL, 0);
	fcntl(peer_sock, F_SETFL, flags | O_NONBLOCK);
	return 0;
}

static void
handleNewClientConnection(chat_server *server, bool *has_data)
{
	while (true) {
		int peer_sock = accept(server->socket, NULL, NULL);
		if (peer_sock == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				break; 
			}
			printf("error = %s\n", strerror(errno));
			break;
		}
		
		auto status = createNewClient(peer_sock, server);
		if(status < 0) break;
		*has_data = true;
	}
}

static void
sendClientsDataToClients(chat_server *server, chat_peer *p, bool *has_data, bool *drop_client)
{
	ssize_t sent = send(p->socket, p->out_buffer.c_str(), p->out_buffer.size(), 0);
	if (sent > 0) {
		*has_data = true;
		p->out_buffer.erase(0, sent);
	} else if (sent == -1 && errno != EAGAIN && errno != EWOULDBLOCK) *drop_client = true;

	if(p->out_buffer.empty() && !*drop_client){
		struct epoll_event ev;
		memset(&ev, 0, sizeof(ev));
		ev.events = EPOLLIN;
		ev.data.ptr = p;
		epoll_ctl(server->epoll_desk, EPOLL_CTL_MOD, p->socket, &ev);
	}
}

static void
sendServerDataToClients(chat_server *server)
{
	while (!server->input_buffer.empty()) {
		std::string fed_msg = std::move(server->input_buffer.front());
		server->input_buffer.pop();
		
		if (!server->peers.empty()) {
			for (chat_peer *item : server->peers) {
				item->out_buffer.append(fed_msg);
				struct epoll_event ev;
				memset(&ev, 0, sizeof(ev));
				ev.events = EPOLLIN | EPOLLOUT;
				ev.data.ptr = item;
				epoll_ctl(server->epoll_desk, EPOLL_CTL_MOD, item->socket, &ev);
			}
		}
	}
}

/*
 * 1) Wait on epoll/kqueue/poll for update on any socket.
 * 2) Handle the update.
 * 2.1) If the update was on listen-socket, then you probably need to
 *     call accept() on it - a new client wants to join.
 * 2.2) If the update was on a client-socket, then you might want to
 *     read/write on it.
 */
int
chat_server_update(struct chat_server *server, double timeout)
{
	if(!server || server->socket == -1) return CHAT_ERR_NOT_STARTED;

	int ms = timeout * 1000;
	struct epoll_event events[MAX_EVENTS];
	int nfds = epoll_wait(server->epoll_desk, events, MAX_EVENTS, ms);
	if (nfds == -1) {
		return CHAT_ERR_SYS;
	}
	bool has_data{};

	for(int i = 0; i < nfds; ++i){
		if (events[i].data.ptr == NULL) {
			handleNewClientConnection(server, &has_data);
			continue;
		}

		chat_peer *p = static_cast<chat_peer *>(events[i].data.ptr);
		bool drop_client{};

		if(events[i].events & EPOLLIN){
			int rc = communicate(server, p);
			if (rc == -1) {
				if (errno != EWOULDBLOCK && errno != EAGAIN) {
					printf("error = %s\n", strerror(errno));
					drop_client = true;
				}
			} else {
				has_data = true;
			}
		}

		if(!drop_client && !p->out_buffer.empty()){
			sendClientsDataToClients(server, p, &has_data, &drop_client);
		}

		if(drop_client) {
			close_peer(server, p);
			has_data = true;
		}
	}

	sendServerDataToClients(server);

	if (nfds == 0 && !has_data) {
		return CHAT_ERR_TIMEOUT;
	}
	
	return 0;
}

/*
 * Server has multiple sockets - own and from connected clients. Hence
 * you can't return a socket here. But if you are using epoll/kqueue,
 * then you can return their descriptor. These descriptors can be polled
 * just like sockets and will return an event when any of their owned
 * descriptors has any events.
 *
 * For example, assume you created an epoll descriptor and added to
 * there a listen-socket and a few client-sockets. Now if you will call
 * poll() on the epoll's descriptor, then on return from poll() you can
 * be sure epoll_wait() can return something useful for some of those
 * sockets.
 */
int
chat_server_get_descriptor(const struct chat_server *server)
{
	if(!server) return -1;
	return server->epoll_desk;
}

int
chat_server_get_socket(const struct chat_server *server)
{
	return server->socket;
}

int
chat_server_get_events(const struct chat_server *server)
{
	if(!server || server->epoll_desk == -1) return 0;
	return CHAT_EVENT_INPUT;
}

int
chat_server_feed(struct chat_server *server, const char *msg, uint32_t msg_size)
{
	if(!server || server->epoll_desk == -1) return CHAT_ERR_NOT_STARTED;
	if(!msg || msg_size <= 0) return 0;

#if NEED_AUTHOR
	std::string buf(msg, msg_size);
	size_t start_pos = 0;
	size_t pos;
	std::string full_packet = "";

	while ((pos = buf.find('\n', start_pos)) != std::string::npos) {
		if (server->is_start_of_line) {
			full_packet += static_cast<char>(chat_msg_type::MESSAGE) + std::to_string(1);
		}
		
		full_packet += buf.substr(start_pos, pos - start_pos) + "\n";
		server->is_start_of_line = true;
		start_pos = pos + 1;
	}
	if (start_pos < buf.size()) {
		if (server->is_start_of_line) {
			full_packet += static_cast<char>(chat_msg_type::MESSAGE) + std::to_string(1);
		}
		full_packet += buf.substr(start_pos);
		server->is_start_of_line = false;
	}

	server->input_buffer.push(full_packet);
#else
	std::string full_packet(msg, msg_size);
	server->input_buffer.push(full_packet);
#endif

	return 0;
}

#include "chat.h"
#include "chat_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <memory>
#include <netinet/in.h>
#include <queue>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

enum {
	FRAME_HEADER_SIZE = 9,
	IO_BUFFER_SIZE = 64 * 1024,
	EPOLL_EVENT_LIMIT = 100,
};

struct frame {
	chat_msg_type type{};
	uint32_t id{};
	std::string payload;
};

static void
write_u32(char *destination, uint32_t value)
{
	uint32_t network_value = htonl(value);
	memcpy(destination, &network_value, sizeof(network_value));
}

static uint32_t
read_u32(const char *source)
{
	uint32_t network_value{};
	memcpy(&network_value, source, sizeof(network_value));
	return ntohl(network_value);
}

static bool
append_frame(std::string *buffer, chat_msg_type type, uint32_t id,
	std::string_view payload)
{
	if (payload.size() > UINT32_MAX)
		return false;

	size_t old_size = buffer->size();
	buffer->resize(old_size + FRAME_HEADER_SIZE + payload.size());
	char *header = buffer->data() + old_size;
	header[0] = static_cast<char>(type);
	write_u32(header + 1, id);
	write_u32(header + 5, static_cast<uint32_t>(payload.size()));
	memcpy(header + FRAME_HEADER_SIZE, payload.data(), payload.size());
	return true;
}

static bool
take_frame(std::string *buffer, frame *result)
{
	if (buffer->size() < FRAME_HEADER_SIZE)
		return false;

	uint32_t payload_size = read_u32(buffer->data() + 5);
	if (payload_size > buffer->size() - FRAME_HEADER_SIZE)
		return false;

	result->type = static_cast<chat_msg_type>(
		static_cast<unsigned char>((*buffer)[0]));
	result->id = read_u32(buffer->data() + 1);
	result->payload.assign(buffer->data() + FRAME_HEADER_SIZE, payload_size);
	buffer->erase(0, FRAME_HEADER_SIZE + payload_size);
	return true;
}

static std::string_view
trim_message(std::string_view message)
{
	size_t first = 0;
	while (first < message.size() &&
		std::isspace(static_cast<unsigned char>(message[first])))
		++first;

	size_t last = message.size();
	while (last > first &&
		std::isspace(static_cast<unsigned char>(message[last - 1])))
		--last;

	return message.substr(first, last - first);
}

static int
set_nonblocking(int socket)
{
	int flags = fcntl(socket, F_GETFL, 0);
	if (flags == -1)
		return -1;
	return fcntl(socket, F_SETFL, flags | O_NONBLOCK);
}

static int
timeout_to_milliseconds(double timeout)
{
	if (timeout < 0.0)
		return -1;
	if (timeout >= static_cast<double>(INT32_MAX) / 1000.0)
		return INT32_MAX;
	return static_cast<int>(timeout * 1000.0);
}

} // namespace

struct chat_peer {
	int socket = -1;
	uint32_t id = 0;
	std::string name;
	std::string input_buffer;
	std::string output_buffer;
	std::vector<std::pair<uint32_t, std::string>> pending_messages;
	bool name_received = false;
};

struct chat_server {
	int socket = -1;
	std::unordered_set<chat_peer *> peers;
	int epoll_descriptor = -1;
	std::queue<std::unique_ptr<chat_message>> output_buffer;
	std::queue<std::string> server_messages;
	std::string partial_server_feed;
	uint32_t next_client_id = 1;
};

static void
close_server_resources(chat_server *server)
{
	if (!server)
		return;
	for (chat_peer *peer : server->peers) {
		if (server->epoll_descriptor != -1)
			epoll_ctl(server->epoll_descriptor, EPOLL_CTL_DEL,
				peer->socket, nullptr);
		if (peer->socket != -1)
			close(peer->socket);
		delete peer;
	}
	server->peers.clear();
	if (server->socket != -1) {
		close(server->socket);
		server->socket = -1;
	}
	if (server->epoll_descriptor != -1) {
		close(server->epoll_descriptor);
		server->epoll_descriptor = -1;
	}
}

struct chat_server *
chat_server_new(void)
{
	return new chat_server();
}

void
chat_server_delete(struct chat_server *server)
{
	if (!server)
		return;
	close_server_resources(server);
	delete server;
}

int
chat_server_listen(struct chat_server *server, uint16_t port)
{
	if (!server)
		return CHAT_ERR_INVALID_ARGUMENT;
	if (server->socket != -1)
		return CHAT_ERR_ALREADY_STARTED;

	server->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server->socket == -1)
		return CHAT_ERR_SYS;

	int reuse = 1;
	(void)setsockopt(server->socket, SOL_SOCKET, SO_REUSEADDR,
		&reuse, sizeof(reuse));
	if (set_nonblocking(server->socket) == -1) {
		close_server_resources(server);
		return CHAT_ERR_SYS;
	}

	struct sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(server->socket, reinterpret_cast<struct sockaddr *>(&address),
		sizeof(address)) != 0) {
		int error = errno;
		close_server_resources(server);
		return error == EADDRINUSE ? CHAT_ERR_PORT_BUSY : CHAT_ERR_SYS;
	}
	if (listen(server->socket, 128) == -1) {
		close_server_resources(server);
		return CHAT_ERR_SYS;
	}

	server->epoll_descriptor = epoll_create1(0);
	if (server->epoll_descriptor == -1) {
		close_server_resources(server);
		return CHAT_ERR_SYS;
	}

	struct epoll_event event{};
	event.events = EPOLLIN | EPOLLET;
	event.data.ptr = nullptr;
	if (epoll_ctl(server->epoll_descriptor, EPOLL_CTL_ADD,
		server->socket, &event) == -1) {
		close_server_resources(server);
		return CHAT_ERR_SYS;
	}
	return 0;
}

struct chat_message *
chat_server_pop_next(struct chat_server *server)
{
	if (!server || server->output_buffer.empty())
		return nullptr;
	chat_message *result = server->output_buffer.front().release();
	server->output_buffer.pop();
	return result;
}

static bool
queue_frame(chat_peer *peer, chat_msg_type type, uint32_t id,
	std::string_view payload)
{
	return append_frame(&peer->output_buffer, type, id, payload);
}

#if NEED_AUTHOR
static void
queue_drop_notification(chat_server *server, uint32_t dropped_id)
{
	for (chat_peer *peer : server->peers) {
		if (peer->name_received)
			queue_frame(peer, DROP_CLIENT, dropped_id, {});
	}
}
#endif

static void
close_peer(chat_server *server, chat_peer *peer)
{
	if (!peer)
		return;
	server->peers.erase(peer);
	if (server->epoll_descriptor != -1)
		epoll_ctl(server->epoll_descriptor, EPOLL_CTL_DEL,
			peer->socket, nullptr);
#if NEED_AUTHOR
	if (peer->name_received)
		queue_drop_notification(server, peer->id);
#endif
	if (peer->socket != -1)
		close(peer->socket);
	delete peer;
}

static void
broadcast_message(chat_server *server, chat_peer *sender,
	std::string_view body)
{
	body = trim_message(body);
	if (body.empty())
		return;

	auto message = std::make_unique<chat_message>();
	message->data.assign(body.data(), body.size());
#if NEED_AUTHOR
	message->msg_type = MESSAGE;
	message->author = sender ? sender->name : "server";
#endif
	server->output_buffer.push(std::move(message));

	uint32_t author_id = sender ? sender->id : 0;
	std::vector<chat_peer *> failed_peers;
	for (chat_peer *peer : server->peers) {
		if (peer == sender)
			continue;
		if (!peer->name_received) {
			peer->pending_messages.emplace_back(author_id,
				std::string(body));
			continue;
		}
		if (!queue_frame(peer, MESSAGE, author_id, body))
			failed_peers.push_back(peer);
	}
	for (chat_peer *peer : failed_peers)
		close_peer(server, peer);
}

static int
handle_peer_frame(chat_server *server, chat_peer *peer,
	const frame& incoming)
{
#if NEED_AUTHOR
	if (!peer->name_received) {
		if (incoming.type != CLIENT_NAME)
			return -1;
		peer->name = incoming.payload;
		peer->name_received = true;
		for (chat_peer *other : server->peers) {
			if (other == peer || !other->name_received)
				continue;
			if (!queue_frame(peer, NEW_CLIENT, other->id, other->name) ||
				!queue_frame(other, NEW_CLIENT, peer->id, peer->name))
				return -1;
		}
		for (const auto& pending : peer->pending_messages) {
			if (!queue_frame(peer, MESSAGE, pending.first, pending.second))
				return -1;
		}
		peer->pending_messages.clear();
		return 0;
	}
#endif

	if (incoming.type != MESSAGE)
		return -1;
	broadcast_message(server, peer, incoming.payload);
	return 0;
}

static int
parse_peer_input(chat_server *server, chat_peer *peer)
{
	frame incoming;
	while (take_frame(&peer->input_buffer, &incoming)) {
		if (handle_peer_frame(server, peer, incoming) < 0)
			return -1;
	}
	return 0;
}

static int
receive_from_peer(chat_server *server, chat_peer *peer, bool *has_data)
{
	char buffer[IO_BUFFER_SIZE];
	while (true) {
		ssize_t received = recv(peer->socket, buffer, sizeof(buffer), 0);
		if (received > 0) {
			*has_data = true;
			peer->input_buffer.append(buffer, static_cast<size_t>(received));
			continue;
		}
		if (received == 0)
			return 1;
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			break;
		return -1;
	}
	return parse_peer_input(server, peer) < 0 ? -1 : 0;
}

static int
flush_peer(chat_peer *peer, bool *has_data)
{
	while (!peer->output_buffer.empty()) {
		ssize_t sent = send(peer->socket, peer->output_buffer.data(),
			peer->output_buffer.size(), 0);
		if (sent > 0) {
			*has_data = true;
			peer->output_buffer.erase(0, static_cast<size_t>(sent));
			continue;
		}
		if (sent == -1 && errno == EINTR)
			continue;
		if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		return -1;
	}
	return 0;
}

static int
create_peer(chat_server *server, int peer_socket)
{
	if (set_nonblocking(peer_socket) == -1) {
		close(peer_socket);
		return -1;
	}

	auto *peer = new chat_peer();
	peer->socket = peer_socket;
	peer->id = server->next_client_id++;
	if (peer->id == 0)
		peer->id = server->next_client_id++;

	struct epoll_event event{};
	event.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
	event.data.ptr = peer;
	if (epoll_ctl(server->epoll_descriptor, EPOLL_CTL_ADD,
		peer_socket, &event) == -1) {
		close(peer_socket);
		delete peer;
		return -1;
	}

#if NEED_AUTHOR
	if (!queue_frame(peer, CLIENT_ID, peer->id, {})) {
		epoll_ctl(server->epoll_descriptor, EPOLL_CTL_DEL,
			peer_socket, nullptr);
		close(peer_socket);
		delete peer;
		return -1;
	}
	for (chat_peer *other : server->peers) {
		if (other->name_received &&
			!queue_frame(peer, NEW_CLIENT, other->id, other->name)) {
			epoll_ctl(server->epoll_descriptor, EPOLL_CTL_DEL,
				peer_socket, nullptr);
			close(peer_socket);
			delete peer;
			return -1;
		}
	}
#else
	peer->name_received = true;
#endif

	server->peers.insert(peer);
	return 0;
}

static void
accept_all(chat_server *server, bool *has_data)
{
	while (true) {
		int peer_socket = accept(server->socket, nullptr, nullptr);
		if (peer_socket == -1) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			return;
		}
		if (create_peer(server, peer_socket) == 0)
			*has_data = true;
	}
}

static void
process_server_messages(chat_server *server, bool *has_data)
{
	while (!server->server_messages.empty()) {
		std::string message = std::move(server->server_messages.front());
		server->server_messages.pop();
		broadcast_message(server, nullptr, message);
		*has_data = true;
	}
}

int
chat_server_update(struct chat_server *server, double timeout)
{
	if (!server || server->socket == -1)
		return CHAT_ERR_NOT_STARTED;

	struct epoll_event events[EPOLL_EVENT_LIMIT];
	int event_count = epoll_wait(server->epoll_descriptor, events,
		EPOLL_EVENT_LIMIT, timeout_to_milliseconds(timeout));
	if (event_count == -1) {
		if (errno == EINTR)
			return CHAT_ERR_TIMEOUT;
		return CHAT_ERR_SYS;
	}

	bool has_data = false;
	std::unordered_set<chat_peer *> to_drop;
	for (int index = 0; index < event_count; ++index) {
		if (events[index].data.ptr == nullptr) {
			accept_all(server, &has_data);
			continue;
		}

		auto *peer = static_cast<chat_peer *>(events[index].data.ptr);
		if (server->peers.find(peer) == server->peers.end() ||
			to_drop.find(peer) != to_drop.end())
			continue;

		uint32_t flags = events[index].events;
		if (flags & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
			to_drop.insert(peer);
		if (to_drop.find(peer) == to_drop.end() && (flags & EPOLLIN)) {
			int status = receive_from_peer(server, peer, &has_data);
			if (status != 0)
				to_drop.insert(peer);
		}
		if (to_drop.find(peer) == to_drop.end() && (flags & EPOLLOUT)) {
			if (flush_peer(peer, &has_data) < 0)
				to_drop.insert(peer);
		}
	}

	process_server_messages(server, &has_data);
	for (chat_peer *peer : to_drop)
		close_peer(server, peer);

	std::vector<chat_peer *> output_errors;
	for (chat_peer *peer : server->peers) {
		if (!peer->output_buffer.empty() &&
			flush_peer(peer, &has_data) < 0)
			output_errors.push_back(peer);
	}
	for (chat_peer *peer : output_errors)
		close_peer(server, peer);

	if (event_count == 0 && !has_data)
		return CHAT_ERR_TIMEOUT;
	return 0;
}

int
chat_server_get_descriptor(const struct chat_server *server)
{
	return server ? server->epoll_descriptor : -1;
}

int
chat_server_get_socket(const struct chat_server *server)
{
	return server ? server->socket : -1;
}

int
chat_server_get_events(const struct chat_server *server)
{
	if (!server || server->epoll_descriptor == -1)
		return 0;
	return CHAT_EVENT_INPUT;
}

int
chat_server_feed(struct chat_server *server, const char *msg, uint32_t msg_size)
{
	if (!server || server->socket == -1)
		return CHAT_ERR_NOT_STARTED;
	if (!msg || msg_size == 0)
		return 0;

	server->partial_server_feed.append(msg, msg_size);
	size_t newline;
	while ((newline = server->partial_server_feed.find('\n')) !=
		std::string::npos) {
		std::string_view line(server->partial_server_feed.data(), newline);
		line = trim_message(line);
		if (!line.empty())
			server->server_messages.emplace(line);
		server->partial_server_feed.erase(0, newline + 1);
	}
	return 0;
}

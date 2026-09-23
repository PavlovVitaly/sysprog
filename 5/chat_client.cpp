#include "chat.h"
#include "chat_client.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <memory>
#include <netdb.h>
#include <poll.h>
#include <queue>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>

namespace {

enum {
	FRAME_HEADER_SIZE = 9,
	IO_BUFFER_SIZE = 4 * 1024,
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

} // namespace

struct chat_client {
	/** Socket connected to the server. */
	int socket = -1;
	/** Messages already parsed from the server. */
	std::queue<std::unique_ptr<chat_message>> input_buffer;
	/** Bytes waiting to be sent. */
	std::string output_buffer;
	/** Bytes received but not parsed into complete frames. */
	std::string partial_input;
	/** Bytes supplied to chat_client_feed but not ending in a newline. */
	std::string partial_feed;
	std::string name;
	uint32_t id = 0;
#if NEED_AUTHOR
	std::unordered_map<uint32_t, std::string> other_clients;
#endif
};

struct chat_client *
chat_client_new(std::string_view name)
{
	chat_client *client = new chat_client();
	client->name = name;
	return client;
}

void
chat_client_delete(struct chat_client *client)
{
	if (!client)
		return;
	if (client->socket >= 0)
		close(client->socket);
	delete client;
}

void
parse_addr(std::string_view addr, std::string& ip, std::string& port)
{
	auto pos = addr.find(':');
	if (pos != std::string_view::npos) {
		ip = addr.substr(0, pos);
		port = addr.substr(pos + 1);
		return;
	}
	ip = addr;
	port = "80";
}

static int
parse_data(chat_client *client)
{
	frame incoming;
	while (take_frame(&client->partial_input, &incoming)) {
		switch (incoming.type) {
		case MESSAGE: {
			auto message = std::make_unique<chat_message>();
			message->data = std::move(incoming.payload);
#if NEED_AUTHOR
			message->msg_type = MESSAGE;
			if (incoming.id == 0) {
				message->author = "server";
			} else {
				auto it = client->other_clients.find(incoming.id);
				if (it != client->other_clients.end())
					message->author = it->second;
			}
#endif
			client->input_buffer.push(std::move(message));
			break;
		}
#if NEED_AUTHOR
		case CLIENT_ID:
			client->id = incoming.id;
			break;
		case NEW_CLIENT:
			client->other_clients[incoming.id] = std::move(incoming.payload);
			break;
		case DROP_CLIENT:
			client->other_clients.erase(incoming.id);
			break;
		case CLIENT_NAME:
			return -1;
#endif
		default:
			return -1;
		}
	}
	return 0;
}

int
chat_client_connect(struct chat_client *client, std::string_view addr)
{
	if (!client)
		return CHAT_ERR_INVALID_ARGUMENT;
	if (client->socket != -1)
		return CHAT_ERR_ALREADY_STARTED;

	std::string ip;
	std::string port;
	parse_addr(addr, ip, port);
	if (ip.empty() || port.empty())
		return CHAT_ERR_INVALID_ARGUMENT;

	struct addrinfo *ainfo = nullptr;
	struct addrinfo filter{};
	filter.ai_family = AF_INET;
	filter.ai_socktype = SOCK_STREAM;

	int status = getaddrinfo(ip.c_str(), port.c_str(), &filter, &ainfo);
	if (status != 0)
		return CHAT_ERR_NO_ADDR;

	client->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (client->socket == -1) {
		freeaddrinfo(ainfo);
		return CHAT_ERR_SYS;
	}

	int rc = connect(client->socket, ainfo->ai_addr, ainfo->ai_addrlen);
	freeaddrinfo(ainfo);
	if (rc == -1) {
		close(client->socket);
		client->socket = -1;
		return CHAT_ERR_SYS;
	}

	if (set_nonblocking(client->socket) == -1) {
		close(client->socket);
		client->socket = -1;
		return CHAT_ERR_SYS;
	}

#if NEED_AUTHOR
	if (!append_frame(&client->output_buffer, CLIENT_NAME, 0, client->name)) {
		close(client->socket);
		client->socket = -1;
		return CHAT_ERR_INVALID_ARGUMENT;
	}
	/* Start sending the handshake now, but keep any unsent suffix queued. */
	ssize_t sent;
	do {
		sent = send(client->socket, client->output_buffer.data(),
			client->output_buffer.size(), 0);
	} while (sent == -1 && errno == EINTR);
	if (sent > 0)
		client->output_buffer.erase(0, static_cast<size_t>(sent));
	else if (sent == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
		close(client->socket);
		client->socket = -1;
		return CHAT_ERR_SYS;
	}
#endif
	return 0;
}

struct chat_message *
chat_client_pop_next(struct chat_client *client)
{
	if (!client || client->input_buffer.empty())
		return nullptr;
	chat_message *result = client->input_buffer.front().release();
	client->input_buffer.pop();
	return result;
}

static int
send_pending(chat_client *client, bool *has_data)
{
	while (!client->output_buffer.empty()) {
		ssize_t sent = send(client->socket, client->output_buffer.data(),
			client->output_buffer.size(), 0);
		if (sent > 0) {
			*has_data = true;
			client->output_buffer.erase(0, static_cast<size_t>(sent));
			continue;
		}
		if (sent == -1 && (errno == EINTR))
			continue;
		if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		return -1;
	}
	return 0;
}

static int
receive_available(chat_client *client, bool *has_data)
{
	char buffer[IO_BUFFER_SIZE];
	bool closed = false;
	while (true) {
		ssize_t received = recv(client->socket, buffer, sizeof(buffer), 0);
		if (received > 0) {
			*has_data = true;
			client->partial_input.append(buffer,
				static_cast<size_t>(received));
			continue;
		}
		if (received == 0) {
			closed = true;
			break;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			break;
		return -1;
	}

	if (parse_data(client) < 0)
		return -1;
	if (closed) {
		close(client->socket);
		client->socket = -1;
		return -1;
	}
	return 0;
}

int
chat_client_update(struct chat_client *client, double timeout)
{
	if (!client || client->socket == -1)
		return CHAT_ERR_NOT_STARTED;

	struct pollfd descriptor{};
	descriptor.fd = client->socket;
	descriptor.events = POLLIN;
	if (!client->output_buffer.empty())
		descriptor.events |= POLLOUT;

	int timeout_ms;
	if (timeout < 0.0) {
		timeout_ms = -1;
	} else if (timeout >= static_cast<double>(INT32_MAX) / 1000.0) {
		timeout_ms = INT32_MAX;
	} else {
		timeout_ms = static_cast<int>(timeout * 1000.0);
	}

	int status = poll(&descriptor, 1, timeout_ms);
	if (status == -1) {
		if (errno == EINTR)
			return CHAT_ERR_TIMEOUT;
		return CHAT_ERR_SYS;
	}

	bool has_data = false;
	if (descriptor.revents & POLLOUT) {
		if (send_pending(client, &has_data) < 0)
			return CHAT_ERR_SYS;
	}
	if (descriptor.revents & (POLLIN | POLLHUP)) {
		if (receive_available(client, &has_data) < 0)
			return CHAT_ERR_SYS;
	}
	if (descriptor.revents & (POLLERR | POLLNVAL))
		return CHAT_ERR_SYS;
	if (status == 0 && !has_data)
		return CHAT_ERR_TIMEOUT;
	return 0;
}

int
chat_client_get_descriptor(const struct chat_client *client)
{
	return client ? client->socket : -1;
}

int
chat_client_get_events(const struct chat_client *client)
{
	if (!client || client->socket == -1)
		return 0;
	int result = CHAT_EVENT_INPUT;
	if (!client->output_buffer.empty())
		result |= CHAT_EVENT_OUTPUT;
	return result;
}

int
chat_client_feed(struct chat_client *client, const char *msg, uint32_t msg_size)
{
	if (!client || client->socket == -1)
		return CHAT_ERR_NOT_STARTED;
	if (!msg || msg_size == 0)
		return 0;

	client->partial_feed.append(msg, msg_size);
	size_t newline;
	while ((newline = client->partial_feed.find('\n')) != std::string::npos) {
		std::string_view line(client->partial_feed.data(), newline);
		line = trim_message(line);
		if (!line.empty() &&
			!append_frame(&client->output_buffer, MESSAGE, 0, line))
			return CHAT_ERR_INVALID_ARGUMENT;
		client->partial_feed.erase(0, newline + 1);
	}
	return 0;
}

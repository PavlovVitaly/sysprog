#include "chat.h"
#include "chat_client.h"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <stdlib.h>
#include <string_view>
#include <sys/poll.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <memory>
#include <queue>
#include <fcntl.h>

struct chat_client {
	/** Socket connected to the server. */
	int socket = -1;
	/** Array of received messages. */
	std::queue<std::unique_ptr<chat_message>> input_buffer;
	/** Output buffer. */
	std::queue<std::unique_ptr<chat_message>> output_buffer;
	/* ... */
	/* PUT HERE OTHER MEMBERS */
	std::string name{};
	std::string partial_input{}; 
	bool is_start_of_line = true; 
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
	if (client->socket >= 0)
		close(client->socket);
	while(!client->output_buffer.empty()){
		client->output_buffer.front().reset();
		client->output_buffer.pop();
	}
	while(!client->input_buffer.empty()){
		client->input_buffer.front().reset();
		client->input_buffer.pop();
	}
	delete client;
}

void parse_addr(std::string_view addr, std::string& ip, std::string& port){
	auto pos = addr.find(':');
	if(pos != std::string_view::npos){
		ip = addr.substr(0, pos);
		port = addr.substr(pos + 1);
		return;
	}
	ip = addr;
	port = "80";
}

int
chat_client_connect(struct chat_client *client, std::string_view addr)
{
	/*
	 * 1) Use getaddrinfo() to resolve addr to struct sockaddr_in.
	 * 2) Create a client socket (function socket()).
	 * 3) Connect it by the found address (function connect()).
	 */
	std::string ip{};
	std::string port{};
	parse_addr(addr,ip, port);

	struct addrinfo *ainfo{};
	struct addrinfo filter;
	memset(&filter, 0, sizeof(struct addrinfo));
	filter.ai_family = AF_INET;
	filter.ai_socktype = SOCK_STREAM;

	int status = getaddrinfo(ip.c_str()
						, port.c_str()
						, &filter
						, &ainfo);
	if(status != 0){
		return CHAT_ERR_NO_ADDR;
	}

	client->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(client->socket == -1){
		return -1;
	}

	int rc = connect(client->socket, ainfo->ai_addr, ainfo->ai_addrlen);
	freeaddrinfo(ainfo);

	if (rc == -1) {
		close(client->socket);
		client->socket = -1;
		return -1; // или CHAT_ERR_SYS
	}

	int flags = fcntl(client->socket, F_GETFL, 0);
	fcntl(client->socket, F_SETFL, flags | O_NONBLOCK);

	return 0;
}

struct chat_message *
chat_client_pop_next(struct chat_client *client)
{
	if(!client || client->input_buffer.empty()) return nullptr;
	auto res = client->input_buffer.front().release();
	client->input_buffer.pop();
#if NEED_AUTHOR
	auto tmp = res->data;
	size_t pos{};
	if ((pos = tmp.find(":%:")) != std::string::npos){
		res->author = tmp.substr(0, pos);
		res->data = tmp.substr(pos + 3);
	}
#endif
	return res;
}

int
chat_client_update(struct chat_client *client, double timeout)
{
	/*
	 * The easiest way to wait for updates on a single socket with a timeout
	 * is to use poll(). Epoll is good for many sockets, poll is good for a
	 * few.
	 *
	 * You create one struct pollfd, fill it, call poll() on it, handle the
	 * events (do read/write).
	 */

	if(!client || client->socket == -1) return CHAT_ERR_NOT_STARTED;
	pollfd fd;
	fd.fd = client->socket;
	fd.events = POLLIN;
	if(!client->output_buffer.empty()){
		fd.events |= POLLOUT;
	}

	int ms = timeout * 1000;

	int status = poll(&fd, 1, ms);
	if(status == -1) return -1;
	bool has_data{};

	if(fd.revents & POLLOUT){
		while(!client->output_buffer.empty()){
			auto& data = client->output_buffer.front();
			if (data->data.empty()) {
				client->output_buffer.pop();
				continue;
			}

			int sent = send(client->socket, data->data.c_str(), data->data.size(), 0);			
			if (sent > 0) {
				has_data = true;	
				if (static_cast<size_t>(sent) == data->data.size()) {
					client->output_buffer.pop();
				} else {
					data->data.erase(0, sent); 
					break; 
				}
			} else {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					break; 
				}
				return -1;
			}
		}
	}

	if(fd.revents & POLLIN){
		size_t buf_size = 4096;
		char buf[buf_size];
		ssize_t sz = recv(client->socket, buf, buf_size, 0);
		while(sz > 0){
			has_data = true;
			client->partial_input.append(buf, sz);
			sz = recv(client->socket, buf, buf_size, 0);
		}
		size_t pos;
		while ((pos = client->partial_input.find('\n')) != std::string::npos) {
			auto msg = std::make_unique<chat_message>();
			msg->data = client->partial_input.substr(0, pos);
			client->input_buffer.push(std::move(msg));
			client->partial_input.erase(0, pos + 1);
		}

		if(sz < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
			return -1;
		}
		if(sz == 0){
			close(client->socket);
			client->socket = -1;
			return -1;
		}
	}

	if (status == 0 && !has_data && client->output_buffer.empty()) {
		return CHAT_ERR_TIMEOUT;
	}
	
	return 0;
}

int
chat_client_get_descriptor(const struct chat_client *client)
{
	return client->socket;
}

int
chat_client_get_events(const struct chat_client *client)
{
	if(!client || client->socket == -1) return 0;
	int res = CHAT_EVENT_INPUT;
	if(!client->output_buffer.empty()) res |= CHAT_EVENT_OUTPUT;
	return res;
}

int
chat_client_feed(struct chat_client *client, const char *msg, uint32_t msg_size)
{
	if(!client || client->socket == -1) return CHAT_ERR_NOT_STARTED;
	if(!msg || msg_size == 0) return 0;

#if NEED_AUTHOR	
	std::string buf(msg, msg_size);
	size_t start_pos = 0;
	size_t pos;

	auto data = std::make_unique<chat_message>();
	data->author = client->name;
	data->data = "";

	while ((pos = buf.find('\n', start_pos)) != std::string::npos) {
		if (client->is_start_of_line) {
			data->data += client->name + ":%:";
		}
		
		data->data += buf.substr(start_pos, pos - start_pos) + "\n";
		client->is_start_of_line = true;
		start_pos = pos + 1;
	}
	if (start_pos < buf.size()) {
		if (client->is_start_of_line) {
			data->data += client->name + ":%:";
		}
		data->data += buf.substr(start_pos);
		client->is_start_of_line = false;
	}

	client->output_buffer.push(std::move(data));
#else
	auto data = std::make_unique<chat_message>();
	data->data = std::string(msg, msg_size);
	client->output_buffer.push(std::move(data));
#endif

	return 0;
}

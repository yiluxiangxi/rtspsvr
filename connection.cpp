#include "connection.h"
#include <sys/epoll.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define EPOLLNONE 0x0



int init_packet_buf(packet_buf & s,int size)
{
	if(!s.buf){
	s.buf = (char*)malloc(size);
	s.has = 0;
	s.len = size;
	}
	if(!s.buf){
		return -1;
	}
	return 0;
}

void release_packet_buf(packet_buf & s)
{
	if(s.buf){
		free(s.buf);
		s.buf = 0;
	}
	s.has = 0;
	s.len = 0;
}

void inet_ntoa_convert(ipaddr & dst, const struct sockaddr_in & addr)
{
	unsigned char *p = (unsigned char*)&addr.sin_addr;
	sprintf(dst.ip,"%d.%d.%d.%d", p[0], p[1], p[2], p[3]);
	dst.port = ntohs(addr.sin_port);
}


connection::connection(int epfd, int fd)
{
	m_appid = 0;
	m_fd = fd;
	m_epfd = epfd;
	m_context = NULL;
	m_list_node = NULL;
	m_events = EPOLLNONE;
	m_status = kdisconnected;
	m_operation = knew;
	m_ref = 0;
	m_alive_time  = 0;
	memset(&m_send_buf, 0, sizeof(m_send_buf));
	memset(&m_recv_buf, 0, sizeof(m_recv_buf));
	memset(&m_localaddr,0, sizeof(m_localaddr));
	memset(&m_peeraddr,0, sizeof(m_peeraddr));
}

connection::~connection()
{
	release();
}

int connection::fd()
{
	return m_fd;
}

ipaddr& connection::get_peeraddr()
{
	return m_peeraddr;
}
	
ipaddr& connection::get_localaddr()
{
	return m_localaddr;
}

void connection::set_peeraddr(const ipaddr & addr)
{
	m_peeraddr = addr;
}

void connection::set_peeraddr(const struct sockaddr_in & addr)
{
	inet_ntoa_convert(m_peeraddr , addr);
}
	
packet_buf * connection::get_recv_buf()
{
	return &m_recv_buf;
}

int connection::get_appid()
{
	return m_appid;
}

void connection::set_appid(int appid)
{
	m_appid = appid;
}
	
void connection::set_context(void * context)
{
	m_context = context;
}
	
void* connection::get_context()
{
	return m_context;
}

int connection::set_status(int status)
{
	m_status = status;
	if(status == kconnected)
	{
		return init();
	}
	
	if(status == kdisconnected)
	{
		return release();
	}
	
	if(status == kconnecting)
	{
		return enable_writing();
	}
	return 0;
}
	
int connection::get_status()
{
	return m_status;
}


time_t connection::get_alive_time()
{
	return m_alive_time;
}

void connection::set_alive_time(time_t &tick)
{
	m_alive_time = tick;
}

bool connection::connected(){
	return m_status == kconnected;
}

list_node * connection::get_list_node(){
	return m_list_node;
}
	
void connection::set_list_node(list_node * ln){
	m_list_node = ln;
}
	
int connection::init()
{
	m_events= EPOLLIN | EPOLLET | EPOLLPRI;
	int ret = update();
		
	if(!ret)
	{
		//init_packet_buf(m_send_buf, send_buf_len);
		init_packet_buf(m_recv_buf, recv_buf_len);
	}
	
	struct sockaddr_in localaddr;
	int locallen = sizeof(localaddr);
	if (getsockname(m_fd, (struct sockaddr *)&localaddr, (socklen_t*)&locallen) == 0)
	{
		inet_ntoa_convert(m_localaddr, localaddr);
	}
	else
	{
		printf_t("error: getsockname error(%d)\n", errno);
	}
	return ret;
}

int connection::release()
{	
	int ret = 0;

	if(m_events != EPOLLNONE)
	{
		m_events = EPOLLNONE;
		ret = update();
	}
	
	if(m_fd >= 0)
	{
		close(m_fd);
		m_fd = -1;
	}
	
	release_packet_buf(m_send_buf);
	release_packet_buf(m_recv_buf);

	return ret;
}

int connection::disable_writing()
{	
	if((m_events & EPOLLOUT))
	{
		m_events &= ~EPOLLOUT;
		return update();
	}
	else
	{
		printf_t("warn : already disable_writing socket(%d)\n", m_fd);
	}
	return 0;
}

int connection::enable_writing()
{
	
	if(!(m_events & EPOLLOUT))
	{
		m_events |= EPOLLOUT;
		return update();
	}
	else
	{
		printf_t("warn : already enable_writing socket(%d)\n", m_fd);
	}
	return 0;
}

int connection::update()
{
	struct epoll_event ev = {0};
	ev.data.ptr = (void*)this;
	ev.events = m_events;
	int ret = -1;
	if(m_operation == knew || m_operation == kdel){
	
		m_operation = kadd;
		ret = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_fd, &ev);
	}
	else{
		if(m_events == EPOLLNONE)
		{
			m_operation = kdel;
			ret = epoll_ctl(m_epfd, EPOLL_CTL_DEL, m_fd, &ev);
		}
		else
		{
			ret = epoll_ctl(m_epfd, EPOLL_CTL_MOD, m_fd, &ev);
		}
	}
	
	if(ret < 0)
	{
		printf_t("error: update events error %d socket(%d)\n", errno, m_fd);	
	}
	return ret;
}

int connection::add_ref()
{
	__sync_fetch_and_add(&m_ref, 1);
	return 0;
}
	
int connection::release_ref()
{
	if(__sync_fetch_and_sub(&m_ref, 1) == 0)
	{
		printf_t("error: memory leak\n");
		return -1;
	}
	return 0;
}
	
bool connection::expired()
{
	return __sync_bool_compare_and_swap(&m_ref, 0, 0);
}

int connection::set_tcp_no_delay(bool val)
{
	int optVal = val ? 1: 0;
	int res = setsockopt(m_fd, IPPROTO_TCP, TCP_NODELAY ,(char*) &optVal, sizeof(optVal));
	if(res < 0)
	{
		printf_t("error: set set_tcp_no_delay error(%d)\n", errno);
	}
	return res;
}

int connection::post_send()
{
	if(!m_events)
	{
		printf_t("warn : post send already closed socket(%d)\n", m_fd);
		return -1;
	}
	
	auto_lock __lock(m_mutex);
	
	int len = m_send_buf.has;
	if(len == 0)
	{
		printf_t("debug: post send no data socket(%d)\n", m_fd);
		return 0;
	}

	//lock
	int total = 0;
	do
	{
		int sent = ::send(m_fd, m_send_buf.buf + total , len - total, 0);
		if(sent  < 0 )
		{
			if(errno != EINTR && errno != EAGAIN)
			{
				printf_t("error: post send error(%d) socket(%d)\n", errno, m_fd);
				return -1;
			}
			else
			{
				break;
			}
		}
		total += sent;
		
	}while(total < len);
	
	//printf_t("debug: post_send send %d/%d\n", total, len);
	
	m_send_buf.has = len - total;
	if(!m_send_buf.has)
	{
		disable_writing();
	}
	else
	{
		memcpy(m_send_buf.buf, m_send_buf.buf + total, m_send_buf.has);
	}
		
	return 0;
}

//
int connection::post_send(char * data, int len)
{
	if(!data || len <= 0)
	{
		return 0;
	}
	
	if(!m_events)
	{
		printf_t("warn : post_send already closed socket(%d)\n", m_fd);
		return -1;
	}
	
	auto_lock __lock(m_mutex);
	
	
	//lock
	int has = m_send_buf.has;

	if(has)
	{
		printf_t("debug: has left %d bytes, socket(%d)\n", has + 1, m_fd);
		if(has + len > m_send_buf.len)
		{
			printf_t("warn : buffer overflow socket(%d)\n", m_fd);
			return -1;
		}
		else
		{
			memcpy(m_send_buf.buf + has, data, len);
			m_send_buf.has += len;
			return 0;
		}
	}
	
	int total = 0;
	do
	{
		int sent = ::send(m_fd, data + total, len - total, 0);
		if(sent < 0)
		{
			if(errno != EINTR && errno != EAGAIN)
			{
				printf_t("error: post_send error(%d) socket(%d)\n", errno, m_fd);
				return -1;
			}
			else
			{
				//push into send buf
				if(init_packet_buf(m_send_buf, send_buf_len) < 0){
					printf_t("error: post_send init_packet_buf error(%d)\n", errno);
					return -1;
				}
				memcpy(m_send_buf.buf + m_send_buf.has, data + total, len - total);
				m_send_buf.has += len - total;
				
				printf_t("warn : send buffer full total %d/%d error(%d %s) socket(%d) \n", 
				total, len, errno, strerror(errno), m_fd);
				
				enable_writing();
				return 0;
			}		
		}
		
		total += sent;
		
	}while(total < len);
		
	return 0;
}

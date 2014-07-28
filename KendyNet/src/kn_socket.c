#include "kn_socket.h"
#include <assert.h>

enum{
	SOCKET_NONE = 0,
	SOCKET_ESTABLISH  = 1,
	SOCKET_CONNECTING = 2,
	SOCKET_LISTENING  = 3,
	SOCKET_CLOSE      = 4,
};

static void on_events(handle_t h,int events);
static handle_t new_sock(int fd,int domain,int type,int protocal){
	kn_socket *s = calloc(1,sizeof(*s));
	if(!s){
		return NULL;
	}	
	s->comm_head.fd = fd;
	s->comm_head.type = KN_SOCKET;
	s->domain = domain;
	s->type = type;
	s->protocal = protocal;
	s->comm_head.on_events = on_events;
	return (handle_t)s; 
}

static void process_read(kn_socket *s){
	st_io* io_req = 0;
	int bytes_transfer = 0;
	int total_transfer = 0;
	while(total_transfer < 65536 && (io_req = (st_io*)kn_list_pop(&s->pending_recv))!=NULL){
		errno = 0;
		if(s->protocal == IPPROTO_TCP){
			bytes_transfer = TEMP_FAILURE_RETRY(readv(s->comm_head.fd,io_req->iovec,io_req->iovec_count));
		}else if(s->protocal == IPPROTO_UDP){		
		}else
			assert(0);
		
		if(bytes_transfer < 0 && errno == EAGAIN){
				//将请求重新放回到队列
				kn_list_pushback(&s->pending_recv,(kn_list_node*)io_req);
				break;
		}else{
			s->cb_ontranfnish((handle_t)s,io_req,bytes_transfer,errno);
		}
	}	
	if(kn_list_size(&s->pending_recv) == 0){
		//没有接收请求了,取消EPOLLIN
		int events = s->events ^ EPOLLIN;
		if(0 == kn_event_mod(s->e,(handle_t)s,events))
			s->events = events;
	}
}

static void process_write(kn_socket *s){
	st_io* io_req = 0;
	int bytes_transfer = 0;
	int total_transfer = 0;
	while(total_transfer < 65536 && (io_req = (st_io*)kn_list_pop(&s->pending_send))!=NULL){
		errno = 0;
		if(s->protocal == IPPROTO_TCP){
			bytes_transfer = TEMP_FAILURE_RETRY(writev(s->comm_head.fd,io_req->iovec,io_req->iovec_count));
		}else if(s->protocal == IPPROTO_UDP){		
		}else
			assert(0);
		
		if(bytes_transfer < 0 && errno == EAGAIN){
				//将请求重新放回到队列
				kn_list_pushback(&s->pending_send,(kn_list_node*)io_req);
				break;
		}else{
			s->cb_ontranfnish((handle_t)s,io_req,bytes_transfer,errno);
		}
	}
	if(kn_list_size(&s->pending_send) == 0){
		//没有接收请求了,取消EPOLLOUT
		int events = s->events ^ EPOLLOUT;
		if(0 == kn_event_mod(s->e,(handle_t)s,events))
			s->events = events;
	}		
}

static int _accept(kn_socket *a,kn_sockaddr *remote){
	int fd;
	socklen_t len;
	int domain = a->domain;
again:
	if(domain == AF_INET){
		len = sizeof(remote->in);
		fd = accept(a->comm_head.fd,(struct sockaddr*)&remote->in,&len);
	}else if(domain == AF_INET6){
		len = sizeof(remote->in6);
		fd = accept(a->comm_head.fd,(struct sockaddr*)&remote->in6,&len);
	}else if(domain == AF_LOCAL){
		len = sizeof(remote->un);
		fd = accept(a->comm_head.fd,(struct sockaddr*)&remote->un,&len);
	}else{
		return -1;
	}

	if(fd < 0){
#ifdef EPROTO
		if(errno == EPROTO || errno == ECONNABORTED)
#else
		if(errno == ECONNABORTED)
#endif
			goto again;
		else
			return -errno;
	}
	return fd;
}

static void process_accept(kn_socket *s){
    int fd;
    kn_sockaddr remote;
    while(1)
    {
    	fd = _accept(s,&remote);
    	if(fd < 0)
    		break;
    	else{
		   handle_t h = new_sock(fd,s->domain,s->type,s->protocal);	
		   fcntl(fd, F_SETFL, O_NONBLOCK | O_RDWR);
		   ((kn_socket*)h)->addr_local = s->addr_local;
		   ((kn_socket*)h)->addr_remote = remote;
		   ((kn_socket*)h)->comm_head.status = SOCKET_ESTABLISH;
			s->cb_accept(h,s->comm_head.ud);
    	}      
    }
}

static void process_connect(kn_socket *s,int events){
	int err = 0;
    socklen_t len = sizeof(err);    
    kn_event_del(s->e,(handle_t)s);
    s->events = 0;
    if(getsockopt(s->comm_head.fd, SOL_SOCKET, SO_ERROR, &err, &len) == -1) {
        s->cb_connect((handle_t)s,err,s->comm_head.ud);
        return;
    }
    if(err){
        errno = err;
        s->cb_connect((handle_t)s,errno,s->comm_head.ud);
        return;
    }
    //connect success
    s->comm_head.status = SOCKET_ESTABLISH;
    s->cb_connect((handle_t)s,0,s->comm_head.ud);	
}

static void destroy_socket(kn_socket *s){
	st_io *io_req;
	if(s->destry_stio){
        while((io_req = (st_io*)kn_list_pop(&s->pending_send))!=NULL)
            s->destry_stio(io_req);
        while((io_req = (st_io*)kn_list_pop(&s->pending_recv))!=NULL)
            s->destry_stio(io_req);
	}
	close(s->comm_head.fd);
	free(s);
}

static void on_events(handle_t h,int events){
	kn_socket *s = (kn_socket*)h;
	s->processing = 1;
	if(s->comm_head.status == SOCKET_LISTENING){
		process_accept(s);
	}else if(s->comm_head.status == SOCKET_CONNECTING){
		process_connect(s,events);
	}else if(s->comm_head.status == SOCKET_ESTABLISH){
		if(events & (EPOLLERR | EPOLLHUP)){
			char buf[1];
			errno = 0;
			(void)(read(s->comm_head.fd,buf,1));//触发errno变更
			s->cb_ontranfnish((handle_t)s,NULL,-1,errno);
			return;
		}
		
		if(events & (EPOLLRDHUP | EPOLLIN)){
			process_read(s);
		}
		
		if(s->comm_head.status == SOCKET_ESTABLISH && (events & EPOLLOUT)){
			process_write(s);
		}
	}
	s->processing = 0;
	if(s->comm_head.status == SOCKET_CLOSE){
		destroy_socket(s);
	}	
}

handle_t kn_new_sock(int domain,int type,int protocal){	
	int fd = socket(domain,type|SOCK_NONBLOCK,protocal);
	if(fd < 0) return NULL;
	handle_t h = new_sock(fd,domain,type,protocal);
	if(!h) close(fd);
	return h;
}

int kn_sock_associate(handle_t h,engine_t e,void (*cb_ontranfnish)(handle_t,st_io*,int,int),void (*destry_stio)(st_io*)){
	kn_socket *s = (kn_socket*)h;
	if(!cb_ontranfnish) return -1;
	if(s->comm_head.status != SOCKET_ESTABLISH) return -1;
	if(s->e) kn_event_del(s->e,h);
	s->destry_stio = destry_stio;
	s->cb_ontranfnish = cb_ontranfnish;
	s->e = e;
	return -1;
}

int kn_sock_send(handle_t h,st_io *req){
	kn_socket *s = (kn_socket*)h;
	if(!s->e || s->comm_head.status != SOCKET_ESTABLISH) return -2;
	kn_list_pushback(&s->pending_send,(kn_list_node*)req);
	if(!(s->events & EPOLLOUT)){
		int events = s->events | EPOLLOUT;
		int ret = 0;
		if(s->events == 0){
			events |= EPOLLRDHUP;
			ret = kn_event_add(s->e,(handle_t)s,events);
		}else
			ret = kn_event_mod(s->e,(handle_t)s,events);
			
		if(ret == 0)
			s->events = events;
		else
			return -1;
	} 	
	return 0;
}

int kn_sock_recv(handle_t h,st_io *req){
	kn_socket *s = (kn_socket*)h;
	if(!s->e || s->comm_head.status != SOCKET_ESTABLISH) return -2;	
	kn_list_pushback(&s->pending_recv,(kn_list_node*)req);
	if(!(s->events & EPOLLIN)){
		int events = s->events | EPOLLIN;
		int ret = 0;
		if(s->events == 0){
			events |= EPOLLRDHUP;
			ret = kn_event_add(s->e,(handle_t)s,events);
		}else
			ret = kn_event_mod(s->e,(handle_t)s,events);
			
		if(ret == 0)
			s->events = events;
		else
			return -1;
	} 	
	return 0;	
}

static int kn_bind(int fd,kn_sockaddr *addr_local){
	assert(addr_local);
	int ret = -1;
	if(addr_local->addrtype == AF_INET)
		ret = bind(fd,(struct sockaddr*)&addr_local->in,sizeof(addr_local->in));
	else if(addr_local->addrtype == AF_INET6)
		ret = bind(fd,(struct sockaddr*)&addr_local->in6,sizeof(addr_local->in6));
	else if(addr_local->addrtype == AF_LOCAL)
		ret = bind(fd,(struct sockaddr*)&addr_local->un,sizeof(addr_local->un));
	return ret;	
}


static int stream_listen(engine_t e,kn_socket *s,int fd,kn_sockaddr *local){	
	int32_t yes = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)))
		return -1;
	
	if(kn_bind(fd,local) < 0){
		 return -1;
	}
	
	if(listen(fd,SOMAXCONN) < 0){
		return -1;
	}

	s->addr_local = *local;
	int events = s->events | EPOLLIN;
	if(0 == kn_event_add(e,(handle_t)s,events)){
		s->events = events;
		s->comm_head.status = SOCKET_LISTENING;
	}
	else
		return -1;		
	return 0;
}

static int dgram_listen(engine_t e,kn_socket *s,int fd,kn_sockaddr *local){
	return -1;
}

int kn_sock_listen(engine_t e,handle_t h,kn_sockaddr *local,void (*cb_accept)(handle_t,void*),void *ud){
	kn_socket *s = (kn_socket*)h;
	if(s->comm_head.status != SOCKET_NONE) return -1;
	if(s->e) return -1;
	int ret;
	
	if(s->protocal == IPPROTO_UDP)
		ret = dgram_listen(e,s,s->comm_head.fd,local);
	else
		ret = stream_listen(e,s,s->comm_head.fd,local);
	
	if(ret == 0){
		s->cb_accept = cb_accept;
		s->comm_head.ud = ud;
	}	
	return ret;		
}

static int stream_connect(engine_t e,kn_socket *s,int fd,kn_sockaddr *local,kn_sockaddr *remote){
	socklen_t len;	
	if(local){
		if(kn_bind(fd,local) < 0){
			return -1;
		}
	}
	int ret;
	if(s->domain == AF_INET)
		ret = connect(fd,(const struct sockaddr *)&remote->in,sizeof(remote->in));
	else if(s->domain == AF_INET6)
		ret = connect(fd,(const struct sockaddr *)&remote->in6,sizeof(remote->in6));
	else if(s->domain == AF_LOCAL)
		ret = connect(fd,(const struct sockaddr *)&remote->un,sizeof(remote->un));
	else{
		return -1;
	}
	if(ret < 0 && errno != EINPROGRESS){
		return -1;
	}
	if(ret == 0){		
		if(!local){		
			s->addr_local.addrtype = s->domain;
			if(s->addr_local.addrtype == AF_INET){
				len = sizeof(s->addr_local.in);
				getsockname(fd,(struct sockaddr*)&s->addr_local.in,&len);
			}else if(s->addr_local.addrtype == AF_INET6){
				len = sizeof(s->addr_local.in6);
				getsockname(fd,(struct sockaddr*)&s->addr_local.in6,&len);
			}else{
				len = sizeof(s->addr_local.un);
				getsockname(fd,(struct sockaddr*)&s->addr_local.un,&len);
			}
    	}
		return 1;
	}else{
		int events = s->events | EPOLLIN | EPOLLOUT;
		if(0 == kn_event_add(e,(handle_t)s,events)){
			s->events = events;
			s->comm_head.status = SOCKET_CONNECTING;
		}else
			return -1;
	}
	return 0;
}

static int dgram_connect(engine_t e,kn_socket *s,int fd,kn_sockaddr *local,kn_sockaddr *remote){
	return -1;
}

int kn_sock_connect(engine_t e,handle_t h,kn_sockaddr *remote,kn_sockaddr *local,void (*cb_connect)(handle_t,int,void*),void *ud){

	kn_socket *s = (kn_socket*)h;
	if(s->comm_head.status != SOCKET_NONE) return -1;
	if(s->e) return -1;	

	int ret;
	
	if(s->protocal == IPPROTO_UDP)
		ret = dgram_connect(e,s,s->comm_head.fd,local,remote);
	else
		ret = stream_connect(e,s,s->comm_head.fd,local,remote);
	
	if(ret == 0){
		s->cb_connect = cb_connect;
		s->comm_head.ud = ud;
	}else if(ret == 1){
		s->comm_head.status = SOCKET_ESTABLISH;
		cb_connect(h,0,ud);
		ret = 0;
	}	
	return ret;
}

int kn_close_sock(handle_t h){
	kn_socket *s = (kn_socket*)h;
	if(s->comm_head.status != SOCKET_CLOSE){
		if(s->processing){
			s->comm_head.status = SOCKET_CLOSE;
		}else{
			//可以安全释放
			destroy_socket(s);
		}
		return 0;
	}
	return -1;	
}

void kn_sock_setud(handle_t h,void *ud){
	kn_socket *s = (kn_socket*)h;
	s->comm_head.ud = ud;
}

void* kn_sock_getud(handle_t h){
	kn_socket *s = (kn_socket*)h;
	return s->comm_head.ud;
}
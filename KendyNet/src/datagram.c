#include "datagram.h"
#include <assert.h>

static void datagram_destroy(void *ptr)
{
	datagram_t c = (datagram_t)ptr;
	buffer_release(c->recv_buf);
	kn_close_sock(c->handle);
	destroy_decoder(c->_decoder);
	if(c->ud && c->destroy_ud){
		c->destroy_ud(c->ud);
	}
	free(c);				
}

datagram_t new_datagram(handle_t sock,uint32_t buffersize,decoder *_decoder)
{
	buffersize = size_of_pow2(buffersize);
    	if(buffersize < 1024) buffersize = 1024;	
	datagram_t c = calloc(1,sizeof(*c));
	c->recv_bufsize = buffersize;
	refobj_init((refobj*)c,datagram_destroy);
	c->handle = sock;
	kn_sock_setud(sock,c);
	c->_decoder = _decoder;
	if(!c->_decoder) c->_decoder = new_datagram_rawpk_decoder();
	return c;
}


static inline void prepare_recv(datagram_t c){
	buffer_t buf = buffer_create(c->recv_bufsize);
	c->recv_buf = buffer_acquire(c->recv_buf,buf);	
	c->wrecvbuf.iov_len = c->recv_bufsize;
	c->wrecvbuf.iov_base = buf->buf;
	c->recv_overlap.iovec_count = 1;
	c->recv_overlap.iovec = &c->wrecvbuf;	
}

static inline void PostRecv(datagram_t c){
	prepare_recv(c);
	kn_sock_post_recv(c->handle,&c->recv_overlap);	
	c->doing_recv = 1;	
}

/*static inline int Recv(datagram_t c,int32_t* err_code){
	prepare_recv(c);
	int ret = kn_sock_recv(c->handle,&c->recv_overlap);
	if(err_code) *err_code = errno;
	if(ret == 0){
		c->doing_recv = 1;
		return 0;
	}
	return ret;
}*/

static int raw_unpack(decoder *_,void* _1){
	((void)_);
	datagram_t c = (datagram_t)_1;
	packet_t r = (packet_t)rawpacket_create1(c->recv_buf,0,c->recv_buf->size);
	c->on_packet(c,r,&c->recv_overlap.addr); 
	destroy_packet(r);
	return 0;
}

static int rpk_unpack(decoder *_,void *_1){
	/*((void)_);
	datagram_t c = (datagram_t)_1;
	if(c->unpack_size <= sizeof(uint32_t))
		return 0;	
	uint32_t pk_len = 0;
	uint32_t pk_hlen;

	buffer_read(c->next_recv_buf,c->next_recv_pos,(int8_t*)&pk_len,sizeof(pk_len));
	pk_hlen = kn_ntoh32(pk_len);
	uint32_t pk_total_size = pk_hlen+sizeof(pk_len);
	if(c->unpack_size != pk_total_size)
		return 0;
	packet_t r = (packet_t)rpk_create(c->next_recv_buf,c->next_recv_pos,pk_hlen);
	c->on_packet(c,r,&c->recv_overlap.addr); 
	destroy_packet(r);
	*/
	return 0;
}	

static void IoFinish(handle_t sock,void *_,int32_t bytestransfer,int32_t err_code)
{
	datagram_t c = kn_sock_getud(sock);
	c->doing_recv = 0;	
	refobj_inc((refobj*)c);
	if(bytestransfer > 0){
		c->recv_buf->size = bytestransfer;
		c->_decoder->unpack(c->_decoder,c);
	}
	PostRecv(c);
	refobj_dec((refobj*)c);
}

int datagram_send(datagram_t c,packet_t w,kn_sockaddr *addr)
{
	int ret = -1;
	do{	
		if(!addr){
			errno = EINVAL;
			break;
		}
		if(packet_type(w) != WPACKET && packet_type(w) != RAWPACKET){
			errno = EMSGSIZE;
			break;
		}
		st_io o;
		int32_t i = 0;
		uint32_t size = 0;
		uint32_t pos = packet_begpos(w);
		buffer_t b = packet_buf(w);
		uint32_t buffer_size = packet_datasize(w);
		while(i < MAX_WBAF && b && buffer_size)
		{
			c->wsendbuf[i].iov_base = b->buf + pos;
			size = b->size - pos;
			size = size > buffer_size ? buffer_size:size;
			buffer_size -= size;
			c->wsendbuf[i].iov_len = size;
			++i;
			b = b->next;
			pos = 0;
		}
		if(buffer_size != 0){
			errno = EMSGSIZE;
			break;		
		}
		o.iovec_count = i;
		o.iovec = c->wsendbuf;
		o.addr = *addr;
		ret = kn_sock_send(c->handle,&o);
	}while(0);
	destroy_packet(w);
	return ret;
}

int datagram_associate(engine_t e,datagram_t conn,DCCB_PROCESS_PKT on_packet)
{		
      kn_engine_associate(e,conn->handle,IoFinish);
      if(on_packet) conn->on_packet = on_packet;
      if(e && !conn->doing_recv) PostRecv(conn);
      return 0;
}

decoder* new_datagram_rpk_decoder(){
	datagram_rpk_decoder *de = calloc(1,sizeof(*de));
	de->base.unpack = rpk_unpack;
	de->base.destroy = NULL;
	return (decoder*)de;
}

decoder* new_datagram_rawpk_decoder(){
	datagram_rawpk_decoder *de = calloc(1,sizeof(*de));
	de->base.destroy = NULL;
	de->base.unpack = raw_unpack;
	return (decoder*)de;	
}

void datagram_close(datagram_t c){
	refobj_dec((refobj*)c); 	
}
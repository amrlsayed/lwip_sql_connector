/*
 sql_connector.c
 Copyright (c) 2017 DIY Life. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
  Created on: Aug 21, 2017
       Author: Amr Elsayed
 */

#include "lwip/tcp.h"
#include "lwip/raw.h"

#include "lwip/opt.h"
#include "lwip/dns.h"  // DNS
#include "lwip/def.h"
#include "lwip/memp.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/raw.h"

#include "lwip/stats.h"

#include "arch/perf.h"
#include "arch/cc.h"
#include "lwip/snmp.h"

#include "lwip/api.h"
#include "lwip/sys.h"
#include "sql_connector.h"
#include <string.h>

/* HTTP methods Strings*/
#define GET_METHOD "GET"
#define POST_METHOD "POST"

const char* get_str = GET_METHOD;
const char* post_str = POST_METHOD;


#define SQLC_POLL_INTERVAL 4   //  4 * 0.5 SEC.
#define SQLC_TIMEOUT 5// * 60 * (sqlc_poll_INTERVAL / 2) // two minutes.

/** Maximum length reserved for server name */
#ifndef SQLC_MAX_SERVERNAME_LEN
#define SQLC_MAX_SERVERNAME_LEN 256
#endif
#define HTTP_MAX_REQUEST_LENGTH 1024
#ifndef SQLC_DEFAULT_PORT
#define SQLC_DEFAULT_PORT 80
#endif
#ifdef SQLC_DEBUG
#undef SQLCC_DEBUG
#endif
#define SQLC_DEBUG         LWIP_DBG_ON

enum sqlc_session_state{
	SQLC_NEW = 0,
	SQLC_CONNECTED,
	SQLC_RECV,
	SQLC_SENT,
	SQLC_CLOSED
};
struct sql_connector{
	char connected;
	enum error_state es;
	enum state connector_state;
	/** keeping the state of the SQL Client session */
	enum sqlc_session_state state;
	const char* hostname;
	const char* username;
	const char* password;
	int port;
	ip_addr_t remote_ipaddr;
	  /** timeout handling, if this reaches 0, the connection is closed */
    u16_t  timer;
    struct tcp_pcb *pcb;
    struct pbuf* p;
    /** this is the body of the payload to be sent */
    char* payload;
    /** this is the length of the body to be sent */
    u16_t payload_len;
    /** amount of data from body already sent */
    u16_t payload_sent;
    char* server_version;
};
struct sql_cd{
	sqlc_descriptor* sqlc_d;
	struct sql_connector* sqlc;
};
static struct sql_cd sqlcd_array[MAX_SQL_CONNECTORS];

int sqlc_create( sqlc_descriptor* d ){
	int i = 0 ;
	for (i = 0 ; i<MAX_SQL_CONNECTORS ;i++){
		if(sqlcd_array[i].sqlc_d == d)
			return 1 ;
	}
	for (i = 0 ; i<MAX_SQL_CONNECTORS ;i++){
		if(sqlcd_array[i].sqlc_d == NULL && sqlcd_array[i].sqlc == NULL)
			break;
	}
	if(i == MAX_SQL_CONNECTORS)
		return 1 ;
	sqlcd_array[i].sqlc = mem_malloc(sizeof(struct sql_connector));
	if(sqlcd_array[i].sqlc == NULL)
		return 1 ;
	memset(sqlcd_array[i].sqlc,0,sizeof(struct sql_connector));
	sqlcd_array[i].sqlc->connected = 0 ;
	sqlcd_array[i].sqlc->connector_state = CONNECTOR_STATE_IDLE ;
	sqlcd_array[i].sqlc->es = CONNECTOR_ERROR_OK;
	sqlcd_array[i].sqlc_d = d;
	return 0 ;
}
static err_t sqlc_sendrequest_allocated(struct sql_connector* sqlc_ptr)
{
	 struct tcp_pcb *pcb = NULL;
	 err_t ret_code = ERR_OK,err;
	 pcb = tcp_new();
	 if(NULL == pcb)
	 {
		 LWIP_DEBUGF(SQLC_DEBUG, ("httpc_sendrequest_allocated(): calling tcp_new can not allocate memory for PCB.\n\r"));
		 err = ERR_MEM;
		 goto leave;
	 }
	 sqlc_ptr->pcb = pcb;
	 sqlc_ptr->timer = SQLC_TIMEOUT;
	 tcp_arg(pcb, sqlc_ptr);
	 tcp_poll(pcb, sqlc_poll, SQLC_POLL_INTERVAL);
	 tcp_err(pcb, sqlc_err);

	 sqlc_ptr->remote_ipaddr.addr = ipaddr_addr(sqlc_ptr->hostname);
	 err = sqlc_ptr->remote_ipaddr.addr == IPADDR_NONE ? ERR_ARG : ERR_OK;
	 if(err == ERR_OK){
		 ret_code = tcp_connect(pcb, &sqlc_ptr->remote_ipaddr, sqlc_ptr->port, sqlc_connected);
		 if(ERR_OK != ret_code)
		 {
				 LWIP_DEBUGF(SQLC_DEBUG, ("tcp_connect():no memory is available for enqueueing the SYN segment %d\n\r",ret_code));
				 goto deallocate_and_leave;
		 }
	   } else if (err != ERR_INPROGRESS) {
		LWIP_DEBUGF(SQLC_DEBUG, ("dns_gethostbyname failed: %d\r\n", (int)err));
		goto deallocate_and_leave;
	  }

  return ERR_OK;
deallocate_and_leave:
  if (pcb != NULL) {
    tcp_arg(pcb, NULL);
    tcp_close(pcb);
  }
leave:
//  mem_free(sqlc_ptr);
  /* no need to call the callback here since we return != ERR_OK */
  return err;
}
/*
 * hostname  username , password , need to be permenant in memory as long as we
 * have the connector..
 *
 *
 */
int sqlc_connect(sqlc_descriptor* d ,const char* hostname ,int port, const char* username ,const char* password )
{
	int i = 0 ;
	for (i = 0 ; i < MAX_SQL_CONNECTORS; i++)
	{
		if(sqlcd_array[i].sqlc_d == d && sqlcd_array[i].sqlc != NULL)
			break;
	}
	if(i == MAX_SQL_CONNECTORS)
		return 1 ;
	struct sql_connector* sqlc_ptr = sqlcd_array[i].sqlc;
	if(sqlc_ptr->connected)
		return 1 ;
	sqlc_ptr->hostname = hostname;
	sqlc_ptr->username = username;
	sqlc_ptr->password = password;
	sqlc_ptr->port = port;

	err_t err = sqlc_sendrequest_allocated(sqlc_ptr);
	if(err != ERR_OK)
		return 1;
	sqlc_ptr->connector_state = CONNECTOR_STATE_CONNECTING;
	sqlc_ptr->es = CONNECTOR_ERROR_OK;
	return 0 ;
}
int sqlc_disconnect(sqlc_descriptor*d)
{
	int i = 0 ;
	for (i = 0 ; i < MAX_SQL_CONNECTORS; i++)
	{
		if(sqlcd_array[i].sqlc_d == d && sqlcd_array[i].sqlc != NULL)
			break;
	}
	if(i == MAX_SQL_CONNECTORS)
		return 1 ;
	/* Don't disconnect a busy connector */
	if(sqlcd_array[i].sqlc->connector_state != CONNECTOR_STATE_IDLE){
		return 1 ;
	}
	if(sqlc_close(sqlcd_array[i].sqlc))
		return 1 ;
	if(sqlcd_array[i].sqlc->connected){
		sqlcd_array[i].sqlc->connected = 0 ;
		sqlcd_array[i].sqlc->connector_state = CONNECTOR_STATE_IDLE ;
		sqlcd_array[i].sqlc->es = CONNECTOR_ERROR_OK;
		sqlcd_array[i].sqlc->hostname = NULL;
		sqlcd_array[i].sqlc->port = SQLC_DEFAULT_PORT;
		sqlcd_array[i].sqlc->username = NULL;
		sqlcd_array[i].sqlc->password = NULL;
		return 0 ;
	}
	return 1 ;
}
/*
 * you can't delete a connected or not IDLE connector
 * */

int sqlc_delete(sqlc_descriptor*d)
{
	int i = 0 ;
	for (i = 0 ; i<MAX_SQL_CONNECTORS ;i++){
		if(sqlcd_array[i].sqlc_d == d)
			break;
	}
	if(i == MAX_SQL_CONNECTORS)
		return 1 ;
	if(sqlcd_array[i].sqlc->connected || sqlcd_array[i].sqlc->connector_state != CONNECTOR_STATE_IDLE)
		return 1 ;
	mem_free(sqlcd_array[i].sqlc);
	sqlcd_array[i].sqlc = NULL;
	sqlcd_array[i].sqlc_d = NULL;
	return 0 ;
}
int sqlc_get_state(sqlc_descriptor*d,enum state* state)
{
	int i ;
	for (i = 0 ; i<MAX_SQL_CONNECTORS ;i++){
		if(sqlcd_array[i].sqlc_d == d)
			break;
	}
	if(i == MAX_SQL_CONNECTORS)
		return 1 ;
	*state = sqlcd_array[i].sqlc->connector_state;
	return 0 ;
}
int sqlc_get_error_state(sqlc_descriptor*d,enum error_state* es)
{
	int i ;
	for (i = 0 ; i<MAX_SQL_CONNECTORS ;i++){
		if(sqlcd_array[i].sqlc_d == d)
			break;
	}
	if(i == MAX_SQL_CONNECTORS)
		return 1 ;
	*es = sqlcd_array[i].sqlc->es;
	return 0 ;
}
int sqlc_is_connected(sqlc_descriptor*d, char* connected)
{
	int i ;
	for (i = 0 ; i<MAX_SQL_CONNECTORS ;i++){
		if(sqlcd_array[i].sqlc_d == d)
			break;
	}
	if(i == MAX_SQL_CONNECTORS)
		return 1 ;
	*connected = sqlcd_array[i].sqlc->connected;
	return 0 ;
}

err_t sqlc_sent(void *arg, struct tcp_pcb *pcb, u16_t len);
void sqlc_err(void *arg, err_t err);
err_t sqlc_connected(void *arg, struct tcp_pcb *pcb, err_t err);
err_t sqlc_poll(void *arg, struct tcp_pcb *pcb);
err_t sqlc_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *pbuf, err_t err);
static void sqlc_cleanup(struct sql_connector *s);

err_t sqlc_connected(void *arg, struct tcp_pcb *pcb, err_t err)
 {
     err_t ret_code = ERR_OK;
     struct sql_connector* sqlc_ptr = arg;
	 LWIP_UNUSED_ARG(err); /* for some compilers warnings. */
	 sqlc_ptr->timer = SQLC_TIMEOUT;
	 tcp_recv(pcb, sqlc_recv);
	 sqlc_ptr->state = SQLC_CONNECTED ;

	 return ret_code;
}
void sqlc_err(void *arg, err_t err)
 {
	 struct sql_connector *s = arg;
	 LWIP_DEBUGF(SQLC_DEBUG, ("sqlc_err():error at client : %d\n\r",err));
	 if(s->connector_state == CONNECTOR_STATE_CONNECTING  && (s->state <= SQLC_SENT)){
		 s->connector_state  =  CONNECTOR_STATE_CONNECTOR_ERROR ;
		 s->es = CONNECTOR_ERROR_TCP_ERROR;
		 s->state = SQLC_CLOSED;
	 }else if (s->connected && s->connector_state == CONNECTOR_STATE_IDLE){
		 s->connected = 0 ;
		 s->connector_state  =  CONNECTOR_STATE_CONNECTOR_ERROR ;
		 s->es = CONNECTOR_ERROR_TCP_ERROR;
		 s->state = SQLC_CLOSED;
	 }
	 //@ TODO handle other events , basically we check the connector state and the session state...
	 sqlc_cleanup(s);

}
err_t sqlc_poll(void *arg, struct tcp_pcb *pcb)
{
    err_t ret_code = ERR_OK,err;
	LWIP_DEBUGF(SQLC_DEBUG, ("sqlc_poll()\r\n"));
	if (arg != NULL) {
		struct sql_connector *s = arg;
		LWIP_DEBUGF(SQLC_DEBUG, ("sqlc_poll(): %d\n\r",s->timer));
		if(s->connector_state == CONNECTOR_STATE_CONNECTING && s->state <= SQLC_SENT){
			if (s->timer != 0) {
				s->timer--;
			}
			 /* idle timer, close connection if timed out */
			if (s->timer == 0) {
				LWIP_DEBUGF(SQLC_DEBUG, ("sqlc_poll: connection timed out, closing\n\r"));
				sqlc_close(s); // TODO handle it's return...
				s->connector_state = CONNECTOR_STATE_CONNECTOR_ERROR;
				s->es = CONNECTOR_ERROR_CANNOT_CONNECT;
				s->state = SQLC_CLOSED;
				ret_code = ERR_ABRT;
			}
		}
		// TODO handle other events..
	}
	else{
		LWIP_DEBUGF(HTTPC_DEBUG, ("sqlc_poll: something wrong\n\r"));
	}
 return ret_code;
 }
static void
sqlc_cleanup(struct sql_connector *s)
{
	LWIP_DEBUGF(SQLC_DEBUG, ("sqlc_cleanup()\r\n"));
	if(s->pcb)
		/* try to clean up the pcb if not already deallocated*/
		sqlc_close(s);
	if(s->payload){
		mem_free(s->payload);
		s->payload = NULL;
	}
	if(s->server_version){
		mem_free(s->server_version);
		s->server_version = NULL;
	}
}

/** Try to close a pcb and free the arg if successful */
static int
sqlc_close(struct sql_connector *s)//, err_t err)
{
	LWIP_DEBUGF(HTTPC_DEBUG, ("sqlc_close()\r\n"));
	tcp_arg(s->pcb, NULL);
	tcp_poll(s->pcb,NULL,0);  // may be wrong ?
	tcp_sent(s->pcb, NULL);
	tcp_recv(s->pcb, NULL);
	tcp_err(s->pcb, NULL);
	tcp_connect(s->pcb,NULL,0,NULL);
	if (tcp_close(s->pcb) == ERR_OK) {
	  return 0 ;
	}
	/* close failed, set back arg */
	tcp_arg(s->pcb, s);
	s->connected = 0 ;
	sqlc_cleanup(s);
	return 1;
}

char seed[20];

void parse_handshake_packet(struct sql_connector s,struct pbuf *p)
{
	int len = strlen(p->payload[5]);
	s->server_version = mem_malloc(len);
	if(s->server_version)
		strcpy(s->server_version,p->payload[5]);
	int seed_index = len + 6 ;
	for(int i = 0 ; i < 8 ; i++)
		seed[i] = p->payload[seed_index + i ];
	seed_index += 27 ;
	for(int i = 0 ; i < 12 ; i++)
	{
		seed[i + 8] = p->payload[seed_index + i ];
	}
}
err_t sqlc_send(struct tcp_pcb *pcb,char* payload){
	int len ;
	err_t ret_code = ERR_OK,err;
	len=strlen(payload);
	if(len > tcp_sndbuf(pcb)){
		LWIP_DEBUGF(HTTPC_DEBUG,("httpc_send_request: request length is Larger than max amount%d\n\r",err));
		len = tcp_sndbuf(pcb);
	}
	LWIP_DEBUGF(HTTPC_DEBUG, ("httpc_send_request: TCP write: %d\r\n",len));
	err =  tcp_write(pcb, payload, len, 1);
	if (err != ERR_OK) {
		LWIP_DEBUGF(HTTPC_DEBUG,("httpc_send_request: error writing! %d\n\r",err));
		ret_code = err ;
		return ret_code;
	}
	tcp_sent(pcb, sqlc_sent);
	return ret_code;
}
char scramble_password(char* password , char* scramble)
{


		return 1;
}
err_t send_authentication_packet( struct sql_connector* s, struct tcp_pcb *pcb, char *user, char *password)
{
	s->payload = (char*) mem_malloc(256);
	if(s){
	  int size_send = 4;
	  err_t err = ERR_OK;
	  // client flags
	  s->payload[size_send] = 0x85;
	  s->payload[size_send+1] = 0xa6;
	  s->payload[size_send+2] = 0x03;
	  s->payload[size_send+3] = 0x00;
	  size_send += 4;

	  // max_allowed_packet
	  s->payload[size_send] = 0;
	  s->payload[size_send+1] = 0;
	  s->payload[size_send+2] = 0;
	  s->payload[size_send+3] = 1;
	  size_send += 4;

	  // charset - default is 8
	  s->payload[size_send] = 0x08;
	  size_send += 1;
	  for(int i = 0; i < 24; i++)
	    s->payload[size_send+i] = 0x00;
	  size_send += 23;

	  // user name
	  memcpy((char *)&s->payload[size_send], user, strlen(user));
	  size_send += strlen(user) + 1;
	  s->payload[size_send-1] = 0x00;

	  // password - see scramble password
	   char scramble[20];
	   if (scramble_password(password, scramble)) {
	     s->payload[size_send] = 0x14;
	     size_send += 1;
	     for (int i = 0; i < 20; i++)
	       s->payload[i+size_send] = scramble[i];
	     size_send += 20;
	     s->payload[size_send] = 0x00;
	   }
	   // terminate password response
	   s->payload[size_send] = 0x00;
	   size_send += 1;

	   // database
	   s->payload[size_send+1] = 0x00;
	   size_send += 1;
	   s->payload_len = size_send;
	   // Write packet size
	   int p_size = size_send - 4;
	   store_int(&s->payload[0], p_size, 3);
	   s->payload[3] = 0x01;
	   err = sqlc_send(pcb, s->payload);
	   return err;
	}
	return ERR_MEM;
}
/*
  get_lcb_len - Retrieves the length of a length coded binary value

  This reads the first byte from the offset into the buffer and returns
  the number of bytes (size) that the integer consumes. It is used in
  conjunction with read_int() to read length coded binary integers
  from the buffer.

  Returns integer - number of bytes integer consumes
*/
int get_lcb_len(char* buffer,int offset) {
  int read_len = buffer[offset];
  if (read_len > 250) {
    // read type:
    char type = buffer[offset+1];
    if (type == 0xfc)
      read_len = 2;
    else if (type == 0xfd)
      read_len = 3;
    else if (type == 0xfe)
      read_len = 8;
  }
  return 1;
}

/*
  read_int - Retrieve an integer from the buffer in size bytes.

  This reads an integer from the buffer at offset position indicated for
  the number of bytes specified (size).

  offset[in]      offset from start of buffer
  size[in]        number of bytes to use to store the integer

  Returns integer - integer from the buffer
*/
int read_int(char* buffer,int offset, int size) {
  int value = 0;
  int new_size = 0;
  if (size == 0)
     new_size = get_lcb_len(offset);
  if (size == 1)
     return buffer[offset];
  new_size = size;
  int shifter = (new_size - 1) * 8;
  for (int i = new_size; i > 0; i--) {
    value += (char)(buffer[i-1] << shifter);
    shifter -= 8;
  }
  return value;
}

int check_ok_packet(char* buffer) {
	if(buffer != NULL){
	  int type = buffer[4];
	  if (type != MYSQL_OK_PACKET)
		return type;
	  return 0;
	}
	return MYSQL_ERROR_PACKET;
}

/*
  parse_error_packet - Display the error returned from the server

  This method parses an error packet from the server and displays the
  error code and text via Serial.print. The error packet is defined
  as follows.

  Note: the error packet is already stored in the buffer since this
        packet is not an expected response.

  Bytes                       Name
  -----                       ----
  1                           field_count, always = 0xff
  2                           errno
  1                           (sqlstate marker), always '#'
  5                           sqlstate (5 characters)
  n                           message
*/
void parse_error_packet(char* buffer,int packet_len) {
  LWIP_DEBUGF(SQLC_DEBUG,("Error: "));
  LWIP_DEBUGF(SQLC_DEBUG,("%d",read_int(buffer,5, 2)));
  LWIP_DEBUGF(SQLC_DEBUG,(" = "));
  for (int i = 0; i < packet_len-9; i++)
	  LWIP_DEBUGF(SQLC_DEBUG,("%c",(char)buffer[i+13]));
  LWIP_DEBUGF(SQLC_DEBUG,("."));
}

err_t sqlc_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
     err_t ret_code = ERR_OK;
     struct sql_connector* s = arg;
	 LWIP_UNUSED_ARG(err);
	 if(p != NULL)
	 {
		 struct pbuf *q;
		 int i =0;
		 /* received data */
		 if (s->p == NULL) {
				s->p = p;
		 }else {
				pbuf_cat(s->p, p);
		 }
		 if(s->connector_state == CONNECTOR_STATE_CONNECTING && s->state == SQLC_CONNECTED){
			 if(p->tot_len > 4){
				 unsigned long packet_length = p->payload[0];
				 packet_length += p->payload[1]<<8;
				 packet_length += p->payload[2]<<16;
				 if(p->tot_len >= packet_length + 4){
					 parse_handshake_packet(s,p);
					 tcp_recved(pcb, p->tot_len);
				     pbuf_free(p);
				     err_t err = send_authentication_packet(pcb,s->username,s->password);
				     if(err != ERR_OK){
				    	 s->connector_state = CONNECTOR_STATE_CONNECTOR_ERROR;
				    	 s->es = CONNECTOR_ERROR_CANNOT_CONNECT;
				    	 /* Don't Need to close the connection as the server will already abort it on time out ??*/
				     }
				     s->es = CONNECTOR_ERROR_OK;
				     s->timer = SQLC_TIMEOUT;
				 }
			 }
		 }else if (s->connector_state == CONNECTOR_STATE_CONNECTING && (s->state == SQLC_RECV || s->state == SQLC_SENT)){
			 if(p->tot_len > 4){
				 unsigned long packet_length = p->payload[0];
				 packet_length += p->payload[1]<<8;
				 packet_length += p->payload[2]<<16;
				 if(p->tot_len >= packet_length + 4){
					    if (check_ok_packet(p->payload) != 0) {
					      parse_error_packet(p->payload,p->tot_len);
					      // return false; meaning tell the user we don't have the connection , further close it...
					      s->connector_state = CONNECTOR_STATE_CONNECTOR_ERROR;
						  s->es = CONNECTOR_ERROR_CANNOT_CONNECT;
						  sqlc_close(s);
					    }
					    LWIP_DEBUGF(SQLC_DEBUG, ("Connected to server version %s\n\r",s->server_version));
					    mem_free(s->server_version);
					    s->server_version = NULL;


					    // Tell the application the Good news ?
					    s->connected = 1 ; // TODO handle error , sent , poll events.. if connected
					    s->connector_state = CONNECTOR_STATE_IDLE;
					    s->es = CONNECTOR_ERROR_OK;
				 }
			 }
		 }
	     s->state = SQLC_RECV;
	 }
	 else{
		 LWIP_DEBUGF(SQLC_DEBUG, ("sqlc_recv: connection closed by remote host\n\r"));
		 if((s->connector_state == CONNECTOR_STATE_CONNECTING  )
				 && (s->state == SQLC_CONNECTED || s->state == SQLC_RECV || s->state == SQLC_SENT)){
			 s->connector_state  =  CONNECTOR_STATE_CONNECTOR_ERROR ;
			 s->es = CONNECTOR_ERROR_UNEXPECTED_CLOSED_CONNECTION;
		 }
		 sqlc_close(s);
		 s->state = SQLC_CLOSED;
	 }

	 return ret_code;
}
err_t sqlc_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
	struct sql_connector * s = arg;
	LWIP_DEBUGF(SQLC_DEBUG,("sqlc_sent:Done Sending to client : %d",len));
    LWIP_DEBUGF(SQLC_DEBUG,("\n\r"));
    if(s->connector_state == CONNECTOR_STATE_CONNECTING && s->state == SQLC_RECV){

    	s->timer = SQLC_TIMEOUT;
    }
    s->payload_sent +=len;
    if(s->payload && s->payload_len - s->payload_sent)
    {
    	sqlc_send(pcb,&s->payload[s->payload_sent]);
    }
    else if (s->payload && !(s->payload_len - s->payload_sent))
    {
    	mem_free(s->payload);
    	s->payload = NULL;
    }
	s->state = SQLC_SENT;

  return ERR_OK;
}


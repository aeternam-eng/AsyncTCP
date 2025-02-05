/*
  Asynchronous TCP library for Espressif MCUs

  Copyright (c) 2016 Hristo Gochkov. All rights reserved.
  This file is part of the esp8266 core for Arduino environment.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef NEWASYNCTCP_H_
#define NEWASYNCTCP_H_

#include <Arduino.h>
#include "IPAddress.h"
#include "sdkconfig.h"
#include <functional>
extern "C" {
    #include "freertos/FreeRTOS.h"
    #include "freertos/semphr.h"
    #include "freertos/queue.h"
    #include "new_lwip/pbuf.h"
}

//If core is not defined, then we are running in Arduino or PIO
#ifndef NEW_CONFIG_ASYNC_TCP_RUNNING_CORE
#define NEW_CONFIG_ASYNC_TCP_RUNNING_CORE -1 //any available core
#define NEW_CONFIG_ASYNC_TCP_USE_WDT 0 //if enabled, adds between 33us and 200us per event
#endif

class newAsyncClient;

#define ASYNC_MAX_ACK_TIME 5000
#define ASYNC_WRITE_FLAG_COPY 0x01 //will allocate new buffer to hold the data while sending (else will hold reference to the data given)
#define ASYNC_WRITE_FLAG_MORE 0x02 //will not send PSH flag, meaning that there should be more data to be sent before the application should react.

typedef std::function<void(void*, newAsyncClient*)> newAcConnectHandler;
typedef std::function<void(void*, newAsyncClient*, size_t len, uint32_t time)> newAcAckHandler;
typedef std::function<void(void*, newAsyncClient*, int8_t error)> newAcErrorHandler;
typedef std::function<void(void*, newAsyncClient*, void *data, size_t len)> newAcDataHandler;
typedef std::function<void(void*, newAsyncClient*, struct pbuf *pb)> newAcPacketHandler;
typedef std::function<void(void*, newAsyncClient*, uint32_t time)> newAcTimeoutHandler;

struct new_tcp_pcb;
struct new_ip_addr;

class newAsyncClient {
  public:
    newAsyncClient(new_tcp_pcb* pcb = 0);
    ~newAsyncClient();

    newAsyncClient & operator=(const newAsyncClient &other);
    newAsyncClient & operator+=(const newAsyncClient &other);

    bool operator==(const newAsyncClient &other);

    bool operator!=(const newAsyncClient &other) {
      return !(*this == other);
    }
    bool connect(IPAddress ip, uint16_t port);
    bool connect(const char* host, uint16_t port);
    void close(bool now = false);
    void stop();
    int8_t abort();
    bool free();

    bool canSend();//ack is not pending
    size_t space();//space available in the TCP window
    size_t add(const char* data, size_t size, uint8_t apiflags=ASYNC_WRITE_FLAG_COPY);//add for sending
    bool send();//send all data added with the method above

    //write equals add()+send()
    size_t write(const char* data);
    size_t write(const char* data, size_t size, uint8_t apiflags=ASYNC_WRITE_FLAG_COPY); //only when canSend() == true

    uint8_t state();
    bool connecting();
    bool connected();
    bool disconnecting();
    bool disconnected();
    bool freeable();//disconnected or disconnecting

    uint16_t getMss();

    uint32_t getRxTimeout();
    void setRxTimeout(uint32_t timeout);//no RX data timeout for the connection in seconds

    uint32_t getAckTimeout();
    void setAckTimeout(uint32_t timeout);//no ACK timeout for the last sent packet in milliseconds

    void setNoDelay(bool nodelay);
    bool getNoDelay();

    uint32_t getRemoteAddress();
    uint16_t getRemotePort();
    uint32_t getLocalAddress();
    uint16_t getLocalPort();

    //compatibility
    IPAddress remoteIP();
    uint16_t  remotePort();
    IPAddress localIP();
    uint16_t  localPort();

    void onConnect(newAcConnectHandler cb, void* arg = 0);     //on successful connect
    void onDisconnect(newAcConnectHandler cb, void* arg = 0);  //disconnected
    void onAck(newAcAckHandler cb, void* arg = 0);             //ack received
    void onError(newAcErrorHandler cb, void* arg = 0);         //unsuccessful connect or error
    void onData(newAcDataHandler cb, void* arg = 0);           //data received (called if onPacket is not used)
    void onPacket(newAcPacketHandler cb, void* arg = 0);       //data received
    void onTimeout(newAcTimeoutHandler cb, void* arg = 0);     //ack timeout
    void onPoll(newAcConnectHandler cb, void* arg = 0);        //every 125ms when connected

    void ackPacket(struct pbuf * pb);//ack pbuf from onPacket
    size_t ack(size_t len); //ack data that you have not acked using the method below
    void ackLater(){ _ack_pcb = false; } //will not ack the current packet. Call from onData

    const char * errorToString(int8_t error);
    const char * stateToString();

    //Do not use any of the functions below!
    static int8_t _s_poll(void *arg, struct new_tcp_pcb *tpcb);
    static int8_t _s_recv(void *arg, struct new_tcp_pcb *tpcb, struct pbuf *pb, int8_t err);
    static int8_t _s_fin(void *arg, struct new_tcp_pcb *tpcb, int8_t err);
    static int8_t _s_lwip_fin(void *arg, struct new_tcp_pcb *tpcb, int8_t err);
    static void _s_error(void *arg, int8_t err);
    static int8_t _s_sent(void *arg, struct new_tcp_pcb *tpcb, uint16_t len);
    static int8_t _s_connected(void* arg, void* tpcb, int8_t err);
    static void _s_dns_found(const char *name, struct new_ip_addr *ipaddr, void *arg);

    int8_t _recv(new_tcp_pcb* pcb, pbuf* pb, int8_t err);
    new_tcp_pcb * pcb(){ return _pcb; }

  protected:
    new_tcp_pcb* _pcb;
    int8_t  _closed_slot;

    newAcConnectHandler _connect_cb;
    void* _connect_cb_arg;
    newAcConnectHandler _discard_cb;
    void* _discard_cb_arg;
    newAcAckHandler _sent_cb;
    void* _sent_cb_arg;
    newAcErrorHandler _error_cb;
    void* _error_cb_arg;
    newAcDataHandler _recv_cb;
    void* _recv_cb_arg;
    newAcPacketHandler _pb_cb;
    void* _pb_cb_arg;
    newAcTimeoutHandler _timeout_cb;
    void* _timeout_cb_arg;
    newAcConnectHandler _poll_cb;
    void* _poll_cb_arg;

    bool _pcb_busy;
    uint32_t _pcb_sent_at;
    bool _ack_pcb;
    uint32_t _rx_ack_len;
    uint32_t _rx_last_packet;
    uint32_t _rx_since_timeout;
    uint32_t _ack_timeout;
    uint16_t _connect_port;

    int8_t _close();
    int8_t _connected(void* pcb, int8_t err);
    void _error(int8_t err);
    int8_t _poll(new_tcp_pcb* pcb);
    int8_t _sent(new_tcp_pcb* pcb, uint16_t len);
    int8_t _fin(new_tcp_pcb* pcb, int8_t err);
    int8_t _lwip_fin(new_tcp_pcb* pcb, int8_t err);
    void _dns_found(struct new_ip_addr *ipaddr);

  public:
    newAsyncClient* prev;
    newAsyncClient* next;
};

class newAsyncServer {
  public:
    newAsyncServer(IPAddress addr, uint16_t port);
    newAsyncServer(uint16_t port);
    ~newAsyncServer();
    void onClient(newAcConnectHandler cb, void* arg);
    void begin();
    void end();
    void setNoDelay(bool nodelay);
    bool getNoDelay();
    uint8_t status();

    //Do not use any of the functions below!
    static int8_t _s_accept(void *arg, new_tcp_pcb* newpcb, int8_t err);
    static int8_t _s_accepted(void *arg, newAsyncClient* client);

  protected:
    uint16_t _port;
    IPAddress _addr;
    bool _noDelay;
    new_tcp_pcb* _pcb;
    newAcConnectHandler _connect_cb;
    void* _connect_cb_arg;

    int8_t _accept(new_tcp_pcb* newpcb, int8_t err);
    int8_t _accepted(newAsyncClient* client);
};

#endif /* ASYNCTCP_H_ */
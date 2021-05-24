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

#include "Arduino.h"

#include "newAsyncTCP.h"

#include "rpcWiFi.h"

#include "esp/esp_err.h"

extern "C"{
#include "new_lwip/opt.h"
#include "new_lwip/tcp.h"
#include "new_lwip/inet.h"
#include "new_lwip/dns.h"
#include "new_lwip/err.h"
}
#include "esp_task_wdt.h"

/*
 * TCP/IP Event Task
 * */

typedef enum : uint8_t {
    NEW_LWIP_TCP_SENT, NEW_LWIP_TCP_RECV, NEW_LWIP_TCP_FIN, NEW_LWIP_TCP_ERROR, NEW_LWIP_TCP_POLL, NEW_LWIP_TCP_CLEAR, NEW_LWIP_TCP_ACCEPT, NEW_LWIP_TCP_CONNECTED, NEW_LWIP_TCP_DNS
} new_lwip_event_t;

typedef struct {
        new_lwip_event_t event;
        void *arg;
        union {
                struct {
                        void * pcb;
                        int8_t err;
                } connected;
                struct {
                        int8_t err;
                } error;
                struct {
                        new_tcp_pcb * pcb;
                        uint16_t len;
                } sent;
                struct {
                        new_tcp_pcb * pcb;
                        pbuf * pb;
                        int8_t err;
                } recv;
                struct {
                        new_tcp_pcb * pcb;
                        int8_t err;
                } fin;
                struct {
                        new_tcp_pcb * pcb;
                } poll;
                struct {
                        newAsyncClient * client;
                } accept;
                struct {
                        const char * name;
                        new_ip_addr_t addr;
                } dns;
        };
} new_lwip_event_packet_t;

static xQueueHandle _new_async_queue;
static TaskHandle_t _new_async_service_task_handle = NULL;

SemaphoreHandle_t _new_slots_lock;
const int _new_number_of_closed_slots = CONFIG_LWIP_MAX_ACTIVE_TCP;
static int _new_closed_slots[_new_number_of_closed_slots];
static int _new_closed_index = []() {
    _new_slots_lock = xSemaphoreCreateBinary();
    xSemaphoreGive(_new_slots_lock);
    for (int i = 0; i < _new_number_of_closed_slots; ++ i) {
        _new_closed_slots[i] = 1;
    }
    return 1;
}();


static inline bool _new_init_async_event_queue(){
    if(!_new_async_queue){
        _new_async_queue = xQueueCreate(32, sizeof(new_lwip_event_packet_t *));
        if(!_new_async_queue){
            return false;
        }
    }
    return true;
}

static inline bool _new_send_async_event(new_lwip_event_packet_t ** e){
    return _new_async_queue && xQueueSend(_new_async_queue, e, portMAX_DELAY) == pdPASS;
}

static inline bool _new_prepend_async_event(new_lwip_event_packet_t ** e){
    return _new_async_queue && xQueueSendToFront(_new_async_queue, e, portMAX_DELAY) == pdPASS;
}

static inline bool _new_get_async_event(new_lwip_event_packet_t ** e){
    return _new_async_queue && xQueueReceive(_new_async_queue, e, portMAX_DELAY) == pdPASS;
}

static bool _new_remove_events_with_arg(void * arg){
    new_lwip_event_packet_t * first_packet = NULL;
    new_lwip_event_packet_t * packet = NULL;

    if(!_new_async_queue){
        return false;
    }
    //figure out which is the first packet so we can keep the order
    while(!first_packet){
        if(xQueueReceive(_new_async_queue, &first_packet, 0) != pdPASS){
            return false;
        }
        //discard packet if matching
        if((int)first_packet->arg == (int)arg){
            free(first_packet);
            first_packet = NULL;
        //return first packet to the back of the queue
        } else if(xQueueSend(_new_async_queue, &first_packet, portMAX_DELAY) != pdPASS){
            return false;
        }
    }

    while(xQueuePeek(_new_async_queue, &packet, 0) == pdPASS && packet != first_packet){
        if(xQueueReceive(_new_async_queue, &packet, 0) != pdPASS){
            return false;
        }
        if((int)packet->arg == (int)arg){
            free(packet);
            packet = NULL;
        } else if(xQueueSend(_new_async_queue, &packet, portMAX_DELAY) != pdPASS){
            return false;
        }
    }
    return true;
}

static void _new_handle_async_event(new_lwip_event_packet_t * e){
    if(e->event == NEW_LWIP_TCP_CLEAR){
        _new_remove_events_with_arg(e->arg);
    } else if(e->event == NEW_LWIP_TCP_RECV){
        log_d("-R: 0x%08x\n", e->recv.pcb);
        newAsyncClient::_s_recv(e->arg, e->recv.pcb, e->recv.pb, e->recv.err);
    } else if(e->event == NEW_LWIP_TCP_FIN){
        log_d("-F: 0x%08x\n", e->fin.pcb);
        newAsyncClient::_s_fin(e->arg, e->fin.pcb, e->fin.err);
    } else if(e->event == NEW_LWIP_TCP_SENT){
        log_d("-S: 0x%08x\n", e->sent.pcb);
        newAsyncClient::_s_sent(e->arg, e->sent.pcb, e->sent.len);
    } else if(e->event == NEW_LWIP_TCP_POLL){
        log_d("-P: 0x%08x\n", e->poll.pcb);
        newAsyncClient::_s_poll(e->arg, e->poll.pcb);
    } else if(e->event == NEW_LWIP_TCP_ERROR){
        log_d("-E: 0x%08x %d\n", e->arg, e->error.err);
        newAsyncClient::_s_error(e->arg, e->error.err);
    } else if(e->event == NEW_LWIP_TCP_CONNECTED){
        log_d("C: 0x%08x 0x%08x %d\n", e->arg, e->connected.pcb, e->connected.err);
        newAsyncClient::_s_connected(e->arg, e->connected.pcb, e->connected.err);
    } else if(e->event == NEW_LWIP_TCP_ACCEPT){
        log_d("A: 0x%08x 0x%08x\n", e->arg, e->accept.client);
        newAsyncServer::_s_accepted(e->arg, e->accept.client);
    } else if(e->event == NEW_LWIP_TCP_DNS){
        log_d("D: 0x%08x %s = %s\n", e->arg, e->dns.name, ipaddr_ntoa(&e->dns.addr));
        newAsyncClient::_s_dns_found(e->dns.name, &e->dns.addr, e->arg);
    }
    free((void*)(e));
}

static void _new_async_service_task(void *pvParameters) {
    new_lwip_event_packet_t * packet = NULL;
    for (;;) {
        //delay(10);
        if(_new_get_async_event(&packet)){
#if NEW_CONFIG_ASYNC_TCP_USE_WDT
            if(esp_task_wdt_add(NULL) != ESP_OK){
                log_e("Failed to add async task to WDT");
            }
#endif
            _new_handle_async_event(packet);
            //delay(10);
#if NEW_CONFIG_ASYNC_TCP_USE_WDT
            if(esp_task_wdt_delete(NULL) != ESP_OK){
                log_e("Failed to remove loop task from WDT");
            }
#endif
        }
    }
    vTaskDelete(NULL);
    _new_async_service_task_handle = NULL;
}
/*
static void _stop_async_task(){
    if(_new_async_service_task_handle){
        vTaskDelete(_new_async_service_task_handle);
        _new_async_service_task_handle = NULL;
    }
}
*/
static bool _new_start_async_task(){
    log_w("Started new async task in newAsyncTCP");
    if(!_new_init_async_event_queue()){
        return false;
    }
    if(!_new_async_service_task_handle){
        xTaskCreateUniversal(_new_async_service_task, "async_tcp", 8192 * 2, NULL, 3, &_new_async_service_task_handle, NEW_CONFIG_ASYNC_TCP_RUNNING_CORE);
        if(!_new_async_service_task_handle){
            return false;
        }
    }
    return true;
}

/*
 * LwIP Callbacks
 * */

static int8_t _new_tcp_clear_events(void * arg) {
    new_lwip_event_packet_t * e = (new_lwip_event_packet_t *)malloc(sizeof(new_lwip_event_packet_t));
    e->event = NEW_LWIP_TCP_CLEAR;
    e->arg = arg;
    if (!_new_prepend_async_event(&e)) {
        free((void*)(e));
    }
    return ERR_OK;
}

static int8_t _new_tcp_connected(void * arg, new_tcp_pcb * pcb, int8_t err) {
    //ets_printf("+C: 0x%08x\n", pcb);
    new_lwip_event_packet_t * e = (new_lwip_event_packet_t *)malloc(sizeof(new_lwip_event_packet_t));
    e->event = NEW_LWIP_TCP_CONNECTED;
    e->arg = arg;
    e->connected.pcb = pcb;
    e->connected.err = err;
    if (!_new_prepend_async_event(&e)) {
        free((void*)(e));
    }
    return ERR_OK;
}

static int8_t _new_tcp_poll(void * arg, struct new_tcp_pcb * pcb) {
    //ets_printf("+P: 0x%08x\n", pcb);
    new_lwip_event_packet_t * e = (new_lwip_event_packet_t *)malloc(sizeof(new_lwip_event_packet_t));
    e->event = NEW_LWIP_TCP_POLL;
    e->arg = arg;
    e->poll.pcb = pcb;
    if (!_new_send_async_event(&e)) {
        free((void*)(e));
    }
    return ERR_OK;
}

static int8_t _new_tcp_recv(void * arg, struct new_tcp_pcb * pcb, struct pbuf *pb, int8_t err) {
    new_lwip_event_packet_t * e = (new_lwip_event_packet_t *)malloc(sizeof(new_lwip_event_packet_t));
    e->arg = arg;
    if(pb){
        //ets_printf("+R: 0x%08x\n", pcb);
        e->event = NEW_LWIP_TCP_RECV;
        e->recv.pcb = pcb;
        e->recv.pb = pb;
        e->recv.err = err;
    } else {
        //ets_printf("+F: 0x%08x\n", pcb);
        e->event = NEW_LWIP_TCP_FIN;
        e->fin.pcb = pcb;
        e->fin.err = err;
        //close the PCB in LwIP thread
        newAsyncClient::_s_lwip_fin(e->arg, e->fin.pcb, e->fin.err);
    }
    if (!_new_send_async_event(&e)) {
        free((void*)(e));
    }
    return ERR_OK;
}

static int8_t _new_tcp_sent(void * arg, struct new_tcp_pcb * pcb, uint16_t len) {
    //ets_printf("+S: 0x%08x\n", pcb);
    new_lwip_event_packet_t * e = (new_lwip_event_packet_t *)malloc(sizeof(new_lwip_event_packet_t));
    e->event = NEW_LWIP_TCP_SENT;
    e->arg = arg;
    e->sent.pcb = pcb;
    e->sent.len = len;
    if (!_new_send_async_event(&e)) {
        free((void*)(e));
    }
    return ERR_OK;
}

static void _new_tcp_error(void * arg, int8_t err) {
    log_d("+E: 0x%08x\n", arg);
    new_lwip_event_packet_t * e = (new_lwip_event_packet_t *)malloc(sizeof(new_lwip_event_packet_t));
    e->event = NEW_LWIP_TCP_ERROR;
    e->arg = arg;
    e->error.err = err;
    if (!_new_send_async_event(&e)) {
        free((void*)(e));
    }
}

static void _new_tcp_dns_found(const char * name, struct new_ip_addr * ipaddr, void * arg) {
    new_lwip_event_packet_t * e = (new_lwip_event_packet_t *)malloc(sizeof(new_lwip_event_packet_t));
    log_d("+DNS: name=%s ipaddr=0x%08x arg=%x\n", name, ipaddr, arg);
    e->event = NEW_LWIP_TCP_DNS;
    e->arg = arg;
    e->dns.name = name;
    if (ipaddr) {
        memcpy(&e->dns.addr, ipaddr, sizeof(struct new_ip_addr));
    } else {
        memset(&e->dns.addr, 0, sizeof(e->dns.addr));
    }
    if (!_new_send_async_event(&e)) {
        free((void*)(e));
    }
}

//Used to switch out from LwIP thread
static int8_t _new_tcp_accept(void * arg, newAsyncClient * client) {
    new_lwip_event_packet_t * e = (new_lwip_event_packet_t *)malloc(sizeof(new_lwip_event_packet_t));
    e->event = NEW_LWIP_TCP_ACCEPT;
    e->arg = arg;
    e->accept.client = client;
    if (!_new_prepend_async_event(&e)) {
        free((void*)(e));
    }
    return ERR_OK;
}

/*
 * TCP/IP API Calls
 * */

#include "new_lwip/priv/tcpip_priv.h"

typedef struct {
    struct new_tcpip_api_call_data call;
    new_tcp_pcb * pcb;
    int8_t closed_slot;
    int8_t err;
    union {
            struct {
                    const char* data;
                    size_t size;
                    uint8_t apiflags;
            } write;
            size_t received;
            struct {
                    new_ip_addr_t * addr;
                    uint16_t port;
                    tcp_connected_fn cb;
            } connect;
            struct {
                    new_ip_addr_t * addr;
                    uint16_t port;
            } bind;
            uint8_t backlog;
    };
} new_tcp_api_call_t;

static err_t _new_tcp_output_api(struct new_tcpip_api_call_data *api_call_msg){
    new_tcp_api_call_t * msg = (new_tcp_api_call_t *)api_call_msg;
    msg->err = ERR_CONN;
    if(msg->closed_slot == -1 || !_new_closed_slots[msg->closed_slot]) {
        msg->err = new_tcp_output(msg->pcb);
    }
    return msg->err;
}

static rpc_esp_err_t _new_tcp_output(new_tcp_pcb * pcb, int8_t closed_slot) {
    if(!pcb){
        return ERR_CONN;
    }
    new_tcp_api_call_t msg;
    msg.pcb = pcb;
    msg.closed_slot = closed_slot;
    new_tcpip_api_call(_new_tcp_output_api, (struct new_tcpip_api_call_data*)&msg);
    return msg.err;
}

static err_t _new_tcp_write_api(struct new_tcpip_api_call_data *api_call_msg){
    new_tcp_api_call_t * msg = (new_tcp_api_call_t *)api_call_msg;
    msg->err = ERR_CONN;
    if(msg->closed_slot == -1 || !_new_closed_slots[msg->closed_slot]) {
        msg->err = new_tcp_write(msg->pcb, msg->write.data, msg->write.size, msg->write.apiflags);
    }
    return msg->err;
}

static rpc_esp_err_t _new_tcp_write(new_tcp_pcb * pcb, int8_t closed_slot, const char* data, size_t size, uint8_t apiflags) {
    if(!pcb){
        return ERR_CONN;
    }
    new_tcp_api_call_t msg;
    msg.pcb = pcb;
    msg.closed_slot = closed_slot;
    msg.write.data = data;
    msg.write.size = size;
    msg.write.apiflags = apiflags;
    new_tcpip_api_call(_new_tcp_write_api, (struct new_tcpip_api_call_data*)&msg);
    return msg.err;
}

static err_t _new_tcp_recved_api(struct new_tcpip_api_call_data *api_call_msg){
    new_tcp_api_call_t * msg = (new_tcp_api_call_t *)api_call_msg;
    msg->err = ERR_CONN;
    if(msg->closed_slot == -1 || !_new_closed_slots[msg->closed_slot]) {
        msg->err = 0;
        new_tcp_recved(msg->pcb, msg->received);
    }
    return msg->err;
}

static rpc_esp_err_t _new_tcp_recved(new_tcp_pcb * pcb, int8_t closed_slot, size_t len) {
    if(!pcb){
        return ERR_CONN;
    }
    new_tcp_api_call_t msg;
    msg.pcb = pcb;
    msg.closed_slot = closed_slot;
    msg.received = len;
    new_tcpip_api_call(_new_tcp_recved_api, (struct new_tcpip_api_call_data*)&msg);
    return msg.err;
}

static err_t _new_tcp_close_api(struct new_tcpip_api_call_data *api_call_msg){
    new_tcp_api_call_t * msg = (new_tcp_api_call_t *)api_call_msg;
    msg->err = ERR_CONN;
    if(msg->closed_slot == -1 || !_new_closed_slots[msg->closed_slot]) {
        msg->err = new_tcp_close(msg->pcb);
    }
    return msg->err;
}

static rpc_esp_err_t _new_tcp_close(new_tcp_pcb * pcb, int8_t closed_slot) {
    if(!pcb){
        return ERR_CONN;
    }
    new_tcp_api_call_t msg;
    msg.pcb = pcb;
    msg.closed_slot = closed_slot;
    new_tcpip_api_call(_new_tcp_close_api, (struct new_tcpip_api_call_data*)&msg);
    return msg.err;
}

static err_t _new_tcp_abort_api(struct new_tcpip_api_call_data *api_call_msg){
    new_tcp_api_call_t * msg = (new_tcp_api_call_t *)api_call_msg;
    msg->err = ERR_CONN;
    if(msg->closed_slot == -1 || !_new_closed_slots[msg->closed_slot]) {
        new_tcp_abort(msg->pcb);
    }
    return msg->err;
}

static rpc_esp_err_t _new_tcp_abort(new_tcp_pcb * pcb, int8_t closed_slot) {
    if(!pcb){
        return ERR_CONN;
    }
    new_tcp_api_call_t msg;
    msg.pcb = pcb;
    msg.closed_slot = closed_slot;
    new_tcpip_api_call(_new_tcp_abort_api, (struct new_tcpip_api_call_data*)&msg);
    return msg.err;
}

static err_t _new_tcp_connect_api(struct new_tcpip_api_call_data *api_call_msg){
    new_tcp_api_call_t * msg = (new_tcp_api_call_t *)api_call_msg;
    msg->err = new_tcp_connect(msg->pcb, msg->connect.addr, msg->connect.port, msg->connect.cb);
    return msg->err;
}

static rpc_esp_err_t _new_tcp_connect(new_tcp_pcb * pcb, int8_t closed_slot, new_ip_addr_t * addr, uint16_t port, tcp_connected_fn cb) {
    if(!pcb){
        return ESP_FAIL;
    }
    new_tcp_api_call_t msg;
    msg.pcb = pcb;
    msg.closed_slot = closed_slot;
    msg.connect.addr = addr;
    msg.connect.port = port;
    msg.connect.cb = cb;
    new_tcpip_api_call(_new_tcp_connect_api, (struct new_tcpip_api_call_data*)&msg);
    return msg.err;
}

static err_t _new_tcp_bind_api(struct new_tcpip_api_call_data *api_call_msg){
    new_tcp_api_call_t * msg = (new_tcp_api_call_t *)api_call_msg;
    msg->err = new_tcp_bind(msg->pcb, msg->bind.addr, msg->bind.port);
    return msg->err;
}

static rpc_esp_err_t _new_tcp_bind(new_tcp_pcb * pcb, new_ip_addr_t * addr, uint16_t port) {
    if(!pcb){
        return ESP_FAIL;
    }
    new_tcp_api_call_t msg;
    msg.pcb = pcb;
    msg.closed_slot = -1;
    msg.bind.addr = addr;
    msg.bind.port = port;
    new_tcpip_api_call(_new_tcp_bind_api, (struct new_tcpip_api_call_data*)&msg);
    return msg.err;
}

static err_t _new_tcp_listen_api(struct new_tcpip_api_call_data *api_call_msg){
    new_tcp_api_call_t * msg = (new_tcp_api_call_t *)api_call_msg;
    msg->err = 0;
    msg->pcb = new_tcp_listen_with_backlog(msg->pcb, msg->backlog);
    return msg->err;
}

static new_tcp_pcb * _new_tcp_listen_with_backlog(new_tcp_pcb * pcb, uint8_t backlog) {
    if(!pcb){
        return NULL;
    }
    new_tcp_api_call_t msg;
    msg.pcb = pcb;
    msg.closed_slot = -1;
    msg.backlog = backlog?backlog:0xFF;
    new_tcpip_api_call(_new_tcp_listen_api, (struct new_tcpip_api_call_data*)&msg);
    return msg.pcb;
}



/*
  Async TCP Client
 */

newAsyncClient::newAsyncClient(new_tcp_pcb* pcb)
: _connect_cb(0)
, _connect_cb_arg(0)
, _discard_cb(0)
, _discard_cb_arg(0)
, _sent_cb(0)
, _sent_cb_arg(0)
, _error_cb(0)
, _error_cb_arg(0)
, _recv_cb(0)
, _recv_cb_arg(0)
, _pb_cb(0)
, _pb_cb_arg(0)
, _timeout_cb(0)
, _timeout_cb_arg(0)
, _pcb_busy(false)
, _pcb_sent_at(0)
, _ack_pcb(true)
, _rx_last_packet(0)
, _rx_since_timeout(0)
, _ack_timeout(ASYNC_MAX_ACK_TIME)
, _connect_port(0)
, prev(NULL)
, next(NULL)
{
    _pcb = pcb;
    _closed_slot = -1;
    if(_pcb){
        xSemaphoreTake(_new_slots_lock, portMAX_DELAY);
        int closed_slot_min_index = 0;
        for (int i = 0; i < _new_number_of_closed_slots; ++ i) {
            if ((_closed_slot == -1 || _new_closed_slots[i] <= closed_slot_min_index) && _new_closed_slots[i] != 0) {
                closed_slot_min_index = _new_closed_slots[i];
                _closed_slot = i;
            }
        }
        _new_closed_slots[_closed_slot] = 0;
        xSemaphoreGive(_new_slots_lock);

        _rx_last_packet = millis();
        new_tcp_arg(_pcb, this);
        new_tcp_recv(_pcb, &_new_tcp_recv);
        new_tcp_sent(_pcb, &_new_tcp_sent);
        new_tcp_err(_pcb, &_new_tcp_error);
        new_tcp_poll(_pcb, &_new_tcp_poll, 1);
    }
}

newAsyncClient::~newAsyncClient(){
    if(_pcb) {
        _close();
    }
}

/*
 * Operators
 * */

newAsyncClient& newAsyncClient::operator=(const newAsyncClient& other){
    if (_pcb) {
        _close();
    }

    _pcb = other._pcb;
    _closed_slot = other._closed_slot;
    if (_pcb) {
        _rx_last_packet = millis();
        new_tcp_arg(_pcb, this);
        new_tcp_recv(_pcb, &_new_tcp_recv);
        new_tcp_sent(_pcb, &_new_tcp_sent);
        new_tcp_err(_pcb, &_new_tcp_error);
        new_tcp_poll(_pcb, &_new_tcp_poll, 1);
    }
    return *this;
}

bool newAsyncClient::operator==(const newAsyncClient &other) {
    return _pcb == other._pcb;
}

newAsyncClient & newAsyncClient::operator+=(const newAsyncClient &other) {
    if(next == NULL){
        next = (newAsyncClient*)(&other);
        next->prev = this;
    } else {
        newAsyncClient *c = next;
        while(c->next != NULL) {
            c = c->next;
        }
        c->next =(newAsyncClient*)(&other);
        c->next->prev = c;
    }
    return *this;
}

/*
 * Callback Setters
 * */

void newAsyncClient::onConnect(newAcConnectHandler cb, void* arg){
    _connect_cb = cb;
    _connect_cb_arg = arg;
}

void newAsyncClient::onDisconnect(newAcConnectHandler cb, void* arg){
    _discard_cb = cb;
    _discard_cb_arg = arg;
}

void newAsyncClient::onAck(newAcAckHandler cb, void* arg){
    _sent_cb = cb;
    _sent_cb_arg = arg;
}

void newAsyncClient::onError(newAcErrorHandler cb, void* arg){
    _error_cb = cb;
    _error_cb_arg = arg;
}

void newAsyncClient::onData(newAcDataHandler cb, void* arg){
    _recv_cb = cb;
    _recv_cb_arg = arg;
}

void newAsyncClient::onPacket(newAcPacketHandler cb, void* arg){
  _pb_cb = cb;
  _pb_cb_arg = arg;
}

void newAsyncClient::onTimeout(newAcTimeoutHandler cb, void* arg){
    _timeout_cb = cb;
    _timeout_cb_arg = arg;
}

void newAsyncClient::onPoll(newAcConnectHandler cb, void* arg){
    _poll_cb = cb;
    _poll_cb_arg = arg;
}

/*
 * Main Public Methods
 * */

bool newAsyncClient::connect(IPAddress ip, uint16_t port){
    if (_pcb){
        log_w("already connected, state %d", _pcb->state);
        return false;
    }
    log_d("new start async task from ip port connect");
    if(!_new_start_async_task()){
        log_e("failed to start task");
        return false;
    }

    new_ip_addr_t addr;
    addr.type = NEW_IPADDR_TYPE_V4;
    addr.u_addr.ip4.addr = ip;

    new_tcp_pcb* pcb = new_tcp_new_ip_type(NEW_IPADDR_TYPE_V4);
    if (!pcb){
        log_e("pcb == NULL");
        return false;
    }

    new_tcp_arg(pcb, this);
    new_tcp_err(pcb, &_new_tcp_error);
    new_tcp_recv(pcb, &_new_tcp_recv);
    new_tcp_sent(pcb, &_new_tcp_sent);
    new_tcp_poll(pcb, &_new_tcp_poll, 1);
    //_new_tcp_connect(pcb, &addr, port,(tcp_connected_fn)&_s_connected);
    _new_tcp_connect(pcb, _closed_slot, &addr, port,(tcp_connected_fn)&_new_tcp_connected);
    return true;
}

bool newAsyncClient::connect(const char* host, uint16_t port){
    new_ip_addr_t addr;
    
    log_d("new async task inside host port connect");
    if(!_new_start_async_task()){
      Serial.println("failed to start task");
      log_e("failed to start task");
      return false;
    }
    
    err_t err = new_dns_gethostbyname(host, &addr, (dns_found_callback)&_new_tcp_dns_found, this);
    if(err == ERR_OK) {
        log_d("calling connect after new_dns_gethostbyname");
        return connect(IPAddress(addr.u_addr.ip4.addr), port);
    } else if(err == ERR_INPROGRESS) {
        _connect_port = port;
        return true;
    }
    log_e("error: %d", err);
    return false;
}

void newAsyncClient::close(bool now){
    if(_pcb){
        _new_tcp_recved(_pcb, _closed_slot, _rx_ack_len);
    }
    _close();
}

int8_t newAsyncClient::abort(){
    if(_pcb) {
        _new_tcp_abort(_pcb, _closed_slot );
        _pcb = NULL;
    }
    return ERR_ABRT;
}

size_t newAsyncClient::space(){
    if((_pcb != NULL) && (_pcb->state == 4)){
        return tcp_sndbuf(_pcb);
    }
    return 0;
}

size_t newAsyncClient::add(const char* data, size_t size, uint8_t apiflags) {
    if(!_pcb || size == 0 || data == NULL) {
        return 0;
    }
    size_t room = space();
    if(!room) {
        return 0;
    }
    size_t will_send = (room < size) ? room : size;
    int8_t err = ERR_OK;
    err = _new_tcp_write(_pcb, _closed_slot, data, will_send, apiflags);
    if(err != ERR_OK) {
        return 0;
    }
    return will_send;
}

bool newAsyncClient::send(){
    int8_t err = ERR_OK;
    err = _new_tcp_output(_pcb, _closed_slot);
    if(err == ERR_OK){
        _pcb_sent_at = millis();
        _pcb_busy = true;
        return true;
    }
    return false;
}

size_t newAsyncClient::ack(size_t len){
    if(len > _rx_ack_len)
        len = _rx_ack_len;
    if(len){
        _new_tcp_recved(_pcb, _closed_slot, len);
    }
    _rx_ack_len -= len;
    return len;
}

void newAsyncClient::ackPacket(struct pbuf * pb){
  if(!pb){
    return;
  }
  _new_tcp_recved(_pcb, _closed_slot, pb->len);
  new_pbuf_free(pb);
}

/*
 * Main Private Methods
 * */

int8_t newAsyncClient::_close(){
    log_d("X: 0x%08x\n", (uint32_t)this);
    int8_t err = ERR_OK;
    if(_pcb) {
        //log_i("");
        new_tcp_arg(_pcb, NULL);
        new_tcp_sent(_pcb, NULL);
        new_tcp_recv(_pcb, NULL);
        new_tcp_err(_pcb, NULL);
        new_tcp_poll(_pcb, NULL, 0);
        _new_tcp_clear_events(this);
        err = _new_tcp_close(_pcb, _closed_slot);
        if(err != ERR_OK) {
            err = abort();
        }
        _pcb = NULL;
        if(_discard_cb) {
            _discard_cb(_discard_cb_arg, this);
        }
    }
    return err;
}

/*
 * Private Callbacks
 * */

int8_t newAsyncClient::_connected(void* pcb, int8_t err){
    _pcb = reinterpret_cast<new_tcp_pcb*>(pcb);
    if(_pcb){
        _rx_last_packet = millis();
        _pcb_busy = false;
//        new_tcp_recv(_pcb, &_new_tcp_recv);
//        new_tcp_sent(_pcb, &_new_tcp_sent);
//        new_tcp_poll(_pcb, &_new_tcp_poll, 1);
    }
    if(_connect_cb) {
        _connect_cb(_connect_cb_arg, this);
    }
    return ERR_OK;
}

void newAsyncClient::_error(int8_t err) {
    if(_pcb){
        new_tcp_arg(_pcb, NULL);
        new_tcp_sent(_pcb, NULL);
        new_tcp_recv(_pcb, NULL);
        new_tcp_err(_pcb, NULL);
        new_tcp_poll(_pcb, NULL, 0);
        _pcb = NULL;
    }
    if(_error_cb) {
        _error_cb(_error_cb_arg, this, err);
    }
    if(_discard_cb) {
        _discard_cb(_discard_cb_arg, this);
    }
}

//In LwIP Thread
int8_t newAsyncClient::_lwip_fin(new_tcp_pcb* pcb, int8_t err) {
    if(!_pcb || pcb != _pcb){
        log_e("0x%08x != 0x%08x", (uint32_t)pcb, (uint32_t)_pcb);
        return ERR_OK;
    }
    new_tcp_arg(_pcb, NULL);
    new_tcp_sent(_pcb, NULL);
    new_tcp_recv(_pcb, NULL);
    new_tcp_err(_pcb, NULL);
    new_tcp_poll(_pcb, NULL, 0);
    if(new_tcp_close(_pcb) != ERR_OK) {
        new_tcp_abort(_pcb);
    }
    _new_closed_slots[_closed_slot] = _new_closed_index;
    ++ _new_closed_index;
    _pcb = NULL;
    return ERR_OK;
}

//In Async Thread
int8_t newAsyncClient::_fin(new_tcp_pcb* pcb, int8_t err) {
    _new_tcp_clear_events(this);
    if(_discard_cb) {
        _discard_cb(_discard_cb_arg, this);
    }
    return ERR_OK;
}

int8_t newAsyncClient::_sent(new_tcp_pcb* pcb, uint16_t len) {
    _rx_last_packet = millis();
    log_i("%u", len);
    _pcb_busy = false;
    if(_sent_cb) {
        _sent_cb(_sent_cb_arg, this, len, (millis() - _pcb_sent_at));
    }
    return ERR_OK;
}

int8_t newAsyncClient::_recv(new_tcp_pcb* pcb, pbuf* pb, int8_t err) {
    if(pcb != _pcb){
        log_e("0x%08x != 0x%08x", (uint32_t)pcb, (uint32_t)_pcb);
        return ERR_OK;
    }
    while(pb != NULL) {
        _rx_last_packet = millis();
        //we should not ack before we assimilate the data
        _ack_pcb = true;
        pbuf *b = pb;
        pb = b->next;
        b->next = NULL;
        if(_pb_cb){
            _pb_cb(_pb_cb_arg, this, b);
        } else {
            if(_recv_cb) {
                _recv_cb(_recv_cb_arg, this, b->payload, b->len);
            }
            if(!_ack_pcb) {
                _rx_ack_len += b->len;
            } else if(_pcb) {
                _new_tcp_recved(_pcb, _closed_slot, b->len);
            }
            new_pbuf_free(b);
        }
    }
    return ERR_OK;
}

int8_t newAsyncClient::_poll(new_tcp_pcb* pcb){
    if(!_pcb){
        log_w("pcb is NULL");
        return ERR_OK;
    }
    if(pcb != _pcb){
        log_e("0x%08x != 0x%08x", (uint32_t)pcb, (uint32_t)_pcb);
        return ERR_OK;
    }

    uint32_t now = millis();

    // ACK Timeout
    if(_pcb_busy && _ack_timeout && (now - _pcb_sent_at) >= _ack_timeout){
        _pcb_busy = false;
        log_w("ack timeout %d", pcb->state);
        if(_timeout_cb)
            _timeout_cb(_timeout_cb_arg, this, (now - _pcb_sent_at));
        return ERR_OK;
    }
    // RX Timeout
    if(_rx_since_timeout && (now - _rx_last_packet) >= (_rx_since_timeout * 1000)){
        log_w("rx timeout %d", pcb->state);
        _close();
        return ERR_OK;
    }
    // Everything is fine
    if(_poll_cb) {
        _poll_cb(_poll_cb_arg, this);
    }
    return ERR_OK;
}

void newAsyncClient::_dns_found(struct new_ip_addr *ipaddr){
    if(ipaddr && ipaddr->u_addr.ip4.addr){
        log_d("calling connect from dns found");
        //connect(IPAddress(ipaddr->u_addr.ip4.addr), _connect_port);
    } else {
        if(_error_cb) {
            _error_cb(_error_cb_arg, this, -55);
        }
        if(_discard_cb) {
            _discard_cb(_discard_cb_arg, this);
        }
    }
}

/*
 * Public Helper Methods
 * */

void newAsyncClient::stop() {
    close(false);
}

bool newAsyncClient::free(){
    if(!_pcb) {
        return true;
    }
    if(_pcb->state == 0 || _pcb->state > 4) {
        return true;
    }
    return false;
}

size_t newAsyncClient::write(const char* data) {
    if(data == NULL) {
        return 0;
    }
    return write(data, strlen(data));
}

size_t newAsyncClient::write(const char* data, size_t size, uint8_t apiflags) {
    size_t will_send = add(data, size, apiflags);
    if(!will_send || !send()) {
        return 0;
    }
    return will_send;
}

void newAsyncClient::setRxTimeout(uint32_t timeout){
    _rx_since_timeout = timeout;
}

uint32_t newAsyncClient::getRxTimeout(){
    return _rx_since_timeout;
}

uint32_t newAsyncClient::getAckTimeout(){
    return _ack_timeout;
}

void newAsyncClient::setAckTimeout(uint32_t timeout){
    _ack_timeout = timeout;
}

void newAsyncClient::setNoDelay(bool nodelay){
    if(!_pcb) {
        return;
    }
    if(nodelay) {
        tcp_nagle_disable(_pcb);
    } else {
        tcp_nagle_enable(_pcb);
    }
}

bool newAsyncClient::getNoDelay(){
    if(!_pcb) {
        return false;
    }
    return tcp_nagle_disabled(_pcb);
}

uint16_t newAsyncClient::getMss(){
    if(!_pcb) {
        return 0;
    }
    return tcp_mss(_pcb);
}

uint32_t newAsyncClient::getRemoteAddress() {
    if(!_pcb) {
        return 0;
    }
    return _pcb->remote_ip.u_addr.ip4.addr;
}

uint16_t newAsyncClient::getRemotePort() {
    if(!_pcb) {
        return 0;
    }
    return _pcb->remote_port;
}

uint32_t newAsyncClient::getLocalAddress() {
    if(!_pcb) {
        return 0;
    }
    return _pcb->local_ip.u_addr.ip4.addr;
}

uint16_t newAsyncClient::getLocalPort() {
    if(!_pcb) {
        return 0;
    }
    return _pcb->local_port;
}

IPAddress newAsyncClient::remoteIP() {
    return IPAddress(getRemoteAddress());
}

uint16_t newAsyncClient::remotePort() {
    return getRemotePort();
}

IPAddress newAsyncClient::localIP() {
    return IPAddress(getLocalAddress());
}

uint16_t newAsyncClient::localPort() {
    return getLocalPort();
}

uint8_t newAsyncClient::state() {
    if(!_pcb) {
        return 0;
    }
    return _pcb->state;
}

bool newAsyncClient::connected(){
    if (!_pcb) {
        return false;
    }
    return _pcb->state == 4;
}

bool newAsyncClient::connecting(){
    if (!_pcb) {
        return false;
    }
    return _pcb->state > 0 && _pcb->state < 4;
}

bool newAsyncClient::disconnecting(){
    if (!_pcb) {
        return false;
    }
    return _pcb->state > 4 && _pcb->state < 10;
}

bool newAsyncClient::disconnected(){
    if (!_pcb) {
        return true;
    }
    return _pcb->state == 0 || _pcb->state == 10;
}

bool newAsyncClient::freeable(){
    if (!_pcb) {
        return true;
    }
    return _pcb->state == 0 || _pcb->state > 4;
}

bool newAsyncClient::canSend(){
    return space() > 0;
}

const char * newAsyncClient::errorToString(int8_t error){
    switch(error){
        case ERR_OK: return "OK";
        case ERR_MEM: return "Out of memory error";
        case ERR_BUF: return "Buffer error";
        case ERR_TIMEOUT: return "Timeout";
        case ERR_RTE: return "Routing problem";
        case ERR_INPROGRESS: return "Operation in progress";
        case ERR_VAL: return "Illegal value";
        case ERR_WOULDBLOCK: return "Operation would block";
        case ERR_USE: return "Address in use";
        case ERR_ALREADY: return "Already connected";
        case ERR_CONN: return "Not connected";
        case ERR_IF: return "Low-level netif error";
        case ERR_ABRT: return "Connection aborted";
        case ERR_RST: return "Connection reset";
        case ERR_CLSD: return "Connection closed";
        case ERR_ARG: return "Illegal argument";
        case -55: return "DNS failed";
        default: return "UNKNOWN";
    }
}

const char * newAsyncClient::stateToString(){
    switch(state()){
        case 0: return "Closed";
        case 1: return "Listen";
        case 2: return "SYN Sent";
        case 3: return "SYN Received";
        case 4: return "Established";
        case 5: return "FIN Wait 1";
        case 6: return "FIN Wait 2";
        case 7: return "Close Wait";
        case 8: return "Closing";
        case 9: return "Last ACK";
        case 10: return "Time Wait";
        default: return "UNKNOWN";
    }
}

/*
 * Static Callbacks (LwIP C2C++ interconnect)
 * */

void newAsyncClient::_s_dns_found(const char * name, struct new_ip_addr * ipaddr, void * arg){
    reinterpret_cast<newAsyncClient*>(arg)->_dns_found(ipaddr);
}

int8_t newAsyncClient::_s_poll(void * arg, struct new_tcp_pcb * pcb) {
    return reinterpret_cast<newAsyncClient*>(arg)->_poll(pcb);
}

int8_t newAsyncClient::_s_recv(void * arg, struct new_tcp_pcb * pcb, struct pbuf *pb, int8_t err) {
    return reinterpret_cast<newAsyncClient*>(arg)->_recv(pcb, pb, err);
}

int8_t newAsyncClient::_s_fin(void * arg, struct new_tcp_pcb * pcb, int8_t err) {
    return reinterpret_cast<newAsyncClient*>(arg)->_fin(pcb, err);
}

int8_t newAsyncClient::_s_lwip_fin(void * arg, struct new_tcp_pcb * pcb, int8_t err) {
    return reinterpret_cast<newAsyncClient*>(arg)->_lwip_fin(pcb, err);
}

int8_t newAsyncClient::_s_sent(void * arg, struct new_tcp_pcb * pcb, uint16_t len) {
    return reinterpret_cast<newAsyncClient*>(arg)->_sent(pcb, len);
}

void newAsyncClient::_s_error(void * arg, int8_t err) {
    reinterpret_cast<newAsyncClient*>(arg)->_error(err);
}

int8_t newAsyncClient::_s_connected(void * arg, void * pcb, int8_t err){
    return reinterpret_cast<newAsyncClient*>(arg)->_connected(pcb, err);
}

/*
  Async TCP Server
 */

newAsyncServer::newAsyncServer(IPAddress addr, uint16_t port)
: _port(port)
, _addr(addr)
, _noDelay(false)
, _pcb(0)
, _connect_cb(0)
, _connect_cb_arg(0)
{}

newAsyncServer::newAsyncServer(uint16_t port)
: _port(port)
, _addr((uint32_t) IPADDR_ANY)
, _noDelay(false)
, _pcb(0)
, _connect_cb(0)
, _connect_cb_arg(0)
{}

newAsyncServer::~newAsyncServer(){
    end();
}

void newAsyncServer::onClient(newAcConnectHandler cb, void* arg){
    _connect_cb = cb;
    _connect_cb_arg = arg;
}

void newAsyncServer::begin(){
    if(_pcb) {
        return;
    }

    if(!_new_start_async_task()){
        log_e("failed to start task");
        return;
    }
    int8_t err;
    _pcb = new_tcp_new_ip_type(NEW_IPADDR_TYPE_V4);
    if (!_pcb){
        log_e("_pcb == NULL");
        return;
    }

    new_ip_addr_t local_addr;
    local_addr.type = NEW_IPADDR_TYPE_V4;
    local_addr.u_addr.ip4.addr = (uint32_t) _addr;
    err = _new_tcp_bind(_pcb, &local_addr, _port);

    if (err != ERR_OK) {
        _new_tcp_close(_pcb, -1);
        log_e("bind error: %d", err);
        return;
    }

    static uint8_t backlog = 5;
    _pcb = _new_tcp_listen_with_backlog(_pcb, backlog);
    if (!_pcb) {
        log_e("listen_pcb == NULL");
        return;
    }
    new_tcp_arg(_pcb, (void*) this);
    new_tcp_accept(_pcb, &_s_accept);
}

void newAsyncServer::end(){
    if(_pcb){
        new_tcp_arg(_pcb, NULL);
        new_tcp_accept(_pcb, NULL);
        if(new_tcp_close(_pcb) != ERR_OK){
            _new_tcp_abort(_pcb, -1);
        }
        _pcb = NULL;
    }
}

//runs on LwIP thread
int8_t newAsyncServer::_accept(new_tcp_pcb* pcb, int8_t err){
    log_d("+A: 0x%08x\n", pcb);
    if(_connect_cb){
        newAsyncClient *c = new newAsyncClient(pcb);
        if(c){
            c->setNoDelay(_noDelay);
            return _new_tcp_accept(this, c);
        }
    }
    if(new_tcp_close(pcb) != ERR_OK){
        new_tcp_abort(pcb);
    }
    log_e("FAIL");
    return ERR_OK;
}

int8_t newAsyncServer::_accepted(newAsyncClient* client){
    if(_connect_cb){
        _connect_cb(_connect_cb_arg, client);
    }
    return ERR_OK;
}

void newAsyncServer::setNoDelay(bool nodelay){
    _noDelay = nodelay;
}

bool newAsyncServer::getNoDelay(){
    return _noDelay;
}

uint8_t newAsyncServer::status(){
    if (!_pcb) {
        return 0;
    }
    return _pcb->state;
}

int8_t newAsyncServer::_s_accept(void * arg, new_tcp_pcb * pcb, int8_t err){
    return reinterpret_cast<newAsyncServer*>(arg)->_accept(pcb, err);
}

int8_t newAsyncServer::_s_accepted(void *arg, newAsyncClient* client){
    return reinterpret_cast<newAsyncServer*>(arg)->_accepted(client);
}

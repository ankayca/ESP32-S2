
/*
esp32-websocket - a websocket component on esp-idf
Copyright (C) 2019 Blake Felt - blake.w.felt@gmail.com

This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "websocket_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>







#define AP_SSID "admin"
#define AP_PSSWD "password"
static QueueHandle_t client_queue;
const static int client_queue_size = 10;
void websocket_callback(uint8_t num,WEBSOCKET_TYPE_t type,char* msg,uint64_t len) ;


static SemaphoreHandle_t xwebsocket_mutex; // to lock the client array
static QueueHandle_t xwebsocket_queue; // to hold the clients that send messages
static ws_client_t clients[WEBSOCKET_SERVER_MAX_CLIENTS]; // holds list of clients
static TaskHandle_t xtask; // the task itself

static void background_callback(struct netconn* conn, enum netconn_evt evt,u16_t len) {
  switch(evt) {
    case NETCONN_EVT_RCVPLUS:
      xQueueSendToBack(xwebsocket_queue,&conn,WEBSOCKET_SERVER_QUEUE_TIMEOUT);
      break;
    default:
      break;
  }
}

static void handle_read(uint8_t num) {
  ws_header_t header;
  char* msg;

  header.received = 0;
  msg = ws_read(&clients[num],&header);

  if(!header.received) {
    if(msg) free(msg);
    return;
  }

  switch(clients[num].last_opcode) {
    case WEBSOCKET_OPCODE_CONT:
      break;
    case WEBSOCKET_OPCODE_BIN:
      clients[num].scallback(num,WEBSOCKET_BIN,msg,header.length);
      break;
    case WEBSOCKET_OPCODE_TEXT:
      clients[num].scallback(num,WEBSOCKET_TEXT,msg,header.length);
      break;
    case WEBSOCKET_OPCODE_PING:
      ws_send(&clients[num],WEBSOCKET_OPCODE_PONG,msg,header.length,0);
      clients[num].scallback(num,WEBSOCKET_PING,msg,header.length);
      break;
    case WEBSOCKET_OPCODE_PONG:
      if(clients[num].ping) {
        clients[num].scallback(num,WEBSOCKET_PONG,NULL,0);
        clients[num].ping = 0;
      }
      break;
    case WEBSOCKET_OPCODE_CLOSE:
      clients[num].scallback(num,WEBSOCKET_DISCONNECT_EXTERNAL,NULL,0);
      ws_disconnect_client(&clients[num], 0);
      break;
    default:
      break;
  }
  if(msg) free(msg);
}

static void ws_server_task(void* pvParameters) {
  struct netconn* conn;

  xwebsocket_mutex = xSemaphoreCreateMutex();
  xwebsocket_queue = xQueueCreate(WEBSOCKET_SERVER_QUEUE_SIZE, sizeof(struct netconn*));

  // initialize all clients
  for(int i=0;i<WEBSOCKET_SERVER_MAX_CLIENTS;i++) {
    clients[i].conn = NULL;
    clients[i].url  = NULL;
    clients[i].ping = 0;
    clients[i].last_opcode = 0;
    clients[i].contin = NULL;
    clients[i].len = 0;
    clients[i].ccallback = NULL;
    clients[i].scallback = NULL;
  }

  for(;;) {
    xQueueReceive(xwebsocket_queue,&conn,portMAX_DELAY);
    if(!conn) continue; // if the connection was NULL, ignore it

    xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY); // take access
    for(int i=0;i<WEBSOCKET_SERVER_MAX_CLIENTS;i++) {
      if(clients[i].conn == conn) {
        handle_read(i);
        break;
      }
    }
    xSemaphoreGive(xwebsocket_mutex); // return access
  }
  vTaskDelete(NULL);
}

int ws_server_start() {
  if(xtask) return 0;
  #if WEBSOCKET_SERVER_PINNED
  xTaskCreatePinnedToCore(&ws_server_task,
                          "ws_server_task",
                          WEBSOCKET_SERVER_TASK_STACK_DEPTH,
                          NULL,
                          WEBSOCKET_SERVER_TASK_PRIORITY,
                          &xtask,
                          WEBSOCKET_SERVER_PINNED_CORE);
  #else
  xTaskCreate(&ws_server_task,
              "ws_server_task",
              WEBSOCKET_SERVER_TASK_STACK_DEPTH,
              NULL,
              WEBSOCKET_SERVER_TASK_PRIORITY,
              &xtask);
  #endif
  return 1;
}

int ws_server_stop() {
  if(!xtask) return 0;
  vTaskDelete(xtask);
  return 1;
}

static bool prepare_response(char* buf,uint32_t buflen,char* handshake,char* protocol) {
  const char WS_HEADER[] = "Upgrade: websocket\r\n";
  const char WS_KEY[] = "Sec-WebSocket-Key: ";
  const char WS_RSP[] = "HTTP/1.1 101 Switching Protocols\r\n" \
                        "Upgrade: websocket\r\n" \
                        "Connection: Upgrade\r\n" \
                        "Sec-WebSocket-Accept: %s\r\n" \
                        "%s\r\n";

  char* key_start;
  char* key_end;
  char* hashed_key;

  if(!strstr(buf,WS_HEADER)) return 0;
  if(!buflen) return 0;
  key_start = strstr(buf,WS_KEY);
  if(!key_start) return 0;
  key_start += 19;
  key_end = strstr(key_start,"\r\n");
  if(!key_end) return 0;

  hashed_key = ws_hash_handshake(key_start,key_end-key_start);
  if(!hashed_key) return 0;
  if(protocol) {
    char tmp[256];
    sprintf(tmp,WS_RSP,hashed_key,"Sec-WebSocket-Protocol: %s\r\n");
    sprintf(handshake,tmp,protocol);
  }
  else {
    sprintf(handshake,WS_RSP,hashed_key,"");
  }
  free(hashed_key);
  return 1;
}

int ws_server_add_client_protocol(struct netconn* conn,
                         char* msg,
                         uint16_t len,
                         char* url,
                         char* protocol,
                         void (*callback)(uint8_t num,
                                          WEBSOCKET_TYPE_t type,
                                          char* msg,
                                          uint64_t len)) {
  int ret;
  char handshake[256];

  if(!prepare_response(msg,len,handshake,protocol)) {
    netconn_close(conn);
    netconn_delete(conn);
    return -2;
  }


  ret = -1;
  xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY);
  conn->callback = background_callback;
  netconn_write(conn,handshake,strlen(handshake),NETCONN_COPY);

  for(int i=0;i<WEBSOCKET_SERVER_MAX_CLIENTS;i++) {
    if(clients[i].conn) continue;
    clients[i] = ws_connect_client(conn,url,NULL,callback);
    callback(i,WEBSOCKET_CONNECT,NULL,0);
    if(!ws_is_connected(clients[i])) {
      callback(i,WEBSOCKET_DISCONNECT_ERROR,NULL,0);
      ws_disconnect_client(&clients[i], 0);
      break;
    }
    ret = i;
    break;
  }
  xSemaphoreGive(xwebsocket_mutex);
  return ret;
}

int ws_server_len_url(char* url) {
  int ret;
  ret = 0;
  xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY);
  for(int i=0;i<WEBSOCKET_SERVER_MAX_CLIENTS;i++) {
    if(clients[i].url && strcmp(url,clients[i].url)) ret++;
  }
  xSemaphoreGive(xwebsocket_mutex);
  return ret;
}

int ws_server_add_client(struct netconn* conn,
                         char* msg,
                         uint16_t len,
                         char* url,
                         void (*callback)(uint8_t num,
                                          WEBSOCKET_TYPE_t type,
                                          char* msg,
                                          uint64_t len)) {

  return ws_server_add_client_protocol(conn,msg,len,url,NULL,callback);

}

int ws_server_len_all() {
  int ret;
  ret = 0;
  xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY);
  for(int i=0;i<WEBSOCKET_SERVER_MAX_CLIENTS;i++) {
    if(clients[i].conn) ret++;
  }
  xSemaphoreGive(xwebsocket_mutex);
  return ret;
}

int ws_server_remove_client(int num) {
  int ret = 0;
  xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY);
  if(ws_is_connected(clients[num])) {
    clients[num].scallback(num,WEBSOCKET_DISCONNECT_INTERNAL,NULL,0);
    ws_disconnect_client(&clients[num], 0);
    ret = 1;
  }
  xSemaphoreGive(xwebsocket_mutex);
  return ret;
}

int ws_server_remove_clients(char* url) {
  int ret = 0;
  xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY);
  for(int i=0;i<WEBSOCKET_SERVER_MAX_CLIENTS;i++) {
    if(ws_is_connected(clients[i]) && strcmp(url,clients[i].url)) {
      clients[i].scallback(i,WEBSOCKET_DISCONNECT_INTERNAL,NULL,0);
      ws_disconnect_client(&clients[i], 0);
      ret += 1;
    }
  }
  xSemaphoreGive(xwebsocket_mutex);
  return ret;
}

int ws_server_remove_all() {
  int ret = 0;
  xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY);
  for(int i=0;i<WEBSOCKET_SERVER_MAX_CLIENTS;i++) {
    if(ws_is_connected(clients[i])) {
      clients[i].scallback(i,WEBSOCKET_DISCONNECT_INTERNAL,NULL,0);
      ws_disconnect_client(&clients[i], 0);
      ret += 1;
    }
  }
  xSemaphoreGive(xwebsocket_mutex);
  return ret;
}

// The following functions are already written below, but without the mutex.

int ws_server_send_text_client(int num,char* msg,uint64_t len) {
  xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY);
  int ret = ws_server_send_text_client_from_callback(num, msg, len);
  xSemaphoreGive(xwebsocket_mutex);
  return ret;
}

int ws_server_send_text_clients(char* url,char* msg,uint64_t len) {
  xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY);
  int ret = ws_server_send_text_clients_from_callback(url, msg, len);
  xSemaphoreGive(xwebsocket_mutex);
  return ret;
}

int ws_server_send_text_all(char* msg,uint64_t len) {
  xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY);
  int ret = ws_server_send_text_all_from_callback(msg, len);
  xSemaphoreGive(xwebsocket_mutex);
  return ret;
}

// the following functions should be used inside of the callback. The regular versions
// grab the mutex, but it is already grabbed from inside the callback so it will hang.

int ws_server_send_text_client_from_callback(int num,char* msg,uint64_t len) {
  int ret = 0;
  int err;
  if(ws_is_connected(clients[num])) {
    err = ws_send(&clients[num],WEBSOCKET_OPCODE_TEXT,msg,len,0);
    ret = 1;
    if(err) {
      clients[num].scallback(num,WEBSOCKET_DISCONNECT_ERROR,NULL,0);
      ws_disconnect_client(&clients[num], 0);
      ret = 0;
    }
  }
  return ret;
}

int ws_server_send_text_clients_from_callback(char* url,char* msg,uint64_t len) {
  int ret = 0;
  int err;

  if(url == NULL) {
    return ret;
  }

  for(int i=0;i<WEBSOCKET_SERVER_MAX_CLIENTS;i++) {
    if(clients[i].url != NULL && ws_is_connected(clients[i]) && !strcmp(clients[i].url,url)) {
      err = ws_send(&clients[i],WEBSOCKET_OPCODE_TEXT,msg,len,0);
      if(!err) ret += 1;
      else {
        clients[i].scallback(i,WEBSOCKET_DISCONNECT_ERROR,NULL,0);
        ws_disconnect_client(&clients[i], 0);
      }
    }
  }
  return ret;
}

int ws_server_send_text_all_from_callback(char* msg,uint64_t len) {
  int ret = 0;
  int err;
  for(int i=0;i<WEBSOCKET_SERVER_MAX_CLIENTS;i++) {
    if(ws_is_connected(clients[i])) {
      err = ws_send(&clients[i],WEBSOCKET_OPCODE_TEXT,msg,len,0);
      if(!err) ret += 1;
      else {
        clients[i].scallback(i,WEBSOCKET_DISCONNECT_ERROR,NULL,0);
        ws_disconnect_client(&clients[i], 0);
      }
    }
  }
  return ret;
}
// handles WiFi events
static esp_err_t event_handler(void* ctx, system_event_t* event) {
  const char* TAG = "event_handler";
  switch(event->event_id) {
    case SYSTEM_EVENT_AP_START:
      //ESP_ERROR_CHECK(tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, "esp32"));
      ESP_LOGI(TAG,"Access Point Started");
      break;
    case SYSTEM_EVENT_AP_STOP:
      ESP_LOGI(TAG,"Access Point Stopped");
      break;
    case SYSTEM_EVENT_AP_STACONNECTED:
      ESP_LOGI(TAG,"STA Connected, MAC=%02x:%02x:%02x:%02x:%02x:%02x AID=%i",
        event->event_info.sta_connected.mac[0],event->event_info.sta_connected.mac[1],
        event->event_info.sta_connected.mac[2],event->event_info.sta_connected.mac[3],
        event->event_info.sta_connected.mac[4],event->event_info.sta_connected.mac[5],
        event->event_info.sta_connected.aid);
      break;
    case SYSTEM_EVENT_AP_STADISCONNECTED:
      ESP_LOGI(TAG,"STA Disconnected, MAC=%02x:%02x:%02x:%02x:%02x:%02x AID=%i",
        event->event_info.sta_disconnected.mac[0],event->event_info.sta_disconnected.mac[1],
        event->event_info.sta_disconnected.mac[2],event->event_info.sta_disconnected.mac[3],
        event->event_info.sta_disconnected.mac[4],event->event_info.sta_disconnected.mac[5],
        event->event_info.sta_disconnected.aid);
      break;
    case SYSTEM_EVENT_AP_PROBEREQRECVED:
      ESP_LOGI(TAG,"AP Probe Received");
      break;
    case SYSTEM_EVENT_AP_STA_GOT_IP6:
      ESP_LOGI(TAG,"Got IP6=%01x:%01x:%01x:%01x",
        event->event_info.got_ip6.ip6_info.ip.addr[0],event->event_info.got_ip6.ip6_info.ip.addr[1],
        event->event_info.got_ip6.ip6_info.ip.addr[2],event->event_info.got_ip6.ip6_info.ip.addr[3]);
      break;
    default:
      ESP_LOGI(TAG,"Unregistered event=%i",event->event_id);
      break;
  }
  return ESP_OK;
}


// sets up WiFi
void wifi_setup() {
  const char* TAG = "wifi_setup";


  ESP_ERROR_CHECK(tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP));

  tcpip_adapter_ip_info_t info;
  memset(&info, 0, sizeof(info));
  IP4_ADDR(&info.ip, 192, 168, 4, 1);
  IP4_ADDR(&info.gw, 192, 168, 4, 1);
  IP4_ADDR(&info.netmask, 255, 255, 255, 0);
  ESP_LOGI(TAG,"setting gateway IP");
  ESP_ERROR_CHECK(tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &info));


  ESP_LOGI(TAG,"starting DHCPS adapter");
  ESP_ERROR_CHECK(tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP));

  ESP_LOGI(TAG,"starting event loop");
  ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));

  ESP_LOGI(TAG,"initializing WiFi");
  wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

  wifi_config_t wifi_config = {
    .ap = {
      .ssid = AP_SSID,
      .password= AP_PSSWD,
      .channel = 0,
      .authmode = WIFI_AUTH_WPA2_PSK,
      .ssid_hidden = 0,
      .max_connection = 4,
      .beacon_interval = 100
    }
  };

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_LOGI(TAG,"WiFi set up");
}
// receives clients from queue, handles them
void server_handle_task(void* pvParameters) {
  const static char* TAG = "server_handle_task";
  struct netconn* conn;
  ESP_LOGI(TAG,"task starting");
  for(;;) {
    xQueueReceive(client_queue,&conn,portMAX_DELAY);
    if(!conn) continue;
    http_serve(conn);
  }
  vTaskDelete(NULL);
}
// handles clients when they first connect. passes to a queue
void server_task(void* pvParameters) {
  const static char* TAG = "server_task";
  struct netconn *conn, *newconn;
  static err_t err;
  client_queue = xQueueCreate(client_queue_size,sizeof(struct netconn*));

  conn = netconn_new(NETCONN_TCP);
  netconn_bind(conn,NULL,80);
  netconn_listen(conn);
  ESP_LOGI(TAG,"server listening");
  do {
    err = netconn_accept(conn, &newconn);
    ESP_LOGI(TAG,"new client");
    if(err == ERR_OK) {
      xQueueSendToBack(client_queue,&newconn,portMAX_DELAY);
    }
  } while(err == ERR_OK);
  netconn_close(conn);
  netconn_delete(conn);
  ESP_LOGE(TAG,"task ending, rebooting board");
  esp_restart();
}
// serves any clients
void http_serve(struct netconn *conn) {
  const static char* TAG = "http_server";
  const static char HTML_HEADER[] = "HTTP/1.1 200 OK\nContent-type: text/html\n\n";

  const static char JS_HEADER[] = "HTTP/1.1 200 OK\nContent-type: text/javascript\n\n";
  const static char CSS_HEADER[] = "HTTP/1.1 200 OK\nContent-type: text/css\n\n";

  struct netbuf* inbuf;
  static char* buf;
  static uint16_t buflen;
  static err_t err;

  // default page
  extern const uint8_t root_html_start[] asm("_binary_root_html_start");
  extern const uint8_t root_html_end[] asm("_binary_root_html_end");
  const uint32_t root_html_len = root_html_end - root_html_start;

  // test.js
  extern const uint8_t test_js_start[] asm("_binary_test_js_start");
  extern const uint8_t test_js_end[] asm("_binary_test_js_end");
  const uint32_t test_js_len = test_js_end - test_js_start;

  // test.css
  extern const uint8_t test_css_start[] asm("_binary_test_css_start");
  extern const uint8_t test_css_end[] asm("_binary_test_css_end");
  const uint32_t test_css_len = test_css_end - test_css_start;



  netconn_set_recvtimeout(conn,1000); // allow a connection timeout of 1 second
  ESP_LOGI(TAG,"reading from client...");
  err = netconn_recv(conn, &inbuf);
  ESP_LOGI(TAG,"read from client");
  if(err==ERR_OK) {
    netbuf_data(inbuf, (void**)&buf, &buflen);
    if(buf) {

      // default page
      if     (strstr(buf,"GET / ")
          && !strstr(buf,"Upgrade: websocket")) {
        ESP_LOGI(TAG,"Sending /");
        netconn_write(conn, HTML_HEADER, sizeof(HTML_HEADER)-1,NETCONN_NOCOPY);
        netconn_write(conn, root_html_start,root_html_len,NETCONN_NOCOPY);
        netconn_close(conn);
        netconn_delete(conn);
        netbuf_delete(inbuf);
      }

      // default page websocket
      else if(strstr(buf,"GET / ")
           && strstr(buf,"Upgrade: websocket")) {
        ESP_LOGI(TAG,"Requesting websocket on /");

        ws_server_add_client(conn,buf,buflen,"/",websocket_callback);
        netbuf_delete(inbuf);
      }

      else if(strstr(buf,"GET /test.js ")) {
        ESP_LOGI(TAG,"Sending /test.js");
        netconn_write(conn, JS_HEADER, sizeof(JS_HEADER)-1,NETCONN_NOCOPY);
        netconn_write(conn, test_js_start, test_js_len,NETCONN_NOCOPY);
        netconn_close(conn);
        netconn_delete(conn);
        netbuf_delete(inbuf);
      }

      else if(strstr(buf,"GET /test.css ")) {
        ESP_LOGI(TAG,"Sending /test.css");
        netconn_write(conn, CSS_HEADER, sizeof(CSS_HEADER)-1,NETCONN_NOCOPY);
        netconn_write(conn, test_css_start, test_css_len,NETCONN_NOCOPY);
        netconn_close(conn);
        netconn_delete(conn);
        netbuf_delete(inbuf);
      }
      //USER CODES FOR NEW SPIFFS



      //USER CODES FOR NEW SPIFFS
      else {
        ESP_LOGI(TAG,"Unknown request");
        netconn_close(conn);
        netconn_delete(conn);
        netbuf_delete(inbuf);
      }
    }
    else {
      ESP_LOGI(TAG,"Unknown request (empty?...)");
      netconn_close(conn);
      netconn_delete(conn);
      netbuf_delete(inbuf);
    }
  }
  else { // if err==ERR_OK
    ESP_LOGI(TAG,"error on read, closing connection");
    netconn_close(conn);
    netconn_delete(conn);
    netbuf_delete(inbuf);
  }
}
// handles websocket events
void websocket_callback(uint8_t num,WEBSOCKET_TYPE_t type,char* msg,uint64_t len) {
  const static char* TAG = "websocket_callback";


  switch(type) {
    case WEBSOCKET_CONNECT:
      ESP_LOGI(TAG,"client %i connected!",num);
      break;
    case WEBSOCKET_DISCONNECT_EXTERNAL:
      ESP_LOGI(TAG,"client %i sent a disconnect message",num);

      break;
    case WEBSOCKET_DISCONNECT_INTERNAL:
      ESP_LOGI(TAG,"client %i was disconnected",num);
      break;
    case WEBSOCKET_DISCONNECT_ERROR:
      ESP_LOGI(TAG,"client %i was disconnected due to an error",num);

      break;
    case WEBSOCKET_TEXT:
      if(len) { // if the message length was greater than zero
        switch(msg[0]) {
          case 'L':
            if(sscanf(msg,"L")) {


              ws_server_send_text_all_from_callback(msg,len); // broadcast it!
            }
            break;
          case 'M':
            ESP_LOGI(TAG, "got message length %i: %s", (int)len-1, &(msg[1]));
            break;
          default:
	          ESP_LOGI(TAG, "got an unknown message with length %i", (int)len);
	          break;
        }
      }
      break;
    case WEBSOCKET_BIN:
      ESP_LOGI(TAG,"client %i sent binary message of size %i:\n%s",num,(uint32_t)len,msg);
      break;
    case WEBSOCKET_PING:
      ESP_LOGI(TAG,"client %i pinged us with message of size %i:\n%s",num,(uint32_t)len,msg);
      break;
    case WEBSOCKET_PONG:
      ESP_LOGI(TAG,"client %i responded to the ping",num);
      break;
  }
}


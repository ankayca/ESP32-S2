/*
 * server.h
 *
 *  Created on: Aug 18, 2020
 *      Author: ankayca
 */

#ifndef COMPONENTS_ESP32_WEBSOCKET_MASTER_INCLUDE_SERVER_H_
#define COMPONENTS_ESP32_WEBSOCKET_MASTER_INCLUDE_SERVER_H_
//QueueHandle_t client_queue;

esp_err_t event_handler(void* ctx, system_event_t* event);
 void wifi_setup();
 void led_setup();
 void led_duty(uint16_t duty);
 void websocket_callback(uint8_t num,WEBSOCKET_TYPE_t type,char* msg,uint64_t len);
 void http_serve(struct netconn *conn);
 void server_task(void* pvParameters);
 void server_handle_task(void* pvParameters);
#endif /* COMPONENTS_ESP32_WEBSOCKET_MASTER_INCLUDE_SERVER_H_ */

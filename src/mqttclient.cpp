#ifdef HAS_MQTT

#include "mqttclient.h"

static QueueHandle_t MQTTSendQueue;
TaskHandle_t mqttTask;

Ticker mqttTimer;
WiFiClient netClient;
MQTTClient mqttClient;

void mqtt_deinit(void) {
  mqttClient.unsubscribe(MQTT_INTOPIC);
  mqttClient.onMessageAdvanced(NULL);
  mqttClient.disconnect();
  vTaskDelete(mqttTask);
}

esp_err_t mqtt_init(void) {
  // setup network connection and MQTT client

  // Check if MQTT_ETHERNET is set to PHY or Wifi -- return true if set to wifi
  if (MQTT_ETHERNET == 1) {
    ETH.begin();
    ETH.setHostname(clientId);
  }
  mqttClient.begin(MQTT_SERVER, MQTT_PORT, netClient);
  mqttClient.setKeepAlive(MQTT_KEEPALIVE);
  mqttClient.onMessageAdvanced(mqtt_callback);

  _ASSERT(SEND_QUEUE_SIZE > 0);
  MQTTSendQueue = xQueueCreate(SEND_QUEUE_SIZE, sizeof(MessageBuffer_t));
  if (MQTTSendQueue == 0) {
    ESP_LOGE(TAG, "Could not create MQTT send queue. Aborting.");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "MQTT send queue created, size %d Bytes",
           SEND_QUEUE_SIZE * PAYLOAD_BUFFER_SIZE);

  ESP_LOGI(TAG, "Starting MQTTloop...");
  xTaskCreatePinnedToCore(mqtt_client_task, "mqttloop", 4096, (void *)NULL, 5,
                          &mqttTask, 0);
  ESP_LOGI(TAG, "MQTTloop started.");
  return ESP_OK;
}

int mqtt_connect(const char *my_host, const uint16_t my_port) {
  IPAddress mqtt_server_ip;

  ESP_LOGI(TAG, "MQTT name is %s", MQTT_CLIENTNAME);

  // resolve server host name
  if (WiFi.hostByName(my_host, mqtt_server_ip)) {
    ESP_LOGI(TAG, "Attempting to connect to %s [%s]", my_host,
             mqtt_server_ip.toString().c_str());
  } else {
    ESP_LOGI(TAG, "Could not resolve %s", my_host);
    return -1;
  }

  if (mqttClient.connect(MQTT_CLIENTNAME, MQTT_USER, MQTT_PASSWD)) {
    ESP_LOGI(TAG, "MQTT server connected, subscribing...");
    mqttClient.publish(MQTT_OUTTOPIC, MQTT_CLIENTNAME);
    // Clear retained messages that may have been published earlier on topic
    mqttClient.publish(MQTT_INTOPIC, "", true, 1);
    mqttClient.subscribe(MQTT_INTOPIC);
    ESP_LOGI(TAG, "MQTT topic subscribed");
  } else {
    ESP_LOGD(TAG, "MQTT last_error = %d / rc = %d", mqttClient.lastError(),
             mqttClient.returnCode());
    ESP_LOGW(TAG, "MQTT server not responding, retrying later");
    return -1;
  }

  return 0;
}

void mqtt_client_task(void *param) {

  MessageBuffer_t msg;

  while (1) {
    if (xQueuePeek(MQTTSendQueue, &msg, 1000 / portTICK_PERIOD_MS) == pdTRUE){//if there is a message to send
      if (mqttClient.connected()) {
        // check for incoming messages
        ESP_LOGD(TAG, "In loop");
        mqttClient.loop();

        // fetch next or wait for payload to send from queue
        // do not delete item from queue until it is transmitted
        // consider mqtt timeout while waiting

        // prepare mqtt topic
        char topic[16];
        snprintf(topic, 16, "%s/%u", MQTT_OUTTOPIC, msg.MessagePort);
        size_t out_len = 64;
        // base64 encode the message
        // unsigned char encoded[out_len];
        unsigned char encoded[64];//Set as static limit.
        switch (MQTT_ENCODER) {
          case 0://base64
          // get length of base64 encoded message
          // mbedtls_base64_encode(NULL, 0, &out_len, (unsigned char *)msg.Message,
          //                       msg.MessageSize);

          mbedtls_base64_encode(encoded, 64, &out_len,
                                (unsigned char *)msg.Message, msg.MessageSize);

          break;

          case 1://json
            int wifi= msg.Message[0] | msg.Message[1] << 8;
            int ble = msg.MessageSize>=4 ? msg.Message[2] | msg.Message[3] << 8 : 0;
            out_len=snprintf((char*) encoded, 64, "{'total':%d,'ble':%d,'wifi':%d}",wifi+ble,ble, wifi);
          break;
        }


        // send encoded message to mqtt server and delete it from queue
        if (mqttClient.publish(topic, (const char *)encoded, out_len)) {
          ESP_LOGD(TAG, "%u bytes sent to MQTT server: %s", out_len,msg.Message );
          xQueueReceive(MQTTSendQueue, &msg, (TickType_t)0);
          startWifiScan();

        } else
          ESP_LOGD(TAG, "Couldn't sent message to MQTT server");
      } else {
        // attempt to reconnect to MQTT server
        if (!connectWifi()) {
          ESP_LOGW(TAG, "Cannot connect to wifi for sending MQTT messages.");
          continue;
        }
        ESP_LOGD(TAG, "MQTT client reconnecting...");
        delay(MQTT_RETRYSEC * 1000);
        mqtt_connect(MQTT_SERVER, MQTT_PORT);
      }
    }
  } // while (1)
}

// process incoming MQTT messages
void mqtt_callback(MQTTClient *client, char *topic, char *payload, int length) {
  if (strcmp(topic, MQTT_INTOPIC) == 0) {
    // get length of base64 encoded message
    size_t out_len = 0;
    mbedtls_base64_decode(NULL, 0, &out_len, (unsigned char *)payload, length);

    // decode the base64 message
    unsigned char decoded[out_len];
    mbedtls_base64_decode(decoded, out_len, &out_len, (unsigned char *)payload,
                          length);

    rcommand(decoded, out_len);
  }
}

// enqueue outgoing messages in MQTT send queue
void mqtt_enqueuedata(MessageBuffer_t *message) {
  if (xQueueSendToBack(MQTTSendQueue, (void *)message, (TickType_t)0) != pdTRUE)
    ESP_LOGW(TAG, "MQTT sendqueue is full");
}

void mqtt_queuereset(void) { xQueueReset(MQTTSendQueue); }

uint32_t mqtt_queuewaiting(void) {
  return uxQueueMessagesWaiting(MQTTSendQueue);
}

#endif // HAS_MQTT
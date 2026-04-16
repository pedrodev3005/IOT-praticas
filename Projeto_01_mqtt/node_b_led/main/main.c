#include <stdio.h>
#include <string.h>

#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_err.h"

#include "driver/gpio.h"
#include "mqtt_client.h"
#include "protocol_examples_common.h"

static const char *TAG = "NODE_B_LED";

#define BROKER_URL "mqtt://192.168.12.1"
#define MQTT_TOPIC "ifpb/projeto/led"

#define LED_GPIO GPIO_NUM_8

static void led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_set_level(LED_GPIO, 0);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Conectado ao broker MQTT");
            esp_mqtt_client_subscribe(event->client, MQTT_TOPIC, 1);
            ESP_LOGI(TAG, "Inscrito no topico: %s", MQTT_TOPIC);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT desconectado");
            break;

        case MQTT_EVENT_DATA: {
            char topic[64];
            char msg[64];

            int topic_len = event->topic_len;
            int data_len = event->data_len;

            if (topic_len >= sizeof(topic)) {
                topic_len = sizeof(topic) - 1;
            }

            if (data_len >= sizeof(msg)) {
                data_len = sizeof(msg) - 1;
            }

            memcpy(topic, event->topic, topic_len);
            topic[topic_len] = '\0';

            memcpy(msg, event->data, data_len);
            msg[data_len] = '\0';

            ESP_LOGI(TAG, "Topico recebido: %s", topic);
            ESP_LOGI(TAG, "Mensagem recebida: %s", msg);

            if (strcmp(topic, MQTT_TOPIC) == 0) {
                if (strcmp(msg, "ON") == 0) {
                    gpio_set_level(LED_GPIO, 1);
                    ESP_LOGI(TAG, "LED ligado");
                } else if (strcmp(msg, "OFF") == 0) {
                    gpio_set_level(LED_GPIO, 0);
                    ESP_LOGI(TAG, "LED desligado");
                } else {
                    ESP_LOGW(TAG, "Comando desconhecido");
                }
            }
            break;
        }

        default:
            break;
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    led_init();

    ESP_LOGI(TAG, "Conectando ao Wi-Fi...");
    ESP_ERROR_CHECK(example_connect());
    ESP_LOGI(TAG, "Wi-Fi conectado");

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = BROKER_URL,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}
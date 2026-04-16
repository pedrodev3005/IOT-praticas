#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "NODE_A_BOTAO";

#define BROKER_URL "mqtt://192.168.12.1"
#define TOPIC_LED "ifpb/projeto/led"

#define BUTTON_GPIO GPIO_NUM_9
#define DEBOUNCE_MS 200

static esp_mqtt_client_handle_t client = NULL;
static bool estado_led = false;
static bool mqtt_conectado = false;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_BEFORE_CONNECT:
            ESP_LOGI(TAG, "Tentando conectar ao broker MQTT...");
            break;

        case MQTT_EVENT_CONNECTED:
            mqtt_conectado = true;
            ESP_LOGI(TAG, "Conectado ao broker MQTT");
            break;

        case MQTT_EVENT_DISCONNECTED:
            mqtt_conectado = false;
            ESP_LOGW(TAG, "MQTT desconectado");
            break;

        default:
            break;
    }
}

static void button_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));
}

static void button_task(void *pvParameters)
{
    int last_state = 1;

    while (1) {
        int current_state = gpio_get_level(BUTTON_GPIO);

        if (last_state == 1 && current_state == 0) {
            estado_led = !estado_led;

            const char *msg = estado_led ? "ON" : "OFF";

            ESP_LOGI(TAG, "Botao pressionado -> publicando: %s", msg);

            if (client != NULL && mqtt_conectado) {
                esp_mqtt_client_publish(client, TOPIC_LED, msg, 0, 1, 0);
            } else {
                ESP_LOGW(TAG, "MQTT ainda nao conectado");
            }

            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
        }

        last_state = current_state;
        vTaskDelay(pdMS_TO_TICKS(20));
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

    ESP_ERROR_CHECK(example_connect());

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = BROKER_URL,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    button_init();

    xTaskCreate(button_task, "button_task", 4096, NULL, 5, NULL);
}
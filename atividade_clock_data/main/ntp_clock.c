#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_sntp.h"
#include "protocol_examples_common.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

#define TAG "NTP_CLOCK"

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Inicializando SNTP...");

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.br");
    esp_sntp_init();
}

static void obtain_time(void)
{
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 15;

    initialize_sntp();

    while (timeinfo.tm_year < (2016 - 1900) && retry < retry_count) {
        ESP_LOGI(TAG, "Aguardando sincronizacao do horario... (%d/%d)", retry + 1, retry_count);
        vTaskDelay(pdMS_TO_TICKS(2000));
        time(&now);
        localtime_r(&now, &timeinfo);
        retry++;
    }

    if (timeinfo.tm_year < (2016 - 1900)) {
        ESP_LOGE(TAG, "Nao foi possivel sincronizar o horario via NTP.");
    } else {
        ESP_LOGI(TAG, "Horario sincronizado com sucesso.");
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(example_connect());

    setenv("TZ", "BRT3", 1);
    tzset();

    obtain_time();

    while (1) {
        time_t now;
        struct tm timeinfo;
        char data_hora[64];

        time(&now);
        localtime_r(&now, &timeinfo);

        strftime(data_hora, sizeof(data_hora), "%d/%m/%Y %H:%M:%S", &timeinfo);

        ESP_LOGI(TAG, "Data e hora atuais: %s", data_hora);

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
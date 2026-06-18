#include <stdio.h>
#include <string.h> 
#include <ctype.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "freertos/timers.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "driver/uart.h"

#include "esp_zigbee.h"
#include "ezbee/zha.h"

static const char *TAG = "NODE_B_ZIGBEE_LED";

#define LED_GPIO GPIO_NUM_10
#define CMD_BUFFER_SIZE 32

#define BBB_UART_NUM UART_NUM_1
#define BBB_UART_TX_GPIO GPIO_NUM_5
#define BBB_UART_RX_GPIO GPIO_NUM_4
#define BBB_UART_BAUD_RATE 115200
#define BBB_UART_BUFFER_SIZE 1024

#define NODE_B_ENDPOINT 10

#define ESP_ZIGBEE_PRIMARY_CHANNEL_MASK   ((1U << 13))
#define ESP_ZIGBEE_SECONDARY_CHANNEL_MASK (0x07FFF800U)

#define ESP_ZIGBEE_STORAGE_PARTITION_NAME "zb_storage"

#define ESP_MANUFACTURER_NAME "\x09""ESPRESSIF"
#define ESP_MODEL_IDENTIFIER "\x07"CONFIG_IDF_TARGET

#define ESP_ZIGBEE_ZC_CONFIG()                         \
    {                                                   \
        .device_type = EZB_NWK_DEVICE_TYPE_COORDINATOR, \
        .install_code_policy = false,                   \
        .zczr_config = {                                \
            .max_children = 10,                         \
        },                                              \
    }

#define ESP_ZIGBEE_PLATFORM_CONFIG()                    \
    {                                                   \
        .storage_partition_name = ESP_ZIGBEE_STORAGE_PARTITION_NAME, \
        .radio_config = {                               \
            .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE, \
        },                                              \
    }

#define ESP_ZIGBEE_DEFAULT_CONFIG()                     \
    {                                                   \
        .device_config = ESP_ZIGBEE_ZC_CONFIG(),        \
        .platform_config = ESP_ZIGBEE_PLATFORM_CONFIG(),\
    };

typedef enum {
    MODO_AUTO = 0,
    MODO_MANUAL = 1
} modo_controle_t;

#define TEMPO_DESLIGAMENTO_MS 5000

static TimerHandle_t timer_desligamento_node_a = NULL;
static bool desligamento_pendente_node_a = false;

static modo_controle_t modo_atual = MODO_AUTO;
static int estado_led = 0;

static void limpar_comando(char *cmd)
{
    int i = 0;
    int j = 0;
    char temp[CMD_BUFFER_SIZE];

    while (cmd[i] != '\0' && j < CMD_BUFFER_SIZE - 1) {
        if (cmd[i] != '\n' && cmd[i] != '\r') {
            temp[j] = toupper((unsigned char)cmd[i]);
            j++;
        }
        i++;
    }

    temp[j] = '\0';

    int inicio = 0;
    while (temp[inicio] == ' ') {
        inicio++;
    }

    int fim = strlen(temp) - 1;
    while (fim >= inicio && temp[fim] == ' ') {
        temp[fim] = '\0';
        fim--;
    }

    strcpy(cmd, &temp[inicio]);
}

static void bbb_uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = BBB_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(
        BBB_UART_NUM,
        BBB_UART_BUFFER_SIZE,
        BBB_UART_BUFFER_SIZE,
        0,
        NULL,
        0
    ));

    ESP_ERROR_CHECK(uart_param_config(BBB_UART_NUM, &uart_config));

    ESP_ERROR_CHECK(uart_set_pin(
        BBB_UART_NUM,
        BBB_UART_TX_GPIO,
        BBB_UART_RX_GPIO,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE
    ));

    ESP_LOGI(TAG, "UART BBB inicializada: TX=GPIO%d, RX=GPIO%d, baud=%d",
             BBB_UART_TX_GPIO, BBB_UART_RX_GPIO, BBB_UART_BAUD_RATE);
}

static void enviar_bbb(const char *mensagem)
{
    uart_write_bytes(BBB_UART_NUM, mensagem, strlen(mensagem));
}

static void enviar_status_bbb(void)
{
    if (estado_led) {
        enviar_bbb("STATUS:ON\n");
    } else {
        enviar_bbb("STATUS:OFF\n");
    }
}

static void enviar_modo_bbb(void)
{
    if (modo_atual == MODO_AUTO) {
        enviar_bbb("MODE:AUTO\n");
    } else {
        enviar_bbb("MODE:MANUAL\n");
    }
}

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

    estado_led = 0;

    ESP_LOGI(TAG, "LED inicializado no GPIO %d", LED_GPIO);
}

static void ligar_led(void)
{
    gpio_set_level(LED_GPIO, 1);
    estado_led = 1;

    ESP_LOGI(TAG, "LED ligado");
    enviar_status_bbb();
}

static void desligar_led(void)
{
    gpio_set_level(LED_GPIO, 0);
    estado_led = 0;

    ESP_LOGI(TAG, "LED desligado");
    enviar_status_bbb();
}

static void cancelar_desligamento_node_a(void)
{
    desligamento_pendente_node_a = false;

    if (timer_desligamento_node_a != NULL) {
        xTimerStop(timer_desligamento_node_a, 0);
    }

    ESP_LOGI(TAG, "Temporizador de desligamento cancelado");
}

static void timer_desligamento_node_a_callback(TimerHandle_t xTimer)
{
    desligamento_pendente_node_a = false;

    if (modo_atual == MODO_AUTO) {
        ESP_LOGI(TAG, "Tempo de 5 segundos finalizado. Desligando LED.");
        desligar_led();
    } else {
        ESP_LOGI(TAG, "Temporizador finalizado, mas modo manual esta ativo. LED nao foi alterado.");
    }
}

static void agendar_desligamento_node_a(void)
{
    desligamento_pendente_node_a = true;

    if (timer_desligamento_node_a != NULL) {
        xTimerStop(timer_desligamento_node_a, 0);
        xTimerChangePeriod(timer_desligamento_node_a, pdMS_TO_TICKS(TEMPO_DESLIGAMENTO_MS), 0);
        xTimerStart(timer_desligamento_node_a, 0);

        ESP_LOGI(TAG, "OFF recebido do Node A. LED sera desligado em 5 segundos.");
    }
}

static void processar_comando_node_a(const char *mensagem_recebida)
{
    char cmd[CMD_BUFFER_SIZE];

    strncpy(cmd, mensagem_recebida, CMD_BUFFER_SIZE - 1);
    cmd[CMD_BUFFER_SIZE - 1] = '\0';

    limpar_comando(cmd);

    ESP_LOGI(TAG, "Mensagem recebida do Node A: %s", cmd);

    if (modo_atual == MODO_MANUAL) {
        ESP_LOGW(TAG, "Modo manual ativo. Comando do Node A ignorado.");
        return;
    }

    if (strcmp(cmd, "ON") == 0) {
        cancelar_desligamento_node_a();
        ligar_led();
    }
    else if (strcmp(cmd, "OFF") == 0) {
        agendar_desligamento_node_a();
    }
    else {
        ESP_LOGW(TAG, "Comando desconhecido recebido do Node A");
    }
}

static void processar_comando_bbb(char *cmd)
{
    limpar_comando(cmd);

    ESP_LOGI(TAG, "Comando recebido da BBB/UART: %s", cmd);

    if (strcmp(cmd, "CMD:ON") == 0) {
        if (modo_atual == MODO_MANUAL) {
            ligar_led();
            enviar_status_bbb();
        } else {
            ESP_LOGW(TAG, "CMD:ON ignorado porque o modo atual eh automatico");
            enviar_status_bbb();
            enviar_modo_bbb();
        }
    }
    else if (strcmp(cmd, "CMD:OFF") == 0) {
        if (modo_atual == MODO_MANUAL) {
            desligar_led();
            enviar_status_bbb();
        } else {
            ESP_LOGW(TAG, "CMD:OFF ignorado porque o modo atual eh automatico");
            enviar_status_bbb();
            enviar_modo_bbb();
        }
    }
    else if (strcmp(cmd, "MODE:AUTO") == 0) {
        modo_atual = MODO_AUTO;
        ESP_LOGI(TAG, "Modo automatico ativado");
        enviar_modo_bbb();
        enviar_status_bbb();
    }
    else if (strcmp(cmd, "MODE:MANUAL") == 0) {
        modo_atual = MODO_MANUAL;
        cancelar_desligamento_node_a();

        ESP_LOGI(TAG, "Modo manual ativado");
        enviar_modo_bbb();
        enviar_status_bbb();
    }
    else {
        ESP_LOGW(TAG, "Comando desconhecido da BBB: %s", cmd);
        enviar_status_bbb();
        enviar_modo_bbb();
    }
}

static void tarefa_serial_bbb(void *pvParameters)
{
    uint8_t data[128];
    char cmd[CMD_BUFFER_SIZE];
    int cmd_pos = 0;

    while (1) {
        int len = uart_read_bytes(
            BBB_UART_NUM,
            data,
            sizeof(data) - 1,
            pdMS_TO_TICKS(100)
        );

        if (len > 0) {
            for (int i = 0; i < len; i++) {
                char c = (char)data[i];

                if (c == '\n' || c == '\r') {
                    if (cmd_pos > 0) {
                        cmd[cmd_pos] = '\0';
                        processar_comando_bbb(cmd);
                        cmd_pos = 0;
                    }
                } else {
                    if (cmd_pos < CMD_BUFFER_SIZE - 1) {
                        cmd[cmd_pos++] = c;
                    } else {
                        cmd_pos = 0;
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static bool node_b_zigbee_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(app_signal);

    switch (signal_type) {
        case EZB_ZDO_SIGNAL_SKIP_STARTUP:
            ESP_LOGI(TAG, "Inicializando pilha Zigbee");
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
            break;

        case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
        case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
            ezb_bdb_comm_status_t status =
                *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));

            if (status == EZB_BDB_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Dispositivo iniciado em modo %s",
                         ezb_bdb_is_factory_new() ? "factory-reset" : "normal");

                if (ezb_bdb_is_factory_new()) {
                    ESP_LOGI(TAG, "Formando rede Zigbee");
                    ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
                } else {
                    ESP_LOGI(TAG, "Dispositivo reiniciado. Abrindo rede por 180 segundos.");
                    ezb_bdb_open_network(180);
                }
            } else {
                ESP_LOGW(TAG, "Falha no startup Zigbee: 0x%02x", status);
            }

            break;
        }

        case EZB_BDB_SIGNAL_FORMATION: {
            ezb_bdb_comm_status_t status =
                *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));

            if (status == EZB_BDB_STATUS_SUCCESS) {
                ezb_extpanid_t extended_pan_id;
                ezb_nwk_get_extended_panid(&extended_pan_id);

                ESP_LOGI(TAG,
                         "Rede formada com sucesso: PAN ID(0x%04hx), EXT(0x%llx), Canal(%d), Short Address(0x%04hx)",
                         ezb_nwk_get_panid(),
                         extended_pan_id.u64,
                         ezb_nwk_get_current_channel(),
                         ezb_nwk_get_short_address());

                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGW(TAG, "Falha ao formar rede Zigbee: 0x%02x", status);
            }

            break;
        }

        case EZB_BDB_SIGNAL_STEERING: {
            ezb_bdb_comm_status_t status =
                *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));

            if (status == EZB_BDB_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Network steering concluido. Node A pode entrar na rede.");
            } else {
                ESP_LOGW(TAG, "Falha no network steering: 0x%02x", status);
            }

            break;
        }

        case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
            const ezb_zdo_signal_device_annce_params_t *dev_annce_params =
                ezb_app_signal_get_params(app_signal);

            ESP_LOGI(TAG,
                     "Novo dispositivo entrou/reentrou na rede. Short addr: 0x%04hx",
                     dev_annce_params->short_addr);

            break;
        }

        case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
            uint8_t duration = *(uint8_t *)ezb_app_signal_get_params(app_signal);

            if (duration) {
                ESP_LOGI(TAG,
                         "Rede Zigbee 0x%04hx aberta por %d segundos",
                         ezb_nwk_get_panid(),
                         duration);
            } else {
                ESP_LOGW(TAG, "Rede Zigbee fechada para novos dispositivos");
            }

            break;
        }

        default:
            ESP_LOGI(TAG,
                     "Sinal Zigbee: %s, tipo: 0x%02x",
                     ezb_app_signal_to_string(signal_type),
                     signal_type);
            break;
    }

    return true;
}

static void node_b_zcl_set_attr_value_handler(ezb_zcl_set_attr_value_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, , TAG, "Mensagem Zigbee vazia");

    ESP_LOGI(TAG,
             "ZCL SetAttributeValue: endpoint(%d), cluster(0x%04x), role(%s), status(0x%02x)",
             message->info.dst_ep,
             message->info.cluster_id,
             message->info.cluster_role == EZB_ZCL_CLUSTER_SERVER ? "server" : "client",
             message->info.status);

    if (message->info.cluster_id == EZB_ZCL_CLUSTER_ID_ON_OFF) {
        uint8_t valor = *(uint8_t *)message->in.attribute.data.value;

        ESP_LOGI(TAG, "Comando Zigbee On/Off recebido: %d", valor);

        if (valor) {
            processar_comando_node_a("ON");
        } else {
            processar_comando_node_a("OFF");
        }
    } else {
        ESP_LOGW(TAG, "Cluster Zigbee nao suportado: 0x%04x", message->info.cluster_id);
    }
}

static void node_b_zcl_core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
        case EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID:
            node_b_zcl_set_attr_value_handler((ezb_zcl_set_attr_value_message_t *)message);
            break;

        case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID: {
            ezb_zcl_cmd_default_rsp_message_t *default_rsp =
                (ezb_zcl_cmd_default_rsp_message_t *)message;

            ESP_LOGI(TAG,
                     "ZCL Default Response recebido com status(0x%02x)",
                     default_rsp->in.status_code);

            break;
        }

        default:
            ESP_LOGW(TAG, "Acao ZCL nao tratada: 0x%04lx", callback_id);
            break;
    }
}

static esp_err_t criar_dispositivo_zigbee_on_off_light(void)
{
    ezb_af_device_desc_t dev_desc = ezb_af_create_device_desc();

    ezb_zha_on_off_light_config_t light_cfg = EZB_ZHA_ON_OFF_LIGHT_CONFIG();

    ezb_af_ep_desc_t ep_desc =
        ezb_zha_create_on_off_light(NODE_B_ENDPOINT, &light_cfg);

    ezb_zcl_cluster_desc_t basic_desc = {0};

    basic_desc = ezb_af_endpoint_get_cluster_desc(
        ep_desc,
        EZB_ZCL_CLUSTER_ID_BASIC,
        EZB_ZCL_CLUSTER_SERVER
    );

    ezb_zcl_basic_cluster_desc_add_attr(
        basic_desc,
        EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
        (void *)ESP_MANUFACTURER_NAME
    );

    ezb_zcl_basic_cluster_desc_add_attr(
        basic_desc,
        EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
        (void *)ESP_MODEL_IDENTIFIER
    );

    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev_desc, ep_desc));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev_desc));

    ezb_zcl_core_action_handler_register(node_b_zcl_core_action_handler);

    return ESP_OK;
}

static esp_err_t configurar_comissionamento_zigbee(void)
{
    ezb_aps_secur_enable_distributed_security(false);

    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(ESP_ZIGBEE_PRIMARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(ESP_ZIGBEE_SECONDARY_CHANNEL_MASK));

    ESP_ERROR_CHECK(ezb_app_signal_add_handler(node_b_zigbee_signal_handler));

    return ESP_OK;
}

static void tarefa_zigbee(void *pvParameters)
{
    esp_zigbee_config_t config = ESP_ZIGBEE_DEFAULT_CONFIG();

    ESP_ERROR_CHECK(esp_zigbee_init(&config));

    ESP_ERROR_CHECK(configurar_comissionamento_zigbee());

    ESP_ERROR_CHECK(criar_dispositivo_zigbee_on_off_light());

    ESP_ERROR_CHECK(esp_zigbee_start(false));

    esp_zigbee_launch_mainloop();

    esp_zigbee_deinit();

    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t ret;

    ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    ret = nvs_flash_init_partition(ESP_ZIGBEE_STORAGE_PARTITION_NAME);

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition(ESP_ZIGBEE_STORAGE_PARTITION_NAME));
        ret = nvs_flash_init_partition(ESP_ZIGBEE_STORAGE_PARTITION_NAME);
    }

    ESP_ERROR_CHECK(ret);

    led_init();
    bbb_uart_init();

    timer_desligamento_node_a = xTimerCreate(
        "timer_off_node_a",
        pdMS_TO_TICKS(TEMPO_DESLIGAMENTO_MS),
        pdFALSE,
        NULL,
        timer_desligamento_node_a_callback
    );

    if (timer_desligamento_node_a == NULL) {
        ESP_LOGE(TAG, "Falha ao criar timer de desligamento do Node A");
    }

    ESP_LOGI(TAG, "Node B iniciado");
    ESP_LOGI(TAG, "Modo inicial: automatico");
    ESP_LOGI(TAG, "Iniciando Zigbee Coordinator + On/Off Light");

    enviar_status_bbb();
    enviar_modo_bbb();

    xTaskCreate(tarefa_serial_bbb, "serial_bbb", 4096, NULL, 4, NULL);
    xTaskCreate(tarefa_zigbee, "zigbee_main", 4096, NULL, 5, NULL);
}  
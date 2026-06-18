#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "driver/gpio.h"

#include "esp_zigbee.h"
#include "ezbee/zha.h"

static const char *TAG = "NODE_A_BOTAO";

#define BUTTON_GPIO GPIO_NUM_9
#define DEBOUNCE_MS 200

#define ESP_ZIGBEE_PRIMARY_CHANNEL_MASK   ((1U << 13))
#define ESP_ZIGBEE_SECONDARY_CHANNEL_MASK (0x07FFF800U)

#define NODE_A_ENDPOINT 1
#define ESP_ZIGBEE_STORAGE_PARTITION_NAME "zb_storage"

#define ESP_MANUFACTURER_NAME "\x09""ESPRESSIF"
#define ESP_MODEL_IDENTIFIER "\x07"CONFIG_IDF_TARGET

#define ESP_ZIGBEE_ZED_CONFIG()                         \
    {                                                    \
        .device_type = EZB_NWK_DEVICE_TYPE_END_DEVICE,   \
        .install_code_policy = false,                    \
        .zed_config = {                                  \
            .ed_timeout = EZB_NWK_ED_TIMEOUT_64MIN,      \
            .keep_alive = 4000,                          \
        },                                               \
    }

#define ESP_ZIGBEE_PLATFORM_CONFIG()                     \
    {                                                    \
        .storage_partition_name = ESP_ZIGBEE_STORAGE_PARTITION_NAME, \
        .radio_config = {                                \
            .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE,  \
        },                                               \
    }

#define ESP_ZIGBEE_DEFAULT_CONFIG()                      \
    {                                                    \
        .device_config = ESP_ZIGBEE_ZED_CONFIG(),        \
        .platform_config = ESP_ZIGBEE_PLATFORM_CONFIG(), \
    };

static int ultimo_estado_botao = 1;
static bool zigbee_bound = false;
static bool estado_logico_luz = false;

static void botao_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));

    ultimo_estado_botao = gpio_get_level(BUTTON_GPIO);

    ESP_LOGI(TAG, "Botao inicializado no GPIO %d", BUTTON_GPIO);
}

static void zigbee_send_toggle(void)
{
    if (!zigbee_bound) {
        ESP_LOGW(TAG, "Ainda nao fez bind com o Node B. Comando Zigbee ignorado.");
        return;
    }

    ezb_zcl_on_off_cmd_t cmd_req = {
        .cmd_ctrl = {
            .dst_addr.addr_mode = EZB_ADDR_MODE_NONE,
            .src_ep = NODE_A_ENDPOINT,
        },
    };

    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zcl_on_off_toggle_cmd_req(&cmd_req);
    esp_zigbee_lock_release();

    ESP_LOGI(TAG, "Comando Zigbee Toggle enviado");
}

static void enviar_estado_botao_para_node_b(int estado_botao)
{
    bool deseja_ligar = (estado_botao == 0);

    if (!zigbee_bound) { 
        ESP_LOGW(TAG, "Zigbee ainda nao conectado/bindado. Estado do botao nao enviado.");
        return;
    }

    if (deseja_ligar != estado_logico_luz) {
        zigbee_send_toggle();
        estado_logico_luz = deseja_ligar;

        if (deseja_ligar) {
            ESP_LOGI(TAG, "Botao pressionado -> presenca detectada -> enviado comando para LIGAR");
        } else {
            ESP_LOGI(TAG, "Botao solto -> sem presenca -> enviado comando para DESLIGAR");
        }
    } else {
        ESP_LOGI(TAG, "Estado do botao nao mudou em relacao ao estado logico da luz");
    }
}

static void botao_task(void *pvParameters)
{
    while (1) {
        int estado_atual = gpio_get_level(BUTTON_GPIO);

        if (estado_atual != ultimo_estado_botao) {
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

            estado_atual = gpio_get_level(BUTTON_GPIO);

            if (estado_atual != ultimo_estado_botao) {
                ultimo_estado_botao = estado_atual;

                if (estado_atual == 0) {
                    ESP_LOGI(TAG, "Botao pressionado: presenca detectada");
                } else {
                    ESP_LOGI(TAG, "Botao solto: sem presenca");
                }

                enviar_estado_botao_para_node_b(estado_atual);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void zdo_bind_ha_light_device_result(const ezb_zdp_bind_req_result_t *result, void *user_ctx)
{
    assert(result);

    if (result->error == EZB_ERR_NONE) {
        if (result->rsp && result->rsp->status == EZB_ZDP_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Bind com Node B/On-Off Light feito com sucesso");
            zigbee_bound = true;

            int estado_atual = gpio_get_level(BUTTON_GPIO);
            ultimo_estado_botao = estado_atual;
            enviar_estado_botao_para_node_b(estado_atual);

            ESP_LOGI(TAG, "Node A pronto. Botao pressionado = presenca, botao solto = sem presenca.");
        } else {
            ESP_LOGE(TAG, "Falha no bind com status 0x%02x", result->rsp ? result->rsp->status : 0xFF);
            zigbee_bound = false;
        }
    } else {
        ESP_LOGE(TAG, "Falha no bind com erro 0x%04x", result->error);
        zigbee_bound = false;
    }
}

static ezb_err_t zdo_bind_ha_light_device(uint16_t dst_short_addr, uint8_t dst_ep)
{
    ezb_zdo_bind_req_t bind_req = {
        .dst_nwk_addr = ezb_nwk_get_short_address(),
        .field = {
            .src_ep = NODE_A_ENDPOINT,
            .cluster_id = EZB_ZCL_CLUSTER_ID_ON_OFF,
            .dst_addr_mode = EZB_ADDR_MODE_EXT,
            .dst_ep = dst_ep,
        },
        .cb = zdo_bind_ha_light_device_result,
        .user_ctx = NULL,
    };

    ezb_nwk_get_extended_address(&bind_req.field.src_addr);

    ESP_RETURN_ON_ERROR(
        ezb_address_extended_by_short(dst_short_addr, &bind_req.field.dst_addr.extended_addr),
        TAG,
        "Falha ao obter endereco estendido do Node B"
    );

    ezb_err_t ret = ezb_zdo_bind_req(&bind_req);

    if (ret == EZB_ERR_NONE) {
        ESP_LOGI(TAG, "Tentando fazer bind com Node B. Short addr: 0x%04hx, endpoint: %d",
                 dst_short_addr, dst_ep);
    } else {
        ESP_LOGE(TAG, "Falha ao solicitar bind. Erro: 0x%04x", ret);
    }

    return ret;
}

static void zdo_find_ha_light_device_result(const ezb_zdo_match_desc_req_result_t *result, void *user_ctx)
{
    assert(result);

    if (result->error == EZB_ERR_NONE) {
        if (result->rsp &&
            result->rsp->status == EZB_ZDP_STATUS_SUCCESS &&
            result->rsp->match_length > 0 &&
            result->rsp->match_list) {

            for (size_t i = 0; i < result->rsp->match_length; i++) {
                ESP_LOGI(TAG, "Node B encontrado. Endpoint compativel: %d",
                         result->rsp->match_list[i]);

                zdo_bind_ha_light_device(
                    result->rsp->nwk_addr_of_interest,
                    result->rsp->match_list[i]
                );
            }
        } else {
            ESP_LOGW(TAG, "Nenhum On/Off Light encontrado na rede");
        }
    } else {
        ESP_LOGE(TAG, "Falha ao procurar On/Off Light. Erro: 0x%04x", result->error);
    }
}

static ezb_err_t zdo_find_ha_light_device(void)
{
    uint16_t cluster_list[1] = {EZB_ZCL_CLUSTER_ID_ON_OFF};

    ezb_zdo_match_desc_req_t req = {
        .dst_nwk_addr = 0xFFFD,
        .field = {
            .nwk_addr_of_interest = 0xFFFD,
            .profile_id = EZB_AF_HA_PROFILE_ID,
            .num_in_clusters = 1,
            .num_out_clusters = 0,
            .cluster_list = cluster_list,
        },
        .cb = zdo_find_ha_light_device_result,
        .user_ctx = NULL,
    };

    ezb_err_t ret = ezb_zdo_match_desc_req(&req);

    if (ret == EZB_ERR_NONE) {
        ESP_LOGI(TAG, "Procurando Node B/On-Off Light na rede");
    } else {
        ESP_LOGE(TAG, "Falha ao iniciar busca por On-Off Light. Erro: 0x%04x", ret);
    }

    return ret;
}

static bool node_a_zigbee_signal_handler(const ezb_app_signal_t *app_signal)
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
                    ESP_LOGI(TAG, "Procurando rede Zigbee do Node B");
                    ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
                } else {
                    ESP_LOGI(TAG, "Dispositivo reiniciado na rede");
                    zdo_find_ha_light_device();
                }
            } else {
                ESP_LOGW(TAG, "Falha no startup Zigbee: 0x%02x", status);
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
            }

            break;
        }

        case EZB_BDB_SIGNAL_STEERING: {
            ezb_bdb_comm_status_t status =
                *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));

            if (status == EZB_BDB_STATUS_SUCCESS) {
                ezb_extpanid_t extended_pan_id;
                ezb_nwk_get_extended_panid(&extended_pan_id);

                ESP_LOGI(TAG,
                         "Entrou na rede Zigbee. PAN ID: 0x%04hx, EXT: 0x%llx, Canal: %d, Short Address: 0x%04hx",
                         ezb_nwk_get_panid(),
                         extended_pan_id.u64,
                         ezb_nwk_get_current_channel(),
                         ezb_nwk_get_short_address());

                zdo_find_ha_light_device();
            } else {
                ESP_LOGW(TAG, "Falha ao entrar na rede Zigbee: 0x%02x", status);
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
            }

            break;
        }

        case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
            uint8_t duration = *(uint8_t *)ezb_app_signal_get_params(app_signal);

            if (duration) {
                ESP_LOGI(TAG, "Rede 0x%04hx aberta por %d segundos",
                         ezb_nwk_get_panid(), duration);
            } else {
                ESP_LOGW(TAG, "Rede fechada para novos dispositivos");
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

static void node_a_zcl_core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
        case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID: {
            ezb_zcl_cmd_default_rsp_message_t *default_rsp =
                (ezb_zcl_cmd_default_rsp_message_t *)message;

            ESP_LOGI(TAG,
                     "Resposta ZCL recebida com status 0x%02x",
                     default_rsp->in.status_code);
            break;
        }

        default:
            ESP_LOGW(TAG, "Callback ZCL nao tratado: 0x%04lx", callback_id);
            break;
    }
}

static esp_err_t criar_dispositivo_zigbee_on_off_switch(void)
{
    ezb_af_device_desc_t dev_desc = ezb_af_create_device_desc();

    ezb_zha_on_off_switch_config_t switch_cfg = EZB_ZHA_ON_OFF_SWITCH_CONFIG();

    ezb_af_ep_desc_t ep_desc =
        ezb_zha_create_on_off_switch(NODE_A_ENDPOINT, &switch_cfg);

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

    ezb_zcl_core_action_handler_register(node_a_zcl_core_action_handler);

    return ESP_OK;
}

static esp_err_t configurar_comissionamento_zigbee(void)
{
    ezb_aps_secur_enable_distributed_security(false);

    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(ESP_ZIGBEE_PRIMARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(ESP_ZIGBEE_SECONDARY_CHANNEL_MASK));

    ESP_ERROR_CHECK(ezb_app_signal_add_handler(node_a_zigbee_signal_handler));

    return ESP_OK;
}

static void tarefa_zigbee(void *pvParameters)
{
    esp_zigbee_config_t config = ESP_ZIGBEE_DEFAULT_CONFIG();

    ESP_ERROR_CHECK(esp_zigbee_init(&config));

    ESP_ERROR_CHECK(configurar_comissionamento_zigbee());

    ESP_ERROR_CHECK(criar_dispositivo_zigbee_on_off_switch());

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

    botao_init();

    ESP_LOGI(TAG, "Node A iniciado");
    ESP_LOGI(TAG, "Botao no GPIO %d", BUTTON_GPIO);
    ESP_LOGI(TAG, "Iniciando Zigbee End Device + On/Off Switch");

    xTaskCreate(botao_task, "botao_task", 4096, NULL, 4, NULL);
    xTaskCreate(tarefa_zigbee, "zigbee_main", 4096, NULL, 5, NULL);
}
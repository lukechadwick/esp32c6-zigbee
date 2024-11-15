
#ifndef ZIGBEE_MODE_ED
#error "Zigbee end device mode is not selected in Tools->Zigbee mode"
#endif

#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"

#define LED_PIN LED_BUILTIN

/* Default End Device config */
#define ESP_ZB_ZED_CONFIG()                             \
  {                                                     \
      .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,             \
      .install_code_policy = INSTALLCODE_POLICY_ENABLE, \
      .nwk_cfg = {                                      \
          .zed_cfg = {                                  \
              .ed_timeout = ED_AGING_TIMEOUT,           \
              .keep_alive = ED_KEEP_ALIVE,              \
          },                                            \
      },                                                \
  }

#define ESP_ZB_DEFAULT_RADIO_CONFIG()     \
  {                                       \
      .radio_mode = ZB_RADIO_MODE_NATIVE, \
  }

#define ESP_ZB_DEFAULT_HOST_CONFIG()                        \
  {                                                         \
      .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE, \
  }

/* Zigbee configuration */
#define INSTALLCODE_POLICY_ENABLE false
#define ED_AGING_TIMEOUT ESP_ZB_ED_AGING_TIMEOUT_64MIN
#define ED_KEEP_ALIVE 3000
#define HA_ESP_LIGHT_ENDPOINT 10
#define HA_ESP_SWITCH_ENDPOINT 11
#define ESP_ZB_PRIMARY_CHANNEL_MASK ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

/********************* Zigbee functions **************************/
static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
  ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(mode_mask));
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
  uint32_t *p_sg_p = signal_struct->p_app_signal;
  esp_err_t err_status = signal_struct->esp_err_status;
  esp_zb_app_signal_type_t sig_type = (esp_zb_app_signal_type_t)*p_sg_p;
  switch (sig_type)
  {
  case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
    log_i("Zigbee stack initialized");
    esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
    break;
  case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
  case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
    if (err_status == ESP_OK)
    {
      log_i("Start network steering");
      esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
    }
    else
    {
      /* commissioning failed */
      log_w("Failed to initialize Zigbee stack (status: %s)", esp_err_to_name(err_status));
    }
    break;
  case ESP_ZB_BDB_SIGNAL_STEERING:
    if (err_status == ESP_OK)
    {
      esp_zb_ieee_addr_t extended_pan_id;
      esp_zb_get_extended_pan_id(extended_pan_id);
      log_i("Joined network successfully (Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, PAN ID: 0x%04hx, Channel:%d, Short Address: 0x%04hx)",
            extended_pan_id[7], extended_pan_id[6], extended_pan_id[5], extended_pan_id[4],
            extended_pan_id[3], extended_pan_id[2], extended_pan_id[1], extended_pan_id[0],
            esp_zb_get_pan_id(), esp_zb_get_current_channel(), esp_zb_get_short_address());
    }
    else
    {
      log_i("Network steering was not successful (status: %s)", esp_err_to_name(err_status));
      esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
    }
    break;
  default:
    log_i("ZDO signal: %s (0x%x), status: %s", esp_zb_zdo_signal_to_string(sig_type), sig_type,
          esp_err_to_name(err_status));
    break;
  }
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
  esp_err_t ret = ESP_OK;
  switch (callback_id)
  {
  case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
    ret = zb_attribute_handler((esp_zb_zcl_set_attr_value_message_t *)message);
    break;
  default:
    log_w("Receive Zigbee action(0x%x) callback", callback_id);
    break;
  }
  return ret;
}

static void esp_zb_task(void *pvParameters)
{
  esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZED_CONFIG();
  esp_zb_init(&zb_nwk_cfg);

  // Create endpoint list
  esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();

  // Add light endpoint
  esp_zb_on_off_light_cfg_t light_cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
  esp_zb_endpoint_config_t endpoint_config_light = {
      .endpoint = HA_ESP_LIGHT_ENDPOINT,
      .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
      .app_device_id = ESP_ZB_HA_ON_OFF_LIGHT_DEVICE_ID,
      .app_device_version = 0,
  };
  esp_zb_ep_list_add_ep(ep_list, esp_zb_on_off_light_clusters_create(&light_cfg), endpoint_config_light);

  // Add switch endpoint
  esp_zb_on_off_switch_cfg_t switch_cfg = ESP_ZB_DEFAULT_ON_OFF_SWITCH_CONFIG();
  esp_zb_endpoint_config_t endpoint_config_switch = {
      .endpoint = HA_ESP_SWITCH_ENDPOINT,
      .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
      .app_device_id = ESP_ZB_HA_ON_OFF_SWITCH_DEVICE_ID,
      .app_device_version = 0,
  };
  esp_zb_ep_list_add_ep(ep_list, esp_zb_on_off_switch_clusters_create(&switch_cfg), endpoint_config_switch);

  esp_zb_device_register(ep_list);

  esp_zb_core_action_handler_register(zb_action_handler);
  esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);

  // Erase NVRAM before creating connection to new Coordinator
  // esp_zb_nvram_erase_at_start(true); //Comment out this line to erase NVRAM data if you are conneting to new Coordinator

  ESP_ERROR_CHECK(esp_zb_start(false));
  esp_zb_main_loop_iteration();
}

/* Handle the light attribute */

static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
  esp_err_t ret = ESP_OK;
  bool light_state = 0;
  bool switch_state = 0;

  if (!message)
  {
    log_e("Empty message");
  }
  if (message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS)
  {
    log_e("Received message: error status(%d)", message->info.status);
  }

  log_i("Received message: endpoint(%d), cluster(0x%x), attribute(0x%x), data size(%d)", message->info.dst_endpoint, message->info.cluster,
        message->attribute.id, message->attribute.data.size);

  // Handle Light Change
  if (message->info.dst_endpoint == HA_ESP_LIGHT_ENDPOINT)
  {
    if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF)
    {
      if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL)
      {
        light_state = message->attribute.data.value ? *(bool *)message->attribute.data.value : light_state;
        log_i("Light sets to %s", light_state ? "On" : "Off");
        // Here is the code to turn on/off your LED
        if (light_state == 1)
        {
          digitalWrite(LED_PIN, LOW);
        }
        else
        {
          digitalWrite(LED_PIN, HIGH);
        }
      }
    }
  }

  // Handle Switch Change
  if (message->info.dst_endpoint == HA_ESP_SWITCH_ENDPOINT)
  {
    if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF)
    {
      if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL)
      {
        switch_state = message->attribute.data.value ? *(bool *)message->attribute.data.value : switch_state;
        log_i("Switch sets to %s", switch_state ? "On" : "Off");
        // Handle the switch state here
      }
    }
  }

  return ret;
}

/********************* Arduino functions **************************/
void setup()
{
  // Init Zigbee
  esp_zb_platform_config_t config = {
      .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
      .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
  };
  ESP_ERROR_CHECK(esp_zb_platform_config(&config));

  // initialize LED pin
  pinMode(LED_PIN, OUTPUT);

  // turn off LED
  digitalWrite(LED_PIN, LOW);

  // Start Zigbee task
  xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, NULL);
}

void loop()
{
  // empty, zigbee running in task
}
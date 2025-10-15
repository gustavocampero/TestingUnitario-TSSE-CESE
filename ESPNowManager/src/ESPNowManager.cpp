#include "ESPNowManager.hpp"
#include "ESPNowScan.hpp"

static const char* TAG = "ESPNOW";

/**
 * @brief Estado global del envío por ESP-NOW.
 * @details Se actualiza desde el callback de envío para indicar SENDING/OK/FAIL.
 */
ESPNOW::sending_status_t sending_status = ESPNOW::sending_status_t::SEND_OK;

static uint8_t s_verb = 0; ///< Nivel de verbosidad de logs.

static QueueHandle_t espnow_queue; ///< Cola de eventos internos de ESP-NOW. Recibe eventos de envío/recepción y es procesada en loop().
static QueueHandle_t toSend_queue; ///< Cola de mensajes para enviar.
static SemaphoreHandle_t send_mutex; ///< Mutex para serializar operaciones de envío. Protege el acceso concurrente a sendPackets y recursos asociados.

/**
 * @brief Convierte una MAC a string.
 * @param arr Dirección MAC como array de 6 bytes.
 */
static std::string printTarget(const std::array<uint8_t,6>& arr);

esp_err_t ESPNOW::init(bool wifi_init, uint8_t ch, uint8_t verb)
{
    esp_err_t ret = ESP_OK;
    s_verb = verb;

    uint8_t _chipmacid[6];
    esp_efuse_mac_get_default(_chipmacid);
    ESP_LOGN(TAG,"Initiating Sensify ESP-NOW Manager... (" MACSTR ")", MAC2STR(_chipmacid));

    if (wifi_init) {
        espnow_wifi_init();
    }

    if(ch > 0)
    {
        esp_err_t ch_err = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        if (ch_err != ESP_OK) {
            ESP_LOG_E(TAG, "Error al setear canal: %s", esp_err_to_name(ch_err));
        }
    }

    ret = esp_now_init();
    if(ret != ESP_OK)
    {
        ESP_LOG_E(TAG, "esp_now_init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_now_register_send_cb(ESPNOW::espnow_send_cb));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_now_register_recv_cb(ESPNOW::espnow_recv_cb));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_now_set_pmk((uint8_t *)espnow_pmk));

    espnow_queue    = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(espnow_event_t));
    toSend_queue    = xQueueCreate(TOSEND_QUEUE_SIZE, sizeof(sensify_data_t));
    send_mutex      = xSemaphoreCreateMutex();
    return ret;
}

void ESPNOW::deinit(void)
{
    esp_now_unregister_send_cb();
    esp_now_unregister_recv_cb();
    
    if (toSend_queue) {
        sensify_data_t *item = nullptr;
        while (xQueueReceive(toSend_queue, &item, 0) == pdTRUE) {
            if (item) {
                if (item->value) { free(item->value); item->value = nullptr; }
                free(item);
            }
        }
        vQueueDelete(toSend_queue);
        toSend_queue = nullptr;
    }

    if (send_mutex) {
        if (xSemaphoreTake(send_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            xSemaphoreGive(send_mutex);
        }
        vSemaphoreDelete(send_mutex);
        send_mutex = nullptr;
    }

    if (espnow_queue) {
        vQueueDelete(espnow_queue);
        espnow_queue = nullptr;
    }

    esp_now_deinit();
}

void ESPNOW::espnow_wifi_init(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_init());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(ESPNOW_WIFI_MODE));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());
}

static inline bool is_all_zero_mac(const uint8_t m[6]) {
    return (m[0]|m[1]|m[2]|m[3]|m[4]|m[5]) == 0;
}

esp_err_t ESPNOW::add_peer(const uint8_t* address, bool encrypt)
{
    if (!address) return ESP_ERR_INVALID_ARG;

    if(is_all_zero_mac(address)) {
        ESP_LOGE(TAG, "Error: Dirección MAC no válida (all zeros)");
        return ESP_FAIL;
    }

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, address, ESP_NOW_ETH_ALEN);
    peer.channel = 0; // 0 = canal actual del Wi-Fi
    peer.ifidx = (wifi_interface_t)ESPNOW_WIFI_IF;
    peer.encrypt = encrypt;
    if(encrypt) {
        memcpy(peer.lmk, espnow_lmk, ESP_NOW_KEY_LEN);
    }

    esp_err_t ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK) {
        ESP_LOG_E(TAG, "Error: esp_now_add_peer falló (err = %s)", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

uint8_t ESPNOW::get_channel()
{
    wifi_second_chan_t orig_sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&orig_channel, &orig_sec);
    return orig_channel;
}

void ESPNOW::set_channel(uint8_t ch)
{
    if(ch >= SCAN_CH_INIT && ch <= SCAN_CH_FINISH) {
        wifi_second_chan_t orig_sec = WIFI_SECOND_CHAN_NONE;
        esp_wifi_get_channel(&orig_channel, &orig_sec);

        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    }
}

esp_err_t ESPNOW::start(void)
{
    esp_err_t ret = ESP_OK;
    
    const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    ret = add_peer(bcast, false);

    BaseType_t xReturned=xTaskCreatePinnedToCore (
        &(ESPNOW::generic_thread_entry),
        "ESPNOW-loop",
        4096,
        this,
        4,
        NULL,
        1
    );

    if((int)xReturned>=0) {
        if(s_verb>2)ESP_LOGP(TAG, " xTaskCreatePinnedToCore(Cameraloop_espnow)=%d",(int)xReturned);
    }
    else {
        ret = ESP_FAIL;
        ESP_LOG_E(TAG, "ERROR  xTaskCreatePinnedToCore(Cameraloop_espnow)=%d",(int)xReturned);
    }

    vTaskDelay(10/portTICK_PERIOD_MS);
    return ret;
}

void ESPNOW::generic_thread_entry(void * pvParameter)
{
    if(s_verb>2)ESP_LOG_I("STACK","SIZE %d",uxTaskGetStackHighWaterMark(NULL));
    ESPNOW *myself = reinterpret_cast<ESPNOW*>(pvParameter);
    if (myself)
    {
        myself->loop();
    }

    ESP_LOG_W(TAG,"WARNING!! error start ESPCAM thread.");
}

void ESPNOW::loop(void)
{
    espnow_event_t evt;

    while (xQueueReceive(espnow_queue, &evt, portMAX_DELAY) == pdTRUE)
    {
        switch(evt.id)
        {
            case ESPNOW_SEND_EVT:
            {
                send();
                break;
            }

            case ESPNOW_RECV_EVT:
            {
                espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;
                receive(recv_cb);
                break;
            }

            default:
                ESP_LOG_E(TAG, "Callback type error: %d", evt.id);
                break;
        }
        vTaskDelay(10/portTICK_PERIOD_MS);
    }
}

void ESPNOW::receive(espnow_event_recv_cb_t* recv)
{
    espnow_data_t *packet = reinterpret_cast<espnow_data_t *>(recv->data);
    if(recv->data_len >= sizeof(espnow_data_t)-sizeof(uint8_t*))
    {
        assembleState state;
        std::array<uint8_t,6> mac;
        memcpy(mac.data(), recv->mac_addr, 6);

        if(scan.isActive() && packet->sensify_type == S_TYPE_SCAN)
        {
            if (packet->seq_num == 0) {
                scan.add_scanned_device(mac, recv->rssi);
            }
            return;
        }

        if(packet->sensify_type == large_msg_type)
        {
            static size_t msg_size = 0;

            state = assembleLargeMsg(recv->data, recv->data_len, &msg_size);
            if(state == READY) {
                sensify_data_t data = {
                    .mac_addr = {0},
                    .type = large_msg_type,
                    .value_size = sizeof(uint32_t),
                    .value = 0,
                };
                data.value = (uint8_t*)malloc(sizeof(uint32_t));
                memcpy(data.value, &msg_size, sizeof(uint32_t));
                memcpy(data.mac_addr, mac.data(), mac.size());

                finish_data_t finish = {
                    .mac_addr = {0},
                    .type = large_msg_type,
                    .result = RECV_FINISH_OK,
                };
                memcpy(finish.mac_addr, mac.data(), mac.size());
                if(_finish_recv_cb) { _finish_recv_cb(&finish); }

                if(_data_recv_cb) { _data_recv_cb(&data); }
                msg_size = 0;
            }
            else if(state == ASSEMBLE_ERROR) {
                ESP_LOG_E(TAG, "ASSEMBLE ERROR");

                finish_data_t finish = {
                    .mac_addr = {0},
                    .type = large_msg_type,
                    .result = RECV_ASSEMBLE_ERROR,
                };
                memcpy(finish.mac_addr, mac.data(), mac.size());
                if(_finish_recv_cb) { _finish_recv_cb(&finish); }
            }
        }
        else
        {
            state = assembleData(recv->data, recv->data_len, mac);
            if(state == READY) {
                Session session = sessionMap[mac];
                ESP_LOGN(TAG, "Session %s%s%s data received! Type: %d (%d bytes)",LOG_COLOR_P,printTarget(mac).c_str(),LOG_RESET_COLOR, session.sensify_type, session.payload.value_size);
                sensify_data_t data = {
                    .mac_addr = {0},
                    .type = session.sensify_type,
                    .value_size = session.payload.value_size,
                    .value = session.payload.value.data(),
                };
                memcpy(data.mac_addr, mac.data(), mac.size());

                finish_data_t finish = {
                    .mac_addr = {0},
                    .type = session.sensify_type,
                    .result = RECV_FINISH_OK,
                };
                memcpy(finish.mac_addr, mac.data(), mac.size());
                if(_finish_recv_cb) { _finish_recv_cb(&finish); }

                if(_data_recv_cb) { _data_recv_cb(&data); }
                
                sessionMap.erase(mac);
            }
            else if(state == ASSEMBLE_ERROR) {
                ESP_LOG_E(TAG, "Session %s assemble ERROR", printTarget(mac).c_str());

                finish_data_t finish = {
                    .mac_addr = {0},
                    .type = packet->sensify_type,
                    .result = RECV_FINISH_FAIL,
                };
                memcpy(finish.mac_addr, mac.data(), mac.size());
                if(_finish_recv_cb) { _finish_recv_cb(&finish); }
                
                sessionMap.erase(mac);
            }
        }
    }
    else {
        ESP_LOG_E(TAG, "Receive error data from: " MACSTR "", MAC2STR(recv->mac_addr));
    }
    free(recv->data);
}

ESPNOW::assembleState ESPNOW::assembleLargeMsg(uint8_t *data, size_t data_len, size_t *msg_size)
{
    static uint16_t last_sequence = 0;
    assembleState state = ESPNOW::ASSEMBLING;
    espnow_data_t *packet = reinterpret_cast<espnow_data_t *>(data);
    uint16_t seq_num = packet->seq_num;

    if (seq_num == last_sequence) {
        return ESPNOW::ASSEMBLING;
    }
    last_sequence = seq_num;

    size_t size = data_len - sizeof(espnow_data_t) + sizeof(uint8_t *);
    uint8_t *p = (uint8_t *)(packet) + sizeof(espnow_data_t) - sizeof(uint8_t *);

    switch(seq_num)
    {
        case 1:
        {
            if(_start_recv_cb) {
                _start_recv_cb();
            }

            // start
            if(_large_msg.start) {
                if(!_large_msg.start(_large_msg.ctx)) {
                    ESP_LOG_E(TAG, "LargeMsgIO.start failed");
                    return ESPNOW::ASSEMBLE_ERROR;
                }
            }

            if(_large_msg.write) {
                if(!_large_msg.write(_large_msg.ctx, p, size)) {
                    ESP_LOGE(TAG, "Failed to upload initial data to _large_msg.write");
                    return state;
                }

                *msg_size = size;
            }

            break;
        }

        case 0:
        {
            printf("\n");
            ESP_LOGN(TAG,"recv_seq = 0. %sTransmission complete!%s",LOG_COLOR_G, LOG_RESET_COLOR);
            if(*msg_size > 0) {
                if(_large_msg.finish) {
                    if (!_large_msg.finish(_large_msg.ctx, false)) {
                        state = ASSEMBLE_ERROR;
                        ESP_LOG_E(TAG, "Failed to finish upload");
                        return state;
                    }
                }
                if(_large_msg.get_size) {
                    int sz = 0;
                    if (_large_msg.get_size(_large_msg.ctx, &sz)) {
                        ESP_LOGI(TAG, "Large message COMPLETE [length = %d bytes]", sz);
                    }
                }
                
                state = ESPNOW::READY;
            }
    
            break;
        }

        default:
        {
            if(_large_msg.write) {
                size_t new_size = *msg_size + size;
                if(!_large_msg.write(_large_msg.ctx, p, size)) {
                    ESP_LOGE(TAG, "Failed to upload initial data to _large_msg.write");
                }

                *msg_size = new_size;
            }

            break;
        }
    }
    printf("Total data received: %d/%d bytes\n\r",*msg_size,_large_msg.next_total);
    return state;
}

ESPNOW::assembleState ESPNOW::assembleData(uint8_t *data, size_t data_len, std::array<uint8_t,6> target)
{
    assembleState state = ESPNOW::ASSEMBLING;
    espnow_data_t *packet = reinterpret_cast<espnow_data_t *>(data);

    uint16_t seq_num = packet->seq_num;
    sensify_data_type_t sensify_type = packet->sensify_type;
    size_t size = data_len - sizeof(espnow_data_t) + sizeof(uint8_t *);
    uint8_t *p = (uint8_t *)(packet) + sizeof(espnow_data_t) - sizeof(uint8_t *);

    Session &session = sessionMap[target];

    switch(seq_num)
    {
        case 1:
        {
            session.payload = payload_data();
            session.sensify_type = S_TYPE_EMPTY;
            if(_start_recv_cb)
            {
                _start_recv_cb();
            }

            session.payload.value.resize(size);
            memcpy(session.payload.value.data(), p, size);
            session.payload.value_size = size;
            session.sensify_type = sensify_type;
            session.next_seq = 2;

            break;
        }

        case 0:
        {
            ESP_LOGN(TAG,"(%s%s%s) recv_seq = 0. %sTransmission complete!%s",LOG_COLOR_P
                                                                            ,printTarget(target).c_str()
                                                                            ,LOG_RESET_COLOR,LOG_COLOR_G, LOG_RESET_COLOR);
            if(session.payload.value.size()>0) {
                state = ESPNOW::READY;
                return state;
            }
            else {
                ESP_LOG_W(TAG, "payload = null - Packet discarded");
            }

            break;
        }

        default:
        {
            // Verificar la secuencia esperada
            if(seq_num != session.next_seq)
            {
                ESP_LOGW(TAG, "Session %s sequence error: expected %d, got %d", 
                        printTarget(target).c_str(), session.next_seq, seq_num);
                return ASSEMBLE_ERROR;
            }

            if (session.payload.value.size() > 0)
            {
                size_t new_size = session.payload.value_size + size;
                size_t free_heap = xPortGetFreeHeapSize();
                size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
                if (new_size <= largest_block) {
                    session.payload.value.reserve(new_size);
                    session.payload.value.insert(session.payload.value.end(), p, p + size);
                    session.payload.value_size = new_size;
                    session.next_seq++;
                }
                else
                {
                    ESP_LOGW(TAG, "No hay suficiente memoria contigua para reservar %d bytes (heap libre: %d, bloque más grande: %d)",
                              new_size, free_heap, largest_block);
                    state = ESPNOW::ASSEMBLE_ERROR;
                    
                }
            }
            else
            {
                ESP_LOG_W(TAG, "payload=null - packet discarded");
            }
            break;
        }
    }
    ESP_LOGN(TAG,"Receiving from %s%s%s: %d bytes (fh: %db)",LOG_COLOR_P,printTarget(target).c_str(),LOG_RESET_COLOR,session.payload.value_size,xPortGetFreeHeapSize());
    return state;
}

void ESPNOW::send(void)
{
    dequeue_dataToSend();
}

esp_err_t ESPNOW::sendMsg_async(sensify_data_type_t type, uint8_t *data, size_t len, const uint8_t mac[ESP_NOW_ETH_ALEN])
{
    if (!data || !mac || len == 0) return ESP_ERR_INVALID_ARG;

    sensify_data_t *msg = (sensify_data_t *)malloc(sizeof(sensify_data_t));
    if (!msg) { free(data); return ESP_ERR_NO_MEM; }

    msg->type = type;
    msg->value_size = len;
    msg->value = data;
    memcpy(msg->mac_addr, mac, ESP_NOW_ETH_ALEN);

    esp_err_t r = enqueue_dataToSend(msg);
    if(r == ESP_OK)
    {
        enqueue_sendRequest();
    }
    else {
        free(msg->value);
        free(msg);
    }

    return r;
}

esp_err_t ESPNOW::enqueue_dataToSend(sensify_data_t *data)
{
    if (!toSend_queue || !data) {
        ESP_LOG_E(TAG, "toSend_queue not initialized or data null");
        return ESP_ERR_INVALID_ARG;
    }

    if (xQueueSend(toSend_queue, &data, 0) != pdTRUE) {
        sensify_data_t *dropped = nullptr;
        if (xQueueReceive(toSend_queue, &dropped, 0) == pdTRUE) {
            if (s_verb > 0) ESP_LOG_W(TAG, "toSend_queue full -> dropped oldest");
            if (dropped) {
                if (dropped->value) free(dropped->value);
                free(dropped);
            }

            if (xQueueSend(toSend_queue, &data, 0) == pdTRUE) {
                if (s_verb > 2) ESP_LOGD(TAG, "Enqueued after drop");
                return ESP_OK;
            }
        }

        ESP_LOG_E(TAG, "Failed to enqueue even after drop");
        return ESP_ERR_NO_MEM;
    }

    if (s_verb > 2) ESP_LOGD(TAG, "Enqueued");
    return ESP_OK;
}

esp_err_t ESPNOW::dequeue_dataToSend(void)
{
    esp_err_t err;
    if (!toSend_queue) {
        ESP_LOG_E(TAG, "toSend_queue not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!send_mutex) send_mutex = xSemaphoreCreateMutex();

    sensify_data_t *item;
    if (xQueueReceive(toSend_queue, &item, 0) != pdTRUE) {
        if (s_verb > 0) ESP_LOG_W(TAG, "toSend_queue empty");
        return ESP_ERR_NOT_FOUND;
    }

    if (!item) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(send_mutex, 10000 / portTICK_PERIOD_MS) == pdTRUE) {
        err = sendPackets(item->type,
                            item->value,
                            item->value_size,
                            item->mac_addr);

        xSemaphoreGive(send_mutex);

        if (err != ESP_OK) {
            ESP_LOGB(TAG,"Send data to " MACSTR " %sFAILED", MAC2STR(item->mac_addr),LOG_COLOR_R);
            if(_log_cb) {
                char text[100] = {};
                sprintf(text,"Send data to " MACSTR " FAILED",MAC2STR(item->mac_addr));
                _log_cb(log_verb::LOG_LEVEL_DEBUG,log_verb::LOG_LEVEL_ERROR,200, text);
            }

            if(_err_cb) { 
                send_result_data_t err = {
                    .mac_addr = {0},
                    .result = SEND_FINISH_FAIL,
                };
                memcpy(err.mac_addr, item->mac_addr, 6);

                _err_cb(&err);
            }
        }
        else {
            ESP_LOGB(TAG, "Send data to " MACSTR " %sCOMPLETE", MAC2STR(item->mac_addr),LOG_COLOR_G);
            if(_err_cb) { 
                send_result_data_t err = {
                    .mac_addr = {0},
                    .result = SEND_FINISH_OK,
                };
                memcpy(err.mac_addr, item->mac_addr, 6);

                _err_cb(&err);
            }
        }
    }
    else {
        ESP_LOG_E(TAG, "ESPNOW busy: timeout al tomar send mutex");
        err = ESP_ERR_TIMEOUT;
    }

    if (item->value) {
        free(item->value);
        item->value = NULL;
    }
    free(item);

    return err;
}

esp_err_t ESPNOW::clean_dataToSend(void)
{
    if (!toSend_queue) return ESP_ERR_INVALID_STATE;

    sensify_data_t *item = NULL;
    while (xQueueReceive(toSend_queue, &item, 0) == pdTRUE) {
        if (item) {
            if (item->value) {
                free(item->value);
                item->value = NULL;
            }
            free(item);
        }
    }

    return ESP_OK;
}

esp_err_t ESPNOW::enqueue_sendRequest(void)
{
    if (!espnow_queue) {
        ESP_LOG_E(TAG, "espnow_queue not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    espnow_event_t evt = {};
    evt.id = ESPNOW_SEND_EVT;

    if(xQueueSend(espnow_queue, &evt, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOG_W(TAG, "espnow_queue full");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t ESPNOW::sendMsg_sync(sensify_data_type_t type, uint8_t *data, size_t len, const uint8_t mac[ESP_NOW_ETH_ALEN])
{
    esp_err_t err;
    if (!data || !mac || len == 0) return ESP_ERR_INVALID_ARG;
    if (!send_mutex) send_mutex = xSemaphoreCreateMutex();

    if (xSemaphoreTake(send_mutex, 10000 / portTICK_PERIOD_MS) == pdTRUE) {
        err = sendPackets(type,
                            data,
                            len,
                            (uint8_t *)mac);

        xSemaphoreGive(send_mutex);
        
        if (err != ESP_OK) {
            ESP_LOGB(TAG,"Send data to " MACSTR " %sFAILED", MAC2STR(mac),LOG_COLOR_R);
            if(_log_cb) {
                char text[100] = {};
                sprintf(text,"Send data to " MACSTR " FAILED",MAC2STR(mac));
                _log_cb(log_verb::LOG_LEVEL_DEBUG,log_verb::LOG_LEVEL_ERROR,200, text);
            }

            if(_err_cb) { 
                send_result_data_t err = {
                    .mac_addr = {0},
                    .result = SEND_FINISH_FAIL,
                };
                memcpy(err.mac_addr, mac, 6);

                _err_cb(&err);
            }
        }
        else {
            ESP_LOGB(TAG, "Send data to " MACSTR " %sCOMPLETE", MAC2STR(mac),LOG_COLOR_G);
            if(_err_cb) { 
                send_result_data_t err = {
                    .mac_addr = {0},
                    .result = SEND_FINISH_OK,
                };
                memcpy(err.mac_addr, mac, 6);

                _err_cb(&err);
            }
        }
    }
    else {
        ESP_LOG_E(TAG, "ESPNOW busy: timeout al tomar send mutex");
        err = ESP_ERR_TIMEOUT;
    }

    return err;
}

esp_err_t ESPNOW::sendPackets(sensify_data_type_t data_type, uint8_t *data, size_t data_len, uint8_t *mac)
{
    const size_t MAX_PACKET_SIZE = ESP_NOW_MAX_DATA_LEN - sizeof(espnow_data_t) - 1;
    
    uint16_t num_packets = (data_len / MAX_PACKET_SIZE) + ( data_len % MAX_PACKET_SIZE == 0 ? 0 : 1); /**< max 2^16 (65536 packets) */
    int magic = rand();
    uint8_t mac_destiny[6];

    // if mac=NULL send to the first mac saved
    if (mac) {
        memcpy(mac_destiny, mac, 6);
    }
    else {
        ESP_LOG_E(TAG,"mac = NULL");
        return ESP_FAIL;
    }

    ESP_LOGN(TAG, "Preparing to send %d packets to " MACSTR, num_packets,MAC2STR(mac_destiny));

    int i = 1;
    int fail_tries = 0;
    while (i <= num_packets + 1)
    {
        if(sending_status == sending_status_t::SEND_OK || sending_status == sending_status_t::SEND_FAIL)
        {
            size_t packet_size = (i == num_packets) ? (data_len % MAX_PACKET_SIZE) : MAX_PACKET_SIZE;
            
            // Prepare packet to send
            espnow_data_t packet;
            packet.sensify_type = data_type;
            packet.type = ESPNOW_DATA_UNICAST;
            if (i == num_packets + 1)
            {
                packet.seq_num = 0;
                packet_size = 0;
            }
            else
            {
                    packet.seq_num = i;
            }
            packet.magic = magic;

            if(s_verb>1) ESP_LOGN(TAG,"Sending packet %d... [fh: %d b]",i,xPortGetFreeHeapSize());
            sending_status = sending_status_t::SENDING;

            if (packet.seq_num != 0)
            {
                // Charge payload in packet
                packet.payload = (uint8_t *)malloc(packet_size);
                memcpy(packet.payload, data + (i - 1) * MAX_PACKET_SIZE, packet_size);

                // packet_buffer = &packet + packet.payload
                uint8_t packet_buffer[sizeof(espnow_data_t) - sizeof(uint8_t *) + packet_size];
                memcpy(packet_buffer, &packet, sizeof(espnow_data_t) - sizeof(uint8_t *));
                memcpy(packet_buffer + sizeof(espnow_data_t) - sizeof(uint8_t *), packet.payload, packet_size);
            
                // send
                esp_err_t err = esp_now_send(mac_destiny, packet_buffer, sizeof(espnow_data_t) - sizeof(uint8_t *) + packet_size);

                free(packet.payload);
                packet.payload = nullptr;
                packet = espnow_data_t();
                if (err == ESP_OK)
                {
                    // sending_status = sending_status_t::SENDING;
                }
                else
                {
                    sending_status = sending_status_t::SEND_FAIL;
                    ESP_LOG_E(TAG,"Send FAILED %d [packet number: %d]",err,i);
                    return ESP_FAIL;
                }
            }
            else
            {
                // send only &packet
                if (ESP_ERROR_CHECK_WITHOUT_ABORT(esp_now_send(mac_destiny, (uint8_t *)&packet, sizeof(espnow_data_t)- sizeof(uint8_t *))) == ESP_OK)
                {
                    // sending_status = sending_status_t::SENDING;
                }
                else
                {
                    sending_status = sending_status_t::SEND_FAIL;
                    ESP_LOG_E(TAG,"Send FAILED [packet number: %d]",i);
                    return ESP_FAIL;
                }
            }

            uint32_t timeout = esp_log_timestamp() + 10000;
            while (sending_status == sending_status_t::SENDING)
            {
                if(esp_log_timestamp() > timeout)
                {
                    sending_status = sending_status_t::SEND_FAIL;
                    ESP_LOGN(TAG,"Sending timeout!");
                    return ESP_FAIL;
                }
                vTaskDelay(10 / portTICK_PERIOD_MS);
            }

            if(sending_status == sending_status_t::SEND_OK)
            {
                i++;
                fail_tries = 0;
            }

            if(sending_status == sending_status_t::SEND_FAIL)
            {
                if(fail_tries > 150)  // timeout ~10 seg
                {
                    // ESP_LOGN(ESPNOW_TAG,"Sending fail!");
                    return ESP_FAIL;
                }
                fail_tries++;
            }

            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
    return ESP_OK;
}

// -------------------------------------------------------------------------------

void ESPNOW::espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (mac_addr == NULL) {
        ESP_LOG_E(TAG, "Send cb arg error");
        return;
    }

    if (status == ESP_NOW_SEND_SUCCESS)
        sending_status = sending_status_t::SEND_OK;
    else
        sending_status = sending_status_t::SEND_FAIL;

    if (s_verb > 1) {
        ESP_LOGB(TAG, "Send data to " MACSTR ", status: %s", MAC2STR(mac_addr),
                 (status == ESP_NOW_SEND_SUCCESS ? LOG_COLOR_G "OK!!" : LOG_COLOR_R "FAILED"));
    }
}

void ESPNOW::espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    espnow_event_t evt;
    espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;
    const uint8_t* mac_addr = info->src_addr;

    if (mac_addr == NULL || data == NULL || len <= 0)
    {
        ESP_LOG_E(TAG, "Receive cb arg error");
        return;
    }

    evt.id = ESPNOW_RECV_EVT;
    memcpy(recv_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    recv_cb->data = static_cast<uint8_t*>(malloc(len)); 
    if (recv_cb->data == NULL)
    {
        ESP_LOG_E(TAG, "Malloc data fail");
        return;
    }

    memcpy(recv_cb->data, data, len);
    recv_cb->data_len = len;
    recv_cb->rssi = info->rx_ctrl->rssi;

    if (xQueueSend(espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE)
    {
        ESP_LOG_E(TAG, "Send queue fail");
        free(recv_cb->data);
    }
}

// -----------------------------------------------------------------

esp_err_t ESPNOW::send_broadcast_msg(uint8_t ch)
{
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);

    uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t data = 0x01;
    auto *buf = (uint8_t*) malloc(sizeof(data));
    memcpy(buf, &data, sizeof(data));

    sendMsg_sync(S_TYPE_SCAN
                    ,buf
                    ,sizeof(data)
                    ,broadcast_mac);

    return ESP_OK;
}

esp_err_t ESPNOW::Scan()
{   
    wifi_second_chan_t orig_sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&orig_channel, &orig_sec);
    setScanProcess(5000);
    send_broadcast_msg(scan.channel());
    return ESP_OK;
}

bool ESPNOW::scanRead()
{
    bool ret = false;
    
    switch (scan.read()) {
        case ScanSession::OFF:
        case ScanSession::SCANNING:
        { 
            break;
        }
        case ScanSession::SWITCH:
        {
            send_broadcast_msg(scan.channel());
            break;
        }
        case ScanSession::READY:
        {
            ret = true;
            esp_wifi_set_channel(orig_channel, WIFI_SECOND_CHAN_NONE);
            break;
        }
        default: break;
    }

    return ret;
}

void ESPNOW::printScanResult()
{
    ESP_LOGP(TAG, "ESP-NOW scan finished:");
    char text[32];
    for (const auto& device : scan.results()) {
        snprintf(text, sizeof(text), "%02d > " MACSTR " (%d)", device.ch, MAC2STR(device.mac), device.rssi);
        ESP_LOGP(TAG, "%s", text);
        if(_log_cb){
            _log_cb(log_verb::LOG_LEVEL_DEBUG,log_verb::LOG_LEVEL_INFO,200, text);
        }
        text[0] = 0;
    }

    scan.clear();
}

// ------------------------------------------------------------------

static std::string printTarget(const std::array<uint8_t,6>& arr) {
    char buf[3*6 + 1];
    char *p = buf;
    for (size_t i = 0; i < 6; i++) {
        sprintf(p, "%02X", arr[i]);
        p += 2;
        if (i < 6-1) *p++ = ':';  // opcional separador
    }
    *p = '\0';
    return std::string(buf);
}
#ifndef ESPNOW_MGR_H
#define ESPNOW_MGR_H
extern "C" {
#include "esp_now.h"
#include "logger.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "esp_netif.h"
#include "esp_wifi.h"
}
#include <string>
#include <vector>
#include <map>
#include <array>
#include "ESPNowScan.hpp"

/**
 * @brief Configuración del modo WiFi para ESP-NOW
 * @details Configura el dispositivo para funcionar en modo AP+STA por defecto
 */
#if 1//CONFIG_ESPNOW_WIFI_MODE_STATION
#define ESPNOW_WIFI_MODE WIFI_MODE_APSTA  ///< Modo WiFi: Access Point + Station
#define ESPNOW_WIFI_IF   ESP_IF_WIFI_STA  ///< Interfaz WiFi: Station
#else
#define ESPNOW_WIFI_MODE WIFI_MODE_AP     ///< Modo WiFi: Solo Access Point
#define ESPNOW_WIFI_IF   ESP_IF_WIFI_AP   ///< Interfaz WiFi: Access Point
#endif

#define TOSEND_QUEUE_SIZE       10   ///< Tamaño de la cola de mensajes para enviar
#define ESPNOW_QUEUE_SIZE       200  ///< Tamaño de la cola de eventos ESP-NOW
#define ESPNOW_MAXDELAY         1000 ///< Timeout máximo para colas (en milisegundos) 


constexpr char espnow_pmk[] = {"pmk1234567890123"};  ///< Primary Master Key (16 bytes)
constexpr char espnow_lmk[] = {"lmk1234567890123"};  ///< Local Master Key (16 bytes)

class ESPNOW {

public:
    // ---------------------------------------------------------- Eventos ESP-NOW
    /**
     * @enum espnow_event_id_t
     * @brief Identificadores de eventos ESP-NOW para paquetes
     */
    typedef enum {
        ESPNOW_SEND_EVT,  ///< Evento de envío
        ESPNOW_RECV_EVT,  ///< Evento de recepción
        ESPNOW_STOP_EVT,  ///< Evento de stop
    } espnow_event_id_t;

    /**
     * @enum espnow_event_send_cb_t
     * @brief Estructura para evento de envío de paquete
     */
    typedef struct {
        uint8_t mac_addr[ESP_NOW_ETH_ALEN]; ///< Dirección MAC del dispositivo
        esp_now_send_status_t status;       ///< Estado del envío del paquete
    } espnow_event_send_cb_t;

    /**
     * @enum espnow_event_recv_cb_t
     * @brief Estructura para evento de recepción de paquete
     */
    typedef struct {
        uint8_t mac_addr[ESP_NOW_ETH_ALEN]; ///< Dirección MAC del dispositivo
        uint8_t *data;                      ///< Datos recibidos
        size_t data_len;                    ///< Longitud de los datos recibidos
        int rssi;                           ///< Señal
    } espnow_event_recv_cb_t;

    /**
     * @brief Unión. Evento de envío o recepción
     */
    typedef union {
        espnow_event_send_cb_t send_cb; ///< Evento de envío
        espnow_event_recv_cb_t recv_cb; ///< Evento de recepción
    } espnow_event_info_t;

    /**
     * @brief Evento ESP-NOW
     */
    typedef struct {
        espnow_event_id_t id; ///< Identificador del evento
        espnow_event_info_t info; ///< Información del evento
    } espnow_event_t;

    /**
     * @enum espnow_data_mode_t
     * @brief Modos de transmisión de datos ESP-NOW
     */
     enum {
        ESPNOW_DATA_BROADCAST,  ///< Transmisión broadcast (todos los dispositivos)
        ESPNOW_DATA_UNICAST,    ///< Transmisión unicast (dispositivo específico)
        ESPNOW_DATA_MAX,        ///< Número máximo de modos
    };

    // ---------------------------------------------------------- Tipos de datos

    /**
     * @brief Tipos de mensajes Sensify ESP-NOW
     */
    typedef enum {
        S_TYPE_EMPTY = -1,                      ///< Mensaje vacío
        S_TYPE_PHOTO = 0x00,                    ///< Photo process: Photo request
        S_TYPE_PHOTOSIZE = 0x01,                ///< Photoprocess: Recibir tamaño de foto
        S_TYPE_PHOTO_WIFI = 0x02,               ///< Photo process: Photo request w/WIFI
        S_TYPE_CONFIG_DETECTMODE = 0x03,        ///< Configurar Detect mode
        S_TYPE_CONFIG_EXTRAAP = 0x04,           ///< Deshabilitado
        // S_TYPE_CONFIG_FACE_TIMEOUT,          ///< Deshabilitado
        S_TYPE_COMMAND = 0x06,                  ///< Comando
        S_TYPE_ERROR = 0x07,                    ///< Error
        S_TYPE_LOG = 0x08,                      ///< Log
        // S_TYPE_INTEREST,                     ///< Deshabilitado
        // S_TYPE_MODE,                         ///< Deshabilitado
        S_TYPE_UPDATE = 0x0B,                   ///< Update request (WIFI)
        S_TYPE_CAM_CONFIG = 0x0C,               ///< Configuracion de camera sensor
        // S_TYPE_DEFOG_CONFIG,                 ///< Deshabilitado
        S_TYPE_VERSION = 0x0E,                  ///< Update process: Request version 
        S_TYPE_FW = 0x0F,                       ///< Update process: Enviar paquete de firmware
        S_TYPE_INSTALL = 0x10,                  ///< Update process: Comando para instalar
        S_TYPE_GET = 0x11,                      ///< Photo process: Pedir buffer
        // S_TYPE_PARAMS,                       ///< Deshabilitado
        S_TYPE_SCAN = 0x013,                    ///< Mensaje de Scan
        S_TYPE_CONFIG_STANDBY = 0x14,           ///< Configurar standby mode
        S_TYPE_SYNC = 0x15,                     ///< Mensaje de sincronizacion
        S_TYPE_CREDENTIALS = 0x16               ///< Enviar credenciales
    } sensify_data_type_t;

    // ---------------------------------------------------------- Estructuras de mensajes

    /**
     * @struct sensify_data_t
     * @brief Estructura para mensajes Sensify
     */
    typedef struct {
        uint8_t mac_addr[ESP_NOW_ETH_ALEN]; ///< Dirección MAC del dispositivo destino
        sensify_data_type_t type;           ///< Tipo de mensaje Sensify
        size_t value_size;                  ///< Tamaño de los datos
        uint8_t *value;                     ///< Puntero a los datos
    } __attribute__((packed)) sensify_data_t;

    /**
     * @struct espnow_data_t
     * @brief Estructura de datos para paquetes ESP-NOW
     */
    typedef struct {
        sensify_data_type_t sensify_type; ///< Tipo de mensaje Sensify
        uint8_t type;                     ///< Modo de transmisión (broadcast/unicast)
        uint16_t seq_num;                 ///< Número de secuencia del paquete
        // uint16_t crc;                  ///< (deshabilitado)
        uint32_t magic;                   ///< Número mágico para identificación de dispositivo
        uint8_t *payload;                 ///< Payload real de los datos ESP-NOW
    } __attribute__((packed)) espnow_data_t;

    // ---------------------------------------------------------- Envío

    /**
     * @enum sending_status_t
     * @brief Estado del proceso de envío de mensajes
     */
     typedef enum {
        SENDING,    ///< Enviando
        SEND_OK,    ///< Enviado correctamente
        SEND_FAIL,  ///< Error en el envío
    } sending_status_t;

    /**
     * @enum send_result_t
     * @brief Resultado del send process
     */
    typedef enum {
        SEND_FINISH_OK = 0,    ///< Envío completado exitosamente
        SEND_FINISH_FAIL = 1,  ///< Error en el envío
    } send_result_t;

    /**
     * @struct send_result_data_t
     * @brief Estructura para callback de send result
     */
    typedef struct {
        uint8_t mac_addr[ESP_NOW_ETH_ALEN]; ///< Dirección MAC del dispositivo target
        send_result_t result;               ///< Resultado del send process
    } send_result_data_t;

    // ---------------------------------------------------------- Recepción

    /**
     * @enum recv_result_t
     * @brief Resultado del receive process
     */
    typedef enum {
        RECV_FINISH_OK = 0,        ///< Recepción completada exitosamente
        RECV_FINISH_FAIL = 1,      ///< Error en la recepción
        RECV_ASSEMBLE_ERROR = 2,   ///< Error en el ensamblado de paquetes
    } recv_result_t;

    /**
     * @struct finish_data_t
     * @brief Estructura para callback de finish receive
     */
    typedef struct {
        uint8_t mac_addr[ESP_NOW_ETH_ALEN]; ///< Dirección MAC del dispositivo destino
        sensify_data_type_t type;           ///< Tipo de mensaje Sensify
        recv_result_t result;               ///< Resultado del receive process
    } __attribute__((packed)) finish_data_t;

    /**
     * @enum assembleState
     * @brief Estado del ensamblado de paquetes
     */
    typedef enum {
        ASSEMBLE_ERROR = -1,  ///< Error
        ASSEMBLING,           ///< Ensamblado
        READY,                ///< Mensaje listo para procesar
    } assembleState;

    /**
     * @struct payload_data
     * @brief Estructura para ensamblar data
     */
    struct payload_data {
        size_t value_size;              ///< Tamaño de los datos
        std::vector<uint8_t> value;     ///< Payload
    };

    /**
     * @struct Session
     * @brief Sesion de recepcion de data
     */
    struct Session {
        payload_data payload;               ///< Data
        sensify_data_type_t sensify_type;   ///< Tipo de mensaje Sensify
        uint16_t next_seq;                  ///< Siguiente número de secuencia esperado
    };

    /**
     * @struct LargeMsgIO_t
     * @brief Manejador de mensajes demasiado grandes para payload_data
     */
    typedef struct LargeMsgIO_t {
        bool (*start)(void* ctx) = nullptr;                                     ///< Callback para iniciar save process
        bool (*write)(void* ctx, const uint8_t* data, size_t len) = nullptr;    ///< Callback para escribir paquete
        bool (*finish)(void* ctx, bool cancel) = nullptr;                       ///< Callback para finalizar save process
        bool (*get_size)(void* ctx, int* out_size) = nullptr;                   ///< Callback para obtener size
        void* ctx = nullptr;                                                    ///< Contexto
        
        size_t next_total = 0;                                                  ///< Total de bytes esperados para el siguiente mensaje
    } LargeMsgIO_t;

    // ---------------------------------------------------------- Tipos para callbacks

    typedef void (*send_log_cb)(log_verb, const log_verb, uint16_t, const char*);   ///< Callback para log
    typedef void (*err_cb)(const send_result_data_t*);                              ///< Callback para error
    typedef void (*start_recv_cb)(void);                                            ///< Callback para iniciar assemble data
    typedef void (*data_recv_cb)(sensify_data_t*);                                  ///< Callback para procesar data
    typedef void (*finish_recv_cb)(finish_data_t*);                                 ///< Callback para finalizar assemble data
    
    // ---------------------------------------------------------- Funciones principales

    /**
     * @brief Constructor
     */
    ESPNOW() {}

    /**
     * @brief Inicializa el módulo ESP-NOW. Si el wifi no esta iniciado, lo inicia
     * @param wifi_init Si es true, inicializa WiFi.
     * @param ch Canal WiFi a usar (default: 0)
     * @param verb Nivel de verbosidad de terminal logs
     * @return esp_err_t ESP_OK en éxito o código de error
     */
    esp_err_t init(bool wifi_init, uint8_t ch = 0, uint8_t verb = 0);
    
    /**
     * @brief Desinicializa ESP-NOW y libera recursos
     */
    void deinit(void);
    
    /**
     * @brief Detiene el loop interno de ESP-NOW
     * @return esp_err_t ESP_OK en éxito o código de error
     */
    esp_err_t stop(void);
    
    /**
     * @brief Inicia el loop interno de ESP-NOW
     * @return esp_err_t ESP_OK en éxito o código de error
     */
    esp_err_t start(void);
    
    /**
     * @brief Añade/actualiza un peer en la tabla de ESP-NOW
     * @param address Dirección MAC del peer
     * @param encrypt Si true, habilita cifrado LMK con ese peer
     * @return esp_err_t ESP_OK en éxito o código de error
     */
    esp_err_t add_peer(const uint8_t* address, bool encrypt);

    /**
     * @brief Setea nuevo canal ESPNOW. Guarda el anterior en orig_channel
     * @param ch Nuevo canal
     */
    void set_channel(uint8_t ch);

    /**
     * @brief Obtiene el canal ESPNOW
     * @return uint8_t canal
     */
    uint8_t get_channel();
    
    // ---------------------------------------------------------- Funciones Send
    
    /**
     * @brief Encola un mensaje para envío asíncrono por ESP-NOW.
     * @note La función crea un `sensify_data_t`, apunta al payload dado por `data`
     * y lo encola para ser enviado por una tarea interna. Si tiene exito,
     * dispara la solicitud de envío (_enqueue_sendRequest_).
     * @param type Tipo Sensify del mensaje
     * @param data Puntero al payload. **Debe apuntar a memoria de heap**. La función **no copia** el contenido, solo guarda el puntero.
     * @param len Tamaño del payload en bytes ( > 0 ).
     * @param mac Dirección MAC destino
     * @return esp_err_t:   ESP_OK si se encola correctamente
                            ESP_ERR_INVALID_ARG si algún argumento es inválido
                            ESP_ERR_NO_MEM si no hay memoria para el contenedor o no se pudo encolar
                            Otros si error al encolar
     */
    esp_err_t sendMsg_async(sensify_data_type_t type, uint8_t *data, size_t len, const uint8_t mac[ESP_NOW_ETH_ALEN]);
    
    /**
     * @brief Envía un mensaje con `sendPackets` y espera el resultado
     * @param type Tipo Sensify del mensaje
     * @param data Puntero al payload. **Debe apuntar a memoria de heap**. La función **no copia** el contenido ni transfiere su propiedad.
     * @param len Tamaño del payload en bytes ( > 0 ).
     * @param mac Dirección MAC destino
     * @return esp_err_t:   ESP_OK si envío completado
                            ESP_ERR_INVALID_ARG si algún argumento es inválido
                            ESP_ERR_TIMEOUT si no se pudo tomar el mutex de envio dentro de 10 s
                            Otros si error de `sendPackets(...)`
     */
    esp_err_t sendMsg_sync(sensify_data_type_t type, uint8_t *data, size_t len, const uint8_t mac[ESP_NOW_ETH_ALEN]);
    
    // ---------------------------------------------------------- Funciones de set callback
    
    /**
     * @brief Establece callback para _log_cb
     * @param cb Función callback
     */
    void setLogCallback(send_log_cb cb) { _log_cb = cb; }
    
    /**
     * @brief Establece callback para _err_cb
     * @param cb Función callback
     */
    void setErrCallback(err_cb cb) { _err_cb = cb; }
    
    /**
     * @brief Establece callback para _start_recv_cb
     * @param cb Función callback
     */
    void setStartReceiveCallback(start_recv_cb cb) { _start_recv_cb = cb; }
    
    /**
     * @brief Establece callback para _data_recv_cb
     * @param cb Función callback
     */
    void setDataReceivedCallback(data_recv_cb cb) { _data_recv_cb = cb; }
    
    /**
     * @brief Establece callback para _finish_recv_cb
     * @param cb Función callback
     */
    void setFinishReceiveCallback(finish_recv_cb cb) { _finish_recv_cb = cb; }
    
    // ---------------------------------------------------------- Funciones de large msg

    /**
     * @brief Configura el IO para large msg
     * @param io Estructura con callbacks y contexto
     */
    void setLargeMsgIO(const LargeMsgIO_t& io) { _large_msg = io; }
    
    /**
     * @brief Setea el tamaño total esperado del siguiente large msg
     * @param total_bytes Tamaño; si es 0 se ignora
     */
    void setLargeMsgIONextTotal(size_t total_bytes) { if(total_bytes > 0) _large_msg.next_total = total_bytes; }

    /**
     * @brief Configura el tipo Sensify asociado al large msg
     * @param type Tipo Sensify
     */
    void setLargeMsgType(sensify_data_type_t type) { large_msg_type = type; }
    
    /**
     * @brief Obtiene el tipo Sensify asociado al large msg
     * @return sensify_data_type_t Tipo Sensify configurado o S_TYPE_EMPTY
     */
    sensify_data_type_t getLargeMsgType() { return large_msg_type; }

    // ---------------------------------------------------------- Funciones de escaneo
    
    /**
     * @brief Ejecuta un escaneo activo de targets ESP-NOW
     * @return esp_err_t ESP_OK en éxito o código de error
     */
    esp_err_t Scan();
    
    /**
     * @brief Setea la duracion del proceso de escaneo y lo inicia
     * @param duration Duración en ms
     */
    void setScanProcess(uint32_t duration) { scan.start(duration); }
    
    /**
     * @brief Verifica que finalize el proceso de escaneo
     * @return true si finalizo y se puede leer el resultado
     */
    bool scanRead();
    
    /**
     * @brief Imprime por log el resultado del último escaneo
     */
    void printScanResult();

    /**
     * @brief Envia mensaje de SCAN al broadcast
     * @param ch canal seteado antes de enviar el mensaje
     */
    esp_err_t send_broadcast_msg(uint8_t ch);
    
private:

    /**
     * @brief Tabla de assemble sessions por MAC origen
     */
    std::map<std::array<uint8_t,6>, Session> sessionMap;
    
    /**
     * @brief Manager de escaneo y lectura de resultados
     */
    ScanSession scan;

    /**
     * @brief Variable para guardar el canal actual antes de un scan
     */
    uint8_t orig_channel;

    // ---------------------------------------------------------- Callbacks

    send_log_cb _log_cb = nullptr;
    err_cb _err_cb = nullptr;
    start_recv_cb _start_recv_cb = nullptr;
    data_recv_cb _data_recv_cb = nullptr;
    finish_recv_cb _finish_recv_cb = nullptr;

    // --------------------------------------------------------------------

    /**
     * @brief IO para large msg y su tipo Sensify asociado
     */
    LargeMsgIO_t   _large_msg{};
    sensify_data_type_t large_msg_type = S_TYPE_EMPTY;

    /**
     * @brief Inicializa y configura la interfaz WiFi para ESP-NOW
     */
    void espnow_wifi_init(void);
    
    /**
     * @brief Punto de entrada del hilo/tsk de procesamiento
     */
    static void generic_thread_entry(void * pvParameter);
    
    /**
     * @brief Loop principal de procesamiento de eventos
     */
    void loop(void);

    // ---------------------------------------------------------- Funciones de envío
    
    /**
      * @brief Manager del ciclo de envío desde la cola interna
      */
    void send(void);
    
    /**
     * @brief Callback de ESP-NOW al terminar un envío
     */
    static void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status);
    
    /**
     * @brief Encola un msg para ser enviado
     * @param data Msg para enviar
     * @return esp_err_t ESP_OK si se encola; error si la cola está llena o inválida
     */
    esp_err_t enqueue_dataToSend(sensify_data_t *data);
    
    /**
     * @brief Desencola, envia y libera el msg
     */
    esp_err_t dequeue_dataToSend(void);
    
    /**
     * @brief Limpia la cola y libera recursos asociados a envío
     */
    esp_err_t clean_dataToSend(void);
    
    /**
     * @brief Señaliza que hay datos por enviar (evento interno)
     */
    esp_err_t enqueue_sendRequest(void);
    
    /**
     * @brief Fragmenta un msg en paquetes ESP-NOW y los envía
     * @param data_type Tipo Sensify
     * @param data Msg para enviar
     * @param data_len Tamaño total del mensaje para enviar
     * @param mac Direccion MAC destino
     */
    esp_err_t sendPackets(sensify_data_type_t data_type, uint8_t *data, size_t data_len, uint8_t *mac);
    
    // ---------------------------------------------------------- Funciones de recepción
    
    /**
     * @brief Manager del ciclo de recepción desde la cola interna
     */
    void receive(espnow_event_recv_cb_t *recv);

    /**
     * @brief Callback de ESP-NOW al recibir un paquete
     */
     static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len);
    
    /**
     * @brief Ensambla (en memoria) paquetes ESP-NOW de un msg para procesar
     * @param data Paquete ESP-NOW
     * @param data_len Tamaño del paquete ESP-NOW
     * @param target Dirección MAC de origen
     * @return assembleState:   ASSEMBLE_ERROR si fallo el assemble
                                ASSEMBLING si esta en proceso
                                READY si el mensaje esta listo para procesar
     */
    assembleState assembleData(uint8_t *data, size_t data_len, std::array<uint8_t,6> target);
    
    /**
     * @brief Ensamblador para mensajes que no entran en memoria RAM (utilizando LargeMsgIO_t)
     * @param data Paquete ESP-NOW
     * @param data_len Tamaño del paquete ESP-NOW
     * @param msg_size Tamaño total del msg
     * @return assembleState:   ASSEMBLE_ERROR si fallo el assemble
                                ASSEMBLING si esta en proceso
                                READY si el mensaje esta listo para procesar
    */
    assembleState assembleLargeMsg(uint8_t *data, size_t data_len, size_t *msg_size);
};

#endif
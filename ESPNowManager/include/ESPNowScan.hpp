/**
 * @file espnow_Scan.hpp
 * @brief Sesión de escaneo y almacenamiento de dispositivos para ESPNOW.
 *
 * Provee una clase ligera para gestionar un escaneo temporizado de dispositivos
 * cercanos y conservar, para cada MAC única, el mejor RSSI observado hasta un máximo de 5 dispositivos.
 */
#ifndef ESPNOW_SCAN_H
#define ESPNOW_SCAN_H

#include <array>
#include <vector>
#include <algorithm>
#include "esp_log.h"

#define SCAN_CH_INIT    0
#define SCAN_CH_FINISH  13

/**
 * @brief Gestiona una sesión de escaneo temporizada y almacena los mejores dispositivos por RSSI.
 *
 * Mantiene hasta 5 dispositivos únicos identificados por su dirección MAC. Si un
 * dispositivo vuelve a detectarse con mejor RSSI, se actualiza su valor. El ciclo
 * de escaneo se inicia con start() y se consulta periódicamente con read() hasta que
 * expira la duración indicada.
 */
class ScanSession {
public:
    /**
     * @brief Dispositivo detectado durante el escaneo.
     */
    typedef struct {
        std::array<uint8_t,6> mac;  ///< Dirección MAC del dispositivo (6 bytes).
        int rssi;                   ///< Intensidad de señal (RSSI) en dBm.
        int ch;                     ///< Channel
    } scanned_device_t;

    /**
     * @brief Estado interno del proceso de escaneo.
     */
    typedef struct {
        bool active;        ///< Indica si la sesión de escaneo está activa.
        uint32_t startTime; ///< Marca de tiempo (ms) al iniciar el escaneo.
        uint32_t duration;  ///< Duración objetivo del escaneo en milisegundos.
        uint8_t actual_ch;  ///< Ch donde se esta haciendo el scan
    } scan_process_t;

    typedef enum {
        OFF = 0,
        SCANNING = 1,
        SWITCH = 2,
        READY = 3,
    } scan_status_t;

    /**
     * @brief Construye una nueva sesión de escaneo con estado inactivo.
     */
    ScanSession() {
        proc.active = false;
        proc.startTime = 0;
        proc.duration = 0;
    }

    /**
     * @brief Inicia la sesión de escaneo por una duración dada.
     * @param duration Duración del escaneo en milisegundos.
     */
    void start(uint32_t duration) {
        proc.startTime = esp_log_timestamp();
        proc.duration = duration;
        proc.active = true;
        proc.actual_ch = 0;
    }

    /**
     * @brief Lee el estado del escaneo y determina si terminó por tiempo.
     *
     * Debe llamarse periódicamente. Si la sesión estaba activa y ya transcurrió la
     * duración configurada, marcará la sesión como inactiva y devolverá true.
     * @return true si el escaneo ha finalizado; false si sigue activo o no fue iniciado.
     */
    scan_status_t read() {
        uint32_t now = esp_log_timestamp(); // o millis()
        if (proc.active) {
            if ((now - proc.startTime) >= proc.duration) {
                if (proc.actual_ch >= SCAN_CH_FINISH) {
                    proc.active = false;
                    return READY; // terminó
                }
                proc.startTime = now;
                proc.actual_ch++;
                return SWITCH;
            }
            return SCANNING;
        }
        return OFF; // sigue activo o no estaba iniciado
    }

    /**
     * @brief Agrega o actualiza un dispositivo escaneado.
     *
     * Si la MAC ya existe, mantiene el mejor RSSI observado. Si no existe y
     * el almacenamiento está lleno (5 elementos), reemplaza al de menor RSSI
     * cuando el nuevo RSSI sea mejor.
     * @param mac Dirección MAC del dispositivo (6 bytes).
     * @param rssi Valor RSSI observado (dBm).
     */
    void add_scanned_device(const std::array<uint8_t,6>& mac, int rssi) {
        for (auto& d : scanned_devices) {
            if (d.mac == mac) {
                if (rssi > d.rssi) d.rssi = rssi; // actualizar si mejora
                return;
            }
        }
        scanned_device_t new_dev{mac, rssi, proc.actual_ch};

        if (scanned_devices.size() < 5) {
            scanned_devices.push_back(new_dev);
        } else {
            auto weakest_it = std::min_element(
                scanned_devices.begin(),
                scanned_devices.end(),
                [](const scanned_device_t& a, const scanned_device_t& b) {
                    return a.rssi < b.rssi;
                }
            );

            if (new_dev.rssi > weakest_it->rssi) {
                *weakest_it = new_dev;
            }
        }
    }

    /**
     * @brief Devuelve los resultados acumulados del escaneo.
     * @return Referencia constante al vector de dispositivos detectados.
     */
    const std::vector<scanned_device_t>& results() const {
        return scanned_devices;
    }

    /**
     * @brief Limpia todos los resultados almacenados.
     */
    void clear() { scanned_devices.clear(); }

    /**
     * @brief Indica si la sesión de escaneo está actualmente activa.
     * @return true si activa; false en caso contrario.
     */
    bool isActive() const { return proc.active; }

    uint8_t channel() const { return proc.actual_ch; }

private:
    scan_process_t proc; ///< Estado del proceso de escaneo.
    std::vector<ScanSession::scanned_device_t> scanned_devices; ///< Lista de dispositivos únicos detectados (máximo 5).
};

#endif
idf_component_register(SRCS "src/ESPNowManager.cpp"
                    INCLUDE_DIRS "." "./include"
                    REQUIRES freertos esp_common driver esp_system nvs_flash esp_event esp_netif esp_wifi
                    PRIV_REQUIRES sfy-Loggerlib)
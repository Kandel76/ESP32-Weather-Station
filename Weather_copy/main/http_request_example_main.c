/* HTTP GET Example using plain POSIX sockets

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "sdkconfig.h"

//Header for the temp sensor
#include <math.h>
#include "driver/i2c.h"
#include "freertos/task.h"
#include "freertos/FreeRTOS.h"

#define I2C_MASTER_NUM                  I2C_NUM_0
#define I2C_MASTER_SCL_IO               8
#define I2C_MASTER_SDA_IO               10
#define I2C_MASTER_FREQ_HZ              1000000
#define SHTC_SENSOR_ADDR                0x70
#define WRITE_DELAY                     5
#define POWER_ON_DELAY                  15

//functions to read temperature and humidity
void i2c_master_init(void){
        i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,                            // Set as master
        .sda_io_num = I2C_MASTER_SDA_IO,                    // SDA pin number
        .scl_io_num = I2C_MASTER_SCL_IO,                    // SCL pin number
        .sda_pullup_en = GPIO_PULLUP_ENABLE,                // Enable pull-up on SDA
        .scl_pullup_en = GPIO_PULLUP_ENABLE,                // Enable pull-up on SCL
        .master.clk_speed = I2C_MASTER_FREQ_HZ,              // Set clock frequency
        .clk_flags = 0
        };
    i2c_param_config(I2C_MASTER_NUM, &conf);                // Configure I2C parameters
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0); // Install I2C driver
}

bool check_crc(uint8_t *data) {
    uint8_t crc = 0xFF;
    for (int i = 0; i < 2; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80) crc = (crc << 1) ^ 0x31;
            else crc <<= 1;
        }
    }
    return (crc == data[2]);
}


void power_up(void){
        //wakeup command
        uint8_t cmd[]= {0x35,0x17};
        i2c_master_write_to_device(I2C_MASTER_NUM, SHTC_SENSOR_ADDR, cmd, 2, 1000 / portTICK_PERIOD_MS); // Send command
        vTaskDelay(POWER_ON_DELAY / portTICK_PERIOD_MS);  // Wait 15ms after wakeup
}

void power_down(void){
        uint8_t cmd[]= {0xB0, 0x98};
        i2c_master_write_to_device(I2C_MASTER_NUM, SHTC_SENSOR_ADDR, cmd, 2, 1000 / portTICK_PERIOD_MS); // Send command
        vTaskDelay(POWER_ON_DELAY / portTICK_PERIOD_MS);  // Wait 15ms after wakeup
}

float read_temp_cel(void) {
    uint8_t cmd[] = {0x7C, 0xA2};  // Temp first
    i2c_master_write_to_device(I2C_MASTER_NUM, SHTC_SENSOR_ADDR, cmd, 2, 1000 / portTICK_PERIOD_MS);

    vTaskDelay(WRITE_DELAY / portTICK_PERIOD_MS); //give it 10ms

    uint8_t data[3];
    i2c_master_read_from_device(I2C_MASTER_NUM, SHTC_SENSOR_ADDR, data, 3, 1000 / portTICK_PERIOD_MS);

    if (!check_crc(data)) {
    return NAN;}

    uint16_t raw_temp = (data[0] << 8) | data[1];
    return -45.0 + 175.0 * (raw_temp / 65535.0);
}

float read_humidity() {
    uint8_t cmd[] = {0x5C, 0x24};  // Hum first
    i2c_master_write_to_device(I2C_MASTER_NUM, SHTC_SENSOR_ADDR, cmd, 2, 1000 / portTICK_PERIOD_MS);

    vTaskDelay(WRITE_DELAY / portTICK_PERIOD_MS);

    uint8_t data[3];
    i2c_master_read_from_device(I2C_MASTER_NUM, SHTC_SENSOR_ADDR, data, 3, 1000 / portTICK_PERIOD_MS);

    // Verify checksum
    // if(checksum_fails) return NAN;
    if (!check_crc(data)) {
    printf("CRC check failed!\n");
    return NAN;}

    uint16_t raw_hum = (data[0] << 8) | data[1];
    //printf("DEBUG: Raw Hum Value: 0x%04X\n", raw_hum);
    return 100.0 * (raw_hum / 65535.0);  // Convert to percentage
}


/* Constants that aren't configurable in menuconfig */
#define WEB_SERVER "192.168.0.158"
#define WEB_PORT "1234"
#define WEB_PATH "/"

static const char *TAG = "example";


static void http_post_task(void *pvParameters){
    char *post_data = (char *)pvParameters;

    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res;
    struct in_addr *addr;
    int s, r;
    char recv_buf[64];

    while(1) {
        int err = getaddrinfo(WEB_SERVER, WEB_PORT, &hints, &res);

        if(err != 0 || res == NULL) {
            ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
        ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

        s = socket(res->ai_family, res->ai_socktype, 0);
        if(s < 0) {
            ESP_LOGE(TAG, "... Failed to allocate socket.");
            freeaddrinfo(res);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "... allocated socket");

        if(connect(s, res->ai_addr, res->ai_addrlen) != 0) {
            ESP_LOGE(TAG, "... socket connect failed errno=%d", errno);
            close(s);
            freeaddrinfo(res);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI(TAG, "... connected");
        freeaddrinfo(res);

        // Build HTTP POST request with data
        char request[512];
        snprintf(request, sizeof(request),
            "POST " WEB_PATH " HTTP/1.1\r\n"
            "Host: "WEB_SERVER ":" WEB_PORT "\r\n"
            "User-Agent: esp-idf/1.0 esp32\r\n"
            "Content-Type: application/x-www-form-urlencoded\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            strlen(post_data), post_data);

        if (write(s, request, strlen(request)) < 0) {
            ESP_LOGE(TAG, "... socket send failed");
            close(s);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "... socket send success");

        struct timeval receiving_timeout;
        receiving_timeout.tv_sec = 5;
        receiving_timeout.tv_usec = 0;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout, sizeof(receiving_timeout));

        do {
            bzero(recv_buf, sizeof(recv_buf));
            r = read(s, recv_buf, sizeof(recv_buf)-1);
            for(int i = 0; i < r; i++) {
                putchar(recv_buf[i]);
            }
        } while(r > 0);

        ESP_LOGI(TAG, "... done reading from socket. Last read return=%d errno=%d.", r, errno);
        close(s);
        break; // Only send once per task
    }
    free(post_data); // Free the allocated memory
    vTaskDelete(NULL);
}

//-----------------------------------------------------

static void http_get_location(char *location_buf, size_t buf_size) {
    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res;
    int s, r;
    char recv_buf[128];
    char request[128];
    
    snprintf(request, sizeof(request),
    "GET /location HTTP/1.1\r\n"  //change this to your desired location
    "Host: %s\r\n"
    "User-Agent: curl\r\n"
    "\r\n", WEB_SERVER);
        
    int err = getaddrinfo(WEB_SERVER, WEB_PORT, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS lookup failed for location");
        return;
    }
    
    s = socket(res->ai_family, res->ai_socktype, 0);
    if (s < 0) {
        ESP_LOGE(TAG, "Failed to allocate socket for location");
        freeaddrinfo(res);
        return;
    }

    if (connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "Socket connect failed for location");
        close(s);
        freeaddrinfo(res);
        return;
    }
    freeaddrinfo(res);
    
    if (write(s, request, strlen(request)) < 0) {
        ESP_LOGE(TAG, "Socket send failed for location");
        close(s);
        return;
    }

    // Read HTTP response, skip headers, copy body to location_buf
    bool header_ended = false;
    size_t loc_len = 0;
    while ((r = read(s, recv_buf, sizeof(recv_buf)-1)) > 0) {
        recv_buf[r] = 0;
        char *body = recv_buf;
        if (!header_ended) {
            char *header_end = strstr(recv_buf, "\r\n\r\n");
            if (header_end) {
                header_ended = true;
                body = header_end + 4;
            } else {
                continue;
            }
        }
        size_t body_len = strlen(body);
        if (loc_len + body_len < buf_size) {
            memcpy(location_buf + loc_len, body, body_len);
            loc_len += body_len;
        }
    }
    location_buf[loc_len] = 0;
    close(s);
}

//-----------------------------------------------------
#define WEB_PORT_WTTR "80"  // Port for wttr.in
#define WEB_SERVER_WTTR "wttr.in"  // Port for wttr.in
// #define WEB_PATH_WTTR "/San-Francisco?format=%t"  // Weather for Tokyo, Japan

static void http_get_wttr_temp(const char *location, char *temp_buf, size_t buf_size) {
    // snprintf(path, sizeof(path), "/%s?format=%%t", location);
    char WEB_PATH_WTTR[64];  // Weather for a given location

    snprintf(WEB_PATH_WTTR, sizeof(WEB_PATH_WTTR),
        "/%s?format=%%t", location);  // Format for wttr.in to get temperature

    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res;
    int s, r;
    char recv_buf[128];
    char request[256];

    snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\n"
        "Host: %s:%s\r\n"
        "User-Agent: esp-idf/1.0 curl\r\n"
        "\r\n", WEB_PATH_WTTR, WEB_SERVER_WTTR, WEB_PORT_WTTR);

    int err = getaddrinfo(WEB_SERVER_WTTR, WEB_PORT_WTTR, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS lookup failed for wttr.in");
        return;
    }

    s = socket(res->ai_family, res->ai_socktype, 0);
    if (s < 0) {
        ESP_LOGE(TAG, "Failed to allocate socket for wttr.in");
        freeaddrinfo(res);
        return;
    }

    if (connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "Socket connect failed for wttr.in");
        close(s);
        freeaddrinfo(res);
        return;
    }
    freeaddrinfo(res);

    if (write(s, request, strlen(request)) < 0) {
        ESP_LOGE(TAG, "Socket send failed for wttr.in");
        close(s);
        return;
    }

    // Read HTTP response, skip headers, copy body to temp_buf
    bool header_ended = false;
    size_t temp_len = 0;
    while ((r = read(s, recv_buf, sizeof(recv_buf)-1)) > 0) {
        recv_buf[r] = 0;
        char *body = recv_buf;
        if (!header_ended) {
            char *header_end = strstr(recv_buf, "\r\n\r\n");
            if (header_end) {
                header_ended = true;
                body = header_end + 4;
            } else {
                continue;
            }
        }
        size_t body_len = strlen(body);
        if (temp_len + body_len < buf_size) {
            memcpy(temp_buf + temp_len, body, body_len);
            temp_len += body_len;
        }
    }
    temp_buf[temp_len] = 0;
    close(s);
}



void app_main(void)
{
    ESP_ERROR_CHECK( nvs_flash_init() );
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
    * Read "Establishing Wi-Fi or Ethernet Connection" section in
    * examples/protocols/README.md for more information about this function.
    */
    ESP_ERROR_CHECK(example_connect());
    
    i2c_master_init();  // Set up I2C bus
    char location[32] = {0};
    char wttr_temp[16] = {0};
    int temp_c_int = 0;
    int temp_f_int =0;
    int hum_int = 0;
    
    while (1) {

        power_up();
        vTaskDelay(2/ portTICK_PERIOD_MS);
        float temp_c = read_temp_cel();

        power_down();

        power_up();

        float hum = read_humidity();

        power_down();

        if (!isnan(temp_c) && !isnan(hum)) {
                temp_c_int = round(temp_c);
                temp_f_int = round(temp_c * 9.0 / 5.0 + 32);
                hum_int = round(hum);
                printf("Temperature is %dC (or %dF) with a %d%% humidity\n", temp_c_int, temp_f_int, hum_int);
        } else {
                continue;
                        vTaskDelay(2000 / portTICK_PERIOD_MS);  // Wait 2 seconds
        }

        // 1. Get location from server ~~~~~WORKS~~~~~~~~
        http_get_location(location, sizeof(location));
        // location[strcspn(location, "\r\n ")] = 0;

        // 2. Get temperature from wttr.in ~~~~~WORKS~~~~~~~~
        http_get_wttr_temp(location, wttr_temp, sizeof(wttr_temp));
        // wttr_temp[strcspn(wttr_temp, "\r\n ")] = 0; // Remove trailing newline or whitespace

        // 3. Combine and send ~~~~~~WORKS~~~~~~~~
        char *post_data = malloc(200);
        if (post_data == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for post_data");
            continue;
        }
        snprintf(post_data, 200, 
            "\n\n======================================\n\nLocation: %s\nTemp= %s \n\nLocation: Local\nTemp: +%d°F\nLocal Humidity: %d%%\n\n======================================\n\n", 
            location, wttr_temp, temp_f_int, hum_int);
            
        //post the information to the server ~~~~~~WORKS~~~~~~~~
        xTaskCreate(&http_post_task, "http_post_task", 4096, post_data, 5, NULL);
        ESP_LOGE(TAG, "%s",post_data);

        vTaskDelay(1000 / portTICK_PERIOD_MS);  // Wait 1 second
        }
}

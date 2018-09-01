/**
 * Very basic example showing usage of access point mode and the DHCP server.
 * The ESP in the example runs a telnet server on 172.16.0.1 (port 23) that
 * outputs some status information if you connect to it, then closes
 * the connection.
 *
 * This example code is in the public domain.
 */
#include <string.h>

#include <espressif/esp_common.h>
#include <esp/uart.h>
#include <FreeRTOS.h>
#include <dhcpserver.h>
#include <httpd/httpd.h>
#include <lwip/api.h>
#include "smartConfig.h"
#include <paho_mqtt_c/MQTTESP8266.h>
#include <paho_mqtt_c/MQTTClient.h>
#include "colorLed.h"

#define MQTT_HOST ("wifi.h2popo.com")
#define MQTT_PORT 8083

#define MQTT_USER ("wopin")
#define MQTT_PASS ("wopinH2popo")

#define PUB_MSG_LEN 16

#define AP_SSID "H2PoPo"
#define AP_PSK "12345678"

#define SCL_PIN (14)
#define SDA_PIN (2)
#define HYDRO_PIN_A (1)
#define HYDRO_PIN_B (3)

static void wifi_task(void *pvParameters);

static void beat_task(void *pvParameters);
static void ap_task(void *pvParameters);
static void ap_count_task(void *pvParameters);
static void mqtt_task(void *pvParameters);
static void signal_task(void *pvParameters);
static void led_task(void *pvParameters);
static void key_led_task(void *pvParameters);
static void topic_received(mqtt_message_data_t *md);
static void hydro_task(void *pvParameters);
static void send_to_pmc_task(void *pvParameters);

const gpio_inttype_t int_type = GPIO_INTTYPE_EDGE_NEG;
void gpio_intr_handler(uint8_t gpio_num);
void wifiScanDoneCb(void *arg, sdk_scan_status_t status);

SemaphoreHandle_t wifi_alive;
QueueHandle_t publish_queue;

char mqtt_client_id[30];  // this is device id
char mqtt_client_id_sub[30];  // this is device id

const uint32_t wakeupTime = 30*60*1000*1000;
const int gpio = 14;   /* gpio 0 usually has "PROGRAM" button attached */
const int active = 0; /* active == 0 for active low */

static const char * const auth_modes [] = {
    [AUTH_OPEN]         = "Open",
    [AUTH_WEP]          = "WEP",
    [AUTH_WPA_PSK]      = "WPA/PSK",
    [AUTH_WPA2_PSK]     = "WPA2/PSK",
    [AUTH_WPA_WPA2_PSK] = "WPA/WPA2/PSK"
};

static QueueHandle_t tsqueue;

void gpio_intr_handler(uint8_t gpio_num)
{
    uint32_t now = xTaskGetTickCountFromISR();
    xQueueSendToBackFromISR(tsqueue, &now, ( TickType_t ) 0U);
}


//WiFi access point data
typedef struct {
    char ssid[32];
    char bssid[8];
    int channel;
    char rssi;
    char enc;
} ApData;

//Scan result
typedef struct {
    char scanInProgress; //if 1, don't access the underlying stuff from the webpage.
    ApData **apData;
    int noAps;
} ScanResultData;

//Static scan status storage.
static ScanResultData cgiWifiAps;

void gpio_init(void) 
{
    gpio_enable(SCL_PIN, GPIO_INPUT);
    gpio_enable(SDA_PIN, GPIO_INPUT);
    //gpio_enable(HYDRO_PIN_A, GPIO_OUTPUT);
    //gpio_enable(HYDRO_PIN_B, GPIO_OUTPUT);
    
    tsqueue = xQueueCreate(2, sizeof(uint32_t));
}

int send_cmd = 0; // 1 : 0xc1 (Start hydro) 2: 0xc2 (Close hydro) 3: 0xcd (Finish clean)
static void send_to_pmc_task(void *pvParameters)
{
    while (true)
    {
        bool state = gpio_read(SCL_PIN);
        if (state && send_cmd != 0) 
        {
            printf("send command to pmc %d\r\n", send_cmd);
            gpio_enable(SCL_PIN, GPIO_OUT_OPEN_DRAIN);
            gpio_enable(SDA_PIN, GPIO_OUT_OPEN_DRAIN);
            gpio_write(SCL_PIN, 0);
            sdk_os_delay_us(100);
            for (uint8_t i = 0; i < 8; i++) {
                if (i == 0)
                    gpio_write(SDA_PIN, 1);
                else if (i == 1)
                    gpio_write(SDA_PIN, 1);
                else if (i == 2)
                    gpio_write(SDA_PIN, 0);
                else if (i == 3)
                    gpio_write(SDA_PIN, 0);
                else if (i == 4) {
                    if (send_cmd == 3) {
                        gpio_write(SDA_PIN, 1);
                    } else  {
                        gpio_write(SDA_PIN, 0);
                    }
                } else if (i == 5) {
                    if (send_cmd == 3) {
                        gpio_write(SDA_PIN, 1);
                    } else  {
                        gpio_write(SDA_PIN, 0);
                    }
                } else if (i == 6) {
                    if (send_cmd == 2) {
                        gpio_write(SDA_PIN, 1);
                    } else  {
                        gpio_write(SDA_PIN, 0);
                    }
                } else if (i == 7) {
                    if (send_cmd == 2) {
                        gpio_write(SDA_PIN, 0);
                    } else  {
                        gpio_write(SDA_PIN, 1);
                    }
                }
                sdk_os_delay_us(100);
            }
            gpio_write(SCL_PIN, 1);
            sdk_os_delay_us(100);
            gpio_enable(SCL_PIN, GPIO_INPUT);
            gpio_enable(SDA_PIN, GPIO_INPUT);
            send_cmd = 0;
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

//key_led_mode == 0 : blue breath mode
//key_led_mode == 1 : color breath mode
//key_led_mode == 2 : red led on only
//key_led_mode == 3 : green led on only
//key_led_mode == 99 : Off
int key_led_mode = 99; 
bool key_led_forward = true;
int key_color_mode = 0;
int key_r_count = 0, key_g_count = 0, key_b_count = 0;
static void key_led_task(void *pvParameters)
{
    while(1) {
        if (key_led_mode == 0) {
            set_key_led(0, 0, key_b_count);
            if (key_led_forward) key_b_count++;
            else key_b_count--; 
            if (key_b_count == 128) { 
                key_led_forward = false;
            } else if (key_b_count == 0) {
                key_led_forward = true;
            }
        } else if (key_led_mode == 1) {
            //Do in led_task
        } else if (key_led_mode == 2) {
            set_key_led(50, 0, 0);
        } else if (key_led_mode == 3) {
            set_key_led(0, 50, 0);
        } else if (key_led_mode == 99) {
            set_key_led(0, 0, 0);
        } else {

        }
        vTaskDelay(15 / portTICK_PERIOD_MS);
    }
}

//led_mode == 0 : blue breath mode
//led_mode == 1 : color breath mode
//led_mode == 2 : green color on 
//led_mode == 99 : Off
int led_mode = 99; 
bool led_forward = true;
int color_mode = 0;
int r_count = 0, g_count = 0, b_count = 0;
static void led_task(void *pvParameters)
{
    while (1) {
        if (led_mode == 0) { //
            set_led(0, 0, b_count);
            if (led_forward) b_count++;
            else b_count--; 
            if (b_count == 128) { 
                led_forward = false;
            } else if (b_count == 0) {
                led_forward = true;
            }
        } else if (led_mode == 2) {
            set_led(0, 50, 0);
        } 
        else if (led_mode == 1) {
            set_led(r_count, g_count, b_count);
            set_key_led(r_count, g_count, b_count);
            if (color_mode == 0) {    //Green
                if (led_forward) g_count++;
                else g_count--;
                if (g_count == 128) { 
                    led_forward = false;
                } else if (g_count == 0) {
                    led_forward = true;
                    color_mode++;
                    r_count = 0; g_count = 0; b_count = 0;
                }
            } else if (color_mode == 1) {  //Orange
                if (led_forward) { 
                    r_count = r_count + 2;
                    g_count++;
                }
                else {
                    r_count = r_count - 2;
                    g_count--;
                }
                if (g_count == 128) { 
                    led_forward = false;
                } else if (g_count == 0) {
                    led_forward = true;
                    color_mode++;
                    r_count = 0; g_count = 0; b_count = 0;
                }
            } else if (color_mode == 2) {    //Red
                if (led_forward) r_count++;
                else r_count--;
                if (r_count == 128) { 
                    led_forward = false;
                } else if (r_count == 0) {
                    led_forward = true;
                    color_mode++;
                    r_count = 0; g_count = 0; b_count = 0;
                }
            } else if (color_mode == 3) {  //purple
                if (led_forward) { 
                    b_count = b_count + 2;
                    r_count++;
                }
                else  {
                    b_count = b_count - 2;
                    r_count--;
                }
                if (r_count == 128) { 
                    led_forward = false;
                } else if (r_count == 0) {
                    led_forward = true;
                    color_mode++;
                    r_count = 0; g_count = 0; b_count = 0;
                } 
            } else if (color_mode == 4) {    //blue
                if (led_forward) b_count++;
                else b_count--;
                if (b_count == 128) { 
                    led_forward = false;
                } else if (b_count == 0) {
                    led_forward = true;
                    color_mode++;
                    r_count = 0; g_count = 0; b_count = 0;
                }
            } else if (color_mode == 5) {    //cyan
                if (led_forward) { 
                    b_count = b_count + 2;
                    g_count++;
                }
                else  {
                    b_count = b_count - 2;
                    g_count--;
                }
                if (g_count == 128) { 
                    led_forward = false;
                } else if (g_count == 0) {
                    led_forward = true;
                    color_mode = 0;
                    r_count = 0; g_count = 0; b_count = 0;
                } 
            } 
        } else if (led_mode == 99) {
            set_led(0, 0, 0);
        } else {

        }
        vTaskDelay(15 / portTICK_PERIOD_MS);
    }
}

void buttonPollTask(void *pvParameters)
{
    printf("Polling for button press on gpio %d...\r\n", gpio);
    bool isPrevPressed = false;
    uint32_t count = 0;
    while(1) {
        if(gpio_read(gpio) == active)
        {
            if (isPrevPressed) {
                count++;
            }
            isPrevPressed = true;
        } else {
            isPrevPressed = false;
            count = 0;
        }
        if (count > 5*10)
        {
            printf("Reset to AP mode. Restarting system...\n");
            break;
        }
        vTaskDelay(200 / portTICK_PERIOD_MS);
    }
}

int modem_sleep_timer = 0;
int deep_sleep_timer = 0;
int hydro_timer = 0;
int hydro_mode = 0; // 0: normal mode 1: clean mode
static void hydro_task(void *pvParameters)
{
    while(1) {
        if (hydro_timer != 0) {
            if (hydro_mode == 0) {
                gpio_write(HYDRO_PIN_A, 0);
                gpio_write(HYDRO_PIN_B, 1);
            } else {
                gpio_write(HYDRO_PIN_A, 1);
                gpio_write(HYDRO_PIN_B, 0);
            }
            hydro_timer--;
            printf("hydro...%d\r\n", hydro_timer);
            if (hydro_timer == 0) { // If timer count down to 0 success, then send 0xc2 to pmc
                led_mode = 99;
                key_led_mode = 99;
                send_cmd = 2;
            }
        } else {
            gpio_write(HYDRO_PIN_A, 0);
            gpio_write(HYDRO_PIN_B, 0);
        }
       
        if (modem_sleep_timer == 5*60) { // go to modem sleep mode
            key_led_mode = 99;
            led_mode = 99;
            deep_sleep_timer++;
        } else {
            modem_sleep_timer++;
        }
        if (hydro_timer == 0 && deep_sleep_timer >= 15*60) {  //If finish hydro go to deep sleep mode
            sdk_system_deep_sleep(wakeupTime);
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

static void beat_task(void *pvParameters)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    char msg[PUB_MSG_LEN];
    int count = 0;

    while (1) {
        vTaskDelayUntil(&xLastWakeTime, 10000 / portTICK_PERIOD_MS);
        uint32_t adc_read = sdk_system_adc_read();
        printf("adc_read: %d\r\n", adc_read);

        if (adc_read <= 537) {
            led_mode = 50; 
            key_led_mode = 50; 
            set_led(50, 0, 0); 
            set_key_led(50, 0, 0);
            vTaskDelay(500 / portTICK_PERIOD_MS);
            set_led(0, 0, 0); 
            set_key_led(0, 0, 0);
            vTaskDelay(500 / portTICK_PERIOD_MS);
            set_led(50, 0, 0); 
            set_key_led(50, 0, 0);
            vTaskDelay(500 / portTICK_PERIOD_MS);
            set_led(0, 0, 0); 
            set_key_led(0, 0, 0);
            vTaskDelay(500 / portTICK_PERIOD_MS);
            set_led(50, 0, 0); 
            set_key_led(50, 0, 0);
            vTaskDelay(500 / portTICK_PERIOD_MS);
            set_led(0, 0, 0); 
            set_key_led(0, 0, 0);
            gpio_write(HYDRO_PIN_A, 0);
            gpio_write(HYDRO_PIN_B, 0);
            send_cmd = 2;
        }
        printf("beat\r\n");
            /* Print date and time each 5 seconds */
        uint8_t status = sdk_wifi_station_get_connect_status();
        if (status == STATION_GOT_IP)
        {
            snprintf(msg, PUB_MSG_LEN, "P:%d\r\n", (int)((adc_read - 584)*0.422));
            if (xQueueSend(publish_queue, (void *)msg, 0) == pdFALSE) {
                printf("Publish queue overflow.\r\n");
            }
        }
    }
}

static const char* get_my_id(void)
{
    // Use MAC address for Station as unique ID
    static char my_id[13];
    static bool my_id_done = false;
    int8_t i;
    uint8_t x;
    if (my_id_done)
        return my_id;
    if (!sdk_wifi_get_macaddr(STATION_IF, (uint8_t *)my_id))
        return NULL;
    for (i = 5; i >= 0; --i)
    {
        x = my_id[i] & 0x0F;
        if (x > 9) x += 7;
        my_id[i * 2 + 1] = x + '0';
        x = my_id[i] >> 4;
        if (x > 9) x += 7;
        my_id[i * 2] = x + '0';
    }
    my_id[12] = '\0';
    my_id_done = true;
    return my_id;
}

int ap_count = 0;
static void ap_count_task(void *pvParameters)
{
    while(1)
    {
        if (ap_count == 3 * 60) {
            printf("AP Mode Timeout\r\n");
            sdk_system_deep_sleep(wakeupTime); 
        }
        ap_count++;
        vTaskDelay( 1000 / portTICK_PERIOD_MS );
    }
}

void buttonIntTask(void *pvParameters)
{
    printf("Waiting for button press interrupt on gpio %d...\r\n", gpio);
    QueueHandle_t *tsqueue = (QueueHandle_t *)pvParameters;
    gpio_set_interrupt(SCL_PIN, int_type, gpio_intr_handler);
    uint32_t last = 0;
    while(1) {
        uint32_t button_ts;
        xQueueReceive(*tsqueue, &button_ts, portMAX_DELAY);

        taskYIELD();
        modem_sleep_timer = 0; 
        deep_sleep_timer = 0;
        ap_count = 0;
        //gpio_enable(SDA_PIN, GPIO_INPUT);
        sdk_os_delay_us(1000);
        uint8_t buf[8];
        for (uint8_t i = 0; i < 8; i++) {
            sdk_os_delay_us(50);
            uint8_t r = gpio_read(SDA_PIN);
            buf[i] = r;
            sdk_os_delay_us(50);
        }
        printf("\r\n");
        printf("Data : %d%d%d%d %d%d%d%d \r\n", buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
        if (buf[0] == 1 && buf[1] == 1 && buf[2] == 0 && buf[3] == 0) // c : 1101
        {
            if (buf[4] == 0 && buf[5] == 0 && buf[6] == 0 && buf[7] == 1) // 0xc1
            {
                printf("0xc1 command Received\r\n");
                hydro_mode = 0; 
                hydro_timer = 5 * 60;
                color_mode = 0, key_color_mode = 0;
                g_count = 0; r_count = 0; b_count = 0;
                key_g_count = 0; key_r_count = 0; key_b_count = 0;
                led_forward = true; key_led_forward = true;
                key_led_mode = 1;
                led_mode = 1;
            } else if (buf[4] == 0 && buf[5] == 1 && buf[6] == 0 && buf[7] == 1) //0xc5 key led red, main led off
            {
                printf("0xc5 command Received\r\n");
                key_led_mode = 2;
                led_mode = 99;
                hydro_timer = 0;
            } else if (buf[4] == 0 && buf[5] == 0 && buf[6] == 1 && buf[7] == 0) //0xc2
            {
                //send_cmd = 1;  //send 0xc1 to pmc
                //send_cmd = 2;  //send 0xc2 to pmc
                //send_cmd = 3;  //send 0xcd to pmc
                hydro_timer = 0; //off hydro
                led_mode = 99;
                key_led_mode = 99;
                printf("0xc2 command Received\r\n");
            } else if (buf[4] == 0 && buf[5] == 1 && buf[6] == 1 && buf[7] == 1) //0xc7
            {
                printf("0xc7 command Received\r\n");
                led_mode = 50; 
                key_led_mode = 50; 
                hydro_timer = 0;
                set_led(50, 0, 0); 
                set_key_led(50, 0, 0);
                vTaskDelay(500 / portTICK_PERIOD_MS);
                set_led(0, 0, 0); 
                set_key_led(0, 0, 0);
                vTaskDelay(500 / portTICK_PERIOD_MS);
                set_led(50, 0, 0); 
                set_key_led(50, 0, 0);
                vTaskDelay(500 / portTICK_PERIOD_MS);
                set_led(0, 0, 0); 
                set_key_led(0, 0, 0);
                vTaskDelay(500 / portTICK_PERIOD_MS);
                set_led(50, 0, 0); 
                set_key_led(50, 0, 0);
                vTaskDelay(500 / portTICK_PERIOD_MS);
                set_led(0, 0, 0); 
                set_key_led(0, 0, 0);
                //ToDo: send command to PMC
                send_cmd = 2; 
            } else if (buf[4] == 1 && buf[5] == 0 && buf[6] == 0 && buf[7] == 0) //0xc8
            {
                printf("0xc8 command Received\r\n");
                led_mode = 50; 
                key_led_mode = 50; 
                set_led(50, 0, 0); 
                set_key_led(50, 0, 0);
                vTaskDelay(500 / portTICK_PERIOD_MS);
                set_led(0, 0, 0); 
                set_key_led(0, 0, 0);
                vTaskDelay(500 / portTICK_PERIOD_MS);
                set_led(50, 0, 0); 
                set_key_led(50, 0, 0);
                vTaskDelay(500 / portTICK_PERIOD_MS);
                set_led(0, 0, 0); 
                set_key_led(0, 0, 0);
                vTaskDelay(500 / portTICK_PERIOD_MS);
                set_led(50, 0, 0); 
                set_key_led(50, 0, 0);
                vTaskDelay(500 / portTICK_PERIOD_MS);
                set_led(0, 0, 0); 
                set_key_led(0, 0, 0);
                //ToDo: send command to PMC
                send_cmd = 2;
            } else if (buf[4] == 1 && buf[5] == 0 && buf[6] == 0 && buf[7] == 1) //0xc9
            {
                printf("0xc9 command Received\r\n");
                color_mode = 0, key_color_mode = 0;
                g_count = 0; r_count = 0; b_count = 0;
                key_g_count = 0; key_r_count = 0; key_b_count = 0;
                led_forward = true; key_led_forward = true;
                if (led_mode == 1) {
                    led_mode = 99;
                    key_led_mode = 0;
                } else {
                    led_mode = 1;   //color led breath mode
                    key_led_mode = 1;
                }
            } else if (buf[4] == 0 && buf[5] == 1 && buf[6] == 1 && buf[7] == 0) //0xc6 
            {
                printf("0xc6 command Received\r\n");
                key_led_mode = 3;
            } else if (buf[4] == 1 && buf[5] == 0 && buf[6] == 1 && buf[7] == 1) //0xcb
            {
                printf("0xcb command Received\r\n");
                set_device_state();
                sdk_system_restart();
                break;
            }
        }
    }
}

static void signal_task(void *pvParameters)
{
    while (true)
    {
        taskYIELD();
        taskENTER_CRITICAL();
        bool state = gpio_read(SCL_PIN);
        if (!state)
        {
            //Received any signal from PMC, restart timer
            modem_sleep_timer = 0; 
            deep_sleep_timer = 0;
            ap_count = 0;
            gpio_enable(SDA_PIN, GPIO_INPUT);
            sdk_os_delay_us(1000);
            uint8_t buf[8];
            for (uint8_t i = 0; i < 8; i++) {
                sdk_os_delay_us(50);
                uint8_t r = gpio_read(SDA_PIN);
                buf[i] = r;
                sdk_os_delay_us(50);
            }
            printf("\r\n");
            printf("Data : %d%d%d%d %d%d%d%d \r\n", buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
            if (buf[0] == 1 && buf[1] == 1 && buf[2] == 0 && buf[3] == 0) // c : 1101
            {
                if (buf[4] == 0 && buf[5] == 0 && buf[6] == 0 && buf[7] == 1) // 0xc1
                {
                    printf("0xc1 command Received\r\n");
                    hydro_mode = 0; 
                    hydro_timer = 5 * 60;
                    color_mode = 0, key_color_mode = 0;
                    g_count = 0; r_count = 0; b_count = 0;
                    key_g_count = 0; key_r_count = 0; key_b_count = 0;
                    led_forward = true; key_led_forward = true;
                    key_led_mode = 1;
                    led_mode = 1;
                } else if (buf[4] == 0 && buf[5] == 1 && buf[6] == 0 && buf[7] == 1) //0xc5 key led red, main led off
                {
                    printf("0xc5 command Received\r\n");
                    key_led_mode = 2;
                    led_mode = 99;
                    hydro_timer = 0;
                } else if (buf[4] == 0 && buf[5] == 0 && buf[6] == 1 && buf[7] == 0) //0xc2
                {
                    //send_cmd = 1;  //send 0xc1 to pmc
                    //send_cmd = 2;  //send 0xc2 to pmc
                    //send_cmd = 3;  //send 0xcd to pmc
                    hydro_timer = 0; //off hydro
                    led_mode = 99;
                    key_led_mode = 99;
                    printf("0xc2 command Received\r\n");
                } else if (buf[4] == 0 && buf[5] == 1 && buf[6] == 1 && buf[7] == 1) //0xc7
                {
                    printf("0xc7 command Received\r\n");
                    led_mode = 50; 
                    key_led_mode = 50; 
                    hydro_timer = 0;
                    set_led(50, 0, 0); 
                    set_key_led(50, 0, 0);
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                    set_led(0, 0, 0); 
                    set_key_led(0, 0, 0);
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                    set_led(50, 0, 0); 
                    set_key_led(50, 0, 0);
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                    set_led(0, 0, 0); 
                    set_key_led(0, 0, 0);
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                    set_led(50, 0, 0); 
                    set_key_led(50, 0, 0);
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                    set_led(0, 0, 0); 
                    set_key_led(0, 0, 0);
                    //ToDo: send command to PMC
                    send_cmd = 2; 
                } else if (buf[4] == 1 && buf[5] == 0 && buf[6] == 0 && buf[7] == 0) //0xc8
                {
                    printf("0xc8 command Received\r\n");
                    led_mode = 50; 
                    key_led_mode = 50; 
                    set_led(50, 0, 0); 
                    set_key_led(50, 0, 0);
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                    set_led(0, 0, 0); 
                    set_key_led(0, 0, 0);
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                    set_led(50, 0, 0); 
                    set_key_led(50, 0, 0);
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                    set_led(0, 0, 0); 
                    set_key_led(0, 0, 0);
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                    set_led(50, 0, 0); 
                    set_key_led(50, 0, 0);
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                    set_led(0, 0, 0); 
                    set_key_led(0, 0, 0);
                    //ToDo: send command to PMC
                    send_cmd = 2;
                } else if (buf[4] == 1 && buf[5] == 0 && buf[6] == 0 && buf[7] == 1) //0xc9
                {
                    printf("0xc9 command Received\r\n");
                    color_mode = 0, key_color_mode = 0;
                    g_count = 0; r_count = 0; b_count = 0;
                    key_g_count = 0; key_r_count = 0; key_b_count = 0;
                    led_forward = true; key_led_forward = true;
                    if (led_mode == 1) {
                        led_mode = 99;
                        key_led_mode = 0;
                    } else {
                        led_mode = 1;   //color led breath mode
                        key_led_mode = 1;
                    }
                } else if (buf[4] == 0 && buf[5] == 1 && buf[6] == 1 && buf[7] == 0) //0xc6 
                {
                    printf("0xc6 command Received\r\n");
                    key_led_mode = 3;
                } else if (buf[4] == 1 && buf[5] == 0 && buf[6] == 1 && buf[7] == 1) //0xcb
                {
                    printf("0xcb command Received\r\n");
                    set_device_state();
                    sdk_system_restart();
                    break;
                }
            }
            //gpio_enable(SDA_PIN, GPIO_OUT_OPEN_DRAIN);
        }
        taskEXIT_CRITICAL();
        sdk_os_delay_us(10);
    }
}

static void mqtt_task(void *pvParameters)
{
    int ret = 0;
    struct mqtt_network network;
    mqtt_client_t client   = mqtt_client_default;
    uint8_t mqtt_buf[100];
    uint8_t mqtt_readbuf[100];
    mqtt_packet_connect_data_t data = mqtt_packet_connect_data_initializer;

    mqtt_network_new( &network );
    while(1) {
        xSemaphoreTake(wifi_alive, portMAX_DELAY);
        printf("%s: started\n\r", __func__);
        printf("%s: (Re)connecting to MQTT server %s ... ",__func__,
               MQTT_HOST);
        ret = mqtt_network_connect(&network, MQTT_HOST, MQTT_PORT);
        if( ret ){
            printf("error: %d\n\r", ret);
            //taskYIELD();
            continue;
        }
        printf("done\n\r");
        mqtt_client_new(&client, &network, 5000, mqtt_buf, 100,
                      mqtt_readbuf, 100);

        data.willFlag       = 0;
        data.MQTTVersion    = 3;
        data.clientID.cstring   = mqtt_client_id;
        data.username.cstring   = MQTT_USER;
        data.password.cstring   = MQTT_PASS;
        data.keepAliveInterval  = 10;
        data.cleansession   = 0;
        printf("Send MQTT connect ... ");
        ret = mqtt_connect(&client, &data);
        if(ret){
            printf("error: %d\n\r", ret);
            mqtt_network_disconnect(&network);
            //taskYIELD();
            continue;
        }
        //mqtt_subscribe(&client, mqtt_client_id, MQTT_QOS0, topic_received);
        mqtt_subscribe(&client, mqtt_client_id_sub, MQTT_QOS0, topic_received);
        xQueueReset(publish_queue);

        while(1){

            char msg[PUB_MSG_LEN - 1] = "\0";
            while(xQueueReceive(publish_queue, (void *)msg, 0) ==
                  pdTRUE){
                printf("got message to publish\r\n");
                mqtt_message_t message;
                message.payload = msg;
                message.payloadlen = PUB_MSG_LEN;
                message.dup = 0;
                message.qos = MQTT_QOS0;
                message.retained = 0;
                //ret = mqtt_publish(&client, "beat", &message);
                ret = mqtt_publish(&client, mqtt_client_id, &message);
                if (ret != MQTT_SUCCESS ){
                    printf("error while publishing message: %d\n", ret );
                    break;
                }
            }

            ret = mqtt_yield(&client, 1000);
            if (ret == MQTT_DISCONNECTED)
                break;
        }
    
        printf("Connection dropped, request restart\n\r");
        mqtt_network_disconnect(&network);
        
        //taskYIELD();
    }
}

static void topic_received(mqtt_message_data_t *md)
{
    int i;
    mqtt_message_t *message = md->message;
    printf("Received: ");
    for( i = 0; i < md->topic->lenstring.len; ++i)
        printf("%c", md->topic->lenstring.data[ i ]);

    printf(" = ");
    for( i = 0; i < (int)message->payloadlen; ++i)
        printf("%c", ((char *)(message->payload))[i]);

    printf("\r\n");

    if ((int)message->payloadlen == 8) {
        if (((char *)(message->payload))[0] == '0' && ((char *)(message->payload))[1] == '1') //Setting LED
        {
            char r[3] = "00\0";
            char g[3] = "00\0";
            char b[3] = "00\0";
            r[0] = ((char *)(message->payload))[2]; r[1] = ((char *)(message->payload))[3]; 
            g[0] = ((char *)(message->payload))[4]; g[1] = ((char *)(message->payload))[5]; 
            b[0] = ((char *)(message->payload))[6]; b[1] = ((char *)(message->payload))[7]; 
            uint8_t r_val = (uint8_t) strtol(r, NULL, 16);
            uint8_t g_val = (uint8_t) strtol(g, NULL, 16);
            uint8_t b_val = (uint8_t) strtol(b, NULL, 16);
            printf("r : %d g: %d b: %d \r\n", r_val, g_val, b_val);
            set_led(r_val, g_val, b_val);
            set_key_led(r_val, g_val, b_val);
        }
        if (((char *)(message->payload))[0] == '0' && ((char *)(message->payload))[1] == '2') //Setting Hydro
        {
            printf("Setting Hydro\r\n");
            if (((char *)(message->payload))[2] == '1')
            {
                printf("Hydro ON\r\n");
                hydro_mode = 0; 
                hydro_timer = 5 * 60;
            } else {
                printf("Hydro OFF\r\n");  
                hydro_timer = 0; 
            }
        }
    } else if ((int)message->payloadlen == 3) {
        printf("Setting Cleaning\r\n");
        if (((char *)(message->payload))[0] == '0' && ((char *)(message->payload))[1] == '3' ) //Setting Clean
        {
            printf("Setting Cleaning\r\n");
            if (((char *)(message->payload))[2] == '1')
            {
                printf("Cleaning ON\r\n");
                hydro_mode = 1; 
                hydro_timer = 5 * 60;
            } else {
                printf("Cleaning OFF\r\n");  
                hydro_timer = 0; 
            }
        }
    }
    else if ((int)message->payloadlen == 5) {
        if (((char *)(message->payload))[0] == 's' && ((char *)(message->payload))[1] == 'l' &&
            ((char *)(message->payload))[2] == 'e' && ((char *)(message->payload))[3] == 'e' &&
            ((char *)(message->payload))[4] == 'p')
        { 
            sdk_system_deep_sleep(10*1000*1000);
        }
    }
    else if ((int)message->payloadlen == 6) {
        close_led();
    }
}

static void wifi_task(void *pvParameters)
{
    uint8_t status  = 0;
    uint8_t retries = 30;
    while(1)
    {
        //const char* ssid_ = "EmilWin"; 
        //const char* password_ = "Laikwoktai";
        const char* ssid_;
        const char* password_;
        read_wifi_config(0, &ssid_, &password_);
        struct sdk_station_config config; 
        memcpy(&config.ssid, ssid_, strlen((const char *)ssid_) + 1);
        memcpy(&config.password, password_, strlen((const char *)password_) + 1);
        
        printf("WiFi: connecting to WiFi SSID:%s PW:%s\n\r", config.ssid, config.password);
        sdk_wifi_set_opmode(STATION_MODE);
        //sdk_wifi_station_set_auto_connect(true);
        sdk_wifi_station_set_config(&config);
        sdk_wifi_station_connect();

        while ((status != STATION_GOT_IP) && (retries)){
            status = sdk_wifi_station_get_connect_status();
            if( status == STATION_WRONG_PASSWORD ){
                printf("WiFi: wrong password\n\r");
                break;
            } else if( status == STATION_NO_AP_FOUND ) {
                printf("WiFi: AP not found\n\r");
                break;
            } else if( status == STATION_CONNECT_FAIL ) {
                printf("WiFi: connection failed\r\n");
                break;
            }
            vTaskDelay( 1000 / portTICK_PERIOD_MS );
            --retries;
        }

        // if (status == STATION_GOT_IP) {
        //     printf("WiFi: Connected\n\r");
        //     sdk_wifi_set_sleep_type(WIFI_SLEEP_MODEM);
        //     printf("MODEM Mode\n\r");
        //     xSemaphoreGive( wifi_alive );
        //     //taskYIELD();
        // }

        while ((status = sdk_wifi_station_get_connect_status()) == STATION_GOT_IP) {
            //printf("WiFi: Connected\n\r");
            xSemaphoreGive( wifi_alive );
            //taskYIELD()
            vTaskDelay( 2000 / portTICK_PERIOD_MS );
        }
        printf("WiFi: disconnected\n\r");
        sdk_wifi_station_disconnect();
        vTaskDelay( 1000 / portTICK_PERIOD_MS );
        retries = 30;
    } 
}

static void ap_task(void *pvParameters)
{
    struct ip_info ap_ip;
    bool isWifiSet = false;
    //while(1) {
        //xSemaphoreGive( wifi_alive );
        printf("Setting AP mode....\r\n");

        sdk_wifi_station_set_auto_connect(false);
        sdk_wifi_set_opmode(STATIONAP_MODE);
        IP4_ADDR(&ap_ip.ip, 172, 16, 0, 1);
        IP4_ADDR(&ap_ip.gw, 0, 0, 0, 0);
        IP4_ADDR(&ap_ip.netmask, 255, 255, 0, 0);
        sdk_wifi_set_ip_info(1, &ap_ip);

        struct sdk_softap_config ap_config = {
            .ssid = AP_SSID,
            .ssid_hidden = 0,
            .channel = 7,
            .ssid_len = strlen(AP_SSID),
            .authmode = AUTH_WPA_WPA2_PSK,
            .password = AP_PSK,
            .max_connection = 1,
            .beacon_interval = 100,
        };
        sdk_wifi_station_set_auto_connect(false);
        sdk_wifi_softap_set_config(&ap_config);

        ip_addr_t first_client_ip;
        IP4_ADDR(&first_client_ip, 172, 16, 0, 2);
        dhcpserver_start(&first_client_ip, 4);
        dhcpserver_set_dns(&ap_ip.ip);
        dhcpserver_set_router(&ap_ip.ip);
        printf("Setting AP mode finished....\r\n");
        struct netconn *client = NULL;
        struct netconn *nc = netconn_new(NETCONN_TCP);
        if (nc == NULL) {
            printf("Failed to allocate socket\n");
            vTaskDelete(NULL);
        }
        netconn_bind(nc, IP_ADDR_ANY, 80);
        printf("http task binded\r\n");
        netconn_listen(nc);
        printf("http task listening\r\n");
        char buf[1024];
        while (1) {
            err_t err = netconn_accept(nc, &client);
            if (err == ERR_OK) {
                struct netbuf *nb;
                if ((err = netconn_recv(client, &nb)) == ERR_OK) {
                    void *data;
                    u16_t len;
                    netbuf_data(nb, &data, &len);
                    printf("Received data:\n%.*s\n", len, (char*) data);
                    ap_count = 0; //reset the ap timer counter
                    int ret = parse_http_header(data);
                    if (ret == 0)
                    {
                        //Test the wifi
                        printf("sending response....\r\n");
                        snprintf(buf, sizeof(buf),
                                "HTTP/1.1 200 OK\r\n"
                                "Content-type: text/html\r\n\r\n"
                                );
                        netconn_write(client, buf, strlen(buf), NETCONN_COPY);
                        sprintf(buf, "{\"device_id\": \"%s\", \"status\": \"Connecting\"}\n", mqtt_client_id);
                        printf("%s\r\n", buf);
                        netconn_write(client, buf, strlen(buf), NETCONN_COPY);
                        netbuf_delete(nb);
                        printf("Closing connection\n");
                        netconn_close(client);
                        netconn_delete(client);

                        const char* ssid_; 
                        const char* password_;
                        uint8_t status  = 0;
                        uint8_t retries = 20;
                        read_wifi_config(0, &ssid_, &password_);
                        struct sdk_station_config config; 
                        memcpy(&config.ssid, ssid_, strlen((const char *)ssid_) + 1);
                        memcpy(&config.password, password_, strlen((const char *)password_) + 1);
                        printf("AP Mode WiFi: connecting to WiFi SSID:%s PW:%s\n\r", config.ssid, config.password);
                        sdk_wifi_station_set_config(&config);
                        sdk_wifi_station_connect();

                        while ((status != STATION_GOT_IP) && (retries > 0)){
                             printf("Checking wifi status\r\n");
                             status = sdk_wifi_station_get_connect_status();
                             printf("%s: status = %d\n\r", __func__, status );
                             if( status == STATION_WRONG_PASSWORD ){
                                printf("WiFi: wrong password\n\r");
                                break;
                             } else if( status == STATION_NO_AP_FOUND ) {
                                printf("WiFi: AP not found\n\r");
                                break;
                             } else if( status == STATION_CONNECT_FAIL ) {
                                printf("WiFi: connection failed\r\n");
                                break;
                             }
                             vTaskDelay( 1000 / portTICK_PERIOD_MS );
                             --retries;
                        }

                        if (status == STATION_GOT_IP) {
                            isWifiSet = true;
                            set_led(0, 50, 0);
                        } else { //Connection Fail
                            sdk_wifi_station_disconnect();
                            set_led(50, 0, 0);
                        }
                        //End
                    } else if (ret == 1)
                    {
                        snprintf(buf, sizeof(buf),
                                "HTTP/1.1 200 OK\r\n"
                                "Content-type: text/html\r\n\r\n"
                                );
                        netconn_write(client, buf, strlen(buf), NETCONN_COPY);
                        if (!cgiWifiAps.scanInProgress) {
                        //Fill in json code for an access point
                            cgiWifiAps.scanInProgress = 1;
                            sdk_wifi_station_scan(NULL, (sdk_scan_done_cb_t)&wifiScanDoneCb);
                            for ( int k = 0; k < 10; k++)
                            {
                                if (!cgiWifiAps.scanInProgress)
                                {
                                    for (int i = 0; i < cgiWifiAps.noAps; i++) {
                                        sprintf(buf, "{\"essid\": \"%s\", \"bssid\": \"" MACSTR "\", \"rssi\": \"%d\", \"enc\": \"%d\", \"channel\": \"%d\"}%s\n",
                                            cgiWifiAps.apData[i]->ssid, MAC2STR(cgiWifiAps.apData[i]->bssid), cgiWifiAps.apData[i]->rssi,
                                            cgiWifiAps.apData[i]->enc, cgiWifiAps.apData[i]->channel, ((i+1)==cgiWifiAps.noAps)?"":",");
                                        printf("%s\r\n", buf);
                                        netconn_write(client, buf, strlen(buf), NETCONN_COPY);
                                    }
                                    k = 10;  //break the for loop
                                } else {
                                    printf("Scanning in progress...\r\n");
                                    vTaskDelay( 1000 / portTICK_PERIOD_MS );
                                }
                            }
                        }
                        netbuf_delete(nb);
                        printf("Closing connection\n");
                        netconn_close(client);
                        netconn_delete(client);
                    } else if (ret == 2)
                    {
                        printf("Going to reboot..");
                        isWifiSet = true;
                        snprintf(buf, sizeof(buf),
                                "HTTP/1.1 200 OK\r\n"
                                "Content-type: text/html\r\n\r\n"
                                );
                        netconn_write(client, buf, strlen(buf), NETCONN_COPY);
                    }
                }
            }
            printf("Closing connection\n");
            if (isWifiSet) {
                set_device_state();
                printf("Wifi is configured by user\n\r");
                break;
                // vTaskDelay( 10000 / portTICK_PERIOD_MS );
                // send_cmd = 2;
            }
        }
    //}
    vTaskDelay( 1000 / portTICK_PERIOD_MS );
}

void wifiScanDoneCb(void *arg, sdk_scan_status_t status) {
    int n;
    struct sdk_bss_info *bss_link = (struct sdk_bss_info *)arg;
    printf("wifiScanDoneCb %d\n", status);
    if (status!=SCAN_OK) {
        cgiWifiAps.scanInProgress=0;
        return;
    }

    //Clear prev ap data if needed.
    if (cgiWifiAps.apData!=NULL) {
        for (n=0; n<cgiWifiAps.noAps; n++) free(cgiWifiAps.apData[n]);
        free(cgiWifiAps.apData);
    }

    //Count amount of access points found.
    n=0;
    while (bss_link != NULL) {
        bss_link = bss_link->next.stqe_next;
        n++;
    }
    //Allocate memory for access point data
    cgiWifiAps.apData=(ApData **)malloc(sizeof(ApData *)*n);
    if (cgiWifiAps.apData==NULL) {
        printf("Out of memory allocating apData\n");
        return;
    }
    cgiWifiAps.noAps=n;
    printf("Scan done: found %d APs\n", n);

    //Copy access point data to the static struct
    n=0;
    bss_link = (struct sdk_bss_info *)arg;
    while (bss_link != NULL) {
        if (n>=cgiWifiAps.noAps) {
            //This means the bss_link changed under our nose. Shouldn't happen!
            //Break because otherwise we will write in unallocated memory.
            printf("Huh? I have more than the allocated %d aps!\n", cgiWifiAps.noAps);
            break;
        }
        //Save the ap data.
        cgiWifiAps.apData[n]=(ApData *)malloc(sizeof(ApData));
        if (cgiWifiAps.apData[n]==NULL) {
            printf("Can't allocate mem for ap buff.\n");
            cgiWifiAps.scanInProgress=0;
            return;
        }
        cgiWifiAps.apData[n]->rssi=bss_link->rssi;
        cgiWifiAps.apData[n]->channel=bss_link->channel;
        cgiWifiAps.apData[n]->enc=bss_link->authmode;
        strncpy(cgiWifiAps.apData[n]->ssid, (char*)bss_link->ssid, 32);
        strncpy(cgiWifiAps.apData[n]->bssid, (char*)bss_link->bssid, 6);

        bss_link = bss_link->next.stqe_next;
        n++;
    }
    //We're done.
    cgiWifiAps.scanInProgress=0;
}

void user_init(void)
{
    memset(mqtt_client_id, 0, sizeof(mqtt_client_id));
    strcpy(mqtt_client_id, "WOPIN-");
    strcat(mqtt_client_id, get_my_id());

    memset(mqtt_client_id_sub, 0, sizeof(mqtt_client_id_sub));
    strcpy(mqtt_client_id_sub, "WOPIN-");
    strcat(mqtt_client_id_sub, get_my_id());
    strcat(mqtt_client_id_sub, "-D");

    uart_set_baud(0, 115200);
    printf("SDK version:%s\n", sdk_system_get_sdk_version());
    printf("Device id:%s\n", mqtt_client_id);
    printf("Device id sub:%s\n", mqtt_client_id_sub);
    init_led();
    gpio_init();
    int state = read_device_state();
    reset_device_state();
    //xTaskCreate(&buttonIntTask, "buttonIntTask", 256, &tsqueue, 1, NULL);
    if (state == 0)
    {
        vSemaphoreCreateBinary(wifi_alive);
        publish_queue = xQueueCreate(3, PUB_MSG_LEN);
        printf("Normal working mode!!!\r\n");
        //xTaskCreate(&signal_task, "signal_task", 256, NULL, 1, NULL);
        printf("Create Wifi Task Finished\r\n");
        xTaskCreate(&beat_task, "beat_task", 256, NULL, 1, NULL);
        xTaskCreate(&led_task, "led_task", 256, NULL, 1, NULL);
        xTaskCreate(&key_led_task, "key_led_task", 256, NULL, 1, NULL);
        xTaskCreate(&hydro_task, "hydro_task", 256, NULL, 1, NULL);
        xTaskCreate(&send_to_pmc_task, "send_to_pmc_task", 256, NULL, 1, NULL);
        vTaskDelay( 5000 / portTICK_PERIOD_MS );
        xTaskCreate(&wifi_task, "wifi_task", 1024, NULL, 1, NULL);
        xTaskCreate(&mqtt_task, "mqtt_task", 1024, NULL, 1, NULL);
    } else if (state == 1) {
        printf("Wifi AP mode...\r\n");
        set_key_led(50, 50, 0);     
        set_led(50, 50, 0);
        //xTaskCreate(&signal_task, "signal_task", 256, NULL, 1, NULL);
        xTaskCreate(&ap_task, "ap_task", 1024, NULL, 1, NULL);
        xTaskCreate(&ap_count_task, "ap_count_task", 1024, NULL, 1, NULL);
        reset_device_state();
    }
}


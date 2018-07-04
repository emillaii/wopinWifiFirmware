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

#define MQTT_HOST ("192.168.1.110")
#define MQTT_PORT 1883

#define MQTT_USER NULL
#define MQTT_PASS NULL

#define PUB_MSG_LEN 16


#define AP_SSID "H2PoPo"
#define AP_PSK "12345678"

static void httpd_task(void *pvParameters);
static void wifi_task(void *pvParameters);

static void beat_task(void *pvParameters);
static void mqtt_task(void *pvParameters);
static void buttonIntTask(void *pvParameters);

SemaphoreHandle_t wifi_alive;
QueueHandle_t publish_queue;
bool isWifiConnected = false;

const int gpio = 14;   /* gpio 0 usually has "PROGRAM" button attached */
const int active = 0; /* active == 0 for active low */
const gpio_inttype_t int_type = GPIO_INTTYPE_EDGE_NEG;

static QueueHandle_t tsqueue;

void gpio_intr_handler(uint8_t gpio_num);

void gpio_init(void) 
{
    gpio_enable(gpio, GPIO_INPUT);
    tsqueue = xQueueCreate(2, sizeof(uint32_t));
}

void gpio_intr_handler(uint8_t gpio_num)
{
    uint32_t now = xTaskGetTickCountFromISR();
    xQueueSendToBackFromISR(tsqueue, &now, NULL);
}

void buttonIntTask(void *pvParameters)
{
    printf("Waiting for button press interrupt on gpio %d.......\r\n", gpio);
    QueueHandle_t *tsqueue = (QueueHandle_t *)pvParameters;
    printf("1\r\n");
    gpio_set_interrupt(gpio, int_type, gpio_intr_handler);

    printf("2\r\n");
    uint32_t last = 0;
    while (1) {
        uint32_t button_ts;
        xQueueReceive(*tsqueue, &button_ts, portMAX_DELAY);
        button_ts *= portTICK_PERIOD_MS;
        if(last < button_ts-200) {
            printf("Button interrupt fired at %dms\r\n", button_ts);
            uint32_t delay = button_ts - last;
            printf("Button pressed for %dms\r\n", delay);
            last = button_ts;
        }
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
            printf("Sleep....Polled for button press at %d\r\n", count);
        }
        vTaskDelay(200 / portTICK_PERIOD_MS);
    }
}

static void beat_task(void *pvParameters)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    char msg[PUB_MSG_LEN];
    int count = 0;

    while (1) {
        vTaskDelayUntil(&xLastWakeTime, 10000 / portTICK_PERIOD_MS);
        printf("beat\r\n");
        snprintf(msg, PUB_MSG_LEN, "Beat %d\r\n", count++);
        if (xQueueSend(publish_queue, (void *)msg, 0) == pdFALSE) {
            printf("Publish queue overflow.\r\n");
        }
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
            printf("Setting LED command ");
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
        }
    }
    if ((int)message->payloadlen == 5) {
        sdk_system_deep_sleep(10*1000*1000);
    }
}

static const char *  get_my_id(void)
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

static void  mqtt_task(void *pvParameters)
{
    int ret         = 0;
    struct mqtt_network network;
    mqtt_client_t client   = mqtt_client_default;
    char mqtt_client_id[20];
    uint8_t mqtt_buf[100];
    uint8_t mqtt_readbuf[100];
    mqtt_packet_connect_data_t data = mqtt_packet_connect_data_initializer;

    mqtt_network_new( &network );
    memset(mqtt_client_id, 0, sizeof(mqtt_client_id));
    strcpy(mqtt_client_id, "ESP-");
    strcat(mqtt_client_id, get_my_id());

    while(1) {
        xSemaphoreTake(wifi_alive, portMAX_DELAY);
        printf("%s: started\n\r", __func__);
        printf("%s: (Re)connecting to MQTT server %s ... ",__func__,
               MQTT_HOST);
        ret = mqtt_network_connect(&network, MQTT_HOST, MQTT_PORT);
        if( ret ){
            printf("error: %d\n\r", ret);
            taskYIELD();
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
            taskYIELD();
            continue;
        }
        printf("done\r\n");
        mqtt_subscribe(&client, "/esptopic", MQTT_QOS1, topic_received);
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
                message.qos = MQTT_QOS1;
                message.retained = 0;
                ret = mqtt_publish(&client, "/beat", &message);
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
        taskYIELD();
    }
}

static void wifi_task(void *pvParameters)
{
    uint8_t status  = 0;
    uint8_t retries = 30;

    while(1)
    {
        const char* ssid_; 
        const char* password_;
        read_wifi_config(0, &ssid_, &password_);
        struct sdk_station_config config; 
        memcpy(&config.ssid, ssid_, strlen((const char *)config.ssid));
        memcpy(&config.password, password_, strlen((const char *)config.password));
        
        printf("WiFi: connecting to WiFi SSID: %s PW: %s\n\r", config.ssid, config.password);
        sdk_wifi_set_opmode(STATION_MODE);
        sdk_wifi_station_set_auto_connect(true);
        /*struct ip_info ap_ip;
        IP4_ADDR(&ap_ip.ip, 172, 16, 0, 1);
        IP4_ADDR(&ap_ip.gw, 0, 0, 0, 0);
        IP4_ADDR(&ap_ip.netmask, 255, 255, 0, 0);
        sdk_wifi_set_ip_info(1, &ap_ip);
        struct sdk_softap_config ap_config = { .ssid = AP_SSID, .ssid_hidden = 0, .channel = 3, .ssid_len = strlen(AP_SSID), .authmode =
                AUTH_WPA_WPA2_PSK, .password = AP_PSK, .max_connection = 3, .beacon_interval = 100, };
        sdk_wifi_softap_set_config(&ap_config);*/
        sdk_wifi_station_set_config(&config);
        sdk_wifi_station_connect();

        while ((status != STATION_GOT_IP) && (retries)){
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
            printf("WiFi: Connected\n\r");
            //sdk_wifi_set_sleep_type(WIFI_SLEEP_LIGHT);
            sdk_wifi_set_sleep_type(WIFI_SLEEP_LIGHT);
            xSemaphoreGive( wifi_alive );
            taskYIELD();
        }

        while ((status = sdk_wifi_station_get_connect_status()) == STATION_GOT_IP) {
            xSemaphoreGive( wifi_alive );
            taskYIELD();
        }
        printf("WiFi: disconnected\n\r");
        sdk_wifi_station_disconnect();
        vTaskDelay( 1000 / portTICK_PERIOD_MS );
        retries = 30;
    }
}

static void httpd_task(void *pvParameters)
{
    ip_addr_t first_client_ip;
    IP4_ADDR(&first_client_ip, 172, 16, 0, 2);
    dhcpserver_start(&first_client_ip, 4);

    struct netconn *client = NULL;
    struct netconn *nc = netconn_new(NETCONN_TCP);
    if (nc == NULL) {
        printf("Failed to allocate socket\n");
        vTaskDelete(NULL);
    }
    netconn_bind(nc, IP_ADDR_ANY, 80);
    netconn_listen(nc);
    char buf[512];
    while (1) {
        err_t err = netconn_accept(nc, &client);
        if (err == ERR_OK) {
            struct netbuf *nb;
            if ((err = netconn_recv(client, &nb)) == ERR_OK) {
                void *data;
                u16_t len;
                netbuf_data(nb, &data, &len);
                printf("Received data:\n%.*s\n", len, (char*) data);

                parse_http_header(data);

                snprintf(buf, sizeof(buf),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-type: text/html\r\n\r\n"
                        "Test");
                netconn_write(client, buf, strlen(buf), NETCONN_COPY);
            }
            netbuf_delete(nb);
        }
        printf("Closing connection\n");
        netconn_close(client);
        netconn_delete(client);
    }
}

void user_init(void)
{
    uart_set_baud(0, 115200);
    printf("SDK version:%s\n", sdk_system_get_sdk_version());
    vSemaphoreCreateBinary(wifi_alive);
    publish_queue = xQueueCreate(3, PUB_MSG_LEN);
    init_led();
    gpio_init();
    xTaskCreate(&wifi_task, "wifi_task", 256, NULL, 2, NULL);
    xTaskCreate(&beat_task, "beat_task", 256, NULL, 3, NULL);
    xTaskCreate(&mqtt_task, "mqtt_task", 1024, NULL, 4, NULL);
    //xTaskCreate(&buttonIntTask, "buttonIntTask", 256, &tsqueue, 2, NULL);
    xTaskCreate(&buttonPollTask, "buttonPollTask", 256, NULL, 2, NULL);
    //xTaskCreate(&blinkTest, "blink_task", 1024, NULL, 5, NULL);
}


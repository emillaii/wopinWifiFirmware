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

static void wifi_task(void *pvParameters);

static void beat_task(void *pvParameters);
static void ap_task(void *pvParameters);
static void mqtt_task(void *pvParameters);
static void signal_task(void *pvParameters);
static void topic_received(mqtt_message_data_t *md);

void wifiScanDoneCb(void *arg, sdk_scan_status_t status);

SemaphoreHandle_t wifi_alive;
QueueHandle_t publish_queue;

char mqtt_client_id[20];  // this is device id

const int gpio = 14;   /* gpio 0 usually has "PROGRAM" button attached */
const int active = 0; /* active == 0 for active low */
const gpio_inttype_t int_type = GPIO_INTTYPE_EDGE_NEG;

static const char * const auth_modes [] = {
    [AUTH_OPEN]         = "Open",
    [AUTH_WEP]          = "WEP",
    [AUTH_WPA_PSK]      = "WPA/PSK",
    [AUTH_WPA2_PSK]     = "WPA2/PSK",
    [AUTH_WPA_WPA2_PSK] = "WPA/WPA2/PSK"
};

static QueueHandle_t tsqueue;

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
    tsqueue = xQueueCreate(2, sizeof(uint32_t));
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
            //printf("Sleep....Polled for button press at %d\r\n", count);
            printf("Reset to AP mode. Restarting system...\n");
            set_device_state();
            sdk_system_restart();
            break;
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

static void signal_task(void *pvParameters)
{
    while (true)
    {
        bool state = gpio_read(SCL_PIN);
        if (!state)
        {
            gpio_enable(SDA_PIN, GPIO_INPUT);
            taskENTER_CRITICAL();
            sdk_os_delay_us(100);
            uint8_t buf[8];
            for (uint8_t i = 0; i < 8; i++) {
                sdk_os_delay_us(50);
                uint8_t r = gpio_read(SDA_PIN);
                buf[i] = r;
                sdk_os_delay_us(50);
            }
            taskEXIT_CRITICAL();
            printf("\r\n");
            printf("Data : %d%d%d%d %d%d%d%d \r\n", buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
            if (buf[0] == 1 && buf[1] == 1 && buf[2] == 0 && buf[3] == 0) // c : 1101
            {
                if (buf[4] == 0 && buf[5] == 0 && buf[6] == 0 && buf[7] == 1) // 0xc1
                {
                    printf("0xc1 command Received\r\n");
                } else if (buf[4] == 0 && buf[5] == 1 && buf[6] == 0 && buf[7] == 1) //0xc5
                {
                    printf("0xc5 command Received\r\n");
                } else if (buf[4] == 0 && buf[5] == 0 && buf[6] == 1 && buf[7] == 0) //0xc5
                {
                    printf("0xc2 command Received\r\n");
                } else if (buf[4] == 0 && buf[5] == 1 && buf[6] == 1 && buf[7] == 1) //0xc7
                {
                    printf("0xc7 command Received\r\n");
                } else if (buf[4] == 1 && buf[5] == 0 && buf[6] == 0 && buf[7] == 0) //0xc8
                {
                    printf("0xc8 command Received\r\n");
                } else if (buf[4] == 1 && buf[5] == 0 && buf[6] == 0 && buf[7] == 1) //0xc9
                {
                    printf("0xc9 command Received\r\n");
                } else if (buf[4] == 0 && buf[5] == 1 && buf[6] == 1 && buf[7] == 0) //0xc6
                {
                    printf("0xc6 command Received\r\n");
                } else if (buf[4] == 1 && buf[5] == 0 && buf[6] == 1 && buf[7] == 1) //0xcb
                {
                    printf("0xcb command Received\r\n");
                }
            }
            //gpio_enable(SDA_PIN, GPIO_OUT_OPEN_DRAIN);
        }
        sdk_os_delay_us(10);
    }
}

static void mqtt_task(void *pvParameters)
{
    int ret         = 0;
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
        mqtt_subscribe(&client, mqtt_client_id, MQTT_QOS0, topic_received);
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
                ret = mqtt_publish(&client, "beat", &message);
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
            } else {
                printf("Hydro OFF\r\n");   
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
        printf("wifi_task\r\n");
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
            sdk_wifi_set_sleep_type(WIFI_SLEEP_MODEM);
            printf("MODEM Mode\n\r");
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

static void ap_task(void *pvParameters)
{
    struct ip_info ap_ip;
    bool isWifiSet = false;
    //while(1) {
        xSemaphoreGive( wifi_alive );
        printf("Setting AP mode....\r\n");
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

                    int ret = parse_http_header(data);
                    if (ret == 0)
                    {
                        //Test the wifi 
                        const char* ssid_; 
                        const char* password_;
                        uint8_t status  = 0;
                        uint8_t retries = 20;
                        read_wifi_config(0, &ssid_, &password_);
                        struct sdk_station_config config; 
                        memcpy(&config.ssid, ssid_, strlen((const char *)ssid_) + 1);
                        memcpy(&config.password, password_, strlen((const char *)password_) + 1);
                        printf("WiFi: connecting to WiFi SSID:%s PW:%s\n\r", config.ssid, config.password);
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
                            isWifiSet = true;
                            set_led(0, 50, 0);
                            printf("WiFi: Connected..\n\r");
                            snprintf(buf, sizeof(buf),
                                "HTTP/1.1 200 OK\r\n"
                                "Content-type: text/html\r\n\r\n"
                                );
                            netconn_write(client, buf, strlen(buf), NETCONN_COPY);
                            sprintf(buf, "{\"device_id\": \"%s\", \"status\": \"Connected\"}\n", mqtt_client_id);
                            netconn_write(client, buf, strlen(buf), NETCONN_COPY);
                        } else {
                            set_led(50, 0, 0);
                            snprintf(buf, sizeof(buf),
                                "HTTP/1.1 200 OK\r\n"
                                "Content-type: text/html\r\n\r\n"
                                "{\"device_id\": \"%s\", \"status\": \"Fail\"}\r\n", mqtt_client_id);
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
                netbuf_delete(nb);
            }
            printf("Closing connection\n");
            netconn_close(client);
            netconn_delete(client);
            if (isWifiSet) {
                printf("Wifi is configured by user\n\r");
                vTaskDelay( 1000 / portTICK_PERIOD_MS );
                break;
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

    uart_set_baud(0, 115200);
    printf("SDK version:%s\n", sdk_system_get_sdk_version());
    printf("Device id:%s\n", mqtt_client_id);

    vSemaphoreCreateBinary(wifi_alive);
    publish_queue = xQueueCreate(3, PUB_MSG_LEN);
    init_led();
    gpio_init();
    int state = read_device_state();
    if (state == 0)
    {
        printf("Normal working mode!!!\r\n");
        xTaskCreate(&wifi_task, "wifi_task", 1024, NULL, 1, NULL);
        printf("Create Wifi Task Finished\r\n");
        xTaskCreate(&beat_task, "beat_task", 256, NULL, 1, NULL);
        xTaskCreate(&mqtt_task, "mqtt_task", 1024, NULL, 1, NULL);
        xTaskCreate(&signal_task, "signal_task", 256, NULL, 1, NULL);
        set_led(30, 0, 0);
        set_key_led(0, 30, 0);
    } else if (state == 1) {
        printf("Wifi AP mode...\r\n");
        set_led(0, 0, 30);
        xTaskCreate(&ap_task, "ap_task", 1024, NULL, 1, NULL);
        reset_device_state();
    }
    //xTaskCreate(&buttonPollTask, "buttonPollTask", 256, NULL, 1, NULL);
    /*while(1)
    {
        printf("hello....\r\n");
        vTaskDelay(200 / portTICK_PERIOD_MS);
    }*/
}


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
#include <softuart/softuart.h>

#include "http_client_ota.h"
//OTA Related

#define BINARY_PATH "/access_point_new.bin"
#define SHA256_PATH "/access_point_new.sha256"

char binary_filename[30];
char sha256_filename[30];
bool is_wifi_connected = false;

int modem_sleep_timer = 0;
int deep_sleep_timer = 0;
int hydro_timer = 0;
int hydro_mode = 0; // 0: normal mode 1: clean mode

#define OTA_SERVER "wifi.h2popo.com"
#define OTA_PORT "8084"
int version = 1;

static inline void ota_error_handling(OTA_err err) {
    printf("ota_error_handling:");

    switch(err) {
    case OTA_DNS_LOOKUP_FALLIED:
        printf("DNS lookup has fallied\n");
        break;
    case OTA_SOCKET_ALLOCATION_FALLIED:
        printf("Impossible allocate required socket\n");
        break;
    case OTA_SOCKET_CONNECTION_FALLIED:
        printf("Server unreachable, impossible connect\n");
        break;
    case OTA_SHA_DONT_MATCH:
        printf("Sha256 sum does not fit downloaded sha256\n");
        break;
    case OTA_REQUEST_SEND_FALLIED:
        printf("Impossible send HTTP request\n");
        break;
    case OTA_DOWLOAD_SIZE_NOT_MATCH:
        printf("Dowload size don't match with server declared size\n");
        break;
    case OTA_ONE_SLOT_ONLY:
        printf("rboot has only one slot configured, impossible switch it\n");
        break;
    case OTA_FAIL_SET_NEW_SLOT:
        printf("rboot cannot switch between rom\n");
        break;
    case OTA_IMAGE_VERIFY_FALLIED:
        printf("Dowloaded image binary checksum is fallied\n");
        break;
    case OTA_UPDATE_DONE:
        printf("Ota has completed upgrade process, all ready for system software reset\n");
        break;
    case OTA_HTTP_OK:
        printf("HTTP server has response 200, Ok\n");
        break;
    case OTA_HTTP_NOTFOUND:
        printf("HTTP server has response 404, file not found\n");
        break;
    }
}

bool ota_start = false;
static void ota_task(void *PvParameter)
{
    // Wait until we have joined AP and are assigned an IP *
    while (sdk_wifi_station_get_connect_status() != STATION_GOT_IP)
        vTaskDelay(1000 / portTICK_PERIOD_MS);

    while (1) {
        if (ota_start)
        {
            OTA_err err;
            // Remake this task until ota work
            hydro_timer = 300; //Need to reset the hydro time for downloading artifact.
            ota_info info = {
                .server      = OTA_SERVER,
                .port        = OTA_PORT,
                .binary_path = binary_filename,
                .sha256_path = sha256_filename,
            };

            printf("Updating firmware.....\r\n");
            err = ota_update(&info);
            ota_error_handling(err);

            if(err != OTA_UPDATE_DONE) {
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                continue;
            }
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            printf("Delay 1\n");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            printf("Delay 2\n");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            printf("Delay 3\n");
            printf("Reset\n");
            sdk_system_restart();
        } else {
            vTaskDelay(10000 / portTICK_PERIOD_MS);
        }
    }
}

static ota_info ota_info_ = {
    .server      = OTA_SERVER,
    .port        = OTA_PORT,
    .binary_path = BINARY_PATH,
    .sha256_path = SHA256_PATH,
};
// End of ota task

#define MQTT_HOST ("wifi.h2popo.com")
#define MQTT_PORT 8083

#define MQTT_USER ("wopin")
#define MQTT_PASS ("wopinH2popo")

#define PUB_MSG_LEN 22

#define AP_SSID "Wopin-H2PoPo"
#define AP_PSK "Dt-20181025"

#define TEST_SSID "WopinWifiTest"
#define TEST_SSID_PW "12345678"

//#define SCL_PIN (14)            //D5
//#define SDA_PIN (2)             //D4
#define RX_PIN 5                //D1--5 
#define TX_PIN 4                //D2--4
//#define HYDRO_PIN_A (1)
//#define HYDRO_PIN_B (3)

#define TURNON   0xc1
#define TURNOFF  0xb2
#define CLEANON  0xc3
#define CLEANOFF 0xb4
#define TURNONLED 0xba
#define TURNOFFLED 0xbc
#define CHGMODE  0xc5
#define PWMCOLOR 0xb6
#define ILEVEL   0xc7
#define VLEVEL   0xb8
#define STANDBY  0xc9
#define APMODE   0xcb


static void wifi_task(void *pvParameters);

static void beat_task(void *pvParameters);
static void ap_task(void *pvParameters);
static void ap_count_task(void *pvParameters);
static void mqtt_task(void *pvParameters);
static void topic_received(mqtt_message_data_t *md);
static void hydro_task(void *pvParameters);
static void soft_uart_task(void *pvParameters);

void wifiScanDoneCb(void *arg, sdk_scan_status_t status);

SemaphoreHandle_t wifi_alive;
QueueHandle_t publish_queue;
QueueHandle_t publish_queue_1;

char mqtt_client_id[30];  // this is device id
char mqtt_client_id_sub[30];  // this is device id

char send_to_pmc_data[20]={'\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n'};
char read_from_pmc_data[20]={'\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n'};
static bool send_status = 0;                //if send data to MCU, set true
static int ap_count = 0;
//uint16_t sendCnt = 0;
//uint8_t sendDataCnt = 0;
static uint8_t sysStatus = 0;           //0:standby, 1:hydro, 2:clean, 3:clean complete, 4:charge

const uint32_t wakeupTime = 30*60*1000*1000;

static const char * const auth_modes [] = {
    [AUTH_OPEN]         = "Open",
    [AUTH_WEP]          = "WEP",
    [AUTH_WPA_PSK]      = "WPA/PSK",
    [AUTH_WPA2_PSK]     = "WPA2/PSK",
    [AUTH_WPA_WPA2_PSK] = "WPA/WPA2/PSK"
};

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

static void hydro_task(void *pvParameters)
{
    while(1) {
        if (hydro_timer != 0) {
            hydro_timer--;
            printf("hydro...%d\r\n", hydro_timer);
            if ((hydro_timer == 0) && (sysStatus == 1)) { // If timer count down to 0 success, then send 0xc2 to pmc
                deep_sleep_timer = 2;
            } else if (hydro_timer == 10) { //If timer count down to 10 seconds left, then send "drink" event to server
                if (is_wifi_connected) // If wifi is connected, send drink event with previous count as well
                {
                    int accumlated_hydro_count = read_hydro_count();
                    printf("Send drink water event. Accumlated hydro count: %d\r\n", accumlated_hydro_count);
                    char msg[PUB_MSG_LEN] = {0};
                    snprintf(msg, PUB_MSG_LEN, "%s_%d", mqtt_client_id, accumlated_hydro_count);
                    if (xQueueSend(publish_queue_1, (void *)msg, 0) == pdFALSE) {
                        printf("drink water queue overflow.\r\n");
                    }
                    reset_hydro_count();
                } else { //increment the save water count
                    increment_hydro_count();
                }
            }
        } 
        if(hydro_timer == 0)
        {
            modem_sleep_timer++;
            if(modem_sleep_timer >= 1*60)
            {
                deep_sleep_timer++;
                modem_sleep_timer = 0;
            }
        }       
        if (hydro_timer == 0 && deep_sleep_timer >= 2) {  //If finish hydro go to deep sleep mode
            if((!send_status)&&(sysStatus<2))                 //if clean mode,don't turn off
            {
                send_to_pmc_data[0] = TURNOFF;
                send_to_pmc_data[1] = 10;
                send_status = 1;
            }
            else if(sysStatus >=2)                              //if clean mode or charge,don't turn off
                continue;
            deep_sleep_timer = 0;
            modem_sleep_timer = 0;
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            //set_device_deepsleep();
            //sdk_system_restart();
            //break;
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

static void soft_uart_task(void *pvParameters)
{
    uint8_t tempSUart;
    // setup software uart to 9600 8n1
    softuart_open(0, 9600, RX_PIN, TX_PIN);
    while (true)
    {
        if(send_status)
        {
            softuart_put(0,send_to_pmc_data[0]);
            sdk_os_delay_us(500);
            softuart_put(0,send_to_pmc_data[0]);
            if(send_to_pmc_data[0]==0xb6)
            {
                softuart_put(0,send_to_pmc_data[1]);
                sdk_os_delay_us(500);
                softuart_put(0,send_to_pmc_data[2]);
                sdk_os_delay_us(500);
                softuart_put(0,send_to_pmc_data[3]);
            }
            send_status = 0;
            printf("send :%c, 0x%02x\n",send_to_pmc_data[0],send_to_pmc_data[0]);
        }

        sdk_os_delay_us(500);

        if (!softuart_available(0))
            continue;

        char c = softuart_read(0);
        printf("input :%c, 0x%02x\n",c,c);        
        if (c == TURNON) {           // Hydro On
            if(sysStatus != 1)
            {
                hydro_mode = 0; 
                hydro_timer = 5 * 60;
                sysStatus = 1;
            }
        } else if (c == TURNOFF) {    // Hydro Off
            if(sysStatus==1)
            {
                hydro_timer = 0;
                deep_sleep_timer = 2;
            }            
            if(ap_count)
            {
                ap_count=0;
                //set_device_deepsleep();
                //sdk_system_restart();
                //break;
                //sdk_system_deep_sleep(wakeupTime);
            }
        } else if (c == CLEANOFF) {    // Clean Off
            hydro_timer = 0; 
            sysStatus = 3;
        } else if (c == ILEVEL) {    // Receive over current

        } else if (c == VLEVEL) {    // Receive over voltage

        } else if (c == CHGMODE) {    // Receive charging status
            hydro_timer = 0;       //
            sysStatus = 4;
        } else if (c == APMODE) {    // Go To Ap Mode
            set_device_state();
            sdk_system_restart();
            break;
        }            

    }
} 

static void beat_task(void *pvParameters)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    char msg[PUB_MSG_LEN] = {0};

    while (1) {
        vTaskDelayUntil(&xLastWakeTime, 5000 / portTICK_PERIOD_MS);
        uint32_t adc_read = sdk_system_adc_read();
        printf("adc_read: %d\r\n", adc_read);

        if (!send_status && (sysStatus==1)) { 
            send_to_pmc_data[0] = TURNON;
            send_to_pmc_data[1] = 10;           
            send_status = 1;
        }
        int power = (int)(adc_read - 600)*0.476;        //3.1v = 0%, 4.16v=100%
        if (power > 100) power = 100;
        if (power < 0) power = 1;
        //printf("sending status P:%d\r\n", power);
            /* Print date and time each 5 seconds */
        uint8_t status = sdk_wifi_station_get_connect_status();
        if (status == STATION_GOT_IP)
        {
            int mode = 0; 
            if (hydro_timer == 0 && hydro_mode == 0) {
                mode = 0;
            } else if (hydro_timer > 0 && hydro_mode == 0) {
                mode = 1;
            } else if (hydro_mode == 1) {
                mode = 2;
            }
            printf("sending status P:%d:H:%d:M:%d:V:%d\r\n", power, hydro_timer, mode, version);
            snprintf(msg, sizeof(msg), "P:%d:H:%d:M:%d:V:%d", power, hydro_timer, mode, version);
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


static void ap_count_task(void *pvParameters)
{
    while(1)
    {
        ap_count++;
        if (ap_count >= 4 * 60) {
            printf("AP Mode Timeout\r\n");
            send_to_pmc_data[0] = TURNOFF;
            send_to_pmc_data[1] = 10;          
            send_status = 1;
        }
        vTaskDelay( 1000 / portTICK_PERIOD_MS );
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
        printf("Send MQTT connect ... \r\n");
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
        xQueueReset(publish_queue_1);

        while(1){

            char msg[PUB_MSG_LEN - 1] = "\0";
            while(xQueueReceive(publish_queue, (void *)msg, 0) ==
                  pdTRUE){
                //printf("got message to publish\r\n");
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

            while(xQueueReceive(publish_queue_1, (void *)msg, 0) ==
                  pdTRUE){
                //printf("got message to publish\r\n");
                mqtt_message_t message;
                message.payload = msg;
                message.payloadlen = PUB_MSG_LEN;
                message.dup = 0;
                message.qos = MQTT_QOS0;
                message.retained = 0;
                //ret = mqtt_publish(&client, "beat", &message);
                ret = mqtt_publish(&client, "drink", &message);
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
            if(r_val == 10)
                r_val++;
            if(g_val == 10)
                g_val++;
            if(b_val == 10)
                b_val++;
            printf("r : %d g: %d b: %d \r\n", r_val, g_val, b_val);
            if(!send_status)  { 
                send_to_pmc_data[0] = PWMCOLOR;
                send_to_pmc_data[1] = r_val;
                send_to_pmc_data[2] = g_val;
                send_to_pmc_data[3] = b_val;
                send_to_pmc_data[4] = 10;                           //end byte
//                sendDataCnt = 4;
//                sendCnt = 0;
                send_status = 1;
            }
            //set_led(r_val, g_val, b_val);
            //set_key_led(r_val, g_val, b_val);
        }
        if (((char *)(message->payload))[0] == '0' && ((char *)(message->payload))[1] == '2') //Setting Hydro
        {
            printf("Setting Hydro\r\n");
            if (((char *)(message->payload))[2] == '1')
            {
                char val[4] = "000\0";
                val[0] = ((char *)(message->payload))[5]; 
                val[1] = ((char *)(message->payload))[6]; 
                val[2] = ((char *)(message->payload))[7]; 
                uint16_t intVal = 5*60;
                sscanf(val, "%x", &intVal);
                printf("Hydro ON input time: %d\r\n", intVal);
                hydro_mode = 0; 
                hydro_timer = intVal;
                sysStatus = 1;
                if (!send_status) { 
                    send_to_pmc_data[0] = TURNON;
                    send_to_pmc_data[1] = 10;                
                    send_status = 1;
                }
            } else {
                printf("Hydro OFF\r\n");  
                hydro_timer = 0;
                sysStatus = 0;
                if (!send_status) { 
                    send_to_pmc_data[0] = STANDBY;
                    send_to_pmc_data[1] = 10;
//                    sendDataCnt = 1;                    
//                    sendCnt = 0;
                    send_status = 1;
                }
            }
        }
    } else if ((int)message->payloadlen == 3) {
        if (((char *)(message->payload))[0] == '0' && ((char *)(message->payload))[1] == '3' ) //Setting Clean
        {
            if (((char *)(message->payload))[2] == '1')
            {
                printf("Cleaning ON\r\n");
                hydro_mode = 1; 
                hydro_timer = 5 * 60;
                sysStatus = 2;
                if (!send_status) { 
                    send_to_pmc_data[0] = CLEANON;
                    send_to_pmc_data[1] = 10;
//                    sendDataCnt = 1;
//                    sendCnt = 0;
                    send_status = 1;
                }
            } else {
                printf("Cleaning OFF\r\n");  
                hydro_timer = 0; 
                sysStatus = 3;
                if (!send_status) { 
                    send_to_pmc_data[0] = CLEANOFF;
                    send_to_pmc_data[1] = 10;
//                    sendDataCnt = 1;                    
//                    sendCnt = 0;
                    send_status = 1;
                }
            }
        } else if (((char *)(message->payload))[0] == '0' && ((char *)(message->payload))[1] == '4' ) //Setting Clean
        {
            if (((char *)(message->payload))[2] == '0')
            {
                printf("LED OFF\r\n");
                if (!send_status) { 
                    send_to_pmc_data[0] = TURNOFFLED;
                    send_to_pmc_data[1] = 10;
                    send_status = 1;
                }
            } else {
                printf("LED ON\r\n");
                send_to_pmc_data[0] = TURNONLED;
                send_to_pmc_data[1] = 10;
                send_status = 1;
            }
        }
    }
    else if ((int)message->payloadlen == 5) {
        if (((char *)(message->payload))[0] == 's' && ((char *)(message->payload))[1] == 'l' &&
            ((char *)(message->payload))[2] == 'e' && ((char *)(message->payload))[3] == 'e' &&
            ((char *)(message->payload))[4] == 'p')
        { 
            set_device_deepsleep();
            sdk_system_restart();
        }
    }
    else if ((int)message->payloadlen == 6) {
        if (((char *)(message->payload))[0] == 'o' && ((char *)(message->payload))[1] == 't' &&
                   ((char *)(message->payload))[2] == 'a')
        {
            printf("Receive OTA request\r\n");
            char v[3] = "000";
            v[0] = ((char *)(message->payload))[3];
            v[1] = ((char *)(message->payload))[4];
            v[2] = ((char *)(message->payload))[5];
            memset(binary_filename, 0, sizeof(binary_filename));
            memset(sha256_filename, 0, sizeof(sha256_filename));
            strcpy(binary_filename, "/wopin_");
            strcat(binary_filename, v);
            strcat(sha256_filename, binary_filename);
            strcat(binary_filename, ".bin");
            strcat(sha256_filename, ".sha");
            printf("binary_filename : %s sha_filename : %s\r\n", binary_filename, sha256_filename);
            ota_start = true;
        }
    }
}

static void wifi_task(void *pvParameters)
{
    uint8_t status  = 0;
    uint8_t retries = 5;
    uint8_t test_count = 0;
    while(1)
    {
        const char* ssid_ = "";
        const char* password_ = "";
        if (test_count == 1) {  //Use for test
            ssid_ = TEST_SSID;
            password_ = TEST_SSID_PW;
        } else {
            read_wifi_config(0, &ssid_, &password_);
        }
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

        while ((status = sdk_wifi_station_get_connect_status()) == STATION_GOT_IP) {
            is_wifi_connected = true;
            xSemaphoreGive( wifi_alive );
            vTaskDelay( 2000 / portTICK_PERIOD_MS );
        }
        is_wifi_connected = false;
        printf("WiFi: disconnected\n\r");
        sdk_wifi_station_disconnect();
        vTaskDelay( 1000 / portTICK_PERIOD_MS );
        retries = 30;
        test_count++;
    } 
}


static void ap_task(void *pvParameters)
{
    struct ip_info ap_ip;
    bool isWifiSet = false;
    vTaskDelay( 5000 / portTICK_PERIOD_MS );
    //while(1) {
        //xSemaphoreGive( wifi_alive );

        sdk_wifi_station_start();
        sdk_wifi_softap_start();
        sdk_wifi_station_set_auto_connect(false);
        sdk_wifi_set_opmode(STATIONAP_MODE);
        IP4_ADDR(&ap_ip.ip, 172, 16, 0, 1);
        IP4_ADDR(&ap_ip.gw, 0, 0, 0, 0);
        IP4_ADDR(&ap_ip.netmask, 255, 255, 0, 0);
        sdk_wifi_set_ip_info(1, &ap_ip);

        struct sdk_softap_config ap_config = {
            .ssid_hidden = 0,
            .channel = 6,
            .ssid_len = strlen(AP_SSID),
            .authmode = AUTH_WPA_WPA2_PSK,
            .ssid = AP_SSID,
            .password = AP_PSK,
            .max_connection = 3,
            .beacon_interval = 100,
        };

        //strncpy((char *)ap_config.ssid, mqtt_client_id, 32);

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
                        } else { //Connection Fail
                            sdk_wifi_station_disconnect();
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
                    } else {
                        netbuf_delete(nb);
                        netconn_close(client);
                        netconn_delete(client);
                    }
                }
            }
            printf("Closing connection\n");
            if (isWifiSet) {
                //ToDo: This is for testing only
                if (!send_status) { 
                    send_to_pmc_data[0] = STANDBY;
                    send_to_pmc_data[1] = 10;
//                    sendDataCnt = 1;                    
//                    sendCnt = 0;
                    send_status = 1;
                }
                vTaskDelay( 50 / portTICK_PERIOD_MS );
                reset_hydro_count();
                sdk_system_restart();
                printf("Wifi is configured by user\n\r");
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
    printf("I am version 1\r\n");
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

    int state = read_device_state();
    reset_device_state();
    if (state == 0)
    {
        vSemaphoreCreateBinary(wifi_alive);
        publish_queue = xQueueCreate(3, PUB_MSG_LEN);
        publish_queue_1 = xQueueCreate(3, PUB_MSG_LEN);
        printf("Normal working mode!\r\n");
        sdk_wifi_softap_stop();
        sdk_wifi_station_start();
        xTaskCreate(&soft_uart_task, "softuart_task", 256, NULL, 1, NULL);
        xTaskCreate(&beat_task, "beat_task", 256, NULL, 1, NULL);
        xTaskCreate(&hydro_task, "hydro_task", 256, NULL, 1, NULL);
        xTaskCreate(&wifi_task, "wifi_task", 1024, NULL, 1, NULL);
        xTaskCreate(&mqtt_task, "mqtt_task", 1024, NULL, 1, NULL);
        xTaskCreate(&ota_task, "get_task", 4096, &ota_info_, 1, NULL);
    } else if (state == 1) {
        printf("Wifi AP mode!\r\n");
        sdk_wifi_softap_stop();
        sdk_wifi_station_stop();
        xTaskCreate(&ap_task, "ap_task", 2048, NULL, 1, NULL);
        xTaskCreate(&ap_count_task, "ap_count_task", 1024, NULL, 1, NULL);
        xTaskCreate(&soft_uart_task, "softuart_task", 256, NULL, 1, NULL);
        reset_device_state();
    } else if (state == 2) {
        printf("Deepsleep mode!\r\n");
        sysStatus = 0;
        sdk_wifi_station_set_auto_connect(false);
        sdk_wifi_station_stop();
        sdk_system_deep_sleep(wakeupTime);
    }
}


#include "smartConfig.h"
#include <string.h>

#define SMART_CONFIG_SECTOR 	1020
#define DEVICE_ID_SECTOR        1019
#define DEVICE_STATE_SECTOR     1018
//1 byte for ssid length, 64 bytes for ssid
//1 byte for password length, 64 bytes for password
#define TOTAL_BYTE_LENGTH 		130
#define MAX_BYTE_LENGTH 		66

#define HTTP_MSG_SSID 			"ssid"
#define HTTP_MSG_PASSWORD 		"password"
#define HTTP_MSG_SCAN			"scan"
#define HTTP_MSG_END            "end"

char buff_device_id[TOTAL_BYTE_LENGTH] = { 0 };
char buff_ssid[TOTAL_BYTE_LENGTH] = { 0 };
char buff_pw[TOTAL_BYTE_LENGTH] = { 0 };

void memory_read(void)
{
    char buff[TOTAL_BYTE_LENGTH] = { 0 };
    sdk_spi_flash_read(SMART_CONFIG_SECTOR*SPI_FLASH_SEC_SIZE, (uint32_t*)&buff, TOTAL_BYTE_LENGTH);
    printf("%s \r\n", buff); 
    sdk_spi_flash_read(SMART_CONFIG_SECTOR*SPI_FLASH_SEC_SIZE + MAX_BYTE_LENGTH, (uint32_t*)&buff, TOTAL_BYTE_LENGTH);
    printf("%s \r\n", buff); 
    
    /*for (uint32_t j = 0; j < 1024; j++)
    {
        uint8_t values[4] = { 0 };
        sdk_spi_flash_read(addr + j*4096, &values, sizeof(values));
        printf("memory addr: 0x%X value: %x %x %x %x\n", addr + j*4096, values[0], values[1], values[2], values[3]);
    }*/
    //printf("memory addr: 0x%X value: %x %x %x %x\n", addr, values[0], values[1], values[2], values[3]);
}

void read_device_id(const char **device_id)
{
    sdk_spi_flash_read(DEVICE_ID_SECTOR*SPI_FLASH_SEC_SIZE, (uint32_t*)&buff_device_id, TOTAL_BYTE_LENGTH);
    *device_id = buff_device_id;
}

void set_device_state()
{
    char buff[TOTAL_BYTE_LENGTH] = "ap_mode";
    sdk_spi_flash_erase_sector(DEVICE_STATE_SECTOR);
    int result = sdk_spi_flash_write(DEVICE_STATE_SECTOR*SPI_FLASH_SEC_SIZE, (uint32_t*) buff, TOTAL_BYTE_LENGTH);
    if (result == SPI_FLASH_RESULT_OK)
    {
        printf("write ok....");
    }
    printf("write result: %d \r\n", result);
}

void reset_device_state()
{
    char buff[TOTAL_BYTE_LENGTH] = "sta_mode";
    sdk_spi_flash_erase_sector(DEVICE_STATE_SECTOR);
    int result = sdk_spi_flash_write(DEVICE_STATE_SECTOR*SPI_FLASH_SEC_SIZE, (uint32_t*) buff, TOTAL_BYTE_LENGTH);
    if (result == SPI_FLASH_RESULT_OK)
    {
        printf("write ok....");
    }
    printf("write result: %d \r\n", result);
}

int read_device_state(void)
{
    int state = 0;
    char buff[TOTAL_BYTE_LENGTH];
    sdk_spi_flash_read(DEVICE_STATE_SECTOR*SPI_FLASH_SEC_SIZE, (uint32_t*)&buff, TOTAL_BYTE_LENGTH);
    printf("state : %s \r\n", buff);
    if (buff[0] == 'a' && buff[1] == 'p')
    {
        printf("Please go to ap mode!\r\n");
        state = 1;
    }
    return state;
}

void read_wifi_config(int id, const char **ssid, const char **password)
{
	printf("read_wifi_config.....");
    sdk_spi_flash_read(SMART_CONFIG_SECTOR*SPI_FLASH_SEC_SIZE + 1, (uint32_t*)&buff_ssid, TOTAL_BYTE_LENGTH);
    printf("%s \r\n", buff_ssid);
    sdk_spi_flash_read(SMART_CONFIG_SECTOR*SPI_FLASH_SEC_SIZE + MAX_BYTE_LENGTH, (uint32_t*)&buff_pw, TOTAL_BYTE_LENGTH);
    printf("%s \r\n", buff_pw); 
	*ssid = buff_ssid; 
	*password = buff_pw;
}

void save_wifi_config(const char *ssid, const char *password, int id)
{
    printf("[*] input data: ssid=%s password=%s \n", ssid, password);
    char buff[TOTAL_BYTE_LENGTH] = { 0 };
    int ssid_len = strlen(ssid);
    int pw_len = strlen(password);
    buff[0] = ssid_len;
    buff[MAX_BYTE_LENGTH - 1] = pw_len;
    strcpy(buff + 1, ssid);
    strcpy(buff + MAX_BYTE_LENGTH, password);
    sdk_spi_flash_erase_sector(SMART_CONFIG_SECTOR);
    int result = sdk_spi_flash_write(SMART_CONFIG_SECTOR*SPI_FLASH_SEC_SIZE, (uint32_t*) buff, TOTAL_BYTE_LENGTH);
    if (result == SPI_FLASH_RESULT_OK)
    {
        printf("write ok....");
    }
    printf("write result: %d \r\n", result);
}

int parse_http_header(char *header)
{
    char *str1, *str2, *token, *subtoken, *saveptr1, *saveptr2;
    char *ssid_input = NULL, *pw_input = NULL;
    const char line_split[] = "\r\n", sub_chart[] = ":";
    const char ssid[] = HTTP_MSG_SSID;
    const char password[] = HTTP_MSG_PASSWORD;
    const char scan[] = HTTP_MSG_SCAN;
    const char end[] = HTTP_MSG_END;
    bool isFoundSSID = false, isFoundPw = false;
    unsigned int j;

    for (j = 1, str1 = header;; j++, str1 = NULL) {
        token = strtok_r(str1, line_split, &saveptr1);
        if (token == NULL)
            break;
        str2     = token;
        subtoken = strtok_r(str2, sub_chart, &saveptr2);
        if (subtoken != NULL && saveptr2 != NULL)
        {
            printf("Parsing.... %s %s \r\n", subtoken, saveptr2);
            if (strcmp(subtoken, ssid) == 0)
            {
                printf("ssid found\r\n");
                isFoundSSID = true;
                ssid_input = saveptr2 + 1;   //Workaround for the header post with one space ahead
            }
            else if (strcmp(subtoken, password) == 0)
            {
                printf("password found\r\n");
                isFoundPw = true;
                pw_input = saveptr2 + 1;
            }
            else if (strcmp(subtoken, scan) == 0)
            {
            	return 1;
            }
            else if (strcmp(subtoken, end) == 0)
            {
                printf("end signal found\r\n");
                return 2;
            } 
        }
    }

    if (isFoundPw && isFoundSSID)
    {
        printf("%s :::::  %s \r\n", ssid_input, pw_input);
        save_wifi_config(ssid_input, pw_input, 0);
        memory_read();
        return 0;
    }
    return 3;
}


#include <espressif/esp_common.h>
#include <espressif/spi_flash.h>

void memory_read(void);
void read_wifi_config(int id, const char **ssid, const char **password);
void save_wifi_config(const char *ssid, const char *password, int id);
void parse_http_header(char *header);
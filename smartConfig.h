
#include <espressif/esp_common.h>
#include <espressif/spi_flash.h>

void memory_read(void);
void read_device_id(const char **device_id);
int read_device_state(void);
void set_device_deepsleep(void);
void reset_device_state(void);
void set_device_state(void);
void read_wifi_config(int id, const char **ssid, const char **password);
void save_wifi_config(const char *ssid, const char *password, int id);
int parse_http_header(char *header);

void increment_hydro_count();
void reset_hydro_count();
int read_hydro_count();

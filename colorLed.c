#include <FreeRTOS.h>
#include "colorLed.h"
#include "multipwm.h"

//static uint8_t pins[] = {0, 12, 13};   //R G B pins LED_R, G, B
//static uint8_t pins_key[] = {15, 4, 5}; //R G B pings KEY_LED_R, G, B
static uint8_t pins_total[] = {15, 12, 13, 0, 4, 5};
static pwm_info_t pwm_total_info;

bool started = true;
bool key_started = true;

void init_led(void) {
	printf("init led...\r\n");
	pwm_total_info.channels = 6;
    multipwm_init(&pwm_total_info);
    for (uint8_t ii=0; ii<pwm_total_info.channels; ii++) {
        multipwm_set_pin(&pwm_total_info, ii, pins_total[ii]);
    }
    multipwm_start(&pwm_total_info);
    //multipwm_set_duty_all(&pwm_total_info, MULTIPWM_MAX_PERIOD);
    for (int i = 0; i < 3; i++) {
    	multipwm_set_duty(&pwm_total_info, i, 0);
    	gpio_disable(0);
    	gpio_disable(4);
    	gpio_disable(5);
    }
	printf("init led finished...\r\n");
}

void close_led(void) {
	started = false;
	multipwm_stop(&pwm_total_info);
	gpio_disable(15);
	gpio_disable(12);
	gpio_disable(13);
}	

void close_key_led(void) {
	key_started = false;
	multipwm_stop(&pwm_total_info);
	gpio_disable(0);
	gpio_disable(4);
	gpio_disable(5);
}

void set_led(uint8_t r, uint8_t g, uint8_t b) {
	for (uint8_t i = 0; i < 3; i++) {
		uint16_t value = 0; 
		if (i == 0) {
			value +=  r*MULTIPWM_MAX_PERIOD/255;
		} else if (i == 1) {
			value +=  g*MULTIPWM_MAX_PERIOD/255;
		} else if (i == 2) {
			value +=  b*MULTIPWM_MAX_PERIOD/255;
		}
        multipwm_set_duty(&pwm_total_info, i, value);
    }
}

void set_key_led(uint8_t r, uint8_t g, uint8_t b) {
	if (r == 0) {
		gpio_disable(0);
	} else {
		gpio_enable(0, GPIO_OUTPUT);
	}
	if (g == 0) {
		gpio_disable(4);
	} else {
		gpio_enable(4, GPIO_OUTPUT);
	}
	if (b == 0) {
		gpio_disable(5);
	} else {
		gpio_enable(5, GPIO_OUTPUT);
	}
	for (uint8_t i = 0; i < 3; i++) {
		uint16_t value = MULTIPWM_MAX_PERIOD; 
		if (i == 0) {
			value -= r*MULTIPWM_MAX_PERIOD/255;
		} else if (i == 1) {
			value -= g*MULTIPWM_MAX_PERIOD/255;
		} else if (i == 2) {
			value -= b*MULTIPWM_MAX_PERIOD/255;
		}
        multipwm_set_duty(&pwm_total_info, i+3, value);
    }
}





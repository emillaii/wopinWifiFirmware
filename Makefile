# Makefile for access_point example
PROGRAM=access_point
EXTRA_COMPONENTS=extras/dhcpserver extras/mbedtls extras/httpd extras/paho_mqtt_c

EXTRA_CFLAGS=-DLWIP_HTTPD_CGI=1 -DLWIP_HTTPD_SSI=1 -I./fsdata

include ../../common.mk 


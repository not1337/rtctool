#
# Makefile
#
# by Andreas Steinmetz, 2020
#
# This file is put in the public domain. Have fun!
#
OPTS=
#
# Enable the following for Raspberry Pi 4B
#
# OPTS=-march=native -mthumb -fomit-frame-pointer -fno-stack-protector

all: rtctool chrony2rtc

rtctool: rtctool.c
	gcc -Wall -Os $(OPTS) -s -o rtctool rtctool.c

chrony2rtc: chrony2rtc.c
	gcc -Wall -Os $(OPTS) -s -o chrony2rtc chrony2rtc.c -lm

libeeprom_i2c.a: libeeprom_i2c.c eeprom_i2c.h
	gcc -Wall -Os $(OPTS) -c libeeprom_i2c.c
	ar -rcuU libeeprom_i2c.a libeeprom_i2c.o

install: rtctool chrony2rtc
	install -m 0755 -o root -g root rtctool /sbin
	install -m 0755 -o root -g root chrony2rtc /sbin

install-service: rtctool
	install -m 0644 -o root -g root rtctool.service /lib/systemd/system
	install -m 0755 -o root -g root rtctool-cron /etc/cron.hourly
	systemctl enable rtctool

uninstall:
	rm -f /sbin/rtctool

uninstall-service:
	-systemctl stop rtctool
	systemctl disable rtctool
	rm -f /lib/systemd/system/rtctool.service
	rm -f /etc/cron.hourly/rtctool-cron

clean:
	rm -f rtctool libeeprom_i2c.a libeeprom_i2c.o

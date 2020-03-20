#
# Makefile
#
# by Andreas Steinmetz, 2020
#
# This file is put in the public domain. Have fun!
#
rtctool: rtctool.c
	gcc -Wall -Os -s -o rtctool rtctool.c

libeeprom_i2c.a: libeeprom_i2c.c eeprom_i2c.h
	gcc -Wall -Os -c libeeprom_i2c.c
	ar -rcuU libeeprom_i2c.a libeeprom_i2c.o

install: rtctool
	install -m 0755 -o root -g root rtctool /sbin

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

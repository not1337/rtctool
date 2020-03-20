/*
 * eeprom_i2c.h
 *
 * by Andreas Steinmetz, 2020
 *
 * This source is put in the public domain. Have fun!
 */

#ifndef EEPROM_I2C_H_INCLUDED
#define EEPROM_I2C_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

/* 24cXX I2C addresses (depends on A0-A2 pin wiring) */

#define EEPROM_I2C_24CXX_BASE_ADDR	0x50
#define EEPROM_I2C_24CXX_MAX_ADDR	0x57

/* maximum size of a block read or write */

#define EEPROM_I2C_MAX_BLOCK_SIZE	32

/*
 * Common Parameter Description:
 *
 * i2cbus  - the I2C bus to access, for Raspberry Pi 4B this is 1
 * i2cfd   - the handle returned by eeprom_i2c_open()
 * i2caddr - the eeprom address, e.g. 0x57 for a 24CXX with A0|A1|A2 = 1|1|1
 * memaddr - the desired memory address of the eeprom memory
 * data    - pointer to the data to be read or written
 * len     - amount of data to be read or written
 *
 * Return value: 0 in case of success and -1 in case of error
 *
 * Note: eeprom_i2c_open returns a handle or -1 in case of an error.
 * Note: eeprom_i2c_busy returns -1 while the eeprom is busy.
 */

/* access I2C bus */

extern int eeprom_i2c_open(int i2cbus);

/* close access to I2C bus */

extern int eeprom_i2c_close(int i2cfd);

/* read page data from eeprom */

extern int eeprom_i2c_page_read(int i2cfd,unsigned char i2caddr,
	unsigned int memaddr,unsigned char *data,int len);

/* write page data to eeprom */

extern int eeprom_i2c_page_write(int i2cfd,unsigned char i2caddr,
	unsigned int memaddr,unsigned char *data,int len);

/* check if eeprom is busy */

extern int eeprom_i2c_busy(int i2cfd,unsigned char i2caddr);

/* read unlimited amount of data from eeprom (use this) */

extern int eeprom_i2c_read(int i2cfd,unsigned char i2caddr,
	unsigned int memaddr,unsigned char *data,int len);

/* write unlimited amount of data to eeprom (use this) */

extern int eeprom_i2c_write(int i2cfd,unsigned char i2caddr,
	unsigned int memaddr,unsigned char *data,int len);

#ifdef __cplusplus
}
#endif

#endif

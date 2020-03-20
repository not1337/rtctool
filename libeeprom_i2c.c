/*
 * libeeprom_i2c.c
 *
 * by Andreas Steinmetz, 2020
 *
 * This source is put in the public domain. Have fun!
 */

#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include "eeprom_i2c.h"

int eeprom_i2c_open(int i2cbus)
{
	int i2cfd;
	unsigned long data;
	char bfr[16];

	if(i2cbus<0||i2cbus>256)goto err1;
	snprintf(bfr,sizeof(bfr),"/dev/i2c-%d",i2cbus);
	if((i2cfd=open(bfr,O_RDWR|O_CLOEXEC))==-1)
	{
		snprintf(bfr,sizeof(bfr),"/dev/i2c/%d",i2cbus);
		if((i2cfd=open(bfr,O_RDWR|O_CLOEXEC))==-1)goto err1;
	}
	if(ioctl(i2cfd,I2C_FUNCS,&data)<0)goto err2;
	if(!(data&I2C_FUNC_I2C))goto err2;
	return i2cfd;

err2:	close(i2cfd);
err1:	return -1;
}

int eeprom_i2c_close(int i2cfd)
{
	return close(i2cfd);
}

int eeprom_i2c_page_read(int i2cfd,unsigned char i2caddr,
	unsigned int memaddr,unsigned char *data,int len)
{
	struct i2c_rdwr_ioctl_data rdwr;
	struct i2c_msg msg[2];
	unsigned char bfr[2];

	if(len<1||len>32)return -1;

	bfr[0]=(memaddr>>8)&0xff;
	bfr[1]=memaddr&0xff;

	rdwr.msgs=msg;
	rdwr.nmsgs=2;

	msg[0].addr=i2caddr;
	msg[0].flags=0;
	msg[0].buf=bfr;
	msg[0].len=2;

	msg[1].addr=i2caddr;
	msg[1].flags=I2C_M_RD;
	msg[1].buf=data;
	msg[1].len=len;

	return ioctl(i2cfd,I2C_RDWR,&rdwr)==2?0:-1;
}

int eeprom_i2c_page_write(int i2cfd,unsigned char i2caddr,
	unsigned int memaddr,unsigned char *data,int len)
{
	struct i2c_rdwr_ioctl_data rdwr;
	struct i2c_msg msg;
	unsigned char bfr[34];

	if(len<1||len>32)return -1;

	bfr[0]=(memaddr>>8)&0xff;
	bfr[1]=memaddr&0xff;
	memcpy(bfr+2,data,len);

	rdwr.msgs=&msg;
	rdwr.nmsgs=1;

	msg.addr=i2caddr;
	msg.flags=0;
	msg.buf=bfr;
	msg.len=len+2;

	return ioctl(i2cfd,I2C_RDWR,&rdwr)==1?0:-1;
}

int eeprom_i2c_busy(int i2cfd,unsigned char i2caddr)
{
	struct i2c_rdwr_ioctl_data rdwr;
	struct i2c_msg msg;

	rdwr.msgs=&msg;
	rdwr.nmsgs=1;

	msg.addr=i2caddr;
	msg.flags=0;
	msg.buf=NULL;
	msg.len=0;

	return ioctl(i2cfd,I2C_RDWR,&rdwr)==1?0:-1;
}

int eeprom_i2c_read(int i2cfd,unsigned char i2caddr,
	unsigned int memaddr,unsigned char *data,int len)
{
	int n=0x20-(memaddr&0x1f);

	if(len<0)return -1;
	if(n>len)n=len;

	while(n)
	{
		if(eeprom_i2c_page_read(i2cfd,i2caddr,memaddr,data,n))
			return -1;
		len-=n;
		data+=n;
		memaddr+=n;
		n=(len>32?32:len);
	}

	return 0;
}

int eeprom_i2c_write(int i2cfd,unsigned char i2caddr,
	unsigned int memaddr,unsigned char *data,int len)
{
	int i;
	int n=0x20-(memaddr&0x1f);

	if(len<0)return -1;
	if(n>len)n=len;

	while(n)
	{
		if(eeprom_i2c_page_write(i2cfd,i2caddr,memaddr,data,n))
			return -1;
		len-=n;
		data+=n;
		memaddr+=n;
		n=(len>32?32:len);

		for(i=0;i<100;i++)
		{
			usleep(500);
			if(!eeprom_i2c_busy(i2cfd,i2caddr))break;
		}
		if(i==100)return -1;
	}

	return 0;
}

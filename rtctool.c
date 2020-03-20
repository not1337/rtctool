/*
 * rtctool.c
 *
 * (c) 2020 Andreas Steinmetz
 *
 * License: GPLv2 (no later version)
 */

#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/pps.h>
#include <sys/ioctl.h>
#include <sys/shm.h>
#include <sched.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <grp.h>
#include <stdio.h>

struct shmtm
{
	int mode;
	volatile int count;
	time_t clocktssec;
	int clocktsusec;
	time_t rcvtssec;
	int rcvtsusec;
	int leap;
	int precision;
	int nsamples;
	volatile int valid;
	unsigned clocktsnsec;
	unsigned rcvtsnsec;
	int dummy[8];
};

static int openi2cdev(int bus,int device)
{
	int fd;
	unsigned long data;
	char bfr[16];

	if(bus<0||bus>256)goto err1;
	snprintf(bfr,sizeof(bfr),"/dev/i2c-%d",bus);
	if((fd=open(bfr,O_RDWR|O_CLOEXEC))==-1)
	{
		snprintf(bfr,sizeof(bfr),"/dev/i2c/%d",bus);
		if((fd=open(bfr,O_RDWR|O_CLOEXEC))==-1)goto err1;
	}
	if(ioctl(fd,I2C_FUNCS,&data)<0)goto err2;
	if(!(data&I2C_FUNC_SMBUS_READ_BYTE))goto err2;
	if(!(data&I2C_FUNC_SMBUS_READ_BYTE_DATA))goto err2;
	if(!(data&I2C_FUNC_SMBUS_WRITE_BYTE))goto err2;
	if(!(data&I2C_FUNC_SMBUS_WRITE_BYTE_DATA))goto err2;
	if(ioctl(fd,I2C_SLAVE,device)<0)goto err2;
	return fd;

err2:	close(fd);
err1:	return -1;
}

static int readi2cbytes(int fd,int reg,int n,unsigned char *dest)
{
	struct i2c_smbus_ioctl_data ctl;
	union i2c_smbus_data data;

	data.block[0]=n;
	ctl.read_write=I2C_SMBUS_READ;
	ctl.command=reg;
	ctl.size=I2C_SMBUS_I2C_BLOCK_DATA;
	ctl.data=&data;
	if(ioctl(fd,I2C_SMBUS,&ctl)==-1)return -1;
	memcpy(dest,data.block+1,n);
	return 0;
}

static int writei2cbytes(int fd,int reg,int n,unsigned char *src)
{
	struct i2c_smbus_ioctl_data ctl;
	union i2c_smbus_data data;

	data.block[0]=n;
	memcpy(data.block+1,src,n);
	ctl.read_write=I2C_SMBUS_WRITE;
	ctl.command=reg;
	ctl.size=I2C_SMBUS_I2C_BLOCK_DATA;
	ctl.data=&data;
	if(ioctl(fd,I2C_SMBUS,&ctl)==-1)return -1;
	return 0;
}

static int ppsopen(int id)
{
	int fd;
	int caps;
	char bfr[32];
	struct pps_kparams parm;

	if(id<0||id>255)goto err1;
	snprintf(bfr,sizeof(bfr),"/dev/pps%d",id);
	if((fd=open(bfr,O_RDWR|O_CLOEXEC))==-1)goto err1;
	if(ioctl(fd,PPS_GETCAP,&caps))goto err2;
	if(!(caps&PPS_CAPTUREASSERT))goto err2;
	if(!(caps&PPS_CANWAIT))goto err2;
	if(ioctl(fd,PPS_GETPARAMS,&parm))goto err2;
	parm.mode|=PPS_CAPTUREASSERT;
	if(caps&PPS_OFFSETASSERT)
	{
		parm.mode|=PPS_OFFSETASSERT;
		memset(&parm.assert_off_tu,0,sizeof(parm.assert_off_tu));
	}
	if(ioctl(fd,PPS_SETPARAMS,&parm))goto err2;
	return fd;

err2:	close(fd);
err1:	return -1;
}

static int ppswait(int fd,unsigned long *seq,struct timespec *stamp)
{
	struct pps_fdata data;

	data.timeout.sec=1;
	data.timeout.nsec=500000000;
	data.timeout.flags=~PPS_TIME_INVALID;
	if(ioctl(fd,PPS_FETCH,&data))return -1;
	stamp->tv_sec=data.info.assert_tu.sec;
	stamp->tv_nsec=data.info.assert_tu.nsec;
	*seq=data.info.assert_sequence;
	return 0;
}

static int ds3231_open(int bus)
{
	return openi2cdev(bus,0x68);
}

static int ds3231_read_time(int fd,struct tm *datim)
{
	unsigned char i2cdatim[7];

	if(readi2cbytes(fd,0x00,7,i2cdatim))return -1;
	if(i2cdatim[2]&0x40)return -1;

	datim->tm_sec=(i2cdatim[0]&0xf)+10*(i2cdatim[0]>>4);
	datim->tm_min=(i2cdatim[1]&0xf)+10*(i2cdatim[1]>>4);
	datim->tm_hour=(i2cdatim[2]&0xf)+10*(i2cdatim[2]>>4);
	datim->tm_wday=i2cdatim[3]-1;
	datim->tm_mday=(i2cdatim[4]&0xf)+10*(i2cdatim[4]>>4);
	datim->tm_mon=(i2cdatim[5]&0xf)+10*((i2cdatim[5]&0x7f)>>4)-1;
	datim->tm_year=(i2cdatim[6]&0xf)+10*(i2cdatim[6]>>4)+100;
	datim->tm_yday=0;
	datim->tm_isdst=0;
	return 0;
}

static int ds3231_write_time(int fd, struct tm *datim)
{
	int val;
	unsigned char i2cdatim[7];

	if(datim->tm_min>59||datim->tm_year<100||datim->tm_year>199)return -1;

	i2cdatim[0]=(datim->tm_sec%10)+((datim->tm_sec/10)<<4);
	i2cdatim[1]=(datim->tm_min%10)+((datim->tm_min/10)<<4);
	i2cdatim[2]=(datim->tm_hour%10)+((datim->tm_hour/10)<<4);
	i2cdatim[3]=datim->tm_wday+1;
	i2cdatim[4]=(datim->tm_mday%10)+((datim->tm_mday/10)<<4);
	val=datim->tm_mon+1;
	i2cdatim[5]=(val%10)+((val/10)<<4);
	val=datim->tm_year-100;
	i2cdatim[6]=(val%10)+((val/10)<<4);
	return writei2cbytes(fd,0x00,7,i2cdatim);
}

static int ds3231_pps(int fd,int mode)
{
	unsigned char data;

	switch(mode)
	{
	case 0:	data=0x1c;
		if(writei2cbytes(fd,0x0e,1,&data))return -1;
		return 0;

	case 1:	data=0x00;
		if(writei2cbytes(fd,0x0e,1,&data))return -1;
		return 0;

	case -1:if(readi2cbytes(fd,0x0e,1,&data))return -1;
		switch(data&0x04)
		{
		case 0x04:return 0;
		case 0x00:return 1;
		}
	default:return -1;
	}
}

static int ds3231_systohc(int fd)
{
	int m;
	struct timespec now;
	struct timespec next;
	struct tm datim;

	if((m=ds3231_pps(fd,-1))==-1)goto err1;
	if(clock_gettime(CLOCK_REALTIME,&now))goto err1;
	next.tv_sec=now.tv_sec+(now.tv_nsec>=900000000?1:0);
	next.tv_nsec=999500000;
	now.tv_sec=next.tv_sec+1;
	gmtime_r(&now.tv_sec,&datim);
	if(clock_nanosleep(CLOCK_REALTIME,TIMER_ABSTIME,&next,NULL))goto err1;
	if(m)if(ds3231_pps(fd,0))goto err1;
	if(clock_gettime(CLOCK_REALTIME,&now))goto err2;
	if(now.tv_sec!=next.tv_sec+1||next.tv_nsec<999000000)goto err2;
	if(ds3231_write_time(fd,&datim))goto err2;
	if(m)if(ds3231_pps(fd,1))goto err1;
	return 0;

err2:	if(m)ds3231_pps(fd,1);
err1:	return -1;
}

static int ds3231_hctosys_pps(int i2c,int pps)
{
	unsigned long seq;
	struct timespec now;
	struct timespec next;
	struct tm datim;
	time_t t;

	if(ppswait(pps,&seq,&now))return -1;
	if(ds3231_read_time(i2c,&datim))return -1;
	t=timegm(&datim);
	next.tv_sec=t+1;
	next.tv_nsec=0;
	now.tv_nsec+=999500000;
	if(now.tv_nsec>=1000000000)
	{
		now.tv_nsec-=1000000000;
		now.tv_sec+=1;
	}
	if(clock_nanosleep(CLOCK_REALTIME,TIMER_ABSTIME,&now,NULL))return -1;
	if(clock_settime(CLOCK_REALTIME,&next))return -1;
	return 0;
}

static int ds3231_hctosys_guessed(int fd)
{
	struct timespec tv;
	struct tm datim;
	time_t t;
	time_t cmp=-1;

	tv.tv_sec=0;
	tv.tv_nsec=50000000;

	while(1)
	{
		if(ds3231_read_time(fd,&datim))return -1;
		t=timegm(&datim);
		if(cmp==-1)cmp=t;
		else if(cmp!=t)break;
		if(clock_nanosleep(CLOCK_REALTIME,0,&tv,NULL))return -1;
	}
	tv.tv_sec=t;
	tv.tv_nsec=0;
	if(clock_settime(CLOCK_REALTIME,&tv))return -1;
	return 0;
}

static int ds3231_get_ageing(int fd,int *value)
{
	signed char data;

	if(readi2cbytes(fd,0x10,1,(unsigned char *)&data))return -1;
	*value=data;
	return 0;
}

static int ds3231_set_ageing(int fd,int value)
{
	unsigned char ctrl;
	unsigned char data;

	if(value<-127||value>127)return -1;

	while(1)
	{
		while(1)
		{
			if(readi2cbytes(fd,0x0e,1,&ctrl))return -1;
			if(!(ctrl&0x20))break;
			usleep(1000);
		}

		while(1)
		{
			if(readi2cbytes(fd,0x0f,1,&data))return -1;
			if(!(data&0x04))break;
			usleep(1000);
		}

		data=(unsigned char)value;
		if(writei2cbytes(fd,0x10,1,&data))return -1;

		ctrl|=0x20;
		if(writei2cbytes(fd,0x0e,1,&ctrl))return -1;

		if(readi2cbytes(fd,0x0f,1,&data))return -1;
		if(!(data&0x04))break;
		usleep(1000);
	}

	while(1)
	{
		if(readi2cbytes(fd,0x0e,1,&ctrl))return -1;
		if(!(ctrl&0x20))break;
		usleep(1000);
	}

	return 0;
}

static int ds3231_get_temp(int fd,int *value)
{
	unsigned char data[2];

	if(readi2cbytes(fd,0x11,2,data))return -1;
	*value=((signed char)data[0])*100;
	switch(data[1]&0xc0)
	{
	case 0x40:
		if(*value<0)*value-=25;
		else *value+=25;
		break;
	case 0x80:
		if(*value<0)*value-=50;
		else *value+=50;
		break;
	case 0xc0:
		if(*value<0)*value-=75;
		else *value+=75;
		break;
	}
	return 0;
}

static int ds3231_estimate_calibration(int i2c,int pps,int iter,int *result,
	int (*callback)(int current,int total,void *param),void *param)
{
	int total;
	int value=0;
	int delta=64;
	int rqdsec;
	int currsec=0;
	unsigned long seq;
	unsigned long lcl;
	unsigned long long sum;
	struct timespec now;
	struct timespec prev;
	struct timespec data;

	rqdsec=(iter+1)*7;

	while(1)
	{
		if(ds3231_set_ageing(i2c,value))return -1;

		if(!delta)break;

		if(ppswait(pps,&lcl,&prev))return -1;
		if(callback)if(callback(++currsec,rqdsec,param))return -1;

		for(sum=0,total=0;total<iter;total++)
		{
			if(ppswait(pps,&seq,&now))return -1;
			if(callback)if(callback(++currsec,rqdsec,param))
				return -1;
			if(++lcl!=seq)return -1;
			if(now.tv_sec<prev.tv_sec)return -1;
			data.tv_sec=now.tv_sec-prev.tv_sec;
			if(data.tv_sec>1)return -1;
			if(now.tv_nsec<prev.tv_nsec)
			{
				if(!data.tv_sec)return -1;
				data.tv_sec--;
				data.tv_nsec=now.tv_nsec+1000000000-
					prev.tv_nsec;
			}
			else data.tv_nsec=now.tv_nsec-prev.tv_nsec;
			prev=now;
			if(data.tv_sec)
			{
				if(data.tv_nsec>100000000)return -1;
			}
			else if(data.tv_nsec<900000000)return -1;
			sum+=data.tv_sec?1000000000:0;
			sum+=data.tv_nsec;
		}

		sum/=total;

		if(sum>1000000000)value-=delta;
		else value+=delta;
		delta>>=1;
	}

	*result=value;

	return 0;
}

int shmrunner(int i2cid,int ppsid,int id,int bg)
{
	int shmid;
	int pps;
	int i2c;
	time_t now;
	unsigned long seq;
	unsigned long prv;
	struct group *gr;
	struct shmtm *stm;
	struct timespec tv;
	struct tm tm;

	if(getuid()&&geteuid())goto err1;
	if(!(gr=getgrnam("_chrony")))goto err1;
	if(setgid(gr->gr_gid))goto err1;
	if((shmid=shmget((key_t)(0x4e545030+id),sizeof(struct shmtm),
		(int)(IPC_CREAT|0660)))==-1)goto err1;
	if((stm=(struct shmtm *)shmat(shmid,0,0))==(void *)(-1))goto err1;
	memset(stm,0,sizeof(struct shmtm));
	stm->mode=1;
	stm->precision=-20;
	stm->nsamples=3;
	if((pps=ppsopen(ppsid))==-1)goto err2;
	if((i2c=ds3231_open(i2cid))==-1)goto err3;
	if(ppswait(pps,&prv,&tv))goto err4;
	if(bg)if(daemon(0,0))goto err4;

	while(1)
	{
		if(ppswait(pps,&seq,&tv))goto err4;
		if(++prv!=seq)goto err4;
		if(ds3231_read_time(i2c,&tm))goto err4;
		if((now=timegm(&tm))==(time_t)(-1))goto err4;
		stm->count++;
		stm->valid=0;
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		stm->clocktssec=now;
		stm->clocktsusec=0;
		stm->clocktsnsec=0;
		stm->rcvtssec=tv.tv_sec;
		stm->rcvtsusec=tv.tv_nsec/1000;
		stm->rcvtsnsec=tv.tv_nsec;
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		stm->count++;
		stm->valid=1;
	}

err4:	close(i2c);
err3:	close(pps);
err2:	stm->valid=0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	shmdt(stm);
err1:	return -1;
}

static int cb(int current,int total,void *param)
{
	int remain=total-current;
	int es;
	int em;
	int rs;
	int rm;

	em=current/60;
	es=current%60;
	rm=remain/60;
	rs=remain%60;
	printf("\rPlease wait, %dm%02ds elapsed, %dm%02ds remaining...        ",
		em,es,rm,rs);
	if(current==total)printf("\n");
	else fflush(stdout);
	return 0;
}

static void usage(void)
{
fprintf(stderr,
"rtctool - a helper tool for the DS3231 RTC chip with SQW connected for PPS.\n"
"\n"
"Usage:\n"
"\n"
"rtctool -h\n"
"rtctool [-i <i2cid>] -t\n"
"rtctool [-i <i2cid>] -s\n"
"rtctool [-i <i2cid>] [-c <ppsid>] -r\n"
"rtctool [-i <i2cid>] -a\n"
"rtctool [-i <i2cid>] -A value\n"
"rtctool [-i <i2cid>] -p\n"
"rtctool [-i <i2cid>] -P value\n"
"rtctool [-i <i2cid>] [-c <ppsid>] -e\n"
"rtctool [-i <i2cid>] [-c <ppsid>] [-n <ntpid] [-b] -d\n"
"rtctool [-i <i2cid>] -T\n"
"\n"
"-h    this help text\n"
"-t    print rtc time\n"
"-s    system time to rtc time\n"
"-r    rtc time to system time\n"
"-a    print ageing value\n"
"-A    set ageing value (-127 <= value <= 127)\n"
"-p    print PPS output status\n"
"-P    enable/disable PPS output (1=enable, 0=disable)\n"
"-e    estimate ageing value (requires good NTP sync and takes 30 minutes)\n"
"-d    run as SHM master clock daemon (gpsd replacement for chrony)\n"
"-T    print chip temperature\n"
"-i    i2c bus number, default 1, range 0-1\n"
"-c    pps device number, default 0, range 0-3\n"
"-n    ntp shared memory id, default 2, range 0-9\n"
"-b    daemonize and run in background\n");
exit(1);
}

int main(int argc,char *argv[])
{
	int shmid=2;
	int pps=0;
	int i2c=1;
	int op=-1;
	int val=0;
	int rt=0;
	int bg=0;
	int c;
	int fd1;
	int fd2;
	struct tm datim;
	struct sched_param s;
	char bfr[32];

	while((c=getopt(argc,argv,"htsraA:pP:edTi:c:n:b"))!=-1)switch(c)
	{
	case 't':
		if(op!=-1)usage();
		op=0;
		break;

	case 's':
		if(op!=-1)usage();
		op=1;
		rt=1;
		break;

	case 'r':
		if(op!=-1)usage();
		op=2;
		rt=1;
		break;

	case 'a':
		if(op!=-1)usage();
		op=3;
		break;

	case 'A':
		if(op!=-1)usage();
		op=4;
		val=atoi(optarg);
		if(val<-127||val>127)usage();
		break;

	case 'p':
		if(op!=-1)usage();
		op=5;
		break;

	case 'P':
		if(op!=-1)usage();
		op=6;
		val=atoi(optarg);
		if(val<0||val>1)usage();
		break;

	case 'e':
		if(op!=-1)usage();
		op=7;
		rt=1;
		break;

	case 'd':
		if(op!=-1)usage();
		op=8;
		rt=1;
		break;

	case 'T':
		if(op!=-1)usage();
		op=9;
		break;

	case 'i':
		i2c=atoi(optarg);
		if(i2c<0||i2c>1)usage();
		break;

	case 'c':
		pps=atoi(optarg);
		if(pps<0||pps>3)usage();
		break;

	case 'n':
		shmid=atoi(optarg);
		if(shmid<0||shmid>9)usage();
		break;

	case 'b':
		bg=1;
		break;

	case 'h':
	default:usage();
	}

	if(op==-1)usage();
	if(bg&&op!=8)usage();

	if(rt)
	{
		s.sched_priority=sched_get_priority_max(SCHED_RR);
		if(sched_setscheduler(0,SCHED_RR,&s))
		{
			fprintf(stderr,"Can't set realtime priority.\n");
			return 1;
		}
	}

	switch(op)
	{
	case 0:	if((fd1=ds3231_open(i2c))==-1)
		{
			fprintf(stderr,"Can't access DS3231 device.\n");
			return 1;
		}
		if(ds3231_read_time(fd1,&datim))
		{
			fprintf(stderr,"Can't read DS3231 time.\n");
			close(fd1);
			return 1;
		}
		strftime(bfr,sizeof(bfr),"%a %F %T",&datim);
		printf("%s\n",bfr);
		close(fd1);
		break;

	case 1:	if((fd1=ds3231_open(i2c))==-1)
		{
			fprintf(stderr,"Can't access DS3231 device.\n");
			return 1;
		}
		if(ds3231_systohc(fd1))
		{
			fprintf(stderr,"Can't set DS3231 time from system "
				"time.\n");
			close(fd1);
			return 1;
		}
		close(fd1);
		break;

	case 2:	if((fd1=ds3231_open(i2c))==-1)
		{
			fprintf(stderr,"Can't access DS3231 device.\n");
			return 1;
		}
		if((fd2=ppsopen(pps))==-1)goto guess;
		if(ds3231_hctosys_pps(fd1,fd2))
		{
			close(fd2);
			goto guess;
		}
		close(fd2);
		close(fd1);
		break;

guess:		fprintf(stderr,"Warning: Using PPS for precise transfer "
			"failed, guessing now...\n");
		if(ds3231_hctosys_guessed(fd1))
		{
			fprintf(stderr,"Can't set system time from DS3231 "
				"time.\n");
			close(fd1);
			return 1;
		}
		close(fd1);
		break;

	case 3:	if((fd1=ds3231_open(i2c))==-1)
		{
			fprintf(stderr,"Can't access DS3231 device.\n");
			return 1;
		}
		if(ds3231_get_ageing(fd1,&val))
		{
			fprintf(stderr,"Can't read DS3231 ageing value.\n");
			close(fd1);
			return 1;
		}
		printf("Ageing value: %d\n",val);
		close(fd1);
		break;

	case 4:	if((fd1=ds3231_open(i2c))==-1)
		{
			fprintf(stderr,"Can't access DS3231 device.\n");
			return 1;
		}
		if(ds3231_set_ageing(fd1,val))
		{
			fprintf(stderr,"Can't write DS3231 ageing value.\n");
			close(fd1);
			return 1;
		}
		close(fd1);
		break;

	case 5:	if((fd1=ds3231_open(i2c))==-1)
		{
			fprintf(stderr,"Can't access DS3231 device.\n");
			return 1;
		}
		switch(ds3231_pps(fd1,-1))
		{
		case 0:	printf("PPS output on SQW pin disabled.\n");
			break;

		case 1:	printf("PPS output on SQW pin enabled.\n");
			break;

		case -1:fprintf(stderr,"Can't read DS3231 SQW status.\n");
			close(fd1);
			return 1;
		}
		close(fd1);
		break;

	case 6:	if((fd1=ds3231_open(i2c))==-1)
		{
			fprintf(stderr,"Can't access DS3231 device.\n");
			return 1;
		}
		if(ds3231_pps(fd1,val))
		{
			fprintf(stderr,"Can't write DS3231 SQW config.\n");
			close(fd1);
			return 1;
		}
		close(fd1);
		break;

	case 7:	if((fd1=ds3231_open(i2c))==-1)
		{
			fprintf(stderr,"Can't access DS3231 device.\n");
			return 1;
		
		}
		if((fd2=ppsopen(pps))==-1)
		{
			fprintf(stderr,"Can't access /dev/pps%d\n",pps);
			close(fd1);
			return 1;
		}
		if(ds3231_estimate_calibration(fd1,fd2,256,&val,cb,NULL))
		{
			fprintf(stderr,"DS3231 ageing estimation failed.\n");
			close(fd2);
			close(fd1);
			return 1;
		}
		printf("Estimated ageing value: %d\n",val);
		close(fd2);
		close(fd1);
		break;

	case 8:	if(shmrunner(i2c,pps,shmid,bg))
		{
			fprintf(stderr,"Failed to start SHM master clock "
				"daemon\n");
			return 1;
		}
		break;

	case 9:	if((fd1=ds3231_open(i2c))==-1)
		{
			fprintf(stderr,"Can't access DS3231 device.\n");
			return 1;
		}
		if(ds3231_get_temp(fd1,&val))
		{
			fprintf(stderr,"Can't read DS3231 temperature.\n");
			close(fd1);
			return 1;
		}
		if(val<0)
		{
			val=-val;
			printf("Temperature: -%d.%02d°C\n",val/100,val%100);
		}
		else printf("Temperature: %d.%02d°C\n",val/100,val%100);
		close(fd1);
		break;
	}

	return 0;
}

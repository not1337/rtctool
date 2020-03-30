/*
 * chrony2rtc.c
 *
 * (c) 2020 Andreas Steinmetz
 *
 * License: GPLv2 (no later version)
 */

#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <string.h>
#include <endian.h>
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <signal.h>
#include <poll.h>
#include <stdio.h>

#define RTCTOOL "/sbin/rtctool"
#define SOCKET "/run/chrony/chronyd.sock"
#define CLIENT "/run/chrony/rtcclient.%d.sock"
#define REQ_TRACKING 33
#define RPY_TRACKING 5
#define PROTO_VERSION_NUMBER 6
#define PKT_TYPE_CMD_REQUEST 1
#define PKT_TYPE_CMD_REPLY 2
#define FLOAT_EXP_BITS 7
#define FLOAT_COEF_BITS ((((int)sizeof(int32_t))<<3)-FLOAT_EXP_BITS)

typedef struct
{
	uint8_t version;
	uint8_t pkt_type;
	uint8_t res1;
	uint8_t res2;
	uint16_t command;
	uint16_t reply;
	uint16_t status;
	uint16_t pad1;
	uint16_t pad2;
	uint16_t pad3;
	uint32_t sequence;
	uint32_t pad4;
	uint32_t pad5;

	uint32_t ref_id;
	struct
	{
		union {
			uint32_t in4;
			uint8_t in6[16];
		} addr;
		uint16_t family;
		uint16_t _pad;
	} ip_addr;
	uint16_t stratum;
	uint16_t leap_status;
	struct
	{
		uint32_t tv_sec_high;
		uint32_t tv_sec_low;
		uint32_t tv_nsec;
	} ref_time;
	int32_t current_correction;
	int32_t last_offset;
	int32_t rms_offset;
	int32_t freq_ppm;
	int32_t resid_freq_ppm;
	int32_t skew_ppm;
	int32_t root_delay;
	int32_t root_dispersion;
	int32_t last_update_interval;
	int32_t EOR;
} REPLY;

typedef struct
{
	uint8_t version;
	uint8_t pkt_type;
	uint8_t res1;
	uint8_t res2;
	uint16_t command;
	uint16_t attempt;
	uint32_t sequence;
	uint32_t pad1;
	uint32_t pad2;
	uint8_t padding[84];
} REQUEST;

static double fntoh(uint32_t f)
{
	int32_t exp;
	int32_t coef;
	uint32_t x;

	x=be32toh(f);
	exp=x>>FLOAT_COEF_BITS;
	if(exp>=1<<(FLOAT_EXP_BITS-1))exp-= 1<<FLOAT_EXP_BITS;
	exp-=FLOAT_COEF_BITS;
	coef=x%(1U<<FLOAT_COEF_BITS);
	if(coef>=1<<(FLOAT_COEF_BITS-1))coef-=1<<FLOAT_COEF_BITS;
	return coef*pow(2.0,exp);
}

static int doconn(char *sock)
{
	int s;
	int msk;
	struct sockaddr_un a;
	struct sockaddr_un b;

	memset(&a,0,sizeof(a));
	a.sun_family=AF_UNIX;
	strncpy(a.sun_path,sock,sizeof(a.sun_path)-1);
	memset(&b,0,sizeof(b));
	b.sun_family=AF_UNIX;
	snprintf(b.sun_path,sizeof(b.sun_path),CLIENT,getpid());
	if((s=socket(PF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0))==-1)goto err1;
	msk=umask(0);
	if(bind(s,(struct sockaddr *)(&b),sizeof(b)))
	{
		umask(msk);
		goto err2;
	}
	umask(msk);
	if(connect(s,(struct sockaddr *)(&a),sizeof(a)))
	{
		unlink(b.sun_path);
err2:		close(s);
err1:		return -1;
	}
	return s;
}

static void dodisc(int s)
{
	char bfr[128];

	snprintf(bfr,sizeof(bfr),CLIENT,getpid());
	close(s);
	unlink(bfr);
}

static int getdata(int s,int *strt,double *corr,double *skew)
{
	REQUEST req;
	REPLY ans;
	struct pollfd p;

	p.fd=s;
	p.events=POLLIN;

	memset(&req,0,sizeof(req));

	req.command=htobe16(REQ_TRACKING);
	req.pkt_type=PKT_TYPE_CMD_REQUEST;
	req.version=PROTO_VERSION_NUMBER;
	req.sequence=1;

	if(send(s,&req,offsetof(REPLY,EOR),0)!=offsetof(REPLY,EOR))return -1;

	if(poll(&p,1,100)<1)return -1;
	if(!(p.revents&POLLIN))return -1;

	if(recv(s,&ans,offsetof(REPLY,EOR),0)!=offsetof(REPLY,EOR))return -1;

	if(ans.command!=req.command||ans.pkt_type!=PKT_TYPE_CMD_REPLY||
		ans.version!=PROTO_VERSION_NUMBER||ans.sequence!=req.sequence||
		ans.status||be16toh(ans.reply)!=RPY_TRACKING)return -1;

	*strt=be16toh(ans.stratum);
	*corr=fabs(fntoh(ans.current_correction));
	*skew=fntoh(ans.skew_ppm);
	return 0;
}

static void usage(void)
{
	fprintf(stderr,"Usage: chrony2rtc [<options>] -s <stratum> "
		"-c <correction> -S <skew>\n"
		"-s <stratum>           chrony stratum must be smaller than "
		"this value\n"
		"-c <correction>        chrony correction must be smaller "
		"than this value\n"
		"-S <skew>              chrony clock skew must be smaller "
		"than this value\n"
		"-C <chronyd-socket>    chronyd socket, default "
		"/run/chrony/chronyd.sock\n"
		"-T <rtctool-pathname>  rtctool pathname, default "
			"/sbin/rtctool\n"
		"-d                     daemonize\n");
	exit(1);
}

int main(int argc,char *argv[])
{
	int c;
	int err=1;
	int s=-1;
	int dmn=0;
	int tfd;
	int sfd;
	int sts;
	int stratum;
	int cmpstrat=0;
	unsigned long delay=360;
	double correction;
	double cmpcorr=0;
	double skew;
	double cmpskew=0;
	uint64_t v;
	char *sock=SOCKET;
	char *tool=RTCTOOL;
	struct pollfd pp[2];
	struct itimerspec it;
	sigset_t set;

	while((c=getopt(argc,argv,"s:c:S:C:Td"))!=-1)switch(c)
	{
	case 's':
		if((cmpstrat=atoi(optarg))<=0||cmpstrat>=16)usage();
		break;

	case 'c':
		if((cmpcorr=atof(optarg))<=0||cmpcorr>=1)usage();
		break;

	case 'S':
		if((cmpskew=atof(optarg))<=0||cmpskew>=1)usage();
		break;

	case 'C':
		sock=optarg;
		break;

	case 'T':
		tool=optarg;
		break;

	case 'd':
		dmn=1;
		break;

	default:usage();
		break;
	}

	if(!cmpstrat||!cmpcorr||!cmpskew)usage();

	if((tfd=timerfd_create(CLOCK_MONOTONIC,TFD_CLOEXEC|TFD_NONBLOCK))==-1)
	{
		perror("timerfd_create");
		goto err1;
	}

	it.it_value.tv_sec=10;
	it.it_value.tv_nsec=0;
	it.it_interval.tv_sec=10;
	it.it_interval.tv_nsec=0;

	if(timerfd_settime(tfd,0,&it,NULL))
	{
		perror("timerfd_settime");
		goto err2;
	}

	sigfillset(&set);
	sigprocmask(SIG_BLOCK,&set,NULL);
	sigemptyset(&set);
	sigaddset(&set,SIGINT);
	sigaddset(&set,SIGTERM);
	sigaddset(&set,SIGHUP);
	sigaddset(&set,SIGQUIT);
	if((sfd=signalfd(-1,&set,SFD_NONBLOCK|SFD_CLOEXEC))==-1)
	{
		perror("signalfd");
		goto err2;
	}

	if(dmn)if(daemon(0,0))
	{
		perror("daemon");
		goto err3;
	}

	pp[0].fd=tfd;
	pp[0].events=POLLIN;
	pp[1].fd=sfd;
	pp[1].events=POLLIN;

	while(1)
	{
		if(poll(pp,2,-1)<1)continue;
		if(pp[1].revents&POLLIN)break;
		if(!(pp[0].revents&POLLIN))continue;
		if(read(tfd,&v,sizeof(v))!=sizeof(v))continue;

		if(++delay<360)continue;

		if(s==-1)s=doconn(sock);
		if(s==-1)continue;

		if(getdata(s,&stratum,&correction,&skew))
		{
			dodisc(s);
			s=-1;
			continue;
		}

		if(stratum>=cmpstrat||correction>=cmpcorr||skew>=cmpskew)
			continue;

		switch(fork())
		{
		case -1:continue;
		case 0:	return execl(tool,tool,"-s",NULL);
		default:if(wait(&sts)==-1||sts)continue;
			break;
		}

		delay=0;
		dodisc(s);
		s=-1;
	}

	err=0;

	if(s!=-1)dodisc(s);
err3:	close(sfd);
err2:	close(tfd);
err1:	return err;
}

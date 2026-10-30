#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/joystick.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#define PR_ANDR36OID_GUEST 0x41333601
#define WORDS ((KEY_MAX+8*sizeof(long))/(8*sizeof(long)))
#define BIT(b,k) (((b)[(k)/(8*sizeof(long))]>>((k)%(8*sizeof(long))))&1)
static const unsigned short host[]={304,305,307,308,310,311,312,313,103,108,105,106,314,315,158,125,172};
static const unsigned short guest[]={305,304,307,308,310,311,312,313,544,545,546,547,704,705,706,707,708};
static int parse_caps(char *line,unsigned long *bits) {
    unsigned long words[WORDS];unsigned count=0;char *save,*part;
    memset(bits,0,WORDS*sizeof(*bits));
    for(part=strtok_r(line," \n",&save);part;part=strtok_r(NULL," \n",&save)) {
        if(count==WORDS)return -1;
        char *end;words[count++]=strtoul(part,&end,16);if(*end)return -1;
    }
    for(unsigned i=0;i<count;i++)bits[i]=words[count-1-i];
    return count?0:-1;
}
static int sys_caps(const char *event,unsigned long *bits) {
    char path[256],line[4096];const char *name=strrchr(event,'/');if(!name)return -1;
    snprintf(path,sizeof(path),"/sys/class/input/%s/device/capabilities/key",name+1);
    FILE *f=fopen(path,"r");if(!f)return -1;
    char *ok=fgets(line,sizeof(line),f);fclose(f);return ok?parse_caps(line,bits):-1;
}
static int proc_caps(unsigned long *bits) {
    FILE *f=fopen("/proc/bus/input/devices","r");if(!f)return -1;
    char line[4096];int pad=0,result=-1;
    while(fgets(line,sizeof(line),f)) {
        if(line[0]=='N')pad=strstr(line,"GO-Super Gamepad")!=NULL;
        if(pad && !strncmp(line,"B: KEY=",7)){result=parse_caps(line+7,bits);break;}
    }
    fclose(f);return result;
}
int main(int argc,char **argv) {
    if(argc!=3) {fprintf(stderr,"usage: guest-input /dev/input/eventN /dev/input/jsN\n");return 2;}
    int fd=open(argv[1],O_RDONLY|O_NONBLOCK);unsigned long before[WORDS]={0},after[WORDS]={0};
    if(fd<0 || ioctl(fd,EVIOCGBIT(EV_KEY,sizeof(before)),before)<0) {perror("host input");return 1;}
    if(prctl(PR_ANDR36OID_GUEST,1UL,0UL,0UL,0UL)!=-1 || errno!=EPERM) {fprintf(stderr,"host mode must be rejected (kernel ABI missing?)\n");return 1;}
    if(unshare(CLONE_NEWPID|CLONE_NEWIPC)) {perror("namespace");return 1;}
    pid_t p=fork();if(p<0)return 1;
    if(!p) {
        if(prctl(PR_ANDR36OID_GUEST,1UL,0UL,0UL,0UL))_exit(10);
        int g=open(argv[1],O_RDONLY|O_NONBLOCK);
        if(g<0 || ioctl(g,EVIOCGBIT(EV_KEY,sizeof(after)),after)<0)_exit(11);
        for(unsigned i=0;i<sizeof(host)/sizeof(*host);++i)
            if(!BIT(before,host[i]) || !BIT(after,guest[i]))_exit(12);
        if(BIT(after,KEY_UP)||BIT(after,KEY_HOMEPAGE))_exit(13);
        unsigned long sys[WORDS],proc[WORDS];
        if(sys_caps(argv[1],sys) || memcmp(after,sys,sizeof(after)))_exit(17);
        if(proc_caps(proc) || memcmp(after,proc,sizeof(after)))_exit(18);
        /* A host fd inherited by the guest still has the host ABI. */
        unsigned long inherited[WORDS]={0};
        if(ioctl(fd,EVIOCGBIT(EV_KEY,sizeof(inherited)),inherited)<0 || memcmp(before,inherited,sizeof(before)))_exit(14);
        int js=open(argv[2],O_RDONLY|O_NONBLOCK);unsigned char count=0;unsigned short map[KEY_MAX-BTN_MISC+1]={0};
        if(js<0 || ioctl(js,JSIOCGBUTTONS,&count)<0 || count<17 || ioctl(js,JSIOCGBTNMAP,map)<0)_exit(15);
        const unsigned short expected[]={304,305,307,308,310,311,312,313,544,545,546,547,704,705,706,707,708};
        if(memcmp(expected,map,sizeof(expected)))_exit(16);
        _exit(0);
    }
    int status;if(waitpid(p,&status,0)!=p || !WIFEXITED(status)||WEXITSTATUS(status)) {fprintf(stderr,"guest check failed: %d\n",status);return 1;}
    if(ioctl(fd,EVIOCGBIT(EV_KEY,sizeof(after)),after)<0 || memcmp(before,after,sizeof(before)))return 1;
    unsigned long sys[WORDS],proc[WORDS];
    if(sys_caps(argv[1],sys)||proc_caps(proc)||memcmp(before,sys,sizeof(before))||memcmp(before,proc,sizeof(before)))return 1;
    puts("PASS: guest evdev/jsdev/proc/sysfs ABI, unchanged host and inherited evdev descriptors");return 0;
}

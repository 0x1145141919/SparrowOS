// nvme_lat_curve_rr.c — T(s) 轮转版: 所有尺寸按轮次交错采样
// 背景干扰若是时间聚类(一阵一阵的), 均匀摊到每个尺寸 → 曲线形态不被单点扭曲
// 用法: nvme_lat_curve_rr <dev> [window_gb=4] [iters=500] [csv]
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

static uint64_t now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static int cmp_u64(const void*a,const void*b){ uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b; return x<y?-1:x>y?1:0; }

#define MAXSIZES 300
int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage: %s <dev> [window_gb=4] [iters=500] [csv]\n",argv[0]);return 1;}
    const char*path=argv[1];
    long long win=(argc>2?atoll(argv[2]):4)<<30;
    int iters=argc>3?atoi(argv[3]):500;
    const char*csvp=argc>4?argv[4]:"lat_rr.csv";

    int fd=open(path,O_RDWR|O_DIRECT);
    if(fd<0)fd=open(path,O_RDONLY|O_DIRECT);
    if(fd<0){perror("open");return 1;}
    off_t sz=lseek(fd,0,SEEK_END);
    if(sz<win){fprintf(stderr,"too small\n");return 1;}
    off_t base=sz-win;

    size_t maxbuf=32u<<20;
    void*buf; if(posix_memalign(&buf,4096,maxbuf)){perror("memalign");return 1;}
    memset(buf,0xA5,maxbuf);

    long long sizes[MAXSIZES]; int n=0;
    for(long long s=4096;s<=1048576;s+=4096) sizes[n++]=s;
    long long extra[]={2,4,8,16,32};
    for(unsigned i=0;i<sizeof(extra)/sizeof(extra[0]);i++) sizes[n++]=extra[i]<<20;

    uint64_t(*ts)[MAXSIZES]=malloc(sizeof(*ts)*iters);   // ts[iter][size]
    if(!ts){perror("malloc");return 1;}
    unsigned seed=0xc0ffee;
    long long posn[MAXSIZES];
    for(int si=0;si<n;si++){ posn[si]=win/sizes[si]; if(posn[si]<1)posn[si]=1; }
    for(int si=0;si<n;si++)                       // warmup 每尺寸 20 次
        for(int w=0;w<20;w++){ off_t o=base+(rand_r(&seed)%posn[si])*sizes[si]; pread(fd,buf,(size_t)sizes[si],o); }

    for(int r=0;r<iters;r++)                       // 轮转: 一轮内遍历所有尺寸
        for(int si=0;si<n;si++){
            off_t o=base+(rand_r(&seed)%posn[si])*sizes[si];
            uint64_t t0=now_ns();
            ssize_t rr=pread(fd,buf,(size_t)sizes[si],o);
            ts[r][si]=now_ns()-t0;
            if(rr!=(ssize_t)sizes[si]){fprintf(stderr,"pread err s=%lld r=%zd e=%d\n",sizes[si],rr,errno);return 1;}
        }

    FILE*csv=fopen(csvp,"w");
    fprintf(csv,"# %s round-robin window_base=%lld iters=%d\n# size_bytes,ncmd,min_us,avg_us,p50_us,p90_us,p99_us,max_us\n",path,(long long)base,iters);
    printf("%-10s %-4s %8s %8s %8s %8s %8s %8s\n","size","ncmd","min","avg","p50","p90","p99","max");
    double Sx=0,Sy=0,Sxx=0,Sxy=0; int fn=0;
    for(int si=0;si<n;si++){
        long long s=sizes[si];
        int ncmd=(int)((s+1048576-1)/1048576);
        uint64_t v[iters];
        for(int r=0;r<iters;r++) v[r]=ts[r][si];
        qsort(v,iters,sizeof(*v),cmp_u64);
        uint64_t sum=0; for(int r=0;r<iters;r++)sum+=v[r];
        double avg=(double)sum/iters/1000.0;
        double vmin=v[0]/1000.0, v50=v[iters/2]/1000.0;
        double v90=v[(int)(iters*0.90)]/1000.0, v99=v[(int)(iters*0.99)]/1000.0, vmax=v[iters-1]/1000.0;
        fprintf(csv,"%lld,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",s,ncmd,vmin,avg,v50,v90,v99,vmax);
        printf("%-10lld %-4d %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f\n",s,ncmd,vmin,avg,v50,v90,v99,vmax);
        if(s>=65536&&s<=1048576){ double x=s/1024.0,y=v50; Sx+=x;Sy+=y;Sxx+=x*x;Sxy+=x*y;fn++; }
    }
    double b=(fn*Sxy-Sx*Sy)/(fn*Sxx-Sx*Sx), a=(Sy-b*Sx)/fn;
    printf("\n[fit p50 64K..1M] T(s) ≈ %.2f us + %.4f us/KiB * s  (%.2f us/MiB)  n=%d\n",a,b,b*1024,fn);
    fclose(csv);
    printf("[csv] %s\n",csvp);
    close(fd);
    return 0;
}

// nvme_read_bench.c
// 对比: A = 2x阻塞4K读 (pread x2, 串行阻塞), B = 1x阻塞8K读 (pread x1)
// 语义: 搬同样 8KB 数据, O_DIRECT 直通 NVMe
// 用法: nvme_read_bench <file> [window_mb=256] [iters=3000] [fill=0|1] [scatter=0|1]
//   fill=1: 先真实落盘写一遍 window (ext4 空洞读不走盘, 必须先 fill)
//   scatter=1: A 的两次 4K 读随机两块(两次都冷); 默认相邻覆盖同一个 8K 区间
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

static uint64_t now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static int cmp_u64(const void*a,const void*b){ uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b; return x<y?-1:x>y?1:0; }

static void report(uint64_t*v,int n,const char*name){
    qsort(v,n,sizeof(*v),cmp_u64);
    uint64_t s=0; for(int i=0;i<n;i++)s+=v[i];
    printf("%-18s avg %8.2f us | min %7.2f | p50 %7.2f | p90 %7.2f | p99 %8.2f | max %8.2f\n",
        name,(double)s/n/1000.0,v[0]/1000.0,v[n/2]/1000.0,v[(int)(n*0.90)]/1000.0,v[(int)(n*0.99)]/1000.0,v[n-1]/1000.0);
}
int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage: %s <file> [window_mb=256] [iters=3000] [fill=0|1] [scatter=0|1]\n",argv[0]);return 1;}
    const char*path=argv[1];
    long window_mb = argc>2?atol(argv[2]):256;
    int iters      = argc>3?atoi(argv[3]):3000;
    int fill       = argc>4?atoi(argv[4]):0;
    int scatter    = argc>5?atoi(argv[5]):0;

    int fd=open(path,O_RDWR|O_DIRECT);
    if(fd<0 && !fill) fd=open(path,O_RDONLY|O_DIRECT);
    if(fd<0){perror("open");return 1;}
    off_t sz=lseek(fd,0,SEEK_END);
    if(sz<0){perror("lseek");return 1;}
    long win_bytes=window_mb*1024*1024L;
    if(sz<win_bytes){fprintf(stderr,"%s too small: %lld < %ld bytes\n",path,(long long)sz,win_bytes);return 1;}
    long nblk=win_bytes/8192;
    off_t base=sz-win_bytes;

    void*buf; if(posix_memalign(&buf,4096,8192)){perror("memalign");return 1;} memset(buf,0xAB,8192);

    if(fill){
        printf("[fill] O_DIRECT 写 %ld MB 落盘 (%s) ...\n",window_mb,path);
        size_t chunk=1024*1024; void*fb; if(posix_memalign(&fb,4096,chunk)){perror("memalign");return 1;} memset(fb,0,chunk);
        for(off_t o=base;o<sz;o+=chunk)
            if(pwrite(fd,fb,chunk,o)!=(ssize_t)chunk){perror("pwrite");return 1;}
        fsync(fd); free(fb); printf("[fill] done\n");
    }

    uint64_t*ta=malloc(sizeof(*ta)*iters),*tb=malloc(sizeof(*tb)*iters);
    uint64_t*a1=malloc(sizeof(*a1)*iters),*a2=malloc(sizeof(*a2)*iters); // A 内部两次 4K 各自的耗时
    unsigned seed=0x9e3779b9;
    for(int i=0;i<100;i++){ long k=rand_r(&seed)%nblk; pread(fd,buf,4096,base+k*8192); } // warmup

    printf("[bench] A=%s | iters=%d | window=%ldMB\n",
        scatter?"2x4K 随机两块(双冷)":"2x4K 相邻覆盖8K区间", iters, window_mb);
    for(int i=0;i<iters;i++){
        long kA1=rand_r(&seed)%nblk;
        long kB =rand_r(&seed)%nblk;
        long kA2;
        if(scatter){ kA2=rand_r(&seed)%nblk; if(kA2==kA1) kA2=(kA2+1)%nblk; }
        else        kA2=kA1;                       // 相邻第二块就是同一 8K 区间后半
        off_t oA1=base+kA1*8192;
        off_t oA2=scatter? base+kA2*8192 : oA1+4096;
        off_t oB =base+kB*8192;
        uint64_t t0,t1,t2,t3;
        if(i&1){ // 交替顺序, 抵消漂移
            t0=now_ns(); if(pread(fd,buf,4096,oA1)!=4096){perror("preadA1");return 1;} t1=now_ns();
                        if(pread(fd,buf+4096,4096,oA2)!=4096){perror("preadA2");return 1;} t2=now_ns();
            ta[i]=t2-t0; a1[i]=t1-t0; a2[i]=t2-t1;
            t3=now_ns(); if(pread(fd,buf,8192,oB)!=8192){perror("preadB");return 1;} tb[i]=now_ns()-t3;
        } else {
            t0=now_ns(); if(pread(fd,buf,8192,oB)!=8192){perror("preadB");return 1;} tb[i]=now_ns()-t0;
            t0=now_ns(); if(pread(fd,buf,4096,oA1)!=4096){perror("preadA1");return 1;} t1=now_ns();
                        if(pread(fd,buf+4096,4096,oA2)!=4096){perror("preadA2");return 1;} t2=now_ns();
            ta[i]=t2-t0; a1[i]=t1-t0; a2[i]=t2-t1;
        }
    }
    printf("\n-- 事务级耗时 (每搬 8KB 数据) --\n");
    report(ta,iters,"A: 2x4K 阻塞读");
    report(tb,iters,"B: 1x8K 阻塞读");
    printf("\n-- 诊断: A 内部两次 4K 各自耗时 --\n");
    report(a1,iters,scatter?"A 第1次4K(冷)":"A 第1次4K(冷)");
    report(a2,iters,scatter?"A 第2次4K(随机冷)":"A 第2次4K(相邻)");
    double sa=0,sb=0; for(int i=0;i<iters;i++){sa+=ta[i];sb+=tb[i];} sa/=iters; sb/=iters;
    printf("\n平均事务: A=%.2fus  B=%.2fus\n单次8K 比 两次4K 快 %.2fx (省 %.1f%% 时间)\n",
        sa/1000.0,sb/1000.0,sa/sb,(1-sb/sa)*100);
    close(fd);
    return 0;
}

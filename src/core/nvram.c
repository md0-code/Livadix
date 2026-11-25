/* Livadix's own persistence format and file transactions.
 * Host API sources: Microsoft CreateFile (exclusive sharing), _commit,
 * MoveFileEx (same-volume replacement with WRITE_THROUGH); POSIX open,
 * flock, fsync and rename. No guest code or firmware is modified. */
#include "nvram.h"
#include "models.h"
#include "hfs.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#define mkdir_one(p) _mkdir(p)
#define sync_file(f) _commit(_fileno(f))
#define pid() _getpid()
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#define mkdir_one(p) mkdir(p,0700)
#define sync_file(f) fsync(fileno(f))
#define pid() getpid()
#endif
#define OWNER_MAX ((MC_CONFIG_DISKS + MC_CONFIG_FLOPPIES + 3) * MC_CONFIG_PATH + 1024)
#define RECORD_MAX (2 * 1024 * 1024)
struct mc_nvram {
    char target[MC_CONFIG_PATH], work[MC_CONFIG_PATH], owner[OWNER_MAX];
    unsigned size, rom;
    unsigned char *original;
    size_t original_size;
    uint32_t primary_crc; size_t primary_size; int primary_present, recovered;
#ifdef _WIN32
    HANDLE lock;
#else
    int lock, locked;     /* the descriptor of the lock file, and whether it holds the lock (Windows deletes the file on close) */
#endif
};
static uint32_t get32(const unsigned char *p) { return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
static void put32(unsigned char *p,uint32_t n) { p[0]=(unsigned char)(n>>24);p[1]=(unsigned char)(n>>16);p[2]=(unsigned char)(n>>8);p[3]=(unsigned char)n; }
static uint32_t crc32(const void *data,size_t n) {
    const unsigned char *p=data;uint32_t c=~0u;
    while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^((0u-(c&1u))&0xEDB88320u);}
    return ~c;
}
static uint64_t hash(const char *p) { uint64_t h=14695981039346656037ull;while(*p){h^=(unsigned char)*p++;h*=1099511628211ull;}return h; }
static int canonical(const char *p,char *out,size_t n) {
#ifdef _WIN32
    if(!_fullpath(out,p,n))return 0;
    for(char *q=out;*q;q++){if(*q=='\\')*q='/';if(*q>='A'&&*q<='Z')*q=(char)(*q+'a'-'A');}
#else
    if(p[0]=='/'){if(strlen(p)>=n)return 0;strcpy(out,p);}
    else {if(!getcwd(out,n))return 0;size_t k=strlen(out);if(k+1+strlen(p)>=n)return 0;out[k++]='/';strcpy(out+k,p);}
#endif
    return 1;
}
/* Host-side startup-media ownership. Fixed disks retain
 * their association through formatting and installer CD removal. Relative
 * image names keep a portable configuration folder's association intact. */
static int image_family(const char *path) {
    mc_volume_info v;char error[256];
    if(!path[0]||!mc_volume_inspect(path,&v,error,sizeof error))return 0;
    if(v.mac_os_x)return 2;
    return v.bootable||v.kind==MC_IMAGE_MFS?1:0;
}
static int media_family(const mc_config *c) {
    int f=0;
    if(c->boot_priority==MC_BOOT_CDROM&&(f=image_family(c->cdrom))!=0)return f;
    for(size_t i=0;i<c->disk_count;++i)f|=image_family(c->disks[i].image);
    if(f)return f;
    for(size_t i=0;i<c->floppy_count;++i)f|=image_family(c->floppies[i].image);
    return f?f:image_family(c->cdrom);
}
static int media_path(const mc_config *c,const char *image,char *out,size_t cap) {
    char path[MC_CONFIG_PATH],source[MC_CONFIG_PATH];
    if(!canonical(image,path,sizeof path))return 0;
    const char *name=path;
    if(c->source[0]&&canonical(c->source,source,sizeof source)){
        char *slash=strrchr(source,'/');
        if(slash){slash[1]=0;size_t n=strlen(source);if(!strncmp(path,source,n))name=path+n;}
    }
    int n=snprintf(out,cap,"%s:%s",name==path?"absolute":"relative",name);
    return n>=0&&(size_t)n<cap;
}
static int media_owner(const mc_config *c,char *owner,size_t cap,int family) {
    int present=c->disk_count!=0;
    if(!present){
        present=c->cdrom[0]!=0;
        for(size_t i=0;i<c->floppy_count;++i)present|=c->floppies[i].image[0]!=0;
    }
    if(!present)return 0;
    size_t used=strlen(owner);
    int n=snprintf(owner+used,cap-used,"\nstartup-media-v1:%d",family);
    if(n<0||(size_t)n>=cap-used)return -1;
    used+=(size_t)n;
    /* With a hard disk present, removable installer media is not part of
     * the fixed layout; an installer and its installed OS share state. */
    size_t count=c->disk_count?c->disk_count:c->floppy_count+1;
    for(size_t i=0;i<count;++i){
        const mc_disk_config *d=c->disk_count?&c->disks[i]:i<c->floppy_count?&c->floppies[i]:NULL;
        const char *image=d?d->image:c->cdrom;
        if(!image[0])continue;
        char path[MC_CONFIG_PATH+16];if(!media_path(c,image,path,sizeof path))return -1;
        n=snprintf(owner+used,cap-used,"\n%c%u,%d,%d,%d,%zu:%s",c->disk_count?'D':d?'F':'C',
                   (unsigned)i,d?d->bus:c->cdrom_bus,d?d->scsi_id_set:0,d?d->scsi_id:0,strlen(path),path);
        if(n<0||(size_t)n>=cap-used)return -1;
        used+=(size_t)n;
    }
    return 1;
}
/* Classification of retained pre-context records, not a guest-state edit.
 * The original Jaguar Startup Disk record has the $6666 startup selector
 * in PCI PRAM and an OF boot-command of "0 bootr". Its OF options begin
 * at $1800 with signature $1275; the command's offset/length pair is at
 * $1858 (original 78F57389 ROM/installed 10.2.8 observation).
 * Keep these OS X scripts out of a newly encountered Classic context. */
static int legacy_osx(const unsigned char *payload,unsigned size) {
    if(size!=8465u)return 0;
    const unsigned char *nv=payload+273;
    if(nv[0x137A]==0x66&&nv[0x137B]==0x66)return 1;
    if(nv[0x1800]!=0x12||nv[0x1801]!=0x75)return 0;
    unsigned at=(unsigned)nv[0x1858]<<8|nv[0x1859],n=(unsigned)nv[0x185A]<<8|nv[0x185B];
    if(at>8192u||n>8192u-at)return 0;
    while(n&&nv[at+n-1]==' ')--n;
    return n==7&&!memcmp(nv+at,"0 bootr",7);
}
static int parents(const char *p) {
    char b[MC_CONFIG_PATH];if(strlen(p)>=sizeof b)return 0;strcpy(b,p);
    for(char *q=b+1;*q;q++)if(*q=='/'||*q=='\\'){
        if(q==b+2&&b[1]==':')continue;
        char c=*q;*q=0;
#ifdef _WIN32
        DWORD attr=GetFileAttributesA(b);
        /* Another Livadix may create the folder between the test and the
         * creation: a folder there by then is as good. */
        int ok=(attr!=INVALID_FILE_ATTRIBUTES&&(attr&FILE_ATTRIBUTE_DIRECTORY))||mkdir_one(b)==0||
               (errno==EEXIST&&(attr=GetFileAttributesA(b))!=INVALID_FILE_ATTRIBUTES&&(attr&FILE_ATTRIBUTE_DIRECTORY));
#else
        int ok=mkdir_one(b)==0||errno==EEXIST;
#endif
        *q=c;if(!ok)return 0;
    }
    return 1;
}
static unsigned char *read_file(const char *path,size_t *n) {
    FILE *f=fopen(path,"rb");if(!f)return NULL;
    unsigned char *b=NULL;
    if(!fseek(f,0,SEEK_END)){
        long end=ftell(f);
        if(end>=0&&(unsigned long)end<=RECORD_MAX&&!fseek(f,0,SEEK_SET)){
            b=malloc((size_t)end+1);
            if(b&&fread(b,1,(size_t)end,f)==(size_t)end&&!ferror(f))*n=(size_t)end;
            else {free(b);b=NULL;}
        }
    }
    fclose(f);return b;
}
static int exists(const char *p) {
#ifdef _WIN32
    return GetFileAttributesA(p)!=INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;return stat(p,&st)==0;
#endif
}
static int valid(const mc_nvram *s,const unsigned char *b,size_t n) {
    size_t owner=strlen(s->owner),head=32+owner;
    if(n!=head+s->size||memcmp(b,"LVDXNV2\0",8)||get32(b+8)!=head||get32(b+12)!=s->size||
       get32(b+20)!=s->rom||get32(b+24)!=owner||memcmp(b+32,s->owner,owner))return 0;
    unsigned char h[32];memcpy(h,b,32);memset(h+28,0,4);
    /* Header fields and owner are covered together, separately from data. */
    unsigned char header[32+OWNER_MAX];memcpy(header,h,32);memcpy(header+32,b+32,owner);
    return get32(b+28)==crc32(header,head)&&get32(b+16)==crc32(b+head,s->size)&&
           !memcmp(b+head,"LVDXPRAM",8)&&b[head+8]==1;
}
static int write_sync(const char *p,const void *b,size_t n) {
    FILE *f=fopen(p,"wb");if(!f)return 0;
    int ok=fwrite(b,1,n,f)==n&&!fflush(f)&&!sync_file(f);
    if(fclose(f))ok=0;
    return ok;
}
static int replace_file(const char *from,const char *to) {
#ifdef _WIN32
    return MoveFileExA(from,to,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
    if(rename(from,to))return 0;
    char dir[MC_CONFIG_PATH];snprintf(dir,sizeof dir,"%s",to);char *p=strrchr(dir,'/');
    if(p){*p=0;int fd=open(dir,O_RDONLY);if(fd<0)return 0;int ok=fsync(fd)==0;close(fd);return ok;}
    return 1;
#endif
}
static void dispose(mc_nvram *s,int keep_work) {
    if(!s)return;
    if(!keep_work&&s->work[0])remove(s->work);
#ifdef _WIN32
    if(s->lock!=INVALID_HANDLE_VALUE)CloseHandle(s->lock);
#else
    if(s->lock>=0){
        if(s->locked){char lock[MC_CONFIG_PATH];snprintf(lock,sizeof lock,"%s.lock",s->target);unlink(lock);}
        close(s->lock);
    }
#endif
    free(s->original);free(s);
}
static int begin_store(const mc_config *config,const char *base,uint32_t rom,unsigned payload_size,
                       mc_nvram **out,char *working,size_t capacity,char *error,size_t error_capacity,int scoped) {
    *out=NULL;if(capacity)working[0]=0;
    if(!base||!*base||mc_model_builtin(mc_model_entry(config->model)))return 1;
    mc_config c=*config;mc_config_nvram_identity(&c);
    mc_nvram *s=calloc(1,sizeof *s);if(!s)goto failed;
#ifdef _WIN32
    s->lock=INVALID_HANDLE_VALUE;
#else
    s->lock=-1;
#endif
    s->size=payload_size;s->rom=rom;
    char absolute[MC_CONFIG_PATH];
    if(!canonical(base,absolute,sizeof absolute))goto failed;
    const char *base_name=strrchr(absolute,'/');base_name=base_name?base_name+1:absolute;
    int n=snprintf(s->owner,sizeof s->owner,"%s\n%s\n%s",mc_model_entry(c.model)->key,c.nvram_id,base_name);
    if(n<0||(size_t)n>=sizeof s->owner)goto failed;
    int family=scoped==2?1:scoped?media_family(&c):0;
    if(scoped&&!family)family=1; /* Classic and not-yet-identified media */
    int media=scoped?media_owner(&c,s->owner,sizeof s->owner,family):0;
    if(media<0)goto failed;
    const char *name=strrchr(absolute,'/');name=name?name+1:absolute;
    const char *dot=strrchr(name,'.');int stem=dot?(int)(dot-name):(int)strlen(name);if(stem>32)stem=32;
    char label[33];for(int j=0;j<stem;++j){char ch=name[j];label[j]=((ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||ch=='-'||ch=='_')?ch:'_';}label[stem]=0;
    n=snprintf(s->target,sizeof s->target,"%.*s%.*s-%s-%08X-%016llx.nvram",(int)(name-absolute),absolute,stem,label,
               mc_model_entry(c.model)->key,rom,(unsigned long long)hash(s->owner));
    if(n<0||(size_t)n+40>=sizeof s->target||!parents(s->target))goto failed;
    char lock[MC_CONFIG_PATH];snprintf(lock,sizeof lock,"%s.lock",s->target);
#ifdef _WIN32
    s->lock=CreateFileA(lock,GENERIC_READ|GENERIC_WRITE|DELETE,0,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,NULL);
    if(s->lock==INVALID_HANDLE_VALUE)goto locked;
#else
    s->lock=open(lock,O_RDWR|O_CREAT|O_CLOEXEC,0600);if(s->lock<0||flock(s->lock,LOCK_EX|LOCK_NB))goto locked;
    s->locked=1;
#endif
    if(exists(s->target)){
        s->primary_present=1;
        s->original=read_file(s->target,&s->original_size);
        if(s->original){s->primary_size=s->original_size;s->primary_crc=crc32(s->original,s->original_size);}
        if(s->original&&s->original_size>=32&&!memcmp(s->original,"LVDXNV2\0",8)){
            uint32_t owner=get32(s->original+24),head=get32(s->original+8);
            if(owner<OWNER_MAX&&head==32+owner&&head<=s->original_size){
                unsigned char header[32+OWNER_MAX];memcpy(header,s->original,head);memset(header+28,0,4);
                if(get32(s->original+28)==crc32(header,head)&&
                   (get32(s->original+20)!=s->rom||owner!=strlen(s->owner)||memcmp(s->original+32,s->owner,owner))){
                    snprintf(error,error_capacity,"NVRAM belongs to another profile/model/ROM; preserved %s",s->target);dispose(s,0);return 0;
                }
            }
        }
        if(!s->original||!valid(s,s->original,s->original_size)){
            s->recovered=1;
            char backup[MC_CONFIG_PATH];snprintf(backup,sizeof backup,"%s.bak",s->target);
            free(s->original);s->original=read_file(backup,&s->original_size);
            if(!s->original||!valid(s,s->original,s->original_size)){
                snprintf(error,error_capacity,"NVRAM is damaged or belongs to another profile/model/ROM; preserved %s",s->target);dispose(s,0);return 0;
            }
        }
    }
    snprintf(s->work,sizeof s->work,"%s.work-%u",s->target,(unsigned)pid());
    if(exists(s->work)){
        snprintf(error,error_capacity,"An earlier NVRAM session remains at %s; preserve or recover it before starting",s->work);dispose(s,1);return 0;
    }
    /* Empty working files are absent: the board initializes cold NVRAM. */
    if(s->original&&!write_sync(s->work,s->original+32+strlen(s->owner),s->size))goto failed;
    if(!s->original&&media&&scoped==1){
        mc_nvram *seed=NULL;char seed_work[MC_CONFIG_PATH];
        int seeded=0,ok=1;
        /* A blank/unknown disk can become an OS X disk during installation.
         * Carry forward its installer-written scripts only when present. */
        if(family==2){
            if(!begin_store(config,base,rom,payload_size,&seed,seed_work,sizeof seed_work,error,error_capacity,2)){
                dispose(s,0);return 0;
            }
            if(seed&&seed->original){
                const unsigned char *data=seed->original+32+strlen(seed->owner);
                if(legacy_osx(data,payload_size)){ok=write_sync(s->work,data,s->size);seeded=1;}
            }
            dispose(seed,0);seed=NULL;
        }
        /* Existing owned records are read and validated under their own
         * lock. They are never renamed, rewritten or deleted by seeding. */
        if(ok&&!seeded){
            if(!begin_store(config,base,rom,payload_size,&seed,seed_work,sizeof seed_work,error,error_capacity,0)){
                dispose(s,0);return 0;
            }
            if(seed&&seed->original){
                const unsigned char *data=seed->original+32+strlen(seed->owner);
                if(family==3||legacy_osx(data,payload_size)==(family==2))ok=write_sync(s->work,data,s->size);
            }
            dispose(seed,0);
        }
        if(!ok)goto failed;
    }
    if(strlen(s->work)>=capacity)goto failed;
    strcpy(working,s->work);*out=s;return 1;
locked:
    snprintf(error,error_capacity,"NVRAM is already in use or cannot be locked: %s",s->target);dispose(s,0);return 0;
failed:
    snprintf(error,error_capacity,"Cannot prepare private NVRAM storage for this profile (path, permissions or disk space)");dispose(s,0);return 0;
}
int mc_nvram_begin(const mc_config *config,const char *base,uint32_t rom,unsigned payload_size,
                   mc_nvram **out,char *working,size_t capacity,char *error,size_t error_capacity) {
    return begin_store(config,base,rom,payload_size,out,working,capacity,error,error_capacity,1);
}
const char *mc_nvram_target(const mc_nvram *s) { return s?s->target:""; }
int mc_nvram_end(mc_nvram *s,int commit,char *error,size_t capacity) {
    if(!s)return 1;
    if(!commit){dispose(s,0);return 1;}
    size_t current_size=0;unsigned char *current=read_file(s->target,&current_size);
    int unchanged=s->primary_present?(current&&current_size==s->primary_size&&crc32(current,current_size)==s->primary_crc):!exists(s->target);
    int preserved=1;
    if(unchanged&&s->recovered&&current){
        char bad[MC_CONFIG_PATH];snprintf(bad,sizeof bad,"%s.corrupt-%08X",s->target,s->primary_crc);
        if(!exists(bad)&&!write_sync(bad,current,current_size))preserved=0;
    }
    free(current);
    if(!preserved){snprintf(error,capacity,"Cannot preserve the damaged NVRAM file; existing records retained. Recovery: %s",s->work);dispose(s,1);return 0;}
    if(!unchanged){snprintf(error,capacity,"NVRAM changed outside this session; existing file preserved. Recovery: %s",s->work);dispose(s,1);return 0;}
    size_t n=0;unsigned char *payload=read_file(s->work,&n);
    if(!payload||n!=s->size||memcmp(payload,"LVDXPRAM",8)||payload[8]!=1)goto failed;
    size_t owner=strlen(s->owner),head=32+owner,total=head+n;
    unsigned char *record=calloc(1,total);if(!record)goto failed;
    memcpy(record,"LVDXNV2\0",8);put32(record+8,(uint32_t)head);put32(record+12,(uint32_t)n);
    put32(record+16,crc32(payload,n));put32(record+20,s->rom);put32(record+24,(uint32_t)owner);
    memcpy(record+32,s->owner,owner);put32(record+28,crc32(record,head));memcpy(record+head,payload,n);free(payload);payload=NULL;
    char temp[MC_CONFIG_PATH],backup[MC_CONFIG_PATH],backup_temp[MC_CONFIG_PATH];
    snprintf(temp,sizeof temp,"%s.new-%u",s->target,(unsigned)pid());
    snprintf(backup,sizeof backup,"%s.bak",s->target);snprintf(backup_temp,sizeof backup_temp,"%s.bak-new-%u",s->target,(unsigned)pid());
    if(exists(temp)||exists(backup_temp)){free(record);goto failed;}
    int ok=write_sync(temp,record,total);free(record);
    /* The previous validated record is backed up before the replacement.
     * A fallback load never promotes a corrupt primary into the backup. */
    if(ok&&s->original)ok=write_sync(backup_temp,s->original,s->original_size)&&replace_file(backup_temp,backup);
    if(ok)ok=replace_file(temp,s->target);
    remove(temp);remove(backup_temp);
    if(ok){dispose(s,0);return 1;}
failed:
    free(payload);
    snprintf(error,capacity,"NVRAM was not saved; the previous record is preserved. Session data: %s",s->work);
    dispose(s,1);return 0;
}
int mc_nvram_import(const mc_config *c,const char *base,uint32_t rom,unsigned size,
                    const char *legacy,char *error,size_t capacity) {
    mc_nvram *s=NULL;char work[MC_CONFIG_PATH];
    if(!mc_nvram_begin(c,base,rom,size,&s,work,sizeof work,error,capacity)||!s)return 0;
    if(s->original){snprintf(error,capacity,"Owned NVRAM already exists; import refused");dispose(s,0);return 0;}
    size_t n=0;unsigned char *b=read_file(legacy,&n);
    int ok=b&&n==size&&!memcmp(b,"LVDXPRAM",8)&&b[8]==1&&write_sync(work,b,n);free(b);
    if(!ok){snprintf(error,capacity,"Legacy NVRAM has an invalid size or record; import refused");dispose(s,0);return 0;}
    return mc_nvram_end(s,1,error,capacity);
}

int64_t mc_nvram_clock_offset(const uint8_t *p) {
    uint64_t n=0;for(int i=7;i>=0;--i)n=(n<<8)|p[i];
    return n<=INT64_MAX?(int64_t)n:-1-(int64_t)~n;
}

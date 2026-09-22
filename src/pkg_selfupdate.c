/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#if !defined(_WIN32) && !defined(__AROS__)
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "pkg_selfupdate.h"
#include "pkg_fs.h"
#include "pkg_sha256.h"
#include "pkg_sha512.h"
#include "pkg_ed25519.h"
#include "pkg_manifest.h"
#include "pkg_activity.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#ifndef __AROS__
#include <sys/wait.h>
#endif
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef __AROS__
#include <proto/dos.h>
#endif
#ifndef PKG_SELFUPDATE_CHANNEL
#define PKG_SELFUPDATE_CHANNEL "https://aros-pkg.azurewebsites.net/pkg"
#endif
#define OFFICIAL PKG_SELFUPDATE_CHANNEL
#ifndef PKG_SELFUPDATE_KEY
#define PKG_SELFUPDATE_KEY "43c550967bc18dfec7cf3a7cd01297d09450fd364a0e34d8e58aef623cef3077"
#endif
#define PUBKEY PKG_SELFUPDATE_KEY
#define PATHCAP 4096

/* SSH strings are length prefixed, with exact bounds at each nesting level. */
struct bytes { const unsigned char *p; size_t n; };
static int take(struct bytes *b, struct bytes *s)
{
    size_t n;
    if (b->n < 4) return -1;
    n = ((size_t)b->p[0]<<24)|((size_t)b->p[1]<<16)|((size_t)b->p[2]<<8)|b->p[3];
    if (n > b->n - 4) return -1;
    s->p = b->p + 4; s->n = n; b->p += n+4; b->n -= n+4;
    return 0;
}
static int equal(struct bytes s, const char *v)
{ return s.n == strlen(v) && !memcmp(s.p, v, s.n); }
int pkg_selfupdate_verify(const void *message, size_t length,
                          const char *armor, const unsigned char key[32])
{
    unsigned char decoded[1024], hash[64], signed_data[256];
    struct bytes b, pub, ns, reserved, alg, sig, type, value, pk;
    const char *p, *end, *v; size_t n=0, sn=6; unsigned acc=0, bits=0;
    const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!armor || strncmp(armor,"-----BEGIN SSH SIGNATURE-----\n",29)) return -1;
    p=armor+29; end=strstr(p,"-----END SSH SIGNATURE-----");
    if (!end) return -1;
    for (;p<end;p++) {
        if (*p=='\n'||*p=='\r') continue;
        if (*p=='=') { for (;p<end;p++) if (*p!='='&&*p!='\n'&&*p!='\r') return -1; break; }
        v=strchr(alphabet,*p); if (!v) return -1;
        acc=(acc<<6)|(unsigned)(v-alphabet); bits+=6;
        if(bits>=8) { bits-=8; if(n==sizeof decoded) return -1; decoded[n++]=(unsigned char)(acc>>bits); }
    }
    if(n<10||memcmp(decoded,"SSHSIG\0\0\0\1",10)) return -1;
    b.p=decoded+10; b.n=n-10;
    if(take(&b,&pub)||take(&b,&ns)||take(&b,&reserved)||take(&b,&alg)||take(&b,&sig)||b.n) return -1;
    if(!equal(ns,"aros-pkg-bootstrap")||reserved.n||!equal(alg,"sha512")) return -1;
    if(take(&pub,&type)||!equal(type,"ssh-ed25519")||take(&pub,&pk)||pub.n||pk.n!=32||memcmp(pk.p,key,32)) return -1;
    if(take(&sig,&type)||!equal(type,"ssh-ed25519")||take(&sig,&value)||sig.n||value.n!=64) return -1;
    pkg_sha512(hash,message,length);
    memcpy(signed_data,"SSHSIG",6);
#define PUT(ptr,len) do { size_t z=(len); signed_data[sn++]=0; signed_data[sn++]=0; signed_data[sn++]=0; signed_data[sn++]=(unsigned char)z; memcpy(signed_data+sn,(ptr),z); sn+=z; } while(0)
    PUT(ns.p,ns.n); PUT("",0); PUT("sha512",6); PUT(hash,64);
#undef PUT
    return pkg_ed25519_verify(value.p,signed_data,sn,key);
}
static void report(const struct pkg_sink *s, const char *key, const char *value, int error)
{
    char line[PATHCAP+256];
    if(s->structured) { if(s->record) s->record(s->user,key,value); return; }
    if(error) snprintf(line,sizeof line,"%s",value);
    else snprintf(line,sizeof line,"%s: %s",key,value);
    if(s->line) s->line(s->user,error?PKG_LINE_REFUSAL:PKG_LINE_DETAIL,error,line);
    else if(s->text) { size_t n=strlen(line); line[n]='\n'; line[n+1]=0; s->text(s->user,error,line); }
}
/* A question waiting for an answer on the terminal: the one line a person
 * must read before typing. In MACHINE mode it is a record, and nothing is
 * asked: the caller never reaches here without a terminal. Only the systems
 * with a sudo to offer ask anything: not Windows, not AROS. */
#if !defined(_WIN32) && !defined(__AROS__)
static void ask(const struct pkg_sink *s, const char *text)
{
    if(s->structured) { if(s->record) s->record(s->user,"question",text); return; }
    if(s->line) s->line(s->user,PKG_LINE_QUESTION,0,text);
    else if(s->text) { char line[PATHCAP+256]; snprintf(line,sizeof line,"%s ",text); s->text(s->user,0,line); }
}
#endif
/* The outcome, said as a result: the line a person reads to know what
 * happened. MACHINE gets `result: <word>` and the sentence as `summary`.
 * AROS updates pkg as a package, through pkg_upgrade, and never gets here. */
#ifndef __AROS__
static void done_line(const struct pkg_sink *s, const char *word, const char *fmt, ...)
{
    /* Declining is neither a success nor a failure: plain ink, no mark. */
    int kind=strcmp(word,"declined")?PKG_LINE_RESULT:PKG_LINE_TEXT;
    char line[PATHCAP+256]; va_list ap;
    va_start(ap,fmt); vsnprintf(line,sizeof line,fmt,ap); va_end(ap);
    if(s->structured) { if(s->record) { s->record(s->user,"result",word); s->record(s->user,"summary",line); } return; }
    if(s->line) s->line(s->user,kind,0,line);
    else if(s->text) { size_t n=strlen(line); line[n]='\n'; line[n+1]=0; s->text(s->user,0,line); }
}
#endif
static int failure(const struct pkg_sink *s, int code, const char *text)
{ report(s,"self-update",text,1); return code; }
static int executable(char out[PATHCAP])
{
#ifdef _WIN32
    WCHAR w[PATHCAP]; DWORD n=GetModuleFileNameW(NULL,w,PATHCAP);
    if(!n||n>=PATHCAP) return -1;
    if(!WideCharToMultiByte(CP_UTF8,0,w,-1,out,PATHCAP,NULL,NULL)) return -1;
    { size_t i; for(i=0;out[i];i++) if(out[i]=='\\') out[i]='/'; }
    return 0;
#elif defined(__AROS__)
    char name[PATHCAP];
    if(!NameFromLock(GetProgramDir(),(STRPTR)out,PATHCAP)||!GetProgramName((STRPTR)name,PATHCAP)) return -1;
    { const char *base=strrchr(name,'/'), *colon=strrchr(name,':');
      if(!base||(colon&&colon>base)) base=colon;
      return AddPart((STRPTR)out,(STRPTR)(base?base+1:name),PATHCAP)?0:-1; }
#elif defined(__APPLE__)
    char raw[PATHCAP]; uint32_t n=sizeof raw;
    return _NSGetExecutablePath(raw,&n)==0&&realpath(raw,out)?0:-1;
#elif defined(__linux__)
    ssize_t n=readlink("/proc/self/exe",out,PATHCAP-1);
    if(n<0||n>=PATHCAP-1) return -1;
    out[n]=0; return strstr(out," (deleted)")?-1:0;
#else
    (void)out; return -1;
#endif
}
/* Find the owning record before considering a standalone binary replacement.
 * This keeps managed roots' manifests, history and pinned keys coherent. */
static int managed_root(const char *target,char root[PATHCAP])
{
    char *sep,*db,*path,error[256]; unsigned char *data; size_t len,i;
    struct pkg_manifest m;
    strcpy(root,target);
    for(;;) {
        sep=strrchr(root,'/');
#ifdef _WIN32
        { char *back=strrchr(root,'\\'); if(back&&(!sep||back>sep)) sep=back; }
#endif
#ifdef __AROS__
        if(!sep) { sep=strrchr(root,':'); if(sep&&sep[1]==0) return 0; }
#endif
        if(!sep) return 0;
        if(*sep==':') sep[1]=0; else if(sep==root) sep[1]=0;
#ifdef _WIN32
        else if(sep==root+2&&root[1]==':') sep[1]=0;
#endif
        else *sep=0;
        db=pkg_join(root,".pkg/db/pkg"); if(!db) return -1;
        if(pkg_fs_exists(db)) {
            if(pkg_fs_read(db,&data,&len)) { free(db); return -1; }
            pkg_manifest_init(&m);
            if(pkg_manifest_parse((char *)data,len,&m,error,sizeof error)) { free(data); free(db); pkg_manifest_free(&m); return -1; }
            free(data);
            for(i=0;i<m.nfiles;i++) {
                int match; path=pkg_join(root,m.files[i].path);
                if(!path) { free(db); pkg_manifest_free(&m); return -1; }
#if defined(__AROS__) || defined(_WIN32)
                { size_t j; for(j=0;path[j]&&target[j]&&tolower((unsigned char)path[j])==tolower((unsigned char)target[j]);j++) {} match=path[j]==target[j]; }
#else
                match=!strcmp(path,target);
#endif
                free(path);
                if(match) { free(db); pkg_manifest_free(&m); return 1; }
            }
            pkg_manifest_free(&m);
        }
        free(db);
        if(sep==root) return 0;
#ifdef _WIN32
        if(sep==root+2&&root[1]==':') return 0;
#endif
    }
}
#ifndef __AROS__
static const char *platform(void)
{
#ifdef _WIN32
    return "windows-x86_64";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__APPLE__) && defined(__x86_64__)
    return "macos-x86_64";
#elif defined(__linux__) && defined(__aarch64__)
    return "linux-arm64";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x86_64";
#else
    return NULL;
#endif
}
static void progress(void *ctx,const char *text)
{
    const struct pkg_sink *s=ctx;
    if(s->line) s->line(s->user,PKG_LINE_PROGRESS,0,text);
    else if(s->text) { char line[512]; snprintf(line,sizeof line,"\r%s",text); s->text(s->user,0,line); }
}
static int get(const struct pkg_sink *sink,const char *rel,const char *dest)
{
    char url[512],error[512]; int rc;
    snprintf(url,sizeof url,"%s/%s",OFFICIAL,rel);
    report(sink,"download",url,0);
    if(sink->progress&&!sink->structured) {
        pkg_activity_to(progress,NULL,(void *)sink);
        pkg_fs_on_transfer=pkg_activity_bytes; pkg_fs_on_wait=pkg_activity_waiting; pkg_fs_on_tick=pkg_activity_tick;
    }
    pkg_activity_step("Downloading",rel,0,PKG_ACTIVITY_BYTES,NULL);
    rc=pkg_net_get(url,dest,error,sizeof error); pkg_activity_done();
    pkg_activity_to(NULL,NULL,NULL);
    pkg_fs_on_transfer=NULL; pkg_fs_on_wait=NULL; pkg_fs_on_tick=NULL;
    if(rc) report(sink,"download",error,1);
    return rc;
}
static int checksum(const char *sums,const char *path,char digest[65])
{
    const char *p=sums,*end; int found=0; size_t i;
    while(*p) {
        end=strchr(p,'\n'); if(!end) end=p+strlen(p);
        if((size_t)(end-p)==66+strlen(path)&&p[64]==' '&&p[65]==' '&&!memcmp(p+66,path,strlen(path))) {
            if(found++) return -1;
            for(i=0;i<64;i++) if(!isxdigit((unsigned char)p[i])) return -1;
            memcpy(digest,p,64); digest[64]=0;
        }
        p=*end?end+1:end;
    }
    return found?0:-1;
}
static int version(const unsigned char *data,size_t len,char out[100])
{
    const char marker[]="$VER: pkg "; size_t i,j;
    for(i=0;i+sizeof marker<len;i++) if(!memcmp(data+i,marker,sizeof marker-1)) {
        for(j=0;j<99&&i+sizeof marker-1+j<len;j++) {
            unsigned char c=data[i+sizeof marker-1+j];
            if(c==' '&&j) { out[j]=0; return 0; }
            if(!(isdigit(c)||c=='.'||c=='+')) break;
            out[j]=(char)c;
        }
    }
    return -1;
}
#endif
int pkg_selfupdate(const struct pkg_sink *sink,int dryrun)
{
    char target[PATHCAP];
    if(executable(target)) return failure(sink,17,"cannot determine the running executable's path");
    report(sink,"executable",target,0);
    report(sink,"channel",OFFICIAL,0);
    report(sink,"signer",PUBKEY,0);
    {
        char root[PATHCAP]; int managed=managed_root(target,root);
        if(managed<0) return failure(sink,17,"cannot read the executable's package record; repair its managed root first");
        if(managed) {
            struct pkg_options o;
            memset(&o,0,sizeof o); o.target="pkg"; o.root=root; o.channel=OFFICIAL; o.dryrun=dryrun;
            report(sink,"root",root,0); report(sink,"root-source","installed package record owning the running executable",0);
            return pkg_upgrade(sink,&o);
        }
    }
#ifdef __AROS__
    return failure(sink,17,"this executable has no managed package record; install it with Install-Pkg first, or specify ROOT explicitly");
#else
    {
        const char *plat=platform(); char dir[PATHCAP],stage[PATHCAP],sumsfile[PATHCAP],sigfile[PATHCAP],newfile[PATHCAP];
        char rel[128],want[65],got[65],offered[100],*slash;
        unsigned char *sums=NULL,*sig=NULL,*binary=NULL,pk[32]; size_t sl=0,sg=0,bl=0,i;
        int rc=17; int stage_created=0; int said=0;
#ifndef _WIN32
        struct stat st; int need_admin=0;
#endif
        if(!plat) return failure(sink,17,"no official bootstrap exists for this platform");
        #ifndef _WIN32
        if(stat(target,&st)) return failure(sink,17,"cannot inspect the running executable");
#else
        { WCHAR wt[PATHCAP];
          if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,target,-1,wt,PATHCAP)||GetFileAttributesW(wt)==INVALID_FILE_ATTRIBUTES) return failure(sink,17,"cannot inspect the running executable"); }
#endif
        strcpy(dir,target); slash=strrchr(dir,'/');
#ifdef _WIN32
        { char *back=strrchr(dir,'\\'); if(back&&(!slash||back>slash)) slash=back; }
#endif
        if(!slash) return failure(sink,17,"the executable path has no parent directory");
        *slash=0;
#ifndef _WIN32
        /* Whether replacing the executable will need administrator access is
         * known now, but asked only once there is something to replace: a
         * person who already has the newest pkg is never asked for a
         * password, and one who is asked knows what for. Until then the
         * check is staged in /tmp, as DRYRUN's is. */
        need_admin=geteuid()!=0&&(st.st_uid!=geteuid()||access(dir,W_OK));
        if(st.st_mode&(S_ISUID|S_ISGID)) return failure(sink,17,"self-update refuses set-user-ID or set-group-ID executables");
        if(dryrun||need_admin) snprintf(stage,sizeof stage,"/tmp/pkg-selfupdate.XXXXXX");
        else if(snprintf(stage,sizeof stage,"%s/.pkg-selfupdate.XXXXXX",dir)>=(int)sizeof stage) return failure(sink,17,"executable path is too long");
        if(!mkdtemp(stage)) return failure(sink,17,"cannot create a private staging directory beside the executable");
#else
        {
            unsigned char random[12]; char hex[25]; WCHAR ws[PATHCAP];
            if(dryrun) {
                WCHAR temp[PATHCAP]; DWORD count=GetTempPathW(PATHCAP,temp);
                if(!count||count>=PATHCAP||!WideCharToMultiByte(CP_UTF8,0,temp,-1,dir,PATHCAP,NULL,NULL)) return failure(sink,17,"cannot locate the temporary directory");
            }
            if(pkg_fs_random(random,sizeof random)) return failure(sink,17,"cannot create staging name");
            for(i=0;i<12;i++) snprintf(hex+2*i,3,"%02x",random[i]);
            if(snprintf(stage,sizeof stage,"%s/.pkg-selfupdate-%s",dir,hex)>=(int)sizeof stage||!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,stage,-1,ws,PATHCAP)||!CreateDirectoryW(ws,NULL)) return failure(sink,17,"cannot create staging beside executable; run from an administrator terminal if needed");
        }
#endif
        stage_created=1;
        if(snprintf(sumsfile,sizeof sumsfile,"%s/sums",stage)>=(int)sizeof sumsfile||snprintf(sigfile,sizeof sigfile,"%s/sig",stage)>=(int)sizeof sigfile||snprintf(newfile,sizeof newfile,"%s/pkg",stage)>=(int)sizeof newfile) goto done;
        if(get(sink,"Bootstrap/SHA256SUMS",sumsfile)||get(sink,"Bootstrap/SHA256SUMS.sig",sigfile)) { rc=16; goto done; }
        if(pkg_fs_read(sumsfile,&sums,&sl)||pkg_fs_read(sigfile,&sig,&sg)||sl>1024*1024||sg>8192||memchr(sums,0,sl)||memchr(sig,0,sg)) goto done;
        for(i=0;i<32;i++) { unsigned x; if(sscanf(&PUBKEY[2*i],"%2x",&x)!=1) goto done; pk[i]=(unsigned char)x; }
        if(pkg_selfupdate_verify(sums,sl,(char *)sig,pk)) { rc=13; failure(sink,rc,"bootstrap signature verification failed; executable unchanged"); goto done; }
        snprintf(rel,sizeof rel,"Bootstrap/%s/%s",plat,
#ifdef _WIN32
                 "pkg.exe"
#else
                 "pkg"
#endif
        );
        if(checksum((char *)sums,rel,want)) { rc=13; failure(sink,rc,"signed checksums do not uniquely identify this platform"); goto done; }
        if(get(sink,rel,newfile)) { rc=16; goto done; }
        if(pkg_fs_read(newfile,&binary,&bl)) goto done;
        pkg_sha256_hex(binary,bl,got);
        if(strcmp(want,got)) { rc=13; failure(sink,rc,"download does not match its signed checksum; executable unchanged"); goto done; }
        if(version(binary,bl,offered)) { rc=13; failure(sink,rc,"signed executable has no readable pkg version"); goto done; }
        report(sink,"installed",PKG_VERSION_STRING,0); report(sink,"offered",offered,0);
        if(pkg_version_cmp(offered,PKG_VERSION_STRING)<=0) { done_line(sink,"up-to-date","pkg is up to date: %s is the newest the channel offers",PKG_VERSION_STRING); rc=0; goto done; }
        if(dryrun) { report(sink,"result","verified update available; DRYRUN leaves the executable unchanged",0); rc=0; goto done; }
#ifndef _WIN32
        if(need_admin) {
            if(!sink->structured&&isatty(STDIN_FILENO)&&isatty(STDOUT_FILENO)) {
                char answer[16],q[PATHCAP+200]; pid_t child; int status;
                snprintf(q,sizeof q,"pkg %s is offered; this is %s. Replacing %s needs administrator access: run this same executable with sudo? [y/N]",offered,PKG_VERSION_STRING,target);
                ask(sink,q);
                if(!fgets(answer,sizeof answer,stdin)||tolower((unsigned char)answer[0])!='y') {
                    /* A choice, not a failure: nothing was wrong, and the
                     * sentence says what stays on offer. */
                    done_line(sink,"declined","not updated: %s stays offered, and pkg U installs it when you want it",offered);
                    rc=0; goto done;
                }
                child=fork();
                if(child==0) { execl("/usr/bin/sudo","sudo","--",target,"UPGRADE",(char *)NULL); _exit(127); }
                if(child<0||waitpid(child,&status,0)<0) { failure(sink,17,"could not start the update as administrator"); rc=17; said=1; goto done; }
                /* sudo's own pkg said what happened, success or not */
                rc=WIFEXITED(status)?WEXITSTATUS(status):17; said=1; goto done;
            }
            failure(sink,17,"a newer pkg is offered, and replacing this one needs administrator access: run the executable shown above with sudo and UPGRADE");
            rc=17; said=1; goto done;
        }
#endif
        if(sink->cancel&&sink->cancel(sink->user)) { rc=10; goto done; }
#ifndef _WIN32
        {
            struct stat after;
            if(stat(target,&after)||after.st_dev!=st.st_dev||after.st_ino!=st.st_ino) { failure(sink,17,"executable changed during download; retry the update"); goto done; }
            if((geteuid()==0&&chown(newfile,st.st_uid,st.st_gid))||chmod(newfile,st.st_mode&0777)||rename(newfile,target)) { failure(sink,17,"cannot replace executable; original is unchanged"); goto done; }
        }
#else
        {
            char backup[PATHCAP]; WCHAR wt[PATHCAP],wn[PATHCAP],wb[PATHCAP];
            if(snprintf(backup,sizeof backup,"%s/previous.exe",stage)>=(int)sizeof backup) goto done;
            if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,target,-1,wt,PATHCAP)||!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,newfile,-1,wn,PATHCAP)||!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,backup,-1,wb,PATHCAP)) goto done;
            if(!MoveFileExW(wt,wb,MOVEFILE_WRITE_THROUGH)) { failure(sink,17,"cannot rename the running executable; close other users or use an administrator terminal"); goto done; }
            if(!MoveFileExW(wn,wt,MOVEFILE_WRITE_THROUGH)) {
                if(!MoveFileExW(wb,wt,MOVEFILE_WRITE_THROUGH)) {
                    stage_created=0; report(sink,"previous-executable",backup,1);
                    failure(sink,17,"replacement and restore failed; move the preserved previous executable back to the target");
                } else failure(sink,17,"could not install replacement; previous executable restored");
                goto done;
            }
            /* Windows retains the running image until exit. Keep its unique
             * backup drawer; the result records its location for cleanup. */
            stage_created=0; report(sink,"previous-executable",backup,0);
        }
#endif
        report(sink,"result","pkg updated; the next invocation uses the new executable",0); rc=0;
    done:
        free(sums); free(sig); free(binary);
        if(stage_created) pkg_fs_rmtree(stage);
        if(rc==17&&!said) failure(sink,rc,"self-update could not complete");
        return rc;
    }
#endif
}

static int remove_failure(const struct pkg_sink *sink,const char *message)
{
    report(sink,"self-remove",message,1);
    return 17;
}

#ifdef _WIN32
static int remove_windows(const struct pkg_sink *sink,const char *target,const char *root)
    {
        unsigned char random[12]; char hex[25],retired[PATHCAP]; size_t i;
        WCHAR wt[PATHCAP],wr[PATHCAP];
        if(pkg_fs_random(random,sizeof random)) return remove_failure(sink,"cannot generate a private retirement filename");
        for(i=0;i<12;i++) snprintf(hex+2*i,3,"%02x",random[i]);
        if(snprintf(retired,sizeof retired,"%s.removed-%s",target,hex)>=(int)sizeof retired||
           !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,target,-1,wt,PATHCAP)||
           !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,retired,-1,wr,PATHCAP))
            return remove_failure(sink,"cannot encode the executable retirement path");
        if(!MoveFileExW(wt,wr,MOVEFILE_WRITE_THROUGH))
            return remove_failure(sink,"cannot remove executable from its installed path; close other instances or use an administrator terminal");
        if(root) {
            struct pkg_options o; int rc;
            memset(&o,0,sizeof o); o.target="pkg"; o.root=root;
            rc=pkg_remove(sink,&o);
            if(rc) {
                if(!MoveFileExW(wr,wt,MOVEFILE_WRITE_THROUGH)) report(sink,"retired-executable",retired,1);
                return rc;
            }
        }
        if(MoveFileExW(wr,NULL,MOVEFILE_DELAY_UNTIL_REBOOT))
            report(sink,"result","pkg removed from its installed path; Windows will delete the running image at reboot",0);
        else {
            report(sink,"retired-executable",retired,0);
            report(sink,"result","pkg removed from its installed path; delete the retired executable after this process exits",0);
        }
        return 0;
    }
#endif

int pkg_selfremove(const struct pkg_sink *sink,int dryrun)
{
    char target[PATHCAP],root[PATHCAP]; int managed;
#ifndef _WIN32
    struct stat before,after;
#endif
    if(executable(target)) return remove_failure(sink,"cannot determine the running executable's path");
    report(sink,"executable",target,0);
    report(sink,"operation","remove the running pkg installation",0);
#ifndef _WIN32
    if(lstat(target,&before)||!S_ISREG(before.st_mode)) return remove_failure(sink,"running executable is no longer a regular file at this path");
#ifndef __AROS__
    {
        char dir[PATHCAP],*slash;
        strcpy(dir,target); slash=strrchr(dir,'/');
        if(!slash) return remove_failure(sink,"executable path has no parent directory");
        if(slash==dir) slash[1]=0; else *slash=0;
        if(!dryrun&&geteuid()!=0&&(before.st_uid!=geteuid()||access(dir,W_OK))) {
            if(!sink->structured&&isatty(STDIN_FILENO)&&isatty(STDOUT_FILENO)) {
                char answer[16]; pid_t child; int status;
                ask(sink,"administrator access is required. Run this same executable with sudo REMOVE? [y/N]");
                if(!fgets(answer,sizeof answer,stdin)||tolower((unsigned char)answer[0])!='y') return remove_failure(sink,"removal cancelled; executable unchanged");
                child=fork();
                if(child==0) { execl("/usr/bin/sudo","sudo","--",target,"REMOVE",(char *)NULL); _exit(127); }
                if(child<0||waitpid(child,&status,0)<0) return remove_failure(sink,"could not start administrator removal");
                return WIFEXITED(status)?WEXITSTATUS(status):17;
            }
            return remove_failure(sink,"administrator access required; run sudo with the executable shown above and REMOVE");
        }
    }
#endif
#endif
    managed=managed_root(target,root);
    if(managed<0) return remove_failure(sink,"cannot read the executable's package record; repair its managed root first");
    if(managed) {
        struct pkg_options o;
        memset(&o,0,sizeof o); o.target="pkg"; o.root=root; o.dryrun=dryrun;
        report(sink,"root",root,0);
        report(sink,"root-source","installed package record owning the running executable",0);
#ifdef _WIN32
        if(!dryrun) {
            int rc;
            o.dryrun=1;
            rc=pkg_remove(sink,&o);
            if(rc) return rc;
            return remove_windows(sink,target,root);
        }
#endif
        return pkg_remove(sink,&o);
    }
    if(dryrun) { report(sink,"result","DRYRUN would remove the executable shown above; configuration and other programs are kept",0); return 0; }
    if(sink->cancel&&sink->cancel(sink->user)) return 10;
#ifdef _WIN32
    return remove_windows(sink,target,NULL);
#else
    if(lstat(target,&after)||after.st_dev!=before.st_dev||after.st_ino!=before.st_ino||!S_ISREG(after.st_mode))
        return remove_failure(sink,"executable changed during removal; no file was removed");
    if(unlink(target)) return remove_failure(sink,"cannot remove the executable; configuration and other programs are unchanged");
    report(sink,"result","pkg executable removed; configuration and other programs are kept",0);
    return 0;
#endif
}

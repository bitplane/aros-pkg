/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#endif
#include "pkg_environment.h"
#include "pkg_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#endif
#define ENV_MAX 65536
static int fail(char *err, size_t n, const char *msg) { if (err && n) snprintf(err,n,"%s",msg); return -1; }
static char *dupstr(const char *s) { size_t n; char *p; if (!s) return NULL; n=strlen(s)+1; p=malloc(n); if(p) memcpy(p,s,n); return p; }
void pkg_environments_init(struct pkg_environments *e) { memset(e,0,sizeof *e); }
void pkg_environments_free(struct pkg_environments *e) {
    size_t i; for(i=0;i<e->count;i++) { free(e->items[i].name);free(e->items[i].root);free(e->items[i].source); }
    free(e->items);free(e->default_name);free(e->default_source);free(e->system_path);free(e->user_path);pkg_environments_init(e);
}
static int valid_name(const char *s) {
    size_t n=0; if(!s || !*s) return 0;
    for(;*s;s++,n++) if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='-'||*s=='.')) return 0;
    return n<=63;
}
static int valid_root(const char *s) {
    const char *p; size_t n; int absolute=0;
    if(!s || !*s) return 0;
    n=strlen(s); if(n>4095 || isspace((unsigned char)*s) || isspace((unsigned char)s[n-1])) return 0;
    if(*s=='/' || *s=='\\') absolute=1;
    for(p=s;*p;p++) { if((unsigned char)*p<32 || (unsigned char)*p==127) return 0; if(*p==':' && !memchr(s,'/',(size_t)(p-s)) && !memchr(s,'\\',(size_t)(p-s))) absolute=1; }
    return absolute;
}
/* Resolve existing parent directories once, including platform aliases such as
 * macOS /etc and /var. Keep the final filename unresolved so links there are
 * refused. Missing configuration directories are retained without creation. */
static char *config_path(const char *path)
{
#if !defined(_WIN32) && !defined(__AROS__)
    char *prefix, *resolved, *result;
    size_t cut, n;
    if (!path) return NULL;
    prefix = dupstr(path);
    if (!prefix) return NULL;
    n = strlen(path);
    cut = n;
    while (cut > 0 && path[cut - 1] != '/') --cut;
    if (!cut) { free(prefix); return dupstr(path); }
    if (cut > 1) --cut;
    for (;;) {
        prefix[cut] = 0;
        resolved = realpath(prefix, NULL);
        if (resolved) break;
        if (errno != ENOENT || cut <= 1) { free(prefix); return NULL; }
        while (cut > 1 && path[cut - 1] != '/') --cut;
        if (cut > 1) --cut;
    }
    result = malloc(strlen(resolved) + n - cut + 2);
    if (result) {
        strcpy(result, resolved);
        if (cut == 1 && path[0] == '/' && strcmp(resolved, "/")) strcat(result, "/");
        strcat(result, path + cut);
    }
    free(resolved);
    free(prefix);
    return result;
#else
    return dupstr(path);
#endif
}

/* Final configuration files must be regular files, including when their
 * parent directory is reached through an operating-system alias. */
static int safe_path(const char *path)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 0 : -1;
    }
    return a & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY) ? -1 : 0;
#elif !defined(__AROS__)
    struct stat st;
    if (lstat(path, &st) == 0) return S_ISREG(st.st_mode) ? 0 : -1;
    return errno == ENOENT ? 0 : -1;
#else
    (void)path;
    return 0;
#endif
}
static int unchanged_parent(const char *path)
{
    char *resolved = config_path(path);
    int same = resolved && !strcmp(path, resolved);
    free(resolved);
    return same;
}
static char *trim(char *s) { char *p; while(*s==' '||*s=='\t')s++; p=s+strlen(s);while(p>s&&(p[-1]==' '||p[-1]=='\t'||p[-1]=='\r'))*--p=0;return s; }
static int append(struct pkg_environments *e,const char *name,const char *root,const char *path,int system) {
    struct pkg_environment *p; if(e->count>=256)return -1;
    p=realloc(e->items,(e->count+1)*sizeof *p);if(!p)return -1;e->items=p;p+=e->count;memset(p,0,sizeof *p);e->count++;
    p->name=dupstr(name);p->root=dupstr(root);p->source=dupstr(path);p->system=system;
    return p->name&&p->root&&p->source?0:-1;
}
static int read_scope(struct pkg_environments *e,const char *path,int system,char *err,size_t len) {
    FILE *f; char data[ENV_MAX+1], name[64]="", *line,*next,*v,*key;size_t n,i;int need_root=0,seen_default=0;
    if(!path)return 0;
    if(safe_path(path))return fail(err,len,"configuration path contains a link or special file");
    f=fopen(path,"rb");if(!f) { if(errno==ENOENT)return 0;return fail(err,len,"cannot read environment configuration"); }
    n=fread(data,1,sizeof data,f);if(ferror(f)||n>ENV_MAX){fclose(f);return fail(err,len,"environment configuration unreadable or exceeds 65536 bytes");}fclose(f);
    for(i=0;i<n;i++)if(!data[i] || ((unsigned char)data[i]<32&&data[i]!='\n'&&data[i]!='\r'&&data[i]!='\t') || (unsigned char)data[i]==127)return fail(err,len,"control character in environment configuration");
    data[n]=0;
    for(line=data;line;line=next) {
        next=strchr(line,'\n');if(next)*next++=0;line=trim(line);if(!*line||*line=='#')continue;
        if(*line=='[') {
            size_t l=strlen(line);if(need_root||l<3||line[l-1]!=']')goto invalid;line[l-1]=0;
            if(!valid_name(line+1))goto invalid;
            for(i=0;i<e->count;i++)if(e->items[i].system==system&&!strcmp(e->items[i].name,line+1))goto invalid;
            strcpy(name,line+1);need_root=1;continue;
        }
        v=strchr(line,'=');if(!v)goto invalid;*v++=0;key=trim(line);v=trim(v);
        if(!strcmp(key,"default")&&!*name) {
            if(seen_default++||!valid_name(v))goto invalid;
            free(e->default_name);free(e->default_source);e->default_name=dupstr(v);e->default_source=dupstr(path);
            if(!e->default_name||!e->default_source)goto oom;
        } else if(!strcmp(key,"root")&&need_root) {
            if (!valid_root(v)) goto invalid;
            if (append(e,name,v,path,system)) goto oom;
            need_root=0;
        } else goto invalid;
    }
    if (need_root) goto invalid;
    return 0;
invalid:return fail(err,len,"invalid environment configuration: expected default=name and unique [name] with absolute root=path");
oom:return fail(err,len,"out of memory");
}
int pkg_environments_load_paths(struct pkg_environments *e,const char *sys,const char *user,char *err,size_t len) {
    struct pkg_environments p;pkg_environments_init(&p);if(err&&len)*err=0;
    p.system_path=config_path(sys);p.user_path=config_path(user);
    if((sys&&!p.system_path)||(user&&!p.user_path)) { fail(err,len,"cannot resolve environment configuration directory"); goto failed; }
    if(read_scope(&p,p.system_path,1,err,len)|| (p.user_path&&(!p.system_path||strcmp(p.system_path,p.user_path))&&read_scope(&p,p.user_path,0,err,len)))goto failed;
    pkg_environments_free(e);*e=p;return 0;
failed:pkg_environments_free(&p);return -1;
}
int pkg_environments_load(struct pkg_environments *e,char *err,size_t len) {
    char *sys=NULL,*user=NULL;int rc;
#ifdef __AROS__
    sys=dupstr("ENVARC:pkg/environments.conf");
#elif defined(_WIN32)
    const char *s=getenv("PROGRAMDATA"),*u=getenv("APPDATA");
    if (s && *s) sys=pkg_join(s,"aros-pkg/environments.conf");
    if (u && *u) user=pkg_join(u,"aros-pkg/environments.conf");
#else
    const char *u=getenv("XDG_CONFIG_HOME"),*h=getenv("HOME");
    sys=dupstr("/etc/aros-pkg/environments.conf");
    if(u&&*u=='/')user=pkg_join(u,"aros-pkg/environments.conf");
    else if(h&&*h=='/')user=pkg_join(h,".config/aros-pkg/environments.conf");
#endif
    rc=pkg_environments_load_paths(e,sys,user,err,len);free(sys);free(user);return rc;
}
int pkg_environments_select(const struct pkg_environments *e,const char *name,const struct pkg_environment **out,char *err,size_t len) {
    size_t i;const struct pkg_environment *p=NULL;*out=NULL;if(err&&len)*err=0;
    if(!name)name=e->default_name;
    for(i=0;i<e->count;i++) {
        const struct pkg_environment *q=e->items+i;if(name&&strcmp(name,q->name))continue;
        if(p&& (strcmp(p->name,q->name)||strcmp(p->root,q->root))) {fail(err,len,"several environments match; specify ROOT explicitly");return 2;}
        if(!p||!q->system)p=q;
    }
    if(!p) { if(name)return fail(err,len,"named or default environment is not registered");return 1; }
    *out=p;return 0;
}
static int mutate(struct pkg_environments *e,const char *name,const char *root,int system,int op,char *err,size_t len) {
    struct pkg_environments p;struct pkg_fs_id before; const char *path=system?e->system_path:e->user_path;char *dir=NULL,*body=NULL;size_t i,used=0;int found=-1,rc=-1;
#ifdef __AROS__
    if(!path) {system=1;path=e->system_path;}
#endif
    if(!path)return fail(err,len,"configuration location unavailable for this scope");
    if(!valid_name(name)||(op==0&&!valid_root(root)))return fail(err,len,"invalid environment name or absolute root");
    if(op==0&&!pkg_fs_is_dir(root))return fail(err,len,"environment root must be an existing directory");
    pkg_environments_init(&p);
    if(safe_path(path)||pkg_fs_identity(path,&before))return fail(err,len,"unsafe or unreadable configuration path");
    if(read_scope(&p,path,system,err,len))goto done;
    for(i=0;i<p.count;i++)if(!strcmp(p.items[i].name,name))found=(int)i;
    if(op==0) {if(found>=0){fail(err,len,"environment name already registered in this scope");goto done;}if(append(&p,name,root,path,system))goto done;}
    else if(op==1) {
        if(found<0){fail(err,len,"environment name not registered in this scope");goto done;}
        free(p.items[found].name);free(p.items[found].root);free(p.items[found].source);memmove(p.items+found,p.items+found+1,(p.count-(size_t)found-1)*sizeof *p.items);p.count--;
        if(p.default_name&&!strcmp(p.default_name,name)){free(p.default_name);p.default_name=NULL;}
    } else {
        const struct pkg_environment *selected;
        if(pkg_environments_select(system?&p:e,name,&selected,err,len)!=0)goto done;
        free(p.default_name);p.default_name=dupstr(name);if(!p.default_name)goto done;
    }
    body=malloc(ENV_MAX+1);if(!body)goto done;body[0]=0;
    if(p.default_name)used=(size_t)snprintf(body,ENV_MAX+1,"default=%s\n\n",p.default_name);
    for(i=0;i<p.count;i++) {
        int n=snprintf(body+used,ENV_MAX+1-used,"[%s]\nroot=%s\n\n",p.items[i].name,p.items[i].root);
        if(n<0||(size_t)n>ENV_MAX-used){fail(err,len,"environment configuration exceeds 65536 bytes");goto done;}used+=(size_t)n;
    }
    dir=dupstr(path);if(!dir)goto done;
    {char *slash=strrchr(dir,'/'),*back=strrchr(dir,'\\');if(back&&(!slash||back>slash))slash=back;if(slash)*slash=0;else {char *colon=strrchr(dir,':');if(colon)colon[1]=0;else strcpy(dir,".");}}
    if(safe_path(path)||!unchanged_parent(path)||pkg_fs_mkdirs(dir)||!unchanged_parent(path)||pkg_fs_replace_if_same(path,&before,body,used)) {fail(err,len,"cannot save configuration, or file changed concurrently");goto done;}
    rc=pkg_environments_load_paths(e,e->system_path,e->user_path,err,len);
done:free(dir);free(body);pkg_environments_free(&p);return rc;
}
int pkg_environments_add(struct pkg_environments *e,const char *n,const char *r,int s,char *err,size_t l){return mutate(e,n,r,s,0,err,l);}
int pkg_environments_remove(struct pkg_environments *e,const char *n,int s,char *err,size_t l){return mutate(e,n,NULL,s,1,err,l);}
int pkg_environments_default(struct pkg_environments *e,const char *n,int s,char *err,size_t l){return mutate(e,n,NULL,s,2,err,l);}

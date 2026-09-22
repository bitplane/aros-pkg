/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "pkg_environment.h"
#include "pkg_fs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void put(const char *p,const char *s) {FILE *f=fopen(p,"wb");assert(f);assert(fwrite(s,1,strlen(s),f)==strlen(s));assert(!fclose(f));}
int main(void) {
    char tmp[]="/tmp/pkg-environments-XXXXXX",base[4096],sys[4200],user[4200],root[4200],other[4200],bad[4200],err[512];
    struct pkg_environments e;const struct pkg_environment *chosen;size_t i;
    const char *invalid[]={"default=x\ndefault=y\n","[x]\n","[x]\nroot=relative\n","[x]\nroot=/a\nroot=/b\n","[x]\nroot=/a\n[x]\nroot=/b\n","[bad name]\nroot=/a\n","unknown=thing\n","[x]\nroot=/a\ndefault=x\n","[x]\nroot=/a\rhidden\n"};
    assert(mkdtemp(tmp));assert(realpath(tmp,base));
    snprintf(sys,sizeof sys,"%s/system/environments.conf",base);snprintf(user,sizeof user,"%s/user/environments.conf",base);
    snprintf(root,sizeof root,"%s/root",base);snprintf(other,sizeof other,"%s/other",base);snprintf(bad,sizeof bad,"%s/bad",base);
    assert(!pkg_fs_mkdirs(root));assert(!pkg_fs_mkdirs(other));pkg_environments_init(&e);
    /* /etc and /tmp are platform directory aliases on macOS. */
    assert(!setenv("XDG_CONFIG_HOME",tmp,1));
    assert(!pkg_environments_load(&e,err,sizeof err));
    assert(e.system_path && e.user_path);
    assert(!pkg_fs_exists(e.user_path));
    { char alias[4200];
      snprintf(alias,sizeof alias,"%s/config-link",base);
      assert(!symlink(base,alias));
      { char config[4300]; snprintf(config,sizeof config,"%s/missing/env.conf",alias);
        assert(!pkg_environments_load_paths(&e,NULL,config,err,sizeof err));
        assert(!strstr(e.user_path,"config-link"));
        assert(!pkg_environments_add(&e,"aliased",root,0,err,sizeof err));
      }
      assert(!unlink(alias));
    }
    assert(!pkg_environments_load_paths(&e,sys,user,err,sizeof err));assert(!e.count);assert(access(user,F_OK));
    assert(pkg_environments_select(&e,NULL,&chosen,err,sizeof err)==1);
    assert(!pkg_environments_add(&e,"native",root,1,err,sizeof err));assert(e.count==1&&!e.default_name);
    assert(!pkg_environments_select(&e,NULL,&chosen,err,sizeof err));assert(chosen->system);
    assert(!pkg_environments_add(&e,"emu",other,0,err,sizeof err));assert(e.count==2);
    assert(pkg_environments_select(&e,NULL,&chosen,err,sizeof err)==2);
    assert(!pkg_environments_default(&e,"native",1,err,sizeof err));
    assert(!pkg_environments_default(&e,"emu",0,err,sizeof err));assert(!strcmp(e.default_source,user));
    assert(!pkg_environments_select(&e,NULL,&chosen,err,sizeof err));assert(!strcmp(chosen->root,other));
    assert(pkg_environments_add(&e,"emu",root,0,err,sizeof err)==-1);
    assert(pkg_environments_add(&e,"missing",bad,0,err,sizeof err)==-1);assert(access(bad,F_OK));
    assert(!pkg_environments_add(&e,"native",other,0,err,sizeof err));
    assert(pkg_environments_select(&e,"native",&chosen,err,sizeof err)==2);
    assert(pkg_environments_default(&e,"native",0,err,sizeof err)==-1);
    assert(!pkg_environments_remove(&e,"native",0,err,sizeof err));
    assert(!pkg_environments_remove(&e,"emu",0,err,sizeof err));assert(!strcmp(e.default_name,"native"));
    assert(!pkg_environments_select(&e,NULL,&chosen,err,sizeof err));assert(!strcmp(chosen->root,root));
    assert(!pkg_environments_add(&e,"native",root,0,err,sizeof err));assert(!pkg_environments_select(&e,"native",&chosen,err,sizeof err));assert(!chosen->system);
    assert(!pkg_fs_rmtree(root));assert(!pkg_environments_load_paths(&e,sys,user,err,sizeof err));assert(!pkg_environments_select(&e,NULL,&chosen,err,sizeof err));assert(!pkg_fs_exists(root));
    for(i=0;i<sizeof invalid/sizeof *invalid;i++){put(bad,invalid[i]);assert(pkg_environments_load_paths(&e,bad,NULL,err,sizeof err)==-1);assert(e.count==2);}
    put(bad,"default=lost\n");assert(!pkg_environments_load_paths(&e,NULL,bad,err,sizeof err));assert(pkg_environments_select(&e,NULL,&chosen,err,sizeof err)==-1);
    assert(!unlink(bad));assert(!symlink(user,bad));assert(pkg_environments_load_paths(&e,NULL,bad,err,sizeof err)==-1);assert(!unlink(bad));
    put(bad,"[one]\nroot=SYS:\n");assert(!pkg_environments_load_paths(&e,bad,bad,err,sizeof err));assert(e.count==1);
    {FILE *f=fopen(bad,"wb");assert(f);fwrite("[one]\nroot=/a\0hidden",1,20,f);fclose(f);assert(pkg_environments_load_paths(&e,bad,NULL,err,sizeof err)==-1);}
    pkg_environments_free(&e);assert(!pkg_fs_rmtree(base));puts("environment configuration: passed");return 0;
}

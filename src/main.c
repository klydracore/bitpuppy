#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>

#include <yaml.h>
#include <jansson.h>

#define BASE_DIR "/bit"
#define MAX_DEPS 128
#define MAX_STR 4096

typedef struct {
    char name[256];
    char version[128];
    char commands[MAX_STR];
    char url[MAX_STR];
    char root[256];
    char dependencies[MAX_DEPS][256];
    int dep_count;
} Package;

static char g_root_override[PATH_MAX] = "/";
static int g_auto_yes = 0;

/* ----------------------------- utils ------------------------------ */

static int path_exists(const char *p){
    struct stat st; return stat(p, &st) == 0;
}

static int is_dir(const char *p){
    struct stat st; if (stat(p, &st) != 0) return 0; return S_ISDIR(st.st_mode);
}

static int mkpath(const char *path, mode_t mode){
    char tmp[PATH_MAX]; snprintf(tmp, sizeof(tmp), "%s", path);
    for(char *p = tmp + 1; *p; p++){
        if(*p == '/'){
            *p = '\0';
            if(!path_exists(tmp)){
                if(mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
            }
            *p = '/';
        }
    }
    if(!path_exists(tmp)){
        if(mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    }
    return 0;
}

static char *trim(char *s){
    if(!s) return s;
    while(isspace((unsigned char)*s)) s++;
    if(*s == 0) return s;
    char *end = s + strlen(s) - 1;
    while(end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

static char *read_cmd_stdout(const char *cmd){
    FILE *fp = popen(cmd, "r");
    if(!fp) return NULL;
    size_t cap = 8192, len = 0; 
    char *buf = (char*)malloc(cap);
    if(!buf){ pclose(fp); return NULL; }
    int c;
    while((c = fgetc(fp)) != EOF){
        if(len + 1 >= cap){
            cap *= 2;
            char *nb = (char*)realloc(buf, cap);
            if(!nb){ free(buf); pclose(fp); return NULL; }
            buf = nb;
        }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    pclose(fp);
    return buf;
}

static int run_cmd(const char *fmt, ...){
    char cmd[8192];
    va_list ap; va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    return system(cmd);
}

static void replace_all(char *s, size_t s_cap, const char *needle, const char *with){
    if(!s || !needle || !with) return;
    char out[8192]; out[0] = 0;
    size_t nlen = strlen(needle), wlen = strlen(with);
    const char *cur = s;
    while(*cur){
        const char *p = strstr(cur, needle);
        if(!p){
            strncat(out, cur, sizeof(out)-strlen(out)-1);
            break;
        }
        strncat(out, cur, (size_t)(p - cur));
        strncat(out, with, sizeof(out)-strlen(out)-1);
        cur = p + nlen;
    }
    snprintf(s, s_cap, "%s", out);
}

/* ----------------------------- arch ------------------------------- */

static const char *detect_arch(){
    static char buf[64] = {0};
    char *out = read_cmd_stdout("uname -m");
    if(!out) return "unknown";
    snprintf(buf, sizeof(buf), "%s", trim(out));
    free(out);
    if(strcmp(buf, "x86_64") == 0) return "amd64";
    if(strcmp(buf, "aarch64") == 0) return "arm64";
    if(strcmp(buf, "armv7l") == 0) return "armhf";
    return buf;
}

/* --------------------------- YAML parse --------------------------- */

static int yaml_find_url(const char *yaml_text, char *out, size_t outsz){
    yaml_parser_t parser; yaml_event_t event;
    if(!yaml_parser_initialize(&parser)) return 0;
    yaml_parser_set_input_string(&parser, (const unsigned char*)yaml_text, strlen(yaml_text));

    char key[128] = "";
    int ok = 0;
    while(yaml_parser_parse(&parser, &event)){
        if(event.type == YAML_STREAM_END_EVENT){ yaml_event_delete(&event); break; }
        if(event.type == YAML_SCALAR_EVENT){
            const char *v = (const char*)event.data.scalar.value;
            if(key[0]==0){ snprintf(key,sizeof(key),"%s", v); }
            else if(strcmp(key,"url")==0){ snprintf(out,outsz,"%s",v); ok=1; yaml_event_delete(&event); break; }
            else key[0]=0;
        }
        yaml_event_delete(&event);
    }
    yaml_parser_delete(&parser);
    return ok;
}

static int parse_thread_yaml(const char *yaml_text, Package *pkg){
    yaml_parser_t parser; yaml_event_t event;
    if(!yaml_parser_initialize(&parser)) return 0;
    yaml_parser_set_input_string(&parser, (const unsigned char*)yaml_text, strlen(yaml_text));

    char key[256]="";
    char parent[64]="";
    int in_sequence = 0;

    while(yaml_parser_parse(&parser, &event)){
        if(event.type == YAML_STREAM_END_EVENT){ yaml_event_delete(&event); break; }

        switch(event.type){
            case YAML_MAPPING_START_EVENT: break;
            case YAML_MAPPING_END_EVENT: parent[0] = 0; break;
            case YAML_SEQUENCE_START_EVENT: in_sequence=1; break;
            case YAML_SEQUENCE_END_EVENT: in_sequence=0; break;
            case YAML_SCALAR_EVENT: {
                const char *val = (const char*)event.data.scalar.value;
                if(key[0]==0){ snprintf(key,sizeof(key),"%s",val); } 
                else {
                    if(strcmp(parent,"install")==0 && strcmp(key,"commands")==0){
                        snprintf(pkg->commands,sizeof(pkg->commands),"%s",val);
                    } else if(strcmp(parent,"source")==0 && strcmp(key,"package")==0){
                        snprintf(pkg->url,sizeof(pkg->url),"%s",val);
                    } else if(strcmp(key,"name")==0){
                        snprintf(pkg->name,sizeof(pkg->name),"%s",val);
                    } else if(strcmp(key,"version")==0){
                        snprintf(pkg->version,sizeof(pkg->version),"%s",val);
                    } else if(strcmp(key,"install")==0){ snprintf(parent,sizeof(parent),"install"); key[0]=0; yaml_event_delete(&event); continue; }
                    else if(strcmp(key,"source")==0){ snprintf(parent,sizeof(parent),"source"); key[0]=0; yaml_event_delete(&event); continue; }
                    else if(strcmp(key,"dependencies")==0){ key[0]=0; yaml_event_delete(&event); continue; }
                    key[0]=0;
                }
                break;
            }
            default: break;
        }

        if(in_sequence && event.type == YAML_SCALAR_EVENT && pkg->dep_count<MAX_DEPS){
            snprintf(pkg->dependencies[pkg->dep_count], sizeof(pkg->dependencies[0]), "%s", (const char*)event.data.scalar.value);
            pkg->dep_count++;
        }

        yaml_event_delete(&event);
    }

    yaml_parser_delete(&parser);
    return 1;
}

static int fetch_package(const char *pkgname, const char *remote, Package *out_pkg){
    char pointer_url[2048];
    snprintf(pointer_url,sizeof(pointer_url), "%s/%s.choco.yml", remote, pkgname);

    char cmd[4096];
    snprintf(cmd,sizeof(cmd),"curl -s \"%s\"", pointer_url);
    char *pointer_data = read_cmd_stdout(cmd);
    if(!pointer_data || pointer_data[0]=='\0'){ if(pointer_data) free(pointer_data); return 0; }

    char thread_url[MAX_STR] = {0};
    if(!yaml_find_url(pointer_data, thread_url, sizeof(thread_url))){ free(pointer_data); return 0; }
    free(pointer_data);

    snprintf(cmd,sizeof(cmd),"curl -s \"%s\"", thread_url);
    char *thread_data = read_cmd_stdout(cmd);
    if(!thread_data || thread_data[0]=='\0'){ if(thread_data) free(thread_data); return 0; }

    memset(out_pkg,0,sizeof(*out_pkg));
    out_pkg->dep_count=0;
    if(!parse_thread_yaml(thread_data, out_pkg)){ free(thread_data); return 0; }
    snprintf(out_pkg->root,sizeof(out_pkg->root),"%s", pkgname);

    free(thread_data);
    return 1;
}

/* ----------------------- installation logic ---------------------- */

static int install_package(Package *pkg){
    char install_dir[PATH_MAX];
    snprintf(install_dir,sizeof(install_dir), "%s/Chocolaterie/%s", BASE_DIR, pkg->root);
    if(path_exists(install_dir)) return 0;

    printf("\nInstalling:\n- %s\n", pkg->root);
    if(pkg->dep_count>0){ for(int i=0;i<pkg->dep_count;i++) printf("- %s\n", pkg->dependencies[i]); }
    if(!g_auto_yes){ printf("Continue? [Y/n] "); fflush(stdout); char line[64]; if(!fgets(line,sizeof(line),stdin)) return 1; char *ans=trim(line); if(ans[0]=='n'||ans[0]=='N'){ printf("Aborted.\n"); return 1; } }

    if(mkpath(install_dir,0755)!=0){ fprintf(stderr,"Failed to create %s: %s\n", install_dir, strerror(errno)); return 1; }

    char file[PATH_MAX]; snprintf(file,sizeof(file), "%s/%s.choco.pkg", install_dir, pkg->root);

    if(run_cmd("wget --quiet --show-progress -O \"%s\" \"%s\"", file, pkg->url)!=0){ fprintf(stderr,"Download failed: %s\n", pkg->url); return 1; }

    char tmpdir[PATH_MAX]; snprintf(tmpdir,sizeof(tmpdir),"/tmp/bitpuppy-extract-%s", pkg->root);
    if(path_exists(tmpdir)) run_cmd("rm -rf \"%s\"", tmpdir);
    if(mkpath(tmpdir,0755)!=0){ fprintf(stderr,"Failed tmp dir\n"); unlink(file); return 1; }

    if(run_cmd("tar --strip-components=1 -xf \"%s\" -C \"%s\"", file,tmpdir)!=0){ fprintf(stderr,"Extraction failed\n"); run_cmd("rm -rf \"%s\"", tmpdir); unlink(file); return 1; }

    run_cmd("sh -c 'set -e; for f in \"%s\"/*; do mv \"$f\" \"%s\"/; done'", tmpdir, install_dir);
    run_cmd("rm -rf \"%s\"", tmpdir); unlink(file);

    char cmds[MAX_STR]; snprintf(cmds,sizeof(cmds),"%s",pkg->commands);
    replace_all(cmds,sizeof(cmds),"$ROOT",(strcmp(g_root_override,"/")==0)? "" : g_root_override);
    run_cmd("%s", cmds);

    // create data folder
    char data_dir[PATH_MAX];
    snprintf(data_dir,sizeof(data_dir), "%s/data/%s", BASE_DIR, pkg->root);
    mkpath(data_dir,0755);

    printf("    %s: installed v%s\n", pkg->root, pkg->version);
    return 0;
}

static int remove_package(const char *pkgname){
    char path[PATH_MAX], data[PATH_MAX];
    snprintf(path, sizeof(path), "%s/Chocolaterie/%s", BASE_DIR, pkgname);
    snprintf(data, sizeof(data), "%s/data/%s", BASE_DIR, pkgname);
    if(!path_exists(path)){
        fprintf(stderr,"Package not installed: %s\n", pkgname);
        return 1;
    }
    run_cmd("rm -rf \"%s\"", path);
    run_cmd("rm -rf \"%s\"", data);
    printf("Package %s removed\n", pkgname);
    return 0;
}

static int update_all_packages(){
    char choco_dir[PATH_MAX];
    snprintf(choco_dir,sizeof(choco_dir),"%s/Chocolaterie", BASE_DIR);
    DIR *d = opendir(choco_dir);
    if(!d) return 0;
    struct dirent *entry;
    while((entry=readdir(d))){
        if(entry->d_name[0]=='.') continue;
        Package pkg;
        if(fetch_package(entry->d_name,"https://example-remote.com",&pkg)){
            install_package(&pkg);
        }
    }
    closedir(d);
    return 0;
}

static int add_remote(const char *url, const char *name){
    char remfile[PATH_MAX]; snprintf(remfile,sizeof(remfile),"%s/remotes.json", BASE_DIR);
    json_t *root = NULL;
    json_error_t err;
    if(path_exists(remfile)){
        root = json_load_file(remfile,0,&err);
        if(!root) root=json_object();
    } else root=json_object();

    json_t *remote = json_object();
    json_object_set_new(remote,"url",json_string(url));
    if(name) json_object_set_new(remote,"name",json_string(name));
    json_object_set_new(root,url,remote);
    json_dump_file(root,remfile,JSON_INDENT(2));
    json_decref(root);
    printf("Remote %s added\n",url);
    return 0;
}

/* ----------------------- main CLI ---------------------- */

static void print_help() {
    printf("BitPuppy CLI Help:\n\n");
    printf("Packages:\n");
    printf("  install <package>     Install a package.\n");
    printf("  remove <package>      Remove a package.\n");
    printf("  update                Update all packages.\n\n");
    printf("Remotes:\n");
    printf("  remote-add <url> [name] [channels...]  Add a remote from URL.\n");
    printf("  remote-add ppa:<profile>/<ppa>         Add a PPA.\n\n");
    printf("Locking:\n");
    printf("  lock                  Lock BitPuppy (block usage)\n");
    printf("  unlock                Unlock BitPuppy\n\n");
    printf("Options:\n");
    printf("  --yes                 Automatic yes for prompts\n");
}

int main(int argc, char **argv) {
    if(argc < 2) { print_help(); return 1; }

    for(int i = 1; i < argc; i++)
        if(strcmp(argv[i],"--yes")==0) g_auto_yes=1;

    // lock check
    char lock_file[PATH_MAX]; snprintf(lock_file,sizeof(lock_file),"%s/lock",BASE_DIR);

    if(path_exists(lock_file)){
        if(strcmp(argv[1],"unlock")!=0){
            fprintf(stderr,"BitPuppy is locked. Unlock first.\n");
            return 1;
        }
    }

    const char *cmd = argv[1];

    if(strcmp(cmd,"help")==0){ print_help(); return 0; }
    if(strcmp(cmd,"install")==0 && argc>=3){ 
        Package pkg;
        if(!fetch_package(argv[2],"https://example-remote.com",&pkg)){ fprintf(stderr,"Failed to fetch package: %s\n",argv[2]); return 1; }
        return install_package(&pkg);
    }
    if(strcmp(cmd,"remove")==0 && argc>=3) return remove_package(argv[2]);
    if(strcmp(cmd,"update")==0) return update_all_packages();
    if(strcmp(cmd,"remote-add")==0 && argc>=3){
        const char *name = argc>=4?argv[3]:NULL;
        return add_remote(argv[2],name);
    }
    if(strcmp(cmd,"lock")==0){
        FILE *f = fopen(lock_file,"w"); if(f){ fputs("locked",f); fclose(f); }
        printf("BitPuppy locked\n");
        return 0;
    }
    if(strcmp(cmd,"unlock")==0){
        unlink(lock_file);
        printf("BitPuppy unlocked\n");
        return 0;
    }

    fprintf(stderr,"Unknown command: %s\n",cmd);
    print_help();
    return 1;
}

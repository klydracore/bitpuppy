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

#include <yaml.h>
#include <jansson.h>

#define BASE_DIR "/bit"
#define MAX_DEPS 128
#define MAX_URLS 1024
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

static char g_root_override[PATH_MAX] = "/"; // --root=...
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

/* --------------------------- UI / help ---------------------------- */

static void prompt_help(void){
    printf("\nHelp:\n\n");
    printf("Packages:\n");
    printf("- install <package>     Install a package.\n");
    printf("- remove <package>      Remove a package.\n");
    printf("- update                Update all packages.\n\n");
    printf("Remotes:\n");
    printf("- remote-add <url> [name] [channels...]  Add a remote from URL.\n");
    printf("- remote-add ppa:<profile>/<ppa>         Add a PPA.\n\n");
    printf("Locking:\n");
    printf("- lock                  Lock BitPuppy (block usage)\n");
    printf("- unlock                Unlock BitPuppy\n\n");
}

/* ---------------------------- remotes ----------------------------- */

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} StrList;

static void sl_init(StrList *l){ l->items=NULL; l->count=0; l->cap=0; }
static void sl_push(StrList *l, const char *s){
    if(l->count == l->cap){
        size_t nc = l->cap? l->cap*2 : 16;
        char **ni = (char**)realloc(l->items, nc * sizeof(char*));
        if(!ni) return;
        l->items = ni; l->cap = nc;
    }
    l->items[l->count] = strdup(s);
    if(l->items[l->count]) l->count++;
}
static void sl_free(StrList *l){
    for(size_t i=0;i<l->count;i++) free(l->items[i]);
    free(l->items);
    l->items=NULL; l->count=0; l->cap=0;
}

static void add_remote(const char *urlArg, const char *name, char **channels, int channel_count){
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/Chocobitpup/remotes/%s", BASE_DIR, name?name:"default");
    if(mkpath(dir, 0755) != 0){
        fprintf(stderr, "Failed to create %s: %s\n", dir, strerror(errno));
        return;
    }
    char listfile[PATH_MAX];
    snprintf(listfile, sizeof(listfile), "%s/remote.choco.list", dir);
    FILE *f = fopen(listfile, "a");
    if(!f){ perror("fopen"); return; }

    fprintf(f, "choco %s %s", urlArg, name?name:"default");
    for(int i=0;i<channel_count;i++){
        fprintf(f, " %s", channels[i]);
    }
    fprintf(f, "\n");
    fclose(f);
    printf("Remote added to %s\n", dir);
}

static void collect_remote_lists(const char *root, StrList *files){
    DIR *d = opendir(root);
    if(!d) return;
    struct dirent *e;
    while((e = readdir(d))){
        if(strcmp(e->d_name,".")==0 || strcmp(e->d_name,"..")==0) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", root, e->d_name);
        if(is_dir(path)){
            collect_remote_lists(path, files);
        }else{
            if(strcmp(e->d_name, "remote.choco.list")==0){
                sl_push(files, path);
            }
        }
    }
    closedir(d);
}

static void get_remotes(StrList *urls){
    sl_init(urls);
    const char *arch = detect_arch();
    char remroot[PATH_MAX];
    snprintf(remroot, sizeof(remroot), "%s/Chocobitpup/remotes", BASE_DIR);
    if(!path_exists(remroot)) return;

    StrList lists; sl_init(&lists);
    collect_remote_lists(remroot, &lists);

    for(size_t i=0;i<lists.count;i++){
        FILE *f = fopen(lists.items[i], "r");
        if(!f) continue;
        char line[4096];
        while(fgets(line, sizeof(line), f)){
            char *cur = trim(line);
            if(*cur == 0) continue;

            // tokens: type base pool [channels...]
            char *type = strtok(cur, " \t\r\n");
            char *base = strtok(NULL, " \t\r\n");
            char *pool = strtok(NULL, " \t\r\n");
            if(!type || !base || !pool) continue;
            if(strcmp(type, "choco") != 0) continue;

            char *tok;
            while((tok = strtok(NULL, " \t\r\n"))){
                char full[2048];
                snprintf(full, sizeof(full), "%s/pool/%s/%s/%s", base, pool, arch, tok);
                sl_push(urls, full);
            }
        }
        fclose(f);
    }
    sl_free(&lists);
}

/* --------------------------- YAML parse --------------------------- */

static int yaml_get_scalar_value(yaml_parser_t *parser, char *out, size_t outsz){
    yaml_event_t event;
    if(!yaml_parser_parse(parser, &event)) return 0;
    int ok = 0;
    if(event.type == YAML_SCALAR_EVENT){
        snprintf(out, outsz, "%s", (const char*)event.data.scalar.value);
        ok = 1;
    }
    yaml_event_delete(&event);
    return ok;
}

// Parse minimal YAML with structure:
// pointer file: { url: <string> }
// thread file:
// name: <s>
// version: <s>
// install: { commands: <s> }
// source: { package: <s> }
// dependencies: [ <s>, <s>, ... ]
static int parse_thread_yaml(const char *yaml_text, Package *pkg){
    yaml_parser_t parser; yaml_event_t event;
    if(!yaml_parser_initialize(&parser)) return 0;
    yaml_parser_set_input_string(&parser, (const unsigned char*)yaml_text, strlen(yaml_text));

    char key[256]="";
    char parent[64]="";
    int in_mapping = 0, in_sequence = 0;

    while(yaml_parser_parse(&parser, &event)){
        if(event.type == YAML_STREAM_END_EVENT){ yaml_event_delete(&event); break; }

        switch(event.type){
            case YAML_MAPPING_START_EVENT:
                in_mapping = 1;
                break;
            case YAML_MAPPING_END_EVENT:
                parent[0] = 0;
                in_mapping = 0;
                break;
            case YAML_SEQUENCE_START_EVENT:
                in_sequence = 1;
                break;
            case YAML_SEQUENCE_END_EVENT:
                in_sequence = 0;
                break;
            case YAML_SCALAR_EVENT: {
                const char *val = (const char*)event.data.scalar.value;
                if(key[0] == 0){
                    // this scalar is a key
                    snprintf(key, sizeof(key), "%s", val);
                } else {
                    // this scalar is a value for `key`
                    if(strcmp(parent, "install")==0 && strcmp(key, "commands")==0){
                        snprintf(pkg->commands, sizeof(pkg->commands), "%s", val);
                    } else if(strcmp(parent, "source")==0 && strcmp(key, "package")==0){
                        snprintf(pkg->url, sizeof(pkg->url), "%s", val);
                    } else if(strcmp(key, "name")==0){
                        snprintf(pkg->name, sizeof(pkg->name), "%s", val);
                    } else if(strcmp(key, "version")==0){
                        snprintf(pkg->version, sizeof(pkg->version), "%s", val);
                    } else if(strcmp(key, "install")==0){
                        snprintf(parent, sizeof(parent), "install");
                        key[0]=0; // expect nested mapping
                        yaml_event_delete(&event);
                        continue;
                    } else if(strcmp(key, "source")==0){
                        snprintf(parent, sizeof(parent), "source");
                        key[0]=0;
                        yaml_event_delete(&event);
                        continue;
                    } else if(strcmp(key, "dependencies")==0){
                        // next will be a sequence; handle in SEQUENCE events
                        key[0]=0;
                        yaml_event_delete(&event);
                        continue;
                    } else {
                        // unknown simple key -> value; ignore
                    }
                    key[0]=0;
                }
                break;
            }
            default: break;
        }

        // If dependencies sequence: read simple scalars
        if(event.type == YAML_SEQUENCE_START_EVENT && strcmp(key,"")==0){
            // no-op, rely on data flow
        }

        if(event.type == YAML_SEQUENCE_START_EVENT){
            // assume it's dependencies
            // read until sequence end
            while(1){
                yaml_event_t ev2;
                if(!yaml_parser_parse(&parser, &ev2)) break;
                if(ev2.type == YAML_SEQUENCE_END_EVENT){
                    yaml_event_delete(&ev2);
                    break;
                }
                if(ev2.type == YAML_SCALAR_EVENT){
                    if(pkg->dep_count < MAX_DEPS){
                        snprintf(pkg->dependencies[pkg->dep_count], sizeof(pkg->dependencies[0]), "%s",
                                 (const char*)ev2.data.scalar.value);
                        pkg->dep_count++;
                    }
                }
                yaml_event_delete(&ev2);
            }
            // after sequence, continue outer loop
        }

        yaml_event_delete(&event);
    }

    yaml_parser_delete(&parser);
    return 1;
}

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
            if(key[0]==0){
                snprintf(key,sizeof(key),"%s", v);
            }else{
                if(strcmp(key,"url")==0){
                    snprintf(out, outsz, "%s", v);
                    ok = 1;
                    yaml_event_delete(&event);
                    break;
                }
                key[0]=0;
            }
        }
        yaml_event_delete(&event);
    }
    yaml_parser_delete(&parser);
    return ok;
}

/* -------------------------- package I/O --------------------------- */

static int fetch_package(const char *pkgname, const char *remote, Package *out_pkg){
    // download pointer yaml
    char pointer_url[2048];
    snprintf(pointer_url, sizeof(pointer_url), "%s/%s.choco.yml", remote, pkgname);

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "curl -s \"%s\"", pointer_url);
    char *pointer_data = read_cmd_stdout(cmd);
    if(!pointer_data || pointer_data[0] == '\0'){
        if(pointer_data) free(pointer_data);
        return 0;
    }

    char thread_url[MAX_STR] = {0};
    if(!yaml_find_url(pointer_data, thread_url, sizeof(thread_url))){
        free(pointer_data); return 0;
    }
    free(pointer_data);

    snprintf(cmd, sizeof(cmd), "curl -s \"%s\"", thread_url);
    char *thread_data = read_cmd_stdout(cmd);
    if(!thread_data || thread_data[0] == '\0'){
        if(thread_data) free(thread_data);
        return 0;
    }

    memset(out_pkg, 0, sizeof(*out_pkg));
    out_pkg->dep_count = 0;
    if(!parse_thread_yaml(thread_data, out_pkg)){
        free(thread_data); return 0;
    }
    snprintf(out_pkg->root, sizeof(out_pkg->root), "%s", pkgname);

    free(thread_data);
    return 1;
}

static int save_dependency_record(const char *dep, const char *owner){
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/Chocolaterie/%s/dependency.json", BASE_DIR, dep);

    json_t *root = NULL;
    if(path_exists(path)){
        json_error_t jerr;
        root = json_load_file(path, 0, &jerr);
    }
    if(!root) root = json_object();

    json_t *owners = json_object_get(root, "owners");
    if(!owners || !json_is_array(owners)){
        owners = json_array();
        json_object_set_new(root, "owners", owners);
    }

    // only append if not already present
    size_t i, n = json_array_size(owners);
    for(i=0;i<n;i++){
        json_t *it = json_array_get(owners, i);
        if(json_is_string(it) && strcmp(json_string_value(it), owner)==0) break;
    }
    if(i==n) json_array_append_new(owners, json_string(owner));

    int rc = json_dump_file(root, path, JSON_INDENT(2));
    json_decref(root);
    return rc == 0;
}

/* ----------------------- installation logic ---------------------- */

static int install_package(Package *pkg){
    char install_dir[PATH_MAX];
    snprintf(install_dir, sizeof(install_dir), "%s/Chocolaterie/%s", BASE_DIR, pkg->root);
    if(path_exists(install_dir)){
        return 0; // already installed
    }

    printf("\nInstalling:\n- %s\n", pkg->root);
    if(pkg->dep_count > 0){
        printf("Dependencies:\n");
        for(int i=0;i<pkg->dep_count;i++){
            printf("- %s\n", pkg->dependencies[i]);
        }
    }
    if(!g_auto_yes){
        printf("Continue? [Y/n] ");
        fflush(stdout);
        char line[64];
        if(!fgets(line, sizeof(line), stdin)) return 1;
        char *ans = trim(line);
        if(ans[0]=='n' || ans[0]=='N'){
            printf("Aborted.\n");
            return 1;
        }
    }

    if(mkpath(install_dir, 0755) != 0){
        fprintf(stderr, "Failed to create %s: %s\n", install_dir, strerror(errno));
        return 1;
    }

    char file[PATH_MAX];
    snprintf(file, sizeof(file), "%s/%s-%s.choco.pkg", install_dir, pkg->root, pkg->version);

    // download
    if(run_cmd("wget --quiet --show-progress -O \"%s\" \"%s\"", file, pkg->url) != 0){
        fprintf(stderr, "Download failed for %s\n", pkg->root);
        return 1;
    }

    // temp dir
    char tmpdir[PATH_MAX];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/bitpuppy-extract-%s", pkg->root);
    if(path_exists(tmpdir)) run_cmd("rm -rf \"%s\"", tmpdir);
    if(mkpath(tmpdir, 0755) != 0){
        fprintf(stderr, "Failed to create tmp dir: %s\n", strerror(errno));
        unlink(file);
        return 1;
    }

    // extract
    if(run_cmd("tar --strip-components=1 -xf \"%s\" -C \"%s\"", file, tmpdir) != 0){
        fprintf(stderr, "Extraction failed for %s\n", file);
        run_cmd("rm -rf \"%s\"", tmpdir);
        unlink(file);
        return 1;
    }

    // move contents into install_dir
    run_cmd("sh -c 'set -e; for f in \"%s\"/*; do mv \"$f\" \"%s\"/; done'", tmpdir, install_dir);

    // cleanup
    run_cmd("rm -rf \"%s\"", tmpdir);
    unlink(file);

    // run install commands, replacing $ROOT
    char cmds[MAX_STR];
    snprintf(cmds, sizeof(cmds), "%s", pkg->commands);
    replace_all(cmds, sizeof(cmds), "$ROOT", (strcmp(g_root_override,"/")==0) ? "" : g_root_override);
    run_cmd("%s", cmds);

    // record dependency owners
    for(int i=0;i<pkg->dep_count;i++){
        save_dependency_record(pkg->dependencies[i], pkg->name[0]?pkg->name:pkg->root);
    }

    printf("    %s: installed v%s\n", pkg->root, pkg->version);
    return 0;
}

static int remove_package(const char *name){
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/Chocolaterie/%s", BASE_DIR, name);
    if(!path_exists(path)){
        fprintf(stderr, "Package not found: %s\n", name);
        return 1;
    }

    printf("\nRemoving:\n- %s\n", name);
    char depfile[PATH_MAX];
    snprintf(depfile, sizeof(depfile), "%s/dependency.json", path);
    if(path_exists(depfile)){
        json_error_t jerr;
        json_t *root = json_load_file(depfile, 0, &jerr);
        if(root){
            json_t *owners = json_object_get(root, "owners");
            if(owners && json_is_array(owners) && json_array_size(owners)==0){
                printf("Dependencies:\n- %s\n", name);
            }
            json_decref(root);
        }
    }

    if(!g_auto_yes){
        printf("Continue? [Y/n] ");
        fflush(stdout);
        char line[64];
        if(!fgets(line, sizeof(line), stdin)) return 1;
        char *ans = trim(line);
        if(ans[0]=='n' || ans[0]=='N'){
            printf("Aborted.\n");
            return 1;
        }
    }

    run_cmd("rm -rf \"%s\"", path);
    printf("    Removed %s\n", name);
    return 0;
}

/* ------- dependency collection to preserve install order --------- */

static int find_pkg_in_remotes(const char *name, StrList *remotes, Package *out){
    for(size_t i=0;i<remotes->count;i++){
        if(fetch_package(name, remotes->items[i], out)){
            return 1;
        }
    }
    return 0;
}

typedef struct {
    Package *items;
    size_t count;
    size_t cap;
} PkgList;

static void pl_init(PkgList *pl){ pl->items=NULL; pl->count=0; pl->cap=0; }
static void pl_push(PkgList *pl, const Package *p){
    if(pl->count == pl->cap){
        size_t nc = pl->cap? pl->cap*2 : 16;
        Package *ni = (Package*)realloc(pl->items, nc * sizeof(Package));
        if(!ni) return;
        pl->items = ni; pl->cap = nc;
    }
    pl->items[pl->count++] = *p;
}
static void pl_free(PkgList *pl){ free(pl->items); pl->items=NULL; pl->count=0; pl->cap=0; }

static int set_has(StrList *s, const char *val){
    for(size_t i=0;i<s->count;i++) if(strcmp(s->items[i], val)==0) return 1;
    return 0;
}
static void set_add(StrList *s, const char *val){
    if(!set_has(s, val)) sl_push(s, val);
}

static void collect_packages_with_deps(const Package *pkg, StrList *collected, PkgList *ordered, StrList *remotes){
    if(set_has(collected, pkg->root)) return;
    set_add(collected, pkg->root);

    for(int i=0;i<pkg->dep_count;i++){
        const char *dep = pkg->dependencies[i];
        if(set_has(collected, dep)) continue;
        Package d = {0};
        if(find_pkg_in_remotes(dep, remotes, &d)){
            snprintf(d.root, sizeof(d.root), "%s", dep);
            collect_packages_with_deps(&d, collected, ordered, remotes);
        }
    }
    pl_push(ordered, pkg);
}

/* ----------------------------- update ----------------------------- */

static void update_all(void){
    char root[PATH_MAX];
    snprintf(root, sizeof(root), "%s/Chocolaterie", BASE_DIR);
    DIR *d = opendir(root);
    if(!d){
        fprintf(stderr, "No installed packages in %s\n", root);
        return;
    }

    StrList remotes; get_remotes(&remotes);

    struct dirent *e;
    while((e = readdir(d))){
        if(strcmp(e->d_name,".")==0 || strcmp(e->d_name,"..")==0) continue;
        char pkgname[256]; snprintf(pkgname, sizeof(pkgname), "%s", e->d_name);
        printf("    Updating %s...\n", pkgname);

        Package p = {0};
        if(find_pkg_in_remotes(pkgname, &remotes, &p)){
            snprintf(p.root, sizeof(p.root), "%s", pkgname);
            g_auto_yes = 1; // auto for update
            install_package(&p);
        }
    }

    sl_free(&remotes);
    closedir(d);
}

/* ------------------------------- main ----------------------------- */

int main(int argc, char **argv){
    // locking semantics follow original:
    if(path_exists("/bit/lock")){
        if(argc < 2 || strcmp(argv[1], "unlock") != 0){
            fprintf(stderr, "BitPuppy is locked. Run 'bitpup unlock' to unlock.\n");
            return 1;
        }
    }

    if(argc < 2){
        printf("Run 'bitpup help' for help!\n");
        return 0;
    }

    const char *cmd = argv[1];

    // parse flags and args after command
    StrList pkgs; sl_init(&pkgs);
    for(int i=2;i<argc;i++){
        const char *a = argv[i];
        if(strcmp(a,"-y")==0) g_auto_yes = 1;
        else if(strncmp(a, "--root=", 7)==0){
            snprintf(g_root_override, sizeof(g_root_override), "%s", a+7);
        } else {
            sl_push(&pkgs, a);
        }
    }

    if(strcmp(cmd, "help")==0){
        prompt_help();

    } else if(strcmp(cmd, "remote-add")==0 && pkgs.count >= 1){
        const char *url = pkgs.items[0];
        const char *name = (pkgs.count >= 2) ? pkgs.items[1] : "default";
        char **channels = NULL; int ch_cnt = 0;
        if(pkgs.count > 2){
            ch_cnt = (int)(pkgs.count - 2);
            channels = &pkgs.items[2];
        }
        add_remote(url, name, channels, ch_cnt);

    } else if(strcmp(cmd, "remove")==0 && pkgs.count >= 1){
        for(size_t i=0;i<pkgs.count;i++) remove_package(pkgs.items[i]);

    } else if(strcmp(cmd, "install")==0 && pkgs.count >= 1){
        StrList remotes; get_remotes(&remotes);
        if(remotes.count == 0){
            fprintf(stderr, "No remotes configured. Use 'bitpup remote-add <url> [name] [channels...]'\n");
            sl_free(&remotes);
            sl_free(&pkgs);
            return 1;
        }

        // Collect packages + deps in install order
        StrList collected; sl_init(&collected);
        PkgList ordered;  pl_init(&ordered);

        for(size_t i=0;i<pkgs.count;i++){
            const char *pkgname = pkgs.items[i];
            Package p = {0};
            if(!find_pkg_in_remotes(pkgname, &remotes, &p)){
                fprintf(stderr, "Package not found in remotes: %s\n", pkgname);
                continue;
            }
            snprintf(p.root, sizeof(p.root), "%s", pkgname);
            collect_packages_with_deps(&p, &collected, &ordered, &remotes);
        }

        // Present plan
        printf("\nInstalling:\n");
        for(size_t i=0;i<ordered.count;i++){
            printf("- %s\n", ordered.items[i].root);
        }
        if(!g_auto_yes){
            printf("Continue? [Y/n] ");
            fflush(stdout);
            char line[64];
            if(!fgets(line, sizeof(line), stdin)){
                pl_free(&ordered); sl_free(&collected); sl_free(&remotes); sl_free(&pkgs);
                return 0;
            }
            char *ans = trim(line);
            if(ans[0]=='n' || ans[0]=='N'){
                printf("Aborted.\n");
                pl_free(&ordered); sl_free(&collected); sl_free(&remotes); sl_free(&pkgs);
                return 0;
            }
        }

        // Ensure /bit/data/<pkg> exists before install, like original
        for(size_t i=0;i<ordered.count;i++){
            char installPath[PATH_MAX];
            snprintf(installPath, sizeof(installPath), "%s/Chocolaterie/%s", BASE_DIR, ordered.items[i].root);
            if(!path_exists(installPath)){
                char dataPath[PATH_MAX];
                snprintf(dataPath, sizeof(dataPath), "%s/data/%s", BASE_DIR, ordered.items[i].root);
                if(!path_exists(dataPath)) mkpath(dataPath, 0755);
            }
            g_auto_yes = 1; // per original: install executes without prompting per item
            install_package(&ordered.items[i]);
        }

        pl_free(&ordered); sl_free(&collected); sl_free(&remotes);

    } else if(strcmp(cmd, "update")==0){
        update_all();

    } else if(strcmp(cmd, "lock")==0){
        if(mkpath("/opt/bitpuppy", 0755) != 0){
            fprintf(stderr, "Failed to create /opt/bitpuppy: %s\n", strerror(errno));
        }
        FILE *f = fopen("/opt/bitpuppy/lock", "w");
        if(f){ fputs("locked\n", f); fclose(f); }
        printf("BitPuppy locked.\n");

    } else if(strcmp(cmd, "unlock")==0){
        if(path_exists("/opt/bitpuppy/lock")){
            unlink("/opt/bitpuppy/lock");
            printf("BitPuppy unlocked.\n");
        } else {
            printf("BitPuppy was not locked.\n");
        }

    } else if(strcmp(cmd, "version")==0){
        fprintf(stderr, "BitPuppy 3.1.1\n");

    } else {
        fprintf(stderr, "Error: '%s' is not a valid option.\n", cmd);
        printf("Maybe you meant 'install'?\n");
        printf("Run 'bitpup help' for help!\n");
    }

    sl_free(&pkgs);
    return 0;
}

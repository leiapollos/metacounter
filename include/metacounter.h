#ifndef METACOUNTER_H
#define METACOUNTER_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>

typedef enum {
    MC_DUP_IGNORE = 0,
    MC_DUP_WARN = 1,
    MC_DUP_ERROR = 2
} McDuplicatePolicy;

typedef struct McProject McProject;

typedef struct McMarkers {
    const char *standard;
    const char *unique;
} McMarkers;

typedef struct McRegistryOpts {
    const char *output_header;
    const char *enum_name;
    const char *count_name;
    McMarkers markers;
    McDuplicatePolicy duplicate_policy;
} McRegistryOpts;

typedef struct MetaSelfRebuild {
    const char *cc;
    const char *cflags;
    const char *src_path;
    const char *exe_path;
} MetaSelfRebuild;

// Project lifecycle (returns heap-allocated handle; no free needed for short-lived tools)
McProject* mc_begin(void);

// Add inputs (plural-only). Strings are copied into the project's arena.
bool mc_add_extensions(McProject *p, const char *const *exts, size_t count);
bool mc_add_folders(McProject *p, const char *const *folders, size_t count);
bool mc_add_paths(McProject *p, const char *const *paths, size_t count);

// Add a registry to generate
bool mc_add_registry(McProject *p, McRegistryOpts opts);

// Run scan once and emit all registries
bool mc_generate(McProject *p);

// Self-rebuild helper (renamed to avoid copying nob). Returns true when done (or after re-exec).
bool meta_self_rebuild_(int argc, char **argv, MetaSelfRebuild opts);

// Macros
#define MC_EXTS(p, ...) do { const char* _mc_exts_arr[] = { __VA_ARGS__ }; mc_add_extensions((p), _mc_exts_arr, sizeof(_mc_exts_arr)/sizeof(_mc_exts_arr[0])); } while (0)
#define MC_FOLDERS(p, ...) do { const char* _mc_folders_arr[] = { __VA_ARGS__ }; mc_add_folders((p), _mc_folders_arr, sizeof(_mc_folders_arr)/sizeof(_mc_folders_arr[0])); } while (0)
#define MC_PATHS(p, ...) do { const char* _mc_paths_arr[] = { __VA_ARGS__ }; mc_add_paths((p), _mc_paths_arr, sizeof(_mc_paths_arr)/sizeof(_mc_paths_arr[0])); } while (0)
#define MC_REGISTRY(p, ...) mc_add_registry((p), (McRegistryOpts){ __VA_ARGS__ })
#define META_SELF_REBUILD(argc, argv, ...) meta_self_rebuild_((argc), (argv), (MetaSelfRebuild){ .cc = "cc", .cflags = "-O2 -Wall -Wextra", .src_path = __FILE__, .exe_path = NULL, __VA_ARGS__ })

#ifdef __cplusplus
} // extern "C"
#endif

// --------------------------- Implementation ------------------------------

#ifdef METACOUNTER_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------- Arena --------------------------------------

typedef struct MetaArena {
    unsigned char *memory;
    size_t page_size;
    size_t reserved_size;
    size_t committed_size;
    size_t position;
} MetaArena;

static size_t meta_get_page_size(void) {
#ifdef _WIN32
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    return (size_t)sysInfo.dwPageSize;
#else
    long pz = sysconf(_SC_PAGESIZE);
    return (size_t)(pz > 0 ? pz : 4096);
#endif
}

static size_t meta_align_up(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

static void meta_arena_init(MetaArena *a, size_t reserve_size_bytes) {
    a->page_size = meta_get_page_size();
    a->reserved_size = meta_align_up(reserve_size_bytes, a->page_size);
    a->committed_size = 0;
    a->position = 0;
#ifdef _WIN32
    a->memory = (unsigned char*)VirtualAlloc(NULL, a->reserved_size, MEM_RESERVE, PAGE_NOACCESS);
#else
    a->memory = (unsigned char*)mmap(NULL, a->reserved_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a->memory == MAP_FAILED) a->memory = NULL;
#endif
    if (!a->memory) {
        fprintf(stderr, "FATAL: Failed to reserve memory for arena.\n");
        abort();
    }
}

static void *meta_arena_alloc(MetaArena *a, size_t size) {
    if (size == 0) return NULL;
    size_t new_pos = a->position + size;
    if (new_pos > a->reserved_size) {
        fprintf(stderr, "FATAL: Arena out of reserved memory.\n");
        return NULL;
    }
    if (new_pos > a->committed_size) {
        size_t new_commit = meta_align_up(new_pos, a->page_size);
        size_t size_to_commit = new_commit - a->committed_size;
        void *addr = a->memory + a->committed_size;
#ifdef _WIN32
        if (VirtualAlloc(addr, size_to_commit, MEM_COMMIT, PAGE_READWRITE) == NULL) {
            fprintf(stderr, "FATAL: Failed to commit memory.\n");
            return NULL;
        }
#else
        if (mprotect(addr, size_to_commit, PROT_READ | PROT_WRITE) != 0) {
            fprintf(stderr, "FATAL: Failed to commit memory (mprotect).\n");
            return NULL;
        }
#endif
        a->committed_size = new_commit;
    }
    void *result = a->memory + a->position;
    a->position = new_pos;
    return result;
}

static char *meta_arena_strdup(MetaArena *a, const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char*)meta_arena_alloc(a, n);
    if (!p) return NULL;
    memcpy(p, s, n);
    return p;
}

// ----------------------------- Internals ----------------------------------

typedef struct McIdentifier {
    char *name;
    char *filepath;
    int line_num;
    int is_unique;
    int value; // -1 for auto
} McIdentifier;

typedef struct McRegistry {
    McRegistryOpts opts;
    McIdentifier *ids;
    size_t id_count;
    size_t id_capacity;
} McRegistry;

struct McProject {
    MetaArena arena;
    char **extensions; size_t extCount; size_t extCapacity;
    char **folders;    size_t folderCount; size_t folderCapacity;
    char **paths;      size_t pathCount;   size_t pathCapacity;
    McRegistry *registries; size_t regCount; size_t regCapacity;
};

static void *meta_arena_grow_copy(MetaArena *a, void *old_ptr, size_t old_bytes, size_t new_bytes) {
    void *p = meta_arena_alloc(a, new_bytes);
    if (!p) return NULL;
    if (old_ptr && old_bytes) memcpy(p, old_ptr, old_bytes);
    return p;
}

static void mc_vec_push_str(MetaArena *a, char ***arr, size_t *count, size_t *cap, const char *s) {
    if (*count >= *cap) {
        size_t new_cap = (*cap == 0) ? 8 : (*cap * 2);
        char **new_arr = (char**)meta_arena_grow_copy(a, *arr, (*cap) * sizeof(char*), new_cap * sizeof(char*));
        *arr = new_arr; *cap = new_cap;
    }
    (*arr)[(*count)++] = meta_arena_strdup(a, s);
}

static void mc_vec_push_ident(MetaArena *a, McIdentifier **arr, size_t *count, size_t *cap, const McIdentifier *id) {
    if (*count >= *cap) {
        size_t new_cap = (*cap == 0) ? 16 : (*cap * 2);
        McIdentifier *new_arr = (McIdentifier*)meta_arena_grow_copy(a, *arr, (*cap) * sizeof(McIdentifier), new_cap * sizeof(McIdentifier));
        *arr = new_arr; *cap = new_cap;
    }
    (*arr)[(*count)++] = *id;
}

static void mc_vec_push_registry(MetaArena *a, McRegistry **arr, size_t *count, size_t *cap, const McRegistry *reg) {
    if (*count >= *cap) {
        size_t new_cap = (*cap == 0) ? 4 : (*cap * 2);
        McRegistry *new_arr = (McRegistry*)meta_arena_grow_copy(a, *arr, (*cap) * sizeof(McRegistry), new_cap * sizeof(McRegistry));
        *arr = new_arr; *cap = new_cap;
    }
    (*arr)[(*count)++] = *reg;
}

 

// ----------------------------- FS -----------------------------------------

static int mc_is_dir(const char *path) {
    struct stat s;
    if (stat(path, &s) != 0) return 0;
    return (s.st_mode & S_IFDIR) != 0;
}

static int mc_is_file(const char *path) {
    struct stat s;
    if (stat(path, &s) != 0) return 0;
    return (s.st_mode & S_IFREG) != 0;
}

static int mc_has_ext(McProject *p, const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return 0;
    for (size_t i = 0; i < p->extCount; ++i) {
        if (strcmp(ext, p->extensions[i]) == 0) return 1;
    }
    return 0;
}

static void mc_scan_buffer_for_registries(McProject *p, const char *buf, size_t len, const char *filepath) {
    for (size_t r = 0; r < p->regCount; ++r) {
        const char *mstd = p->registries[r].opts.markers.standard ? p->registries[r].opts.markers.standard : "REGISTER_COUNTER";
        const char *munq = p->registries[r].opts.markers.unique   ? p->registries[r].opts.markers.unique   : "REGISTER_UNIQUE_COUNTER";
        size_t mstd_len = strlen(mstd) + 1;
        size_t munq_len = strlen(munq) + 1;
        char *mstd_pat = (char*)meta_arena_alloc(&p->arena, mstd_len + 1);
        char *munq_pat = (char*)meta_arena_alloc(&p->arena, munq_len + 1);
        snprintf(mstd_pat, mstd_len + 1, "%s(", mstd);
        snprintf(munq_pat, munq_len + 1, "%s(", munq);
        const char *patterns[2] = { mstd_pat, munq_pat };
        int unique_flag[2] = { 0, 1 };
        for (int k = 0; k < 2; ++k) {
            const char *pat = patterns[k];
            size_t pat_len = strlen(pat);
            const char *pos = buf;
            const char *end_buf = buf + len;
            while (pos < end_buf) {
                const char *hit = strstr(pos, pat);
                if (!hit) break;
                const char *start = hit + pat_len;
                const char *end = memchr(start, ')', (size_t)(end_buf - start));
                if (!end) break;
                while (start < end && (*start == ' ' || *start == '\t')) start++;
                const char *pcur = start;
                while (pcur < end && *pcur != ',' && *pcur != ' ' && *pcur != '\t' && *pcur != '\r' && *pcur != '\n') pcur++;
                size_t name_len = (size_t)(pcur - start);
                if (name_len > 0) {
                    char *ident = (char*)meta_arena_alloc(&p->arena, name_len + 1);
                    memcpy(ident, start, name_len);
                    ident[name_len] = '\0';
                    int value = -1;
                    const char *comma = memchr(pcur, ',', (size_t)(end - pcur));
                    if (comma) value = (int)strtol(comma + 1, NULL, 10);
                    int line_num = 1; for (const char *t = buf; t < hit; ++t) if (*t == '\n') line_num++;
                    McIdentifier id = {0};
                    id.name = ident;
                    id.filepath = meta_arena_strdup(&p->arena, filepath);
                    id.line_num = line_num;
                    id.is_unique = unique_flag[k];
                    id.value = value;
                    mc_vec_push_ident(&p->arena, &p->registries[r].ids, &p->registries[r].id_count, &p->registries[r].id_capacity, &id);
                }
                pos = end + 1;
            }
        }
    }
}

static int mc_is_output_header(McProject *p, const char *filepath) {
    for (size_t r = 0; r < p->regCount; ++r) {
        const char *out = p->registries[r].opts.output_header;
        if (out && strcmp(out, filepath) == 0) return 1;
    }
    return 0;
}

static void mc_process_file(McProject *p, const char *filepath) {
    if (mc_is_output_header(p, filepath)) return;
    if (!mc_has_ext(p, filepath)) return;
    FILE *f = fopen(filepath, "rb");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return; }
    char *buf = (char*)meta_arena_alloc(&p->arena, (size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    mc_scan_buffer_for_registries(p, buf, rd, filepath);
}

static void mc_process_directory(McProject *p, const char *dirpath) {
#ifdef _WIN32
    size_t pat_len = strlen(dirpath) + 3;
    char *pattern = (char*)malloc(pat_len);
    snprintf(pattern, pat_len, "%s\\*", dirpath);
    WIN32_FIND_DATA fd; HANDLE h = FindFirstFile(pattern, &fd);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        size_t need = strlen(dirpath) + 1 + strlen(fd.cFileName) + 1;
        char *path = (char*)meta_arena_alloc(&p->arena, need);
        snprintf(path, need, "%s\\%s", dirpath, fd.cFileName);
        if (mc_is_dir(path)) mc_process_directory(p, path);
        else if (mc_is_file(path)) mc_process_file(p, path);
    } while (FindNextFile(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dirpath);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        size_t need = strlen(dirpath) + 1 + strlen(e->d_name) + 1;
        char *path = (char*)meta_arena_alloc(&p->arena, need);
        snprintf(path, need, "%s/%s", dirpath, e->d_name);
        if (mc_is_dir(path)) mc_process_directory(p, path);
        else if (mc_is_file(path)) mc_process_file(p, path);
    }
    closedir(d);
#endif
}

static void mc_process_path(McProject *p, const char *path) {
    if (mc_is_output_header(p, path)) return;
    if (mc_is_dir(path)) mc_process_directory(p, path);
    else if (mc_is_file(path)) mc_process_file(p, path);
}

// ----------------------------- Output --------------------------------------

static void mc_write_header_cpp(FILE *out, const McRegistryOpts *opts, const McIdentifier *final_ids, size_t count, int max_value) {
    fprintf(out, "#ifdef __cplusplus\n\n");
    fprintf(out, "enum class %s : unsigned int {\n", opts->enum_name);
    for (size_t i = 0; i < count; ++i) {
        fprintf(out, "    %s = %d,\n", final_ids[i].name, final_ids[i].value);
    }
    fprintf(out, "    %s = %d\n", opts->count_name, max_value + 1);
    fprintf(out, "};\n\n");
    fprintf(out, "constexpr unsigned int %s_INT = %d;\n\n", opts->count_name, max_value + 1);
    fprintf(out, "inline const char* get_name_for_%s(%s id) {\n", opts->enum_name, opts->enum_name);
    fprintf(out, "    static const char* names[] = {\n");
    for (int i = 0; i <= max_value; ++i) {
        int found = 0;
        for (size_t j = 0; j < count; ++j) {
            if (final_ids[j].value == i) { fprintf(out, "        \"%s\",\n", final_ids[j].name); found = 1; break; }
        }
        if (!found) fprintf(out, "        \"(unused)\",\n");
    }
    fprintf(out, "    };\n");
    fprintf(out, "    unsigned int idx = (unsigned int)id;\n");
    fprintf(out, "    if (idx <= %d) return names[idx];\n", max_value);
    fprintf(out, "    return \"(invalid)\";\n}\n\n");
    fprintf(out, "#define %s(name, ...) %s::name\n", opts->markers.standard, opts->enum_name);
    fprintf(out, "#define %s(name, ...) %s::name\n\n", opts->markers.unique, opts->enum_name);
}

static void mc_write_header_c(FILE *out, const McRegistryOpts *opts, const McIdentifier *final_ids, size_t count, int max_value) {
    fprintf(out, "#else\n\n");
    fprintf(out, "typedef enum {\n");
    for (size_t i = 0; i < count; ++i) {
        fprintf(out, "    %s_%s = %d,\n", opts->enum_name, final_ids[i].name, final_ids[i].value);
    }
    fprintf(out, "    %s_%s = %d\n", opts->enum_name, opts->count_name, max_value + 1);
    fprintf(out, "} %s;\n\n", opts->enum_name);
    fprintf(out, "#define %s_INT %d\n\n", opts->count_name, max_value + 1);
    fprintf(out, "static inline const char* get_name_for_%s(%s id) {\n", opts->enum_name, opts->enum_name);
    fprintf(out, "    static const char* names[] = {\n");
    for (int i = 0; i <= max_value; ++i) {
        int found = 0;
        for (size_t j = 0; j < count; ++j) {
            if (final_ids[j].value == i) { fprintf(out, "        \"%s\",\n", final_ids[j].name); found = 1; break; }
        }
        if (!found) fprintf(out, "        \"(unused)\",\n");
    }
    fprintf(out, "    };\n");
    fprintf(out, "    if ((unsigned int)id <= %d) return names[(unsigned int)id];\n", max_value);
    fprintf(out, "    return \"(invalid)\";\n}\n\n");
    fprintf(out, "#define %s(name, ...) %s_##name\n", opts->markers.standard, opts->enum_name);
    fprintf(out, "#define %s(name, ...) %s_##name\n\n", opts->markers.unique, opts->enum_name);
    fprintf(out, "#endif\n");
}

static int mc_generate_one_registry(McProject *p, McRegistry *reg) {
    // Resolve duplicates and assign values
    McIdentifier *final_ids = NULL; size_t final_count = 0; size_t final_cap = 0;
    int current_value = 0;
    int max_value = -1;
    int error_found = 0;

    for (size_t i = 0; i < reg->id_count; ++i) {
        McIdentifier *cur = &reg->ids[i];
        int found = 0;
        for (size_t j = 0; j < final_count; ++j) {
            if (strcmp(cur->name, final_ids[j].name) == 0) {
                found = 1;
                if (cur->is_unique) {
                    fprintf(stderr, "[ERROR] Unique identifier '%s' redefined.\n  Redefined: %s:%d\n", cur->name, cur->filepath, cur->line_num);
                    error_found = 1;
                } else if (reg->opts.duplicate_policy == MC_DUP_WARN) {
                    fprintf(stdout, "[WARN] Identifier '%s' redefined at %s:%d\n", cur->name, cur->filepath, cur->line_num);
                } else if (reg->opts.duplicate_policy == MC_DUP_ERROR) {
                    fprintf(stderr, "[ERROR] Identifier '%s' redefined at %s:%d\n", cur->name, cur->filepath, cur->line_num);
                    error_found = 1;
                }
                break;
            }
        }
        if (!found) {
            McIdentifier out = *cur;
            if (out.value != -1) current_value = out.value; else out.value = current_value;
            if (current_value > max_value) max_value = current_value;
            current_value++;
            mc_vec_push_ident(&p->arena, &final_ids, &final_count, &final_cap, &out);
        }
    }

    if (error_found) return 0;

    if (!reg->opts.output_header || !reg->opts.enum_name || !reg->opts.count_name) {
        fprintf(stderr, "FATAL: Registry options incomplete (output/enum/count).\n");
        return 0;
    }

    FILE *out = fopen(reg->opts.output_header, "w");
    if (!out) {
        fprintf(stderr, "FATAL: Cannot open output file '%s'\n", reg->opts.output_header);
        return 0;
    }
    fprintf(out, "// THIS FILE IS AUTO-GENERATED BY METACOUNTER. DO NOT EDIT.\n");
    fprintf(out, "#pragma once\n\n");
    fprintf(out, "#include <stdint.h>\n\n");
    mc_write_header_cpp(out, &reg->opts, final_ids, final_count, max_value);
    mc_write_header_c(out, &reg->opts, final_ids, final_count, max_value);
    fclose(out);
    return 1;
}

// ----------------------------- Public API impl -----------------------------

McProject* mc_begin(void) {
    McProject *p = (McProject*)malloc(sizeof(McProject));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    meta_arena_init(&p->arena, 64 * 1024 * 1024);
    return p;
}

bool mc_add_extensions(McProject *p, const char *const *exts, size_t count) {
    if (!p || !exts || count == 0) return false;
    for (size_t i = 0; i < count; ++i) {
        mc_vec_push_str(&p->arena, &p->extensions, &p->extCount, &p->extCapacity, exts[i]);
    }
    return true;
}

bool mc_add_folders(McProject *p, const char *const *folders, size_t count) {
    if (!p || !folders || count == 0) return false;
    for (size_t i = 0; i < count; ++i) {
        mc_vec_push_str(&p->arena, &p->folders, &p->folderCount, &p->folderCapacity, folders[i]);
    }
    return true;
}

bool mc_add_paths(McProject *p, const char *const *paths, size_t count) {
    if (!p || !paths || count == 0) return false;
    for (size_t i = 0; i < count; ++i) {
        mc_vec_push_str(&p->arena, &p->paths, &p->pathCount, &p->pathCapacity, paths[i]);
    }
    return true;
}

bool mc_add_registry(McProject *p, McRegistryOpts opts) {
    if (!p || !opts.output_header) return false;
    if (!opts.enum_name) opts.enum_name = "CounterID";
    if (!opts.count_name) opts.count_name = "MAX_COUNT";
    if (!opts.markers.standard) opts.markers.standard = "REGISTER_COUNTER";
    if (!opts.markers.unique) opts.markers.unique = "REGISTER_UNIQUE_COUNTER";
    McRegistry r; memset(&r, 0, sizeof(r));
    r.opts.output_header = meta_arena_strdup(&p->arena, opts.output_header);
    r.opts.enum_name     = meta_arena_strdup(&p->arena, opts.enum_name);
    r.opts.count_name    = meta_arena_strdup(&p->arena, opts.count_name);
    r.opts.markers.standard = meta_arena_strdup(&p->arena, opts.markers.standard);
    r.opts.markers.unique   = meta_arena_strdup(&p->arena, opts.markers.unique);
    r.opts.duplicate_policy = opts.duplicate_policy;
    mc_vec_push_registry(&p->arena, &p->registries, &p->regCount, &p->regCapacity, &r);
    return true;
}

bool mc_generate(McProject *p) {
    if (!p) return false;
    if (p->extCount == 0) {
        fprintf(stderr, "FATAL: No extensions added.\n");
        return false;
    }
    if (p->regCount == 0) {
        fprintf(stderr, "FATAL: No registries added.\n");
        return false;
    }

    for (size_t i = 0; i < p->folderCount; ++i) mc_process_path(p, p->folders[i]);
    for (size_t i = 0; i < p->pathCount; ++i)   mc_process_path(p, p->paths[i]);

    int ok = 1;
    for (size_t r = 0; r < p->regCount; ++r) ok = ok && mc_generate_one_registry(p, &p->registries[r]);
    return ok ? true : false;
}

// ----------------------------- Self-rebuild --------------------------------

static int meta_stat_mtime(const char *path, time_t *out) {
    struct stat s;
    if (stat(path, &s) != 0) return -1;
    *out = s.st_mtime;
    return 0;
}

bool meta_self_rebuild_(int argc, char **argv, MetaSelfRebuild opts) {
    (void)argc;
    const char *exe = opts.exe_path && opts.exe_path[0] ? opts.exe_path : (argv && argv[0] ? argv[0] : NULL);
    const char *src = opts.src_path && opts.src_path[0] ? opts.src_path : __FILE__;
    const char *cc = opts.cc && opts.cc[0] ? opts.cc : "cc";
    const char *cflags = opts.cflags && opts.cflags[0] ? opts.cflags : "-O2 -Wall -Wextra";
    if (!exe || !src) return true;

    time_t exe_time = 0, src_time = 0, hdr_time = 0;
    if (meta_stat_mtime(exe, &exe_time) != 0) exe_time = 0; // force rebuild if missing
    if (meta_stat_mtime(src, &src_time) != 0) return true;  // no src? do nothing
    // Also watch the public header by default (implicit dependency)
    const char *hdr = "include/metacounter.h";
    if (meta_stat_mtime(hdr, &hdr_time) != 0) hdr_time = src_time; // ignore if missing

    time_t newest_src = src_time > hdr_time ? src_time : hdr_time;

    if (exe_time >= newest_src) return true; // up to date

    char cmd[2048];
#ifdef _WIN32
    // Very basic Windows support: expect a POSIX-like toolchain (e.g. clang/llvm-mingw)
    snprintf(cmd, sizeof(cmd), "%s %s -o \"%s\" \"%s\"", cc, cflags, exe, src);
#else
    snprintf(cmd, sizeof(cmd), "%s %s -o '%s' '%s'", cc, cflags, exe, src);
#endif
    fprintf(stdout, "[meta] Rebuilding self: %s\n", cmd);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "Self-rebuild failed: %s (code %d)\n", cmd, rc);
        return true; // continue without re-exec
    }
#ifndef _WIN32
    execv(exe, argv);
#endif
    return true; // on Windows or if execv fails, continue
}

#ifdef __cplusplus
} // extern "C"
#endif

#endif // METACOUNTER_IMPLEMENTATION


#endif



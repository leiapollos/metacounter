// metacounter.c - A fully configurable counter-generator for C++.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Platform-specific headers
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#define MAX_LINE_LEN 2048
#define CONFIG_FILENAME "metacounter.txt"

// --- Forward Declarations ---
typedef struct Arena Arena;
static void arena_init(Arena* arena, size_t reserve_size_bytes);
static void arena_free(Arena* arena);
static void* arena_alloc(Arena* arena, size_t size);

// --- Data Structures ---
typedef struct {
    char *name;
    char *filepath;
    int line_num;
    int is_unique_request;
    int value;
} IdentifierInfo;

typedef struct {
    FILE* file;
    const char* enum_name;
    const char* count_name;
    const char* marker_std;
    const char* marker_unique;
    const IdentifierInfo* identifiers;
    size_t count;
    int max_value;
} OutputContext;

IdentifierInfo *g_identifiers = NULL;
size_t g_id_count = 0;
size_t g_id_capacity = 0;

char **g_extensions = NULL;
size_t g_ext_count = 0;
size_t g_ext_capacity = 0;

typedef enum { POLICY_IGNORE, POLICY_WARN, POLICY_ERROR } DuplicatePolicy;

DuplicatePolicy g_policy = POLICY_IGNORE;
char g_output_file[MAX_LINE_LEN] = {0};
char g_enum_name[128] = "CounterID";
char g_count_name[128] = "MAX_COUNT";
char g_marker_std[128] = "REGISTER_COUNTER";
char g_marker_unique[128] = "REGISTER_UNIQUE_COUNTER";

Arena g_main_arena;

// --- Core Logic ---
static char* arena_strdup(Arena* arena, const char* s) {
    size_t len = strlen(s) + 1;
    char* new_str = arena_alloc(arena, len);
    if (new_str) {
        memcpy(new_str, s, len);
    }
    return new_str;
}

void add_identifier(const char *name, const char *filepath, int line_num, int is_unique, int value) {
    if (g_id_count >= g_id_capacity) {
        size_t new_capacity = (g_id_capacity == 0) ? 16 : g_id_capacity * 2;
        IdentifierInfo* new_block = arena_alloc(&g_main_arena, new_capacity * sizeof(IdentifierInfo));
        if (g_identifiers) {
            memcpy(new_block, g_identifiers, g_id_count * sizeof(IdentifierInfo));
        }
        g_identifiers = new_block;
        g_id_capacity = new_capacity;
    }
    g_identifiers[g_id_count].name = arena_strdup(&g_main_arena, name);
    g_identifiers[g_id_count].filepath = arena_strdup(&g_main_arena, filepath);
    g_identifiers[g_id_count].line_num = line_num;
    g_identifiers[g_id_count].is_unique_request = is_unique;
    g_identifiers[g_id_count].value = value;
    g_id_count++;
}

void add_extension(const char *ext) {
    if (g_ext_count >= g_ext_capacity) {
        size_t new_capacity = (g_ext_capacity == 0) ? 8 : g_ext_capacity * 2;
        char** new_block = arena_alloc(&g_main_arena, new_capacity * sizeof(char*));
        if (g_extensions) {
            memcpy(new_block, g_extensions, g_ext_count * sizeof(char*));
        }
        g_extensions = new_block;
        g_ext_capacity = new_capacity;
    }
    g_extensions[g_ext_count++] = arena_strdup(&g_main_arena, ext);
}

void parse_line_for_markers(char *line, const char *filepath, int line_num) {
    char marker_std_full[256], marker_unique_full[256];
    snprintf(marker_std_full, sizeof(marker_std_full), "%s(", g_marker_std);
    snprintf(marker_unique_full, sizeof(marker_unique_full), "%s(", g_marker_unique);

    const char *markers[] = {marker_std_full, marker_unique_full};
    for (int i = 0; i < 2; ++i) {
        char *start = strstr(line, markers[i]);
        if (start) {
            start += strlen(markers[i]);
            char *end = strchr(start, ')');
            if (end) {
                char identifier[256] = {0};
                int value = -1;

                while (start < end && (*start == ' ' || *start == '\t')) start++;

                char *p = start;
                while (p < end && *p != ',' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
                
                size_t len = p - start;
                if (len > 0 && len < 255) {
                    strncpy(identifier, start, len);
                    
                    char* comma = strchr(p, ',');
                    if (comma && comma < end) {
                        value = strtol(comma + 1, NULL, 10);
                    }
                    add_identifier(identifier, filepath, line_num, i == 1, value);
                }
            }
        }
    }
}

int has_valid_extension(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return 0;
    for (size_t i = 0; i < g_ext_count; ++i) {
        if (strcmp(ext, g_extensions[i]) == 0) return 1;
    }
    return 0;
}

void process_path(const char *path);

void process_directory(const char *dirpath) {
    char path_buffer[MAX_LINE_LEN];
#ifdef _WIN32
    WIN32_FIND_DATA fd;
    snprintf(path_buffer, sizeof(path_buffer), "%s\\*", dirpath);
    HANDLE hFind = FindFirstFile(path_buffer, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        snprintf(path_buffer, sizeof(path_buffer), "%s\\%s", dirpath, fd.cFileName);
        process_path(path_buffer);
    } while (FindNextFile(hFind, &fd) != 0);
    FindClose(hFind);
#else
    DIR *dir = opendir(dirpath);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        snprintf(path_buffer, sizeof(path_buffer), "%s/%s", dirpath, entry->d_name);
        process_path(path_buffer);
    }
    closedir(dir);
#endif
}

void process_file(const char *filepath) {
    FILE *file = fopen(filepath, "r");
    if (!file) return;
    char line[MAX_LINE_LEN];
    int line_num = 0;
    while (fgets(line, sizeof(line), file)) {
        line_num++;
        parse_line_for_markers(line, filepath, line_num);
    }
    fclose(file);
}

void process_path(const char *path) {
    struct stat s;
    if (stat(path, &s) == 0) {
        if (s.st_mode & S_IFDIR) process_directory(path);
        else if (s.st_mode & S_IFREG && has_valid_extension(path)) process_file(path);
    }
}

void trim(char *str) {
    char *start = str;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
    char *end = str + strlen(str) - 1;
    while (end >= str && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        end--;
    }
    *(end + 1) = '\0';
}

// --- Configuration Handling ---

typedef void (*ConfigHandlerFunc)(const char* value);

typedef struct {
    const char* key;
    ConfigHandlerFunc handler;
} ConfigHandler;

static void handle_output_file(const char* value) {
    strncpy(g_output_file, value, sizeof(g_output_file) - 1);
}
static void handle_enum_name(const char* value) {
    strncpy(g_enum_name, value, sizeof(g_enum_name) - 1);
}
static void handle_count_name(const char* value) {
    strncpy(g_count_name, value, sizeof(g_count_name) - 1);
}
static void handle_marker_std(const char* value) {
    strncpy(g_marker_std, value, sizeof(g_marker_std) - 1);
}
static void handle_marker_unique(const char* value) {
    strncpy(g_marker_unique, value, sizeof(g_marker_unique) - 1);
}

static void handle_duplicate_policy(const char* value) {
    if (strcmp(value, "warn") == 0) g_policy = POLICY_WARN;
    else if (strcmp(value, "error") == 0) g_policy = POLICY_ERROR;
    else g_policy = POLICY_IGNORE;
}

static void handle_scan_ext(const char* value) {
    char* value_copy = arena_strdup(&g_main_arena, value);
    char* ext = strtok(value_copy, " ");
    while (ext) {
        add_extension(ext);
        ext = strtok(NULL, " ");
    }
}

static const ConfigHandler g_config_handlers[] = {
    {"output_file",      handle_output_file},
    {"enum_name",        handle_enum_name},
    {"count_name",       handle_count_name},
    {"marker_standard",  handle_marker_std},
    {"marker_unique",    handle_marker_unique},
    {"duplicate_policy", handle_duplicate_policy},
    {"scan_ext",         handle_scan_ext}
};
static const size_t g_num_config_handlers = sizeof(g_config_handlers) / sizeof(g_config_handlers[0]);

void parse_config(const char *config_path) {
    FILE *file = fopen(config_path, "r");
    if (!file) {
        fprintf(stderr, "FATAL: Cannot open config file '%s'\n", config_path);
        exit(1);
    }

    char line[MAX_LINE_LEN];
    int in_sources_block = 0;

    while (fgets(line, sizeof(line), file)) {
        trim(line);
        if (line[0] == '\0' || line[0] == '#') continue;

        if (strcmp(line, "begin_sources") == 0) {
            in_sources_block = 1;
            continue;
        }
        if (strcmp(line, "end_sources") == 0) {
            in_sources_block = 0;
            continue;
        }
        if (in_sources_block) continue;

        char* separator = strchr(line, ':');
        if (!separator) continue;
        *separator = '\0';

        char* key = line;
        char* value = separator + 1;
        trim(key);
        trim(value);

        for (size_t i = 0; i < g_num_config_handlers; ++i) {
            if (strcmp(key, g_config_handlers[i].key) == 0) {
                g_config_handlers[i].handler(value);
                break;
            }
        }
    }
    fclose(file);
}

// --- Output Generation Functions ---

static void write_header(OutputContext* ctx) {
    fprintf(ctx->file, "// THIS FILE IS AUTO-GENERATED BY METACOUNTER. DO NOT EDIT.\n");
    fprintf(ctx->file, "#pragma once\n\n");
    fprintf(ctx->file, "#include <stdint.h>\n\n");
}

static void write_enum_entries(OutputContext* ctx, const char* prefix,
                              const char* separator) {
    for (size_t i = 0; i < ctx->count; ++i) {
        fprintf(ctx->file, "    %s%s%s = %d,\n",
                prefix ? prefix : "",
                prefix ? separator : "",
                ctx->identifiers[i].name,
                ctx->identifiers[i].value);
    }
    fprintf(ctx->file, "    %s%s%s = %d\n",
            prefix ? prefix : "",
            prefix ? separator : "",
            ctx->count_name,
            ctx->max_value + 1);
}

static void write_name_array(OutputContext* ctx) {
    fprintf(ctx->file, "    static const char* names[] = {\n");
    for (int i = 0; i <= ctx->max_value; ++i) {
        int found = 0;
        for (size_t j = 0; j < ctx->count; ++j) {
            if (ctx->identifiers[j].value == i) {
                fprintf(ctx->file, "        \"%s\",\n", ctx->identifiers[j].name);
                found = 1;
                break;
            }
        }
        if (!found) {
            fprintf(ctx->file, "        \"(unused)\",\n");
        }
    }
    fprintf(ctx->file, "    };\n");
}

static void write_cpp_section(OutputContext* ctx) {
    fprintf(ctx->file, "#ifdef __cplusplus\n\n");
    
    // Enum class
    fprintf(ctx->file, "enum class %s : uint32_t {\n", ctx->enum_name);
    write_enum_entries(ctx, NULL, NULL);
    fprintf(ctx->file, "};\n\n");
    
    // Constant
    fprintf(ctx->file, "constexpr uint32_t %s_INT = %d;\n\n",
            ctx->count_name, ctx->max_value + 1);
    
    // Name lookup function
    fprintf(ctx->file, "inline const char* get_name_for_%s(%s id) {\n",
            ctx->enum_name, ctx->enum_name);
    write_name_array(ctx);
    fprintf(ctx->file, "    if ((uint32_t)id <= %d) return names[(uint32_t)id];\n",
            ctx->max_value);
    fprintf(ctx->file, "    return \"(invalid)\";\n");
    fprintf(ctx->file, "}\n\n");
    
    // Macros
    fprintf(ctx->file, "#define %s(name, ...) %s::name\n",
            ctx->marker_std, ctx->enum_name);
    fprintf(ctx->file, "#define %s(name, ...) %s::name\n\n",
            ctx->marker_unique, ctx->enum_name);
}

static void write_c_section(OutputContext* ctx) {
    fprintf(ctx->file, "#else\n\n");
    
    // Typedef enum
    fprintf(ctx->file, "typedef enum {\n");
    write_enum_entries(ctx, ctx->enum_name, "_");
    fprintf(ctx->file, "} %s;\n\n", ctx->enum_name);
    
    // Constant
    fprintf(ctx->file, "#define %s_INT %d\n\n",
            ctx->count_name, ctx->max_value + 1);
    
    // Name lookup function
    fprintf(ctx->file, "static inline const char* get_name_for_%s(%s id) {\n",
            ctx->enum_name, ctx->enum_name);
    write_name_array(ctx);
    fprintf(ctx->file, "    if (id <= %d) return names[id];\n", ctx->max_value);
    fprintf(ctx->file, "    return \"(invalid)\";\n");
    fprintf(ctx->file, "}\n\n");
    
    // Macros
    fprintf(ctx->file, "#define %s(name, ...) %s_##name\n",
            ctx->marker_std, ctx->enum_name);
    fprintf(ctx->file, "#define %s(name, ...) %s_##name\n\n",
            ctx->marker_unique, ctx->enum_name);
    
    fprintf(ctx->file, "#endif\n");
}

static void generate_output_file(const char* filename,
                                const IdentifierInfo* identifiers,
                                size_t count, int max_value) {
    FILE* file = fopen(filename, "w");
    if (!file) {
        fprintf(stderr, "FATAL: Cannot open output file '%s'\n", filename);
        exit(1);
    }
    
    OutputContext ctx = {
        .file = file,
        .enum_name = g_enum_name,
        .count_name = g_count_name,
        .marker_std = g_marker_std,
        .marker_unique = g_marker_unique,
        .identifiers = identifiers,
        .count = count,
        .max_value = max_value
    };
    
    write_header(&ctx);
    write_cpp_section(&ctx);
    write_c_section(&ctx);
    
    fclose(file);
}

// --- Main Function ---

int main(int argc, char *argv[]) {
    arena_init(&g_main_arena, 64 * 1024 * 1024);
    atexit((void(*)(void))arena_free);

    const char *config_path = (argc > 1) ? argv[1] : CONFIG_FILENAME;
    
    parse_config(config_path);
    if (g_output_file[0] == 0) {
        fprintf(stderr, "FATAL: 'output_file' not set in config.\n");
        return 1;
    }
    if (g_ext_count == 0) {
        fprintf(stderr, "FATAL: 'scan_ext' not set in config.\n");
        return 1;
    }

    remove(g_output_file);

    FILE *config_file = fopen(config_path, "r");
    char line[MAX_LINE_LEN];
    int in_sources_block = 0;
    while (fgets(line, sizeof(line), config_file)) {
        trim(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        if (strcmp(line, "begin_sources") == 0) {
            in_sources_block = 1;
            continue;
        }
        if (strcmp(line, "end_sources") == 0) {
            in_sources_block = 0;
            continue;
        }
        if (in_sources_block) {
            process_path(line);
        }
    }
    fclose(config_file);

    IdentifierInfo *final_list = arena_alloc(&g_main_arena, g_id_count * sizeof(IdentifierInfo));
    size_t final_count = 0;
    int error_found = 0;
    int current_value = 0;
    int max_value = -1;

    for (size_t i = 0; i < g_id_count; ++i) {
        int found = 0;
        for (size_t j = 0; j < final_count; ++j) {
            if (strcmp(g_identifiers[i].name, final_list[j].name) == 0) {
                found = 1;
                if (g_identifiers[i].is_unique_request ||
                    final_list[j].is_unique_request) {
                    fprintf(stderr,
                            "[ERROR] Unique identifier '%s' redefined.\n"
                            "  Original: %s:%d\n  Redefined: %s:%d\n",
                            g_identifiers[i].name,
                            final_list[j].filepath, final_list[j].line_num,
                            g_identifiers[i].filepath, g_identifiers[i].line_num);
                    error_found = 1;
                } else if (g_policy == POLICY_WARN) {
                    fprintf(stdout,
                            "[WARN] Identifier '%s' redefined.\n"
                            "  Original: %s:%d\n  Redefined: %s:%d\n",
                            g_identifiers[i].name,
                            final_list[j].filepath, final_list[j].line_num,
                            g_identifiers[i].filepath, g_identifiers[i].line_num);
                } else if (g_policy == POLICY_ERROR) {
                    fprintf(stderr,
                            "[ERROR] Identifier '%s' redefined.\n"
                            "  Original: %s:%d\n  Redefined: %s:%d\n",
                            g_identifiers[i].name,
                            final_list[j].filepath, final_list[j].line_num,
                            g_identifiers[i].filepath, g_identifiers[i].line_num);
                    error_found = 1;
                }
                break;
            }
        }
        if (!found) {
            final_list[final_count] = g_identifiers[i];
            if (final_list[final_count].value != -1) {
                current_value = final_list[final_count].value;
            } else {
                final_list[final_count].value = current_value;
            }
            if (current_value > max_value) {
                max_value = current_value;
            }
            current_value++;
            final_count++;
        }
    }

    if (error_found) {
        return 1;
    }

    generate_output_file(g_output_file, final_list, final_count, max_value);
    
    printf("Metacounter: Success! Wrote %zu identifiers to %s.\n", final_count, g_output_file);
    return 0;
}

// =============================================================================
// Virtual Memory Arena Implementation
// =============================================================================

struct Arena {
    unsigned char* memory;
    size_t page_size;
    size_t reserved_size;
    size_t committed_size;
    size_t position;
};

static size_t get_page_size() {
#ifdef _WIN32
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    return sysInfo.dwPageSize;
#else
    return sysconf(_SC_PAGESIZE);
#endif
}

static size_t align_up(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

static void arena_init(Arena* arena, size_t reserve_size_bytes) {
    arena->page_size = get_page_size();
    arena->reserved_size = align_up(reserve_size_bytes, arena->page_size);
    arena->committed_size = 0;
    arena->position = 0;

#ifdef _WIN32
    arena->memory = (unsigned char*)VirtualAlloc(NULL, arena->reserved_size, MEM_RESERVE, PAGE_NOACCESS);
#else
    arena->memory = (unsigned char*)mmap(NULL, arena->reserved_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena->memory == MAP_FAILED) arena->memory = NULL;
#endif

    if (arena->memory == NULL) {
        fprintf(stderr, "FATAL: Failed to reserve memory for arena.\n");
        exit(1);
    }
}

static void arena_free(Arena* arena) {
    if (arena && arena->memory) {
#ifdef _WIN32
        VirtualFree(arena->memory, 0, MEM_RELEASE);
#else
        munmap(arena->memory, arena->reserved_size);
#endif
        arena->memory = NULL;
    }
}

static void* arena_alloc(Arena* arena, size_t size) {
    if (size == 0) return NULL;

    size_t new_pos = arena->position + size;
    if (new_pos > arena->reserved_size) {
        fprintf(stderr, "FATAL: Arena out of reserved memory.\n");
        return NULL;
    }

    if (new_pos > arena->committed_size) {
        size_t new_commit_target = align_up(new_pos, arena->page_size);
        new_commit_target = (new_commit_target > arena->reserved_size) ? arena->reserved_size : new_commit_target;
        
        size_t size_to_commit = new_commit_target - arena->committed_size;
        void* commit_start_addr = arena->memory + arena->committed_size;

#ifdef _WIN32
        if (VirtualAlloc(commit_start_addr, size_to_commit, MEM_COMMIT, PAGE_READWRITE) == NULL) {
            fprintf(stderr, "FATAL: Failed to commit memory.\n");
            return NULL;
        }
#else
        if (mprotect(commit_start_addr, size_to_commit, PROT_READ | PROT_WRITE) != 0) {
            fprintf(stderr, "FATAL: Failed to commit memory (mprotect failed).\n");
            return NULL;
        }
#endif
        arena->committed_size = new_commit_target;
    }

    void* result = arena->memory + arena->position;
    arena->position = new_pos;
    return result;
}
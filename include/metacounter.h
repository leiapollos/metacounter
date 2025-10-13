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
} DuplicateHandlingPolicy;

typedef struct CounterProject CounterProject;

typedef struct CounterRegistryMarkers {
    const char *standard;
    const char *unique;
} CounterRegistryMarkers;

typedef struct CounterRegistryOptions {
    const char *output_header;
    const char *enum_name;
    const char *count_name;
    CounterRegistryMarkers markers;
    DuplicateHandlingPolicy duplicate_policy;
} CounterRegistryOptions;

typedef struct SelfRebuildConfiguration {
    const char *cc;
    const char *cflags;
    const char *src_path;
    const char *exe_path;
} SelfRebuildConfiguration;

// Project lifecycle (returns heap-allocated handle; no free needed for short-lived tools)
CounterProject* mc_begin_counter_project(void);

// Add inputs (plural-only). Strings are copied into the project's arena.
bool mc_add_file_extensions(CounterProject *project, const char *const *extensions, size_t count);
bool mc_add_search_directories(CounterProject *project, const char *const *directories, size_t count);
bool mc_add_input_paths(CounterProject *project, const char *const *paths, size_t count);

// Add a registry to generate
bool mc_add_counter_registry(CounterProject *project, CounterRegistryOptions options);

// Run scan once and emit all registries
bool mc_generate_all_registries(CounterProject *project);

// Self-rebuild helper (renamed to avoid copying nob). Returns true when done (or after re-exec).
bool mc_execute_self_rebuild(int argc, char **argv, SelfRebuildConfiguration options);

// Macros
#define MC_EXTS(project, ...) do { const char* _mc_extensions_array[] = { __VA_ARGS__ }; mc_add_file_extensions((project), _mc_extensions_array, sizeof(_mc_extensions_array)/sizeof(_mc_extensions_array[0])); } while (0)
#define MC_FOLDERS(project, ...) do { const char* _mc_directories_array[] = { __VA_ARGS__ }; mc_add_search_directories((project), _mc_directories_array, sizeof(_mc_directories_array)/sizeof(_mc_directories_array[0])); } while (0)
#define MC_PATHS(project, ...) do { const char* _mc_paths_array[] = { __VA_ARGS__ }; mc_add_input_paths((project), _mc_paths_array, sizeof(_mc_paths_array)/sizeof(_mc_paths_array[0])); } while (0)
#define MC_REGISTRY(project, ...) mc_add_counter_registry((project), (CounterRegistryOptions){ __VA_ARGS__ })
#define META_SELF_REBUILD(argc, argv, ...) mc_execute_self_rebuild((argc), (argv), (SelfRebuildConfiguration){ .cc = "cc", .cflags = "-O2 -Wall -Wextra", .src_path = __FILE__, .exe_path = NULL, __VA_ARGS__ })

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

typedef struct LinearMemoryArena {
    unsigned char *memory;
    size_t pageSize;
    size_t reservedSize;
    size_t committedSize;
    size_t position;
} LinearMemoryArena;

static size_t meta_get_page_size(void) {
#ifdef _WIN32
    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    return (size_t)systemInfo.dwPageSize;
#else
    long pageSize = sysconf(_SC_PAGESIZE);
    return (size_t)(pageSize > 0 ? pageSize : 4096);
#endif
}

static size_t meta_align_up(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

static void arena_initialize(LinearMemoryArena *arena, size_t reserveSizeBytes) {
    arena->pageSize = meta_get_page_size();
    arena->reservedSize = meta_align_up(reserveSizeBytes, arena->pageSize);
    arena->committedSize = 0;
    arena->position = 0;
#ifdef _WIN32
    arena->memory = (unsigned char*)VirtualAlloc(NULL, arena->reservedSize, MEM_RESERVE, PAGE_NOACCESS);
#else
    arena->memory = (unsigned char*)mmap(NULL, arena->reservedSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena->memory == MAP_FAILED) arena->memory = NULL;
#endif
    if (!arena->memory) {
        fprintf(stderr, "FATAL: Failed to reserve memory for arena.\n");
        abort();
    }
}

static void *arena_allocate(LinearMemoryArena *arena, size_t size) {
    if (size == 0) return NULL;
    size_t newPosition = arena->position + size;
    if (newPosition > arena->reservedSize) {
        fprintf(stderr, "FATAL: Arena out of reserved memory.\n");
        return NULL;
    }
    if (newPosition > arena->committedSize) {
        size_t newCommitSize = meta_align_up(newPosition, arena->pageSize);
        size_t bytesToCommit = newCommitSize - arena->committedSize;
        void *commitAddress = arena->memory + arena->committedSize;
#ifdef _WIN32
        if (VirtualAlloc(commitAddress, bytesToCommit, MEM_COMMIT, PAGE_READWRITE) == NULL) {
            fprintf(stderr, "FATAL: Failed to commit memory.\n");
            return NULL;
        }
#else
        if (mprotect(commitAddress, bytesToCommit, PROT_READ | PROT_WRITE) != 0) {
            fprintf(stderr, "FATAL: Failed to commit memory (mprotect).\n");
            return NULL;
        }
#endif
        arena->committedSize = newCommitSize;
    }
    void *result = arena->memory + arena->position;
    arena->position = newPosition;
    return result;
}

static char *arena_duplicate_string(LinearMemoryArena *arena, const char *text) {
    size_t count = strlen(text) + 1;
    char *buffer = (char*)arena_allocate(arena, count);
    if (!buffer) return NULL;
    memcpy(buffer, text, count);
    return buffer;
}

// ----------------------------- Internals ----------------------------------

typedef struct CounterIdentifier {
    char *name;
    char *filePath;
    int lineNumber;
    int isUnique;
    int value; // -1 for auto
} CounterIdentifier;

typedef struct CounterRegistry {
    CounterRegistryOptions options;
    CounterIdentifier *identifiers;
    size_t identifierCount;
    size_t identifierCapacity;
} CounterRegistry;

struct CounterProject {
    LinearMemoryArena arena;
    char **extensions;
    size_t extensionCount;
    size_t extensionCapacity;
    char **directories;
    size_t directoryCount;
    size_t directoryCapacity;
    char **inputPaths;
    size_t pathCount;
    size_t pathCapacity;
    CounterRegistry *registries;
    size_t registryCount;
    size_t registryCapacity;
};

static void *arena_grow_copy(LinearMemoryArena *arena, void *oldPointer, size_t oldByteCount, size_t newByteCount) {
    void *data = arena_allocate(arena, newByteCount);
    if (!data) return NULL;
    if (oldPointer && oldByteCount) memcpy(data, oldPointer, oldByteCount);
    return data;
}

static void append_string(LinearMemoryArena *arena, char ***array, size_t *count, size_t *capacity, const char *value) {
    if (*count >= *capacity) {
        size_t newCapacity = (*capacity == 0) ? 8 : (*capacity * 2);
        char **newArray = (char**)arena_grow_copy(arena, *array, (*capacity) * sizeof(char*), newCapacity * sizeof(char*));
        *array = newArray;
        *capacity = newCapacity;
    }
    (*array)[(*count)++] = arena_duplicate_string(arena, value);
}

static void append_counter_definition(LinearMemoryArena *arena, CounterIdentifier **array, size_t *count, size_t *capacity, const CounterIdentifier *identifier) {
    if (*count >= *capacity) {
        size_t newCapacity = (*capacity == 0) ? 16 : (*capacity * 2);
        CounterIdentifier *newArray = (CounterIdentifier*)arena_grow_copy(arena, *array, (*capacity) * sizeof(CounterIdentifier), newCapacity * sizeof(CounterIdentifier));
        *array = newArray;
        *capacity = newCapacity;
    }
    (*array)[(*count)++] = *identifier;
}

static void append_counter_registry(LinearMemoryArena *arena, CounterRegistry **array, size_t *count, size_t *capacity, const CounterRegistry *registry) {
    if (*count >= *capacity) {
        size_t newCapacity = (*capacity == 0) ? 4 : (*capacity * 2);
        CounterRegistry *newArray = (CounterRegistry*)arena_grow_copy(arena, *array, (*capacity) * sizeof(CounterRegistry), newCapacity * sizeof(CounterRegistry));
        *array = newArray;
        *capacity = newCapacity;
    }
    (*array)[(*count)++] = *registry;
}

 

// ----------------------------- FS -----------------------------------------

static int is_directory(const char *path) {
    struct stat stats;
    if (stat(path, &stats) != 0) return 0;
    return (stats.st_mode & S_IFDIR) != 0;
}

static int is_regular_file(const char *path) {
    struct stat stats;
    if (stat(path, &stats) != 0) return 0;
    return (stats.st_mode & S_IFREG) != 0;
}

static int has_supported_extension(CounterProject *project, const char *filename) {
    const char *extension = strrchr(filename, '.');
    if (!extension) return 0;
    for (size_t i = 0; i < project->extensionCount; ++i) {
        if (strcmp(extension, project->extensions[i]) == 0) return 1;
    }
    return 0;
}

static void scan_buffer_for_registry_markers(CounterProject *project, const char *buffer, size_t length, const char *filePath) {
    for (size_t registryIndex = 0; registryIndex < project->registryCount; ++registryIndex) {
        const char *standardMarkerName = project->registries[registryIndex].options.markers.standard ? project->registries[registryIndex].options.markers.standard : "REGISTER_COUNTER";
        const char *uniqueMarkerName = project->registries[registryIndex].options.markers.unique   ? project->registries[registryIndex].options.markers.unique   : "REGISTER_UNIQUE_COUNTER";
        size_t standardMarkerLength = strlen(standardMarkerName) + 1;
        size_t uniqueMarkerLength = strlen(uniqueMarkerName) + 1;
        char *standardMarkerPattern = (char*)arena_allocate(&project->arena, standardMarkerLength + 1);
        char *uniqueMarkerPattern = (char*)arena_allocate(&project->arena, uniqueMarkerLength + 1);
        snprintf(standardMarkerPattern, standardMarkerLength + 1, "%s(", standardMarkerName);
        snprintf(uniqueMarkerPattern, uniqueMarkerLength + 1, "%s(", uniqueMarkerName);
        const char *patterns[2] = { standardMarkerPattern, uniqueMarkerPattern };
        int uniqueFlag[2] = { 0, 1 };
        for (int patternIndex = 0; patternIndex < 2; ++patternIndex) {
            const char *pattern = patterns[patternIndex];
            size_t patternLength = strlen(pattern);
            const char *cursor = buffer;
            const char *endBuffer = buffer + length;
            while (cursor < endBuffer) {
                const char *match = strstr(cursor, pattern);
                if (!match) break;
                const char *valueStart = match + patternLength;
                const char *valueEnd = memchr(valueStart, ')', (size_t)(endBuffer - valueStart));
                if (!valueEnd) break;
                while (valueStart < valueEnd && (*valueStart == ' ' || *valueStart == '\t')) valueStart++;
                const char *valueCursor = valueStart;
                while (valueCursor < valueEnd && *valueCursor != ',' && *valueCursor != ' ' && *valueCursor != '\t' && *valueCursor != '\r' && *valueCursor != '\n') valueCursor++;
                size_t nameLength = (size_t)(valueCursor - valueStart);
                if (nameLength > 0) {
                    char *identifierName = (char*)arena_allocate(&project->arena, nameLength + 1);
                    memcpy(identifierName, valueStart, nameLength);
                    identifierName[nameLength] = '\0';
                    int explicitValue = -1;
                    const char *comma = memchr(valueCursor, ',', (size_t)(valueEnd - valueCursor));
                    if (comma) explicitValue = (int)strtol(comma + 1, NULL, 10);
                    int lineNumber = 1;
                    for (const char *positionTracker = buffer; positionTracker < match; ++positionTracker) if (*positionTracker == '\n') lineNumber++;
                    CounterIdentifier identifier = {0};
                    identifier.name = identifierName;
                    identifier.filePath = arena_duplicate_string(&project->arena, filePath);
                    identifier.lineNumber = lineNumber;
                    identifier.isUnique = uniqueFlag[patternIndex];
                    identifier.value = explicitValue;
                    append_counter_definition(&project->arena, &project->registries[registryIndex].identifiers, &project->registries[registryIndex].identifierCount, &project->registries[registryIndex].identifierCapacity, &identifier);
                }
                cursor = valueEnd + 1;
            }
        }
    }
}

static int is_output_header_path(CounterProject *project, const char *filePath) {
    for (size_t registryIndex = 0; registryIndex < project->registryCount; ++registryIndex) {
        const char *outputPath = project->registries[registryIndex].options.output_header;
        if (outputPath && strcmp(outputPath, filePath) == 0) return 1;
    }
    return 0;
}

static void process_file_path(CounterProject *project, const char *filePath) {
    if (is_output_header_path(project, filePath)) return;
    if (!has_supported_extension(project, filePath)) return;
    FILE *fileHandle = fopen(filePath, "rb");
    if (!fileHandle) return;
    if (fseek(fileHandle, 0, SEEK_END) != 0) { fclose(fileHandle); return; }
    long size = ftell(fileHandle);
    if (size < 0) { fclose(fileHandle); return; }
    if (fseek(fileHandle, 0, SEEK_SET) != 0) { fclose(fileHandle); return; }
    char *buffer = (char*)arena_allocate(&project->arena, (size_t)size + 1);
    if (!buffer) { fclose(fileHandle); return; }
    size_t readBytes = fread(buffer, 1, (size_t)size, fileHandle);
    fclose(fileHandle);
    buffer[readBytes] = '\0';
    scan_buffer_for_registry_markers(project, buffer, readBytes, filePath);
}

static void process_directory_path(CounterProject *project, const char *directoryPath) {
#ifdef _WIN32
    size_t patternLength = strlen(directoryPath) + 3;
    char *pattern = (char*)malloc(patternLength);
    snprintf(pattern, patternLength, "%s\\*", directoryPath);
    WIN32_FIND_DATA findData;
    HANDLE handle = FindFirstFile(pattern, &findData);
    free(pattern);
    if (handle == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(findData.cFileName, ".") == 0 || strcmp(findData.cFileName, "..") == 0) continue;
        size_t requiredLength = strlen(directoryPath) + 1 + strlen(findData.cFileName) + 1;
        char *path = (char*)arena_allocate(&project->arena, requiredLength);
        snprintf(path, requiredLength, "%s\\%s", directoryPath, findData.cFileName);
        if (is_directory(path)) process_directory_path(project, path);
        else if (is_regular_file(path)) process_file_path(project, path);
    } while (FindNextFile(handle, &findData));
    FindClose(handle);
#else
    DIR *directory = opendir(directoryPath);
    if (!directory) return;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        size_t requiredLength = strlen(directoryPath) + 1 + strlen(entry->d_name) + 1;
        char *path = (char*)arena_allocate(&project->arena, requiredLength);
        snprintf(path, requiredLength, "%s/%s", directoryPath, entry->d_name);
        if (is_directory(path)) process_directory_path(project, path);
        else if (is_regular_file(path)) process_file_path(project, path);
    }
    closedir(directory);
#endif
}

static void process_input_path(CounterProject *project, const char *path) {
    if (is_output_header_path(project, path)) return;
    if (is_directory(path)) process_directory_path(project, path);
    else if (is_regular_file(path)) process_file_path(project, path);
}

// ----------------------------- Output --------------------------------------

static void write_counter_header_cpp(FILE *out, const CounterRegistryOptions *options, const CounterIdentifier *identifiers, size_t count, int maxValue) {
    fprintf(out, "#ifdef __cplusplus\n\n");
    fprintf(out, "enum class %s : unsigned int {\n", options->enum_name);
    for (size_t i = 0; i < count; ++i) {
        fprintf(out, "    %s = %d,\n", identifiers[i].name, identifiers[i].value);
    }
    fprintf(out, "    %s = %d\n", options->count_name, maxValue + 1);
    fprintf(out, "};\n\n");
    fprintf(out, "constexpr unsigned int %s_INT = %d;\n\n", options->count_name, maxValue + 1);
    fprintf(out, "inline const char* get_name_for_%s(%s id) {\n", options->enum_name, options->enum_name);
    fprintf(out, "    static const char* names[] = {\n");
    for (int value = 0; value <= maxValue; ++value) {
        int found = 0;
        for (size_t j = 0; j < count; ++j) {
            if (identifiers[j].value == value) { fprintf(out, "        \"%s\",\n", identifiers[j].name); found = 1; break; }
        }
        if (!found) fprintf(out, "        \"(unused)\",\n");
    }
    fprintf(out, "    };\n");
    fprintf(out, "    unsigned int index = (unsigned int)id;\n");
    fprintf(out, "    if (index <= %d) return names[index];\n", maxValue);
    fprintf(out, "    return \"(invalid)\";\n}\n\n");
    fprintf(out, "#define %s(name, ...) %s::name\n", options->markers.standard, options->enum_name);
    fprintf(out, "#define %s(name, ...) %s::name\n\n", options->markers.unique, options->enum_name);
}

static void write_counter_header_c(FILE *out, const CounterRegistryOptions *options, const CounterIdentifier *identifiers, size_t count, int maxValue) {
    fprintf(out, "#else\n\n");
    fprintf(out, "typedef enum {\n");
    for (size_t i = 0; i < count; ++i) {
        fprintf(out, "    %s_%s = %d,\n", options->enum_name, identifiers[i].name, identifiers[i].value);
    }
    fprintf(out, "    %s_%s = %d\n", options->enum_name, options->count_name, maxValue + 1);
    fprintf(out, "} %s;\n\n", options->enum_name);
    fprintf(out, "#define %s_INT %d\n\n", options->count_name, maxValue + 1);
    fprintf(out, "static inline const char* get_name_for_%s(%s id) {\n", options->enum_name, options->enum_name);
    fprintf(out, "    static const char* names[] = {\n");
    for (int value = 0; value <= maxValue; ++value) {
        int found = 0;
        for (size_t j = 0; j < count; ++j) {
            if (identifiers[j].value == value) { fprintf(out, "        \"%s\",\n", identifiers[j].name); found = 1; break; }
        }
        if (!found) fprintf(out, "        \"(unused)\",\n");
    }
    fprintf(out, "    };\n");
    fprintf(out, "    if ((unsigned int)id <= %d) return names[(unsigned int)id];\n", maxValue);
    fprintf(out, "    return \"(invalid)\";\n}\n\n");
    fprintf(out, "#define %s(name, ...) %s_##name\n", options->markers.standard, options->enum_name);
    fprintf(out, "#define %s(name, ...) %s_##name\n\n", options->markers.unique, options->enum_name);
    fprintf(out, "#endif\n");
}

static int generate_single_registry(CounterProject *project, CounterRegistry *registry) {
    CounterIdentifier *finalIdentifiers = NULL;
    size_t finalCount = 0;
    size_t finalCapacity = 0;
    int currentValue = 0;
    int maxValue = -1;
    int errorFound = 0;

    for (size_t i = 0; i < registry->identifierCount; ++i) {
        CounterIdentifier *current = &registry->identifiers[i];
        int duplicateFound = 0;
        for (size_t j = 0; j < finalCount; ++j) {
            if (strcmp(current->name, finalIdentifiers[j].name) == 0) {
                duplicateFound = 1;
                if (current->isUnique) {
                    fprintf(stderr, "[ERROR] Unique identifier '%s' redefined.\n  Redefined: %s:%d\n", current->name, current->filePath, current->lineNumber);
                    errorFound = 1;
                } else if (registry->options.duplicate_policy == MC_DUP_WARN) {
                    fprintf(stdout, "[WARNING] Identifier '%s' redefined at %s:%d\n", current->name, current->filePath, current->lineNumber);
                } else if (registry->options.duplicate_policy == MC_DUP_ERROR) {
                    fprintf(stderr, "[ERROR] Identifier '%s' redefined at %s:%d\n", current->name, current->filePath, current->lineNumber);
                    errorFound = 1;
                }
                break;
            }
        }
        if (!duplicateFound) {
            CounterIdentifier output = *current;
            if (output.value != -1) currentValue = output.value; else output.value = currentValue;
            if (currentValue > maxValue) maxValue = currentValue;
            currentValue++;
            append_counter_definition(&project->arena, &finalIdentifiers, &finalCount, &finalCapacity, &output);
        }
    }

    if (errorFound) return 0;

    if (!registry->options.output_header || !registry->options.enum_name || !registry->options.count_name) {
        fprintf(stderr, "FATAL: Registry options incomplete (output/enum/count).\n");
        return 0;
    }

    FILE *out = fopen(registry->options.output_header, "w");
    if (!out) {
        fprintf(stderr, "FATAL: Cannot open output file '%s'\n", registry->options.output_header);
        return 0;
    }
    fprintf(out, "// THIS FILE IS AUTO-GENERATED BY METACOUNTER. DO NOT EDIT.\n");
    fprintf(out, "#pragma once\n\n");
    fprintf(out, "#include <stdint.h>\n\n");
    write_counter_header_cpp(out, &registry->options, finalIdentifiers, finalCount, maxValue);
    write_counter_header_c(out, &registry->options, finalIdentifiers, finalCount, maxValue);
    fclose(out);
    return 1;
}

// ----------------------------- Public API impl -----------------------------

CounterProject* mc_begin_counter_project(void) {
    CounterProject *p = (CounterProject*)malloc(sizeof(CounterProject));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    arena_initialize(&p->arena, 64 * 1024 * 1024);
    return p;
}

bool mc_add_file_extensions(CounterProject *p, const char *const *extensions, size_t count) {
    if (!p || !extensions || count == 0) return false;
    for (size_t i = 0; i < count; ++i) {
        append_string(&p->arena, &p->extensions, &p->extensionCount, &p->extensionCapacity, extensions[i]);
    }
    return true;
}

bool mc_add_search_directories(CounterProject *p, const char *const *directories, size_t count) {
    if (!p || !directories || count == 0) return false;
    for (size_t i = 0; i < count; ++i) {
        append_string(&p->arena, &p->directories, &p->directoryCount, &p->directoryCapacity, directories[i]);
    }
    return true;
}

bool mc_add_input_paths(CounterProject *p, const char *const *paths, size_t count) {
    if (!p || !paths || count == 0) return false;
    for (size_t i = 0; i < count; ++i) {
        append_string(&p->arena, &p->inputPaths, &p->pathCount, &p->pathCapacity, paths[i]);
    }
    return true;
}

bool mc_add_counter_registry(CounterProject *p, CounterRegistryOptions options) {
    if (!p || !options.output_header) return false;
    if (!options.enum_name) options.enum_name = "CounterID";
    if (!options.count_name) options.count_name = "MAX_COUNT";
    if (!options.markers.standard) options.markers.standard = "REGISTER_COUNTER";
    if (!options.markers.unique) options.markers.unique = "REGISTER_UNIQUE_COUNTER";
    CounterRegistry r; memset(&r, 0, sizeof(r));
    r.options.output_header = arena_duplicate_string(&p->arena, options.output_header);
    r.options.enum_name     = arena_duplicate_string(&p->arena, options.enum_name);
    r.options.count_name    = arena_duplicate_string(&p->arena, options.count_name);
    r.options.markers.standard = arena_duplicate_string(&p->arena, options.markers.standard);
    r.options.markers.unique   = arena_duplicate_string(&p->arena, options.markers.unique);
    r.options.duplicate_policy = options.duplicate_policy;
    append_counter_registry(&p->arena, &p->registries, &p->registryCount, &p->registryCapacity, &r);
    return true;
}

bool mc_generate_all_registries(CounterProject *p) {
    if (!p) return false;
    if (p->extensionCount == 0) {
        fprintf(stderr, "FATAL: No extensions added.\n");
        return false;
    }
    if (p->registryCount == 0) {
        fprintf(stderr, "FATAL: No registries added.\n");
        return false;
    }

    for (size_t i = 0; i < p->directoryCount; ++i) process_input_path(p, p->directories[i]);
    for (size_t i = 0; i < p->pathCount; ++i)   process_input_path(p, p->inputPaths[i]);

    int ok = 1;
    for (size_t r = 0; r < p->registryCount; ++r) ok = ok && generate_single_registry(p, &p->registries[r]);
    return ok ? true : false;
}

// ----------------------------- Self-rebuild --------------------------------

static int meta_stat_mtime(const char *path, time_t *out) {
    struct stat s;
    if (stat(path, &s) != 0) return -1;
    *out = s.st_mtime;
    return 0;
}

bool mc_execute_self_rebuild(int argc, char **argv, SelfRebuildConfiguration options) {
    const char *exePath = options.exe_path && options.exe_path[0] ? options.exe_path : (argv && argv[0] ? argv[0] : NULL);
    const char *sourcePath = options.src_path && options.src_path[0] ? options.src_path : __FILE__;
    const char *compiler = options.cc && options.cc[0] ? options.cc : "cc";
    const char *compilerFlags = options.cflags && options.cflags[0] ? options.cflags : "-O2 -Wall -Wextra";
    if (!exePath || !sourcePath) return true;

    time_t executableTime = 0;
    time_t sourceTime = 0;
    time_t headerTime = 0;
    if (meta_stat_mtime(exePath, &executableTime) != 0) executableTime = 0;
    if (meta_stat_mtime(sourcePath, &sourceTime) != 0) return true;
    const char *publicHeader = "include/metacounter.h";
    if (meta_stat_mtime(publicHeader, &headerTime) != 0) headerTime = sourceTime;

    time_t newestSourceTimestamp = sourceTime > headerTime ? sourceTime : headerTime;

    if (executableTime >= newestSourceTimestamp) return true;

    char command[2048];
#ifdef _WIN32
    snprintf(command, sizeof(command), "%s %s -o \"%s\" \"%s\"", compiler, compilerFlags, exePath, sourcePath);
#else
    snprintf(command, sizeof(command), "%s %s -o '%s' '%s'", compiler, compilerFlags, exePath, sourcePath);
#endif
    fprintf(stdout, "[meta] Rebuilding self: %s\n", command);
    int result = system(command);
    if (result != 0) {
        fprintf(stderr, "Self-rebuild failed: %s (code %d)\n", command, result);
        return true;
    }
#ifndef _WIN32
    execv(exePath, argv);
#endif
    return true;
}

#ifdef __cplusplus
} // extern "C"
#endif

#endif // METACOUNTER_IMPLEMENTATION


#endif



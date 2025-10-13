#define METACOUNTER_IMPLEMENTATION
#include "../include/metacounter.h"

int main(int argc, char **argv) {
    // Rebuild the generator if the source or header changes before continuing.
    META_SELF_REBUILD(argc, argv);

    CounterProject *project = mc_begin_counter_project();
    MC_EXTS(project, ".h", ".hpp", ".c", ".cpp");
    MC_FOLDERS(project, "src");

    MC_REGISTRY(project, .output_header = "src/generated_counter_registry.h",
                    .enum_name = "CounterID",
                    .count_name = "MAX_COUNT",
                    .markers = { "REGISTER_COUNTER", "REGISTER_UNIQUE_COUNTER" },
                    .duplicate_policy = MC_DUP_WARN);

    return mc_generate_all_registries(project) ? 0 : 1;
}



#define METACOUNTER_IMPLEMENTATION
#include "../include/metacounter.h"

int main(int argc, char **argv) {
    META_SELF_REBUILD(argc, argv);

    CounterProject *projectContext = mc_begin_counter_project();
    MC_EXTS(projectContext, ".h", ".hpp", ".c", ".cpp");
    MC_FOLDERS(projectContext, "src");

    MC_REGISTRY(projectContext, .outputHeaderPath = "src/generated_counter_registry.h",
                    .enumTypeName = "CounterID",
                    .countConstantName = "MAX_COUNT",
                    .markerNames = { "REGISTER_COUNTER", "REGISTER_UNIQUE_COUNTER" },
                    .duplicatePolicy = MC_DUP_WARN);

    return mc_generate_all_registries(projectContext) ? 0 : 1;
}



#define METACOUNTER_IMPLEMENTATION
#include "../include/metacounter.h"

int main(int argc, char **argv) {
    META_SELF_REBUILD(argc, argv);

    McProject *p = mc_begin();
    MC_EXTS(p, ".h", ".hpp", ".c", ".cpp");
    MC_FOLDERS(p, "src");

    MC_REGISTRY(p, .output_header = "src/generated_counter_registry.h",
                    .enum_name = "CounterID",
                    .count_name = "MAX_COUNT",
                    .markers = { "REGISTER_COUNTER", "REGISTER_UNIQUE_COUNTER" },
                    .duplicate_policy = MC_DUP_WARN);

    return mc_generate(p) ? 0 : 1;
}



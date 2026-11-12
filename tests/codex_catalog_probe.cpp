// codex_catalog_probe — dumps the signed-in account's LIVE ChatGPT (Codex)
// /models catalog: slug, display_name, context_window, priority, default.
//
// NO_TEST on purpose: it prints server-reported metadata for a human to
// compare against the bundled fallback and the codex-rs embedded catalog.
// It requires real ChatGPT OAuth credentials, so it can never run in CI.
// Build and run explicitly:
//   cmake --build build --target codex_catalog_probe && ./build/codex_catalog_probe

#include <cstdio>

#include "agentty/provider/chatgpt/responses.hpp"
#include "agentty/util/io.hpp"

namespace cc = agentty::provider::chatgpt;

int main() {
    auto models = cc::fetch_models(::agentty::IoAccess::grant(),
                                   ::maya::test_root());
    if (models.empty()) {
        std::printf("live catalog unavailable (not signed in or offline)\n");
        return 1;
    }
    std::printf("%-24s %-16s %10s %8s %s\n", "slug", "display", "ctx_window",
                "priority", "default");
    for (const auto& m : models) {
        std::printf("%-24s %-16s %10d %8d %s\n", m.slug.c_str(),
                    m.display_name.c_str(), m.context_window, m.priority,
                    m.is_default ? "yes" : "");
    }
    return 0;
}

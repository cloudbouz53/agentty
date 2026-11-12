// metadata_throughput_test — two audit regressions, in one place because
// they share a shape: a number shown to the user must never be MORE precise
// than the evidence behind it.
//
//   1. CONTEXT WINDOWS. The live Codex /models catalog is parsed into
//      CatalogModel rows whose context_window used to DEFAULT to 272'000.
//      Any slug whose server row omitted (or mis-typed) the field then
//      displayed "272k" as if the endpoint had reported it — fabricated
//      precision. The authoritative shape (codex-rs 0.167.0) is optional:
//      a slug can legitimately have no served window. Missing/invalid must
//      parse to 0 (unknown → the picker shows "auto"); valid values pass
//      through untouched; out-of-range values are rejected, not clamped
//      into a plausible-looking wrong number.
//
//   2. THROUGHPUT. The billed `output_tokens` figure INCLUDES reasoning
//      tokens on every provider that reports them separately. One rate
//      labelled "output" conflated "tokens the user watched appear" with
//      "tokens billed" — on a thinking turn those differ by several ×.
//      The fold now records both series and the stats panel labels them
//      distinctly ("Generated rate" incl. reasoning, "Visible rate").
//
// Purity: both halves are functions of their input payload/telemetry and
// nothing else — no Model, no wire, no clock.

#include <string>
#include <vector>

#include "agtest.hpp"

#include "agentty/domain/stats/facts.hpp"
#include "agentty/domain/stats/tabs.hpp"
#include "agentty/provider/chatgpt/responses.hpp"
#include "agentty/runtime/view/helpers.hpp"

using namespace agentty;
namespace cc = agentty::provider::chatgpt;
namespace st = agentty::stats;

// ── 1. Catalog metadata ──────────────────────────────────────────────────

TEST_CASE("chatgpt catalog: absent or invalid context windows stay unknown") {
    const auto models = cc::parse_models_payload_for_test(R"({"models":[
        {"slug":"no-window"},
        {"slug":"zero",   "context_window":0},
        {"slug":"neg",    "context_window":-5},
        {"slug":"string", "context_window":"abc"},
        {"slug":"null",   "context_window":null},
        {"slug":"huge",   "context_window":25000000},
        {"slug":"ok",     "context_window":272000},
        {"slug":"terra",  "context_window":500000}
    ]})");
    REQUIRE(models.size() == 8);
    // Every row the endpoint declined to describe truthfully is UNKNOWN (0),
    // which the picker renders as "auto" — never a hard-coded 272k.
    for (const auto& slug : {"no-window", "zero", "neg", "string",
                             "null", "huge"}) {
        bool found = false;
        for (const auto& m : models)
            if (m.slug == slug) {
                found = true;
                CAPTURE(m.slug);
                CHECK(m.context_window == 0);
            }
        CHECK(found);
    }
    // A value the endpoint DID report passes through untouched.
    const auto win = [&](std::string_view slug) -> int {
        for (const auto& m : models)
            if (m.slug == slug) return m.context_window;
        return -1;
    };
    CHECK(win("ok") == 272000);
    CHECK(win("terra") == 500000);
}

TEST_CASE("chatgpt catalog: unknown windows render as unknown, not invented") {
    // The picker's context column: 0 → "auto" (names the state), a real
    // window → the same k/M formatting the status bar uses.
    CHECK(ui::context_window_label(0) == "auto");
    CHECK(ui::context_window_label(272'000) == "272k");
    CHECK(ui::context_window_label(500'000) == "500k");
    CHECK(ui::context_window_label(872'000) == "872k");
    CHECK(ui::context_window_label(1'000'000) == "1M");
}

TEST_CASE("context ladder: an unknown chatgpt window falls back conservatively") {
    // With the fabricated 272k default gone, a slug the server did not
    // describe falls through the ladder to the conservative default. That
    // is correct on both sides of the asymmetry: too LOW only compacts
    // earlier than strictly necessary; too HIGH fails the turn on the wire.
    store::Settings s;
    CHECK(ui::resolve_context_window("chatgpt", "gpt-9-unknown-slug", 0, s)
          == ui::kDefaultContextWindow);
    // And a window the server DID report is used verbatim (rung 2 outranks
    // every static source).
    CHECK(ui::resolve_context_window("chatgpt", "gpt-6.1-terra", 500'000, s)
          == 500'000);
}

// ── 2. Throughput accounting ─────────────────────────────────────────────

namespace {

// An assistant message with no telemetry — the "not measured" case (a turn
// that predates the field, or was interrupted before any accounting).
Message unrouted(const char* model) {
    Message m;
    m.role = Role::Assistant;
    m.served_model = ModelId{model};
    return m;
}

Message measured(std::uint32_t out, std::uint32_t reasoning,
                 std::uint32_t stream_ms) {
    Message m;
    m.role = Role::Assistant;
    m.served_model = ModelId{"m1"};
    Message::Telemetry t;
    t.output_tokens    = out;
    t.reasoning_tokens = reasoning;
    t.stream_ms        = stream_ms;
    m.telemetry = t;
    return m;
}

st::Facts folded(std::vector<Message> msgs) {
    Thread th;
    th.id = agentty::ThreadId{"t1"};
    th.messages = std::move(msgs);
    st::Projection p;
    return p.refresh(th);
}

// The "Rates" section of the stream tab: stream_rates() is only reachable
// through the section table.
std::vector<st::Metric> rates_of(const st::Facts& f) {
    std::vector<st::Metric> out;
    for (const auto& sec : st::sections::stream)
        if (sec.heading == "Rates") sec.extract(f, out);
    return out;
}

const st::Metric* find_metric(const std::vector<st::Metric>& ms,
                              std::string_view label) {
    for (const auto& m : ms)
        if (m.label == label) return &m;
    return nullptr;
}

}  // namespace

TEST_CASE("throughput: total generated rate includes reasoning; visible does not") {
    // 400 visible + 600 reasoning over 2 s of generation time. Billed
    // output = 1000 → 500 tok/s; the user watched 400 appear → 200 tok/s.
    const auto f = folded({measured(1000, 600, 2000)});
    REQUIRE(f.stream.rate_series.size() == 1);
    CHECK(f.stream.rate_series[0] == doctest::Approx(500.0));
    REQUIRE(f.stream.visible_rate_series.size() == 1);
    CHECK(f.stream.visible_rate_series[0] == doctest::Approx(200.0));

    const auto ms = rates_of(f);
    const auto* gen = find_metric(ms, "Generated rate");
    const auto* vis = find_metric(ms, "Visible rate");
    REQUIRE(gen != nullptr);
    REQUIRE(vis != nullptr);
    CHECK(gen->value == doctest::Approx(500.0));
    CHECK(vis->value == doctest::Approx(200.0));
    // The labels carry the distinction, not just the numbers.
    CHECK(gen->detail.find("reasoning") != std::string::npos);
    CHECK(vis->detail.find("reasoning") == std::string::npos);
}

TEST_CASE("throughput: a non-thinking turn has exactly one rate") {
    const auto f = folded({measured(300, 0, 1000)});
    REQUIRE(f.stream.rate_series.size() == 1);
    CHECK(f.stream.rate_series[0] == doctest::Approx(300.0));
    CHECK(f.stream.visible_rate_series.empty());
    const auto ms = rates_of(f);
    CHECK(find_metric(ms, "Generated rate") != nullptr);
    CHECK(find_metric(ms, "Visible rate") == nullptr);
}

TEST_CASE("throughput: a provider double-counting reasoning cannot go negative") {
    // If reasoning arrives OUTSIDE output_tokens (a broken wire), the
    // visible rate must simply be absent — never negative, never folded in.
    const auto f = folded({measured(400, 900, 1000)});
    REQUIRE(f.stream.rate_series.size() == 1);   // billed rate is still real
    CHECK(f.stream.visible_rate_series.empty());
}

TEST_CASE("throughput: zero-duration and token-less turns fold nothing") {
    // stream_ms == 0 (interrupted before the first byte, or a clock that
    // refused to advance) divides by nothing — the turn is excluded from
    // the averages, not counted as zero.
    const auto f = folded({measured(500, 100, 0)});
    CHECK(f.stream.rate_series.empty());
    CHECK(f.stream.visible_rate_series.empty());
    const auto f2 = folded({measured(0, 0, 1000)});
    CHECK(f2.stream.rate_series.empty());
}

TEST_CASE("throughput: unmeasured turns are excluded, not zeroed") {
    const auto f = folded({measured(1000, 600, 2000),
                           unrouted("m1")});   // no telemetry at all
    REQUIRE(f.stream.rate_series.size() == 1);
    CHECK(f.stream.rate_series[0] == doctest::Approx(500.0));
    REQUIRE(f.stream.visible_rate_series.size() == 1);
    CHECK(f.stream.visible_rate_series[0] == doctest::Approx(200.0));
}

TEST_CASE("throughput: merge carries the visible series across projections") {
    auto f = folded({measured(1000, 600, 2000)});
    st::Facts g;
    g.fold(measured(2000, 0, 1000));
    f.merge(g);
    REQUIRE(f.stream.rate_series.size() == 2);
    CHECK(f.stream.visible_rate_series.size() == 1);
}

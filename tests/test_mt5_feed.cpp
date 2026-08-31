#include <cstdio>
#include <string>

#include "test_framework.h"

#include "src/feed/mt5_bar_feed.h"

using namespace hftarb;

namespace {

void writeAppend(const char* path, const std::string& text) {
    FILE* f = std::fopen(path, "a");
    if (!f) return;
    std::fputs(text.c_str(), f);
    std::fclose(f);
}

void removeFile(const char* path) { std::remove(path); }

}  // namespace

TEST(mt5_feed_parses_line) {
    const Mt5Bar b =
        Mt5BarFeed::parseLine("{\"i\":\"XAUUSD\",\"p\":4441.45,\"o\":4400.0,"
                              "\"h\":4450.0,\"l\":4395.0,\"t\":1735689600000}");
    CHECK(b.instrument == "XAUUSD");
    CHECK(b.close == 4441.45);
    CHECK(b.open == 4400.0);
    CHECK(b.high == 4450.0);
    CHECK(b.low == 4395.0);
    CHECK(b.tsMs == 1735689600000);
}

TEST(mt5_feed_rejects_garbage) {
    const Mt5Bar a = Mt5BarFeed::parseLine("not json at all");
    CHECK(a.instrument.empty());
    const Mt5Bar b = Mt5BarFeed::parseLine("{\"i\":\"\",\"p\":10}");
    CHECK(b.instrument.empty());
    const Mt5Bar c = Mt5BarFeed::parseLine("{\"i\":\"XAUUSD\",\"p\":-3}");
    CHECK(c.instrument.empty());
}

TEST(mt5_feed_polls_and_dedupes) {
    const char* path = "mt5_feed_test.jsonl";
    removeFile(path);
    Mt5BarFeed feed(path);
    int delivered = 0;
    std::string lastInstr;
    feed.setCallback([&](const Mt5Bar& b) {
        ++delivered;
        lastInstr = b.instrument;
    });

    feed.poll();  // nothing yet (file missing) — no crash
    CHECK(delivered == 0);

    // Pre-existing history must be treated as past (start-from-tail), not
    // replayed to the engines.
    writeAppend(path, "{\"i\":\"XAUUSD\",\"p\":4400.0,\"t\":1000}\n");
    writeAppend(path, "{\"i\":\"XAGUSD\",\"p\":66.5,\"t\":2000}\n");
    feed.poll();
    CHECK(delivered == 0);

    // Newly appended bars are delivered; a rewritten duplicate is not.
    writeAppend(path, "{\"i\":\"XAUUSD\",\"p\":4400.5,\"t\":3000}\n");
    writeAppend(path, "{\"i\":\"XAUUSD\",\"p\":4400.5,\"t\":3000}\n");
    feed.poll();
    CHECK(delivered == 1);
    CHECK(lastInstr == "XAUUSD");

    // Appending a fresh bar to that same file keeps flowing.
    writeAppend(path, "{\"i\":\"XAGUSD\",\"p\":66.7,\"t\":4000}\n");
    feed.poll();
    CHECK(delivered == 2);

    removeFile(path);
}
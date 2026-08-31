#include "test_framework.h"

#include "src/execution/paper_executor.h"

using namespace hftarb;

static Opportunity makeOpp(double buyPrice, double sellPrice, double qty = 0.6) {
    Opportunity o;
    o.symbol = "BTC/USDT";
    o.buyExchange = "A";
    o.sellExchange = "B";
    o.quantity = qty;
    o.cost.buyPrice = buyPrice;
    o.cost.sellPrice = sellPrice;
    o.cost.buyFee = 0.001;
    o.cost.sellFee = 0.001;
    o.cost.notional = buyPrice * qty;
    return o;
}

TEST(paper_full_fill_math) {
    // Fully deterministic case: no rejections, no partials, no drift, no
    // adverse selection. Realized PnL must be the textbook figure.
    PaperConfig c;
    c.fillRateBuy = 1.0;
    c.fillRateSell = 1.0;
    c.partialProbability = 0.0;
    c.rejectProbability = 0.0;
    c.priceMoveBp = 0.0;
    c.adverseEdgeFrac = 0.0;
    PaperExecutor ex(c, 1u);

    const auto r = ex.execute(makeOpp(100.0, 101.0, 1.0));
    CHECK(r.fullyFilled);
    CHECK_NEAR(r.exposureLeft, 0.0, 1e-12);
    // matched*(sell-buy) minus taker fees on each leg.
    CHECK_NEAR(r.realizedPnl, 1.0 * (101.0 - 100.0) - (100.0 * 0.001 + 101.0 * 0.001), 1e-9);
}

TEST(paper_avoids_phantom_loss_on_partial_fill) {
    // If only the buy leg fills, the inventory is carried, not realized.
    // With a tiny buy fill the executor still books the fee, but the leftover
    // base is open exposure, not a loss.
    PaperConfig c;
    c.fillRateBuy = 1.0;
    c.fillRateSell = 0.0;   // sell leg never fills
    c.partialProbability = 0.0;
    c.rejectProbability = 0.0;
    c.priceMoveBp = 0.0;
    c.adverseEdgeFrac = 0.0;
    PaperExecutor ex(c, 2u);

    const auto r = ex.execute(makeOpp(100.0, 101.0, 1.0));
    CHECK(!r.fullyFilled);
    CHECK(r.exposureLeft > 0.0);      // 1.0 unit of base still on the books
    CHECK(r.realizedPnl >= -1e-9);    // no realized loss on unclosed inventory
}

TEST(paper_adverse_selection_eats_the_edge) {
    // Same setup, same seed; the only difference is how much of the observed
    // edge decays while the legs execute. Adverse selection must materially
    // shrink cumulative PnL — that is the realistic model of competition.
    const int n = 4000;
    double sumNaive = 0.0, sumAdverse = 0.0;

    {
        PaperConfig c;
        c.adverseEdgeFrac = 0.0;
        PaperExecutor ex(c, 7u);
        for (int i = 0; i < n; ++i) {
            sumNaive += ex.execute(makeOpp(100000.0, 100800.0)).realizedPnl;
        }
    }
    {
        PaperConfig c;
        c.adverseEdgeFrac = 0.5;
        PaperExecutor ex(c, 7u);
        for (int i = 0; i < n; ++i) {
            sumAdverse += ex.execute(makeOpp(100000.0, 100800.0)).realizedPnl;
        }
    }

    // 80bp gross edge survives fees when nothing moves against us...
    CHECK(sumNaive > 0.0);
    // ...but ~half of it decays adversely, cutting realized PnL drastically.
    CHECK(sumAdverse < sumNaive * 0.6);
}

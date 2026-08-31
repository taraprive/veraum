set(HFT_TEST_SOURCES
  main.cpp
  test_cost.cpp
  test_order_book.cpp
  test_liquidity.cpp
  test_portfolio.cpp
  test_risk.cpp
  test_arbitrage.cpp
  test_multi_strategy.cpp
  test_triangular.cpp
  test_market_state.cpp
  test_paper.cpp
  test_signal.cpp
  test_directional.cpp
  test_mt5_feed.cpp
  test_hmac.cpp
  test_binance_live.cpp
)

add_executable(hft_tests ${HFT_TEST_SOURCES})
target_link_libraries(hft_tests PRIVATE hft_core)

add_test(NAME hft_tests COMMAND hft_tests)

add_executable(test_bybit_balance test_bybit_balance.cpp)
target_link_libraries(test_bybit_balance PRIVATE hft_core)

add_executable(test_okx_depth test_okx_depth.cpp)
target_link_libraries(test_okx_depth PRIVATE hft_core)

add_executable(test_okx_balance test_okx_balance.cpp)
target_link_libraries(test_okx_balance PRIVATE hft_core)

add_executable(test_ws_probe test_ws_probe.cpp)
target_link_libraries(test_ws_probe PRIVATE hft_core)

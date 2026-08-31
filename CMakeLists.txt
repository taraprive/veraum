cmake_minimum_required(VERSION 3.16)
project(hft_arbitrage_bot CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(HFT_CORE_SOURCES
  src/core/order_book.cpp
  src/engine/arbitrage_engine.cpp
  src/engine/cost_engine.cpp
  src/engine/directional_strategy.cpp
  src/engine/liquidity_engine.cpp
  src/engine/multi_strategy_engine.cpp
  src/engine/portfolio.cpp
  src/engine/risk_engine.cpp
  src/engine/triangular_strategy.cpp
  src/exchange/binance_adapter.cpp
  src/exchange/bybit_adapter.cpp
  src/exchange/mexc_adapter.cpp
  src/exchange/okx_adapter.cpp
  src/execution/paper_executor.cpp
  src/execution/real_executor.cpp
  src/feed/mt5_bar_feed.cpp
  src/market/market_state.cpp
  src/market/mock_adapter.cpp
  src/market/rest_feed_adapter.cpp
  src/market/ws_feed_adapter.cpp
  src/monitor/latency_monitor.cpp
  src/monitor/watchdog.cpp
  src/net/http_client.cpp
  src/signal/signal_service.cpp
  src/net/ws_client.cpp
  src/util/config.cpp
  src/util/hmac_sha256.cpp
  src/util/logger.cpp
)

add_library(hft_core STATIC ${HFT_CORE_SOURCES})
target_include_directories(hft_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party)
if(WIN32)
  target_link_libraries(hft_core PUBLIC winhttp bcrypt)
endif()
if(MSVC)
  target_compile_options(hft_core PRIVATE /W4)
  target_compile_options(hft_core PUBLIC /EHsc)
else()
  target_compile_options(hft_core PRIVATE -Wall -Wextra)
endif()

add_executable(hft_arbitrage_bot src/main.cpp)
target_link_libraries(hft_arbitrage_bot PRIVATE hft_core)

# Console tool: honest backtest of the directional LONG/SHORT model on a real
# OHLC price series (CSV). The model is a transparent EMA-cross engine; the
# tool reports win-rate / PnL / drawdown without embellishment.
add_executable(hft_backtest tools/backtest_directional.cpp)
target_link_libraries(hft_backtest PRIVATE hft_core)

enable_testing()
add_subdirectory(tests)

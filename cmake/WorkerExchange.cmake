# Qt Core transport only; neither archive is installed.
add_library(putgd_provider_exchange STATIC src/storage/worker/ProviderExchange.cpp)
target_link_libraries(putgd_provider_exchange PUBLIC putgd_core PRIVATE Qt6::Core)
add_library(putgd_worker_stdio STATIC src/storage/worker/WorkerStdio.cpp)
target_link_libraries(putgd_worker_stdio PUBLIC putgd_core PRIVATE Qt6::Core Threads::Threads)

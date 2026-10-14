# Network check: realmd login + realm list (and later the world session) with the engine's net code. PC only.
add_executable(vd_netcheck main.cpp)
target_link_libraries(vd_netcheck PRIVATE vd_net vd_game)
vd_set_target_options(vd_netcheck)

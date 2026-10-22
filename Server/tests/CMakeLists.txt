add_executable(ServerTests
        ServerTestsMain.cpp
        SerialTaskQueueTest.cpp
        OutboundQueueTest.cpp
        ServerIntegrationTest.cpp
)

target_link_libraries(ServerTests PRIVATE server_core)
target_include_directories(ServerTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src")

add_test(NAME ServerTests COMMAND ServerTests)

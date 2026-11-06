# serenitid: session supervisor (boot order, supervision, lifecycle).

add_executable(serenitid serenity_manager.cpp)
target_link_libraries(serenitid PRIVATE LibCore LibMain)
set_target_properties(serenitid PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

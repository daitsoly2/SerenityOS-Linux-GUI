set(SOURCES
    Client.cpp
    Listener.cpp
)

if (NOT SERENITYOS)
    compile_ipc(../../Services/ConfigServer/ConfigClient.ipc ../../Services/ConfigServer/ConfigClientEndpoint.h)
    compile_ipc(../../Services/ConfigServer/ConfigServer.ipc ../../Services/ConfigServer/ConfigServerEndpoint.h)
endif()

set(GENERATED_SOURCES
    ../../Services/ConfigServer/ConfigServerEndpoint.h
    ../../Services/ConfigServer/ConfigClientEndpoint.h
)

serenity_lib(LibConfig config)
target_link_libraries(LibConfig PRIVATE LibCore LibIPC)

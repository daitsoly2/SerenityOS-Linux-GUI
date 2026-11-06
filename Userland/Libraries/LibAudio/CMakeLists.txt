set(SOURCES
    GenericTypes.cpp
    SampleFormats.cpp
    Loader.cpp
    WavLoader.cpp
    FlacLoader.cpp
    FlacWriter.cpp
    WavWriter.cpp
    Metadata.cpp
    MP3Loader.cpp
    PlaybackStream.cpp
    QOALoader.cpp
    QOATypes.cpp
    UserSampleQueue.cpp
    VorbisComment.cpp
)

if (SERENITYOS)
    list(APPEND SOURCES ConnectionToServer.cpp)
    list(APPEND SOURCES ConnectionToManagerServer.cpp)
    list(APPEND SOURCES PlaybackStreamSerenity.cpp)
    set(GENERATED_SOURCES
        ../../Services/AudioServer/AudioClientEndpoint.h
        ../../Services/AudioServer/AudioServerEndpoint.h
        ../../Services/AudioServer/AudioManagerClientEndpoint.h
        ../../Services/AudioServer/AudioManagerServerEndpoint.h
    )
elseif (ENABLE_LINUX_GUI_PORT)
    # The IPC client half works unchanged on the host: it only needs LibIPC and
    # the endpoints, both of which the Linux port already builds. The device
    # itself is /dev/audio/0, provided by the serenity_compat kernel module.
    list(APPEND SOURCES ConnectionToServer.cpp)
    list(APPEND SOURCES ConnectionToManagerServer.cpp)

    set(AUDIO_IPC_DIR ${SERENITY_PROJECT_ROOT}/Userland/Services/AudioServer)
    set(AUDIO_ENDPOINT_DIR ${CMAKE_BINARY_DIR}/Userland/Services/AudioServer)
    foreach(endpoint AudioClient AudioServer AudioManagerClient AudioManagerServer)
        compile_ipc(${AUDIO_IPC_DIR}/${endpoint}.ipc ${AUDIO_ENDPOINT_DIR}/${endpoint}Endpoint.h)
        list(APPEND GENERATED_SOURCES ${AUDIO_ENDPOINT_DIR}/${endpoint}Endpoint.h)
    endforeach()
endif()

if (HAVE_PULSEAUDIO)
    list(APPEND SOURCES
        PlaybackStreamPulseAudio.cpp
        PulseAudioWrappers.cpp
    )
endif()

serenity_lib(LibAudio audio)
target_link_libraries(LibAudio PRIVATE LibCore LibRIFF LibIPC LibThreading LibUnicode LibCrypto)

if (HAVE_PULSEAUDIO)
    target_link_libraries(LibAudio PRIVATE pulse)
    target_compile_definitions(LibAudio PRIVATE HAVE_PULSEAUDIO=1)
endif()

if (APPLE)
    target_sources(LibAudio PRIVATE PlaybackStreamAudioUnit.cpp)

    find_library(AUDIO_UNIT AudioUnit REQUIRED)
    target_link_libraries(LibAudio PRIVATE ${AUDIO_UNIT})
endif()

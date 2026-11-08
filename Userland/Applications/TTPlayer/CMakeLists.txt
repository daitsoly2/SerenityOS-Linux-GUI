serenity_component(
    TTPlayer
    RECOMMENDED
    TARGETS TTPlayer
    DEPENDS AudioServer
)

set(SOURCES
    EqualizerWindow.cpp
    FlatSlider.cpp
    LedDisplay.cpp
    LyricsView.cpp
    LyricsWindow.cpp
    MainWindow.cpp
    MarqueeLabel.cpp
    PlayerCore.cpp
    PlaylistStore.cpp
    PlaylistWindow.cpp
    SkinButton.cpp
    SkinCanvas.cpp
    SkinDocument.cpp
    SkinPanel.cpp
    SkinText.cpp
    SkinTheme.cpp
    TTSkin.cpp
    SpectrumWidget.cpp
    TransportBar.cpp
    ../SoundPlayer/M3UParser.cpp
    ../SoundPlayer/PlaybackManager.cpp
    main.cpp
)

serenity_app(TTPlayer ICON app-ttplayer)
target_link_libraries(TTPlayer PRIVATE LibAudio LibConfig LibCore LibFileSystem LibDSP LibGfx LibGUI LibIPC LibMain LibTextCodec LibThreading LibURL)

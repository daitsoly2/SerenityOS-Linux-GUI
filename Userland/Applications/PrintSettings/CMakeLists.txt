serenity_component(
    PrintSettings
    REQUIRED
    TARGETS PrintSettings
)

set(SOURCES
    main.cpp
)

# "打印管理": CUPS-backed printer and job management for the Linux GUI port.
# Meaningful only on hosts with CUPS installed; compiles everywhere.
serenity_app(PrintSettings ICON app-settings)
target_link_libraries(PrintSettings PRIVATE LibCore LibGfx LibGUI LibMain)

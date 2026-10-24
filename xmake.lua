-- include subprojects
includes("lib/commonlibsse-ng")

-- set project constants
set_project("MantellaQuestChecks")
set_version("1.0.0")
set_license("MIT")
set_languages("c++23")
set_warnings("allextra")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- define targets
target("MantellaQuestChecks")
    add_rules("commonlibsse-ng.plugin", {
        name = "MantellaQuestChecks",
        author = "Mantella",
        description = "Answers Mantella's quest tag checks inside the game"
    })

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

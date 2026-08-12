# Set a more convenient ARM flag
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64" OR CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    set(ARM64_ARCH TRUE)
    message(STATUS "ARM64 architecture detected")
else()
    set(ARM64_ARCH FALSE)
    message(STATUS "Non-ARM64 architecture detected")
endif()

if(ARM64_ARCH)
    # Add your ARM64-specific configuration or build options here
    set(OG_ASM_FUNCS_FILE kernel/asm_funcs_arm64.s)
    enable_language(ASM)
    set(CMAKE_ASM_SOURCE_FILE_EXTENSIONS ${CMAKE_ASM_SOURCE_FILE_EXTENSIONS} s)
    set_source_files_properties(${OG_ASM_FUNCS_FILE} PROPERTIES COMPILE_FLAGS "-arch arm64 -g")
else()
    set(OG_ASM_FUNCS_FILE kernel/asm_funcs_x86_64.asm)
    enable_language(ASM_NASM)
    set(CMAKE_ASM_NASM_SOURCE_FILE_EXTENSIONS ${CMAKE_ASM_NASM_SOURCE_FILE_EXTENSIONS} asm)
    set(CMAKE_ASM_NASM_COMPILE_OBJECT "<CMAKE_ASM_NASM_COMPILER> <INCLUDES> -f ${CMAKE_ASM_NASM_OBJECT_FORMAT} -o <OBJECT> <SOURCE>")
    set_source_files_properties(${OG_ASM_FUNCS_FILE} PROPERTIES COMPILE_FLAGS "-g")
endif()

set(RUNTIME_SOURCE
        external/discord_jak2.cpp
        external/discord.cpp
        graphics/display.cpp
        graphics/gfx_test.cpp
        graphics/gfx.cpp
        graphics/jak2_texture_remap.cpp
        graphics/screenshot.cpp
        graphics/opengl_renderer/background/background_common.cpp
        graphics/opengl_renderer/background/Hfrag.cpp
        graphics/opengl_renderer/background/Shrub.cpp
        graphics/opengl_renderer/background/TFragment.cpp
        graphics/opengl_renderer/background/Tie3.cpp
        graphics/opengl_renderer/BlitDisplays.cpp
        graphics/opengl_renderer/BucketRenderer.cpp
        graphics/opengl_renderer/CollideMeshRenderer.cpp
        graphics/opengl_renderer/debug_gui.cpp
        graphics/opengl_renderer/DepthCue.cpp
        graphics/opengl_renderer/DirectRenderer.cpp
        graphics/opengl_renderer/DirectRenderer2.cpp
        graphics/opengl_renderer/dma_helpers.cpp
        graphics/opengl_renderer/EyeRenderer.cpp
        graphics/opengl_renderer/foreground/Generic2_Build.cpp
        graphics/opengl_renderer/foreground/Generic2_DMA.cpp
        graphics/opengl_renderer/foreground/Generic2_OpenGL.cpp
        graphics/opengl_renderer/foreground/Generic2.cpp
        graphics/opengl_renderer/foreground/Generic2BucketRenderer.cpp
        graphics/opengl_renderer/foreground/Merc2.cpp
        graphics/opengl_renderer/foreground/Merc2BucketRenderer.cpp
        graphics/opengl_renderer/foreground/Shadow2.cpp
        graphics/opengl_renderer/loader/Loader.cpp
        graphics/opengl_renderer/loader/LoaderStages.cpp
        graphics/opengl_renderer/ocean/CommonOceanRenderer.cpp
        graphics/opengl_renderer/ocean/OceanEnvmap.cpp
        graphics/opengl_renderer/ocean/OceanMid_PS2.cpp
        graphics/opengl_renderer/ocean/OceanMid.cpp
        graphics/opengl_renderer/ocean/OceanMidAndFar.cpp
        graphics/opengl_renderer/ocean/OceanNear_PS2.cpp
        graphics/opengl_renderer/ocean/OceanNear.cpp
        graphics/opengl_renderer/ocean/OceanTexture_PC.cpp
        graphics/opengl_renderer/ocean/OceanTexture.cpp
        graphics/opengl_renderer/opengl_utils.cpp
        graphics/opengl_renderer/OpenGLRenderer.cpp
        graphics/opengl_renderer/Profiler.cpp
        graphics/opengl_renderer/ProgressRenderer.cpp
        graphics/opengl_renderer/Shader.cpp
        graphics/opengl_renderer/Shadow_PS2.cpp
        graphics/opengl_renderer/ShadowRenderer.cpp
        graphics/opengl_renderer/SkyBlendCPU.cpp
        graphics/opengl_renderer/SkyBlendGPU.cpp
        graphics/opengl_renderer/SkyRenderer.cpp
        graphics/opengl_renderer/SlowTimeEffect.cpp
        graphics/opengl_renderer/sprite/GlowRenderer.cpp
        graphics/opengl_renderer/sprite/Sprite3_Distort.cpp
        graphics/opengl_renderer/sprite/Sprite3_Glow.cpp
        graphics/opengl_renderer/sprite/Sprite3.cpp
        graphics/opengl_renderer/TextureAnimator.cpp
        graphics/opengl_renderer/TextureAnimatorDefs.cpp
        graphics/opengl_renderer/TextureUploadHandler.cpp
        graphics/opengl_renderer/VisDataHandler.cpp
        graphics/opengl_renderer/Warp.cpp
        graphics/pipelines/opengl.cpp
        graphics/sceGraphicsInterface.cpp
        graphics/texture/jak2_tpage_dir.cpp
        graphics/texture/TextureConverter.cpp
        graphics/texture/TexturePool.cpp
        ${OG_ASM_FUNCS_FILE}
        kernel/common/fileio.cpp
        kernel/common/kboot.cpp
        kernel/common/kdgo.cpp
        kernel/common/kdsnetm.cpp
        kernel/common/klink.cpp
        kernel/common/klisten.cpp
        kernel/common/kmachine.cpp
        kernel/common/kmalloc.cpp
        kernel/common/kmemcard.cpp
        kernel/common/kprint.cpp
        kernel/common/kscheme.cpp
        kernel/common/ksocket.cpp
        kernel/common/ksound.cpp
        kernel/jak2/fileio.cpp
        kernel/jak2/kboot.cpp
        kernel/jak2/kdgo.cpp
        kernel/jak2/klink.cpp
        kernel/jak2/klisten.cpp
        kernel/jak2/kmachine.cpp
        kernel/jak2/kmachine_extras.cpp
        kernel/jak2/kmalloc.cpp
        kernel/jak2/kprint.cpp
        kernel/jak2/kscheme.cpp
        kernel/jak2/ksound.cpp
        mips2c/jak2_functions/bones.cpp
        mips2c/jak2_functions/collide_cache.cpp
        mips2c/jak2_functions/collide_edge_grab.cpp
        mips2c/jak2_functions/collide_func.cpp
        mips2c/jak2_functions/collide_hash.cpp
        mips2c/jak2_functions/collide_mesh.cpp
        mips2c/jak2_functions/debug.cpp
        mips2c/jak2_functions/font.cpp
        mips2c/jak2_functions/foreground.cpp
        mips2c/jak2_functions/generic_effect.cpp
        mips2c/jak2_functions/generic_merc.cpp
        mips2c/jak2_functions/joint.cpp
        mips2c/jak2_functions/lights.cpp
        mips2c/jak2_functions/merc_blend_shape.cpp
        mips2c/jak2_functions/nav_control.cpp
        mips2c/jak2_functions/nav_mesh.cpp
        mips2c/jak2_functions/ocean_vu0.cpp
        mips2c/jak2_functions/ocean.cpp
        mips2c/jak2_functions/ripple.cpp
        mips2c/jak2_functions/shadow.cpp
        mips2c/jak2_functions/sky.cpp
        mips2c/jak2_functions/sparticle_launcher.cpp
        mips2c/jak2_functions/sparticle.cpp
        mips2c/jak2_functions/spatial_hash.cpp
        mips2c/jak2_functions/squid.cpp
        mips2c/jak2_functions/texture.cpp
        mips2c/jak2_functions/vortex.cpp
        mips2c/mips2c_table.cpp
        overlord/common/dma.cpp
        overlord/common/fake_iso.cpp
        overlord/common/iso_api.cpp
        overlord/common/iso.cpp
        overlord/common/isocommon.cpp
        overlord/common/989_plugins/plugin_strv.cpp
        overlord/common/overlord.cpp
        overlord/common/sbank.cpp
        overlord/common/soundcommon.cpp
        overlord/common/srpc.cpp
        overlord/common/ssound.cpp
        overlord/jak2/dma.cpp
        overlord/jak2/iso_api.cpp
        overlord/jak2/iso_cd.cpp
        overlord/jak2/iso_queue.cpp
        overlord/jak2/iso.cpp
        overlord/jak2/list.cpp
        overlord/jak2/overlord.cpp
        overlord/jak2/pages.cpp
        overlord/jak2/989_plugins/plugin_strv.cpp
        overlord/jak2/ramdisk.cpp
        overlord/jak2/spustreams.cpp
        overlord/jak2/srpc.cpp
        overlord/jak2/ssound.cpp
        overlord/jak2/stream.cpp
        overlord/jak2/streamlfo.cpp
        overlord/jak2/streamlist.cpp
        overlord/jak2/vag.cpp
        runtime.cpp
        sce/deci2.cpp
        sce/iop.cpp
        sce/libcdvd_ee.cpp
        sce/libdma.cpp
        sce/libgraph.cpp
        sce/libpad.cpp
        sce/libscf.cpp
        sce/sif_ee_memcard.cpp
        sce/sif_ee.cpp
        sce/stubs.cpp
        settings/settings.cpp
        system/background_worker.cpp
        system/Deci2Server.cpp
        system/hid/devices/dualsense_effects.cpp
        system/hid/devices/game_controller.cpp
        system/hid/devices/keyboard.cpp
        system/hid/devices/mouse.cpp
        system/hid/display_manager.cpp
        system/hid/input_bindings.cpp
        system/hid/input_manager.cpp
        system/hid/sdl_util.cpp
        system/IOP_Kernel.cpp
        system/iop_thread.cpp
        system/SystemThread.cpp
        tools/filter_menu/filter_menu.cpp
        tools/subtitle_editor/subtitle_editor_db.cpp
        tools/subtitle_editor/subtitle_editor_repl_client.cpp
        tools/subtitle_editor/subtitle_editor.cpp
        )

if(OPENGOAL_GRAPHICS_BACKEND STREQUAL "METAL")
    # Metal has its own display/pipeline implementation. Keep the OpenGL
    # source list available for the default build, but do not compile it into
    # a Metal runtime.
    list(FILTER RUNTIME_SOURCE EXCLUDE REGEX "^graphics/opengl_renderer/")
    list(FILTER RUNTIME_SOURCE EXCLUDE REGEX "^graphics/pipelines/opengl\\.cpp$")
    list(FILTER RUNTIME_SOURCE EXCLUDE REGEX "^graphics/texture/")
    list(FILTER RUNTIME_SOURCE EXCLUDE REGEX "^graphics/gfx_test\\.cpp$")
    list(APPEND RUNTIME_SOURCE
        graphics/gfx_test_metal.cpp
        graphics/pipelines/metal.mm
        graphics/metal/MetalContext.mm
        graphics/metal/MetalRuntimeStubs.cpp)
else()
    list(APPEND RUNTIME_SOURCE "../third-party/glad/src/glad.c")
endif()

if(OG_TARGET_IOS)
    list(FILTER RUNTIME_SOURCE EXCLUDE REGEX "^external/discord")
    list(FILTER RUNTIME_SOURCE EXCLUDE REGEX "^tools/")
    list(APPEND RUNTIME_SOURCE external/discord_ios_stub.cpp)
endif()

add_subdirectory(sound)

# we build the runtime as a static library.
add_library(runtime STATIC ${RUNTIME_SOURCE})

target_link_libraries(runtime common fmt SDL3::SDL3 imgui sound stb_image libco libcurl)
if(NOT OG_TARGET_IOS)
    target_link_libraries(runtime discord-rpc)
endif()
if(APPLE AND OPENGOAL_GRAPHICS_BACKEND STREQUAL "METAL")
    target_link_libraries(runtime "-framework Foundation" "-framework Metal" "-framework QuartzCore")
endif()
if(WIN32)
    target_link_libraries(runtime mman)
else()
    target_link_libraries(runtime pthread dl)
endif()

add_executable(gk main.cpp)
target_link_libraries(gk runtime)
if(OPENGOAL_GRAPHICS_BACKEND STREQUAL "METAL")
    if(CMAKE_OSX_SYSROOT MATCHES "iphonesimulator")
        set(OG_METAL_SDK iphonesimulator)
    elseif(CMAKE_SYSTEM_NAME MATCHES "iOS|tvOS|watchOS")
        set(OG_METAL_SDK iphoneos)
    else()
        set(OG_METAL_SDK macosx)
    endif()
    set(OG_METAL_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/graphics/metal/shaders")
    set(OG_METAL_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/metal")
    set(OG_METAL_AIR "${OG_METAL_OUTPUT_DIR}/synthetic_basic.air")
    set(OG_METAL_LIBRARY "${OG_METAL_OUTPUT_DIR}/OpenGOAL.metallib")
    add_custom_command(
        OUTPUT "${OG_METAL_AIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OG_METAL_OUTPUT_DIR}"
        COMMAND xcrun -sdk "${OG_METAL_SDK}" metal -Werror -c
                "${OG_METAL_SHADER_DIR}/synthetic_basic.metal"
                -o "${OG_METAL_AIR}"
        DEPENDS "${OG_METAL_SHADER_DIR}/synthetic_basic.metal"
        VERBATIM)
    add_custom_command(
        OUTPUT "${OG_METAL_LIBRARY}"
        COMMAND xcrun -sdk "${OG_METAL_SDK}" metallib "${OG_METAL_AIR}"
                -o "${OG_METAL_LIBRARY}"
        DEPENDS "${OG_METAL_AIR}"
        VERBATIM)
    add_custom_target(og-metal-shaders DEPENDS "${OG_METAL_LIBRARY}")
    add_dependencies(gk og-metal-shaders)
    set_source_files_properties("${OG_METAL_LIBRARY}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources/metal")
    target_sources(gk PRIVATE "${OG_METAL_LIBRARY}")
endif()
if(APPLE AND (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64" OR CMAKE_OSX_ARCHITECTURES MATCHES "arm64"))
    set_target_properties(gk PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
        MACOSX_RPATH TRUE
        BUILD_RPATH "@loader_path/../Frameworks"
        INSTALL_RPATH "@executable_path/../Frameworks"
    )
    if(OG_EXECUTION_MODE_DYNAMIC)
        set_target_properties(gk PROPERTIES
            XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/macos/jak-jit.entitlements")
    endif()
endif()
if(WIN32)
    set_target_properties(gk PROPERTIES VS_DPI_AWARE "PerMonitor")
endif()

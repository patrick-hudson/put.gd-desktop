# The putgd-record helper and the tests built from its sources.
# Screen recording helper (#50): GStreamer acquisition and FFmpeg export live in
# this transient process only; the resident never links a media library.
option(PUTGD_RECORDING "Build the putgd-record screen recording helper when the GStreamer and FFmpeg development libraries are present" ON)
if(PUTGD_RECORDING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  pkg_check_modules(PUTGD_GST IMPORTED_TARGET gstreamer-1.0>=1.24 gstreamer-video-1.0>=1.24 gstreamer-app-1.0>=1.24)
  pkg_check_modules(PUTGD_AV IMPORTED_TARGET libavcodec>=60.31 libavformat>=60.16 libavfilter>=9.12 libavutil>=58.29 libswscale>=7.5 libswresample>=4.12)
  if(PUTGD_GST_FOUND AND PUTGD_AV_FOUND)
    add_executable(putgd-record src/record/main.cpp src/record/ClickCheck.cpp src/record/MediaProbe.cpp src/record/EncoderCheck.cpp src/record/BundledRuntime.cpp src/record/PortalScreenCast.cpp src/record/MutterScreenCast.cpp src/record/WaylandAcquisition.cpp src/record/X11Targets.cpp src/record/AdaptiveRate.cpp src/record/RecordingPipeline.cpp src/record/PauseTimeline.cpp src/record/ClipExporter.cpp src/record/Poster.cpp src/record/RecordSession.cpp src/record/Microphones.cpp src/record/ClickOverlay.cpp src/record/X11Clicks.cpp src/record/Overlays.cpp src/record/record.qrc src/record/BarPlacement.cpp src/record/ReviewWindow.cpp src/record/DebugLog.cpp src/record/AvSync.cpp src/core/RecordingModel.cpp)
    # wlr-capture (#1275): ext-image-copy-capture frames into appsrc on compositors
    # with no screencast service (Budgie on labwc, Magpie).
    target_sources(putgd-record PRIVATE src/record/ImageCopySource.cpp ${putgd_capture_protocol_sources})
    target_include_directories(putgd-record PRIVATE src "${protocol_dir}")
    target_link_libraries(putgd-record PRIVATE PkgConfig::PUTGD_WAYLAND PkgConfig::PUTGD_XCB_XINPUT)
    # Scroll capture (#1376), the helper's second session kind: the same sources
    # into a frame tap and a stitcher, and one injector per desktop (XTest, Mutter
    # RemoteDesktop, KWin fake input, a wlroots virtual pointer).
    target_sources(putgd-record PRIVATE src/core/ScrollStitcher.cpp src/record/ScrollRun.cpp src/record/FrameTap.cpp src/record/ScrollBar.cpp
      src/record/ScrollCapture.cpp src/record/ScrollInjectors.cpp src/record/X11ScrollInjector.cpp src/record/X11EscapeWatch.cpp src/platform/wayland/VirtualPointer.cpp)
    target_link_libraries(putgd-record PRIVATE PkgConfig::PUTGD_XCB_XTEST)
    target_link_libraries(putgd-record PRIVATE putgd_build_options putgd_build_identity putgd_ui_control putgd_dialog_attachment putgd_x11_native_snapshot putgd_host_open putgd_visual putgd_fonts putgd_file_dialog Qt6::Core Qt6::Gui Qt6::Widgets Qt6::DBus PkgConfig::PUTGD_GST PkgConfig::PUTGD_AV PkgConfig::PUTGD_XCB)
    if(TARGET putgd_kde)
      # KWin's screencast protocol (vendored XML) for the dialog-free KDE path.
      set(kde_screencast_protocol "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/kde/protocols/zkde-screencast-unstable-v1.xml")
      add_custom_command(OUTPUT "${protocol_dir}/zkde-screencast-unstable-v1-client-protocol.h"
        COMMAND "${PUTGD_WAYLAND_SCANNER}" client-header "${kde_screencast_protocol}" "${protocol_dir}/zkde-screencast-unstable-v1-client-protocol.h"
        DEPENDS "${kde_screencast_protocol}" VERBATIM)
      add_custom_command(OUTPUT "${protocol_dir}/zkde-screencast-unstable-v1-protocol.c"
        COMMAND "${PUTGD_WAYLAND_SCANNER}" private-code "${kde_screencast_protocol}" "${protocol_dir}/zkde-screencast-unstable-v1-protocol.c"
        DEPENDS "${kde_screencast_protocol}" VERBATIM)
      # KWin's fake input protocol (vendored XML, plasma-wayland-protocols): the scroll capture's wheel on Plasma.
      set(kde_fake_input_protocol "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/kde/protocols/fake-input.xml")
      add_custom_command(OUTPUT "${protocol_dir}/fake-input-client-protocol.h"
        COMMAND "${PUTGD_WAYLAND_SCANNER}" client-header "${kde_fake_input_protocol}" "${protocol_dir}/fake-input-client-protocol.h"
        DEPENDS "${kde_fake_input_protocol}" VERBATIM)
      add_custom_command(OUTPUT "${protocol_dir}/fake-input-protocol.c"
        COMMAND "${PUTGD_WAYLAND_SCANNER}" private-code "${kde_fake_input_protocol}" "${protocol_dir}/fake-input-protocol.c"
        DEPENDS "${kde_fake_input_protocol}" VERBATIM)
      target_sources(putgd-record PRIVATE src/record/KdeFakeInput.cpp "${protocol_dir}/fake-input-client-protocol.h" "${protocol_dir}/fake-input-protocol.c")
      target_sources(putgd-record PRIVATE src/record/KdeScreencast.cpp
        "${protocol_dir}/zkde-screencast-unstable-v1-client-protocol.h" "${protocol_dir}/zkde-screencast-unstable-v1-protocol.c"
        "${protocol_dir}/xdg-output-unstable-v1-client-protocol.h" "${protocol_dir}/xdg-output-unstable-v1-protocol.c")
      target_include_directories(putgd-record PRIVATE "${protocol_dir}")
      target_compile_definitions(putgd-record PRIVATE PUTGD_RECORD_KDE_SCREENCAST=1)
      target_link_libraries(putgd-record PRIVATE PkgConfig::PUTGD_WAYLAND)
      if(PUTGD_LAYER_SHELL_REGION)
        # The screenshot region selector and overlay layer surfaces for the
        # ready ring, dimming and control bar on Plasma Wayland.
        target_sources(putgd-record PRIVATE src/platform/wayland/LayerShellRegionOverlay.cpp)
        target_compile_definitions(putgd-record PRIVATE PUTGD_HAVE_LAYER_SHELL=1 PUTGD_RECORD_LAYER_SHELL=1)
        target_link_libraries(putgd-record PRIVATE LayerShellQt::Interface)
      endif()
    endif()
    install(TARGETS putgd-record RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(BUILD_TESTING)
      # Exercise the production helper session, timer, media drain and review with
      # controlled storage samples on a private Xvfb display (not desktop proof).
      get_target_property(record_test_sources putgd-record SOURCES)
      list(REMOVE_ITEM record_test_sources src/record/main.cpp)
      add_executable(test_recording_storage tests/integration/test_recording_storage.cpp ${record_test_sources})
      target_include_directories(test_recording_storage PRIVATE src tests $<TARGET_PROPERTY:putgd-record,INCLUDE_DIRECTORIES>)
      target_compile_definitions(test_recording_storage PRIVATE $<TARGET_PROPERTY:putgd-record,COMPILE_DEFINITIONS>)
      target_link_libraries(test_recording_storage PRIVATE $<TARGET_PROPERTY:putgd-record,LINK_LIBRARIES> Qt6::Test)
      add_test(NAME recording_storage_integration COMMAND test_recording_storage)
      set_tests_properties(recording_storage_integration PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)
      # #6: the live pipeline keeps 60 fps from a moving Xvfb root on the grid.
      add_executable(test_recording_pipeline tests/integration/test_recording_pipeline.cpp src/record/AdaptiveRate.cpp src/record/RecordingPipeline.cpp src/record/Microphones.cpp src/record/ClickOverlay.cpp src/core/RecordingModel.cpp src/record/PauseTimeline.cpp src/record/DebugLog.cpp src/record/WaylandAcquisition.cpp src/record/AvSync.cpp src/record/ImageCopySource.cpp ${putgd_capture_protocol_sources})
      target_include_directories(test_recording_pipeline PRIVATE src tests $<TARGET_PROPERTY:putgd-record,INCLUDE_DIRECTORIES>)
      target_compile_definitions(test_recording_pipeline PRIVATE $<TARGET_PROPERTY:putgd-record,COMPILE_DEFINITIONS>)
      target_link_libraries(test_recording_pipeline PRIVATE $<TARGET_PROPERTY:putgd-record,LINK_LIBRARIES> Qt6::Test)
      add_test(NAME recording_pipeline_throughput COMMAND test_recording_pipeline)
      set_tests_properties(recording_pipeline_throughput PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
      add_executable(test_portal_streams tests/unit/test_portal_streams.cpp src/record/PortalScreenCast.cpp src/record/DebugLog.cpp)
      target_include_directories(test_portal_streams PRIVATE src tests)
      target_link_libraries(test_portal_streams PRIVATE putgd_core putgd_build_options Qt6::Core Qt6::DBus Qt6::Test)
      add_test(NAME portal_streams_contract COMMAND test_portal_streams)
      # #984: when the adaptive frame rate lowers a recording's rate, and the /proc CPU shares it reads.
      add_executable(test_adaptive_rate tests/unit/test_adaptive_rate.cpp src/record/AdaptiveRate.cpp)
      target_include_directories(test_adaptive_rate PRIVATE src tests)
      target_link_libraries(test_adaptive_rate PRIVATE putgd_build_options Qt6::Core Qt6::Test)
      add_test(NAME adaptive_rate_contract COMMAND test_adaptive_rate)
      add_executable(test_wayland_acquisition tests/unit/test_wayland_acquisition.cpp src/record/WaylandAcquisition.cpp)
      target_include_directories(test_wayland_acquisition PRIVATE src tests)
      target_link_libraries(test_wayland_acquisition PRIVATE putgd_build_options Qt6::Core Qt6::Test)
      add_test(NAME wayland_acquisition_contract COMMAND test_wayland_acquisition)
      # #86: export keeps the working clip's audio start relative to its video.
      add_executable(test_export_av_sync tests/unit/test_export_av_sync.cpp src/record/ClipExporter.cpp src/record/AvSync.cpp src/record/DebugLog.cpp src/core/RecordingModel.cpp)
      target_include_directories(test_export_av_sync PRIVATE src tests)
      target_link_libraries(test_export_av_sync PRIVATE putgd_build_options Qt6::Core Qt6::Gui Qt6::Test PkgConfig::PUTGD_AV)
      add_test(NAME export_av_sync_contract COMMAND test_export_av_sync)
      # #112: History's poster frame skips a black lead-in and keeps an export's masks.
      add_executable(test_poster tests/unit/test_poster.cpp src/record/Poster.cpp src/record/ClipExporter.cpp src/record/AvSync.cpp src/record/DebugLog.cpp src/core/RecordingModel.cpp)
      target_include_directories(test_poster PRIVATE src tests)
      target_link_libraries(test_poster PRIVATE putgd_build_options Qt6::Core Qt6::Gui Qt6::Test PkgConfig::PUTGD_AV)
      add_test(NAME poster_contract COMMAND test_poster)
      # #130: pause/resume drops and shifts video and audio by the same paused time.
      add_executable(test_pause_timeline tests/unit/test_pause_timeline.cpp src/record/PauseTimeline.cpp)
      target_include_directories(test_pause_timeline PRIVATE src tests)
      target_link_libraries(test_pause_timeline PRIVATE putgd_build_options Qt6::Core Qt6::Test)
      add_test(NAME pause_timeline_contract COMMAND test_pause_timeline)
      add_executable(test_record_runtime tests/unit/test_record_runtime.cpp src/record/BundledRuntime.cpp)
      target_include_directories(test_record_runtime PRIVATE src tests)
      target_link_libraries(test_record_runtime PRIVATE putgd_build_options Qt6::Core Qt6::Test)
      add_test(NAME record_runtime_contract COMMAND test_record_runtime)
      # The recording control bar: a discard asks first, and no microphone is
      # recorded unasked (#1364).
      add_executable(test_bar_placement tests/unit/test_bar_placement.cpp src/record/BarPlacement.cpp src/record/Overlays.cpp src/record/record.qrc)
      target_include_directories(test_bar_placement PRIVATE src tests)
      target_link_libraries(test_bar_placement PRIVATE putgd_core putgd_build_options putgd_visual putgd_fonts Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
      add_test(NAME bar_placement_contract COMMAND test_bar_placement)
      set_tests_properties(bar_placement_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
      # #283: discarding a recording only after its confirmation.
      add_library(test_review_window_support STATIC src/ui/RegionOverlay.cpp src/ui/RegionSelector.h src/record/ReviewWindow.cpp src/record/Overlays.cpp src/record/BarPlacement.cpp src/record/ClipExporter.cpp src/record/AvSync.cpp src/record/DebugLog.cpp src/record/record.qrc)
      target_include_directories(test_review_window_support PRIVATE src tests)
      target_link_libraries(test_review_window_support PUBLIC putgd_core putgd_build_options putgd_host_open putgd_visual putgd_fonts putgd_file_dialog Qt6::Widgets Qt6::Test PkgConfig::PUTGD_AV PkgConfig::PUTGD_XCB)
      foreach(area actions)
        add_executable(test_review_window_${area} tests/gui/test_review_window_${area}.cpp tests/gui/ReviewWindowFixture.h)
        target_include_directories(test_review_window_${area} PRIVATE src tests)
        target_link_libraries(test_review_window_${area} PRIVATE test_review_window_support)
        add_test(NAME gui_review_window_${area} COMMAND test_review_window_${area})
        set_tests_properties(gui_review_window_${area} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen:configfile=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/offscreen-one-monitor.json" TIMEOUT 60)
      endforeach()
      # #264: the helper's window icon and desktop file name, and the entry it names.
      add_executable(test_record_identity tests/unit/test_record_identity.cpp)
      target_include_directories(test_record_identity PRIVATE src)
      target_compile_definitions(test_record_identity PRIVATE PUTGD_RECORD_DESKTOP_ENTRY="${CMAKE_BINARY_DIR}/packaging/linux/putgd-record.desktop")
      target_link_libraries(test_record_identity PRIVATE putgd_visual Qt6::Test)
      add_test(NAME record_identity_contract COMMAND test_record_identity)
      set_tests_properties(record_identity_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
  else()
    message(STATUS "putgd-record not built: GStreamer >= 1.24 and FFmpeg >= 6.1 development libraries are required; screen recording reports recording_helper_missing")
  endif()
endif()

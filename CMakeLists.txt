# ns-3.48 contrib module. The recorder has no Qt/Boost/JSON dependency.
build_lib(
  LIBNAME meshviz
  SOURCE_FILES helper/meshviz-helper.cc
  HEADER_FILES helper/meshviz-helper.h
  TEST_SOURCES test/meshviz-test-suite.cc
  LIBRARIES_TO_LINK ${libwifi} ${libinternet} ${libmobility} ${libapplications}
)
option(MESHVIZ_BUILD_UI "Build the offline MeshViz Qt viewer" ON)
if(MESHVIZ_BUILD_UI)
  find_package(Qt6 QUIET COMPONENTS Widgets)
  if(Qt6_FOUND)
    set(MESHVIZ_QT Qt6)
  else()
    find_package(Qt5 5.15 QUIET COMPONENTS Widgets)
    set(MESHVIZ_QT Qt5)
  endif()
  if(Qt6_FOUND OR Qt5_FOUND)
    add_subdirectory(ui)
    # Building an instrumented simulation also makes its optional viewer available.
    add_dependencies(${libmeshviz} meshviz-viewer)
  else()
    message(STATUS "meshviz: Qt Widgets unavailable; recorder only")
  endif()
endif()

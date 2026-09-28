# oiio_topos 插件构建（可选；未安装 OIIO SDK 时整体跳过，核心零依赖不受影响）。
# 本文件须在 topos_codec 目标定义之后 include（CMakeLists.txt 主文件中部）。
if(TOPOS_BUILD_OIIO_PLUGIN)
  find_package(OpenImageIO QUIET)
  if(NOT OpenImageIO_FOUND)
    message(FATAL_ERROR
      "TOPOS_BUILD_OIIO_PLUGIN=ON 但未找到 OpenImageIO；"
      "请设置 CMAKE_PREFIX_PATH 指向 OIIO 安装前缀，或关闭该选项")
  endif()

  add_library(topos.imageio MODULE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/oiio_topos/topos_imageio.cpp)
  target_include_directories(topos.imageio PRIVATE
    ${OpenImageIO_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(topos.imageio PRIVATE
    topos_codec
    OpenImageIO::OpenImageIO)
  set_target_properties(topos.imageio PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:topos_codec>")
  if(NOT MSVC)
    target_compile_options(topos.imageio PRIVATE -Wall -Wextra)
  endif()
endif()

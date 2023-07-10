cmake_minimum_required(VERSION 3.24)
project(ffmpeg_tutorial_1)

set(CMAKE_CXX_STANDARD 23)

set(FFMPEG_DIR C:/develop/msys2/home/admin/build/ffmpeg-shared-debug)

include_directories(${FFMPEG_DIR}/include)
link_directories(${FFMPEG_DIR}/lib)

find_package(SDL2 REQUIRED)
include_directories(${SDL2_INCLUDE_DIRS})

#[[add_library(avutil STATIC IMPORTED)
set_target_properties(avutil PROPERTIES IMPORTED_LOCATION ${FFMPEG_DIR}/lib/libavutil.dll.a)]]



#[[add_executable(proj1 main.cpp)]]

# add project/executables
file(GLOB srcFiles main.cpp
        demo/*.cpp
        test/*.cpp
        process/*.cpp)
foreach(src  ${srcFiles})
    get_filename_component(TARGET ${src} NAME)
    add_executable(${TARGET} ${src})
    target_link_libraries(${TARGET}
            avformat avdevice avcodec avfilter avutil swresample swscale
            x264 x265 fdk-aac opus vpx ${SDL2_LIBRARIES}
            )
    message(STATUS "${TARGET} added")
endforeach()




#[[add_custom_target(ffmpeg-dll
        COMMAND ${CMAKE_COMMAND} -E copy ${FFMPEG_DIR}/bin/avutil-58.dll          ${CMAKE_BINARY_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy ${FFMPEG_DIR}/bin/swscale-7.dll          ${CMAKE_BINARY_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy ${FFMPEG_DIR}/bin/swresample-4.dll          ${CMAKE_BINARY_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy ${FFMPEG_DIR}/bin/postproc-57.dll          ${CMAKE_BINARY_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy ${FFMPEG_DIR}/bin/avcodec-60.dll          ${CMAKE_BINARY_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy ${FFMPEG_DIR}/bin/avformat-60.dll          ${CMAKE_BINARY_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy ${FFMPEG_DIR}/bin/avfilter-9.dll          ${CMAKE_BINARY_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy ${FFMPEG_DIR}/bin/avdevice-60.dll          ${CMAKE_BINARY_DIR}

        # Output Message
        COMMENT "Copying ffmpeg-dll from '${FFMPEG_DIR}/bin/*.dll' to '${CMAKE_BINARY_DIR}'" VERBATIM
        )

add_dependencies(proj1 ffmpeg-dll)]]

# copy dlls
file(GLOB ffmpeg_shared_libries ${FFMPEG_DIR}/bin/*dll)
file(COPY ${ffmpeg_shared_libries} DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

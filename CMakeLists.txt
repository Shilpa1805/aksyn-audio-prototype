cmake_minimum_required(VERSION 3.21)

project(aksyn_audio_prototype LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(audio_sender
  cpp/audio_sender.cpp
  cpp/aksyn_audio_common.h
  cpp/aksyn_audio_common.cpp
)

add_executable(audio_receiver
  cpp/audio_receiver.cpp
  cpp/aksyn_audio_common.h
  cpp/aksyn_audio_common.cpp
)

if (WIN32)
  target_link_libraries(audio_sender PRIVATE ws2_32)
  target_link_libraries(audio_receiver PRIVATE ws2_32)
endif()

find_package(portaudio CONFIG REQUIRED)
target_link_libraries(audio_sender PRIVATE portaudio)
target_link_libraries(audio_receiver PRIVATE portaudio)

target_compile_definitions(audio_sender PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_definitions(audio_receiver PRIVATE _CRT_SECURE_NO_WARNINGS)

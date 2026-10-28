cmake_minimum_required(VERSION 3.22.1)
project("gesture_conductor")

add_library(gesture_conductor SHARED
    conductor_jni.c
    conductor_tracker.c
    conductor_audio.c
)

find_library(log-lib log)
find_library(android-lib android)
find_library(aaudio-lib aaudio)

target_link_libraries(gesture_conductor
    ${log-lib}
    ${android-lib}
    ${aaudio-lib}
    m
)

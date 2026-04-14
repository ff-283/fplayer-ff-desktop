project(FPlayer_Desketop)

option(FPLAYER_BUILD_MEDIA_QT6    "Build Qt6 media backend" ON)
option(FPLAYER_BUILD_MEDIA_FFMPEG "Build FFmpeg media backend" ON)
option(FPLAYER_BUILD_NET_QT6      "Build Qt6 network backend" ON)
option(FPLAYER_BUILD_NET_FFMPEG   "Build FFmpeg network backend" ON)
option(FPLAYER_BUILD_RTC_WEBRTC   "Build WebRTC backend" OFF)

if(FPLAYER_BUILD_MEDIA_QT6)
    add_subdirectory(media_qt6)
endif()

if(FPLAYER_BUILD_MEDIA_FFMPEG)
    add_subdirectory(media_ffmpeg)
endif()

if(FPLAYER_BUILD_NET_QT6)
    add_subdirectory(net_qt6)
endif()

if(FPLAYER_BUILD_NET_FFMPEG)
    add_subdirectory(net_ffmpeg)
endif()

if(FPLAYER_BUILD_RTC_WEBRTC)
    add_subdirectory(rtc_webrtc)
endif()
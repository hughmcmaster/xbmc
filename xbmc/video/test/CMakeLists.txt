set(SOURCES TestBookmark.cpp
            TestFilenameAttributes.cpp
            TestPlayerControllerActions.cpp
            TestStacks.cpp
            TestTeletext.cpp
            TestVideoDatabase.cpp
            TestVideoDbUrl.cpp
            TestVideoFileItemClassify.cpp
            TestVideoInfoTag.cpp
            TestVideoStreamSelect.cpp
            TestVideoUtils.cpp)

core_add_test_library(video_test)
if(ENABLE_STATIC_LIBS)
  target_link_libraries(video_test PRIVATE video)
endif()

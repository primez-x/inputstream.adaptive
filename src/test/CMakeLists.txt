set(BINARY ${CMAKE_PROJECT_NAME}_test)

find_package( Threads )

add_definitions(-DINPUTSTREAM_TEST_BUILD)

add_executable(${BINARY}
    TestMain.cpp
    TestADTSReader.cpp
    TestDASHTree.cpp
    TestHLSTree.cpp
    TestSmoothTree.cpp
    TestHelper.cpp
    TestUtils.cpp
    ../decrypters/Helpers.cpp
    ../demuxers/ADTSReader.cpp
    ../samplereader/ADTSSampleReader.cpp
    ../parser/CodecParser.cpp
    ../parser/DASHTree.cpp
    ../parser/HLSTree.cpp
    ../parser/SmoothTree.cpp
    ../parser/PRProtectionParser.cpp
    ../common/AdaptationSet.cpp
    ../common/AdaptiveStream.cpp
    ../common/AdaptiveTree.cpp
    ../common/AdaptiveTreeFactory.cpp
    ../common/AdaptiveUtils.cpp
    ../common/Chooser.cpp
    ../common/ChooserAskQuality.cpp
    ../common/ChooserDefault.cpp
    ../common/ChooserFixedRes.cpp
    ../common/ChooserManualOSD.cpp
    ../common/ChooserTest.cpp
    ../common/CommonAttribs.cpp
    ../common/CommonSegAttribs.cpp
    ../common/Period.cpp
    ../common/Representation.cpp
    ../common/ReprSelector.cpp
    ../common/Segment.cpp
    ../common/SegmentBase.cpp
    ../common/SegmentList.cpp
    ../common/SegTemplate.cpp
    ../AdaptiveByteStream.cpp
    ../oscompat.cpp
    ../SrvBroker.cpp
    ../CompSettings.cpp
    ../CompKodiProps.cpp
    ../utils/Base64Utils.cpp
    ../utils/CharArrayParser.cpp
    ../utils/CurlUtils.cpp
    ../utils/DigestMD5Utils.cpp
    ../utils/FileUtils.cpp
    ../utils/StringUtils.cpp
    ../utils/UrlUtils.cpp
    ../utils/Utils.cpp
    ../utils/XMLUtils.cpp
    )

target_link_libraries(${BINARY} PRIVATE ${BENTO4_LIBRARIES} ${PUGIXML_LIBRARIES} ${GTEST_LIBRARIES} Threads::Threads ${CMAKE_DL_LIBS})

set(TEST_DATA_DIR "${CMAKE_SOURCE_DIR}/src/test/manifests")
add_test(NAME manifest_tests COMMAND ${BINARY} "${TEST_DATA_DIR}")

# ADTSReader.h includes the Kodi headers, which conflict with the test stubs
set_source_files_properties(TestADTSReader.cpp ../samplereader/ADTSSampleReader.cpp
                            PROPERTIES COMPILE_OPTIONS "-UINPUTSTREAM_TEST_BUILD")

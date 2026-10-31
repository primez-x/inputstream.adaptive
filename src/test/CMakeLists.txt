set(BINARY ${CMAKE_PROJECT_NAME}_test)

find_package( Threads )

add_executable(${BINARY}
    TestMain.cpp
    TestDASHTree.cpp
    TestHLSTree.cpp
    TestSmoothTree.cpp
    TestHelper.cpp
    TestUtils.cpp
    ../decrypters/Helpers.cpp
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

target_compile_definitions(${BINARY} PRIVATE INPUTSTREAM_TEST_BUILD)
target_link_libraries(${BINARY} PRIVATE ${BENTO4_LIBRARIES} ${PUGIXML_LIBRARIES} ${GTEST_LIBRARIES} Threads::Threads ${CMAKE_DL_LIBS})

set(TEST_DATA_DIR "${CMAKE_SOURCE_DIR}/src/test/manifests")
add_test(NAME manifest_tests COMMAND ${BINARY} "${TEST_DATA_DIR}")

# The ADTS reader uses the Kodi headers instead of the test stubs, so the ADTS
# tests build without INPUTSTREAM_TEST_BUILD in a separate executable
set(ADTS_BINARY ${CMAKE_PROJECT_NAME}_adts_test)

add_executable(${ADTS_BINARY}
    TestMain.cpp
    TestADTSReader.cpp
    ../demuxers/ADTSReader.cpp
    ../parser/CodecParser.cpp
    ../samplereader/ADTSSampleReader.cpp
    )

target_link_libraries(${ADTS_BINARY} PRIVATE ${BENTO4_LIBRARIES} ${GTEST_LIBRARIES} Threads::Threads)

add_test(NAME adts_tests COMMAND ${ADTS_BINARY} "${TEST_DATA_DIR}")

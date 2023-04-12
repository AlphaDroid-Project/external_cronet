// © 2021 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html

#include "unicode/utypes.h"

#if !UCONFIG_NO_BREAK_ITERATION

#include "lstmbetst.h"
#include "lstmbe.h"

#include <algorithm>
#include <sstream>
#include <vector>

#include "charstr.h"

//---------------------------------------------
// runIndexedTest
//---------------------------------------------


void LSTMBETest::runIndexedTest( int32_t index, UBool exec, const char* &name, char* params )
{
    fTestParams = params;

    TESTCASE_AUTO_BEGIN;

    TESTCASE_AUTO(TestThaiGraphclust);
    TESTCASE_AUTO(TestThaiCodepoints);
    TESTCASE_AUTO(TestBurmeseGraphclust);
    TESTCASE_AUTO(TestThaiGraphclustWithLargeMemory);
    TESTCASE_AUTO(TestThaiCodepointsWithLargeMemory);

    TESTCASE_AUTO_END;
}


//--------------------------------------------------------------------------------------
//
//    LSTMBETest    constructor and destructor
//
//--------------------------------------------------------------------------------------

LSTMBETest::LSTMBETest() {
    fTestParams = NULL;
}


LSTMBETest::~LSTMBETest() {
}

UScriptCode getScriptFromModelName(const std::string& modelName) {
    if (modelName.find("Thai") == 0) {
        return USCRIPT_THAI;
    } else if (modelName.find("Burmese") == 0) {
        return USCRIPT_MYANMAR;
    }
    // Add for other script codes.
    UPRV_UNREACHABLE_EXIT;
}

// Read file generated by
// https://github.com/unicode-org/lstm_word_segmentation/blob/master/segment_text.py
// as test cases and compare the Output.
// Format of the file
//   Model:\t[Model Name (such as 'Thai_graphclust_model4_heavy')]
//   Embedding:\t[Embedding type (such as 'grapheme_clusters_tf')]
//   Input:\t[source text]
//   Output:\t[expected output separated by | ]
//   Input: ...
//   Output: ...
// The test will ensure the Input contains only the characters can be handled by
// the model. Since by default the LSTM models are not included, all the tested
// models need to be included under source/test/testdata.

void LSTMBETest::runTestFromFile(const char* filename) {
    UErrorCode   status = U_ZERO_ERROR;
    LocalPointer<const LanguageBreakEngine> engine;
    //  Open and read the test data file.
    const char *testDataDirectory = IntlTest::getSourceTestData(status);
    CharString testFileName(testDataDirectory, -1, status);
    testFileName.append(filename, -1, status);

    int len;
    UChar *testFile = ReadAndConvertFile(testFileName.data(), len, "UTF-8", status);
    if (U_FAILURE(status)) {
        errln("%s:%d Error %s opening test file %s", __FILE__, __LINE__, u_errorName(status), filename);
        return;
    }

    //  Put the test data into a UnicodeString
    UnicodeString testString(false, testFile, len);

    int32_t start = 0;

    UnicodeString line;
    int32_t end;
    std::string actual_sep_str;
    int32_t caseNum = 0;
    // Iterate through all the lines in the test file.
    do {
        int32_t cr = testString.indexOf(u'\r', start);
        int32_t lf = testString.indexOf(u'\n', start);
        end = cr >= 0 ? (lf >= 0 ? std::min(cr, lf) : cr) : lf;
        line = testString.tempSubString(start, end < 0 ? INT32_MAX : end - start);
        if (line.length() > 0) {
            // Separate each line to key and value by TAB.
            int32_t tab = line.indexOf(u'\t');
            UnicodeString key = line.tempSubString(0, tab);
            const UnicodeString value = line.tempSubString(tab+1);

            if (key == "Model:") {
                std::string modelName;
                value.toUTF8String<std::string>(modelName);
                engine.adoptInstead(createEngineFromTestData(modelName.c_str(), getScriptFromModelName(modelName), status));
                if (U_FAILURE(status)) {
                    dataerrln("Could not CreateLSTMBreakEngine for " + line + UnicodeString(u_errorName(status)));
                    return;
                }
            } else if (key == "Input:") {
                // First, we ensure all the char in the Input lines are accepted
                // by the engine before we test them.
                caseNum++;
                bool canHandleAllChars = true;
                for (int32_t i = 0; i < value.length(); i++) {
                    if (!engine->handles(value.charAt(i))) {
                        errln(UnicodeString("Test Case#") + caseNum + " contains char '" +
                                  UnicodeString(value.charAt(i)) +
                                  "' cannot be handled by the engine in offset " + i + "\n" + line);
                        canHandleAllChars = false;
                        break;
                    }
                }
                if (! canHandleAllChars) {
                    return;
                }

                // If the engine can handle all the chars in the Input line, we
                // then find the break points by calling the engine.
                std::stringstream ss;

                // Construct the UText which is expected by the the engine as
                // input from the UnicodeString.
                UText ut = UTEXT_INITIALIZER;
                utext_openConstUnicodeString(&ut, &value, &status);
                if (U_FAILURE(status)) {
                    dataerrln("Could not utext_openConstUnicodeString for " + value + UnicodeString(u_errorName(status)));
                    return;
                }

                UVector32 actual(status);
                if (U_FAILURE(status)) {
                    dataerrln("%s:%d Error %s Could not allocate UVextor32", __FILE__, __LINE__, u_errorName(status));
                    return;
                }
                engine->findBreaks(&ut, 0, value.length(), actual, false, status);
                if (U_FAILURE(status)) {
                    dataerrln("%s:%d Error %s findBreaks failed", __FILE__, __LINE__, u_errorName(status));
                    return;
                }
                utext_close(&ut);
                for (int32_t i = 0; i < actual.size(); i++) {
                    ss << actual.elementAti(i) << ", ";
                }
                ss << value.length();
                // Turn the break points into a string for easy comparison
                // output.
                actual_sep_str = "{" + ss.str() + "}";
            } else if (key == "Output:" && !actual_sep_str.empty()) {
                std::string d;
                int32_t sep;
                int32_t start = 0;
                int32_t curr = 0;
                std::stringstream ss;
                while ((sep = value.indexOf(u'|', start)) >= 0) {
                    int32_t len = sep - start;
                    if (len > 0) {
                        if (curr > 0) {
                            ss << ", ";
                        }
                        curr += len;
                        ss << curr;
                    }
                    start = sep + 1;
                }
                // Turn the break points into a string for easy comparison
                // output.
                std::string expected = "{" + ss.str() + "}";
                std::string utf8;

                assertEquals((value + " Test Case#" + caseNum).toUTF8String<std::string>(utf8).c_str(),
                             expected.c_str(), actual_sep_str.c_str());
                actual_sep_str.clear();
            }
        }
        start = std::max(cr, lf) + 1;
    } while (end >= 0);

    delete [] testFile;
}

void LSTMBETest::TestThaiGraphclust() {
    runTestFromFile("Thai_graphclust_model4_heavy_Test.txt");
}

void LSTMBETest::TestThaiCodepoints() {
    runTestFromFile("Thai_codepoints_exclusive_model5_heavy_Test.txt");
}

void LSTMBETest::TestBurmeseGraphclust() {
    runTestFromFile("Burmese_graphclust_model5_heavy_Test.txt");
}

const LanguageBreakEngine* LSTMBETest::createEngineFromTestData(
        const char* model, UScriptCode script, UErrorCode& status) {
    const char* testdatapath=loadTestData(status);
    if(U_FAILURE(status))
    {
        dataerrln("Could not load testdata.dat " + UnicodeString(testdatapath) +  ", " +
                  UnicodeString(u_errorName(status)));
        return nullptr;
    }

    LocalUResourceBundlePointer rb(
        ures_openDirect(testdatapath, model, &status));
    if (U_FAILURE(status)) {
        dataerrln("Could not open " + UnicodeString(model) + " under " +  UnicodeString(testdatapath) +  ", " +
                  UnicodeString(u_errorName(status)));
        return nullptr;
    }

    const LSTMData* data = CreateLSTMData(rb.orphan(), status);
    if (U_FAILURE(status)) {
        dataerrln("Could not CreateLSTMData " + UnicodeString(model) + " under " +  UnicodeString(testdatapath) +  ", " +
                  UnicodeString(u_errorName(status)));
        return nullptr;
    }
    if (data == nullptr) {
        return nullptr;
    }

    LocalPointer<const LanguageBreakEngine> engine(CreateLSTMBreakEngine(script, data, status));
    if (U_FAILURE(status) || engine.getAlias() == nullptr) {
        dataerrln("Could not CreateLSTMBreakEngine " + UnicodeString(testdatapath) +  ", " +
                  UnicodeString(u_errorName(status)));
        DeleteLSTMData(data);
        return nullptr;
    }
    return engine.orphan();
}


void LSTMBETest::TestThaiGraphclustWithLargeMemory() {
    runTestWithLargeMemory("Thai_graphclust_model4_heavy", USCRIPT_THAI);

}

void LSTMBETest::TestThaiCodepointsWithLargeMemory() {
    runTestWithLargeMemory("Thai_codepoints_exclusive_model5_heavy", USCRIPT_THAI);
}

constexpr int32_t MEMORY_TEST_THESHOLD_SHORT = 2 * 1024; // 2 K Unicode Chars.
constexpr int32_t MEMORY_TEST_THESHOLD = 32 * 1024; // 32 K Unicode Chars.

// Test with very long unicode string.
void LSTMBETest::runTestWithLargeMemory( const char* model, UScriptCode script) {
    UErrorCode   status = U_ZERO_ERROR;
    int32_t test_threshold = quick ? MEMORY_TEST_THESHOLD_SHORT : MEMORY_TEST_THESHOLD;
    LocalPointer<const LanguageBreakEngine> engine(
        createEngineFromTestData(model, script, status));
    if (U_FAILURE(status)) {
        dataerrln("Could not CreateLSTMBreakEngine for " + UnicodeString(model) + UnicodeString(u_errorName(status)));
        return;
    }
    UnicodeString text(u"อ");  // start with a single Thai char.
    UVector32 actual(status);
    if (U_FAILURE(status)) {
        dataerrln("%s:%d Error %s Could not allocate UVextor32", __FILE__, __LINE__, u_errorName(status));
        return;
    }
    while (U_SUCCESS(status) && text.length() <= test_threshold) {
        // Construct the UText which is expected by the the engine as
        // input from the UnicodeString.
        UText ut = UTEXT_INITIALIZER;
        utext_openConstUnicodeString(&ut, &text, &status);
        if (U_FAILURE(status)) {
            dataerrln("Could not utext_openConstUnicodeString for " + text + UnicodeString(u_errorName(status)));
            return;
        }

        engine->findBreaks(&ut, 0, text.length(), actual, false, status);
        utext_close(&ut);
        text += text;
    }
}
#endif // #if !UCONFIG_NO_BREAK_ITERATION
